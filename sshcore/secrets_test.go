// sshcore/secrets_test.go
package sshcore

import (
	"errors"
	"testing"
)

type fakeSource struct {
	secrets map[string]Secret
	calls   []string
	err     error
}

func (f *fakeSource) Resolve(ref string) (Secret, error) {
	f.calls = append(f.calls, ref)
	if f.err != nil {
		return Secret{}, f.err
	}
	s, ok := f.secrets[ref]
	if !ok {
		return Secret{}, errors.New("not found")
	}
	return s, nil
}

func TestResolveFillsBlankFields(t *testing.T) {
	src := &fakeSource{secrets: map[string]Secret{
		"lab-admin": {Username: "labadmin", Password: "s3cret"},
	}}
	c := Config{Host: "eng-leaf-1.lab.local", Credential: "lab-admin", Secrets: src}

	if err := c.resolveSecrets(); err != nil {
		t.Fatal(err)
	}
	if c.Username != "labadmin" || c.Password != "s3cret" {
		t.Fatalf("got %q/%q", c.Username, c.Password)
	}
}

// The connect-dialog case: an operator picks a saved credential but edits the
// login for this one session. The edit must win, and the rest of the
// credential must still apply.
func TestExplicitFieldsWinOverTheCredential(t *testing.T) {
	src := &fakeSource{secrets: map[string]Secret{
		"lab-admin": {Username: "labadmin", Password: "s3cret", KeyPassphrase: "kp"},
	}}
	c := Config{
		Host:       "eng-leaf-1.lab.local",
		Username:   "someone-else",
		Credential: "lab-admin",
		Secrets:    src,
	}

	if err := c.resolveSecrets(); err != nil {
		t.Fatal(err)
	}
	if c.Username != "someone-else" {
		t.Fatalf("explicit username overwritten: %q", c.Username)
	}
	if c.Password != "s3cret" || c.KeyPassphrase != "kp" {
		t.Fatal("the rest of the credential was not applied")
	}
}

// No reference means the source is never consulted. Quick connect with typed
// credentials must not touch a vault at all.
func TestNoCredentialLeavesConfigAloneAndDoesNotCallTheSource(t *testing.T) {
	src := &fakeSource{secrets: map[string]Secret{}}
	c := Config{Host: "eng-leaf-1.lab.local", Username: "labadmin",
		Password: "typed", Secrets: src}

	if err := c.resolveSecrets(); err != nil {
		t.Fatal(err)
	}
	if len(src.calls) != 0 {
		t.Fatalf("source consulted with no credential named: %v", src.calls)
	}
	if c.Username != "labadmin" || c.Password != "typed" {
		t.Fatal("manual credentials were modified")
	}
}

func TestCredentialWithNoSourceIsAnError(t *testing.T) {
	c := Config{Host: "eng-leaf-1.lab.local", Credential: "lab-admin"}
	if err := c.resolveSecrets(); err == nil {
		t.Fatal("expected an error naming the missing source")
	}
}

// A failure to resolve must name the credential. "not found" on its own sends
// someone to check the host.
func TestResolveErrorNamesTheCredential(t *testing.T) {
	src := &fakeSource{err: errors.New("vault is locked")}
	c := Config{Host: "eng-leaf-1.lab.local", Credential: "lab-admin", Secrets: src}

	err := c.resolveSecrets()
	if err == nil {
		t.Fatal("expected an error")
	}
	if got := err.Error(); got == "vault is locked" {
		t.Fatalf("error lost the credential name: %q", got)
	}
}

func TestJumpHostResolvesItsOwnCredential(t *testing.T) {
	src := &fakeSource{secrets: map[string]Secret{
		"lab-admin":   {Username: "labadmin", Password: "s3cret"},
		"lab-bastion": {Username: "labjump", Password: "j3ump"},
	}}
	c := Config{
		Host:       "eng-leaf-1.lab.local",
		Credential: "lab-admin",
		Secrets:    src,
		Jump:       &JumpConfig{Host: "bastion.lab.local", Credential: "lab-bastion"},
	}

	if err := c.resolveSecrets(); err != nil {
		t.Fatal(err)
	}
	if c.Username != "labadmin" || c.Password != "s3cret" {
		t.Fatalf("target: %q/%q", c.Username, c.Password)
	}
	if c.Jump.Username != "labjump" || c.Jump.Password != "j3ump" {
		t.Fatalf("jump: %q/%q", c.Jump.Username, c.Jump.Password)
	}
	if len(src.calls) != 2 {
		t.Fatalf("expected two lookups, got %v", src.calls)
	}
}

// An agent credential carries no material. That is legitimate, not an error.
func TestAgentCredentialResolvesToNoMaterial(t *testing.T) {
	src := &fakeSource{secrets: map[string]Secret{
		"lab-agent": {Username: "labadmin", UseAgent: true},
	}}
	c := Config{Host: "eng-leaf-1.lab.local", Credential: "lab-agent", Secrets: src}

	if err := c.resolveSecrets(); err != nil {
		t.Fatal(err)
	}
	if !c.UseAgent {
		t.Fatal("UseAgent not applied")
	}
	if c.Password != "" || c.PrivateKeyPath != "" {
		t.Fatal("agent credential supplied material it should not have")
	}
}

// Resolution must not write back into the caller's Config. Dial works on a
// copy for exactly this reason: a C++ caller that names a credential must not
// be able to read the resolved secret out of the config afterwards.
func TestDialDoesNotWriteResolvedMaterialBackToTheCaller(t *testing.T) {
	src := &fakeSource{secrets: map[string]Secret{
		"lab-admin": {Username: "labadmin", Password: "s3cret"},
	}}
	original := Config{Host: "eng-leaf-1.lab.local", Credential: "lab-admin", Secrets: src}

	c := original.withDefaults()
	if err := c.resolveSecrets(); err != nil {
		t.Fatal(err)
	}
	if original.Password != "" || original.Username != "" {
		t.Fatalf("caller's config was mutated: %q/%q",
			original.Username, original.Password)
	}
}
