// app/tabsession.h
//
// The session behind one tab: an omegassh network session (ssh, telnet,
// serial), or a local shell. The tab holds one of these for its whole life and
// never learns which it is driving.
//
// WHY A WRAPPER RATHER THAN AN INTERFACE. The tab makes one session in its
// constructor, before any config exists, and redials it many times -- host-key
// re-dial, credential retry, reconnect -- connecting its signals once. So the
// choice of backend has to be made at start(), not at construction, and the
// signal surface the tab connected to has to stay the same object throughout.
// This class is that object; start() routes to whichever backend the config's
// transport names.
//
// THE SURFACE IS OmegaSshSession's, name for name, because the tab was written
// against it and every one of its state rules still holds. A local shell
// reports through the same states:
//
//   start()          Connecting, then Connected once the child is running --
//                    delivered from the event loop, like a dial, never from
//                    inside start(), so a caller can finish wiring first.
//   child exits      Disconnected, then finished(exit code).
//   terminate()      Disconnected and finished(), synchronously, exactly once.
//   won't start      start() returns false with the reason in error(); no
//                    states at all, the same as a refused network config.
//
// Never Authenticating, Reconnecting or Failed, and never interactionRequired:
// a local child has nothing to authenticate and nowhere to fail mid-dial.

#ifndef OMEGA_APP_TABSESSION_H
#define OMEGA_APP_TABSESSION_H

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>

#include <omegasshsession.h>

namespace qtpyte {
class PtySession;
class TerminalWidget;
}  // namespace qtpyte

namespace omega::app {

class TabSession : public QObject {
    Q_OBJECT

public:
    explicit TabSession(QObject *parent = nullptr);
    ~TabSession() override;

    bool start(const omegassh::Config &config);

    bool running() const;
    QString error() const;
    omegassh::State state() const;
    omegassh::Transport transport() const { return transport_; }
    QString summary() const;
    int exitCode() const;

public slots:
    void write(const QByteArray &data);
    void resize(int cols, int rows);
    void terminate();

signals:
    void dataReceived(const QByteArray &data);
    void stateChanged(omegassh::State state);
    void finished(int exitCode);
    void errorOccurred(const QString &message);
    void interactionRequired(const QString &question, bool echo);

private:
    bool startLocal(const omegassh::Config &config);
    void setLocalState(omegassh::State s);
    void onLocalFinished(int code);
    void dropPty();

    omegassh::OmegaSshSession *ssh_ = nullptr;

    // Present only while a local shell is, or was last, running. Replaced on
    // every local start(): a PtySession runs one child and is spent after it.
    qtpyte::PtySession *pty_ = nullptr;

    omegassh::Transport transport_ = omegassh::Transport::Ssh;

    // Local-only state. The network backend keeps its own.
    omegassh::State localState_ = omegassh::State::Disconnected;
    QString localError_;
    QString localSummary_;
    int localExitCode_ = -1;
    bool localFinishedEmitted_ = true;
};

// The three connections, the same three omegassh::attach makes for a network
// session: bytes in, bytes out, size changes.
void attach(qtpyte::TerminalWidget *widget, TabSession *session);

}  // namespace omega::app

#endif  // OMEGA_APP_TABSESSION_H
