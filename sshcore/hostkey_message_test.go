// sshcore/hostkey_message_test.go
//
// The two host key failures are told apart above the C boundary by matching
// UnknownHostKeyMarker in the error text, because a prompt callback cannot be
// marshalled across it. That makes the wording of these two messages part of
// the interface rather than an implementation detail, and this is what says
// so out loud.
//
// The messages are produced by driving the REAL callback, not by asserting
// against strings copied out of the source. A rewrite that changes what the
// UI sees fails here.

package sshcore

import (
	"crypto/ed25519"
	"crypto/rand"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"golang.org/x/crypto/ssh"
)

// testKey returns a usable ed25519 host key. Two calls give two different
// keys, which is what the mismatch case needs.
func testKey(t *testing.T) ssh.PublicKey {
	t.Helper()
	_, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatalf("generate key: %v", err)
	}
	signer, err := ssh.NewSignerFromKey(priv)
	if err != nil {
		t.Fatalf("signer: %v", err)
	}
	return signer.PublicKey()
}

func remoteAddr(t *testing.T) net.Addr {
	t.Helper()
	addr, err := net.ResolveTCPAddr("tcp", "192.0.2.10:22")
	if err != nil {
		t.Fatalf("resolve: %v", err)
	}
	return addr
}

// callbackFor builds the real strict callback over a known_hosts file whose
// contents the caller chooses.
func callbackFor(t *testing.T, knownHosts string) ssh.HostKeyCallback {
	t.Helper()
	path := filepath.Join(t.TempDir(), "known_hosts")
	if err := os.WriteFile(path, []byte(knownHosts), 0o600); err != nil {
		t.Fatalf("write known_hosts: %v", err)
	}
	cfg := &Config{HostKeys: HostKeyStrict, KnownHostsPath: path}
	cb, err := buildHostKeyCallback(cfg)
	if err != nil {
		t.Fatalf("build callback: %v", err)
	}
	return cb
}

// A host that is not in the file at all: the case a UI should offer to
// accept.
func TestUnknownHostMessageCarriesMarker(t *testing.T) {
	cb := callbackFor(t, "")
	err := cb("lab-sw-01:22", remoteAddr(t), testKey(t))
	if err == nil {
		t.Fatal("expected an unknown-host failure, got nil")
	}

	msg := err.Error()
	if !strings.Contains(msg, UnknownHostKeyMarker) {
		t.Errorf("unknown-host message does not carry the marker.\n"+
			"marker: %q\nmessage: %q\n"+
			"The UI matches on this to offer a fingerprint prompt; without it "+
			"the prompt silently stops appearing.", UnknownHostKeyMarker, msg)
	}

	// The prompt is worth nothing without something to show, so the parts the
	// dialog needs have to be in there too.
	if !strings.Contains(msg, "SHA256:") {
		t.Errorf("no fingerprint in %q", msg)
	}
	if !strings.Contains(msg, "ssh-ed25519") {
		t.Errorf("no key type in %q", msg)
	}
	if !strings.Contains(msg, "lab-sw-01") {
		t.Errorf("no hostname in %q", msg)
	}
}

// A host pinned to a DIFFERENT key: re-key or MITM. This one must fail closed,
// and must not be mistaken for first contact -- offering to trust the offered
// key here is the one outcome worse than refusing to connect.
func TestMismatchMessageDoesNotCarryMarker(t *testing.T) {
	pinned := testKey(t)
	line := "lab-sw-01 " + pinned.Type() + " " +
		strings.TrimSpace(base64Key(pinned)) + "\n"

	cb := callbackFor(t, line)
	err := cb("lab-sw-01:22", remoteAddr(t), testKey(t))
	if err == nil {
		t.Fatal("expected a mismatch failure, got nil")
	}

	msg := err.Error()
	if strings.Contains(msg, UnknownHostKeyMarker) {
		t.Errorf("mismatch message carries the first-contact marker.\n"+
			"marker: %q\nmessage: %q\n"+
			"A UI matching the marker would offer to trust a key that does "+
			"not match the pinned one.", UnknownHostKeyMarker, msg)
	}
	if !strings.Contains(msg, "does not match") {
		t.Errorf("mismatch message does not read as a mismatch: %q", msg)
	}
}

func base64Key(key ssh.PublicKey) string {
	return strings.TrimPrefix(
		strings.TrimSpace(string(ssh.MarshalAuthorizedKey(key))),
		key.Type()+" ")
}
