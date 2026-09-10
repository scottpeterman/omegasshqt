// internal/privatefile/privatefile.go
// Owner-only files, on every platform this builds for.
//
// A Unix mode argument is not a portable way to keep a file private. On
// Windows os.OpenFile and os.WriteFile discard the mode entirely, os.Chmod
// only toggles the read-only attribute, and os.Stat synthesizes Mode().Perm()
// from that one bit: 0666 for a writable file, 0444 for a read-only one. So
// 0600 is not merely ignored on Windows, it is unrepresentable -- code that
// passes it compiles, runs, and protects nothing, and a test that asserts it
// fails while pointing at the wrong thing.
//
// The access control that does exist on Windows is the DACL, and the file
// inherits one from its parent directory unless told otherwise. Inside a user
// profile that inheritance is usually adequate; outside one -- %TEMP% under a
// redirected TMP, a shared spool, a repository working copy, anywhere on a
// terminal server -- it is whatever the parent happened to carry. Inheritance
// is a default, not an invariant, and a credential vault should not rest on
// it.
//
// Harden makes the invariant explicit: chmod on POSIX, an explicit protected
// DACL granting the calling user alone on Windows. IsPrivate reads the same
// property back, so a test can assert privacy in the terms the platform
// actually uses rather than in mode bits one of them does not have.
//
// Both are best-effort in one specific sense: they operate on a path, so a
// file that has already been written and read by someone else cannot be
// un-leaked. Harden as close to creation as the write pattern allows.
package privatefile

// Harden restricts path so that only the calling user can read or write it.
//
// POSIX: chmod 0600 (0700 for a directory, via HardenDir).
// Windows: a protected DACL with a single allow-ACE for the caller's SID,
// which also severs inheritance from the parent directory.
//
// Hardening a path that does not exist returns an error. Hardening a path
// twice is harmless.
func Harden(path string) error { return harden(path, false) }

// HardenDir is Harden for a directory. On POSIX it is chmod 0700. On Windows
// the ACE is additionally marked inheritable, so files created in the
// directory afterwards start out private without a second call -- Harden them
// anyway if they hold secrets, since inheritance does not apply to a file that
// already exists.
func HardenDir(path string) error { return harden(path, true) }

// IsPrivate reports whether path is inaccessible to users other than its
// owner.
//
// POSIX: no group or other permission bits are set.
// Windows: every allow-ACE in the DACL names the calling user, LocalSystem,
// or BUILTIN\Administrators. The latter two are admitted because an
// administrator can take ownership of any file regardless of its ACL, so
// denying them buys nothing and breaks backup and endpoint tooling; this is
// the same set OpenSSH for Windows accepts on a private key.
//
// A false return is a fact about the file, not an error. An error means the
// question could not be answered.
func IsPrivate(path string) (bool, error) { return isPrivate(path) }
