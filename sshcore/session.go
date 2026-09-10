// sshcore/session.go
// Interactive shell sessions on a pty.
//
// This lives in sshcore rather than in the cgo shim on purpose: a Go caller
// that imports this package directly gets the same pty handling, window
// resizing and exit detection as the C++ one. The shim above is marshalling
// only. Nothing here knows that C exists.
package sshcore

import (
	"errors"
	"fmt"
	"io"
	"sync"

	"golang.org/x/crypto/ssh"
)

// ShellOptions describes the pty requested for an interactive session.
type ShellOptions struct {
	Term string // "" => xterm-256color
	Cols int    // 0 => 80
	Rows int    // 0 => 24

	// Modes overrides the default terminal modes. Nil keeps the defaults,
	// which enable echo at 115200 baud in both directions.
	Modes ssh.TerminalModes
}

func (o ShellOptions) withDefaults() ShellOptions {
	out := o
	if out.Term == "" {
		out.Term = "xterm-256color"
	}
	if out.Cols <= 0 {
		out.Cols = 80
	}
	if out.Rows <= 0 {
		out.Rows = 24
	}
	if out.Modes == nil {
		out.Modes = ssh.TerminalModes{
			ssh.ECHO:          1,
			ssh.TTY_OP_ISPEED: 115200,
			ssh.TTY_OP_OSPEED: 115200,
		}
	}
	return out
}

// Session is a running interactive shell on a remote pty.
//
// Output is not buffered here. Stdout returns the raw reader so the caller
// decides how bytes are delivered: a Go caller can io.Copy it, and the cgo
// shim pumps it into an OutputBuffer. Safe for concurrent use.
type Session struct {
	client *Client
	sess   *ssh.Session
	stdin  io.WriteCloser
	stdout io.Reader

	mu       sync.Mutex
	closed   bool
	ownsConn bool
	waited   bool
	exitCode int
}

// NewShell requests a pty on a new channel and starts the login shell.
//
// The returned Session does NOT own the Client: closing it leaves the
// transport up so further sessions can be opened on it. Use OpenShell when
// one session per connection is what you want.
func (c *Client) NewShell(opts ShellOptions) (*Session, error) {
	o := opts.withDefaults()

	sess, err := c.SSH().NewSession()
	if err != nil {
		return nil, fmt.Errorf("open session channel: %w", err)
	}
	if err := sess.RequestPty(o.Term, o.Rows, o.Cols, o.Modes); err != nil {
		sess.Close()
		return nil, fmt.Errorf("request pty %s %dx%d: %w", o.Term, o.Cols, o.Rows, err)
	}
	stdin, err := sess.StdinPipe()
	if err != nil {
		sess.Close()
		return nil, fmt.Errorf("stdin pipe: %w", err)
	}
	// A pty merges stderr into stdout, so there is nothing separate to read.
	stdout, err := sess.StdoutPipe()
	if err != nil {
		sess.Close()
		return nil, fmt.Errorf("stdout pipe: %w", err)
	}
	if err := sess.Shell(); err != nil {
		sess.Close()
		return nil, fmt.Errorf("start shell: %w", err)
	}
	return &Session{client: c, sess: sess, stdin: stdin, stdout: stdout,
		exitCode: ExitUnknown}, nil
}

// OpenShell dials cfg and starts a shell in one step. The returned Session
// owns the connection: closing it tears down the transport and any bastion
// behind it.
func OpenShell(cfg Config, opts ShellOptions) (*Session, error) {
	client, err := Dial(cfg)
	if err != nil {
		return nil, err
	}
	s, err := client.NewShell(opts)
	if err != nil {
		client.Close()
		return nil, err
	}
	s.ownsConn = true
	return s, nil
}

// Stdout is the remote shell's output, stderr included (pty merges them).
// Reading returns io.EOF once the shell exits.
func (s *Session) Stdout() io.Reader { return s.stdout }

// Client is the connection this session runs on.
func (s *Session) Client() *Client { return s.client }

// Write sends keyboard input to the remote shell.
func (s *Session) Write(p []byte) (int, error) {
	s.mu.Lock()
	closed := s.closed
	s.mu.Unlock()
	if closed {
		return 0, errors.New("session is closed")
	}
	return s.stdin.Write(p)
}

// Resize sends a window-change request so the remote tty and any full-screen
// program running on it learn the new geometry.
func (s *Session) Resize(cols, rows int) error {
	if cols <= 0 || rows <= 0 {
		return fmt.Errorf("invalid window size %dx%d", cols, rows)
	}
	s.mu.Lock()
	closed := s.closed
	s.mu.Unlock()
	if closed {
		return errors.New("session is closed")
	}
	if err := s.sess.WindowChange(rows, cols); err != nil {
		return fmt.Errorf("resize to %dx%d: %w", cols, rows, err)
	}
	return nil
}

// Wait blocks until the remote shell exits and records the normalized exit
// code, which ExitCode then reports. A non-zero status comes back as an
// error, which for an interactive shell is ordinary.
//
// Call this only after the caller has read Stdout to EOF. x/crypto's Wait
// closes the session, and anything still sitting in the pipe is lost -- which
// costs you the last screenful of whatever the user just ran.
func (s *Session) Wait() error {
	err := s.sess.Wait()
	code := NormalizeExit(err)
	s.mu.Lock()
	s.waited = true
	s.exitCode = code
	s.mu.Unlock()
	return err
}

// ExitCode reports the shell's exit status, normalized the way
// anytermqt's PtySession reports a local child: the program's own status, or
// 128 + signal number where a signal ended it. Returns ExitUnknown until Wait
// has returned, and for a session that ended without reporting a status.
func (s *Session) ExitCode() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !s.waited {
		return ExitUnknown
	}
	return s.exitCode
}

// Close ends the shell and, for a Session created by OpenShell, the
// connection under it. Safe to call more than once.
func (s *Session) Close() error {
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		return nil
	}
	s.closed = true
	s.mu.Unlock()

	var first error
	if s.stdin != nil {
		if err := s.stdin.Close(); err != nil {
			first = err
		}
	}
	if s.sess != nil {
		if err := s.sess.Close(); err != nil && first == nil && !errors.Is(err, io.EOF) {
			first = err
		}
	}
	if s.ownsConn && s.client != nil {
		if err := s.client.Close(); err != nil && first == nil {
			first = err
		}
	}
	return first
}
