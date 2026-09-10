// app/autherror.h
//
// Recognising a credential rejection in a dial's error text.
//
// THE SAME SHAPE AS hostkeyerror.h, deliberately. A prompt callback cannot
// cross the C boundary, so the flow is: let the dial fail, recognise the
// failure, ask, re-dial. That pattern already exists here for host keys and
// this is the second thing to use it.
//
// The marker comes from omegassh_auth_failed_marker() at runtime rather than
// being copied into this file, for the reason hostkeyerror.h gives: a literal
// here would be a second definition of a load-bearing string, and the failure
// mode of the two drifting apart is a prompt that silently stops appearing.
//
// WHAT DIFFERS FROM THE HOST KEY CASE, and it is the whole reason this is a
// separate flow rather than a parameter on that one:
//
//   - The host key question is asked ONCE per tab. Answering it changes the
//     policy, and if the answer is no, asking again would be nagging.
//   - A rejection is asked up to AuthPromptLimit times. Answering it changes
//     the credentials, and a mistyped password is worth a second go -- which
//     is what sshd itself does.
//
//   - A host key answer is self-contained, so TerminalTab raises that dialog
//     itself. A credential answer needs the vault, which the tab deliberately
//     does not have (see terminaltab.h -- the tab never learns where its
//     config came from). So the tab reports and the window asks.
//
// NOTHING IS PARSED OUT OF THE MESSAGE. The host key dialog needs a
// fingerprint, a key type and a path, so parseHostKeyError picks them out.
// The credential prompt needs a target to put in its title, and the tab
// already knows that from its own config -- reading it back out of an error
// string would be the long way round to something already in hand.

#ifndef OMEGA_APP_AUTHERROR_H
#define OMEGA_APP_AUTHERROR_H

#include <QString>

namespace omega::app {

// How many times one tab will re-ask before it settles on the failure.
//
// Three, matching sshd's default MaxAuthTries, so an operator who is used to
// three tries at a console gets three here. The cap exists because the loop
// is otherwise unbounded: every answer produces another dial, and a dial that
// keeps being refused would keep re-opening the modal. Cancel breaks out at
// any point; this is only the backstop for someone who keeps pressing
// Connect.
inline constexpr int AuthPromptLimit = 3;

// True when the dial failed because the far end refused the credentials.
//
// False for a host key failure, a refused connection, a name that does not
// resolve and a timeout -- that guarantee is the library's, asserted in
// sshcore/authfail_test.go, not something re-derived here.
bool isAuthFailure(const QString &error);

}  // namespace omega::app

#endif  // OMEGA_APP_AUTHERROR_H
