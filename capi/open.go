// capi/open.go
// omegassh_open and the JSON it takes.
//
// One entry point for every transport. The alternative -- omegassh_open_ssh,
// omegassh_open_telnet, omegassh_open_serial -- would have put the branch in
// the caller, where it would then also appear in the session tree, the connect
// dialog and the tab. It belongs here, once.
//
// The config crosses as JSON so the surface can grow without breaking the ABI,
// and every telnet and serial field is string or int for the same reason
// telnetx.Config and serialx.Config are: a caller naming a baud rate or a
// parity should not have to import the package that implements it.
package main

/*
#include <stdlib.h>
*/
import "C"

import (
	"encoding/json"
	"fmt"
	"net"
	"time"

	"github.com/scottpeterman/omegassh/serialx"
	"github.com/scottpeterman/omegassh/sshcore"
	"github.com/scottpeterman/omegassh/telnetx"
	"github.com/scottpeterman/omegassh/transport"
	"github.com/scottpeterman/omegassh/vault/sshsecrets"

	"golang.org/x/crypto/ssh"
)

// openRequest is the JSON accepted by omegassh_open. JSON rather than a wide
// argument list so the surface can grow without breaking the ABI.
type openRequest struct {
	// Transport selects the transport: "ssh" (the default), "telnet" or
	// "serial". Empty means ssh, so a caller written against the pre-Phase-2
	// surface keeps working unchanged.
	Transport string `json:"transport"`

	// --- shared ---------------------------------------------------------

	// Host and Port are the target for ssh and telnet. Port 0 defaults per
	// transport: 22 for ssh, 23 for telnet. Ignored for serial, which names
	// its device in SerialPort.
	Host string `json:"host"`
	Port int    `json:"port"`

	// Term is the terminal type declared to the far end. On ssh it is the
	// pty request; on telnet it answers a TTYPE subnegotiation. Serial has
	// nowhere to declare it.
	Term string `json:"term"`
	Cols int    `json:"cols"`
	Rows int    `json:"rows"`

	Timeout int `json:"timeout_seconds"`

	// --- ssh ------------------------------------------------------------

	Username       string `json:"username"`
	Password       string `json:"password"`
	PrivateKeyPath string `json:"private_key_path"`
	KeyPassphrase  string `json:"key_passphrase"`
	UseAgent       bool   `json:"use_agent"`

	// Credential names an entry in the vault identified by Vault, whose
	// material fills whichever of the fields above were left blank. It is how
	// a caller dials a saved session without ever holding the secret: the name
	// comes down, the vault is read on the Go side during the dial, and
	// nothing goes back up.
	//
	// The plaintext fields above stay, and stay first-class: quick connect
	// with a typed-in password is real use, not a fallback. So the invariant
	// this buys is precisely "STORED secrets never cross the boundary" -- a
	// secret the operator just typed obviously does.
	// KeyboardAnswers answers keyboard-interactive questions, keyed by the
	// exact question text. A caller fills this on a SECOND dial, after a
	// first one failed with omegassh_keyboard_prompt_marker() in its error
	// and the question was put to a person. See sshcore/kbdprompt.go.
	KeyboardAnswers map[string]string `json:"keyboard_answers"`

	Credential string `json:"credential"`
	Vault      int64  `json:"vault"`

	JumpHost     string `json:"jump_host"`
	JumpPort     int    `json:"jump_port"`
	JumpUsername string `json:"jump_username"`
	JumpPassword string `json:"jump_password"`
	JumpKeyPath  string `json:"jump_key_path"`

	// JumpCredential resolves against the same vault. A bastion usually has
	// its own login rather than sharing the target's.
	JumpCredential string `json:"jump_credential"`

	// HostKeyPolicy is "strict", "tofu" or "insecure". Empty means strict.
	//
	// Insecure is a real option, not a trap door: disposable lab gear with a
	// regenerated key on every boot is ordinary, and forcing verification
	// there just teaches people to keep a wildcard known_hosts file.
	HostKeyPolicy  string `json:"host_key_policy"`
	KnownHostsPath string `json:"known_hosts_path"`

	// LegacyAlgorithms appends the old KEX/cipher/MAC/host-key tail that
	// aging network gear still requires.
	LegacyAlgorithms bool `json:"legacy_algorithms"`

	// --- telnet ---------------------------------------------------------

	// TelnetCRLF expands a lone CR on write to CR LF, which is what RFC 854
	// makes the telnet newline and what console servers expect. Null means
	// on. A pointer rather than a bool because "false on purpose" and
	// "absent" are different answers here.
	TelnetCRLF *bool `json:"telnet_crlf"`

	// --- serial ---------------------------------------------------------

	// SerialPort is the OS device name: COM3, /dev/ttyUSB0,
	// /dev/cu.usbserial-XXXX. Enumerate them with omegassh_serial_ports.
	SerialPort string `json:"serial_port"`
	Baud       int    `json:"baud"`      // 0 => 9600
	DataBits   int    `json:"data_bits"` // 5-8, 0 => 8
	Parity     string `json:"parity"`    // none|odd|even|mark|space, "" => none
	StopBits   string `json:"stop_bits"` // 1|1.5|2, "" => 1

	// --- logging --------------------------------------------------------

	// Log turns on session logging. LogPath being set implies it, so a
	// caller naming a file does not also have to remember the flag.
	Log bool `json:"log"`

	// LogPath is an exact file to write. Empty means a generated,
	// timestamped name under LogDir.
	LogPath string `json:"log_path"`

	// LogDir is where a generated name lands. Empty means ~/.omega/logs --
	// a user directory, never the launch directory.
	LogDir string `json:"log_dir"`

	// LogInput also records what the operator types. Off by default: a
	// device's own login prompt is ordinary session data on telnet and on a
	// serial console, so the password answering it is ordinary keystrokes.
	LogInput bool `json:"log_input"`

	// LogAppend opens an existing file for appending instead of truncating.
	LogAppend bool `json:"log_append"`
}

func (r openRequest) size() transport.Size {
	return transport.Size{Cols: r.Cols, Rows: r.Rows}
}

// wantsLog reports whether this request asked for a session log.
func (r openRequest) wantsLog() bool { return r.Log || r.LogPath != "" }

// openLog builds the session log, or returns nil if none was asked for.
//
// A log that cannot be opened fails the dial rather than being skipped. An
// operator who turned logging on for a change window needs to know before the
// change, not afterwards when the file is not there.
func (r openRequest) openLog(label string) (*transport.Logger, error) {
	if !r.wantsLog() {
		return nil, nil
	}
	return transport.OpenLog(transport.LogOptions{
		Path:         r.LogPath,
		Dir:          r.LogDir,
		Label:        label,
		IncludeInput: r.LogInput,
		Append:       r.LogAppend,
	})
}

//export omegassh_open
func omegassh_open(cfgJSON *C.char) C.longlong {
	clearErr()

	var req openRequest
	if err := json.Unmarshal([]byte(C.GoString(cfgJSON)), &req); err != nil {
		setErr("parse config: %v", err)
		return -1
	}

	plan, err := prepare(req)
	if err != nil {
		setErr("%v", err)
		return -1
	}

	h, st, err := newHandle(plan.kind, plan.summary, plan.initial)
	if err != nil {
		plan.discard()
		setErr("create notifier: %v", err)
		return -1
	}

	go st.runDial(plan)
	return h
}

// prepare decides everything that can be decided without touching the far
// end, and builds what the dial will need.
//
// The split it draws is the contract omegassh_open now makes. Above this line
// a failure is a configuration mistake, reported synchronously as -1 and a
// message on the calling thread, because the caller is still in a position to
// fix it -- an unknown transport, a credential that names nothing, a parity
// that does not exist, a log file that cannot be created. Below it a failure
// is the network's answer, and it arrives as a state change on a handle that
// already exists.
//
// Deciding a refusal here rather than in the dial is not only about which way
// it is reported. It is also what keeps the promise the vault surface was
// built on: nothing has been opened, nothing has been sent, and a misnamed
// credential has cost no packets.
func prepare(req openRequest) (*dialPlan, error) {
	kind, err := transport.ParseKind(req.Transport)
	if err != nil {
		return nil, err
	}

	switch kind {
	case transport.KindSSH:
		return prepareSSH(req)
	case transport.KindTelnet:
		return prepareTelnet(req)
	default:
		return prepareSerial(req)
	}
}

// ---------------------------------------------------------------------------
// SSH
// ---------------------------------------------------------------------------

func prepareSSH(req openRequest) (*dialPlan, error) {
	cfg, err := buildConfig(req)
	if err != nil {
		return nil, err
	}

	// The credential reference is resolved and thrown away here so that a
	// name nothing answers to -- renamed, deleted, taken out of service --
	// is refused in the same breath as a missing hostname. It is a
	// configuration mistake and an operator can act on it; making them wait
	// for a connect timeout to be told would be a worse answer for no reason.
	// sshcore.Dial resolves again for itself; see CheckCredentials.
	if err := sshcore.CheckCredentials(cfg); err != nil {
		return nil, err
	}

	// The log is opened before the dial so a bad path fails fast, and closed
	// again if the dial itself fails -- otherwise a failed connect leaves an
	// empty file behind on every attempt.
	summary := net.JoinHostPort(cfg.Host, fmt.Sprintf("%d", portOr(cfg.Port, 22)))
	log, err := req.openLog(summary)
	if err != nil {
		return nil, err
	}

	return &dialPlan{
		kind:    transport.KindSSH,
		summary: summary,
		initial: transport.StateConnecting,
		log:     log,
		ssh: &sshPlan{
			cfg: cfg,
			opts: sshcore.ShellOptions{
				Term: req.Term,
				Cols: req.Cols,
				Rows: req.Rows,
			},
		},
	}, nil
}

func portOr(p, def int) int {
	if p <= 0 {
		return def
	}
	return p
}

func firstNonEmpty(vals ...string) string {
	for _, v := range vals {
		if v != "" {
			return v
		}
	}
	return ""
}

func buildConfig(req openRequest) (sshcore.Config, error) {
	var secrets sshcore.SecretSource
	policy := sshcore.HostKeyStrict
	switch req.HostKeyPolicy {
	case "", "strict":
		policy = sshcore.HostKeyStrict
	case "tofu":
		policy = sshcore.HostKeyTOFU
	case "insecure":
		policy = sshcore.HostKeyInsecure
	default:
		return sshcore.Config{}, fmt.Errorf(
			"unknown host_key_policy %q (want strict, tofu or insecure)",
			req.HostKeyPolicy)
	}

	// The vault-backed path. A credential name with no vault handle is a
	// caller bug worth naming, rather than a silent dial with no credentials
	// that fails later as an authentication error.
	if req.Credential != "" || req.JumpCredential != "" {
		if req.Vault == 0 {
			return sshcore.Config{}, fmt.Errorf(
				"credential %q named but no vault handle given (set \"vault\")",
				firstNonEmpty(req.Credential, req.JumpCredential))
		}
		v := vlookup(C.longlong(req.Vault))
		if v == nil {
			return sshcore.Config{}, fmt.Errorf("no such vault handle: %d", req.Vault)
		}
		if v.IsLocked() {
			return sshcore.Config{}, fmt.Errorf(
				"vault %s is locked; unlock before dialing", v.Path())
		}
		secrets = sshsecrets.New(v)
	}

	cfg := sshcore.Config{
		Host:             req.Host,
		Port:             req.Port,
		Username:         req.Username,
		Password:         req.Password,
		PrivateKeyPath:   req.PrivateKeyPath,
		KeyPassphrase:    req.KeyPassphrase,
		UseAgent:         req.UseAgent,
		Credential:       req.Credential,
		Secrets:          secrets,
		HostKeys:         policy,
		KnownHostsPath:   req.KnownHostsPath,
		LegacyAlgorithms: req.LegacyAlgorithms,
		KeyboardAnswers:  req.KeyboardAnswers,
	}
	if req.Timeout > 0 {
		cfg.Timeout = time.Duration(req.Timeout) * time.Second
	}
	// TOFU auto-accepts first contact at this layer. A UI that wants a
	// fingerprint dialog should import sshcore directly and supply its own
	// HostKeyPrompt; a prompt cannot be marshalled across the C boundary
	// without the callback-on-a-foreign-thread problem this design avoids.
	// A MISMATCH still fails closed inside sshcore and never reaches here.
	if policy == sshcore.HostKeyTOFU {
		cfg.HostKeyPrompt = func(string, net.Addr, ssh.PublicKey) (bool, error) {
			return true, nil
		}
	}
	// A jump credential with no jump host is a config that silently does
	// nothing, which is worse than being told.
	if req.JumpCredential != "" && req.JumpHost == "" {
		return sshcore.Config{}, fmt.Errorf(
			"jump_credential %q given without jump_host", req.JumpCredential)
	}
	if req.JumpHost != "" {
		cfg.Jump = &sshcore.JumpConfig{
			Host:           req.JumpHost,
			Port:           req.JumpPort,
			Username:       req.JumpUsername,
			Password:       req.JumpPassword,
			PrivateKeyPath: req.JumpKeyPath,
			Credential:     req.JumpCredential,
		}
	}
	return cfg, nil
}

// ---------------------------------------------------------------------------
// Telnet
// ---------------------------------------------------------------------------

func prepareTelnet(req openRequest) (*dialPlan, error) {
	if req.Host == "" {
		return nil, fmt.Errorf("telnet: no host given")
	}
	// Fields the SSH path accepts and telnet cannot honour are refused
	// rather than ignored. Telnet has no authentication step and no bastion,
	// so a caller that set one has a session that is not what it thinks it
	// is -- and, in the jump case, one that would reach the target in
	// plaintext across a link it believed was tunneled.
	if req.Credential != "" || req.JumpCredential != "" || req.JumpHost != "" {
		return nil, fmt.Errorf(
			"telnet: credential and jump-host fields do not apply to telnet " +
				"(the protocol has no authentication step and no bastion)")
	}

	cfg := telnetx.Config{
		Host:     req.Host,
		Port:     portOr(req.Port, 23),
		TermType: req.Term,
	}
	if req.Timeout > 0 {
		cfg.ConnectTimeout = time.Duration(req.Timeout) * time.Second
	}
	if req.TelnetCRLF != nil {
		cfg = cfg.WithCRLF(*req.TelnetCRLF)
	}

	b := telnetx.New(cfg)
	summary := b.Config().Summary()

	log, err := req.openLog(summary)
	if err != nil {
		return nil, err
	}
	return &dialPlan{
		kind:    transport.KindTelnet,
		summary: summary,
		initial: transport.StateConnecting,
		log:     log,
		size:    req.size(),
		telnet:  b,
	}, nil
}

// ---------------------------------------------------------------------------
// Serial
// ---------------------------------------------------------------------------

func prepareSerial(req openRequest) (*dialPlan, error) {
	if req.SerialPort == "" {
		return nil, fmt.Errorf(
			"serial: no serial_port given (enumerate with omegassh_serial_ports)")
	}
	if req.Credential != "" || req.JumpCredential != "" || req.JumpHost != "" {
		return nil, fmt.Errorf(
			"serial: credential and jump-host fields do not apply to a serial console")
	}

	cfg := serialx.Config{
		Port:     req.SerialPort,
		Baud:     req.Baud,
		DataBits: req.DataBits,
		Parity:   req.Parity,
		StopBits: req.StopBits,
	}

	// An impossible mode is refused here rather than at the port: it is a
	// configuration mistake like the two above it, and Connect would
	// otherwise reach the same verdict several layers later and report it as
	// though the device had said something.
	if err := cfg.Validate(); err != nil {
		return nil, err
	}

	b := serialx.New(cfg)
	// "/dev/ttyUSB0 9600 8N1" rather than the mode alone: two consoles at
	// 9600 8N1 are otherwise indistinguishable in a tab title or a log name.
	summary := req.SerialPort + " " + b.Config().Summary()

	log, err := req.openLog(summary)
	if err != nil {
		return nil, err
	}
	// Serial starts disconnected, not connecting. Opening the port IS the
	// whole handshake -- there is nothing in between to be in -- and faking a
	// connecting state so the three transports look uniform would put a
	// spinner on screen for something that has no phase to spin through.
	return &dialPlan{
		kind:    transport.KindSerial,
		summary: summary,
		initial: transport.StateDisconnected,
		log:     log,
		serial:  b,
	}, nil
}
