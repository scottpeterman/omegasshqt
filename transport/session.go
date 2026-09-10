// transport/session.go
// The session contract: what sits above a Transport and below the UI.
//
// The byte-level part of a session was reusable and is (Transport). This part
// was not, and could not be lifted from any of the backends: SSH learns of its
// own death from a separate Wait, telnet from a socket error, serial from a
// read error, and each has a different idea of what "connected" means. Session
// is where those become one vocabulary -- the six states in transport.go --
// without any of them pretending to a state it cannot reach.
//
// One Session shape means the shim above holds a single type. omegassh_read,
// omegassh_write, omegassh_resize, omegassh_alive and omegassh_close do not
// branch on which transport is underneath, and neither will the terminal tab.
package transport

import (
	"errors"
	"io"
	"sync"

	"github.com/scottpeterman/omegassh/sshcore"
)

// ExitUnknown is re-exported so a caller holding a Session need not import
// sshcore to interpret WaitExit. Telnet and serial always report it: neither
// protocol carries an exit status, and inventing a zero would read as a clean
// shell exit that never happened.
const ExitUnknown = sshcore.ExitUnknown

// Session is one open connection to a device, whatever it is running over.
//
// Output is deliberately an io.Reader rather than a push: bytes reach a GUI
// through OutputBuffer, on the consumer's own thread. See sshcore/buffer.go.
type Session interface {
	// Kind reports which transport is underneath.
	Kind() Kind

	// Summary is a short human-readable target, e.g. "10.0.0.1:23" or
	// "9600 8N1". For logs, tab titles and about boxes.
	Summary() string

	// Output is the device's byte stream, read to EOF exactly once by
	// whoever owns the read loop. It carries the logging tap, so bytes are
	// recorded whether or not anything above bothers to.
	Output() io.Reader

	// Write sends operator input. The count returned is the caller's logical
	// byte count, not the on-wire count -- telnet doubles IAC and may expand
	// CR, and a caller comparing n against len(p) should not see that.
	Write(p []byte) (int, error)

	// Resize reports a new window geometry. A no-op on serial.
	Resize(Size) error

	// State is the current lifecycle position.
	State() State

	// Done is closed once the session has ended.
	Done() <-chan struct{}

	// Err reports why it ended; nil for a local Close.
	Err() error

	// WaitExit blocks until the session's status is known and returns it,
	// normalized the way anytermqt reports a local child. Call it only after
	// Output has been read to EOF: on SSH, waiting discards anything still
	// in the pipe, which costs the last screenful the operator ran.
	WaitExit() int

	// SetEventHandler installs a callback for state changes and errors. It
	// fires on whichever goroutine caused the change, so a GUI consumer must
	// marshal -- which is why the cgo shim queues events instead. Passing nil
	// removes the handler. Safe to call at any time.
	SetEventHandler(func(Event))

	// Close ends the session and unblocks a blocked read.
	Close() error
}

// base is the state, event and logging machinery every Session shares. The
// per-transport types embed it and supply only what differs.
type base struct {
	kind    Kind
	summary string
	log     *Logger

	mu      sync.Mutex
	state   State
	handler func(Event)
	closing bool // Close was called; a read error after it is not a failure
	ended   bool
	err     error

	done     chan struct{}
	doneOnce sync.Once
}

func newBase(kind Kind, summary string, log *Logger, initial State) *base {
	return &base{
		kind:    kind,
		summary: summary,
		log:     log,
		state:   initial,
		done:    make(chan struct{}),
	}
}

func (b *base) Kind() Kind      { return b.kind }
func (b *base) Summary() string { return b.summary }

func (b *base) State() State {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.state
}

func (b *base) Done() <-chan struct{} { return b.done }

func (b *base) Err() error {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.err
}

func (b *base) SetEventHandler(h func(Event)) {
	b.mu.Lock()
	b.handler = h
	b.mu.Unlock()
}

// setState records a new state and publishes it. A repeat of the current
// state is dropped rather than emitted, so a caller counting transitions is
// counting transitions.
func (b *base) setState(s State) {
	b.mu.Lock()
	if b.state == s {
		b.mu.Unlock()
		return
	}
	b.state = s
	h := b.handler
	b.mu.Unlock()

	if h != nil {
		h(Event{Kind: EventStateChanged, State: s})
	}
}

// emitError publishes a failure without changing state. Used for errors that
// do not end the session -- a write that failed while the read loop is still
// alive, say.
func (b *base) emitError(err error) {
	if err == nil {
		return
	}
	b.mu.Lock()
	h := b.handler
	b.mu.Unlock()
	if h != nil {
		h(Event{Kind: EventError, Error: err.Error()})
	}
}

// markClosing records that the end about to arrive was asked for locally, so
// finish does not report it as a failure.
func (b *base) markClosing() {
	b.mu.Lock()
	b.closing = true
	b.mu.Unlock()
}

// finish settles the terminal state exactly once.
//
// An error that arrives after Close was called is the teardown provoking its
// own read failure, not a fault: it is dropped, and the session reports
// disconnected with a nil Err. That matches what telnetx and serialx already
// do internally and what the SSH side has always reported, so an operator
// disconnecting never lights up as a failure anywhere in the stack.
func (b *base) finish(err error) {
	b.doneOnce.Do(func() {
		b.mu.Lock()
		if b.closing || errors.Is(err, io.EOF) {
			err = nil
		}
		b.err = err
		b.ended = true
		h := b.handler
		b.mu.Unlock()

		if err != nil && h != nil {
			h(Event{Kind: EventError, Error: err.Error()})
		}
		if err != nil {
			b.setState(StateFailed)
		} else {
			b.setState(StateDisconnected)
		}
		if b.log != nil {
			b.log.Close()
		}
		close(b.done)
	})
}

// tap wraps r with the log tap and the end-of-session hook, so both live in
// one place for every transport rather than once per backend.
func (b *base) tap(r io.Reader) io.Reader {
	return &readTap{b: b, r: r}
}

type readTap struct {
	b *base
	r io.Reader
}

func (t *readTap) Read(p []byte) (int, error) {
	n, err := t.r.Read(p)
	if n > 0 && t.b.log != nil {
		t.b.log.Output(p[:n])
	}
	if err != nil {
		t.b.finish(err)
	}
	return n, err
}

// ---------------------------------------------------------------------------
// SSH
// ---------------------------------------------------------------------------

// shell is the part of *sshcore.Session this package needs. Declared as an
// interface rather than taking the concrete type so the adapter is testable
// without a server, and so a caller can substitute one.
type shell interface {
	Stdout() io.Reader
	Write(p []byte) (int, error)
	Resize(cols, rows int) error
	Wait() error
	Close() error
}

type sshSession struct {
	*base
	sh shell
}

// WrapSSH adapts an already-dialed sshcore session to the Session contract.
//
// It takes a live session rather than dialing, because sshcore.OpenShell
// already does the dial and doing it here would mean two ways to open an SSH
// session. The state starts at connected for the same reason: by the time
// there is something to wrap, the connect and the authentication are done.
//
// Connecting and authenticating are therefore not this type's to publish, and
// nothing here pretends otherwise. They belong to whoever is running the dial
// and can see where it has got to -- sshcore reports the stages, and the cgo
// shim publishes them against a handle that exists before its session does.
// A Go caller wanting the same can pass Config.Progress and get it.
//
// log may be nil.
func WrapSSH(sh shell, summary string, log *Logger) Session {
	s := &sshSession{
		base: newBase(KindSSH, summary, log, StateConnected),
		sh:   sh,
	}
	return s
}

func (s *sshSession) Output() io.Reader { return s.tap(s.sh.Stdout()) }

func (s *sshSession) Write(p []byte) (int, error) {
	n, err := s.sh.Write(p)
	if err != nil {
		s.emitError(err)
		return n, err
	}
	if s.log != nil {
		s.log.Input(p[:n])
	}
	return n, nil
}

func (s *sshSession) Resize(sz Size) error {
	if !sz.Valid() {
		return nil
	}
	return s.sh.Resize(sz.Cols, sz.Rows)
}

// WaitExit reaps the remote shell's status. Unlike telnet and serial there is
// a real one to collect, and it arrives on the channel after stdout has hit
// EOF -- which is why this is a separate call rather than something finish
// could have done.
func (s *sshSession) WaitExit() int {
	return sshcore.NormalizeExit(s.sh.Wait())
}

func (s *sshSession) Close() error {
	s.markClosing()
	err := s.sh.Close()
	s.finish(nil)
	return err
}

// ---------------------------------------------------------------------------
// Telnet and serial
// ---------------------------------------------------------------------------

type backendSession struct {
	*base
	t Transport
}

// Wrap adapts a connected Transport -- a telnetx or serialx Backend -- to the
// Session contract. The two are identical at this layer; everything that
// differs between them is inside the backend, which is the point of them both
// being io.Reader and io.Writer.
//
// The caller connects first. Wrap does not dial, for the same reason WrapSSH
// does not: an error during connect is a config or network error the caller
// reports, not a session that exists in a failed state.
//
// log may be nil.
func Wrap(t Transport, kind Kind, summary string, log *Logger) Session {
	s := &backendSession{
		base: newBase(kind, summary, log, StateConnected),
		t:    t,
	}
	// The backend has its own Done, closed on unplug or a dropped socket
	// even if nothing is reading. Bridge it so a session whose read loop has
	// not been started still reports the end.
	go func() {
		<-t.Done()
		s.finish(t.Err())
	}()
	return s
}

func (s *backendSession) Output() io.Reader { return s.tap(s.t) }

func (s *backendSession) Write(p []byte) (int, error) {
	n, err := s.t.Write(p)
	if err != nil {
		s.emitError(err)
		return n, err
	}
	if s.log != nil {
		s.log.Input(p[:n])
	}
	return n, nil
}

func (s *backendSession) Resize(sz Size) error { return s.t.Resize(sz) }

// WaitExit blocks until the session ends and reports ExitUnknown. Neither
// protocol carries an exit status; reporting 0 would be indistinguishable
// from a shell that exited cleanly.
func (s *backendSession) WaitExit() int {
	<-s.done
	return ExitUnknown
}

func (s *backendSession) Close() error {
	s.markClosing()
	err := s.t.Close()
	s.finish(nil)
	return err
}
