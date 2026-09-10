// capi/capi.go
// C-archive shim: handle bookkeeping and the data path.
//
// This layer is marshalling and bookkeeping only. Anything that could have a
// protocol bug in it belongs in sshcore, telnetx, serialx or transport, where
// it is ordinary Go with ordinary tests and where a Go caller gets it for
// free by importing the package directly.
//
// The C surface is split by concern rather than kept in the one file it
// started as:
//
//	capi.go       handles, read/write/resize/alive/close  (this file)
//	open.go       omegassh_open, the config it takes, and the checks that
//	              are decided before anything is dialed
//	dial.go       the dial itself, on its own goroutine
//	events.go     state and the event queue
//	serial.go     serial port enumeration
//	vault.go      the credential surface
//	lasterror.go  per-thread error reporting
//
// Since Phase 2b a handle exists BEFORE its session does. omegassh_open
// validates, files a handle and returns; the dial runs on a goroutine and
// publishes its progress as state changes. So everything below has a case for
// a handle whose session is still nil, and that case is "connecting", not
// "broken" -- see dial.go for why the split is where it is.
//
// Delivery model: there is no Go->C callback. A callback would fire on a
// Go-created OS thread, which in Qt means every byte has to be marshalled
// with QMetaObject::invokeMethod, and forgetting once is a rare crash rather
// than a compile error. Instead each session owns a notifier whose readable
// end the caller watches with QSocketNotifier. Everything lands on the
// caller's thread. See ../include/omegassh/omegassh.h for the contract.
//
// Since Phase 2 the handle holds a transport.Session rather than an SSH
// session, so nothing below branches on whether it is driving SSH, telnet or
// a serial console. The transport is chosen once, in omegassh_open.
//
// Build:
//
//	go build -buildmode=c-archive -o libomegassh.a ./capi
package main

/*
#include <stdlib.h>
*/
import "C"

import (
	"sync"
	"unsafe"

	"github.com/scottpeterman/omegassh/sshcore"
	"github.com/scottpeterman/omegassh/transport"
)

// version is reported by omegassh_version. Bump with the module tag.
const version = "0.2.0"

type handleState struct {
	// kind and summary are known from the configuration alone, so they are
	// answerable the moment the handle exists. A tab gets its title while
	// the dial is still in flight rather than after it lands.
	kind    transport.Kind
	summary string

	buf    *sshcore.OutputBuffer
	notify *notifier

	mu sync.Mutex

	// sess is nil until the dial succeeds, and stays nil forever if it
	// fails. Nothing below dereferences it without checking.
	sess transport.Session

	// state is the handle's own view, not the session's. It has to be: for
	// the first part of a handle's life there is no session to ask, and for
	// a failed dial there never is one. Every change to it is queued as an
	// event in the same critical section, so omegassh_state and
	// omegassh_next_event cannot disagree about what happened.
	state transport.State

	// ended records that a terminal state has been published, by the
	// session or by a failed dial. It is what stops a late connected from
	// overtaking a disconnected that already went out -- the dial goroutine
	// and the session's own teardown can race, and only one order is true.
	ended bool

	// dialFailed distinguishes "no session yet" from "no session ever",
	// which is the whole difference between alive and not.
	dialFailed bool

	// err is the last failure this SESSION reported, as opposed to the last
	// failure on this THREAD. A dial that fails does so on a goroutine the
	// caller has never seen, so omegassh_last_error cannot carry it; this
	// is what omegassh_error reads.
	err string

	// resize records a geometry that arrived while the dial was in flight,
	// applied once there is something to apply it to. A terminal widget
	// settles its layout during the connect, and the alternative is either
	// a failed resize the caller has to ignore or a first screenful drawn
	// at the wrong width.
	resize transport.Size

	closed   bool
	reaped   bool
	exitCode int

	// events is a bounded queue drained by omegassh_next_event. It exists
	// because the session publishes state changes on whatever goroutine
	// caused them, and those have to reach the caller's thread the same way
	// bytes do -- through the notifier, not through a callback.
	events []transport.Event

	// notifyMu guards a wake against the notifier being closed underneath
	// it. Same discipline as OutputBuffer.wakeMu and for the same reason:
	// checking closed under mu, releasing it, then waking leaves a window in
	// which close runs in between and the wake lands on a torn-down
	// descriptor. Held across the wake, which is safe because the write end
	// is non-blocking.
	notifyMu   sync.Mutex
	notifyGone bool
}

// poke wakes the caller's event loop, unless the notifier has been closed.
func (st *handleState) poke() {
	st.notifyMu.Lock()
	defer st.notifyMu.Unlock()
	if st.notifyGone || st.notify == nil {
		return
	}
	st.notify.wake()
}

// pump drains the session, then reaps its exit status.
//
// Reaping runs only after the buffer has seen EOF, never alongside it:
// x/crypto closes the SSH session inside Wait, and anything still in the
// stdout pipe is discarded. Reaping afterwards costs one extra wake and keeps
// the last screenful of output. Telnet and serial have no status to collect,
// so WaitExit returns immediately with ExitUnknown -- the shape is the same
// and the shim does not have to know which it holds.
func (st *handleState) pump() {
	st.mu.Lock()
	sess := st.sess
	st.mu.Unlock()
	if sess == nil {
		return
	}

	st.buf.Pump(sess.Output())

	st.mu.Lock()
	closed := st.closed
	st.mu.Unlock()
	if closed {
		return
	}

	code := sess.WaitExit()
	st.mu.Lock()
	st.exitCode = code
	st.reaped = true
	st.mu.Unlock()

	// Wake once more. Until this point alive still reports 1, so a caller
	// that drained to EOF sees the session as running and comes back for the
	// status rather than reporting -1 for every clean exit.
	st.poke()
}

var (
	reg    = map[int64]*handleState{}
	regMu  sync.Mutex
	nextID int64 = 1
)

// setErr, clearErr and omegassh_last_error live in lasterror.go. They were a
// single global string here, which two Qt worker threads overwrite for each
// other; see that file for why the message is now per-thread.

func lookup(h C.longlong) *handleState {
	regMu.Lock()
	defer regMu.Unlock()
	return reg[int64(h)]
}

// newHandle files a handle for a session that does not exist yet.
//
// The buffer and the notifier are built here rather than after the dial for
// one reason: the caller is handed this handle immediately and starts watching
// it immediately. omegassh_read, omegassh_pending and omegassh_notify_handle
// have to be answerable during the connect -- answering "no such handle" for
// a handle that was just returned is the kind of contract that turns into an
// intermittent bug in the widget above.
//
// initial is the state a handle of this kind starts in: connecting for SSH and
// telnet, disconnected for serial, which reaches connected without passing
// through anything. It is queued as an event as well as recorded, so a caller
// that only drains events sees the whole sequence and not just the tail of it.
func newHandle(kind transport.Kind, summary string, initial transport.State) (C.longlong, *handleState, error) {
	n, err := newNotifier()
	if err != nil {
		return -1, nil, err
	}

	st := &handleState{
		kind:     kind,
		summary:  summary,
		notify:   n,
		state:    initial,
		exitCode: transport.ExitUnknown,
	}
	st.buf = sshcore.NewOutputBuffer(n.wake)
	if initial != transport.StateDisconnected {
		st.events = append(st.events,
			transport.Event{Kind: transport.EventStateChanged, State: initial})
	}

	regMu.Lock()
	id := nextID
	nextID++
	reg[id] = st
	regMu.Unlock()
	return C.longlong(id), st, nil
}

//export omegassh_version
func omegassh_version() *C.char {
	return C.CString(version)
}

// omegassh_unknown_hostkey_marker publishes the exact bytes that open a
// first-contact host key failure, so a UI can recognise one without carrying
// its own copy of the wording.
//
// This exists because a prompt callback cannot cross this boundary (see
// buildConfig in open.go). The UI's route to a fingerprint dialog is to let
// the dial fail, classify the message, ask, and re-dial. Publishing the
// marker is what stops that classification from being a guess that a
// reworded message silently breaks.
//
//export omegassh_unknown_hostkey_marker
func omegassh_unknown_hostkey_marker() *C.char {
	return C.CString(sshcore.UnknownHostKeyMarker)
}

// omegassh_auth_failed_marker publishes the exact bytes that open a credential
// rejection, so a UI can recognise one and re-ask without carrying its own
// copy of the wording.
//
// Same reasoning as the host key marker above, and the same flow: let the dial
// fail, classify the message, ask, re-dial. The difference is what the answer
// changes -- a host key answer changes the policy, this one changes the
// credentials -- and that a rejection can be answered more than once, because
// a mistyped password is worth a second go.
//
// The underlying message is x/crypto's; sshcore re-wraps it with this marker
// so the matching happens in one place next to the go.mod that pins it. See
// sshcore/authfail.go.
//
//export omegassh_auth_failed_marker
func omegassh_auth_failed_marker() *C.char {
	return C.CString(sshcore.AuthFailedMarker)
}

// omegassh_config_dir_name publishes the directory under $HOME that this
// library writes session logs into, e.g. ".omega".
//
// It exists to be CHECKED, not to be used. The C++ application owns the
// canonical definition of that directory; this package needs its own copy
// because it sits below this boundary and cannot call up through it. Exporting
// the copy is what lets a probe above the boundary assert the two agree, so
// moving the directory on one side and not the other is a test failure rather
// than logs quietly landing in a folder nothing else uses.
//
// omegassh_keyboard_prompt_marker publishes the bytes that open a
// keyboard-interactive question nobody could answer, so a UI can recognise
// one, ask its user, and dial again with the answer in "keyboard_answers".
//
// Third of the same kind, after the host key and auth-failure markers, and
// for the same reason: a prompt callback cannot cross this boundary without
// running on a foreign thread. See sshcore/kbdprompt.go for the message shape
// the UI parses, and for the one case this cannot serve -- a challenge whose
// answer is bound to the connection that asked.
//
//export omegassh_keyboard_prompt_marker
func omegassh_keyboard_prompt_marker() *C.char {
	return C.CString(sshcore.KeyboardPromptMarker)
}

//export omegassh_config_dir_name
func omegassh_config_dir_name() *C.char {
	return C.CString(transport.ConfigDirName)
}

//export omegassh_free
func omegassh_free(s *C.char) {
	if s != nil {
		C.free(unsafe.Pointer(s))
	}
}

//export omegassh_notify_handle
func omegassh_notify_handle(h C.longlong) C.longlong {
	st := lookup(h)
	if st == nil {
		return -1
	}
	return C.longlong(st.notify.handle())
}

// Reports which transport the handle is running over: "ssh", "telnet" or
// "serial". Answerable while the dial is still in flight -- the transport is
// chosen in the configuration, not discovered on the wire. The caller owns the
// string and frees it with omegassh_free.
//
//export omegassh_transport
func omegassh_transport(h C.longlong) *C.char {
	st := lookup(h)
	if st == nil {
		return nil
	}
	return C.CString(st.kind.String())
}

// Reports the session's target in short human-readable form -- "10.0.0.1:23",
// "9600 8N1" -- for tab titles and logs. Answerable during the connect, for
// the same reason and so that a tab has its title before it has its session.
// The caller frees it.
//
//export omegassh_summary
func omegassh_summary(h C.longlong) *C.char {
	st := lookup(h)
	if st == nil {
		return nil
	}
	return C.CString(st.summary)
}

//export omegassh_read
func omegassh_read(h C.longlong, dst *C.char, max C.int) C.int {
	st := lookup(h)
	if st == nil {
		return -1
	}
	if dst == nil || max <= 0 {
		return 0
	}
	out := unsafe.Slice((*byte)(unsafe.Pointer(dst)), int(max))
	return C.int(st.buf.Drain(out))
}

//export omegassh_write
func omegassh_write(h C.longlong, src *C.char, n C.int) C.int {
	st := lookup(h)
	if st == nil {
		return -1
	}
	if src == nil || n <= 0 {
		return 0
	}
	sess := st.session()
	if sess == nil {
		// Refused rather than buffered. Keystrokes typed at a connection
		// overlay are not input to a device that has not answered yet, and
		// replaying them into the first prompt is how a password ends up
		// echoed into a username field.
		setErr("write: session is still connecting")
		return -1
	}
	written, err := sess.Write(C.GoBytes(unsafe.Pointer(src), n))
	if err != nil {
		setErr("write: %v", err)
		return -1
	}
	return C.int(written)
}

// Reports a new window geometry to the far end. A serial console has no
// window-change concept, so this succeeds and does nothing there rather than
// making the caller branch on which transport it holds.
//
// A resize during the connect is recorded and applied when the session
// arrives. A widget settles its layout while the dial is still running, and
// the alternatives are both worse than remembering it: fail, and every caller
// has to ignore an error it cannot act on; drop it, and the first screenful
// is drawn at whatever width the config happened to name.
//
//export omegassh_resize
func omegassh_resize(h C.longlong, cols, rows C.int) C.int {
	st := lookup(h)
	if st == nil {
		return -1
	}
	size := transport.Size{Cols: int(cols), Rows: int(rows)}

	st.mu.Lock()
	sess := st.sess
	if sess == nil {
		st.resize = size
		st.mu.Unlock()
		return 0
	}
	st.mu.Unlock()

	if err := sess.Resize(size); err != nil {
		setErr("%v", err)
		return -1
	}
	return 0
}

//export omegassh_alive
func omegassh_alive(h C.longlong) C.int {
	st := lookup(h)
	if st == nil {
		return 0
	}
	st.mu.Lock()
	closed, failed, sess, reaped := st.closed, st.dialFailed, st.sess, st.reaped
	st.mu.Unlock()
	if closed || failed {
		return 0
	}
	// No session yet means the dial is still running, which is a session
	// that has not ended. Reporting 0 here would have every caller tear the
	// handle down a microsecond after opening it.
	if sess == nil {
		return 1
	}
	// EOF on the byte stream is not the end of an SSH session: the exit
	// status arrives on the channel afterwards. Reporting 0 here would race
	// every caller that reads the status on seeing the session end.
	if st.buf.Done() && reaped {
		return 0
	}
	return 1
}

//export omegassh_pending
func omegassh_pending(h C.longlong) C.int {
	st := lookup(h)
	if st == nil {
		return -1
	}
	return C.int(st.buf.Pending())
}

// Returns the remote shell's exit status once it has ended: the program's own
// status, or 128 + signal number where a signal ended it. -1 while the shell
// is still running, and for a session that ended without reporting a status.
//
// Telnet and serial always report -1. Neither protocol carries an exit
// status, and reporting 0 would be indistinguishable from a clean shell exit.
//
//export omegassh_exit_code
func omegassh_exit_code(h C.longlong) C.int {
	st := lookup(h)
	if st == nil {
		return C.int(transport.ExitUnknown)
	}
	st.mu.Lock()
	defer st.mu.Unlock()
	return C.int(st.exitCode)
}

//export omegassh_close
func omegassh_close(h C.longlong) {
	regMu.Lock()
	st := reg[int64(h)]
	delete(reg, int64(h))
	regMu.Unlock()
	if st == nil {
		return
	}
	st.mu.Lock()
	if st.closed {
		st.mu.Unlock()
		return
	}
	st.closed = true
	sess := st.sess
	st.mu.Unlock()

	// A dial still in flight has nothing here to stop. It finds st.closed
	// when it lands and closes whatever it built instead of filing it, so
	// this returns at once rather than waiting on a connect timeout -- an
	// operator closing a tab against dead gear must not wait for it. The
	// cost is that the socket and the log file live on inside the dial
	// goroutine until the timeout expires; see dial.go.
	if sess != nil {
		// Stop the session publishing first: a handler firing during
		// teardown would enqueue an event and poke a notifier that is about
		// to go away.
		sess.SetEventHandler(nil)
	}

	// Buffer next: it drops the wake hook, so the pump goroutine cannot poke
	// a notifier that is about to be closed underneath it.
	st.buf.Close()
	if sess != nil {
		sess.Close()
	}

	st.notifyMu.Lock()
	st.notifyGone = true
	st.notify.close()
	st.notifyMu.Unlock()
}

// session returns the handle's session, or nil while the dial is in flight.
func (st *handleState) session() transport.Session {
	st.mu.Lock()
	defer st.mu.Unlock()
	return st.sess
}

func main() {}
