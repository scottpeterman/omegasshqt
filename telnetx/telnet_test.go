// telnetx/telnet_test.go
package telnetx

import (
	"bytes"
	"errors"
	"net"
	"testing"
	"time"

	"github.com/scottpeterman/omegassh/transport"
)

// newTestBackend returns a Backend with maps initialized but no socket, for
// driving filter()/Write transforms directly.
func newTestBackend() *Backend {
	return &Backend{
		cfg:       Config{Port: 23, CRLF: true, crlfSet: true},
		cols:      80,
		rows:      24,
		done:      make(chan struct{}),
		localWill: make(map[byte]bool),
		remoteDo:  make(map[byte]bool),
	}
}

func TestFilterStripsNegotiationKeepsData(t *testing.T) {
	b := newTestBackend()
	dst := make([]byte, 256)
	// "Hi" + IAC DO SGA + "!" -> data should be "Hi!"
	raw := []byte{'H', 'i', iac, doo, optSGA, '!'}
	out := b.filter(raw, dst)
	if string(out) != "Hi!" {
		t.Fatalf("data = %q, want %q", out, "Hi!")
	}
}

func TestFilterSplitAcrossReads(t *testing.T) {
	b := newTestBackend()
	dst := make([]byte, 256)
	// IAC split across two filter calls: first chunk ends mid-command. The real
	// readLoop copies each result out before the next Read reuses the buffer, so
	// copy here too rather than holding a slice into the shared dst.
	out1 := append([]byte(nil), b.filter([]byte{'A', iac, will}, dst)...)
	out2 := append([]byte(nil), b.filter([]byte{optEcho, 'B'}, dst)...)
	got := string(out1) + string(out2)
	if got != "AB" {
		t.Fatalf("data across split = %q, want %q", got, "AB")
	}
}

func TestFilterEscapedIAC(t *testing.T) {
	b := newTestBackend()
	dst := make([]byte, 16)
	// IAC IAC in data is a single literal 0xFF byte.
	out := b.filter([]byte{'x', iac, iac, 'y'}, dst)
	if !bytes.Equal(out, []byte{'x', 0xFF, 'y'}) {
		t.Fatalf("escaped IAC = % x, want 78 ff 79", out)
	}
}

func TestFilterDropsNulAfterCR(t *testing.T) {
	b := newTestBackend()
	dst := make([]byte, 16)
	out := b.filter([]byte{'a', '\r', 0x00, 'b'}, dst)
	if string(out) != "a\rb" {
		t.Fatalf("CR NUL handling = %q, want %q", out, "a\rb")
	}
}

func TestFilterSubnegotiation(t *testing.T) {
	b := newTestBackend()
	dst := make([]byte, 16)
	// A TTYPE SEND subnegotiation carries no data and must be fully consumed.
	raw := []byte{'a', iac, sb, optTType, ttypeSEND, iac, se, 'b'}
	out := b.filter(raw, dst)
	if string(out) != "ab" {
		t.Fatalf("subneg consumption = %q, want %q", out, "ab")
	}
}

// fakeConn is a net.Conn whose Write buffers bytes without blocking, so the
// filter state machine completes a transition before we inspect the reply -
// unlike net.Pipe, and matching how a real buffered TCP socket behaves.
type fakeConn struct {
	written   bytes.Buffer
	failWrite bool
}

func (f *fakeConn) Read(p []byte) (int, error) { return 0, nil }
func (f *fakeConn) Write(p []byte) (int, error) {
	if f.failWrite {
		return 0, errors.New("fake conn: write failed")
	}
	return f.written.Write(p)
}
func (f *fakeConn) Close() error                       { return nil }
func (f *fakeConn) LocalAddr() net.Addr                { return nil }
func (f *fakeConn) RemoteAddr() net.Addr               { return nil }
func (f *fakeConn) SetDeadline(t time.Time) error      { return nil }
func (f *fakeConn) SetReadDeadline(t time.Time) error  { return nil }
func (f *fakeConn) SetWriteDeadline(t time.Time) error { return nil }

// TestNegotiationReplyOnlyOnChange confirms a supported DO is answered with
// WILL, and that a *repeated* DO produces no second reply (no negotiation loop).
func TestNegotiationReplyOnlyOnChange(t *testing.T) {
	fc := &fakeConn{}
	b := newTestBackend()
	b.conn = fc
	b.open = true
	dst := make([]byte, 64)

	// Peer asks us to DO SGA. We support it -> WILL SGA.
	b.filter([]byte{iac, doo, optSGA}, dst)
	if got := fc.written.Bytes(); !bytes.Equal(got, []byte{iac, will, optSGA}) {
		t.Fatalf("reply to DO SGA = % x, want ff fb 03", got)
	}

	// Repeated DO SGA: state already matches, so no further bytes on the wire.
	before := fc.written.Len()
	b.filter([]byte{iac, doo, optSGA}, dst)
	if after := fc.written.Len(); after != before {
		t.Fatalf("repeated DO SGA produced %d extra bytes; expected none",
			after-before)
	}
}

// TestNegotiationRefusesUnknownOption confirms an unsupported DO is refused with
// WONT (so the peer stops asking) rather than ignored.
func TestNegotiationRefusesUnknownOption(t *testing.T) {
	fc := &fakeConn{}
	b := newTestBackend()
	b.conn = fc
	b.open = true
	dst := make([]byte, 64)

	const optLineMode = 34 // not supported
	b.filter([]byte{iac, doo, optLineMode}, dst)
	if got := fc.written.Bytes(); !bytes.Equal(got, []byte{iac, wont, optLineMode}) {
		t.Fatalf("reply to DO LINEMODE = % x, want ff fc 22", got)
	}
}

func TestWriteCRLFExpansion(t *testing.T) {
	// Simulate Enter: a lone CR becomes CR LF.
	got := transformWrite([]byte{'\r'}, true)
	if !bytes.Equal(got, []byte{'\r', '\n'}) {
		t.Fatalf("CR expansion = % x, want 0d 0a", got)
	}
	// CR already followed by LF is not doubled.
	got = transformWrite([]byte{'\r', '\n'}, true)
	if !bytes.Equal(got, []byte{'\r', '\n'}) {
		t.Fatalf("CRLF passthrough = % x, want 0d 0a", got)
	}
	// With CRLF disabled, CR passes through untouched.
	got = transformWrite([]byte{'\r'}, false)
	if !bytes.Equal(got, []byte{'\r'}) {
		t.Fatalf("CR with CRLF off = % x, want 0d", got)
	}
}

func TestWriteIACDoubling(t *testing.T) {
	got := transformWrite([]byte{0xFF, 'z'}, true)
	if !bytes.Equal(got, []byte{0xFF, 0xFF, 'z'}) {
		t.Fatalf("IAC doubling = % x, want ff ff 7a", got)
	}
}

func TestSubnegReplyUsesConfiguredTermType(t *testing.T) {
	fc := &fakeConn{}
	b := newTestBackend()
	b.cfg.TermType = "vt100"
	b.conn = fc
	b.open = true
	dst := make([]byte, 64)

	// Server sends IAC SB TTYPE SEND IAC SE; we must answer IS "VT100".
	b.filter([]byte{iac, sb, optTType, ttypeSEND, iac, se}, dst)
	want := append([]byte{iac, sb, optTType, ttypeIS}, []byte("vt100")...)
	want = append(want, iac, se)
	if got := fc.written.Bytes(); !bytes.Equal(got, want) {
		t.Fatalf("TTYPE reply = % x, want % x", got, want)
	}
}

func TestConfigTermTypeDefault(t *testing.T) {
	if got := (Config{}).withDefaults().TermType; got != termType {
		t.Fatalf("default TermType = %q, want %q", got, termType)
	}
	if got := (Config{TermType: "ansi"}).withDefaults().TermType; got != "ansi" {
		t.Fatalf("explicit TermType = %q, want ansi", got)
	}
}

// TestCloseReportsNilErr pins the contract the widget depends on: a local
// teardown is not a failure. transport.Session and serialx.Backend both guarantee
// this, and watchDone in the UI reports anything non-nil to the operator -- so
// a telnet backend that leaked its own close error would put a spurious
// "connection lost" on screen every time someone closed a tab.
func TestCloseReportsNilErr(t *testing.T) {
	b := newTestBackend()

	if err := b.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}
	select {
	case <-b.Done():
	default:
		t.Fatal("Done not closed after Close")
	}
	if err := b.Err(); err != nil {
		t.Errorf("Err after local Close = %v, want nil", err)
	}
	if b.IsConnected() {
		t.Error("IsConnected true after Close")
	}
	// Idempotent: a second Close must not panic on the already-closed channel.
	if err := b.Close(); err != nil {
		t.Errorf("second Close: %v", err)
	}
}

// TestFailureIsReported is the other half: a read or write failure is the far
// end going away, and Err has to say so.
func TestFailureIsReported(t *testing.T) {
	b := newTestBackend()
	b.conn = &fakeConn{failWrite: true}
	b.open = true

	if _, err := b.Write([]byte("show version\r")); err == nil {
		t.Fatal("Write to a failing conn returned nil error")
	}
	select {
	case <-b.Done():
	default:
		t.Fatal("Done not closed after a write failure")
	}
	if b.Err() == nil {
		t.Error("Err after a write failure = nil, want the failure")
	}
}

// TestResizeIgnoresInvalidSize guards the case the widget actually produces:
// a size query before layout has run returns zeroes, and pushing a 0x0 NAWS
// frame tells the device the window has no width.
func TestResizeIgnoresInvalidSize(t *testing.T) {
	b := newTestBackend()
	b.localWill[optNAWS] = true
	fc := &fakeConn{}
	b.conn = fc
	b.open = true

	if err := b.Resize(transport.Size{Cols: 0, Rows: 0}); err != nil {
		t.Fatalf("Resize: %v", err)
	}
	if fc.written.Len() != 0 {
		t.Errorf("invalid size sent %d bytes on the wire, want 0", fc.written.Len())
	}

	if err := b.Resize(transport.Size{Cols: 132, Rows: 40}); err != nil {
		t.Fatalf("Resize: %v", err)
	}
	if fc.written.Len() == 0 {
		t.Error("valid resize sent nothing; expected a NAWS subnegotiation")
	}
	if b.cols != 132 || b.rows != 40 {
		t.Errorf("recorded size = %dx%d, want 132x40", b.cols, b.rows)
	}
}
