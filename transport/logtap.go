// transport/logtap.go
// Session logging, tapped at the byte-stream boundary.
//
// nterm-qt hangs capture off the terminal widget, which means the widget has
// to be told about it, every transport has to route through the widget to be
// covered, and anything the emulator drops is not in the log. The boundary is
// the better place: with all transports sharing one read loop there is exactly
// one tap, it is below the emulator, and a transport added later is logged
// without being taught to be.
//
// What lands in the file is the raw byte stream, escape sequences included, so
// `cat` replays the session as it looked. Stripping them would make the file
// easier to read and would also throw away the thing that is usually being
// investigated when someone goes looking at a session log.
package transport

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"github.com/scottpeterman/omegassh/internal/privatefile"
)

// LogOptions describes one session's log file.
type LogOptions struct {
	// Path is the file to write. Empty means a generated name under Dir.
	Path string

	// Dir is where a generated name lands. Empty means DefaultLogDir().
	//
	// It defaults to a user directory rather than the process working
	// directory on purpose: an operator launching a portable binary from a
	// share should not be writing session transcripts into it, and in a
	// locked-down enterprise environment that directory is frequently not
	// writable at all.
	Dir string

	// Label is worked into a generated filename -- a host, a port name, a
	// session name. Sanitized; anything is safe to pass.
	Label string

	// IncludeInput records what the operator typed as well as what the
	// device sent.
	//
	// Off by default, and deliberately: a device's own login prompt is
	// ordinary session data on telnet and on a serial console, so the
	// password answering it is ordinary keystrokes. Turning this on writes
	// that password to a plaintext file. It is genuinely useful for
	// reproducing a config change, which is why it exists at all, but it is
	// not something to get by accident.
	IncludeInput bool

	// Append opens an existing file for appending instead of truncating.
	Append bool
}

// Logger writes one session's byte stream to one file. It belongs to a single
// Session, which closes it when the session ends.
//
// Writes are unbuffered. A buffered log loses the last screenful in exactly
// the case a session log is being read for -- the session that ended badly --
// and the volume here is a terminal's worth of bytes, so there is nothing to
// be gained by holding them.
type Logger struct {
	mu       sync.Mutex
	f        *os.File
	path     string
	input    bool
	closed   bool
	writeErr error
}

// ConfigDirName is the directory under $HOME that holds config.json,
// omega.json, sessions.db, vault.json and logs/.
//
// A SECOND DEFINITION, and knowingly so. The C++ side owns the canonical one
// in SettingsManager::defaultConfigDir(); this package sits below the C
// boundary and cannot call up through it, so the name is repeated here rather
// than plumbed down through every Config that might want a log. The two are
// asserted against each other in tests/compat/config_dir_probe.cpp, which is
// what stops them drifting.
const ConfigDirName = ".omega"

// DefaultLogDir is where generated log names land: ~/.omega/logs, beside the
// config and the session store, on every platform.
//
// Falls back to the OS temp directory if the home directory cannot be
// determined, which is the one case where writing nothing would be worse than
// writing somewhere unexpected.
func DefaultLogDir() string {
	home, err := os.UserHomeDir()
	if err != nil || home == "" {
		return filepath.Join(os.TempDir(), "omegassh-logs")
	}
	return filepath.Join(home, ConfigDirName, "logs")
}

// sanitizeLabel reduces a target summary to something legal as a filename on
// all three platforms. "10.0.0.1:23" and "/dev/ttyUSB0" both arrive here.
func sanitizeLabel(s string) string {
	s = strings.TrimSpace(s)
	if s == "" {
		return "session"
	}
	var b strings.Builder
	for _, r := range s {
		switch {
		case r >= 'a' && r <= 'z', r >= 'A' && r <= 'Z', r >= '0' && r <= '9':
			b.WriteRune(r)
		case r == '.' || r == '-' || r == '_':
			b.WriteRune(r)
		default:
			b.WriteRune('_')
		}
	}
	out := strings.Trim(b.String(), "._-")
	if out == "" {
		return "session"
	}
	if len(out) > 64 {
		out = out[:64]
	}
	return out
}

// OpenLog creates the log file, making the directory if it does not exist.
func OpenLog(opts LogOptions) (*Logger, error) {
	path := opts.Path
	if path == "" {
		dir := opts.Dir
		if dir == "" {
			dir = DefaultLogDir()
		}
		name := fmt.Sprintf("%s-%s.log",
			time.Now().Format("20060102-150405"), sanitizeLabel(opts.Label))
		path = filepath.Join(dir, name)
	}
	if dir := filepath.Dir(path); dir != "" {
		if err := os.MkdirAll(dir, 0o700); err != nil {
			return nil, fmt.Errorf("create log directory %s: %w", dir, err)
		}
		if err := privatefile.HardenDir(dir); err != nil {
			return nil, err
		}
	}

	flags := os.O_CREATE | os.O_WRONLY
	if opts.Append {
		flags |= os.O_APPEND
	} else {
		flags |= os.O_TRUNC
	}
	// 0600: a session transcript is device output and, with IncludeInput on,
	// possibly a password. It is not group-readable.
	f, err := os.OpenFile(path, flags, 0o600)
	if err != nil {
		return nil, fmt.Errorf("open log %s: %w", path, err)
	}
	// The mode above does nothing on Windows, where the transcript would
	// otherwise inherit whatever the log directory carries. Harden before the
	// first write rather than after.
	if err := privatefile.Harden(path); err != nil {
		f.Close()
		return nil, err
	}
	return &Logger{f: f, path: path, input: opts.IncludeInput}, nil
}

// Path is the file being written.
func (l *Logger) Path() string {
	if l == nil {
		return ""
	}
	return l.path
}

// Output records bytes received from the device.
func (l *Logger) Output(p []byte) { l.write(p) }

// Input records bytes sent to the device, if IncludeInput was set. A nil
// receiver and a disabled logger both do nothing, so callers never branch.
func (l *Logger) Input(p []byte) {
	if l == nil || !l.input {
		return
	}
	l.write(p)
}

// write is best-effort. A full disk must not take a live session down with
// it, so the first failure is recorded for Err and then the log goes quiet.
func (l *Logger) write(p []byte) {
	if l == nil || len(p) == 0 {
		return
	}
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.closed || l.f == nil || l.writeErr != nil {
		return
	}
	if _, err := l.f.Write(p); err != nil {
		l.writeErr = err
	}
}

// Err reports the first write failure, if the log stopped recording.
func (l *Logger) Err() error {
	if l == nil {
		return nil
	}
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.writeErr
}

// Close flushes and closes the file. Idempotent.
func (l *Logger) Close() error {
	if l == nil {
		return nil
	}
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.closed {
		return nil
	}
	l.closed = true
	if l.f == nil {
		return nil
	}
	err := l.f.Close()
	l.f = nil
	return err
}
