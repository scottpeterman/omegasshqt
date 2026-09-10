// examples/qt/terminal_window.cpp
//
// The point of the whole project in one file: an anytermqt TerminalWidget
// with an omegassh session behind it instead of a local pty -- over SSH,
// telnet or a serial console.
//
// Note what is NOT here -- no threads, no marshalling, no buffering. Three
// signal connections, made by omegassh::attach, which is qtpyte::attach with
// the transport swapped. Replacing PtySession::create() with an
// OmegaSshSession is the entire difference between a local shell and a device
// on the other side of the world.
//
// ONE BINARY, THREE TRANSPORTS, AND THAT IS THE DEMONSTRATION. Three separate
// examples would each be this file with one config block changed, and would
// prove less: what Phase 2 claims is that everything above the transport is
// identical, and three copies quietly diverging would be evidence against it.
// Below buildConfig() there is not a single branch on which transport is
// open -- the attach, the widget, the resize path, the status bar and the
// teardown are the same code for all three. If that ever stops being true,
// this file stops compiling as one file, which is the point.
//
// Only built when anytermqt is found; see examples/qt/CMakeLists.txt.
//
// Usage:
//   terminal_window ssh    <host> <port> <user> <key-path>
//   terminal_window telnet <host> [port]         (port defaults to 23)
//   terminal_window serial <device> [baud]       (baud defaults to 9600)
//   terminal_window serial                       lists ports and exits

#include <omegassh_anytermqt.h>

#include <QApplication>
#include <QGridLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QStatusBar>
#include <QStringList>
#include <QWidget>

#include <cstdio>

namespace {

// The overlay text. It is the state and nothing else: the label renders what
// the transport publishes rather than deciding a story of its own, which is
// the entire reason the six states are the transport's vocabulary and not the
// UI's.
QString stateLabel(omegassh::State s) {
    switch (s) {
        case omegassh::State::Connecting:
            return QObject::tr("Connecting...");
        case omegassh::State::Authenticating:
            return QObject::tr("Authenticating...");
        case omegassh::State::Connected:
            return QObject::tr("Connected");
        case omegassh::State::Reconnecting:
            return QObject::tr("Reconnecting...");
        case omegassh::State::Failed:
            return QObject::tr("Failed");
        case omegassh::State::Disconnected:
            break;
    }
    return QObject::tr("Disconnected");
}

void usage(const char *argv0) {
    std::fprintf(stderr,
                 "usage:\n"
                 "  %s ssh    <host> <port> <user> <key-path>\n"
                 "  %s telnet <host> [port]      (default 23)\n"
                 "  %s serial <device> [baud]    (default 9600)\n"
                 "  %s serial                    list ports and exit\n",
                 argv0, argv0, argv0, argv0);
}

// Prints what the library can see, in the form a quick-connect combo box
// would show it. Two identical USB-serial cables in one laptop are otherwise
// told apart only by which /dev node the kernel handed out, which is why
// displayName() folds in the vendor and product IDs.
int listSerialPorts() {
    const QVector<omegassh::SerialPortInfo> ports =
        omegassh::OmegaSshSession::serialPorts();

    const QString err = omegassh::OmegaSshSession::lastEnumerationError();
    if (!err.isEmpty()) {
        std::fprintf(stderr, "serial ports could not be enumerated: %s\n",
                     qPrintable(err));
        return 1;
    }
    if (ports.isEmpty()) {
        // Not an error. No adapter plugged in is a valid answer, and the
        // empty-versus-failed distinction is exactly what the error string
        // above is for.
        std::printf("no serial ports found\n");
        return 0;
    }
    for (const omegassh::SerialPortInfo &p : ports) {
        std::printf("  %s\n", qPrintable(p.displayName()));
    }
    return 0;
}

// buildConfig is the ONLY place in this file that knows which transport is
// being opened. Everything after it is transport-blind.
bool buildConfig(const QStringList &args, omegassh::Config &cfg) {
    const QString kind = args.value(0);

    if (kind == QLatin1String("ssh")) {
        if (args.size() < 5) return false;
        cfg.transport = omegassh::Transport::Ssh;
        cfg.host = args.at(1);
        cfg.port = args.at(2).toInt();
        cfg.username = args.at(3);
        cfg.privateKeyPath = args.at(4);
        // Trust on first use: pins the key, and still refuses a later
        // mismatch. Strict is the library default; this is an example, and it
        // dials hosts it has never met.
        cfg.hostKeyPolicy = omegassh::HostKeyPolicy::Tofu;
        // Harmless against modern servers, necessary for older gear.
        cfg.legacyAlgorithms = true;
        return true;
    }

    if (kind == QLatin1String("telnet")) {
        if (args.size() < 2) return false;
        cfg.transport = omegassh::Transport::Telnet;
        cfg.host = args.at(1);
        cfg.port = args.size() >= 3 ? args.at(2).toInt() : 23;
        // No credential, no jump host, no host-key policy. The library
        // refuses those on telnet rather than ignoring them: a jump host
        // silently dropped would put this session on the wire in plaintext
        // across a link the operator believed was tunneled.
        return true;
    }

    if (kind == QLatin1String("serial")) {
        if (args.size() < 2) return false;
        cfg.transport = omegassh::Transport::Serial;
        cfg.serialPort = args.at(1);
        cfg.baud = args.size() >= 3 ? args.at(2).toInt() : 9600;
        // 8N1 is the default in the library too; spelled out here because a
        // console cable that needs something else is the case someone reading
        // this example is likely to be in.
        cfg.dataBits = 8;
        cfg.parity = QStringLiteral("none");
        cfg.stopBits = QStringLiteral("1");
        return true;
    }

    return false;
}

}  // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);

    QStringList args;
    for (int i = 1; i < argc; ++i) args << QString::fromLocal8Bit(argv[i]);

    // `serial` with no device lists what is plugged in. This is the one path
    // that needs no window, and it is how you find the argument for the next
    // run.
    if (args.size() == 1 && args.at(0) == QLatin1String("serial")) {
        return listSerialPorts();
    }

    omegassh::Config cfg;
    if (!buildConfig(args, cfg)) {
        usage(argv[0]);
        return 2;
    }

    // ---- everything below here is transport-blind -------------------------

    QMainWindow window;

    // The terminal and the overlay share one grid cell, so the label paints
    // over the widget with no event filter and no custom paint. A real
    // connection overlay is Phase 5 work -- a spinner, the theme's overlay
    // colours, a Cancel button -- and this is not it. It is the cheapest thing
    // that proves the states arrive while there is still something to say
    // about them, which until the dial moved off the calling thread was not
    // true: "Connecting..." was a label that could never be seen, because by
    // the time a session existed to publish for, the connect was over.
    auto *central = new QWidget(&window);
    auto *stack = new QGridLayout(central);
    stack->setContentsMargins(0, 0, 0, 0);

    auto *terminal = new qtpyte::TerminalWidget(central);
    stack->addWidget(terminal, 0, 0);

    auto *overlay = new QLabel(central);
    overlay->setAlignment(Qt::AlignCenter);
    overlay->setStyleSheet(QStringLiteral(
        "background: rgba(0,0,0,180); color: #e6e6e6; font-size: 16pt;"));
    stack->addWidget(overlay, 0, 0);

    window.setCentralWidget(central);
    window.resize(1100, 700);

    auto *session = new omegassh::OmegaSshSession(&window);
    omegassh::attach(terminal, session);

    // One handler drives both. The status bar is the running commentary; the
    // overlay is up whenever the session is not connected, and gets out of the
    // way the moment it is.
    //
    // On SSH this now runs Connecting -> Authenticating -> Connected, and the
    // two intermediate states are worth telling apart on screen: an operator
    // staring at "Connecting..." against a device that is switched off is
    // being told something different from one staring at "Authenticating...",
    // where the network is fine and the credentials are the suspect. Telnet
    // shows connecting and never authenticating -- the protocol has no
    // authentication step -- and serial shows neither, because opening the
    // port is the whole handshake. None of them fake a state to look uniform,
    // so the overlay simply renders what arrives.
    QObject::connect(session, &omegassh::OmegaSshSession::stateChanged,
                     [&](omegassh::State s) {
                         window.statusBar()->showMessage(stateLabel(s));
                         const bool up = s == omegassh::State::Connected;
                         overlay->setText(
                             QStringLiteral("%1\n%2").arg(stateLabel(s),
                                                          session->summary()));
                         overlay->setVisible(!up);
                         if (up) terminal->setFocus();
                     });

    QObject::connect(session, &omegassh::OmegaSshSession::finished,
                     [&](int exitCode) {
                         // A dial that never connected also ends here -- one
                         // place for a tab to learn it is over, whether the
                         // session died at second one or an hour in -- so the
                         // reason has to win over the epitaph. Announcing
                         // "Session closed" for a host that was never reached
                         // replaces the only explanation on screen with a
                         // sentence that explains nothing.
                         if (session->state() == omegassh::State::Failed) {
                             window.statusBar()->showMessage(session->error());
                             return;
                         }
                         // Telnet and serial carry no exit status and always
                         // report -1. Printing "exit -1" for every one of them
                         // would read as a failure rather than as a protocol
                         // that has nothing to say.
                         window.statusBar()->showMessage(
                             exitCode < 0
                                 ? QObject::tr("Session closed")
                                 : QObject::tr("Session closed (exit %1)").arg(exitCode));
                     });

    // Connected BEFORE start(), and it has to be. This used to be wired up
    // afterwards, because errorOccurred fired from inside a blocking start()
    // and a modal box there was followed by a second box on the false return
    // -- two dialogs for one failure.
    //
    // With the dial asynchronous, that ordering springs a leak: start() drains
    // whatever the dial has already produced before it returns, and a refused
    // connection on loopback can fail inside that window. The error is then
    // emitted to nobody and the window sits there saying "Failed" with no
    // reason anywhere. (Found by running it against a closed port: the
    // message box never appeared.)
    //
    // So: one handler, connected first, and the critical box on the false
    // return is gone. A configuration refusal emits this synchronously and
    // then returns false; the far end's answer emits it on the event loop
    // later. One failure, one dialog, in both cases.
    QObject::connect(session, &omegassh::OmegaSshSession::errorOccurred,
                     [&](const QString &message) {
                         QMessageBox::warning(&window, QObject::tr("omegassh"),
                                              message);
                     });

    // Declared and never emitted yet. The dial can park in Authenticating now
    // -- that was the blocker -- and what is left is the answer travelling
    // back. Wired here so the shape is on record, and so this example does not
    // need revisiting when it starts firing.
    QObject::connect(session, &omegassh::OmegaSshSession::interactionRequired,
                     [&](const QString &question, bool echo) {
                         std::fprintf(stderr, "interaction required (echo=%d): %s\n",
                                      echo ? 1 : 0, qPrintable(question));
                     });

    // Open at the widget's real geometry so the first prompt wraps correctly.
    // The widget derives its grid from the font, so this is only right after
    // the window has been laid out -- hence show() before start().
    //
    // A resize that arrives DURING the dial is no longer a problem to time
    // around: the library records it and applies it when the session lands, so
    // a window the user drags while it is connecting still gets the right
    // width on its first screenful.
    window.show();
    cfg.cols = terminal->columns();
    cfg.rows = terminal->terminalRows();

    overlay->setText(QObject::tr("Starting..."));
    overlay->setVisible(true);

    // start() returning true means the dial has BEGUN, not that it landed. The
    // window is already up with the overlay on it, and the connect happens
    // behind them -- on this thread, without blocking it, which is why the
    // overlay can be painted at all.
    if (!session->start(cfg)) {
        // The reason was already shown by the handler above.
        return 1;
    }

    // summary() rather than a string assembled here: it comes from the
    // transport, so it is "10.0.0.1:23" or "/dev/ttyUSB0 9600 8N1" without
    // this file knowing which. It is answerable already, before the dial has
    // landed -- the target came from the configuration, not from the wire --
    // so the window is titled for the device it is still connecting to.
    window.setWindowTitle(QStringLiteral("omegassh - %1").arg(session->summary()));
    terminal->setFocus();

    return app.exec();
}
