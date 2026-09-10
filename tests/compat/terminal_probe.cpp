// tests/compat/terminal_probe.cpp
//
// The two claims the terminal tab makes that cannot be checked by reading it.
//
//   terminal_probe focus     Tab and Shift+Tab reach the emulator instead of
//                            the focus chain
//   terminal_probe capture   escape stripping survives a chunk boundary
//   terminal_probe paste     rate-limited paste chunks on lines, caps long
//                            ones, and delivers every byte in order
//   terminal_probe hostkey   first contact is recognised, a key mismatch is
//                            not
//   terminal_probe antiidle  the keystroke bytes, the config round trip, and
//                            every condition that suppresses a send
//   terminal_probe scroll    the emulator honours a scrollback limit the tab
//                            hands it
//   terminal_probe tabclose  a multi-tab close asks only when a session in the
//                            set is still live, and names it
//   terminal_probe reconnect the guard on re-dialing a tab in place, and what a
//                            refused re-dial leaves the tab text saying
//   terminal_probe           all eight
//
// Unlike the differentials there is no second implementation to compare
// against here, so this one judges: it prints a line per case and exits
// non-zero if any of them is wrong.
//
// The focus case runs under -platform offscreen. It does not need a real X
// server because the interception it is testing is not the server's:
// QWidget::event() consumes Tab and hands it to focusNextPrevChild() before
// keyPressEvent() ever runs, and a posted key event takes exactly that path.
// The control case constructs a bare qtpyte::TerminalWidget and asserts that
// it DOES lose focus -- that is the bug TerminalView exists to fix, and if
// anytermqt ever fixes it upstream this line is what says so.

#include <QApplication>
#include <QByteArray>
#include <QEventLoop>
#include <QKeyEvent>
#include <QTimer>
#include <QLineEdit>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

#include <qtpyte/terminalwidget.h>

#include <omegassh/omegassh.h>

#include "app/capturewriter.h"
#include "app/hostkeyerror.h"
#include "app/antiidle.h"
#include "app/omegasettings.h"
#include "app/pastequeue.h"
#include "app/tabclose.h"
#include "app/terminaltab.h"
#include "app/terminalview.h"

#include <cstdio>

using namespace omega::app;

namespace {

int failures = 0;

void check(const char *name, bool ok, const QByteArray &detail) {
    std::printf("%-28s %s  %s\n", name, ok ? "ok  " : "FAIL",
                detail.constData());
    if (!ok) {
        ++failures;
    }
}

void sendKey(QWidget *target, int key, Qt::KeyboardModifiers mods) {
    QKeyEvent press(QEvent::KeyPress, key, mods);
    QApplication::sendEvent(target, &press);
}

// One window, a terminal and something else focusable -- which is the whole
// precondition for the bug. A terminal alone in a window has nowhere for the
// focus to go and behaves correctly by accident.
template <typename TermT>
void focusCase(const char *label, bool expectTerminalKeepsFocus) {
    QWidget window;
    auto *layout = new QVBoxLayout(&window);
    auto *term = new TermT(&window);
    auto *other = new QLineEdit(&window);
    layout->addWidget(term);
    layout->addWidget(other);

    QByteArray emitted;
    QObject::connect(term, &qtpyte::TerminalWidget::dataReady,
                     [&emitted](const QByteArray &d) { emitted += d; });

    window.show();
    term->setFocus();

    // The window's own focus widget, not hasFocus() and not
    // QApplication::focusWidget(): both of those also ask whether the window
    // is ACTIVE, and under the offscreen platform with no window manager it
    // never is. What is being tested is where the focus went inside the
    // window, which QWidget::focusWidget() answers regardless.
    auto focused = [&] { return window.focusWidget(); };

    sendKey(term, Qt::Key_Tab, Qt::NoModifier);
    const bool keptFocus = focused() == term;

    if (keptFocus) {
        sendKey(term, Qt::Key_Backtab, Qt::ShiftModifier);
    }

    QByteArray detail = "emitted=" + emitted.toHex() + " focus=" +
                        (focused() ? focused()->metaObject()->className()
                                   : "none");
    bool ok = keptFocus == expectTerminalKeepsFocus;
    if (expectTerminalKeepsFocus) {
        // HT for Tab, CSI Z for Shift+Tab -- what keymap.cpp sends once the
        // keys actually arrive.
        ok = ok && emitted == QByteArrayLiteral("\t\x1b[Z");
    } else {
        ok = ok && emitted.isEmpty();
    }
    check(label, ok, detail);
}

void runFocus() {
    focusCase<TerminalView>("focus/terminalview", true);
    focusCase<qtpyte::TerminalWidget>("focus/bare-widget", false);
}

void runCapture() {
    // A prompt redraw with colour, a title set through OSC, and a cursor
    // save. Split so that a CSI sequence, an OSC string and its terminator
    // each straddle a boundary -- which is what a per-chunk regex cannot see
    // and what this state machine exists for.
    const QList<QByteArray> chunks = {
        QByteArrayLiteral("hostname\x1b[1;32m#\x1b"),
        QByteArrayLiteral("[0m show ver\x1b]0;lab-sw-01"),
        QByteArrayLiteral("\x07\r\nVersion 1.0\x1b"),
        QByteArrayLiteral("7 done\n"),
    };

    CaptureWriter writer;
    QByteArray out;
    for (const QByteArray &chunk : chunks) {
        out += writer.strip(chunk);
    }

    const QByteArray want =
        QByteArrayLiteral("hostname# show ver\r\nVersion 1.0 done\n");
    check("capture/split-sequences", out == want, "got=" + out.toPercentEncoding());

    // Multi-byte UTF-8 split across a boundary comes through whole, which a
    // QString-based stripper cannot promise.
    CaptureWriter utf8;
    QByteArray joined = utf8.strip(QByteArrayLiteral("box \xe2\x94"));
    joined += utf8.strip(QByteArrayLiteral("\x82 end"));
    check("capture/split-utf8",
          joined == QByteArrayLiteral("box \xe2\x94\x82 end"),
          "got=" + joined.toPercentEncoding());
}

void runPaste() {
    // A chunk is a line WITH its terminator: the gap belongs after the
    // newline, where the device is parsing.
    PasteQueue q;
    q.load(QStringLiteral("interface Gi0/1\n description lab uplink\n no shut\n"));
    QStringList got;
    while (q.hasNext()) {
        got << q.next();
    }
    check("paste/line-chunks",
          got == QStringList{QStringLiteral("interface Gi0/1\n"),
                             QStringLiteral(" description lab uplink\n"),
                             QStringLiteral(" no shut\n")},
          "chunks=" + QByteArray::number(got.size()));

    // Trailing text with no newline is its own chunk, so a paste without a
    // final newline behaves the way typing it would -- nothing is added.
    PasteQueue tail;
    tail.load(QStringLiteral("show ver\nshow inv"));
    QString joined;
    while (tail.hasNext()) {
        joined += tail.next();
    }
    check("paste/no-trailing-newline",
          joined == QStringLiteral("show ver\nshow inv") && tail.total() == 2,
          "total=" + QByteArray::number(tail.total()));

    // A pathological single line is split at the cap, and the terminator
    // stays on the last piece.
    const QString longLine = QString(700, QLatin1Char('x')) + QLatin1Char('\n');
    PasteQueue big;
    big.load(longLine);
    QString rebuilt;
    bool allWithinCap = true;
    while (big.hasNext()) {
        const QString chunk = big.next();
        allWithinCap = allWithinCap && chunk.size() <= PasteQueue::kMaxChunkChars;
        rebuilt += chunk;
    }
    check("paste/long-line-capped",
          allWithinCap && rebuilt == longLine && big.total() == 3,
          "total=" + QByteArray::number(big.total()));

    // Whatever the chunking, the bytes that come out must be the bytes that
    // went in. This is the claim that matters: a paste that arrives reordered
    // or short is worse than one that arrives all at once.
    const QString mixed =
        QStringLiteral("a\n") + QString(300, QLatin1Char('b')) +
        QStringLiteral("\n\nc");
    PasteQueue fidelity;
    fidelity.load(mixed);
    QString out;
    while (fidelity.hasNext()) {
        out += fidelity.next();
    }
    check("paste/byte-fidelity", out == mixed,
          "len=" + QByteArray::number(out.size()));

    // Baud pacing: ten bits a character, so 9600 baud carries 960 characters
    // a second and 80 characters owe 83 ms. Eight bits would give 67 and
    // overrun a console slowly enough to look like something else.
    const int ms = pasteDelayMs(80, 9600);
    check("paste/baud-delay", ms == 84, "ms=" + QByteArray::number(ms));

    // Unlimited is 0 baud and owes nothing.
    check("paste/baud-unlimited",
          pasteDelayMs(4000, 0) == 0 && pasteDurationSeconds(4000, 0) == 0.0,
          "");

    // The estimate the dialog shows: 9600 baud, 9600 characters, 10 s.
    const double seconds = pasteDurationSeconds(9600, 9600);
    check("paste/duration-estimate", qAbs(seconds - 10.0) < 0.001,
          "s=" + QByteArray::number(seconds));
}

void runHostKey() {
    // The marker as the LIBRARY defines it, not as this file remembers it.
    // The fixtures below are shaped by hand; this is what stops them from
    // quietly describing a message the library no longer emits.
    char *raw = omegassh_unknown_hostkey_marker();
    const QString marker = QString::fromUtf8(raw ? raw : "");
    omegassh_free(raw);
    check("hostkey/marker-published", !marker.isEmpty(),
          "marker=" + marker.toUtf8());

    // Wrapped the way the transport wraps it: context in front of the marker,
    // which is why the classifier searches rather than matching a prefix.
    const QString unknown =
        QStringLiteral("connect to lab-sw-01:22: ") + marker +
        QStringLiteral(" lab-sw-01:22 (ssh-ed25519 "
                       "SHA256:2P/aB+cd0EfGhIjKlMnOpQrStUvWxYz1234567890abc); "
                       "not in /home/lab/.ssh/known_hosts");
    // Fails when the library reworded the marker and this fixture did not
    // follow -- which is the drift the published marker exists to catch.
    check("hostkey/fixture-current", unknown.contains(marker), "");

    const HostKeyInfo info = parseHostKeyError(unknown);
    check("hostkey/unknown-parsed",
          info.unknownHost && info.hostname == QLatin1String("lab-sw-01:22") &&
              info.keyType == QLatin1String("ssh-ed25519") &&
              info.fingerprint.startsWith(QLatin1String("SHA256:")) &&
              info.knownHostsPath ==
                  QLatin1String("/home/lab/.ssh/known_hosts"),
          "host=" + info.hostname.toUtf8() + " type=" + info.keyType.toUtf8() +
              " fp=" + info.fingerprint.toUtf8());

    // A SHA256 fingerprint carries /, + and =. A character class that forgot
    // one would truncate it into something that still looks like a
    // fingerprint, which is the worst possible way for this to be wrong.
    check("hostkey/fingerprint-intact",
          info.fingerprint ==
              QLatin1String("SHA256:2P/aB+cd0EfGhIjKlMnOpQrStUvWxYz1234567890abc"),
          "fp=" + info.fingerprint.toUtf8());

    // The one that must never parse: pinned to a different key. Offering to
    // trust the offered key here is worse than refusing to connect.
    const QString mismatch = QStringLiteral(
        "connect to lab-sw-01:22: host key verification failed for "
        "lab-sw-01:22: offered key (ssh-ed25519 SHA256:zzzz) does not match "
        "the pinned key in /home/lab/.ssh/known_hosts; if this change is "
        "expected, remove the old entry and reconnect");
    check("hostkey/mismatch-refused", !parseHostKeyError(mismatch).unknownHost,
          "");

    // Ordinary failures stay ordinary.
    check("hostkey/unrelated-refused",
          !parseHostKeyError(
               QStringLiteral("connect to lab-sw-01:22: dial tcp: i/o timeout"))
               .unknownHost,
          "");

    // The marker present but the shape wrong: better an ordinary failure than
    // a dialog with blank fields where the fingerprint should be.
    check("hostkey/malformed-refused",
          !parseHostKeyError(marker + QStringLiteral(" lab-sw-01 but then nothing"))
               .unknownHost,
          "");
}

void runAntiIdle() {
    // The bytes. Backspace is the default because it is the convention; the
    // half-typed-line case that makes space-backspace safer is in antiidle.h.
    AntiIdleConfig cfg;
    check("antiidle/bytes-backspace",
          antiIdleBytes(cfg) == QByteArray(1, '\x08'),
          antiIdleBytes(cfg).toHex());

    cfg.keystroke = AntiIdleKeystroke::SpaceBackspace;
    check("antiidle/bytes-space-bs",
          antiIdleBytes(cfg) == QByteArray("\x20\x08", 2),
          antiIdleBytes(cfg).toHex());

    cfg.keystroke = AntiIdleKeystroke::Nul;
    check("antiidle/bytes-nul", antiIdleBytes(cfg) == QByteArray(1, '\0'),
          antiIdleBytes(cfg).toHex());

    cfg.keystroke = AntiIdleKeystroke::Custom;
    cfg.custom = QByteArray("\x0d", 1);
    check("antiidle/bytes-custom", antiIdleBytes(cfg) == QByteArray(1, '\x0d'),
          antiIdleBytes(cfg).toHex());

    // A Custom with nothing in it must read as "do not fire" rather than as a
    // zero-length write, which resets no idle timer anywhere.
    cfg.custom.clear();
    cfg.enabled = true;
    check("antiidle/custom-empty-off",
          !antiIdleAllowed(cfg, true, false, false), "");

    // Names round-trip, and an unrecognised one falls back rather than
    // failing the file -- same contract AppSettings has for a bad type.
    bool allNames = true;
    for (const AntiIdleKeystroke k :
         {AntiIdleKeystroke::Backspace, AntiIdleKeystroke::SpaceBackspace,
          AntiIdleKeystroke::Nul, AntiIdleKeystroke::Custom}) {
        allNames = allNames && keystrokeFromName(keystrokeName(k)) == k;
    }
    check("antiidle/name-roundtrip", allNames, "");

    bool known = true;
    const AntiIdleKeystroke fallback = keystrokeFromName("ctrl-shift-moon", &known);
    check("antiidle/unknown-name",
          !known && fallback == AntiIdleKeystroke::Backspace, "");

    // Hex refuses rather than truncates. A custom keystroke that silently
    // loses its last nibble is worse than one that visibly does nothing.
    check("antiidle/hex-roundtrip",
          hexToBytes(bytesToHex(QByteArray("\x1b[B", 3))) == QByteArray("\x1b[B", 3),
          bytesToHex(QByteArray("\x1b[B", 3)).toUtf8());
    check("antiidle/hex-odd-refused", hexToBytes("080").isEmpty(), "");
    check("antiidle/hex-nonhex-refused", hexToBytes("0g").isEmpty(), "");

    // The suppressions, one at a time. Each of these has cost somebody a
    // session: a backspace into vi deletes a character, any key at a pager
    // prompt moves it on, and firing into a paced paste corrupts a line.
    AntiIdleConfig on;
    on.enabled = true;
    check("antiidle/allowed", antiIdleAllowed(on, true, false, false), "");
    check("antiidle/not-connected", !antiIdleAllowed(on, false, false, false), "");
    check("antiidle/alt-screen", !antiIdleAllowed(on, true, true, false), "");
    check("antiidle/pasting", !antiIdleAllowed(on, true, false, true), "");

    AntiIdleConfig off;
    check("antiidle/disabled", !antiIdleAllowed(off, true, false, false), "");

    on.seconds = 0;
    check("antiidle/zero-seconds", !antiIdleAllowed(on, true, false, false), "");

    // The file. Omega-only, so this is a round trip against itself rather
    // than a differential -- there is no second implementation to agree with.
    OmegaSettings written;
    written.anti_idle.enabled = true;
    written.anti_idle.seconds = 45;
    written.anti_idle.keystroke = AntiIdleKeystroke::Custom;
    written.anti_idle.custom = QByteArray("\x20\x08", 2);

    QStringList warnings;
    const OmegaSettings read =
        OmegaSettings::fromJson(written.toJson().toUtf8(), &warnings);
    check("antiidle/file-roundtrip",
          read.anti_idle.enabled == written.anti_idle.enabled &&
              read.anti_idle.seconds == written.anti_idle.seconds &&
              read.anti_idle.keystroke == written.anti_idle.keystroke &&
              read.anti_idle.custom == written.anti_idle.custom &&
              warnings.isEmpty(),
          warnings.join(QStringLiteral("; ")).toUtf8());

    // An unparseable file yields defaults and says so, rather than refusing
    // to start.
    warnings.clear();
    const OmegaSettings broken = OmegaSettings::fromJson("{not json", &warnings);
    check("antiidle/file-forgiving",
          !broken.anti_idle.enabled && !warnings.isEmpty(),
          warnings.join(QStringLiteral("; ")).toUtf8());

    // --- ssh_default_auth ---------------------------------------------------
    for (const SshDefaultAuth mode :
         {SshDefaultAuth::VaultDefault, SshDefaultAuth::Ask,
          SshDefaultAuth::Agent}) {
        OmegaSettings out;
        out.ssh_default_auth = mode;
        warnings.clear();
        const OmegaSettings back =
            OmegaSettings::fromJson(out.toJson().toUtf8(), &warnings);
        check(QByteArray("sshauth/roundtrip-") + sshDefaultAuthName(mode),
              back.ssh_default_auth == mode && warnings.isEmpty(),
              warnings.join(QStringLiteral("; ")).toUtf8());
    }

    // Read BEFORE the anti_idle block, which returns early when there is no
    // anti_idle object. An omega.json written by a build that predates this
    // field has exactly that shape, so a mode parsed after it would be
    // dropped on every upgrade.
    warnings.clear();
    const OmegaSettings noIdle =
        OmegaSettings::fromJson("{\"ssh_default_auth\": \"agent\"}", &warnings);
    check("sshauth/survives-a-file-with-no-anti-idle",
          noIdle.ssh_default_auth == SshDefaultAuth::Agent,
          warnings.join(QStringLiteral("; ")).toUtf8());

    // A file that predates the field takes the shipped default rather than
    // the behaviour the application used to have.
    warnings.clear();
    const OmegaSettings old = OmegaSettings::fromJson(
        "{\"anti_idle\": {\"enabled\": true, \"seconds\": 30}}", &warnings);
    check("sshauth/absent-takes-the-shipped-default",
          old.ssh_default_auth == SshDefaultAuth::VaultDefault &&
              old.anti_idle.enabled && warnings.isEmpty(),
          warnings.join(QStringLiteral("; ")).toUtf8());

    // A typo must not be the reason credentials are picked by a path nobody
    // chose. Named, and landed on the mode that asks first.
    warnings.clear();
    const OmegaSettings typo =
        OmegaSettings::fromJson("{\"ssh_default_auth\": \"agnet\"}", &warnings);
    check("sshauth/unknown-name-is-named",
          typo.ssh_default_auth == SshDefaultAuth::VaultDefault &&
              !warnings.isEmpty(),
          "");
}

void runScrollback() {
    // What TerminalTab::setScrollbackLines hands down. Checked at the widget
    // rather than through the tab because the tab keeps its terminal private,
    // and the claim being made is the emulator's: that the limit is honoured
    // and that history trims to it.
    TerminalView view;
    view.setScrollbackSize(50);
    check("scroll/limit-set", view.scrollbackLimit() == 50,
          QByteArray::number(view.scrollbackLimit()));

    QByteArray lines;
    for (int i = 0; i < 300; ++i) {
        lines += QByteArrayLiteral("line\r\n");
    }
    view.feed(lines);
    check("scroll/trims-to-limit", view.scrollbackSize() <= 50,
          QByteArray::number(view.scrollbackSize()));

    // The default the tab overrides. anytermqt's widget constructs its Screen
    // with 5000 -- NOT pyte's own kDefaultScrollback of 1000, which is what
    // the header reads like -- while config.json says 10000. Pinned here
    // because the gap is the reason this wire-up was worth doing, and because
    // an upstream change to that constant should show up as a failure here
    // rather than as tabs quietly holding a different amount of history.
    TerminalView fresh;
    check("scroll/widget-default", fresh.scrollbackLimit() == 5000,
          QByteArray::number(fresh.scrollbackLimit()));
}

void runTabClose() {
    // Dead tabs never ask. A set of five whose sessions have all ended is five
    // pieces of scrollback, and the close button already takes those one at a
    // time without a word.
    TabCloseSet quiet;
    quiet.total = 5;
    check("tabclose/no-live-no-ask", !quiet.needsConfirmation(), "");
    check("tabclose/no-live-no-detail", tabCloseDetail(quiet).isEmpty(), "");

    TabCloseSet one;
    one.total = 3;
    one.liveTitles << QStringLiteral("lab-core-1");
    check("tabclose/live-asks", one.needsConfirmation(), "");
    check("tabclose/question-plural",
          tabCloseQuestion(one) == QStringLiteral("Close 3 tabs?"),
          tabCloseQuestion(one).toUtf8());

    // The name of what is about to be dropped has to be IN the message. A
    // count alone leaves the person guessing which session they are about to
    // lose, which is the whole question being asked.
    check("tabclose/detail-names-session",
          tabCloseDetail(one).contains(QStringLiteral("lab-core-1")),
          tabCloseDetail(one).toUtf8());

    TabCloseSet single;
    single.total = 1;
    single.liveTitles << QStringLiteral("lab-edge-2");
    check("tabclose/question-singular",
          tabCloseQuestion(single) == QStringLiteral("Close this tab?"),
          tabCloseQuestion(single).toUtf8());

    // Thirty tabs must not produce a message box taller than the window behind
    // it. The cap holds and the remainder is counted rather than dropped --
    // silently listing eight of thirty would understate what is being closed.
    TabCloseSet many;
    many.total = 30;
    for (int i = 0; i < 30; ++i) {
        many.liveTitles << QStringLiteral("lab-sw-%1").arg(i);
    }
    const QString detail = tabCloseDetail(many);
    const int lines = static_cast<int>(detail.count(QLatin1Char('\n')));
    check("tabclose/list-capped",
          detail.contains(QStringLiteral("and 22 more")) &&
              lines == kTabCloseMaxListed + 2,
          "lines=" + QByteArray::number(lines));
    check("tabclose/count-is-total",
          detail.contains(QStringLiteral("30 sessions")), "");
}

void runReconnect() {
    // No network, no hardware and no vault: a parity that does not exist is
    // refused by capi's prepare() before anything is opened, so this is a dial
    // that fails identically on every machine and never reaches a port.
    //
    // NOT the jump-credential-without-a-jump-host refusal, which looks like the
    // obvious choice and is unreachable from here: Config::toJson() only writes
    // the jump fields when jumpHost is set, so the Go side never sees the
    // combination it would refuse.
    TerminalTab tab;
    check("reconnect/fresh-tab-refuses", !tab.canReconnect(),
          "a tab that never dialed has no config to dial again");

    omegassh::Config bad;
    bad.transport = omegassh::Transport::Serial;
    bad.serialPort = QStringLiteral("/dev/ttyLAB0");
    bad.parity = QStringLiteral("sideways");

    const bool started = tab.start(bad);
    check("reconnect/config-error-refused", !started, tab.error().toUtf8());

    // The point of the whole feature: a tab whose session did not survive is
    // one somebody can dial again in place, keeping whatever the last session
    // left on screen.
    check("reconnect/offered-after-failure", tab.canReconnect(),
          "state=" + QByteArray::number(static_cast<int>(tab.state())));

    // restarted() is what takes "(closed)" back off the tab text. A dial that
    // is refused before it starts must NOT emit it -- the tab really is still
    // closed, and a title saying otherwise would be the one lie the tab strip
    // can tell about a tab nobody is looking at.
    int restarts = 0;
    QObject::connect(&tab, &TerminalTab::restarted, [&restarts] { ++restarts; });
    tab.reconnect();
    check("reconnect/refused-dial-stays-closed", restarts == 0,
          "restarts=" + QByteArray::number(restarts));

    // And it is still offered afterwards, so a second attempt is possible.
    // The prompt-once guard reset inside reconnect() must not have latched
    // anything shut.
    check("reconnect/still-offered", tab.canReconnect(), "");

    // --- the re-dial that does start ---------------------------------------
    //
    // Everything above is the refusal path, where nothing is ever opened. This
    // is the one the feature exists for: a session that reached the network,
    // failed, and is dialed again in the same tab.
    //
    // .invalid is reserved by RFC 2606 and resolves nowhere, so the failure is
    // a name lookup rather than a connect to somebody's real host -- the same
    // reason credential_probe uses it.
    TerminalTab live;
    omegassh::Config gone;
    gone.transport = omegassh::Transport::Telnet;
    gone.host = QStringLiteral("lab-sw-01.lab.invalid");

    if (!live.start(gone)) {
        check("reconnect/live-dial-started", false, live.error().toUtf8());
        return;
    }

    // Wait for the dial to reach a terminal state. Bounded, because a resolver
    // that is slow or absent must fail this probe rather than hang a build.
    auto settle = [&live](int ms) {
        QEventLoop loop;
        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(&live, &TerminalTab::finished, &loop, &QEventLoop::quit);
        deadline.start(ms);
        loop.exec();
    };
    settle(15000);

    if (live.isRunning()) {
        check("reconnect/live-dial-ended", false,
              "dial did not reach a terminal state in 15s");
        return;
    }
    check("reconnect/live-dial-ended", true, live.error().toUtf8());
    check("reconnect/offered-after-live-failure", live.canReconnect(), "");

    int restarted = 0;
    QObject::connect(&live, &TerminalTab::restarted,
                     [&restarted] { ++restarted; });
    live.reconnect();

    // Emitted synchronously from reconnect(), because that is what the tab
    // text depends on: the window has to take "(closed)" off before the new
    // dial's first state arrives, not after.
    check("reconnect/restarted-emitted", restarted == 1,
          "restarted=" + QByteArray::number(restarted));

    // A dial is in flight now, so the offer is withdrawn -- Reconnect is for a
    // session that has ended, and an entry that appeared on a connecting tab
    // would be a second way to tear down the one it just started.
    check("reconnect/withdrawn-while-dialing", !live.canReconnect(),
          "state=" + QByteArray::number(static_cast<int>(live.state())));

    // Same config, same tab. A re-dial that quietly changed something would be
    // the hardest kind of wrong to notice, since the second attempt looks like
    // the first from the outside.
    settle(15000);
    check("reconnect/second-dial-ended", !live.isRunning(),
          live.error().toUtf8());

    live.terminate();
}

}  // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    const QStringList args = app.arguments();
    const QString which = args.size() > 1 ? args.at(1) : QString();

    if (which.isEmpty() || which == QLatin1String("focus")) {
        runFocus();
    }
    if (which.isEmpty() || which == QLatin1String("capture")) {
        runCapture();
    }
    if (which.isEmpty() || which == QLatin1String("paste")) {
        runPaste();
    }
    if (which.isEmpty() || which == QLatin1String("hostkey")) {
        runHostKey();
    }
    if (which.isEmpty() || which == QLatin1String("antiidle")) {
        runAntiIdle();
    }
    if (which.isEmpty() || which == QLatin1String("scroll")) {
        runScrollback();
    }
    if (which.isEmpty() || which == QLatin1String("tabclose")) {
        runTabClose();
    }
    if (which.isEmpty() || which == QLatin1String("reconnect")) {
        runReconnect();
    }

    return failures == 0 ? 0 : 1;
}