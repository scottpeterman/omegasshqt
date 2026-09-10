// app/linkstate.h
//
// What the session tree shows about a connection, and the colours for it.
//
// NOT A RENAME OF omegassh::State. qt/omegasshsession.h publishes
// { Disconnected, Connecting, Authenticating, Connected, Reconnecting, Failed }
// and is explicit that Reconnecting is not published by anything yet. Link is a
// superset, because two of the states the design asks for have no
// transport-level source at all:
//
//   Resolving   the DNS lookup ahead of the socket. sshcore does not report
//               it separately, so nothing sets this yet -- it exists so the
//               view does not have to change when something does.
//   NeedsInput  a credential or host-key prompt is open. That is the window's
//               knowledge, not the transport's, so MainWindow pushes it in
//               around the prompts it raises.
//
// The rest come from omegassh::State through linkFromState().
//
// WHY A SEPARATE ENUM AT ALL. The alternative is showing the transport's state
// directly, and it cannot express "this session is waiting for a person". That
// is the one state where the dot has to mean something different from the
// three that are merely slow, because it is the only one that will never
// resolve on its own.

#ifndef OMEGA_APP_LINKSTATE_H
#define OMEGA_APP_LINKSTATE_H

#include <QColor>
#include <QCoreApplication>
#include <QString>

#include <omegasshsession.h>

#include "theme/tokens.h"

namespace omega::app {

enum class Link {
    Idle,            // never dialled, or the tab was closed
    Resolving,       // no source yet; see above
    Connecting,
    Authenticating,
    Live,
    NeedsInput,      // a prompt is open and nothing will progress without one
    Reconnecting,    // no source yet
    Dead,            // failed, or ended
};

// The four that pulse. A pulse says "this is in flight and will change on its
// own", which is exactly what separates them from NeedsInput -- that one is
// solid because it will not.
inline bool linkIsTransient(Link l) {
    return l == Link::Resolving || l == Link::Connecting ||
           l == Link::Authenticating || l == Link::Reconnecting;
}

inline bool linkIsWarn(Link l) { return l == Link::NeedsInput; }

// The honest half of the mapping. Reconnecting is carried through even though
// nothing emits it, so the day something does, this does not need editing.
inline Link linkFromState(omegassh::State s) {
    switch (s) {
        case omegassh::State::Connecting:     return Link::Connecting;
        case omegassh::State::Authenticating: return Link::Authenticating;
        case omegassh::State::Connected:      return Link::Live;
        case omegassh::State::Reconnecting:   return Link::Reconnecting;
        case omegassh::State::Failed:         return Link::Dead;
        case omegassh::State::Disconnected:   break;
    }
    return Link::Dead;
}

// tokens.state, NOT tokens.accent. On six of the 33 shipped themes the accent
// is the same colour as body text, so a live dot painted in it is not a
// signal -- see Tokens::state and Tokens::stateIsDistinct.
inline QColor dotColor(Link l, const theme::Tokens &t) {
    const auto q = [](const theme::Rgba &c) { return QColor(c.r, c.g, c.b); };
    switch (l) {
        case Link::NeedsInput:
            return q(t.warn);
        case Link::Live:
        case Link::Resolving:
        case Link::Connecting:
        case Link::Authenticating:
        case Link::Reconnecting:
            return q(t.state);
        case Link::Idle:
        case Link::Dead:
            break;
    }
    return q(t.dead);
}

// Under 50 ms is good, up to 250 is unremarkable, above that is worth reading.
// The thresholds are round numbers on purpose: they are a reading aid, not a
// measurement, and a chip that changed colour at 47 ms would imply a precision
// a keepalive round trip does not have.
inline QColor rttColor(int ms, const theme::Tokens &t) {
    const auto q = [](const theme::Rgba &c) { return QColor(c.r, c.g, c.b); };
    if (ms < 50) return q(t.state);
    if (ms <= 250) return q(t.inkMuted);
    return q(t.warn);
}

// The word beside the dot. Section 6's "never colour alone" is not politeness
// here: on crt-amber, crt-green and cyberpunk nothing in the palette can carry
// the state in colour at all -- Tokens::stateIsDistinct is false -- so on those
// three the word is the only channel there is.
inline QString linkWord(Link l) {
    switch (l) {
        case Link::Idle:           return QCoreApplication::translate("Link", "idle");
        case Link::Resolving:      return QCoreApplication::translate("Link", "resolving");
        case Link::Connecting:     return QCoreApplication::translate("Link", "connecting");
        case Link::Authenticating: return QCoreApplication::translate("Link", "authenticating");
        case Link::Live:           return QCoreApplication::translate("Link", "live");
        case Link::NeedsInput:     return QCoreApplication::translate("Link", "needs input");
        case Link::Reconnecting:   return QCoreApplication::translate("Link", "reconnecting");
        case Link::Dead:           return QCoreApplication::translate("Link", "disconnected");
    }
    return {};
}

}  // namespace omega::app

#endif  // OMEGA_APP_LINKSTATE_H
