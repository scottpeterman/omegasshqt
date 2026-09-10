// sshcore/knownhostsfilter.go
// Tolerating a malformed known_hosts file.
//
// x/crypto/ssh/knownhosts parses the ENTIRE file when the callback is built,
// and returns an error on the first line it cannot read. The blast radius is
// the whole file, not the offending host: one bad line and every subsequent
// connection fails at load time with a parse complaint about a host the
// operator was not even dialing. OpenSSH does not behave this way, so the
// failure reads as "your client is broken" rather than "line 47 is junk".
//
// This filter parses the file a line at a time, keeps what is valid, and
// reports what it dropped. Dropping a line is never silent: a skipped entry
// means a host that WAS pinned no longer is, which the operator must know.
package sshcore

import (
	"bufio"
	"fmt"
	"os"
	"strings"

	"golang.org/x/crypto/ssh"

	"github.com/scottpeterman/omegassh/internal/privatefile"
)

// SkippedHostLine describes one known_hosts line that could not be parsed.
type SkippedHostLine struct {
	Line int    // 1-based line number in the source file
	Text string // the raw line, truncated for display
	Err  error
}

func (s SkippedHostLine) String() string {
	return fmt.Sprintf("line %d: %v", s.Line, s.Err)
}

// KnownHostsWarnFunc is called once per unparseable line when a known_hosts
// file is loaded. Never called when the file is clean.
type KnownHostsWarnFunc func(skipped []SkippedHostLine)

// usableKnownHosts returns a path safe to hand to knownhosts.New.
//
// A clean file is returned unchanged. A file with unparseable lines is
// rewritten to a temporary file containing only the valid entries; the
// caller gets that path plus the list of what was dropped. The temporary
// file is the caller's to remove.
func usableKnownHosts(path string) (usable string, tmp string, skipped []SkippedHostLine, err error) {
	f, err := os.Open(path)
	if err != nil {
		return "", "", nil, fmt.Errorf("open known_hosts: %w", err)
	}
	defer f.Close()

	var good []string
	scanner := bufio.NewScanner(f)
	// known_hosts lines carry full base64 keys; the default 64KB token limit
	// is generous but a certificate line can approach it.
	scanner.Buffer(make([]byte, 0, 64*1024), 1024*1024)

	lineNo := 0
	for scanner.Scan() {
		lineNo++
		raw := scanner.Text()
		trimmed := strings.TrimSpace(raw)
		// Blank lines and comments are legal and parse to nothing.
		if trimmed == "" || strings.HasPrefix(trimmed, "#") {
			good = append(good, raw)
			continue
		}
		if _, _, _, _, _, perr := ssh.ParseKnownHosts([]byte(raw + "\n")); perr != nil {
			skipped = append(skipped, SkippedHostLine{
				Line: lineNo,
				Text: truncate(trimmed, 60),
				Err:  perr,
			})
			continue
		}
		good = append(good, raw)
	}
	if serr := scanner.Err(); serr != nil {
		return "", "", nil, fmt.Errorf("read known_hosts: %w", serr)
	}

	if len(skipped) == 0 {
		return path, "", nil, nil
	}

	// Rewrite the valid subset somewhere knownhosts.New can read it. The
	// source file is never modified: repairing the operator's known_hosts
	// behind their back is not this package's call.
	tf, err := os.CreateTemp("", "known_hosts_filtered_*")
	if err != nil {
		return "", "", skipped, fmt.Errorf("stage filtered known_hosts: %w", err)
	}
	// Not tf.Chmod: on Windows that sets the read-only attribute and nothing
	// else. The staged file holds public keys, so this is consistency rather
	// than secrecy -- but a filtered known_hosts left readable in %TEMP% is
	// still a list of every host this operator reaches.
	if err := privatefile.Harden(tf.Name()); err != nil {
		tf.Close()
		os.Remove(tf.Name())
		return "", "", skipped, fmt.Errorf("stage filtered known_hosts: %w", err)
	}
	for _, line := range good {
		if _, werr := tf.WriteString(line + "\n"); werr != nil {
			tf.Close()
			os.Remove(tf.Name())
			return "", "", skipped, fmt.Errorf("write filtered known_hosts: %w", werr)
		}
	}
	if cerr := tf.Close(); cerr != nil {
		os.Remove(tf.Name())
		return "", "", skipped, fmt.Errorf("write filtered known_hosts: %w", cerr)
	}
	return tf.Name(), tf.Name(), skipped, nil
}

func truncate(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n] + "..."
}
