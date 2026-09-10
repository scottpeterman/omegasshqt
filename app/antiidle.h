// app/antiidle.h
//
// The anti-idle keystroke: what to send, and when sending it is safe.
//
// Distinct from keepalive, and the distinction is the whole reason this
// exists. TCP keepalive holds the PATH open and tells the client when the far
// end has gone; it is set on the socket in sshcore/dial.go and is nobody's
// setting here. What it cannot do is stop a device deciding the operator has
// left: `exec-timeout` and its equivalents count terminal INPUT, not packets,
// so no keepalive at any layer resets one. Only a keystroke does.
//
// Which is why this lives in the tab rather than in the transport. It is
// bytes written to the session, so it works identically over ssh, telnet and
// serial -- and serial is where it earns its keep, since a console line has
// its own timeout and no keepalive concept whatsoever.
//
// WHY THE BYTES ARE A SETTING. There is no keystroke that is a no-op
// everywhere:
//
//   Backspace       The common choice, and a no-op at an idle prompt with
//                   nothing to erase. The case it is not: an operator
//                   half-types a command and is called away, and this deletes
//                   the last character off their line. They come back to
//                   `show ru` and read it as their own typo.
//   Space+Backspace Net zero on an empty line AND on a half-typed one. Costs
//                   a stray space on a far end that does not process BS.
//   NUL             Ignored by most shells and by IOS. Some terminal servers
//                   pass it through to the device unchanged.
//   Custom          Anything else, as hex. A device that wants a bare CR is
//                   not hypothetical, but it is also a device where this
//                   feature submits an empty command every interval.
//
// Backspace is the default because it is the convention, not because it is
// the safest -- Space+Backspace is one setting away and has no half-typed-line
// case. Both are off until switched on.
//
// WHY HEX RATHER THAN A STRING for the custom form: control characters typed
// into a text field produce a config file nobody can read back, and a
// trailing space in one is invisible and load-bearing.

#ifndef OMEGA_APP_ANTIIDLE_H
#define OMEGA_APP_ANTIIDLE_H

#include <QByteArray>
#include <QString>

namespace omega::app {

enum class AntiIdleKeystroke {
    Backspace,       // 0x08
    SpaceBackspace,  // 0x20 0x08
    Nul,             // 0x00
    Custom,          // whatever AntiIdleConfig::custom holds
};

struct AntiIdleConfig {
    bool enabled = false;

    // Seconds of silence -- nothing SENT -- before a keystroke goes out. Not
    // a fixed tick: what a device's idle timer counts is input from this end,
    // so the clock restarts on every write, including each line of a paced
    // paste. A fixed ticker would fire in the middle of a four-minute paste.
    int seconds = 60;

    AntiIdleKeystroke keystroke = AntiIdleKeystroke::Backspace;

    // Raw bytes for AntiIdleKeystroke::Custom. Parsed from hex in the config
    // file; empty means nothing is sent, which disables the feature as surely
    // as clearing `enabled`.
    QByteArray custom;
};

// The bytes this config sends. Empty for a Custom with nothing in it, which
// callers must treat as "do not fire" rather than "send zero bytes" -- a
// zero-length write still costs a round trip on some transports and resets
// nothing on any of them.
QByteArray antiIdleBytes(const AntiIdleConfig &config);

// Config-file spellings. Round-trips: keystrokeFromName(keystrokeName(k)) == k
// for every k. An unrecognised name yields the default rather than failing the
// file, matching how AppSettings treats a field of the wrong type.
QString keystrokeName(AntiIdleKeystroke keystroke);
AntiIdleKeystroke keystrokeFromName(const QString &name, bool *known = nullptr);

// Hex with no separators or prefix -- "08", "2008", "0d". Parsing refuses an
// odd length and any non-hex digit, returning empty, because a custom
// keystroke that silently loses its last nibble is worse than one that
// visibly does nothing.
QString bytesToHex(const QByteArray &bytes);
QByteArray hexToBytes(const QString &hex);

// Whether a keystroke may be sent right now. Every argument is a reason NOT
// to, and each one has cost somebody a session somewhere:
//
//   connected        Nothing to write to otherwise, and a dial in flight is
//                    not idle.
//   alternateScreen  vi, htop, a pager. A backspace deletes a character for
//                    real in an editor, and any key at a `--More--` prompt
//                    advances or aborts output somebody is reading. This is
//                    the one that needs qtpyte::TerminalWidget::
//                    alternateScreen().
//   pasting          A paced paste is writing continuously; the idle clock
//                    should never have run down, and firing into the middle
//                    of one corrupts a line.
bool antiIdleAllowed(const AntiIdleConfig &config, bool connected,
                     bool alternateScreen, bool pasting);

}  // namespace omega::app

#endif  // OMEGA_APP_ANTIIDLE_H
