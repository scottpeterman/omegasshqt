// telnetx/telnet.go
// Package telnetx is the telnet transport.
//
// Backend satisfies transport.Transport, so the terminal widget drives a telnet
// session through the identical path it uses for SSH and serial: same gopyte
// stream, same session logging, same read loop, no branching on which one it
// holds.
//
// Telnet is plaintext, including whatever password the device prompts for.
// It is here because the equipment that needs it has no alternative: terminal
// and console servers, reverse-telnet console ports on GNS3 and dynamips, and
// legacy gear with no SSH stack at all. It is never a fallback from a failed
// SSH connection -- that would silently downgrade a session the operator
// believed was encrypted.
//
// What telnet actually is, for the parts that matter here:
//
//   - Raw TCP plus an in-band option-negotiation protocol (RFC 854). Every
//     control sequence is introduced by the IAC byte (0xFF). The Read path is
//     a small state machine that consumes those sequences, answers them on the
//     socket, and hands gopyte only the application data - so a "DO SUPPRESS
//     GO-AHEAD" never reaches the emulator as garbage.
//
//   - Negotiation must not loop. The classic telnet bug is two peers ping-
//     ponging WILL/DO forever. We track our declared state per option and only
//     reply when a request would actually change it (RFC 854's "don't respond
//     if the state already matches" rule).
//
//   - Line endings. RFC 854 makes CR LF the telnet newline; the terminal
//     widget emits a bare CR on Enter. On write we expand a lone CR to CR LF
//     (most network gear and reverse-telnet console servers expect this) and
//     double any literal 0xFF in user data per RFC 854 section 3.
//
//   - Window size. We advertise WILL NAWS and push the real terminal size via
//     subnegotiation, so a device that honors NAWS (and full-screen tools over
//     it) lay out at the right width instead of a hardcoded 80 columns.
//
// Use cases: telnet-only legacy gear, terminal-server / console-server lines,
// and GNS3 / dynamips reverse-telnet console ports.
package telnetx

import (
	"errors"
	"fmt"
	"net"
	"sync"
	"time"

	"github.com/scottpeterman/omegassh/transport"
)

// ErrNotConnected is returned by Read/Write before Connect (or after Close).
var ErrNotConnected = errors.New("telnet: not connected")

// IAC and the command/option bytes we care about (RFC 854 / 855 / 1073 / 1091).
const (
	iac  = 255 // Interpret As Command
	dont = 254
	doo  = 253 // "DO" (do is a Go keyword)
	wont = 252
	will = 251
	sb   = 250 // subnegotiation begin
	se   = 240 // subnegotiation end

	optEcho  = 1  // RFC 857
	optSGA   = 3  // suppress go-ahead, RFC 858
	optTType = 24 // terminal type, RFC 1091
	optNAWS  = 31 // negotiate about window size, RFC 1073

	ttypeIS   = 0
	ttypeSEND = 1
)

// termType is what we answer to an IAC SB TTYPE SEND. "XTERM" matches what the
// gopyte emulator targets; uppercase is conventional on the wire.
const termType = "XTERM"

// Config describes a telnet connection. Kept string/int-only so callers (YAML
// session files, CLI flags, the Quick Connect dialog) need not import telnetx
// to name a setting.
type Config struct {
	Host string // hostname or IP
	Port int    // default 23

	// ConnectTimeout bounds the TCP dial. Zero means a 15s default; the read
	// loop itself is blocking (no per-read deadline) once connected.
	ConnectTimeout time.Duration

	// CRLF, when true (the default via withDefaults), expands a lone CR on
	// write to CR LF. Disable only for a device that echoes a doubled newline.
	CRLF bool

	// TermType is answered to a telnet TTYPE SEND subnegotiation; "" defaults
	// to XTERM via withDefaults. Lets a saved session pin a terminal type the
	// same way the SSH path does.
	TermType string

	crlfSet bool // internal: distinguishes "CRLF=false on purpose" from zero value
}

// WithCRLF returns a copy of c with CRLF set explicitly (so a deliberate
// false survives withDefaults instead of being reset to true).
func (c Config) WithCRLF(v bool) Config {
	c.CRLF = v
	c.crlfSet = true
	return c
}

func (c Config) withDefaults() Config {
	if c.Port == 0 {
		c.Port = 23
	}
	if c.ConnectTimeout == 0 {
		c.ConnectTimeout = 15 * time.Second
	}
	if !c.crlfSet {
		c.CRLF = true
	}
	if c.TermType == "" {
		c.TermType = termType
	}
	return c
}

func (c Config) addr() string {
	c = c.withDefaults()
	return net.JoinHostPort(c.Host, fmt.Sprintf("%d", c.Port))
}

// Summary is a short human-readable form, e.g. "10.0.0.1:23".
func (c Config) Summary() string { return c.addr() }

// parseState is the inbound IAC state machine's position.
type parseState int

const (
	stData parseState = iota
	stIAC
	stWill
	stWont
	stDo
	stDont
	stSB     // expecting the SB option byte
	stSBData // collecting subnegotiation payload
	stSBIAC  // saw IAC inside subnegotiation (expect SE, or escaped IAC)
)

// Backend is a telnet connection that satisfies the terminal backend contract.
type Backend struct {
	cfg Config

	mu   sync.Mutex
	conn net.Conn
	open bool
	shut bool // Close was called; blocks a later Connect on the same Backend

	// done is closed once, when the session ends for any reason; err records
	// why. A local Close reports nil, matching serialx and transport.Session --
	// see finish.
	done     chan struct{}
	doneOnce sync.Once
	err      error

	// window size for NAWS; updated by Resize, sent on negotiation and resize.
	cols, rows int

	// Inbound parse state, carried across Read calls because an IAC sequence
	// can be split over TCP segment boundaries.
	state  parseState
	sbOpt  byte
	sbBuf  []byte
	prevCR bool // last emitted data byte was CR (to drop a following NUL)

	// Declared option state, to obey RFC 854's "reply only on change" and so
	// avoid negotiation loops. Absence means undeclared.
	localWill map[byte]bool // options WE will perform (we sent WILL/WONT)
	remoteDo  map[byte]bool // options the PEER may perform (we sent DO/DONT)
}

// New builds a Backend from cfg, applying defaults. It does not dial; call
// Connect.
func New(cfg Config) *Backend {
	return &Backend{
		cfg:       cfg.withDefaults(),
		cols:      80,
		rows:      24,
		done:      make(chan struct{}),
		localWill: make(map[byte]bool),
		remoteDo:  make(map[byte]bool),
	}
}

// Config returns the effective (defaulted) configuration.
func (b *Backend) Config() Config { return b.cfg }

// Connect dials the TCP socket and sends our opening option offers. Unlike SSH
// there is no auth or host-key step; the device's login prompt (if any) arrives
// as ordinary data once connected.
func (b *Backend) Connect() error {
	b.mu.Lock()
	if b.shut {
		b.mu.Unlock()
		return ErrNotConnected
	}
	if b.open {
		b.mu.Unlock()
		return errors.New("telnet: already connected")
	}
	b.mu.Unlock()

	conn, err := net.DialTimeout("tcp", b.cfg.addr(), b.cfg.ConnectTimeout)
	if err != nil {
		return fmt.Errorf("dial %s: %w", b.cfg.addr(), err)
	}

	b.mu.Lock()
	b.conn = conn
	b.open = true
	b.mu.Unlock()

	// Opening offers. Advertise terminal type and window size, and ask the
	// peer to suppress go-ahead and to echo (network devices echo). Each
	// declaration is recorded so a mirrored DO/WILL from the peer is a no-op
	// rather than a fresh round.
	b.localWill[optTType] = true
	b.localWill[optNAWS] = true
	b.remoteDo[optSGA] = true
	b.remoteDo[optEcho] = true
	b.sendCmd(will, optTType)
	b.sendCmd(will, optNAWS)
	b.sendCmd(doo, optSGA)
	b.sendCmd(doo, optEcho)

	return nil
}

// Read fills p with application data, transparently consuming and answering any
// telnet negotiation in the byte stream. It loops over socket reads until it
// has at least one data byte (or an error), so a read that contained only
// negotiation never returns (0, nil) and spins the caller's loop.
func (b *Backend) Read(p []byte) (int, error) {
	b.mu.Lock()
	conn := b.conn
	b.mu.Unlock()
	if conn == nil {
		return 0, ErrNotConnected
	}

	raw := make([]byte, len(p))
	for {
		n, err := conn.Read(raw)
		if n > 0 {
			out := b.filter(raw[:n], p)
			if len(out) > 0 {
				return len(out), nil
			}
			// Only negotiation this pass - keep reading for real data.
			if err == nil {
				continue
			}
		}
		if err != nil {
			b.finish(err)
			return 0, err
		}
		// n == 0, no error: rare; loop and read again.
	}
}

// filter runs raw bytes through the IAC state machine, writing application data
// into dst and answering negotiation on the socket. It returns the slice of dst
// actually filled. dst is always >= len(raw), so it cannot overflow (every
// rule either drops a byte or emits exactly one).
func (b *Backend) filter(raw, dst []byte) []byte {
	w := 0
	emit := func(c byte) {
		// Drop a NUL that immediately follows CR (RFC 854 CR NUL = bare CR);
		// gopyte has no use for the stray NUL.
		if b.prevCR && c == 0x00 {
			b.prevCR = false
			return
		}
		b.prevCR = (c == '\r')
		dst[w] = c
		w++
	}

	for _, c := range raw {
		switch b.state {
		case stData:
			if c == iac {
				b.state = stIAC
			} else {
				emit(c)
			}
		case stIAC:
			switch c {
			case iac:
				emit(iac) // escaped 0xFF in data
				b.state = stData
			case will:
				b.state = stWill
			case wont:
				b.state = stWont
			case doo:
				b.state = stDo
			case dont:
				b.state = stDont
			case sb:
				b.sbOpt = 0
				b.sbBuf = b.sbBuf[:0]
				b.state = stSB
			default:
				// Single-byte command (GA, NOP, DM, etc.) - ignore.
				b.state = stData
			}
		case stWill:
			b.onWill(c)
			b.state = stData
		case stWont:
			b.onWont(c)
			b.state = stData
		case stDo:
			b.onDo(c)
			b.state = stData
		case stDont:
			b.onDont(c)
			b.state = stData
		case stSB:
			b.sbOpt = c
			b.state = stSBData
		case stSBData:
			if c == iac {
				b.state = stSBIAC
			} else {
				b.sbBuf = append(b.sbBuf, c)
			}
		case stSBIAC:
			switch c {
			case se:
				b.onSubneg()
				b.state = stData
			case iac:
				b.sbBuf = append(b.sbBuf, iac) // escaped 0xFF inside SB
				b.state = stSBData
			default:
				// Malformed; resync to data.
				b.state = stData
			}
		}
	}
	return dst[:w]
}

// onWill handles a peer "I WILL do <opt>". We accept ECHO and SGA (so the peer
// drives echo and char-at-a-time), refuse the rest. Reply only on a change.
func (b *Backend) onWill(opt byte) {
	want := opt == optEcho || opt == optSGA
	if cur, ok := b.remoteDo[opt]; !ok || cur != want {
		b.remoteDo[opt] = want
		if want {
			b.sendCmd(doo, opt)
		} else {
			b.sendCmd(dont, opt)
		}
	}
}

// onWont handles "I WON'T do <opt>". Acknowledge with DONT, once.
func (b *Backend) onWont(opt byte) {
	if cur, ok := b.remoteDo[opt]; !ok || cur {
		b.remoteDo[opt] = false
		b.sendCmd(dont, opt)
	}
}

// onDo handles "please DO <opt>". We agree to NAWS, TTYPE and SGA; refuse the
// rest. On agreeing to NAWS, push the current window size immediately.
func (b *Backend) onDo(opt byte) {
	want := opt == optNAWS || opt == optTType || opt == optSGA
	if cur, ok := b.localWill[opt]; !ok || cur != want {
		b.localWill[opt] = want
		if want {
			b.sendCmd(will, opt)
			if opt == optNAWS {
				b.sendNAWS()
			}
		} else {
			b.sendCmd(wont, opt)
		}
	}
}

// onDont handles "please DON'T <opt>". Acknowledge with WONT, once.
func (b *Backend) onDont(opt byte) {
	if cur, ok := b.localWill[opt]; !ok || cur {
		b.localWill[opt] = false
		b.sendCmd(wont, opt)
	}
}

// onSubneg processes a completed subnegotiation. The only one we answer is a
// terminal-type SEND, to which we reply with our configured type.
func (b *Backend) onSubneg() {
	if b.sbOpt == optTType && len(b.sbBuf) >= 1 && b.sbBuf[0] == ttypeSEND {
		tt := b.cfg.TermType
		if tt == "" {
			tt = termType // direct-built backends that skipped withDefaults
		}
		payload := []byte{iac, sb, optTType, ttypeIS}
		payload = append(payload, []byte(tt)...)
		payload = append(payload, iac, se)
		b.rawWrite(payload)
	}
}

// sendCmd writes a 3-byte IAC command (IAC <verb> <opt>).
func (b *Backend) sendCmd(verb, opt byte) {
	b.rawWrite([]byte{iac, verb, opt})
}

// nawsFrame builds IAC SB NAWS <w-hi> <w-lo> <h-hi> <h-lo> IAC SE, doubling any
// 0xFF that lands in the four size bytes (RFC 1073 / 854 section 3). Pure, so
// the framing is unit-testable without a socket.
func nawsFrame(cols, rows int) []byte {
	if cols <= 0 {
		cols = 80
	}
	if rows <= 0 {
		rows = 24
	}
	dims := []byte{byte(cols >> 8), byte(cols), byte(rows >> 8), byte(rows)}
	out := []byte{iac, sb, optNAWS}
	for _, d := range dims {
		if d == iac {
			out = append(out, iac, iac)
		} else {
			out = append(out, d)
		}
	}
	return append(out, iac, se)
}

// sendNAWS writes the current window size to the socket.
func (b *Backend) sendNAWS() {
	b.mu.Lock()
	cols, rows := b.cols, b.rows
	b.mu.Unlock()
	b.rawWrite(nawsFrame(cols, rows))
}

// rawWrite writes bytes straight to the socket without CRLF or IAC transforms.
// Used for negotiation frames, which are already exact wire bytes. A write
// concurrent with the blocking Read is fine for a TCP conn.
func (b *Backend) rawWrite(p []byte) {
	b.mu.Lock()
	conn := b.conn
	b.mu.Unlock()
	if conn != nil {
		_, _ = conn.Write(p)
	}
}

// transformWrite applies the on-wire encoding to user data: optional CR -> CR
// LF expansion and mandatory IAC doubling. Pure, so it is unit-testable.
func transformWrite(p []byte, crlf bool) []byte {
	out := make([]byte, 0, len(p)+8)
	for i := 0; i < len(p); i++ {
		c := p[i]
		if crlf && c == '\r' {
			out = append(out, '\r')
			// Only synthesize the LF if the next byte isn't already one.
			if i+1 >= len(p) || p[i+1] != '\n' {
				out = append(out, '\n')
			}
			continue
		}
		if c == iac {
			out = append(out, iac, iac) // double a literal 0xFF
			continue
		}
		out = append(out, c)
	}
	return out
}

// Write sends user data: CR -> CR LF expansion (when enabled) and IAC doubling,
// then a single socket write.
func (b *Backend) Write(p []byte) (int, error) {
	b.mu.Lock()
	conn := b.conn
	crlf := b.cfg.CRLF
	b.mu.Unlock()
	if conn == nil {
		return 0, ErrNotConnected
	}

	if _, err := conn.Write(transformWrite(p, crlf)); err != nil {
		b.finish(err)
		return 0, err
	}
	// Report the caller's logical byte count, not the on-wire count.
	return len(p), nil
}

// Resize records the new window size and, if NAWS was negotiated, pushes it.
// Unlike the serial backend's no-op Resize this one is real: a device that
// honours NAWS lays out at the true width instead of a hardcoded 80 columns.
func (b *Backend) Resize(sz transport.Size) error {
	if !sz.Valid() {
		return nil
	}
	b.mu.Lock()
	b.cols, b.rows = sz.Cols, sz.Rows
	naws := b.localWill[optNAWS]
	b.mu.Unlock()
	if naws {
		b.sendNAWS()
	}
	return nil
}

// Close closes the socket; an in-flight blocking Read returns an error.
func (b *Backend) Close() error {
	b.mu.Lock()
	conn := b.conn
	b.conn = nil
	b.shut = true
	b.mu.Unlock()

	// Recorded before the socket is torn down, so the read error that Close
	// itself provokes cannot overwrite it with a spurious failure.
	b.finish(nil)

	if conn == nil {
		return nil
	}
	return conn.Close()
}

// Done is closed when the session ends, whether by a dropped connection, a
// read or write failure, or a local Close.
func (b *Backend) Done() <-chan struct{} { return b.done }

// Err reports why the session ended; meaningful only once Done is closed. A
// local Close reports nil, matching the SSH and serial sides: an operator
// disconnecting is not a failure and should not surface as one.
func (b *Backend) Err() error {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.err
}

// IsConnected reports whether the socket is currently usable. It goes false on
// a read or write failure, not just on Close.
func (b *Backend) IsConnected() bool {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.open
}

// finish records the terminating error, clears the connected flag and closes
// done, at most once.
func (b *Backend) finish(err error) {
	b.doneOnce.Do(func() {
		b.mu.Lock()
		b.err = err
		b.open = false
		b.mu.Unlock()
		close(b.done)
	})
}

// Backend is a transport.Transport. Asserted here so a signature drift in either
// package is a compile error rather than a runtime surprise at the widget.
var _ transport.Transport = (*Backend)(nil)
