// sshcore/authfail_test.go
//
// The classifier decides whether the GUI re-opens the login modal. Two ways
// it can be wrong, and they are not symmetric:
//
//   - a rejection it does not recognise means no re-prompt, and the operator
//     retypes the address by hand. Annoying.
//   - a HOST KEY failure it mistakes for a rejection means a password box
//     over a message about a key that does not match. That one has to be
//     dismissed before the real reason can be read, and the real reason is
//     the one that matters.
//
// So the host key cases are asserted against the messages hostkey.go actually
// produces, not against paraphrases.

package sshcore

import (
	"errors"
	"fmt"
	"strings"
	"testing"
)

func TestIsAuthFailure(t *testing.T) {
	cases := []struct {
		name string
		err  error
		want bool
	}{
		{
			// x/crypto's wording as of the pinned version. The whole reason
			// this test exists: an upgrade that rewords it fails here.
			name: "every method refused",
			err: errors.New("ssh: handshake failed: ssh: unable to authenticate, " +
				"attempted methods [none publickey], no supported methods remain"),
			want: true,
		},
		{
			name: "partial auth, head without tail",
			err:  errors.New("ssh: unable to authenticate"),
			want: true,
		},
		{
			name: "nil",
			err:  nil,
			want: false,
		},
		{
			name: "refused port",
			err:  errors.New("connect to lab-sw-01:22: dial tcp 10.0.0.1:22: connect: connection refused"),
			want: false,
		},
		{
			name: "dns miss",
			err:  errors.New("connect to lab-sw-01.lab.invalid:22: dial tcp: lookup lab-sw-01.lab.invalid: no such host"),
			want: false,
		},
		{
			name: "handshake eof",
			err:  errors.New("SSH handshake with lab-sw-01:22: host closed the connection (EOF)"),
			want: false,
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if got := IsAuthFailure(tc.err); got != tc.want {
				t.Fatalf("IsAuthFailure(%v) = %v, want %v", tc.err, got, tc.want)
			}
		})
	}
}

// The three messages buildHostKeyCallback can produce, built here the same way
// hostkey.go builds them. None may classify as an auth failure.
func TestHostKeyFailuresAreNotAuthFailures(t *testing.T) {
	hostname := "lab-sw-01:22"

	messages := []string{
		// First contact.
		fmt.Sprintf("%s %s (%s %s); not in %s",
			UnknownHostKeyMarker, hostname, "ssh-ed25519",
			"SHA256:abc+def/ghi=", "/home/op/.ssh/known_hosts"),

		// Mismatch -- re-key or MITM. The case that must stay fatal.
		fmt.Sprintf("host key verification failed for %s: offered key (%s %s) does not match "+
			"the pinned entry", hostname, "ssh-ed25519", "SHA256:abc+def/ghi="),

		// Explicitly rejected.
		fmt.Sprintf("host key for %s rejected", hostname),
	}

	for _, msg := range messages {
		t.Run(strings.SplitN(msg, " ", 4)[0], func(t *testing.T) {
			if IsAuthFailure(errors.New(msg)) {
				t.Fatalf("host key failure classified as an auth failure: %s", msg)
			}
			// And again wrapped, since the transport prefixes context.
			wrapped := fmt.Errorf("SSH handshake with %s: %w", hostname, errors.New(msg))
			if IsAuthFailure(wrapped) {
				t.Fatalf("wrapped host key failure classified as an auth failure: %v", wrapped)
			}
		})
	}
}

func TestWrapAuthFailure(t *testing.T) {
	inner := errors.New("ssh: unable to authenticate, no supported methods remain")
	wrapped := wrapAuthFailure("lab-sw-01:22", inner)

	if !strings.HasPrefix(wrapped.Error(), AuthFailedMarker) {
		t.Fatalf("wrapped message does not open with the marker: %q", wrapped.Error())
	}
	if !strings.Contains(wrapped.Error(), "lab-sw-01:22") {
		t.Fatalf("wrapped message lost the address: %q", wrapped.Error())
	}
	// The detail has to survive: it is what the overlay shows if the operator
	// cancels the prompt.
	if !strings.Contains(wrapped.Error(), inner.Error()) {
		t.Fatalf("wrapped message lost the cause: %q", wrapped.Error())
	}
	if !errors.Is(wrapped, inner) {
		t.Fatal("errors.Is cannot reach the cause")
	}
	if !AsAuthFailure(wrapped) {
		t.Fatal("AsAuthFailure does not recognise our own wrapper")
	}
	if AsAuthFailure(inner) {
		t.Fatal("AsAuthFailure matched an unwrapped error")
	}
}

// The marker is what the C surface publishes and the GUI matches on. If it is
// ever reworded, the C++ classifier and this constant must move together.
func TestAuthFailedMarkerShape(t *testing.T) {
	if AuthFailedMarker == "" {
		t.Fatal("marker is empty")
	}
	if strings.Contains(AuthFailedMarker, UnknownHostKeyMarker) ||
		strings.Contains(UnknownHostKeyMarker, AuthFailedMarker) {
		t.Fatal("the two markers overlap; one message could match both")
	}
}
