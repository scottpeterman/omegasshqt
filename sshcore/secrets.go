// sshcore/secrets.go
//
// Dialing from a reference to a credential rather than from the credential.
//
// The point is not convenience. A caller outside this process -- the C++ shell
// above the cgo boundary -- should never need to hold a password in order to
// use one. It names a credential, the material is fetched here, and the
// plaintext exists only inside this package for the length of a dial.
//
// SecretSource is declared here, and satisfied elsewhere, so that sshcore
// imports nothing to support it. A credential store implements this interface;
// sshcore does not know that any particular store exists. Both stay importable
// on their own by a Go program that wants one and not the other, which is the
// reason sshcore is exported rather than internal.
package sshcore

import "fmt"

// Secret is credential material for one dial. It is the resolved form of a
// reference: whatever the store held, expressed in the terms sshcore uses.
//
// A zero Secret is legitimate -- an agent-only credential carries no material
// at all -- so emptiness is not an error here. A dial with no usable method
// fails later, where the message can say which host it was for.
type Secret struct {
	Username       string
	Password       string
	PrivateKeyPath string
	PrivateKey     []byte // in-memory key; takes precedence over the path
	KeyPassphrase  string
	UseAgent       bool
}

// SecretSource resolves a reference to credential material.
//
// ref is whatever the store uses to name a credential -- an id, a name -- and
// sshcore does not interpret it. Resolve is called at most twice per dial, once
// for the target and once for a jump host, and only when a reference was given.
//
// An implementation must be safe for concurrent use: several dials can be in
// flight, and they will not be on the same goroutine.
type SecretSource interface {
	Resolve(ref string) (Secret, error)
}

// CheckCredentials resolves cfg's credential references and throws the
// material away, so a caller can refuse a bad reference -- an unknown name, a
// credential taken out of service, a store that is locked -- before it commits
// to a dial it would then have to unwind.
//
// It exists for a caller that has moved the dial onto its own goroutine: a
// misnamed credential is a configuration mistake and belongs in the same
// answer as a missing hostname, not several seconds later as a connection
// failure.
//
// The copy is the point. Resolved material lives in this call's stack frame
// and nothing survives the return, and Dial still resolves for itself -- one
// extra store lookup against an already-unlocked vault, in exchange for
// dialing never depending on what somebody else resolved beforehand.
func CheckCredentials(cfg Config) error {
	c := cfg.withDefaults()
	return c.resolveSecrets()
}

// resolveInto fills the empty credential fields of c from cfg.Credential.
//
// Explicit fields win. A caller that names a credential AND sets a username is
// overriding one field of it, not discarding the rest -- which is what a
// connect dialog does when the operator edits the login for one session
// without wanting a second credential. The rule is uniform: the reference
// supplies what was left blank and nothing more.
//
// Fields cleared to empty on purpose cannot be told from fields never set,
// because JSON absence and JSON "" arrive here identically. That is a real
// limitation and the reason the rule is "fills blanks" rather than something
// cleverer that would need a tri-state for every field.
func (c *Config) resolveSecrets() error {
	if c.Credential != "" {
		if c.Secrets == nil {
			return fmt.Errorf("credential %q named but no secret source configured", c.Credential)
		}
		s, err := c.Secrets.Resolve(c.Credential)
		if err != nil {
			return fmt.Errorf("credential %q: %w", c.Credential, err)
		}
		applySecret(&c.Username, &c.Password, &c.PrivateKeyPath, &c.PrivateKey,
			&c.KeyPassphrase, &c.UseAgent, s)
	}

	if c.Jump != nil && c.Jump.Credential != "" {
		if c.Secrets == nil {
			return fmt.Errorf("jump host credential %q named but no secret source configured",
				c.Jump.Credential)
		}
		s, err := c.Secrets.Resolve(c.Jump.Credential)
		if err != nil {
			return fmt.Errorf("jump host credential %q: %w", c.Jump.Credential, err)
		}
		// The jump host has no agent flag and no in-memory key field, so those
		// parts of a resolved credential do not apply to it.
		var key []byte
		var useAgent bool
		applySecret(&c.Jump.Username, &c.Jump.Password, &c.Jump.PrivateKeyPath,
			&key, &c.Jump.KeyPassphrase, &useAgent, s)
	}

	return nil
}

func applySecret(username, password, keyPath *string, key *[]byte,
	passphrase *string, useAgent *bool, s Secret) {

	if *username == "" {
		*username = s.Username
	}
	if *password == "" {
		*password = s.Password
	}
	if *keyPath == "" {
		*keyPath = s.PrivateKeyPath
	}
	if len(*key) == 0 {
		*key = s.PrivateKey
	}
	if *passphrase == "" {
		*passphrase = s.KeyPassphrase
	}
	// A bool has no blank, so a resolved true wins and a resolved false cannot
	// turn off a caller's explicit true. Agent auth is additive -- it is tried
	// first and falls through -- so the asymmetry costs nothing.
	if s.UseAgent {
		*useAgent = true
	}
}
