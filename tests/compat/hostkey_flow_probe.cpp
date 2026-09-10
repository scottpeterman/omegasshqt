// tests/compat/hostkey_flow_probe.cpp
//
// The host key prompt, end to end against a real sshd.
//
//   hostkey_flow_probe <client-key> <decoy-hostline> <host> <port> <user>
//
// Stand the server up with tests/labsshd.sh first; this is the same lab
// sshd examples/c/policy_suite talks to.
//
// Three runs against a throwaway known_hosts each time:
//
//   accept    first contact prompts, and accepting re-dials and CONNECTS,
//             leaving one entry behind
//   reject    first contact prompts, and rejecting leaves no entry and no
//             session
//   mismatch  a host pinned to a DIFFERENT key does NOT prompt, does not
//             connect, and does not touch the file
//
// The first is here because it broke once in a way no unit test could see:
// the failed session still held its handle, so the re-dial was refused with
// "session is already running" and the prompt led nowhere. That is a
// sequencing bug between two objects, and the only thing that catches it is
// doing the sequence.
//
// The third is the one that matters most. A key that does not match the
// pinned one is re-key or MITM; prompting there would offer to trust exactly
// what the check refused.

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>

#include "app/hostkeypromptdialog.h"
#include "app/terminaltab.h"

#include <cstdio>

using namespace omega::app;

namespace {

int failures = 0;

void check(const char *name, bool ok, const QString &detail) {
    std::printf("%-28s %s  %s\n", name, ok ? "ok  " : "FAIL",
                qPrintable(detail));
    if (!ok) {
        ++failures;
    }
}

int countLines(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return -1;
    }
    const QString body = QString::fromUtf8(f.readAll()).trimmed();
    return body.isEmpty() ? 0 : static_cast<int>(body.split(QLatin1Char('\n')).size());
}

struct Outcome {
    bool prompted = false;
    bool connected = false;
    int knownHostsLines = 0;

    // What the tab was left holding. Without this a run that never reached
    // the host key check at all -- a server that is not listening, a port
    // that does not match, a key the account will not take -- reports as
    // "no prompt" and says nothing about why.
    QString error;
    omegassh::State state = omegassh::State::Disconnected;
};

QString stateName(omegassh::State s) {
    switch (s) {
        case omegassh::State::Disconnected:   return QStringLiteral("disconnected");
        case omegassh::State::Connecting:     return QStringLiteral("connecting");
        case omegassh::State::Authenticating: return QStringLiteral("authenticating");
        case omegassh::State::Connected:      return QStringLiteral("connected");
        case omegassh::State::Reconnecting:   return QStringLiteral("reconnecting");
        case omegassh::State::Failed:         return QStringLiteral("failed");
    }
    return QStringLiteral("?");
}

// One line describing where a run actually ended, for a failure to quote.
QString describe(const Outcome &o) {
    QString text = QStringLiteral("state=%1").arg(stateName(o.state));
    if (!o.error.isEmpty()) {
        text += QStringLiteral(" error=\"%1\"").arg(o.error);
    }
    return text;
}

// Runs one dial to completion, answering the prompt if one appears.
Outcome run(const QString &knownHosts, const QString &clientKey,
            const QString &host, int port, const QString &user, bool acceptKey) {
    Outcome out;

    TerminalTab tab;
    tab.resize(700, 400);
    tab.show();

    // Polled rather than hooked: the dialog is modal and runs its own event
    // loop, so there is nothing to connect to from out here.
    QTimer answer;
    answer.setInterval(100);
    QObject::connect(&answer, &QTimer::timeout, [&] {
        for (QWidget *w : qApp->topLevelWidgets()) {
            auto *dialog = qobject_cast<HostKeyPromptDialog *>(w);
            if (dialog && dialog->isVisible()) {
                out.prompted = true;
                if (acceptKey) {
                    dialog->accept();
                } else {
                    dialog->reject();
                }
            }
        }
    });
    answer.start();

    omegassh::Config cfg;
    cfg.transport = omegassh::Transport::Ssh;
    cfg.host = host;
    cfg.port = port;
    cfg.username = user;
    cfg.privateKeyPath = clientKey;
    cfg.knownHostsPath = knownHosts;
    cfg.hostKeyPolicy = omegassh::HostKeyPolicy::Strict;
    cfg.timeoutSeconds = 10;

    tab.start(cfg);

    // Long enough for two dials and a prompt on a loopback server.
    QEventLoop wait;
    QTimer::singleShot(6000, &wait, &QEventLoop::quit);
    wait.exec();

    out.connected = tab.state() == omegassh::State::Connected;
    out.state = tab.state();
    out.error = tab.error();
    out.knownHostsLines = countLines(knownHosts);
    tab.terminate();
    return out;
}

}  // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 6) {
        std::fprintf(stderr,
                     "usage: hostkey_flow_probe <client-key> <decoy-hostline> "
                     "<host> <port> <user>\n");
        return 2;
    }

    const QString clientKey = args.at(1);
    const QString decoyLine = args.at(2);
    const QString host = args.at(3);
    const int port = args.at(4).toInt();
    const QString user = args.at(5);

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::fprintf(stderr, "could not create a temporary directory\n");
        return 2;
    }

    // --- accept ------------------------------------------------------------
    const QString acceptPath = tmp.filePath(QStringLiteral("kh_accept"));
    QFile(acceptPath).open(QIODevice::WriteOnly);
    const Outcome accepted =
        run(acceptPath, clientKey, host, port, user, true);
    check("flow/accept-prompted", accepted.prompted,
          accepted.prompted ? QString() : describe(accepted));
    // The one that regressed: without the teardown, the re-dial is refused
    // and this stays false while the prompt still appeared.
    check("flow/accept-connected", accepted.connected,
          accepted.connected ? QString()
                             : QStringLiteral("re-dial after accept did not "
                                              "connect: %1").arg(describe(accepted)));
    check("flow/accept-wrote-entry", accepted.knownHostsLines == 1,
          accepted.knownHostsLines == 1
              ? QString()
              : QStringLiteral("lines=%1").arg(accepted.knownHostsLines));

    // --- reject ------------------------------------------------------------
    const QString rejectPath = tmp.filePath(QStringLiteral("kh_reject"));
    QFile(rejectPath).open(QIODevice::WriteOnly);
    const Outcome rejected =
        run(rejectPath, clientKey, host, port, user, false);
    check("flow/reject-prompted", rejected.prompted,
          rejected.prompted ? QString() : describe(rejected));
    check("flow/reject-not-connected", !rejected.connected, QString());
    // reject-not-connected and reject-wrote-nothing both pass trivially when
    // no prompt ever appeared, so they say nothing on their own. The line
    // above is the one to read.
    check("flow/reject-wrote-nothing", rejected.knownHostsLines == 0,
          rejected.knownHostsLines == 0
              ? QString()
              : QStringLiteral("lines=%1").arg(rejected.knownHostsLines));

    // --- mismatch ----------------------------------------------------------
    //
    // The decoy file is a bare "type key" pair with no host field, which
    // matches nothing on its own. Pinning it means writing the host in, and
    // a non-22 port takes the bracketed form -- get this wrong and the test
    // silently becomes another first-contact case that passes for the wrong
    // reason.
    QFile decoy(decoyLine);
    if (!decoy.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "cannot read decoy host line %s\n",
                     qPrintable(decoyLine));
        return 2;
    }
    const QString key = QString::fromUtf8(decoy.readAll()).trimmed();
    const QString pinned = port == 22
                               ? QStringLiteral("%1 %2\n").arg(host, key)
                               : QStringLiteral("[%1]:%2 %3\n")
                                     .arg(host)
                                     .arg(port)
                                     .arg(key);

    const QString mismatchPath = tmp.filePath(QStringLiteral("kh_mismatch"));
    QFile out(mismatchPath);
    out.open(QIODevice::WriteOnly);
    out.write(pinned.toUtf8());
    out.close();

    // acceptKey is true on purpose: if a prompt appears at all here, it would
    // be accepted, and the assertions below are what say it must not.
    const Outcome mismatch =
        run(mismatchPath, clientKey, host, port, user, true);
    check("flow/mismatch-no-prompt", !mismatch.prompted,
          mismatch.prompted
              ? QStringLiteral("a mismatched key was offered for acceptance")
              : QString());
    check("flow/mismatch-not-connected", !mismatch.connected,
          mismatch.connected ? describe(mismatch) : QString());
    check("flow/mismatch-file-untouched", mismatch.knownHostsLines == 1,
          mismatch.knownHostsLines == 1
              ? QString()
              : QStringLiteral("lines=%1").arg(mismatch.knownHostsLines));

    return failures == 0 ? 0 : 1;
}
