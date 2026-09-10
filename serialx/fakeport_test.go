// serialx/fakeport_test.go
// A programmable serial.Port, so the lifecycle this package actually has to
// get right -- connect, blocking read, unplug mid-session, close unblocking a
// blocked read -- is testable on a machine with no serial hardware.
//
// The alternative was testing only the pure mapping functions and taking the
// state machine on faith, which is backwards: the mapping is the part that
// fails loudly, and the unplug path is the part that fails at 2am.
package serialx

import (
	"errors"
	"io"
	"sync"
	"time"

	"go.bug.st/serial"
)

// errUnplugged stands in for what a yanked USB adapter produces.
var errUnplugged = errors.New("device not configured")

// fakePort implements serial.Port. Reads block until fed, unplugged, or
// closed, which is the behaviour that matters here.
type fakePort struct {
	mu     sync.Mutex
	closed bool
	failed error

	inbox chan []byte
	gone  chan struct{}

	written  []byte
	mode     *serial.Mode
	readTO   time.Duration
	toSet    bool
	writeErr error
	toErr    error
}

func newFakePort() *fakePort {
	return &fakePort{
		inbox: make(chan []byte, 16),
		gone:  make(chan struct{}),
	}
}

// feed queues bytes for the next Read.
func (f *fakePort) feed(s string) { f.inbox <- []byte(s) }

// unplug makes the in-flight and all subsequent Reads fail, the way a
// physically removed adapter does.
func (f *fakePort) unplug() {
	f.mu.Lock()
	if f.failed == nil {
		f.failed = errUnplugged
	}
	f.mu.Unlock()
	f.closeGone()
}

func (f *fakePort) closeGone() {
	select {
	case <-f.gone:
	default:
		close(f.gone)
	}
}

func (f *fakePort) Read(p []byte) (int, error) {
	f.mu.Lock()
	if err := f.failed; err != nil {
		f.mu.Unlock()
		return 0, err
	}
	if f.closed {
		f.mu.Unlock()
		return 0, io.EOF
	}
	f.mu.Unlock()

	select {
	case b := <-f.inbox:
		n := copy(p, b)
		return n, nil
	case <-f.gone:
		f.mu.Lock()
		err := f.failed
		f.mu.Unlock()
		if err == nil {
			err = io.EOF
		}
		return 0, err
	}
}

func (f *fakePort) Write(p []byte) (int, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.writeErr != nil {
		return 0, f.writeErr
	}
	if f.closed {
		return 0, io.EOF
	}
	f.written = append(f.written, p...)
	return len(p), nil
}

func (f *fakePort) Close() error {
	f.mu.Lock()
	f.closed = true
	f.mu.Unlock()
	f.closeGone()
	return nil
}

func (f *fakePort) SetReadTimeout(t time.Duration) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.toErr != nil {
		return f.toErr
	}
	f.readTO = t
	f.toSet = true
	return nil
}

func (f *fakePort) SetMode(m *serial.Mode) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.mode = m
	return nil
}

func (f *fakePort) wrote() string {
	f.mu.Lock()
	defer f.mu.Unlock()
	return string(f.written)
}

func (f *fakePort) timeout() (time.Duration, bool) {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.readTO, f.toSet
}

// The remainder of the serial.Port surface, unused by this package.
func (f *fakePort) Drain() error             { return nil }
func (f *fakePort) ResetInputBuffer() error  { return nil }
func (f *fakePort) ResetOutputBuffer() error { return nil }
func (f *fakePort) SetDTR(bool) error        { return nil }
func (f *fakePort) SetRTS(bool) error        { return nil }
func (f *fakePort) GetModemStatusBits() (*serial.ModemStatusBits, error) {
	return &serial.ModemStatusBits{}, nil
}
func (f *fakePort) Break(time.Duration) error { return nil }

var _ serial.Port = (*fakePort)(nil)

// withFakePort swaps the package-level opener for the duration of a test and
// hands back the port plus the mode it was opened with.
func withFakePort(t interface{ Cleanup(func()) }, f *fakePort, openErr error) *[]*serial.Mode {
	modes := &[]*serial.Mode{}
	prev := openPort
	openPort = func(_ string, m *serial.Mode) (serial.Port, error) {
		if openErr != nil {
			return nil, openErr
		}
		*modes = append(*modes, m)
		return f, nil
	}
	t.Cleanup(func() { openPort = prev })
	return modes
}
