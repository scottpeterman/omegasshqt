// vault/changemaster_test.go
//
// ChangeMasterPassword verifies the old master against the file on disk, then
// re-keys and calls saveLocked() -- which serializes v.creds, the IN-MEMORY
// slice. On a handle that was never unlocked, v.creds is nil, so the re-key
// writes an empty credential list sealed under the new master and returns nil.
// The old master no longer opens the file, so there is nothing to fall back to.
//
// The unlocked-handle case is included so a fix can be told apart from a
// change in behavior: it passes today and must keep passing.
package vault

import (
	"path/filepath"
	"testing"
)

func seedLabVault(t *testing.T, path, master string) {
	t.Helper()
	v := New(path)
	if err := v.Create(master); err != nil {
		t.Fatalf("create: %v", err)
	}
	if _, err := v.Add(Credential{
		Name:     "lab-admin",
		Username: "labadmin",
		AuthType: "password",
		Password: "s3cret",
	}); err != nil {
		t.Fatalf("add: %v", err)
	}
	v.Lock()
}

// A caller that constructs a Vault purely to re-key it -- a CLI subcommand, or
// a C++ caller reaching through the cgo shim -- must not lose the contents.
func TestChangeMasterOnANeverUnlockedHandleKeepsCredentials(t *testing.T) {
	path := filepath.Join(t.TempDir(), "vault.json")
	seedLabVault(t, path, "labmaster01")

	if err := New(path).ChangeMasterPassword("labmaster01", "labmaster02"); err != nil {
		t.Fatalf("change master: %v", err)
	}

	reopened := New(path)
	if err := reopened.Unlock("labmaster02"); err != nil {
		t.Fatalf("unlock under new master: %v", err)
	}
	creds, err := reopened.All()
	if err != nil {
		t.Fatalf("all: %v", err)
	}
	if len(creds) != 1 {
		t.Fatalf("had 1 credential before the re-key, have %d after", len(creds))
	}
	if creds[0].Password != "s3cret" {
		t.Fatalf("secret did not survive the re-key")
	}
}

// The same operation on an unlocked handle already works. Kept so a fix is
// not mistaken for a behavior change.
func TestChangeMasterOnAnUnlockedHandleKeepsCredentials(t *testing.T) {
	path := filepath.Join(t.TempDir(), "vault.json")

	v := New(path)
	if err := v.Create("labmaster01"); err != nil {
		t.Fatalf("create: %v", err)
	}
	if _, err := v.Add(Credential{
		Name:     "lab-admin",
		Username: "labadmin",
		AuthType: "password",
		Password: "s3cret",
	}); err != nil {
		t.Fatalf("add: %v", err)
	}
	if err := v.ChangeMasterPassword("labmaster01", "labmaster02"); err != nil {
		t.Fatalf("change master: %v", err)
	}

	reopened := New(path)
	if err := reopened.Unlock("labmaster02"); err != nil {
		t.Fatalf("unlock under new master: %v", err)
	}
	creds, _ := reopened.All()
	if len(creds) != 1 {
		t.Fatalf("had 1 credential before the re-key, have %d after", len(creds))
	}
}

// A re-key must invalidate the old master. This passes today; it is here so a
// fix to the two tests above cannot be achieved by weakening the re-key.
func TestChangeMasterInvalidatesTheOldMaster(t *testing.T) {
	path := filepath.Join(t.TempDir(), "vault.json")
	seedLabVault(t, path, "labmaster01")

	if err := New(path).ChangeMasterPassword("labmaster01", "labmaster02"); err != nil {
		t.Fatalf("change master: %v", err)
	}
	if err := New(path).Unlock("labmaster01"); err == nil {
		t.Fatal("the old master still unlocks the vault")
	}
}
