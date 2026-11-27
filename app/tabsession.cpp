// app/tabsession.cpp

#include "app/tabsession.h"

#include <QDir>
#include <QFileInfo>
#include <QTimer>

#include <qtpyte/ptysession.h>
#include <qtpyte/terminalwidget.h>

#include "app/localshell.h"

namespace omega::app {

TabSession::TabSession(QObject *parent) : QObject(parent) {
    ssh_ = new omegassh::OmegaSshSession(this);

    // Forwarded unconditionally. The network session only produces events
    // after it has been started, and it is only started for a network
    // transport, so nothing from it can arrive while a local shell is the
    // backend.
    connect(ssh_, &omegassh::OmegaSshSession::dataReceived, this,
            &TabSession::dataReceived);
    connect(ssh_, &omegassh::OmegaSshSession::stateChanged, this,
            &TabSession::stateChanged);
    connect(ssh_, &omegassh::OmegaSshSession::finished, this,
            &TabSession::finished);
    connect(ssh_, &omegassh::OmegaSshSession::errorOccurred, this,
            &TabSession::errorOccurred);
    connect(ssh_, &omegassh::OmegaSshSession::interactionRequired, this,
            &TabSession::interactionRequired);
}

TabSession::~TabSession() {
    // Nothing is listening any more, and the tab that was is being destroyed
    // around us: end the child without announcing it.
    if (pty_) {
        disconnect(pty_, nullptr, this, nullptr);
        pty_->terminate();
    }
}

bool TabSession::start(const omegassh::Config &config) {
    if (running()) {
        const QString message = tr("session is already running");
        if (transport_ == omegassh::Transport::Local) localError_ = message;
        emit errorOccurred(message);
        return false;
    }
    transport_ = config.transport;
    if (config.transport == omegassh::Transport::Local) {
        return startLocal(config);
    }
    dropPty();
    return ssh_->start(config);
}

bool TabSession::running() const {
    if (transport_ == omegassh::Transport::Local) {
        return pty_ && !localFinishedEmitted_;
    }
    return ssh_->running();
}

QString TabSession::error() const {
    return transport_ == omegassh::Transport::Local ? localError_ : ssh_->error();
}

omegassh::State TabSession::state() const {
    return transport_ == omegassh::Transport::Local ? localState_ : ssh_->state();
}

QString TabSession::summary() const {
    return transport_ == omegassh::Transport::Local ? localSummary_ : ssh_->summary();
}

int TabSession::exitCode() const {
    return transport_ == omegassh::Transport::Local ? localExitCode_
                                                    : ssh_->exitCode();
}

void TabSession::write(const QByteArray &data) {
    if (transport_ == omegassh::Transport::Local) {
        if (pty_ && !localFinishedEmitted_ && !data.isEmpty()) pty_->write(data);
        return;
    }
    ssh_->write(data);
}

void TabSession::resize(int cols, int rows) {
    if (transport_ == omegassh::Transport::Local) {
        if (pty_ && !localFinishedEmitted_ && cols > 0 && rows > 0) {
            pty_->resize(cols, rows);
        }
        return;
    }
    ssh_->resize(cols, rows);
}

void TabSession::terminate() {
    if (transport_ == omegassh::Transport::Local) {
        // PtySession::terminate() reports the exit synchronously, once, on
        // every platform -- so Disconnected and finished() have both been
        // emitted by the time this returns, the same as the network session.
        if (pty_ && !localFinishedEmitted_) pty_->terminate();
        // A child that ignored the hang-up and could not be reaped still
        // counts as ended here: the tab is done with it either way.
        onLocalFinished(localExitCode_);
        return;
    }
    ssh_->terminate();
}

bool TabSession::startLocal(const omegassh::Config &config) {
    dropPty();
    localError_.clear();
    localExitCode_ = -1;

    const ShellCommand shell = resolveShell(config.localCommand);
    if (!shell.ok()) {
        localError_ = shell.error;
        emit errorOccurred(localError_);
        return false;
    }

    QString dir = config.localDirectory.trimmed();
    if (dir.startsWith(QLatin1Char('~'))) dir.replace(0, 1, QDir::homePath());
    if (dir.isEmpty()) dir = QDir::homePath();
    if (!QFileInfo(dir).isDir()) {
        localError_ = tr("start folder not found: %1").arg(dir);
        emit errorOccurred(localError_);
        return false;
    }

    // TERM is what the emulator speaks. COLORTERM tells the programs that read
    // it -- vim, bat, ls on some systems -- that 24-bit colour is on offer,
    // which a TERM name alone cannot say.
    const QStringList env = {
        QStringLiteral("TERM=") + (config.term.isEmpty()
                                       ? QStringLiteral("xterm-256color")
                                       : config.term),
        QStringLiteral("COLORTERM=truecolor"),
        QStringLiteral("TERM_PROGRAM=Omega"),
    };

    pty_ = qtpyte::PtySession::create(this);
    connect(pty_, &qtpyte::PtySession::dataReceived, this,
            &TabSession::dataReceived);
    connect(pty_, &qtpyte::PtySession::finished, this,
            &TabSession::onLocalFinished);

    // PtySession takes no working directory, and both of its backends start
    // the child in this process's current one (forkpty inherits it;
    // CreateProcessW is given none). So the current directory is moved for
    // the length of the call and put back. start() is synchronous and this is
    // the GUI thread, so nothing else can observe the change.
    // QTPYTE_PTY_DEBUG=1: what Omega resolved, ahead of what the pty layer
    // spawned. Both land in the terminal, dim.
    if (qEnvironmentVariableIntValue("QTPYTE_PTY_DEBUG") != 0) {
        const QString line =
            QStringLiteral("[omega] session '%1' -> program %2 args [%3] dir %4")
                .arg(config.localCommand, shell.program,
                     shell.args.join(QStringLiteral(", ")), dir);
        emit dataReceived(QByteArrayLiteral("\x1b[2m") + line.toUtf8() +
                          QByteArrayLiteral("\x1b[0m\r\n"));
    }

    const QString previous = QDir::currentPath();
    QDir::setCurrent(dir);
    localFinishedEmitted_ = false;
    const bool ok = pty_->start(shell.program, shell.args, env,
                                qMax(1, config.cols), qMax(1, config.rows));
    QDir::setCurrent(previous);

    if (!ok) {
        localError_ = pty_->error().isEmpty() ? tr("could not start %1")
                                                    .arg(shell.program)
                                              : pty_->error();
        localFinishedEmitted_ = true;
        dropPty();
        emit errorOccurred(localError_);
        return false;
    }

    localSummary_ = tr("%1 (local)").arg(shellDisplayName(config.localCommand));

    // Delivered from the event loop, not from inside start(), so the caller
    // sees the same order a network dial gives it: start() returns, then the
    // states arrive. A shell that has already exited by then -- a bad
    // argument, `exit` in a profile -- has reported Disconnected, and is not
    // then reported Connected after it.
    QPointer<qtpyte::PtySession> run = pty_;
    QTimer::singleShot(0, this, [this, run] {
        if (!run || run != pty_ || localFinishedEmitted_) return;
        setLocalState(omegassh::State::Connecting);
        setLocalState(omegassh::State::Connected);
    });
    return true;
}

void TabSession::setLocalState(omegassh::State s) {
    if (localState_ == s) return;
    localState_ = s;
    emit stateChanged(s);
}

void TabSession::onLocalFinished(int code) {
    if (localFinishedEmitted_) return;
    localFinishedEmitted_ = true;
    localExitCode_ = code;
    setLocalState(omegassh::State::Disconnected);
    emit finished(code);
}

void TabSession::dropPty() {
    if (!pty_) return;
    disconnect(pty_, nullptr, this, nullptr);
    if (!localFinishedEmitted_) pty_->terminate();
    pty_->deleteLater();
    pty_ = nullptr;
}

void attach(qtpyte::TerminalWidget *widget, TabSession *session) {
    if (!widget || !session) return;
    QObject::connect(session, &TabSession::dataReceived, widget,
                     &qtpyte::TerminalWidget::feed);
    QObject::connect(widget, &qtpyte::TerminalWidget::dataReady, session,
                     &TabSession::write);
    QObject::connect(widget, &qtpyte::TerminalWidget::resized, session,
                     &TabSession::resize);
}

}  // namespace omega::app
