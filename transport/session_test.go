// transport/session_test.go
// Behaviour tests for the session contract.
//
// The two backends arrived with their own suites and those still run, so what
// is unproven is the layer added on top: the states, the terminal verdict, and
// the tap. Those are what these cover, with fakes for both shapes -- a shell
// (SSH) and a Transport (telnet, serial) -- so no socket or adapter is needed.
package transport

import (
	"errors"
	"io"
	"strings"
	"sync"
	"testing"
	"time"
)

// --- fakes -----------------------------------------------------------------

// fakeShell stands in for *sshcore.Session.
type fakeShell struct {
	out      io.Reader
	written  []byte
	resized  []Size
	waitErr  error
	waitedCh chan struct{}
	closed   bool
	writeErr error
	mu       sync.Mutex
}

func newFakeShell(out string) *fakeShell {
	return &fakeShell{out: strings.NewReader(out), waitedCh: make(chan struct{})}
}

func (f *fakeShell) Stdout() io.Reader { return f.out }

func (f *fakeShell) Write(p []byte) (int, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.writeErr != nil {
		return 0, f.writeErr
	}
	f.written = append(f.written, p...)
	return len(p), nil
}

func (f *fakeShell) Resize(cols, rows int) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.resized = append(f.resized, Size{Cols: cols, Rows: rows})
	return nil
}

func (f *fakeShell) Wait() error {
	close(f.waitedCh)
	return f.waitErr
}

func (f *fakeShell) Close() error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.closed = true
	return nil
}

// fakeTransport stands in for a telnetx or serialx Backend.
type fakeTransport struct {
	mu       sync.Mutex
	reads    chan []byte
	readErr  error
	written  []byte
	resized  []Size
	open     bool
	done     chan struct{}
	doneOnce sync.Once
	err      error
	pending  []byte
}

func newFakeTransport() *fakeTransport {
	return &fakeTransport{reads: make(chan []byte, 8), done: make(chan struct{}), open: true}
}

func (f *fakeTransport) Read(p []byte) (int, error) {
	f.mu.Lock()
	if len(f.pending) > 0 {
		n := copy(p, f.pending)
		f.pending = f.pending[n:]
		f.mu.Unlock()
		return n, nil
	}
	f.mu.Unlock()

	chunk, ok := <-f.reads
	if !ok {
		f.mu.Lock()
		err := f.readErr
		f.mu.Unlock()
		if err == nil {
			err = io.EOF
		}
		f.finish(err)
		return 0, err
	}
	n := copy(p, chunk)
	if n < len(chunk) {
		f.mu.Lock()
		f.pending = append(f.pending, chunk[n:]...)
		f.mu.Unlock()
	}
	return n, nil
}

func (f *fakeTransport) Write(p []byte) (int, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.written = append(f.written, p...)
	return len(p), nil
}

func (f *fakeTransport) Connect() error { return nil }

func (f *fakeTransport) Resize(s Size) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.resized = append(f.resized, s)
	return nil
}

func (f *fakeTransport) Done() <-chan struct{} { return f.done }

func (f *fakeTransport) Err() error {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.err
}

func (f *fakeTransport) IsConnected() bool {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.open
}

// Close mimics telnetx and serialx: the terminating verdict is recorded
// before the socket or port is torn down, and the teardown then provokes a
// real read failure -- "use of closed network connection", not a clean EOF.
// A fake that returns EOF here would be too polite to exercise anything.
func (f *fakeTransport) Close() error {
	f.mu.Lock()
	f.readErr = errors.New("use of closed network connection")
	f.mu.Unlock()
	f.finish(nil)
	close(f.reads)
	return nil
}

func (f *fakeTransport) finish(err error) {
	f.doneOnce.Do(func() {
		f.mu.Lock()
		f.err = err
		f.open = false
		f.mu.Unlock()
		close(f.done)
	})
}

// drop simulates the far end going away mid-session: the read loop is
// unblocked with a failure rather than a clean EOF.
func (f *fakeTransport) drop(err error) {
	f.mu.Lock()
	f.readErr = err
	f.mu.Unlock()
	close(f.reads)
}

// waitState polls until the session reaches want, or fails the test.
func waitState(t *testing.T, s Session, want State) {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if s.State() == want {
			return
		}
		time.Sleep(time.Millisecond)
	}
	t.Fatalf("state is %s, want %s", s.State(), want)
}

// --- Size and Kind ---------------------------------------------------------

func TestSizeValid(t *testing.T) {
	for _, tc := range []struct {
		s    Size
		want bool
	}{
		{Size{80, 24}, true},
		{Size{}, false},
		{Size{Cols: 80}, false},
		{Size{Rows: 24}, false},
		{Size{Cols: -1, Rows: 24}, false},
	} {
		if got := tc.s.Valid(); got != tc.want {
			t.Errorf("Size%v.Valid() = %v, want %v", tc.s, got, tc.want)
		}
	}
}

// An empty transport string has to mean SSH. Every caller written against the
// surface before the selector existed sends no transport field at all, and
// rejecting that would break all of them at once.
func TestParseKindEmptyIsSSH(t *testing.T) {
	k, err := ParseKind("")
	if err != nil || k != KindSSH {
		t.Fatalf("ParseKind(\"\") = %v, %v; want ssh, nil", k, err)
	}
	for _, in := range []string{"telnet", "TELNET", " Telnet "} {
		if k, err := ParseKind(in); err != nil || k != KindTelnet {
			t.Errorf("ParseKind(%q) = %v, %v; want telnet, nil", in, k, err)
		}
	}
	if _, err := ParseKind("rlogin"); err == nil {
		t.Error("ParseKind(\"rlogin\") should have failed")
	}
}

// --- SSH adapter -----------------------------------------------------------

func TestSSHSessionReadsAndReaps(t *testing.T) {
	sh := newFakeShell("hello lab")
	s := WrapSSH(sh, "lab-sw1:22", nil)

	if s.Kind() != KindSSH || s.Summary() != "lab-sw1:22" {
		t.Fatalf("kind=%v summary=%q", s.Kind(), s.Summary())
	}
	if s.State() != StateConnected {
		t.Fatalf("state = %s, want connected", s.State())
	}

	got, err := io.ReadAll(s.Output())
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	if string(got) != "hello lab" {
		t.Fatalf("read %q", got)
	}

	// EOF on stdout is a clean end, not a failure.
	waitState(t, s, StateDisconnected)
	if s.Err() != nil {
		t.Fatalf("Err() = %v, want nil after clean EOF", s.Err())
	}
	if code := s.WaitExit(); code != 0 {
		t.Fatalf("WaitExit() = %d, want 0", code)
	}
}

// A zero Size is what a widget reports before its first layout pass. Pushing
// it downstream tells the remote tty its window is 0x0, so it is dropped here
// rather than in each transport.
func TestSSHSessionDropsInvalidResize(t *testing.T) {
	sh := newFakeShell("")
	s := WrapSSH(sh, "lab-sw1:22", nil)

	if err := s.Resize(Size{}); err != nil {
		t.Fatalf("Resize(zero): %v", err)
	}
	if err := s.Resize(Size{Cols: 132, Rows: 40}); err != nil {
		t.Fatalf("Resize(132x40): %v", err)
	}
	sh.mu.Lock()
	defer sh.mu.Unlock()
	if len(sh.resized) != 1 || sh.resized[0] != (Size{132, 40}) {
		t.Fatalf("shell saw %v, want one 132x40", sh.resized)
	}
}

// --- backend adapter -------------------------------------------------------

func TestBackendSessionEndsFailedOnDrop(t *testing.T) {
	ft := newFakeTransport()
	s := Wrap(ft, KindTelnet, "10.0.0.1:23", nil)

	var states []State
	var mu sync.Mutex
	s.SetEventHandler(func(ev Event) {
		mu.Lock()
		defer mu.Unlock()
		if ev.Kind == EventStateChanged {
			states = append(states, ev.State)
		}
	})

	boom := errors.New("connection reset by peer")
	go func() {
		ft.reads <- []byte("switch> ")
		ft.drop(boom)
	}()

	got, _ := io.ReadAll(s.Output())
	if string(got) != "switch> " {
		t.Fatalf("read %q", got)
	}

	waitState(t, s, StateFailed)
	if !errors.Is(s.Err(), boom) {
		t.Fatalf("Err() = %v, want %v", s.Err(), boom)
	}
	if code := s.WaitExit(); code != ExitUnknown {
		t.Fatalf("WaitExit() = %d, want ExitUnknown", code)
	}

	mu.Lock()
	defer mu.Unlock()
	if len(states) != 1 || states[0] != StateFailed {
		t.Fatalf("published %v, want one failed", states)
	}
}

// An operator disconnecting is not a failure. Close provokes the backend's own
// read error, and that must not be reported as the reason the session ended --
// the overlay would light up red on every deliberate disconnect.
func TestBackendSessionCloseIsNotAFailure(t *testing.T) {
	ft := newFakeTransport()
	s := Wrap(ft, KindSerial, "/dev/ttyUSB0 9600 8N1", nil)

	done := make(chan struct{})
	go func() {
		defer close(done)
		_, _ = io.ReadAll(s.Output())
	}()

	if err := s.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}
	<-done

	waitState(t, s, StateDisconnected)
	if s.Err() != nil {
		t.Fatalf("Err() = %v, want nil after a local Close", s.Err())
	}
}

// The backend closes its own Done on an unplug even when nothing is reading.
// A session whose read loop was never started still has to report the end.
func TestBackendSessionEndsWithoutAReadLoop(t *testing.T) {
	ft := newFakeTransport()
	s := Wrap(ft, KindSerial, "/dev/ttyUSB0 9600 8N1", nil)

	ft.finish(errors.New("device disconnected"))
	waitState(t, s, StateFailed)

	select {
	case <-s.Done():
	case <-time.After(2 * time.Second):
		t.Fatal("Done never closed")
	}
}

// The terminal verdict, tested on the state machine directly rather than
// through a backend. Going through one makes the outcome depend on whether
// the read loop or the Done bridge reaches finish first, and a test that
// catches a regression only sometimes is not evidence.
//
// A teardown-provoked read error is a non-EOF error, so nothing else in
// finish drops it: without the closing guard this session reports failed with
// "use of closed network connection" as the reason it ended, and every
// deliberate disconnect lights the overlay up red.
func TestLocalCloseSuppressesTheTeardownError(t *testing.T) {
	b := newBase(KindTelnet, "10.0.0.1:23", nil, StateConnected)
	b.markClosing()
	b.finish(errors.New("use of closed network connection"))

	if got := b.State(); got != StateDisconnected {
		t.Fatalf("state = %s, want disconnected", got)
	}
	if err := b.Err(); err != nil {
		t.Fatalf("Err() = %v, want nil after a local Close", err)
	}
}

// The same guard from the other side: an error that arrives without a Close
// having been asked for is a genuine failure and must survive.
func TestUnaskedForErrorIsAFailure(t *testing.T) {
	boom := errors.New("connection reset by peer")
	b := newBase(KindTelnet, "10.0.0.1:23", nil, StateConnected)
	b.finish(boom)

	if got := b.State(); got != StateFailed {
		t.Fatalf("state = %s, want failed", got)
	}
	if !errors.Is(b.Err(), boom) {
		t.Fatalf("Err() = %v, want %v", b.Err(), boom)
	}
}

// A repeated state is not a transition and must not be published as one, or a
// caller counting transitions is counting wakes instead. Driven through
// setState directly: a backend only ever reaches its terminal state once, so
// routing this through one tests the idempotence of finish rather than the
// guard in setState.
func TestStateChangeIsNotRepublished(t *testing.T) {
	var got []State
	var mu sync.Mutex

	b := newBase(KindTelnet, "10.0.0.1:23", nil, StateDisconnected)
	b.SetEventHandler(func(ev Event) {
		if ev.Kind == EventStateChanged {
			mu.Lock()
			got = append(got, ev.State)
			mu.Unlock()
		}
	})

	b.setState(StateConnecting)
	b.setState(StateConnecting)
	b.setState(StateConnected)
	b.setState(StateConnected)
	b.setState(StateConnecting) // a real move back is still a transition

	mu.Lock()
	defer mu.Unlock()
	want := []State{StateConnecting, StateConnected, StateConnecting}
	if len(got) != len(want) {
		t.Fatalf("published %v, want %v", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("published %v, want %v", got, want)
		}
	}
}

// Removing the handler stops delivery. The shim does this during teardown, so
// that a state change published while the notifier is being closed cannot
// reach a descriptor that has gone away.
func TestNilHandlerStopsDelivery(t *testing.T) {
	var n int
	var mu sync.Mutex

	b := newBase(KindSerial, "/dev/ttyUSB0 9600 8N1", nil, StateConnected)
	b.SetEventHandler(func(Event) { mu.Lock(); n++; mu.Unlock() })
	b.setState(StateFailed)
	b.SetEventHandler(nil)
	b.setState(StateDisconnected)

	mu.Lock()
	defer mu.Unlock()
	if n != 1 {
		t.Fatalf("handler fired %d times, want 1", n)
	}
}
