// integration/omegassh_anytermqt.h
//
// Wiring an omegassh session to an anytermqt terminal widget.
//
// This header is the only place the two projects meet, and it is deliberately
// NOT part of the omegassh_qt library: that target links QtCore alone, and
// pulling in a widget would cost every headless consumer a dependency on
// QtWidgets. Include this from an application that already has both.
//
// The function is a copy of qtpyte::attach with the transport swapped, and
// exists for the same reason that one does: there are exactly three
// connections, getting one wrong produces a terminal that looks almost right,
// and the third is the one people forget.

#ifndef OMEGASSH_INTEGRATION_ANYTERMQT_H
#define OMEGASSH_INTEGRATION_ANYTERMQT_H

#include <omegasshsession.h>

#include <qtpyte/terminalwidget.h>

namespace omegassh {

// Connect a session to a widget: bytes in, bytes out, and size changes.
//
// Identical in shape to qtpyte::attach(TerminalWidget*, PtySession*), because
// OmegaSshSession carries the same signal names as qtpyte::PtySession. A
// caller swapping a local shell for an SSH session changes the type and
// nothing else.
inline void attach(qtpyte::TerminalWidget *widget, OmegaSshSession *session) {
    if (!widget || !session) {
        return;
    }
    QObject::connect(session, &OmegaSshSession::dataReceived,
                     widget, &qtpyte::TerminalWidget::feed);
    QObject::connect(widget, &qtpyte::TerminalWidget::dataReady,
                     session, &OmegaSshSession::write);
    // The third one is the one that gets forgotten. Without it the emulator
    // knows the new shape and the remote tty does not, so a full-screen
    // application keeps drawing to the old size after a window resize.
    QObject::connect(widget, &qtpyte::TerminalWidget::resized,
                     session, &OmegaSshSession::resize);
}

}  // namespace omegassh

#endif  // OMEGASSH_INTEGRATION_ANYTERMQT_H
