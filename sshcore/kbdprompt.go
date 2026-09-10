// sshcore/kbdprompt.go
// Reporting a keyboard-interactive question that nobody was there to answer.
//
// THE THIRD USER OF THE SAME PATTERN, after hostkey.go and authfail.go. A
// prompt callback cannot cross the C boundary without running on a foreign
// thread (see capi/open.go), so a caller on the far side cannot answer a
// question while the handshake is waiting on it. What it can do is let the
// dial fail, read the question out of the error, ask its own user, and dial
// again with the answer in Config.KeyboardAnswers.
//
// That works because the questions this is for -- a Yubico OTP, a TOTP code --
// are answered by something the operator holds, not by a nonce the server
// issued for this particular connection. A NONCE-BASED challenge cannot be
// answered this way: the redial gets a new one, and the answer to the old one
// is worthless. Nothing here can detect that difference, and a caller pointing
// this at a challenge-response scheme will see it fail every time rather than
// misbehave quietly.
//
// THE MESSAGE IS PARSED, so its shape is load-bearing:
//
//	keyboard-interactive prompt [secret]: YubiKey for `speterman':
//	<--------- marker --------> <-tag-->  <------- question ------>
//
// The question is everything after the tag and its colon-space, to the end of
// the string. Last on purpose: a question can contain colons, quotes and
// backticks -- the one above has two of the three -- and anything that tried
// to delimit the end of it would be wrong on the first prompt that used the
// delimiter.

package sshcore

// KeyboardPromptMarker opens the message for a keyboard-interactive question
// that could not be answered. Matched by a UI to decide whether to ask, so
// renaming it is a compile error here rather than a prompt that silently stops
// appearing.
const KeyboardPromptMarker = "keyboard-interactive prompt"

// The two tags. Secret means the server asked for the input not to be echoed,
// which is what a UI keys its masking off -- an OTP is secret, "Enter your
// username" is not.
const (
	KeyboardPromptSecretTag  = "[secret]"
	KeyboardPromptVisibleTag = "[visible]"
)

func echoTag(echos []bool, i int) string {
	// echos is documented to parallel questions, but it arrives from the far
	// end and a short slice would panic. A question we cannot classify is
	// treated as secret: masking something that did not need it is a
	// cosmetic problem, and the reverse puts an OTP on screen.
	if i >= len(echos) || !echos[i] {
		return KeyboardPromptSecretTag
	}
	return KeyboardPromptVisibleTag
}
