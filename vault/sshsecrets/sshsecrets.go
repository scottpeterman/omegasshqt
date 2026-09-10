// vault/sshsecrets/sshsecrets.go
//
// Exposes a credential vault as an sshcore.SecretSource.
//
// It lives in its own package for a reason worth stating: vault must not
// import sshcore and sshcore must not import vault. Either edge would tie two
// independently useful packages together, and the whole point of exporting
// sshcore rather than hiding it under internal/ is that a Go program can take
// the SSH half without the credential store, or the store without the SSH.
//
// So the join goes in a third place that imports both and that nothing else
// depends on. Anyone who wants the pairing imports this; anyone who wants one
// half ignores it and carries no extra dependency.
//
// This is also the piece that makes "plaintext never crosses the C boundary"
// true rather than aspirational. The shim hands down a credential NAME. The
// vault is read here, inside the process, on the Go side, during the dial --
// and the resolved material is dropped when the dial returns.
package sshsecrets

import (
	"errors"

	"github.com/scottpeterman/omegassh/sshcore"
	"github.com/scottpeterman/omegassh/vault"
)

// ErrDisabled is returned for a credential that exists but has been taken out
// of service. It is deliberately distinct from "not found": an operator who
// disabled a credential wants to be told that is why the dial refused, not
// sent looking for a typo in the name.
var ErrDisabled = errors.New("credential is disabled")

// Source resolves sshcore credential references against a vault.
//
// The vault must be unlocked. Resolving against a locked one fails with
// vault.ErrVaultLocked, which is the right answer: it is the caller's job to
// unlock before dialing, and silently prompting from inside a dial is how a
// GUI ends up blocked on a dialog it did not open.
type Source struct {
	v *vault.Vault
}

// New wraps a vault. The vault is borrowed, not owned -- closing or locking it
// remains the caller's business, and a Source over a locked vault simply fails
// to resolve rather than becoming invalid.
func New(v *vault.Vault) *Source { return &Source{v: v} }

// Resolve implements sshcore.SecretSource.
//
// ref is matched the way the rest of the vault matches: exact id, then exact
// name, then case-insensitive name. A session store that records a credential
// by name therefore keeps working after a rename only if the name is what was
// stored -- which is why the store should record ids where it can.
func (s *Source) Resolve(ref string) (sshcore.Secret, error) {
	if s == nil || s.v == nil {
		return sshcore.Secret{}, errors.New("no vault configured")
	}

	cred, err := s.v.Get(ref)
	if err != nil {
		return sshcore.Secret{}, err
	}
	if cred.Disabled {
		// Bare, without the name: sshcore prefixes the reference it was given,
		// and doubling it reads as a bug in the error path.
		return sshcore.Secret{}, ErrDisabled
	}

	// MarkUsed is best-effort on purpose: a "last used" timestamp that fails
	// to write must not fail the connection the operator asked for.
	s.v.MarkUsed(cred.ID)

	out := sshcore.Secret{
		Username:      cred.Username,
		KeyPassphrase: cred.KeyPassphrase,
	}

	// The vault's auth type decides which material is offered, rather than
	// handing over everything the record happens to hold. A key-based
	// credential that also carries a stale password should not quietly fall
	// back to it -- that turns a key problem into a successful login by a path
	// nobody chose, and it is the kind of thing that only shows up in an audit.
	switch cred.Method() {
	case vault.AuthAgent:
		out.UseAgent = true
	case vault.AuthPublicKey:
		out.PrivateKeyPath = cred.KeyPath
	case vault.AuthKeyboardInteractive, vault.AuthPassword:
		out.Password = cred.Password
	}

	return out, nil
}

// Ensure the interface is satisfied at compile time rather than at the first
// dial that uses one.
var _ sshcore.SecretSource = (*Source)(nil)
