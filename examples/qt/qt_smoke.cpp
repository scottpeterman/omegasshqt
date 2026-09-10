// examples/qt/qt_smoke.cpp
//
// End-to-end check of the Qt wrapper against a live SSH host. QtCore only --
// no widgets, no display, so it runs on a build box or in CI.
//
// Proves the part that matters: every byte and every state change arrives on
// the Qt event loop thread, with no marshalling in the caller.
//
// Two runs, because the wrapper now has two outcomes worth checking and they
// are reported in different places. A dial that fails does NOT come back from
// start() -- start() returns as soon as the dial has begun -- so the failure
// arrives as stateChanged(Failed), errorOccurred and finished(-1), and a
// caller that only checked the return value would sit forever on a spinner.
// That is the behavioural change this file exists to pin down.
//
// Usage:
//   qt_smoke <host> <port> <user> <key-path>

#include <omegasshsession.h>

#include <QCoreApplication>
#include <QThread>
#include <QTimer>
#include <QVector>
#include <cstdio>

namespace {

int failures = 0;

void check(bool cond, const char *what, const QString &detail = {}) {
    if (cond) {
        printf("[ ok ] %s\n", what);
        return;
    }
    printf("[FAIL] %s%s%s\n", what, detail.isEmpty() ? "" : " -- ",
           detail.isEmpty() ? "" : qPrintable(detail));
    failures++;
}

// Verifies the promise the whole notifier design exists to keep.
void requireQtThread(QThread *expected, const char *what) {
    if (QThread::currentThread() != expected) {
        printf("[FAIL] %s arrived on a foreign thread\n", what);
        failures++;
        QCoreApplication::exit(1);
    }
}

const char *stateName(omegassh::State s) {
    switch (s) {
        case omegassh::State::Connecting:     return "connecting";
        case omegassh::State::Authenticating: return "authenticating";
        case omegassh::State::Connected:      return "connected";
        case omegassh::State::Reconnecting:   return "reconnecting";
        case omegassh::State::Failed:         return "failed";
        case omegassh::State::Disconnected:   break;
    }
    return "disconnected";
}

QString trail(const QVector<omegassh::State> &states) {
    QStringList parts;
    for (omegassh::State s : states) parts << QString::fromLatin1(stateName(s));
    return parts.join(QStringLiteral(" -> "));
}

// ---------------------------------------------------------------------------
// A dial that cannot succeed.
//
// 10.255.255.1 is chosen to hang rather than refuse: a refused connection
// comes back in milliseconds and would not prove that the caller's thread
// stayed free while it waited. With a two-second timeout this run takes about
// two seconds, and the event loop is answering throughout -- which is the
// whole reason the dial moved off the calling thread.
// ---------------------------------------------------------------------------
void runFailedDial(QThread *qtThread) {
    printf("\nfailed dial\n");

    omegassh::Config cfg;
    cfg.host = QStringLiteral("10.255.255.1");
    cfg.port = 22;
    cfg.username = QStringLiteral("labuser");
    cfg.password = QStringLiteral("not-used");
    cfg.timeoutSeconds = 2;
    cfg.hostKeyPolicy = omegassh::HostKeyPolicy::Insecure;

    omegassh::OmegaSshSession session;
    QVector<omegassh::State> states;
    QString errorSeen;
    int reportedExit = -2;
    bool finished = false;
    int ticks = 0;

    QObject::connect(&session, &omegassh::OmegaSshSession::stateChanged,
                     [&](omegassh::State s) {
                         requireQtThread(qtThread, "stateChanged");
                         states << s;
                     });
    QObject::connect(&session, &omegassh::OmegaSshSession::errorOccurred,
                     [&](const QString &message) {
                         requireQtThread(qtThread, "errorOccurred");
                         if (errorSeen.isEmpty()) errorSeen = message;
                     });
    QObject::connect(&session, &omegassh::OmegaSshSession::finished,
                     [&](int exitCode) {
                         requireQtThread(qtThread, "finished");
                         finished = true;
                         reportedExit = exitCode;
                         QCoreApplication::quit();
                     });

    // Ticking while the dial is in flight. If start() were still blocking,
    // none of these would run until it returned and the count would be zero.
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, [&]() { ticks++; });
    heartbeat.start(50);

    const bool started = session.start(cfg);
    check(started, "start() returns true for a dial that will fail",
          session.error());
    check(session.state() == omegassh::State::Connecting,
          "state is connecting the moment start() returns",
          QString::fromLatin1(stateName(session.state())));

    QTimer::singleShot(15000, []() { QCoreApplication::quit(); });
    QCoreApplication::exec();
    heartbeat.stop();

    check(ticks > 5, "the event loop kept running during the dial",
          QStringLiteral("%1 ticks").arg(ticks));
    check(!states.isEmpty() && states.constLast() == omegassh::State::Failed,
          "the dial ends in Failed", trail(states));
    check(!errorSeen.isEmpty(), "errorOccurred carried the reason");
    if (!errorSeen.isEmpty()) printf("       %s\n", qPrintable(errorSeen));
    check(finished && reportedExit == -1,
          "finished(-1) so a tab learns it is over",
          QStringLiteral("finished=%1 exit=%2").arg(finished).arg(reportedExit));
    check(!session.running(), "the session is not running afterwards");
}

// ---------------------------------------------------------------------------
// The live session.
// ---------------------------------------------------------------------------
void runLiveSession(QThread *qtThread, const QStringList &args) {
    printf("\nlive session against %s:%s\n", qPrintable(args.at(0)),
           qPrintable(args.at(1)));

    omegassh::Config cfg;
    cfg.host = args.at(0);
    cfg.port = args.at(1).toInt();
    cfg.username = args.at(2);
    cfg.privateKeyPath = args.at(3);
    // Disposable lab target: no stable host key worth pinning.
    cfg.hostKeyPolicy = omegassh::HostKeyPolicy::Insecure;
    cfg.cols = 110;
    cfg.rows = 32;

    omegassh::OmegaSshSession session;
    QVector<omegassh::State> states;
    QByteArray transcript;
    int reportedExit = -2;
    bool finished = false;

    QObject::connect(&session, &omegassh::OmegaSshSession::dataReceived,
                     [&](const QByteArray &data) {
                         requireQtThread(qtThread, "dataReceived");
                         transcript += data;
                     });
    QObject::connect(&session, &omegassh::OmegaSshSession::errorOccurred,
                     [&](const QString &message) {
                         printf("[FAIL] error: %s\n", qPrintable(message));
                         failures++;
                     });
    QObject::connect(&session, &omegassh::OmegaSshSession::finished,
                     [&](int exitCode) {
                         requireQtThread(qtThread, "finished");
                         finished = true;
                         reportedExit = exitCode;
                         QCoreApplication::quit();
                     });

    // The session is driven from the state signal rather than from a timer
    // long enough to cover a dial. A fixed wait is a guess that holds against
    // a lab box on loopback and stops holding against gear on a satellite
    // link; the signal is the actual answer to "can I type yet".
    QObject::connect(&session, &omegassh::OmegaSshSession::stateChanged,
                     [&](omegassh::State s) {
                         requireQtThread(qtThread, "stateChanged");
                         states << s;
                         if (s != omegassh::State::Connected) return;

                         QTimer::singleShot(600, [&]() {
                             transcript.clear();
                             session.write("echo QT_MARKER_$((6*7))\n");
                         });
                         QTimer::singleShot(1800, [&]() {
                             check(transcript.contains("QT_MARKER_42"),
                                   "command round-trip through dataReceived");
                             transcript.clear();
                             session.resize(140, 45);
                             session.write("stty size\n");
                         });
                         QTimer::singleShot(3200, [&]() {
                             check(transcript.contains("45 140"),
                                   "resize reached the remote tty (45x140)");
                             session.write("exit\n");
                         });
                     });

    check(session.start(cfg), "start() returns true", session.error());

    // Backstop: if the shell never exits, do not hang a build.
    QTimer::singleShot(15000, [&]() {
        if (!finished) {
            printf("[FAIL] timed out waiting for finished()\n");
            failures++;
            QCoreApplication::quit();
        }
    });

    QCoreApplication::exec();

    // The sequence the dial published. Connecting and Authenticating did not
    // exist on SSH before the dial moved off the calling thread: open returned
    // a session that was already up, so a connection overlay had nothing to
    // render but the end of the story.
    check(states.contains(omegassh::State::Connecting) &&
              states.contains(omegassh::State::Authenticating) &&
              states.contains(omegassh::State::Connected),
          "connecting -> authenticating -> connected reached Qt as signals",
          trail(states));

    check(!session.running(), "session reports not running after teardown");

    // `exit` with no argument returns the status of the last command, which
    // was a successful stty. Anything else means the status was invented
    // rather than read off the channel.
    check(reportedExit == 0, "exit status read from the channel (0)",
          QStringLiteral("got %1").arg(reportedExit));
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc < 5) {
        fprintf(stderr, "usage: %s <host> <port> <user> <key-path>\n", argv[0]);
        return 2;
    }

    printf("[ ok ] omegassh %s\n", qPrintable(omegassh::OmegaSshSession::version()));

    QStringList args;
    for (int i = 1; i < argc; ++i) args << QString::fromLocal8Bit(argv[i]);

    QThread *qtThread = QThread::currentThread();
    runFailedDial(qtThread);
    runLiveSession(qtThread, args);

    printf("\n%s (%d failure%s)\n", failures ? "FAILURES" : "ALL PASS", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
