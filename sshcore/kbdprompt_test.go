// sshcore/kbdprompt_test.go
//
// The message shape here is an interface, not a diagnostic: a UI splits it to
// get the question it puts in front of a person. A reworded message is a
// prompt that stops appearing, or worse, one that appears with a mangled
// question. So the shape is asserted rather than left to care.

package sshcore

import (
	"errors"
	"strings"
	"testing"
)

// The question from the field: contains a colon, a backtick and a quote. Any
// attempt to delimit the END of the question would break on one of them,
// which is why it runs to the end of the string.
const awkwardQuestion = "YubiKey for `speterman': "

func TestEchoTag(t *testing.T) {
	cases := []struct {
		name  string
		echos []bool
		i     int
		want  string
	}{
		{"secret", []bool{false}, 0, KeyboardPromptSecretTag},
		{"visible", []bool{true}, 0, KeyboardPromptVisibleTag},
		{"second question", []bool{true, false}, 1, KeyboardPromptSecretTag},
		// A short slice must not panic, and must fail closed: an unmasked OTP
		// is worse than a masked username.
		{"index past the end", []bool{}, 0, KeyboardPromptSecretTag},
		{"nil echos", nil, 3, KeyboardPromptSecretTag},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if got := echoTag(tc.echos, tc.i); got != tc.want {
				t.Fatalf("echoTag(%v, %d) = %s, want %s",
					tc.echos, tc.i, got, tc.want)
			}
		})
	}
}

// The split a UI performs, done here so a change to the message breaks this
// test rather than the UI.
func TestMessageIsSplittable(t *testing.T) {
	cfg := &Config{}
	challenge := keyboardInteractive(cfg)

	_, err := challenge("speterman", "", []string{awkwardQuestion}, []bool{false})
	if err == nil {
		t.Fatal("a question with no handler and no pre-supplied answer must fail")
	}
	msg := err.Error()

	if !strings.HasPrefix(msg, KeyboardPromptMarker) {
		t.Fatalf("message does not open with the marker: %q", msg)
	}
	if !strings.Contains(msg, KeyboardPromptSecretTag) {
		t.Fatalf("echo=false did not produce the secret tag: %q", msg)
	}

	// Everything after the tag and its ": " is the question, verbatim.
	tail := msg[strings.Index(msg, KeyboardPromptSecretTag)+
		len(KeyboardPromptSecretTag):]
	question := strings.TrimPrefix(tail, ": ")
	if question != awkwardQuestion {
		t.Fatalf("question did not survive the round trip:\n got %q\nwant %q",
			question, awkwardQuestion)
	}
}

// A pre-supplied answer is the whole point: it is what the second dial uses.
func TestKeyboardAnswersAreUsed(t *testing.T) {
	cfg := &Config{
		KeyboardAnswers: map[string]string{awkwardQuestion: "ccccccbhtrgudvvv"},
	}
	answers, err := keyboardInteractive(cfg)(
		"speterman", "", []string{awkwardQuestion}, []bool{false})
	if err != nil {
		t.Fatalf("a pre-supplied answer should not fail: %v", err)
	}
	if len(answers) != 1 || answers[0] != "ccccccbhtrgudvvv" {
		t.Fatalf("wrong answer: %v", answers)
	}
}

// A pre-supplied answer beats the password auto-answer. The question below
// contains "password", so without the precedence the config password would
// win and the answer the operator actually typed would be discarded.
func TestPreSuppliedAnswerBeatsPasswordHeuristic(t *testing.T) {
	const q = "One-time password: "
	cfg := &Config{
		Password:        "the-account-password",
		KeyboardAnswers: map[string]string{q: "123456"},
	}
	answers, err := keyboardInteractive(cfg)(
		"speterman", "", []string{q}, []bool{false})
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if answers[0] != "123456" {
		t.Fatalf("password heuristic overrode the supplied answer: %q", answers[0])
	}
}

// An unanswered question must never look like a credential rejection: the
// credential dialog cannot answer an OTP, and showing it would be a box the
// operator has to dismiss before reading what was actually asked.
func TestKeyboardPromptIsNotAnAuthFailure(t *testing.T) {
	msg := KeyboardPromptMarker + " " + KeyboardPromptSecretTag + ": " + awkwardQuestion
	if IsAuthFailure(errors.New(msg)) {
		t.Fatal("a keyboard-interactive prompt classified as an auth failure")
	}
	// And wrapped the way the transport wraps it.
	wrapped := errors.New("ssh: handshake failed: " + msg)
	if IsAuthFailure(wrapped) {
		t.Fatal("wrapped keyboard-interactive prompt classified as an auth failure")
	}
}

func TestMarkersDoNotOverlap(t *testing.T) {
	all := []string{KeyboardPromptMarker, UnknownHostKeyMarker, AuthFailedMarker}
	for i, a := range all {
		for j, b := range all {
			if i == j {
				continue
			}
			if strings.Contains(a, b) {
				t.Fatalf("marker %q contains %q; one message could match both", a, b)
			}
		}
	}
}
