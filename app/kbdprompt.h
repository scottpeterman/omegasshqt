// app/kbdprompt.h
//
// Recognising a keyboard-interactive question in a dial's error text, and
// getting the question back out of it.
//
// THE THIRD OF THE SAME SHAPE, after hostkeyerror.h and autherror.h. The
// library cannot call up here to ask a question while the handshake waits on
// it -- that would be a callback on a foreign thread, which capi/open.go
// explains at length -- so the flow is: let the dial fail, recognise it, ask,
// dial again with the answer supplied.
//
// UNLIKE THE OTHER TWO, this one PARSES. A host key prompt is raised from a
// fingerprint the library formats for it; a credential prompt needs only the
// target, which the tab already knows. Here the question comes from the far
// end and is the whole point -- "YubiKey for `speterman':" has to reach the
// operator verbatim, because it is what tells them which token to touch.
//
// The message shape is defined in sshcore/kbdprompt.go and asserted in
// sshcore/kbdprompt_test.go:
//
//	keyboard-interactive prompt [secret]: YubiKey for `speterman':
//
// The question is everything after the tag and its ": ", to the end. Nothing
// delimits its end because nothing safely can: real questions contain colons,
// quotes and backticks.

#ifndef OMEGA_APP_KBDPROMPT_H
#define OMEGA_APP_KBDPROMPT_H

#include <QString>

namespace omega::app {

// How many keyboard-interactive questions one tab will answer before it gives
// up and settles on the failure.
//
// Higher than the credential cap because a wrong answer here is cheap and
// common: an OTP mistyped, or a token touched a moment too late. The cap is
// only a backstop against a server that asks forever -- Cancel is the way out
// at any point, and every attempt is a fresh dial the operator chose to make.
inline constexpr int KeyboardPromptLimit = 5;

struct KeyboardPrompt {
    // False when the error is not a keyboard-interactive question at all, in
    // which case nothing else here is meaningful.
    bool valid = false;

    // What the server asked, verbatim. This is BOTH what the operator is
    // shown AND the key the answer must be filed under for the next dial, so
    // it must not be trimmed, unquoted or otherwise tidied -- the far end
    // will send the same bytes again and they have to match.
    QString question;

    // The server asked for the answer not to be echoed. Masks the field.
    // Defaults to true so that a message this fails to classify hides its
    // input rather than putting a one-time password on screen.
    bool secret = true;
};

// Splits a dial error into a question, or returns an invalid result when the
// error is something else.
KeyboardPrompt parseKeyboardPrompt(const QString &error);

}  // namespace omega::app

#endif  // OMEGA_APP_KBDPROMPT_H
