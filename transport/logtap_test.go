// transport/logtap_test.go
// Behaviour tests for the session log tap.
//
// The rule worth a test more than any other here is that operator input is
// NOT recorded unless it was asked for. On telnet and on a serial console the
// device's own login prompt is ordinary session data, so the password
// answering it is ordinary keystrokes -- a default that flipped the wrong way
// would write device passwords to a plaintext file and nothing would say so.
package transport

import (
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func readLog(t *testing.T, path string) string {
	t.Helper()
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read log: %v", err)
	}
	return string(b)
}

// The default records what the device sent and nothing the operator typed.
func TestInputIsNotLoggedByDefault(t *testing.T) {
	path := filepath.Join(t.TempDir(), "session.log")
	log, err := OpenLog(LogOptions{Path: path})
	if err != nil {
		t.Fatalf("OpenLog: %v", err)
	}

	sh := newFakeShell("lab-sw1 login: ")
	s := WrapSSH(sh, "lab-sw1:22", log)

	if _, err := s.Write([]byte("labadmin\rhunter2\r")); err != nil {
		t.Fatalf("write: %v", err)
	}
	if _, err := io.ReadAll(s.Output()); err != nil {
		t.Fatalf("read: %v", err)
	}
	_ = s.Close()

	got := readLog(t, path)
	if !strings.Contains(got, "lab-sw1 login: ") {
		t.Errorf("device output missing from the log: %q", got)
	}
	if strings.Contains(got, "hunter2") {
		t.Fatalf("operator input was logged without log_input being set: %q", got)
	}
}

// With IncludeInput on it records both, which is the point of the option.
func TestInputIsLoggedWhenAskedFor(t *testing.T) {
	path := filepath.Join(t.TempDir(), "session.log")
	log, err := OpenLog(LogOptions{Path: path, IncludeInput: true})
	if err != nil {
		t.Fatalf("OpenLog: %v", err)
	}

	sh := newFakeShell("lab-sw1# ")
	s := WrapSSH(sh, "lab-sw1:22", log)

	if _, err := s.Write([]byte("show version\r")); err != nil {
		t.Fatalf("write: %v", err)
	}
	if _, err := io.ReadAll(s.Output()); err != nil {
		t.Fatalf("read: %v", err)
	}
	_ = s.Close()

	got := readLog(t, path)
	for _, want := range []string{"lab-sw1# ", "show version"} {
		if !strings.Contains(got, want) {
			t.Errorf("log missing %q: %q", want, got)
		}
	}
}

// The log is closed when the session ends, not left for the caller to
// remember. A tab closed at the end of a change window should not need a
// flush that nothing calls.
func TestLogIsClosedWithTheSession(t *testing.T) {
	path := filepath.Join(t.TempDir(), "session.log")
	log, err := OpenLog(LogOptions{Path: path})
	if err != nil {
		t.Fatalf("OpenLog: %v", err)
	}

	ft := newFakeTransport()
	s := Wrap(ft, KindSerial, "/dev/ttyUSB0 9600 8N1", log)
	_ = s.Close()
	<-s.Done()

	// Writing after the close is a no-op rather than a panic on a nil file:
	// a late byte from a read that was already in flight must not take the
	// process down.
	log.Output([]byte("late"))
	if strings.Contains(readLog(t, path), "late") {
		t.Fatal("bytes were written to a closed log")
	}
	if err := log.Close(); err != nil {
		t.Fatalf("Close is not idempotent: %v", err)
	}
}

// A generated name lands under the given directory, and a target summary that
// is not a legal filename does not stop it. "10.0.0.1:23" has a colon, which
// Windows refuses, and "/dev/ttyUSB0" has separators.
func TestGeneratedNameSanitizesTheLabel(t *testing.T) {
	dir := t.TempDir()
	for _, label := range []string{"10.0.0.1:23", "/dev/ttyUSB0 9600 8N1", "", "///"} {
		log, err := OpenLog(LogOptions{Dir: dir, Label: label})
		if err != nil {
			t.Fatalf("OpenLog(%q): %v", label, err)
		}
		name := filepath.Base(log.Path())
		if filepath.Dir(log.Path()) != dir {
			t.Errorf("log for %q landed at %s, not under %s", label, log.Path(), dir)
		}
		if strings.ContainsAny(name, `:/\"'`) {
			t.Errorf("generated name for %q is not filename-safe: %q", label, name)
		}
		log.Close()
	}
}

// A nil Logger is the "logging off" case and every call on it has to be safe,
// so the session adapters never branch on whether there is one.
func TestNilLoggerIsSafe(t *testing.T) {
	var log *Logger
	log.Output([]byte("x"))
	log.Input([]byte("x"))
	if log.Path() != "" || log.Err() != nil {
		t.Fatal("nil Logger reported state")
	}
	if err := log.Close(); err != nil {
		t.Fatalf("nil Logger Close: %v", err)
	}
}

// The default directory is a user directory, never the process working
// directory. An operator launching a portable binary from a share should not
// be writing session transcripts into it, and in a locked-down environment
// that directory is frequently not writable at all.
func TestDefaultLogDirIsNotTheLaunchDirectory(t *testing.T) {
	dir := DefaultLogDir()
	if dir == "" {
		t.Fatal("DefaultLogDir is empty")
	}
	if !filepath.IsAbs(dir) {
		t.Fatalf("DefaultLogDir is relative, so it follows the launch directory: %q", dir)
	}
	cwd, err := os.Getwd()
	if err == nil && dir == cwd {
		t.Fatalf("DefaultLogDir is the working directory: %q", dir)
	}
}
