// vault/sshsecrets/sshsecrets_test.go
package sshsecrets

import (
	"errors"
	"path/filepath"
	"testing"

	"github.com/scottpeterman/omegassh/vault"
)

func labVault(t *testing.T) *vault.Vault {
	t.Helper()
	v := vault.New(filepath.Join(t.TempDir(), "vault.json"))
	if err := v.Create("labmaster01"); err != nil {
		t.Fatal(err)
	}
	return v
}

func add(t *testing.T, v *vault.Vault, c vault.Credential) vault.Credential {
	t.Helper()
	out, err := v.Add(c)
	if err != nil {
		t.Fatal(err)
	}
	return out
}

func TestResolvePasswordCredential(t *testing.T) {
	v := labVault(t)
	add(t, v, vault.Credential{Name: "lab-admin", Username: "labadmin",
		AuthType: "password", Password: "s3cret"})

	got, err := New(v).Resolve("lab-admin")
	if err != nil {
		t.Fatal(err)
	}
	if got.Username != "labadmin" || got.Password != "s3cret" {
		t.Fatalf("got %+v", got)
	}
}

// The vault's auth type decides what is offered. A key credential that also
// holds a stale password must not silently fall back to it -- that turns a key
// failure into a login by a path nobody chose.
func TestKeyCredentialDoesNotOfferItsStalePassword(t *testing.T) {
	v := labVault(t)
	add(t, v, vault.Credential{Name: "lab-key", Username: "labadmin",
		AuthType: "publickey", KeyPath: "~/.ssh/id_ed25519",
		KeyPassphrase: "kp", Password: "left-over"})

	got, err := New(v).Resolve("lab-key")
	if err != nil {
		t.Fatal(err)
	}
	if got.PrivateKeyPath != "~/.ssh/id_ed25519" || got.KeyPassphrase != "kp" {
		t.Fatalf("key material missing: %+v", got)
	}
	if got.Password != "" {
		t.Fatalf("stale password offered: %q", got.Password)
	}
}

func TestAgentCredentialCarriesNoMaterial(t *testing.T) {
	v := labVault(t)
	add(t, v, vault.Credential{Name: "lab-agent", Username: "labadmin",
		AuthType: "agent"})

	got, err := New(v).Resolve("lab-agent")
	if err != nil {
		t.Fatal(err)
	}
	if !got.UseAgent {
		t.Fatal("UseAgent not set")
	}
	if got.Password != "" || got.PrivateKeyPath != "" {
		t.Fatalf("material supplied: %+v", got)
	}
}

func TestResolveByID(t *testing.T) {
	v := labVault(t)
	c := add(t, v, vault.Credential{Name: "lab-admin", Username: "labadmin",
		AuthType: "password", Password: "s3cret"})

	got, err := New(v).Resolve(c.ID)
	if err != nil {
		t.Fatal(err)
	}
	if got.Username != "labadmin" {
		t.Fatalf("got %+v", got)
	}
}

// Disabled is distinct from missing: an operator who disabled a credential
// should be told that, not sent hunting for a typo.
func TestDisabledCredentialIsRefusedDistinctly(t *testing.T) {
	v := labVault(t)
	c := add(t, v, vault.Credential{Name: "lab-old", Username: "labadmin",
		AuthType: "password", Password: "s3cret"})
	if err := v.SetDisabled(c.ID, true); err != nil {
		t.Fatal(err)
	}

	_, err := New(v).Resolve("lab-old")
	if !errors.Is(err, ErrDisabled) {
		t.Fatalf("want ErrDisabled, got %v", err)
	}
	if errors.Is(err, vault.ErrCredNotFound) {
		t.Fatal("disabled reported as not-found")
	}
}

func TestUnknownCredential(t *testing.T) {
	v := labVault(t)
	if _, err := New(v).Resolve("not-there"); !errors.Is(err, vault.ErrCredNotFound) {
		t.Fatalf("want ErrCredNotFound, got %v", err)
	}
}

// A locked vault must fail rather than prompt. Prompting from inside a dial is
// how a GUI ends up blocked on a dialog it did not open.
func TestLockedVaultRefusesToResolve(t *testing.T) {
	v := labVault(t)
	add(t, v, vault.Credential{Name: "lab-admin", Username: "labadmin",
		AuthType: "password", Password: "s3cret"})
	v.Lock()

	if _, err := New(v).Resolve("lab-admin"); !errors.Is(err, vault.ErrVaultLocked) {
		t.Fatalf("want ErrVaultLocked, got %v", err)
	}
}

func TestResolveStampsLastUsed(t *testing.T) {
	v := labVault(t)
	c := add(t, v, vault.Credential{Name: "lab-admin", Username: "labadmin",
		AuthType: "password", Password: "s3cret"})

	if _, err := New(v).Resolve("lab-admin"); err != nil {
		t.Fatal(err)
	}
	after, err := v.Get(c.ID)
	if err != nil {
		t.Fatal(err)
	}
	if after.LastUsed.IsZero() {
		t.Fatal("LastUsed not stamped")
	}
}
