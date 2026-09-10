// app/sessionribbon.h
//
// The strip above the terminal: what this tab is connected to, and how.
//
// INSIDE THE TAB, NOT UNDER THE TAB BAR. The mockup shows it spanning the page
// below the tab strip, and there are two ways to build that. Splitting the
// QTabWidget into a QTabBar plus a QStackedWidget would let one ribbon sit
// between them -- and would mean reimplementing tab close, tab moves, the
// current-index plumbing and the context menu that MainWindow already has
// working. Putting a ribbon in each tab lands in the same place on screen,
// makes the per-tab state per-tab by construction, and touches nothing else.
//
// WHAT IS ON IT IS LIMITED BY WHAT EXISTS. The design also asks for the
// negotiated cipher, the host key algorithm and whether the key is pinned.
// None of the three is available:
//
//   cipher, host key algorithm   x/crypto/ssh negotiates both and sshcore
//                                does not surface either. Reaching them means
//                                a field on the session, an event through
//                                capi, and a signal on OmegaSshSession.
//   host key pinned              there is no query for it. The fingerprint is
//                                only produced on the error path, in
//                                app/hostkeyerror.h, when the key is UNKNOWN.
//                                Answering "is this one pinned" means either a
//                                new C API over sshcore's known_hosts filter,
//                                or a second known_hosts parser here that
//                                would disagree with the first one about
//                                hashed hosts.
//
// The preview harness faked the third with host.startsWith("dfw-core"). This
// shows nothing rather than something untrue: a ribbon that says "host key
// pinned" when nobody checked is worse than a ribbon that does not mention it.

#ifndef OMEGA_APP_SESSIONRIBBON_H
#define OMEGA_APP_SESSIONRIBBON_H

#include <QFrame>

#include <omegasshsession.h>

#include "app/linkstate.h"
#include "theme/tokens.h"

class QLabel;
class QPushButton;

namespace omega::app {

class SessionRibbon : public QFrame {
    Q_OBJECT

public:
    explicit SessionRibbon(QWidget *parent = nullptr);

    // The unchanging half: what was dialled. Called once per dial, because a
    // re-dial of the same config is still the same session.
    void setSession(const QString &displayName, const omegassh::Config &config);

    // The moving half. The word beside the dot is not decoration -- on three
    // of the 33 shipped themes nothing in the palette can carry the state in
    // colour at all, and there the word is the only channel. See
    // Tokens::stateIsDistinct.
    void setLink(Link link);

    // Shown only when there is something to reconnect to.
    void setReconnectVisible(bool visible);

    void setTokens(const theme::Tokens &tokens);

signals:
    void reconnectRequested();

protected:
    // The dot is painted rather than styled: its colour is a value, not a
    // role, and it pulses.
    void paintEvent(QPaintEvent *event) override;

private:
    void relayout();

    QLabel *name_ = nullptr;
    QLabel *target_ = nullptr;
    QLabel *credential_ = nullptr;
    QLabel *transport_ = nullptr;
    QLabel *state_ = nullptr;
    QPushButton *reconnect_ = nullptr;

    Link link_ = Link::Idle;
    theme::Tokens tokens_ = theme::specTokens();
};

}  // namespace omega::app

#endif  // OMEGA_APP_SESSIONRIBBON_H
