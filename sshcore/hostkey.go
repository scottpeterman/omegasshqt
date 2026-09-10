// sshcore/hostkey.go
// Host-key verification: known_hosts checking with optional TOFU on top.
//
// Ported from the tetherssh backend's callback. Semantics:
//   - known host, key matches  -> accept
//   - known host, key MISMATCH -> reject hard, always (possible MITM);
//     TOFU never applies to a mismatch
//   - unknown host, Strict     -> reject
//   - unknown host, TOFU       -> ask HostKeyPrompt; accepted keys persist
//   - Insecure                 -> skip verification (explicit opt-in only)
//
// One behavioral change from the baseline: with no known_hosts file the
// baseline silently fell back to InsecureIgnoreHostKey. Here a missing file
// is created empty and verification proceeds — an unverifiable host is a
// decision for the policy, never a silent downgrade.
package sshcore

import (
	"errors"
	"fmt"
	"net"
	"os"
	"path/filepath"

	"golang.org/x/crypto/ssh"
	"golang.org/x/crypto/ssh/knownhosts"

	"github.com/scottpeterman/omegassh/internal/privatefile"
)

// UnknownHostKeyMarker opens the message for the FIRST-CONTACT case, and only
// that one. A caller above the C boundary cannot be handed a prompt callback
// (see the note in capi/open.go), so the way it recognises "this host is new,
// offer the fingerprint" is by matching this marker in the error text.
//
// That makes the wording load-bearing. It is a constant rather than a literal
// so the C surface can publish the exact bytes that are matched against, and
// so renaming it is a compile error here rather than a prompt that silently
// stops appearing.
//
// It must NOT match the mismatch message below. That one is the re-key or
// MITM case, it fails closed, and offering to trust the offered key there is
// the one outcome worse than refusing to connect.
const UnknownHostKeyMarker = "unknown host key for"

func buildHostKeyCallback(cfg *Config) (ssh.HostKeyCallback, error) {
	if cfg.HostKeys == HostKeyInsecure {
		return ssh.InsecureIgnoreHostKey(), nil
	}

	path := cfg.KnownHostsPath
	if path == "" {
		return nil, errors.New("no known_hosts path available; set Config.KnownHostsPath")
	}

	if _, err := os.Stat(path); os.IsNotExist(err) {
		dir := filepath.Dir(path)
		if err := os.MkdirAll(dir, 0o700); err != nil {
			return nil, fmt.Errorf("create known_hosts directory: %w", err)
		}
		if err := privatefile.HardenDir(dir); err != nil {
			return nil, err
		}
		if err := os.WriteFile(path, []byte{}, 0o600); err != nil {
			return nil, fmt.Errorf("create known_hosts file: %w", err)
		}
		if err := privatefile.Harden(path); err != nil {
			return nil, err
		}
	}

	// A single malformed line makes knownhosts.New reject the whole file,
	// which would fail every connection over an entry for some unrelated
	// host. Filter to the parseable subset and report what was dropped.
	usable, tmp, skipped, err := usableKnownHosts(path)
	if err != nil {
		return nil, err
	}
	if tmp != "" {
		defer os.Remove(tmp)
	}
	if len(skipped) > 0 {
		if cfg.KnownHostsWarn != nil {
			cfg.KnownHostsWarn(skipped)
		} else {
			for _, s := range skipped {
				fmt.Fprintf(os.Stderr,
					"warning: %s: %s -- entry ignored, that host is no longer pinned\n",
					path, s)
			}
		}
	}

	base, err := knownhosts.New(usable)
	if err != nil {
		return nil, fmt.Errorf("load known_hosts: %w", err)
	}

	return func(hostname string, remote net.Addr, key ssh.PublicKey) error {
		err := base(hostname, remote, key)
		if err == nil {
			return nil // pinned and matched
		}

		var keyErr *knownhosts.KeyError
		if !errors.As(err, &keyErr) {
			return err // not a verification result — surface as-is
		}

		if len(keyErr.Want) > 0 {
			// Pinned to a DIFFERENT key: legitimate re-key or MITM.
			// Fail closed and tell the operator how to recover deliberately.
			return fmt.Errorf(
				"host key verification failed for %s: offered key (%s %s) does not match "+
					"the pinned key in %s; if this change is expected, remove the old entry "+
					"and reconnect", hostname, key.Type(), ssh.FingerprintSHA256(key), path)
		}

		// Unknown host — first contact.
		if cfg.HostKeys != HostKeyTOFU || cfg.HostKeyPrompt == nil {
			return fmt.Errorf("%s %s (%s %s); not in %s",
				UnknownHostKeyMarker, hostname, key.Type(),
				ssh.FingerprintSHA256(key), path)
		}

		accept, perr := cfg.HostKeyPrompt(hostname, remote, key)
		if perr != nil {
			return perr
		}
		if !accept {
			return fmt.Errorf("host key for %s rejected", hostname)
		}
		if werr := appendKnownHost(path, hostname, key); werr != nil {
			// Trusted for THIS session; warn that it won't be remembered.
			fmt.Fprintf(os.Stderr, "warning: accepted host key for %s but could not persist it: %v\n",
				hostname, werr)
		}
		return nil
	}, nil
}

// appendKnownHost appends one entry in OpenSSH line format. The hostname is
// normalized (port-22 collapses to the bare name, other ports become
// "[host]:port") so the entry matches on the next connect.
func appendKnownHost(path, hostname string, key ssh.PublicKey) error {
	dir := filepath.Dir(path)
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return fmt.Errorf("create known_hosts directory: %w", err)
	}
	if err := privatefile.HardenDir(dir); err != nil {
		return err
	}
	line := knownhosts.Line([]string{knownhosts.Normalize(hostname)}, key)
	f, err := os.OpenFile(path, os.O_APPEND|os.O_WRONLY|os.O_CREATE, 0o600)
	if err != nil {
		return fmt.Errorf("open known_hosts: %w", err)
	}
	defer f.Close()
	if err := privatefile.Harden(path); err != nil {
		return err
	}
	if _, err := f.WriteString(line + "\n"); err != nil {
		return fmt.Errorf("write known_hosts entry: %w", err)
	}
	return nil
}
