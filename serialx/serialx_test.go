// serialx/serialx_test.go
// Behaviour tests for the serial transport.
package serialx

import (
	"errors"
	"strings"
	"testing"
	"time"

	"go.bug.st/serial"

	"github.com/scottpeterman/omegassh/transport"
)

func TestConfigDefaults(t *testing.T) {
	got := Config{Port: "/dev/ttyUSB0"}.withDefaults()
	if got.Baud != 9600 || got.DataBits != 8 || got.Parity != "none" || got.StopBits != "1" {
		t.Errorf("defaults = %+v, want 9600 8 none 1", got)
	}

	// Explicit values must survive defaulting.
	set := Config{Port: "COM3", Baud: 115200, DataBits: 7, Parity: "even", StopBits: "2"}.withDefaults()
	if set.Baud != 115200 || set.DataBits != 7 || set.Parity != "even" || set.StopBits != "2" {
		t.Errorf("explicit config was overwritten: %+v", set)
	}
}

func TestConfigSummary(t *testing.T) {
	cases := []struct {
		in   Config
		want string
	}{
		{Config{}, "9600 8N1"},
		{Config{Baud: 115200}, "115200 8N1"},
		{Config{Baud: 19200, Parity: "even", StopBits: "2"}, "19200 8E2"},
		{Config{Baud: 9600, DataBits: 7, Parity: "odd"}, "9600 7O1"},
		{Config{Parity: "mark"}, "9600 8M1"},
		{Config{Parity: "space", StopBits: "1.5"}, "9600 8S1.5"},
	}
	for _, c := range cases {
		if got := c.in.Summary(); got != c.want {
			t.Errorf("Summary(%+v) = %q, want %q", c.in, got, c.want)
		}
	}
}

func TestModeMapping(t *testing.T) {
	m, err := Config{Baud: 115200, DataBits: 7, Parity: "even", StopBits: "2"}.mode()
	if err != nil {
		t.Fatalf("mode: %v", err)
	}
	if m.BaudRate != 115200 || m.DataBits != 7 {
		t.Errorf("mode = %d baud %d bits, want 115200 7", m.BaudRate, m.DataBits)
	}
	if m.Parity != serial.EvenParity {
		t.Errorf("parity = %v, want EvenParity", m.Parity)
	}
	if m.StopBits != serial.TwoStopBits {
		t.Errorf("stop bits = %v, want TwoStopBits", m.StopBits)
	}
}

func TestModeRejectsBadSettings(t *testing.T) {
	cases := []struct {
		name string
		cfg  Config
	}{
		{"unknown parity", Config{Parity: "sideways"}},
		{"unknown stop bits", Config{StopBits: "3"}},
		{"data bits too low", Config{DataBits: 4}},
		{"data bits too high", Config{DataBits: 9}},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			if _, err := c.cfg.mode(); err == nil {
				t.Errorf("mode() succeeded for %+v, want an error", c.cfg)
			}
		})
	}
}

func TestConnectSetsBlockingReadByDefault(t *testing.T) {
	f := newFakePort()
	withFakePort(t, f, nil)

	b := New(Config{Port: "/dev/ttyUSB0"})
	if err := b.Connect(); err != nil {
		t.Fatalf("connect: %v", err)
	}
	defer b.Close()

	to, set := f.timeout()
	if !set {
		t.Fatal("read timeout was never set explicitly")
	}
	// An interactive console wants Read to block, not to spin returning
	// (0, nil). This is set explicitly rather than left to the library.
	if to != serial.NoTimeout {
		t.Errorf("read timeout = %v, want serial.NoTimeout", to)
	}
}

func TestConnectHonoursExplicitReadTimeout(t *testing.T) {
	f := newFakePort()
	withFakePort(t, f, nil)

	b := New(Config{Port: "/dev/ttyUSB0", ReadTimeout: 250 * time.Millisecond})
	if err := b.Connect(); err != nil {
		t.Fatalf("connect: %v", err)
	}
	defer b.Close()

	if to, _ := f.timeout(); to != 250*time.Millisecond {
		t.Errorf("read timeout = %v, want 250ms", to)
	}
}

func TestConnectPassesMappedMode(t *testing.T) {
	f := newFakePort()
	modes := withFakePort(t, f, nil)

	b := New(Config{Port: "COM3", Baud: 115200, Parity: "odd", StopBits: "2", DataBits: 7})
	if err := b.Connect(); err != nil {
		t.Fatalf("connect: %v", err)
	}
	defer b.Close()

	if len(*modes) != 1 {
		t.Fatalf("opened %d times, want 1", len(*modes))
	}
	m := (*modes)[0]
	if m.BaudRate != 115200 || m.DataBits != 7 ||
		m.Parity != serial.OddParity || m.StopBits != serial.TwoStopBits {
		t.Errorf("opened with %+v, want 115200 7O2", m)
	}
}

func TestConnectRejectsBadConfigBeforeOpening(t *testing.T) {
	f := newFakePort()
	modes := withFakePort(t, f, nil)

	b := New(Config{Port: "/dev/ttyUSB0", Parity: "sideways"})
	if err := b.Connect(); err == nil {
		t.Fatal("connect succeeded with an invalid parity")
	}
	if len(*modes) != 0 {
		t.Error("the port was opened despite an invalid config")
	}
}

func TestConnectPropagatesOpenFailure(t *testing.T) {
	withFakePort(t, newFakePort(), errors.New("permission denied"))

	b := New(Config{Port: "/dev/ttyUSB0"})
	err := b.Connect()
	if err == nil {
		t.Fatal("connect succeeded on an open failure")
	}
	// The port name belongs in the message; "permission denied" alone sends
	// people looking at the wrong device.
	if !strings.Contains(err.Error(), "/dev/ttyUSB0") {
		t.Errorf("error %q does not name the port", err)
	}
	if b.IsConnected() {
		t.Error("IsConnected true after a failed connect")
	}
}

func TestReadWriteRoundTrip(t *testing.T) {
	f := newFakePort()
	withFakePort(t, f, nil)

	b := New(Config{Port: "/dev/ttyUSB0"})
	if err := b.Connect(); err != nil {
		t.Fatalf("connect: %v", err)
	}
	defer b.Close()

	if _, err := b.Write([]byte("show version\r")); err != nil {
		t.Fatalf("write: %v", err)
	}
	if got := f.wrote(); got != "show version\r" {
		t.Errorf("port received %q", got)
	}

	f.feed("lab-r1#")
	buf := make([]byte, 32)
	n, err := b.Read(buf)
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	if string(buf[:n]) != "lab-r1#" {
		t.Errorf("read %q, want %q", buf[:n], "lab-r1#")
	}
}

func TestReadWriteBeforeConnect(t *testing.T) {
	b := New(Config{Port: "/dev/ttyUSB0"})
	if _, err := b.Read(make([]byte, 8)); !errors.Is(err, ErrNotConnected) {
		t.Errorf("Read before Connect = %v, want ErrNotConnected", err)
	}
	if _, err := b.Write([]byte("x")); !errors.Is(err, ErrNotConnected) {
		t.Errorf("Write before Connect = %v, want ErrNotConnected", err)
	}
	if b.IsConnected() {
		t.Error("IsConnected true before Connect")
	}
}

// The case this whole seam exists for: the adapter is physically removed
// mid-session. Read must return the error rather than hanging or reporting
// (0, nil), and the rest of the state must follow it.
func TestUnplugSurfacesThroughDoneErrAndIsConnected(t *testing.T) {
	f := newFakePort()
	withFakePort(t, f, nil)

	b := New(Config{Port: "/dev/ttyUSB0"})
	if err := b.Connect(); err != nil {
		t.Fatalf("connect: %v", err)
	}
	defer b.Close()

	if !b.IsConnected() {
		t.Fatal("IsConnected false while connected")
	}

	readErr := make(chan error, 1)
	go func() {
		_, err := b.Read(make([]byte, 32))
		readErr <- err
	}()

	time.Sleep(20 * time.Millisecond)
	f.unplug()

	select {
	case err := <-readErr:
		if err == nil {
			t.Fatal("Read returned nil after an unplug")
		}
	case <-time.After(3 * time.Second):
		t.Fatal("Read hung after an unplug")
	}

	select {
	case <-b.Done():
	case <-time.After(3 * time.Second):
		t.Fatal("Done never closed after an unplug")
	}
	if b.Err() == nil {
		t.Error("Err() = nil after an unplug, want the read failure")
	}
	// This is the regression the carried-over version had: the flag stayed
	// true until something called Close.
	if b.IsConnected() {
		t.Error("IsConnected still true after an unplug")
	}
}

func TestWriteFailureAlsoEndsTheSession(t *testing.T) {
	f := newFakePort()
	withFakePort(t, f, nil)

	b := New(Config{Port: "/dev/ttyUSB0"})
	if err := b.Connect(); err != nil {
		t.Fatalf("connect: %v", err)
	}
	defer b.Close()

	f.mu.Lock()
	f.writeErr = errUnplugged
	f.mu.Unlock()

	if _, err := b.Write([]byte("x")); err == nil {
		t.Fatal("Write succeeded against a dead port")
	}
	select {
	case <-b.Done():
	case <-time.After(3 * time.Second):
		t.Fatal("Done never closed after a write failure")
	}
	if b.Err() == nil {
		t.Error("Err() = nil after a write failure")
	}
}

func TestCloseUnblocksAReadInFlight(t *testing.T) {
	f := newFakePort()
	withFakePort(t, f, nil)

	b := New(Config{Port: "/dev/ttyUSB0"})
	if err := b.Connect(); err != nil {
		t.Fatalf("connect: %v", err)
	}

	done := make(chan struct{})
	go func() {
		b.Read(make([]byte, 32))
		close(done)
	}()

	time.Sleep(20 * time.Millisecond)
	if err := b.Close(); err != nil {
		t.Fatalf("close: %v", err)
	}

	select {
	case <-done:
	case <-time.After(3 * time.Second):
		t.Fatal("Read did not unwind after Close")
	}
}

func TestCloseReportsNilErr(t *testing.T) {
	f := newFakePort()
	withFakePort(t, f, nil)

	b := New(Config{Port: "/dev/ttyUSB0"})
	if err := b.Connect(); err != nil {
		t.Fatalf("connect: %v", err)
	}
	if err := b.Close(); err != nil {
		t.Fatalf("close: %v", err)
	}

	<-b.Done()
	// An operator disconnecting is not a failure; a UI that shows a
	// transport error on a deliberate close is wrong.
	if err := b.Err(); err != nil {
		t.Errorf("Err() = %v after a local Close, want nil", err)
	}
	if b.IsConnected() {
		t.Error("IsConnected true after Close")
	}
}

func TestCloseIsIdempotentAndBeforeConnectIsSafe(t *testing.T) {
	b := New(Config{Port: "/dev/ttyUSB0"})
	if err := b.Close(); err != nil {
		t.Fatalf("close before connect: %v", err)
	}
	if err := b.Close(); err != nil {
		t.Fatalf("second close: %v", err)
	}
}

func TestBackendIsSingleUse(t *testing.T) {
	f := newFakePort()
	withFakePort(t, f, nil)

	b := New(Config{Port: "/dev/ttyUSB0"})
	if err := b.Connect(); err != nil {
		t.Fatalf("connect: %v", err)
	}
	if err := b.Close(); err != nil {
		t.Fatalf("close: %v", err)
	}
	// Reconnect policy lives above the transport; a closed Backend stays
	// closed so a stale one can never quietly reopen a port.
	if err := b.Connect(); !errors.Is(err, ErrClosed) {
		t.Errorf("Connect after Close = %v, want ErrClosed", err)
	}
}

func TestDoubleConnectRefused(t *testing.T) {
	f := newFakePort()
	withFakePort(t, f, nil)

	b := New(Config{Port: "/dev/ttyUSB0"})
	if err := b.Connect(); err != nil {
		t.Fatalf("connect: %v", err)
	}
	defer b.Close()

	if err := b.Connect(); err == nil {
		t.Error("second Connect succeeded, leaking the first port")
	}
}

// Resize must succeed and do nothing. The widget calls it on every layout
// pass without knowing which transport it holds; an error here would force it
// to branch on transport type, which is the thing the interface exists to
// avoid.
func TestResizeIsANoOpThatSucceeds(t *testing.T) {
	f := newFakePort()
	withFakePort(t, f, nil)

	b := New(Config{Port: "/dev/ttyUSB0"})
	if err := b.Connect(); err != nil {
		t.Fatalf("connect: %v", err)
	}
	defer b.Close()

	for _, s := range []transport.Size{{Cols: 80, Rows: 24}, {Cols: 200, Rows: 60}, {}} {
		if err := b.Resize(s); err != nil {
			t.Errorf("Resize(%+v) = %v, want nil", s, err)
		}
	}
	if f.wrote() != "" {
		t.Errorf("Resize sent %q downstream, want nothing", f.wrote())
	}
}

func TestConfigIsDefaultedOnTheBackend(t *testing.T) {
	b := New(Config{Port: "/dev/ttyUSB0"})
	if got := b.Config(); got.Baud != 9600 || got.StopBits != "1" {
		t.Errorf("Config() = %+v, want defaults applied", got)
	}
}

// List has no hardware to find in CI. It must not panic, and an empty result
// is a valid answer rather than an error condition.
func TestListDoesNotPanic(t *testing.T) {
	if _, err := List(); err != nil {
		t.Logf("List: %v (acceptable without hardware)", err)
	}
	if _, err := ListDetailed(); err != nil {
		t.Logf("ListDetailed: %v (acceptable without hardware)", err)
	}
}
