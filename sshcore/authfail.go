// sshcore/authfail.go
// Recognising a credential rejection, and saying so in our own words.
//
// SAME SHAPE AS hostkey.go, and for the same reason. A UI above the C
// boundary cannot be handed a callback (see the note in capi/open.go), so the
// way it recognises "the password was wrong, ask again" is by matching a
// marker in the error text. That makes the wording load-bearing, so it is a
// constant here rather than a literal at the far end.
//
// WHY WE RE-WRAP RATHER THAN MATCH x/crypto DIRECTLY AT THE FAR END. The
// underlying message belongs to golang.org/x/crypto/ssh:
//
//	ssh: handshake failed: ssh: unable to authenticate, attempted methods
//	[none publickey], no supported methods remain
//
// That is a third-party string, and pinning a C++ classifier to it would put
// a dependency on someone else's phrasing three layers away from the module
// that upgrades it. Matching it in ONE place -- here, next to the go.mod that
// pins the version -- means an x/crypto rewording is a test failure in this
// package rather than a prompt that silently stops appearing in the GUI.
//
// WHAT MUST NOT MATCH. A host key failure, a refused connection, a DNS miss
// and a timeout are all failures that re-prompting cannot fix; offering the
// login modal for any of them would be a dialog the operator has to dismiss
// before they can read the real reason. Host key failures are the sharp case,
// because they also happen during the handshake -- they are asserted against
// in authfail_test.go.

package sshcore

import (
	"errors"
	"strings"
)

// AuthFailedMarker opens the message for a credential rejection, and only
// that. It is matched by the UI to decide whether to re-ask for credentials
// and re-dial, so renaming it is a compile error here rather than a silent
// behaviour change up there.
const AuthFailedMarker = "authentication failed for"

// authFailureNeedles are x/crypto's own wordings for "your credentials were
// refused". Both are present in a modern failure; either alone is enough.
//
// "no supported methods remain" is the tail of the message when every method
// in the chain was tried and rejected. "unable to authenticate" is the head.
// A partial-auth failure carries the head without the tail, which is why both
// are checked rather than only the longer one.
var authFailureNeedles = []string{
	"unable to authenticate",
	"no supported methods remain",
}

// hostKeyNeedles are the openings of every host key failure buildHostKeyCallback
// can produce: first contact, mismatch, and an explicitly rejected key.
//
// Only the first is a published constant -- the other two are literals in
// hostkey.go, so these are copies and are asserted against the real messages
// in authfail_test.go. That test is what stops a reworded mismatch from
// quietly becoming something this file classifies as a password problem.
var hostKeyNeedles = []string{
	UnknownHostKeyMarker,
	// Not a host key, but the same rule applies and for the same reason: an
	// unanswered keyboard-interactive question has its own prompt, and
	// offering a username/password box instead would be a dialog that cannot
	// answer what was asked.
	KeyboardPromptMarker,
	"host key verification failed for",
	"host key for",
}

// IsAuthFailure reports whether err is the far end refusing the credentials
// offered, as opposed to any other handshake failure.
//
// A host key failure is explicitly not one. It surfaces from the same
// ssh.NewClientConn call and has its own prompt, and conflating the two would
// mean answering "the key is unknown" with a password box.
func IsAuthFailure(err error) bool {
	if err == nil {
		return false
	}
	text := err.Error()

	// Host key failures win. Checked first so that a message which somehow
	// carried both is classified as the one that must stay fatal.
	for _, needle := range hostKeyNeedles {
		if strings.Contains(text, needle) {
			return false
		}
	}

	for _, needle := range authFailureNeedles {
		if strings.Contains(text, needle) {
			return true
		}
	}
	return false
}

// wrapAuthFailure re-labels a credential rejection with our own marker,
// keeping the original error wrapped so errors.Is and errors.As still reach
// it and so the detail stays readable in the message.
func wrapAuthFailure(addr string, err error) error {
	return &authError{addr: addr, err: err}
}

type authError struct {
	addr string
	err  error
}

func (e *authError) Error() string {
	return AuthFailedMarker + " " + e.addr + ": " + e.err.Error()
}

func (e *authError) Unwrap() error { return e.err }

// AsAuthFailure reports whether err is, or wraps, a rejection this package
// labelled. Distinct from IsAuthFailure, which classifies a raw x/crypto
// error before it has been wrapped.
func AsAuthFailure(err error) bool {
	var target *authError
	return errors.As(err, &target)
}
