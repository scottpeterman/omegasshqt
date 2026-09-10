// sshcore/exit.go
// Normalizing a remote shell's exit status.
//
// SSH reports an exit two different ways: a numeric status, or a signal name
// for a command the far end killed. anytermqt's PtySession already settled
// what a caller should see for the local case -- the program's own status, or
// 128 + signal number where a signal ended it, which is the convention a
// shell uses. Matching that here means an application does not have to know
// whether it is driving a pty or an SSH channel to interpret the number.
package sshcore

import (
	"errors"

	"golang.org/x/crypto/ssh"
)

// ExitUnknown is reported when the session ended without a usable status --
// the transport dropped, or the shell is still running.
const ExitUnknown = -1

// signalNumbers maps the signal names SSH is allowed to send (RFC 4254 6.10)
// onto their POSIX numbers. SSH carries the name, not the number, precisely
// because the numbers differ between systems; the shell convention of
// 128+signal needs the number back.
var signalNumbers = map[ssh.Signal]int{
	ssh.SIGABRT: 6,
	ssh.SIGALRM: 14,
	ssh.SIGFPE:  8,
	ssh.SIGHUP:  1,
	ssh.SIGILL:  4,
	ssh.SIGINT:  2,
	ssh.SIGKILL: 9,
	ssh.SIGPIPE: 13,
	ssh.SIGQUIT: 3,
	ssh.SIGSEGV: 11,
	ssh.SIGTERM: 15,
	ssh.SIGUSR1: 10,
	ssh.SIGUSR2: 12,
}

// NormalizeExit turns the error from Session.Wait into an exit code.
//
//	nil                     -> 0
//	exited with status N    -> N
//	killed by signal S      -> 128 + S
//	anything else           -> ExitUnknown
//
// A signal SSH named but this package does not recognize still reports as a
// signal death rather than as unknown: 128 alone is wrong, but so is
// pretending the shell exited cleanly.
func NormalizeExit(err error) int {
	if err == nil {
		return 0
	}
	var exitErr *ssh.ExitError
	if errors.As(err, &exitErr) {
		if sig := ssh.Signal(exitErr.Signal()); sig != "" {
			if n, ok := signalNumbers[sig]; ok {
				return 128 + n
			}
			return 128
		}
		return exitErr.ExitStatus()
	}
	var missing *ssh.ExitMissingError
	if errors.As(err, &missing) {
		// The channel closed without a status. Common enough against network
		// gear, which frequently drops the session rather than reporting one.
		return ExitUnknown
	}
	return ExitUnknown
}
