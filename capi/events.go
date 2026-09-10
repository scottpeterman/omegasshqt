// capi/events.go
// Session state and the event queue.
//
// The session publishes state changes on whatever goroutine caused them,
// which for a GUI is the wrong thread by construction. That is the same
// problem the byte path already solved, so events take the same route: they
// are queued here, the notifier is poked, and the caller drains them on its
// own thread alongside omegassh_read.
//
// Two calls rather than one. omegassh_state answers "what is it doing now",
// which is what the connection overlay needs and what a caller polls; it
// allocates nothing. omegassh_next_event replays the transitions in order,
// for a caller that wants every one of them -- a session log, or the
// interactive-auth prompt when that arrives. omegassh_error is the third and
// exists because of the second half of Phase 2b: since the dial runs on its
// own goroutine, a failure has no thread of the caller's to be reported on,
// and a state without a reason attached sends an operator to the wrong place.
//
// st.state, not sess.State(). The handle keeps its own view because for the
// first part of its life there is no session to ask and after a failed dial
// there never is one. Every change to it -- from the dial below, from the
// session above -- goes through one of publish, failDial or onEvent, each of
// which records and queues in the same critical section, so the two calls can
// never disagree about what happened.
package main

/*
#include <stdlib.h>
*/
import "C"

import (
	"encoding/json"

	"github.com/scottpeterman/omegassh/transport"
)

// maxQueuedEvents bounds the queue. A session produces a handful of state
// changes in its whole life, so the cap is only reached by a caller that
// never drains -- in which case the recent events are the useful ones and the
// old ones are the ones to drop.
const maxQueuedEvents = 64

// terminal reports whether a state is one a session does not come back from.
// Used to stop a late arrival overtaking an end that already happened: the
// dial goroutine and the session's own teardown run concurrently, and only
// one of the two orders they can land in is true.
func terminal(s transport.State) bool {
	return s == transport.StateDisconnected || s == transport.StateFailed
}

// queueLocked appends an event. The caller holds st.mu.
func (st *handleState) queueLocked(ev transport.Event) {
	if len(st.events) >= maxQueuedEvents {
		st.events = st.events[1:]
	}
	st.events = append(st.events, ev)
}

// publish records a state the DIAL reached and queues it. It is the path for
// everything that happens before there is a session to publish for itself:
// connecting, authenticating, and the connected that follows a successful
// dial.
//
// It never publishes a terminal state and never overrides one, which is why it
// is separate from onEvent rather than the same function. See dial.go.
func (st *handleState) publish(s transport.State) {
	st.mu.Lock()
	if st.closed || st.ended || st.state == s {
		st.mu.Unlock()
		return
	}
	st.state = s
	st.queueLocked(transport.Event{Kind: transport.EventStateChanged, State: s})
	st.mu.Unlock()

	st.poke()
}

// failDial reports a dial that never produced a session at all.
//
// The error goes out before the state, matching the order base.finish uses
// when a live session dies, so a caller wiring both to one overlay gets the
// reason and the verdict in the same order either way.
func (st *handleState) failDial(err error) {
	msg := err.Error()

	st.mu.Lock()
	if st.closed {
		st.mu.Unlock()
		return
	}
	st.err = msg
	st.dialFailed = true
	st.ended = true
	st.state = transport.StateFailed
	st.queueLocked(transport.Event{Kind: transport.EventError, Error: msg})
	st.queueLocked(transport.Event{
		Kind: transport.EventStateChanged, State: transport.StateFailed})
	st.mu.Unlock()

	st.poke()
}

// onEvent is the handler installed on every session. It runs on the
// session's goroutine and must not block.
//
// A state change is recorded as well as queued: st.state is the answer
// omegassh_state gives, and updating it here is what keeps that answer and the
// event queue telling one story rather than two.
func (st *handleState) onEvent(ev transport.Event) {
	st.mu.Lock()
	if st.closed {
		st.mu.Unlock()
		return
	}
	switch ev.Kind {
	case transport.EventStateChanged:
		if st.state == ev.State {
			st.mu.Unlock()
			return
		}
		st.state = ev.State
		if terminal(ev.State) {
			st.ended = true
		}
	case transport.EventError:
		st.err = ev.Error
	}
	st.queueLocked(ev)
	st.mu.Unlock()

	st.poke()
}

// Reports the session's current state as one of the transport.State values:
// 0 disconnected, 1 connecting, 2 authenticating, 3 connected,
// 4 reconnecting, 5 failed. -1 for an unknown handle.
//
// Not every transport reaches every state and none of them fake one. Telnet
// never reports authenticating -- the protocol has no authentication step,
// and a login prompt on it is ordinary session data. Serial reports neither
// connecting nor authenticating: opening the port is the whole handshake.
//
//export omegassh_state
func omegassh_state(h C.longlong) C.int {
	st := lookup(h)
	if st == nil {
		return -1
	}
	st.mu.Lock()
	defer st.mu.Unlock()
	return C.int(st.state)
}

// Reports the session's current state as a string -- "connected", "failed" --
// for an overlay that would rather render text than map an enum. NULL for an
// unknown handle. The caller frees it with omegassh_free.
//
//export omegassh_state_name
func omegassh_state_name(h C.longlong) *C.char {
	st := lookup(h)
	if st == nil {
		return nil
	}
	st.mu.Lock()
	s := st.state
	st.mu.Unlock()
	return C.CString(s.String())
}

// Reports why THIS SESSION last failed, or an empty string. The caller frees
// it with omegassh_free.
//
// Distinct from omegassh_last_error, which is per-thread and reports the last
// failing CALL. A dial runs on a goroutine the caller has never touched, so
// there is no thread of the caller's for its failure to be recorded on -- and
// a state a caller can poll but cannot ask the reason for is not much of an
// answer. The message is the one that also arrived as an error event, kept
// here so a caller need not have been draining at the moment it happened.
//
//export omegassh_error
func omegassh_error(h C.longlong) *C.char {
	st := lookup(h)
	if st == nil {
		return nil
	}
	st.mu.Lock()
	msg := st.err
	st.mu.Unlock()
	return C.CString(msg)
}

// Removes and returns the oldest queued event as a JSON object, or NULL when
// the queue is empty. The caller frees a non-NULL result with omegassh_free.
//
//	{"kind": 0, "state": 3}                     state change to connected
//	{"kind": 2, "error": "..."}                 an error the session reported
//	{"kind": 3, "ask": {"question": "...",      a prompt awaiting an answer
//	                    "echo": false}}
//
// Drain in a loop until it returns NULL, the same way omegassh_read is
// drained, since one wake can cover several events.
//
//export omegassh_next_event
func omegassh_next_event(h C.longlong) *C.char {
	st := lookup(h)
	if st == nil {
		return nil
	}
	st.mu.Lock()
	if len(st.events) == 0 {
		st.mu.Unlock()
		return nil
	}
	ev := st.events[0]
	st.events = st.events[1:]
	st.mu.Unlock()

	blob, err := json.Marshal(ev)
	if err != nil {
		// Marshalling a struct of ints and strings does not fail, but
		// returning NULL here would read as "queue empty" and silently lose
		// the rest of the queue behind it.
		setErr("encode event: %v", err)
		return C.CString(`{"kind":2,"error":"event could not be encoded"}`)
	}
	return C.CString(string(blob))
}
