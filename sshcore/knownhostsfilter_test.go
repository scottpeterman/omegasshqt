// sshcore/knownhostsfilter_test.go
package sshcore

import (
	"github.com/scottpeterman/omegassh/internal/privatefile"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// A real ed25519 host key line, used as the "valid entry" fixture.
const validKey = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIF7pQ0Xs1n6oQjK9wKlkS9nJvT5vN2xO1qYm3rZbT4hP"

func writeKnownHosts(t *testing.T, lines ...string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "known_hosts")
	if err := os.WriteFile(path, []byte(strings.Join(lines, "\n")+"\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	return path
}

func TestUsableKnownHostsCleanFileIsUntouched(t *testing.T) {
	path := writeKnownHosts(t,
		"# lab fabric",
		"eng-leaf-1.lab.local "+validKey,
		"",
		"[usa-spine-1.lab.local]:2222 "+validKey,
	)

	usable, tmp, skipped, err := usableKnownHosts(path)
	if err != nil {
		t.Fatal(err)
	}
	if len(skipped) != 0 {
		t.Fatalf("clean file reported %d skipped lines: %v", len(skipped), skipped)
	}
	if usable != path {
		t.Fatalf("clean file should be used in place, got %q", usable)
	}
	if tmp != "" {
		t.Fatalf("clean file should not stage a temp copy, got %q", tmp)
	}
}

// The whole point: one bad line must not take the file down with it.
func TestUsableKnownHostsSkipsMalformedLines(t *testing.T) {
	path := writeKnownHosts(t,
		"eng-leaf-1.lab.local "+validKey,
		"this-is-not-a-valid-known-hosts-line",
		"usa-leaf-2.lab.local "+validKey,
		"wan-core-1.lab.local ssh-ed25519 not-valid-base64!!",
	)

	usable, tmp, skipped, err := usableKnownHosts(path)
	if err != nil {
		t.Fatal(err)
	}
	if len(skipped) != 2 {
		t.Fatalf("skipped %d lines, want 2: %v", len(skipped), skipped)
	}
	if skipped[0].Line != 2 || skipped[1].Line != 4 {
		t.Fatalf("skipped the wrong lines: %v", skipped)
	}
	if tmp == "" || usable != tmp {
		t.Fatal("a filtered file should have been staged")
	}
	defer os.Remove(tmp)

	body, err := os.ReadFile(usable)
	if err != nil {
		t.Fatal(err)
	}
	got := string(body)
	for _, want := range []string{"eng-leaf-1.lab.local", "usa-leaf-2.lab.local"} {
		if !strings.Contains(got, want) {
			t.Errorf("filtered file lost a valid entry for %s", want)
		}
	}
	if strings.Contains(got, "not-a-valid-known-hosts-line") {
		t.Error("filtered file kept a malformed line")
	}

	// The operator's file is theirs. Repairing it behind their back is not
	// this package's call.
	orig, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(orig), "this-is-not-a-valid-known-hosts-line") {
		t.Error("the source file was modified")
	}
}

func TestUsableKnownHostsStagedFileIsPrivate(t *testing.T) {
	path := writeKnownHosts(t, "junk", "eng-leaf-1.lab.local "+validKey)

	_, tmp, _, err := usableKnownHosts(path)
	if err != nil {
		t.Fatal(err)
	}
	if tmp == "" {
		t.Fatal("expected a staged file")
	}
	defer os.Remove(tmp)

	// Not a mode check: os.Stat on Windows reports 0666 for any writable
	// file regardless of its ACL, so asserting 0600 fails there on every
	// correctly-secured file. privatefile asks the question each platform
	// can actually answer.
	private, err := privatefile.IsPrivate(tmp)
	if err != nil {
		t.Fatal(err)
	}
	if !private {
		t.Error("staged known_hosts is readable by someone other than its owner")
	}
}

func TestUsableKnownHostsPreservesCommentsAndBlanks(t *testing.T) {
	path := writeKnownHosts(t,
		"# managed by the lab build",
		"",
		"junk line",
		"eng-leaf-1.lab.local "+validKey,
	)

	usable, tmp, skipped, err := usableKnownHosts(path)
	if err != nil {
		t.Fatal(err)
	}
	if len(skipped) != 1 {
		t.Fatalf("skipped %d lines, want 1", len(skipped))
	}
	defer os.Remove(tmp)

	body, _ := os.ReadFile(usable)
	if !strings.Contains(string(body), "# managed by the lab build") {
		t.Error("comments should survive filtering")
	}
}

func TestUsableKnownHostsMissingFile(t *testing.T) {
	_, _, _, err := usableKnownHosts(filepath.Join(t.TempDir(), "nope"))
	if err == nil {
		t.Fatal("expected an error for a missing file")
	}
}

func TestSkippedHostLineMessageNamesTheLine(t *testing.T) {
	path := writeKnownHosts(t, "junk")
	_, tmp, skipped, err := usableKnownHosts(path)
	if err != nil {
		t.Fatal(err)
	}
	if tmp != "" {
		defer os.Remove(tmp)
	}
	if len(skipped) != 1 {
		t.Fatalf("skipped %d lines, want 1", len(skipped))
	}
	if !strings.Contains(skipped[0].String(), "line 1") {
		t.Fatalf("message should name the line: %q", skipped[0].String())
	}
}
