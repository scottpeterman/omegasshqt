// sshcore/config.go
// Connection configuration for the headless SSH core.
//
// Extracted from the tetherssh SSH backend (cli/ssh_backend.go) with all
// GUI/terminal coupling removed. Differences from the baseline are
// deliberate:
//   - Host-key handling is a first-class policy enum (strict / TOFU /
//     insecure opt-in) instead of a pair of loosely related fields.
//   - Legacy KEX/cipher/MAC support is opt-in per connection, not the
//     global default.
package sshcore

import (
	"net"
	"time"

	"golang.org/x/crypto/ssh"
)

// HostKeyPolicy selects how server host keys are verified.
type HostKeyPolicy int

const (
	// HostKeyStrict verifies against known_hosts only. Unknown hosts fail.
	HostKeyStrict HostKeyPolicy = iota
	// HostKeyTOFU verifies against known_hosts; on first contact with an
	// unknown host the HostKeyPrompt callback decides, and an accepted key
	// is persisted. A key MISMATCH against a pinned host always fails
	// closed regardless of the callback (possible MITM).
	HostKeyTOFU
	// HostKeyInsecure skips verification entirely. Explicit opt-in for
	// disposable lab gear only.
	HostKeyInsecure
)

// HostKeyPromptFunc is consulted on first contact with an unknown host under
// HostKeyTOFU. Return true to accept-and-persist the key. It is never called
// for a mismatch against an already-pinned host.
type HostKeyPromptFunc func(hostname string, remote net.Addr, key ssh.PublicKey) (bool, error)

// AuthPromptFunc supplies answers for keyboard-interactive prompts and
// encrypted-key passphrases. echo=false means the input is a secret.
type AuthPromptFunc func(prompt string, echo bool) (string, error)

// DialStage names a point a dial passes through. It exists because a connect
// and an authentication fail for different reasons and take their time in
// different places: an operator watching a spinner against unreachable gear
// wants to know which of the two is hanging, and a caller cannot tell from
// outside a single blocking call.
//
// Nothing in this package acts on a stage. It is published and forgotten.
type DialStage int

const (
	// StageConnecting is reported once, immediately before the TCP
	// connection is opened -- to the bastion where there is one, since
	// until the bastion is up the target is not being reached at all.
	StageConnecting DialStage = iota

	// StageAuthenticating is reported once the connection to the target is
	// established and the SSH handshake and authentication are about to
	// run. Through a bastion that means the tunneled connection: the
	// bastion's own login happens inside the connect leg, because a
	// half-open path to the target is still a connect from where the
	// operator is standing.
	StageAuthenticating
)

// ProgressFunc is called as a dial passes each stage. It runs on the dialing
// goroutine, before the call it precedes, and must not block.
type ProgressFunc func(stage DialStage)

// JumpConfig describes an optional bastion the target is tunneled through.
type JumpConfig struct {
	Host           string
	Port           int // 0 => 22
	Username       string
	Password       string
	PrivateKeyPath string
	KeyPassphrase  string

	// Credential names an entry in Config.Secrets to fill the blank fields
	// above. A bastion usually has its own login, so it gets its own
	// reference rather than sharing the target's.
	Credential string
}

// Config holds everything needed to dial one target.
type Config struct {
	Host    string
	Port    int           // 0 => 22
	Timeout time.Duration // 0 => 30s

	Username       string
	Password       string
	PrivateKeyPath string // path to key file; "~/" expands
	PrivateKey     []byte // in-memory key; takes precedence over path
	KeyPassphrase  string
	UseAgent       bool // try SSH agent first (SSH_AUTH_SOCK)

	// Credential names an entry in Secrets whose material fills whichever of
	// the fields above were left blank. It is how a caller dials without ever
	// holding the secret: see secrets.go.
	Credential string
	Secrets    SecretSource

	Jump *JumpConfig // nil => direct connection

	HostKeys       HostKeyPolicy
	KnownHostsPath string // "" => ~/.ssh/known_hosts
	HostKeyPrompt  HostKeyPromptFunc
	AuthPrompt     AuthPromptFunc

	// KeyboardAnswers pre-supplies answers to keyboard-interactive questions,
	// keyed by the exact question text the server sent. Consulted BEFORE
	// AuthPrompt, and before the password auto-answer.
	//
	// It exists for callers that cannot supply a live AuthPrompt -- anything
	// reaching this package through the C boundary, where a prompt callback
	// would have to run on a foreign thread (see capi/open.go). Those callers
	// let the dial fail, read the question out of the error, ask their own
	// user, and dial again with the answer in here. Same fail-ask-redial shape
	// as the host key and credential flows.
	//
	// SINGLE USE IS THE CALLER'S PROBLEM. A one-time password is consumed by
	// the attempt that submits it, so a map entry that answers the same
	// question twice answers the second one with a dead OTP. The caller
	// supplies these per dial and does not reuse them.
	KeyboardAnswers map[string]string

	// Progress reports the dial's position to a caller that is rendering
	// one. Nil is the ordinary case and costs nothing.
	Progress ProgressFunc

	// KnownHostsWarn is called when known_hosts contains lines that could
	// not be parsed. Those entries are ignored rather than failing the whole
	// file, so the affected hosts are no longer pinned. Defaults to a
	// warning on stderr.
	KnownHostsWarn KnownHostsWarnFunc

	// LegacyAlgorithms appends the legacy KEX/cipher/MAC/host-key tail
	// (group1-sha1, CBC modes, hmac-sha1/md5, ssh-rsa/ssh-dss) after the
	// modern set. Required for old routers/switches; off by default.
	LegacyAlgorithms bool
}

func (c *Config) withDefaults() Config {
	out := *c
	if out.Port == 0 {
		out.Port = 22
	}
	if out.Timeout == 0 {
		out.Timeout = 30 * time.Second
	}
	if out.KnownHostsPath == "" {
		out.KnownHostsPath = defaultKnownHostsPath()
	}
	return out
}
