// internal/privatefile/privatefile_posix.go
//go:build !windows

// The POSIX half, which is the boring one: the mode bits mean what they say,
// so Harden is chmod and IsPrivate is a stat. It exists so the Windows file
// has something to be the other half of.
package privatefile

import (
	"fmt"
	"os"
)

func harden(path string, dir bool) error {
	mode := os.FileMode(0o600)
	if dir {
		mode = 0o700
	}
	if err := os.Chmod(path, mode); err != nil {
		return fmt.Errorf("restrict %s: %w", path, err)
	}
	return nil
}

func isPrivate(path string) (bool, error) {
	info, err := os.Stat(path)
	if err != nil {
		return false, fmt.Errorf("stat %s: %w", path, err)
	}
	return info.Mode().Perm()&0o077 == 0, nil
}
