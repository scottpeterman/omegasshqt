// internal/privatefile/privatefile_test.go
//
// These run identically on both platforms on purpose. Nothing here mentions a
// mode bit or a DACL -- if a case needed a GOOS check to pass, the package
// would not be doing its job.
package privatefile

import (
	"os"
	"path/filepath"
	"testing"
)

func writeFile(t *testing.T, name string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), name)
	if err := os.WriteFile(path, []byte("lab-edge-1.lab.local\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	return path
}

func TestHardenMakesAFilePrivate(t *testing.T) {
	path := writeFile(t, "secrets")
	if err := Harden(path); err != nil {
		t.Fatalf("Harden: %v", err)
	}
	private, err := IsPrivate(path)
	if err != nil {
		t.Fatalf("IsPrivate: %v", err)
	}
	if !private {
		t.Error("file is readable by someone other than its owner")
	}
}

func TestHardenIsIdempotent(t *testing.T) {
	path := writeFile(t, "secrets")
	for i := 0; i < 3; i++ {
		if err := Harden(path); err != nil {
			t.Fatalf("Harden call %d: %v", i+1, err)
		}
	}
	if private, err := IsPrivate(path); err != nil || !private {
		t.Fatalf("IsPrivate = %v, %v after repeated Harden", private, err)
	}
}

func TestHardenDirMakesADirectoryPrivate(t *testing.T) {
	dir := filepath.Join(t.TempDir(), "vault")
	if err := os.Mkdir(dir, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := HardenDir(dir); err != nil {
		t.Fatalf("HardenDir: %v", err)
	}
	private, err := IsPrivate(dir)
	if err != nil {
		t.Fatalf("IsPrivate: %v", err)
	}
	if !private {
		t.Error("directory is accessible to someone other than its owner")
	}
}

// A file created without hardening is not asserted to be non-private -- in a
// user profile it usually is private already, inherited. What is asserted is
// that the question can be answered, since a test elsewhere reporting "not
// private" has to mean the file and not a broken query.
func TestIsPrivateAnswersForAnUnhardenedFile(t *testing.T) {
	path := writeFile(t, "plain")
	if _, err := IsPrivate(path); err != nil {
		t.Fatalf("IsPrivate: %v", err)
	}
}

func TestHardenMissingFileIsAnError(t *testing.T) {
	if err := Harden(filepath.Join(t.TempDir(), "nope")); err == nil {
		t.Error("expected an error hardening a path that does not exist")
	}
}

func TestIsPrivateMissingFileIsAnError(t *testing.T) {
	if _, err := IsPrivate(filepath.Join(t.TempDir(), "nope")); err == nil {
		t.Error("expected an error querying a path that does not exist")
	}
}
