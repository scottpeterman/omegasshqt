# Keyboard-interactive (OTP) — what works, what is broken, what is next

Status at hand-off. Everything below was driven through the GUI against real
SSH servers, not reasoned about, except where it says otherwise.

## Why it is built this way

`sshcore.Config.AuthPrompt` has always existed and works: a caller that imports
`sshcore` directly can answer keyboard-interactive questions live. Omega cannot.
It reaches the library through the C boundary, and `capi/open.go` refuses to
marshal a prompt across it — a callback would have to run on a foreign thread
while the handshake blocks on it.

So this uses the same fail-ask-redial shape the host key and credential flows
already use:

1. The dial fails on a question nobody could answer.
2. The error carries the question, tagged `[secret]` or `[visible]`.
3. The UI parses it, asks the operator, and dials again with the answer in
   `Config.KeyboardAnswers`, keyed by the question verbatim.

`sshcore/kbdprompt.go` defines the message shape; `app/kbdprompt.cpp` parses it.

### The one thing this cannot ever serve

A challenge whose answer is bound to the connection that asked it — HMAC
challenge-response with a server-issued nonce. The second dial gets a fresh
nonce and the answer to the first is worthless. It fails cleanly every time
rather than misbehaving, but no amount of work on this path fixes it. That
needs a real callback across the C boundary, which is the thing `capi/open.go`
is deliberately avoiding.

Anything the operator can produce independently is fine: Yubico OTP, TOTP,
SecurID, SMS codes, Duo push, static PINs, menu-style questions.

## Verified working

| Case | Result |
| --- | --- |
| One secret question (`YubiKey for \`speterman':`) | asked verbatim, answered, authenticated |
| Password **and** OTP in one round | only the OTP is asked; password auto-answered from config |
| Visible question (`echo=true`) | rendered unmasked |
| Two separate rounds | both asked in turn, answers accumulate across dials |
| Cancel | tab settles `(closed)` with the question on the overlay |

Nothing in the logic mentions YubiKey or OTP. The question string is opaque
end to end.

## BROKEN: a wrong answer routes to the credentials dialog

**Reproduce:** connect to a server that asks for an OTP, answer it wrongly.

**Expected:** the question is asked again.

**Actual:** the username/password credentials dialog appears, saying "the far
end refused these credentials". Neither field can fix a mistyped code. The
operator has to cancel and start over.

**Why.** Both classifiers are behaving correctly in isolation. On the first
dial there is no answer, so the library reports the question and
`offerKeyboardPrompt()` claims it. On the retry an answer IS supplied, it is
rejected, and the failure comes back as x/crypto's `unable to authenticate` —
a genuine credential rejection, which `offerCredentialPrompt()` correctly
claims. Neither knows a keyboard answer was just submitted and refused.

**Fix.** In `TerminalTab::onStateChanged`, before the credential branch: if
this tab has already answered a keyboard question on this dial
(`keyboardPrompts_ > 0` and `config_.keyboardAnswers` is non-empty), an auth
failure means that answer was wrong. Re-ask the question rather than offering
credentials. The failing answer must be cleared from `config_.keyboardAnswers`
before the re-ask, or the next dial resubmits the same dead code.

The question text to re-ask is not in the rejection message, so the tab has to
keep the last question it asked — a `lastKeyboardQuestion_` member set in
`offerKeyboardPrompt()` and cleared on success.

Roughly twenty lines. Untested because it was found at the end of a session.

## Untested

- **The 5-attempt cap** (`KeyboardPromptLimit`). Unreachable until the routing
  bug above is fixed: a wrong answer never comes back as a keyboard prompt, so
  the counter never advances past 1 in practice.
- **Multi-question rounds beyond two.** Two works; three should, by the same
  accumulate-don't-replace mechanism, but has not been run.

## Cosmetic

The attempt chip reads `attempt 2 of 5` on a *successful* two-round login,
because a second question legitimately costs a second dial. It implies
something failed when nothing did. The counter should count rejected answers,
not dials — which is the same distinction the routing fix above turns on.

## Test servers

`/tmp` harnesses, not in the repo, but trivial to rebuild — a Go SSH server
with `KeyboardInteractiveCallback` on four ports:

- one secret question, matching the field report verbatim
- password + OTP in a single round
- a visible (`echo=true`) question
- two separate rounds

Worth committing as a fixture under `tests/` if this gets further work: none
of the above can be exercised against ordinary sshd without a PAM stack.
