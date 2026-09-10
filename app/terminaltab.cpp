// app/terminaltab.cpp

#include "app/terminaltab.h"

#include "app/sessionribbon.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileDialog>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QLayout>
#include <QVBoxLayout>
#include <QKeyEvent>
#include <QWheelEvent>

#include <cstdlib>
#include <QLabel>
#include <QActionGroup>
#include <QMenu>
#include <QPushButton>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>
#include <QTimer>

#include <omegassh_anytermqt.h>
#include <omegassh_theme_anytermqt.h>

#include "app/autherror.h"
#include "app/effectiveconfig.h"
#include "app/kbdprompt.h"
#include "app/hostkeypromptdialog.h"
#include "app/pasteconfirmdialog.h"
#include "app/terminalview.h"

namespace omega::app {
namespace {

// The overlay's text is the state and nothing else. It renders what the
// transport publishes rather than deciding a story of its own.
QString stateLabel(omegassh::State s) {
    switch (s) {
        case omegassh::State::Connecting:     return QObject::tr("Connecting...");
        case omegassh::State::Authenticating: return QObject::tr("Authenticating...");
        case omegassh::State::Connected:      return QObject::tr("Connected");
        case omegassh::State::Reconnecting:   return QObject::tr("Reconnecting...");
        case omegassh::State::Failed:         return QObject::tr("Failed");
        case omegassh::State::Disconnected:   break;
    }
    return QObject::tr("Disconnected");
}

// Erase display, erase saved lines, cursor home. CSI 3 J is the one that
// takes the scrollback with it: clearing only the window would leave whoever
// pressed Clear able to scroll back to what they just cleared.
const QByteArray kClearSequence = QByteArrayLiteral("\x1b[H\x1b[2J\x1b[3J");

// How much of a paste the confirmation shows. Enough to recognise what is
// about to be applied, short enough that the dialog does not need scrolling
// to answer the question it is asking.
constexpr int kPreviewLines = 5;

// Braille frames, the same set nterm-qt spins. One cell wide in a monospace
// font, so the message beside it does not shuffle left and right as it turns.
//
// Built from explicit code points rather than written literally in the
// source: the file stays ASCII, which is what keeps MSVC from reading it in
// the machine's ANSI codepage without /utf-8. Not QLatin1String either --
// that would index BYTES, and a braille glyph is three of them.
const QString &spinnerFrames() {
    static const char16_t points[] = {0x2807, 0x280B, 0x2819, 0x2839, 0x2838,
                                      0x283C, 0x2834, 0x2826, 0x2827, 0x280F, 0};
    static const QString frames = QString::fromUtf16(points);
    return frames;
}

// Fast enough to read as motion, slow enough not to look frantic.
constexpr int kSpinnerIntervalMs = 80;

// The widest the overlay's message may get before it wraps, in pixels.
//
// Chosen to sit inside a tab area on a window narrower than anything anyone
// would deliberately use, so the cap binds before the window does. A message
// longer than this wraps onto more lines rather than making the tab wider --
// see the note where it is applied.
constexpr int kOverlayMaxWidth = 520;

// The range Ctrl+wheel may reach, matching the terminal font spin box in the
// settings dialog. Zoom must not be able to reach a size the settings dialog
// would refuse, or a saved base and a zoomed tab could disagree about what
// sizes exist.
// The shared bounds; see app/effectiveconfig.h. Aliased rather than restated
// so the session editor's font row and this clamp cannot drift apart.
constexpr int kMinFontPointSize = kMinTerminalFontPointSize;
constexpr int kMaxFontPointSize = kMaxTerminalFontPointSize;

// How long after Connected the second window-size push goes out. Long enough
// for telnet's option negotiation to have finished -- NAWS is agreed in the
// first exchange, not seconds in -- and short enough that a full-screen
// application started immediately has not drawn a second frame at the wrong
// width.
constexpr int kSizeSyncDelayMs = 250;

// The default paste rate per transport, in baud.
//
// Serial takes the port's OWN baud, which is the one case where the right
// answer is already in the config: pacing the paste to the line rate it is
// going out over cannot overrun it. Telnet assumes a terminal server on the
// far side and takes 9600, the rate a console port is at unless somebody
// changed it. SSH is unpaced -- it has a window and does not need the help,
// and a delay there is a delay for nothing.
//
// Defaults, not policy: the confirmation dialog and the Paste Speed submenu
// both override for the life of the tab.
int defaultPasteBaud(const omegassh::Config &config) {
    switch (config.transport) {
        case omegassh::Transport::Serial: return qMax(0, config.baud);
        case omegassh::Transport::Telnet: return 9600;
        case omegassh::Transport::Ssh:    break;
    }
    return 0;
}

// Anything that would need quoting in a filename becomes an underscore.
// serialPort arrives as /dev/ttyUSB0, and a capture named after it must not
// try to write into /dev.
QString filenameSafe(const QString &text) {
    QString out = text;
    out.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")),
                QStringLiteral("_"));
    return out;
}

}  // namespace

TerminalTab::TerminalTab(QWidget *parent) : QWidget(parent) {
    // The ribbon on top, the terminal-and-overlay stack below it. The stack is
    // still a grid because the overlay has to sit ON the terminal; only the
    // ribbon is new, and it is a row rather than a cell.
    auto *column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);

    ribbon_ = new SessionRibbon(this);
    ribbon_->setVisible(false);  // nothing dialled yet: nothing to describe
    connect(ribbon_, &SessionRibbon::reconnectRequested, this,
            &TerminalTab::reconnect);
    column->addWidget(ribbon_);

    auto *stack = new QGridLayout;
    column->addLayout(stack, 1);
    stack->setContentsMargins(0, 0, 0, 0);

    // TerminalView rather than the widget itself: see terminalview.h. Tab is
    // the far end's key here, not the layout's.
    terminal_ = new TerminalView(this);

    // On the scroll area, NOT on its viewport. TerminalWidget is a
    // QAbstractScrollArea: a right-click lands on the viewport first, but the
    // viewport's own policy never runs -- the scroll area's event filter
    // consumes viewport events ahead of QWidget::event() -- and the event
    // then propagates up to here. Setting the policy on viewport() compiles,
    // runs, and silently produces no menu.
    terminal_->installEventFilter(this);

    // A SECOND filter, on the viewport. QAbstractScrollArea delivers wheel
    // events there and not to the widget, so the filter above -- which is
    // watching key events -- would never see one. See eventFilter().
    terminal_->viewport()->installEventFilter(this);
    terminal_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(terminal_, &QWidget::customContextMenuRequested, this,
            &TerminalTab::showContextMenu);
    connect(terminal_, &TerminalView::pasteRequested, this,
            &TerminalTab::paste);
    stack->addWidget(terminal_, 0, 0);

    // Centred rather than filling the cell. A full-bleed overlay hides the
    // scrollback behind it, and there is nothing to hide before a session has
    // written anything -- but there is once a reconnect is in flight over a
    // screen full of output somebody wants to keep reading.
    overlay_ = new QFrame(this);
    overlay_->setObjectName(QStringLiteral("overlayPanel"));
    overlay_->setVisible(false);

    // NOT WA_TransparentForMouseEvents any more: the Cancel button has to be
    // clickable. Keyboard focus is a different matter -- the panel and its
    // button both refuse it, so the terminal keeps focus while an overlay is
    // up and a keystroke still reaches the far end.
    overlay_->setFocusPolicy(Qt::NoFocus);

    auto *overlayLayout = new QVBoxLayout(overlay_);
    overlayLayout->setContentsMargins(16, 12, 16, 12);
    overlayLayout->setSpacing(8);

    overlayText_ = new QLabel(overlay_);
    overlayText_->setAlignment(Qt::AlignCenter);

    // WRAPPED, AND CAPPED. Without both, the overlay's width is the width of
    // the longest line in the message, and the panel is inside the tab, so
    // that width becomes a minimum for the tab, the splitter and finally the
    // window -- which grows to fit and can end up wider than the screen it is
    // on. A dial error is the one message here with no length bound: it comes
    // from the far end wrapped in the transport's own context, and the
    // credential rejection is longer than anything the overlay carried before.
    //
    // The cap is on the label rather than the panel because word wrap alone
    // does not do it: a QLabel that may wrap still reports a sizeHint for the
    // unwrapped text, and the layout believes it.
    overlayText_->setWordWrap(true);
    overlayText_->setMaximumWidth(kOverlayMaxWidth);
    overlayLayout->addWidget(overlayText_);

    overlayCancel_ = new QPushButton(tr("Cancel"), overlay_);
    overlayCancel_->setFocusPolicy(Qt::NoFocus);
    overlayCancel_->setVisible(false);
    connect(overlayCancel_, &QPushButton::clicked, this,
            &TerminalTab::cancelConnect);
    overlayLayout->addWidget(overlayCancel_, 0, Qt::AlignCenter);

    stack->addWidget(overlay_, 0, 0, Qt::AlignCenter);

    spinnerTimer_ = new QTimer(this);
    spinnerTimer_->setInterval(kSpinnerIntervalMs);
    connect(spinnerTimer_, &QTimer::timeout, this, &TerminalTab::tickSpinner);

    // Before any theme lands. See the note on selectionAlpha_.
    selectionAlpha_ = theme::defaultSelectionAlpha(terminal_);

    pasteTimer_ = new QTimer(this);
    pasteTimer_->setSingleShot(true);
    connect(pasteTimer_, &QTimer::timeout, this,
            &TerminalTab::sendNextPasteChunk);

    // Single-shot and restarted on every write rather than a repeating tick:
    // the thing being measured is silence from this end, so each byte sent
    // pushes the deadline out. A repeating timer would fire in the middle of
    // a four-minute paced paste, which is the one moment the far end is least
    // idle.
    antiIdleTimer_ = new QTimer(this);
    antiIdleTimer_->setSingleShot(true);
    connect(antiIdleTimer_, &QTimer::timeout, this, &TerminalTab::sendAntiIdle);

    session_ = new omegassh::OmegaSshSession(this);

    // The three connections, made by the same function terminal_window uses.
    // Getting one wrong produces a terminal that looks almost right, and the
    // third -- resize -- is the one people forget.
    omegassh::attach(terminal_, session_);

    // Rides alongside the resize connection attach() just made, for the same
    // reason capture rides alongside dataReceived: attach() stays the one
    // description of the wiring. What this adds is memory -- the widget
    // announces a grid change and forgets it, and a session that starts after
    // the last announcement has no way to ask.
    connect(terminal_, &qtpyte::TerminalWidget::resized, this,
            [this](int cols, int rows) {
                if (cols > 0 && rows > 0) {
                    lastCols_ = cols;
                    lastRows_ = rows;
                }
            });

    sizeSyncTimer_ = new QTimer(this);
    sizeSyncTimer_->setSingleShot(true);
    sizeSyncTimer_->setInterval(kSizeSyncDelayMs);
    connect(sizeSyncTimer_, &QTimer::timeout, this,
            &TerminalTab::syncRemoteSize);

    // One tap for everything this end sends. attach() already routes
    // dataReady to the session; this rides alongside rather than between, the
    // same arrangement capture has with dataReceived -- so a keystroke, a
    // paste chunk and a synthesised anti-idle all reset the clock without the
    // wiring having a second description.
    connect(terminal_, &qtpyte::TerminalWidget::dataReady, this,
            [this](const QByteArray &) { noteOutbound(); });

    // Capture taps the same signal rather than sitting between the two, so
    // attach() stays the one description of the wiring and a capture that
    // failed can never cost the terminal its bytes.
    connect(session_, &omegassh::OmegaSshSession::dataReceived, this,
            [this](const QByteArray &data) { capture_.write(data); });

    connect(session_, &omegassh::OmegaSshSession::stateChanged, this,
            &TerminalTab::onStateChanged);
    connect(session_, &omegassh::OmegaSshSession::errorOccurred, this,
            [this](const QString &message) { error_ = message; });
    connect(session_, &omegassh::OmegaSshSession::finished, this,
            [this](int code) {
                // A teardown that this tab asked for as part of a host key
                // re-dial is not the session ending. Forwarding it would have
                // the window mark the tab "(closed)" a moment before it
                // reconnects.
                if (redialing_) {
                    return;
                }
                emit finished(code);
            });
}

bool TerminalTab::start(const omegassh::Config &config) {
    host_ = config.transport == omegassh::Transport::Serial ? config.serialPort
                                                            : config.host;
    pasteBaud_ = defaultPasteBaud(config);

    // The overlay goes up BEFORE the dial, not when the first state arrives.
    // start() is asynchronous, so the window is already painted when it
    // returns -- and a tab that showed nothing until the first event would
    // flash empty on a fast connect and look hung on a slow one.
    cancelled_ = false;
    config_ = config;
    haveConfig_ = true;

    // The pty is requested at the size the terminal actually is, not at
    // Config's 80x24. The window has already added this tab and made it
    // current by the time start() runs, but the layout that gives the widget
    // its geometry is posted, not immediate -- so on a first dial the widget
    // has not been sized yet and has emitted no resized(). Forcing the
    // window's layout through fixes that: the geometry lands synchronously,
    // the widget's resizeEvent runs, and the tap above has a grid to cache
    // before the next line reads it.
    //
    // If it still comes back empty -- a tab constructed off-screen, a window
    // not yet shown -- the dial goes out at the Config's size and
    // syncRemoteSize() corrects it when the session lands.
    if (QWidget *top = window()) {
        if (QLayout *l = top->layout()) {
            l->activate();
        }
    }
    if (lastCols_ > 0 && lastRows_ > 0) {
        config_.cols = lastCols_;
        config_.rows = lastRows_;
    }

    setOverlayMessage(stateLabel(omegassh::State::Connecting), true, true);

    // The ribbon describes what was dialled, so it is filled from the config
    // rather than from anything the far end says -- which means it is right
    // from the first frame instead of after the first state change.
    ribbon_->setSession(title(), config_);
    ribbon_->setVisible(true);

    // config_, not config: the grid was stamped into the copy, and dialing the
    // argument would put 80x24 back on the wire while reconnect() -- which
    // already dials config_ -- used the right one.
    if (!session_->start(config_)) {
        error_ = session_->error();
        // A configuration mistake, decided before a socket opened. Nothing is
        // in flight, so there is nothing to cancel and no spinner to turn.
        setOverlayMessage(
            error_.isEmpty() ? stateLabel(omegassh::State::Failed) : error_,
            false, false);
        return false;
    }
    return true;
}

QString TerminalTab::title() const {
    if (!displayName_.isEmpty()) return displayName_;
    const QString summary = session_->summary();
    return summary.isEmpty() ? tr("(no session)") : summary;
}

void TerminalTab::setDisplayName(const QString &name) {
    displayName_ = name.trimmed();
}

QString TerminalTab::error() const {
    return error_.isEmpty() ? session_->error() : error_;
}

bool TerminalTab::isRunning() const { return session_->running(); }

omegassh::State TerminalTab::state() const { return session_->state(); }

void TerminalTab::terminate() {
    // The capture closes with the session rather than at destruction: a tab
    // is deliberately left open after a session ends so its scrollback can be
    // read, and a log file held open that whole time is a log file nothing
    // else can rotate.
    stopCapture();
    cancelPaste();
    spinnerTimer_->stop();
    antiIdleTimer_->stop();
    sizeSyncTimer_->stop();
    session_->terminate();
}

bool TerminalTab::canReconnect() const {
    return haveConfig_ && !session_->running();
}

void TerminalTab::reconnect() {
    if (!canReconnect()) {
        return;
    }

    // Everything the last attempt left behind, cleared -- the prompt-once
    // guard included. That guard exists so the AUTOMATIC re-dial cannot loop
    // inside a single attempt; it was never meant to make an unknown host key
    // unanswerable for the life of the tab. A person clicking Reconnect is a
    // new attempt, and gets the question again if the key is still unknown.
    error_.clear();
    hostKeyError_.clear();
    hostKeyPrompted_ = false;
    cancelled_ = false;

    // Same reasoning as the host key guard above, one step further: the cap
    // bounds the AUTOMATIC re-prompting inside one attempt. A person clicking
    // Reconnect is a new attempt and gets a fresh three.
    authPrompts_ = 0;
    authError_.clear();

    // The same suppression the host key re-dial uses, and for the same reason:
    // by now the session has usually released its handle on its own, but
    // "usually" is doing work in that sentence, and a teardown that does fire
    // finished() here would have the window write "(closed)" over a tab that is
    // about to be connecting.
    redialing_ = true;
    session_->terminate();
    redialing_ = false;

    setOverlayMessage(stateLabel(omegassh::State::Connecting), true, true);

    // Re-filled, because a reconnect can follow a setDisplayName and the
    // ribbon's name would otherwise be the one from the first dial.
    ribbon_->setSession(title(), config_);
    if (!session_->start(config_)) {
        // Refused before a socket opened. The tab text stays "(closed)"
        // because nothing started -- the reason is on the overlay, which is
        // where the first dial's would have been too.
        error_ = session_->error();
        setOverlayMessage(
            error_.isEmpty() ? stateLabel(omegassh::State::Failed) : error_,
            false, false);
        return;
    }

    // After start(), so summary() has been repopulated and title() is the new
    // session's rather than the one that just ended.
    emit restarted();
}

// -----------------------------------------------------------------------------
// Clipboard
// -----------------------------------------------------------------------------

void TerminalTab::copy() { terminal_->copySelection(); }

void TerminalTab::paste() {
    const QString text = QApplication::clipboard()->text();
    if (text.isEmpty()) {
        return;
    }

    const int lineCount = static_cast<int>(text.count(QLatin1Char('\n')));
    if (pasteThreshold_ > 0 && lineCount > pasteThreshold_) {
        const QStringList lines = text.split(QLatin1Char('\n'));
        QString preview = lines.mid(0, kPreviewLines).join(QLatin1Char('\n'));
        if (lines.size() > kPreviewLines) {
            // %1 and not Qt's %n plural form: "(s)" only resolves through an
            // installed translator, and with none the literal parenthesis
            // renders.
            preview += tr("\n... (%1 more lines)")
                           .arg(static_cast<int>(lines.size()) - kPreviewLines);
        }

        PasteConfirmDialog dialog(preview, static_cast<int>(lines.size()),
                                  static_cast<int>(text.size()), pasteBaud_,
                                  this);
        const bool accepted = dialog.exec() == QDialog::Accepted;
        // Focus back either way. A dialog that leaves the terminal unfocused
        // on Cancel makes a refused paste look like a hung session.
        terminal_->setFocus();
        if (!accepted) {
            return;
        }
        // The rate chosen on the modal sticks for the tab. Somebody who has
        // just decided 9600 is right for this device has decided it for the
        // next paste too, and having to say so again every time is how a
        // control on a modal becomes an annoyance.
        setPasteBaud(dialog.selectedBaud());
    }

    if (pasteBaud_ > 0) {
        beginThrottledPaste(text);
        return;
    }

    // Through the widget, not through the session: it converts newlines and
    // wraps the text in bracketed-paste markers when the far end has asked
    // for them, and the result reaches the session the way a keystroke does.
    terminal_->paste(text);
}

// -----------------------------------------------------------------------------
// Rate-limited paste
// -----------------------------------------------------------------------------

void TerminalTab::setPasteBaud(int baud) { pasteBaud_ = qMax(0, baud); }

bool TerminalTab::isPasting() const { return pasteQueue_.hasNext(); }

void TerminalTab::cancelPaste() {
    if (!pasteQueue_.hasNext()) {
        return;
    }
    const int sent = pasteQueue_.sent();
    const int total = pasteQueue_.total();
    pasteTimer_->stop();
    pasteQueue_.clear();
    emit statusChanged(tr("Paste cancelled after %1 of %2 lines").arg(sent).arg(total));
}

bool TerminalTab::eventFilter(QObject *watched, QEvent *event) {
    // Escape cancels a paste that is still running, and is swallowed when it
    // does. Taking a key away from the far end needs a better reason than
    // convenience, and this is it: a 400-line block at 60 ms is four minutes
    // during which every keystroke is queued behind lines that are still
    // being sent, so the ordinary way out -- type something -- is exactly the
    // thing that does not work. Outside a paste the key is untouched, which
    // is every keystroke of a normal session.
    // Ctrl+0 puts the size back. A temporary zoom with no way home except
    // closing the tab is a trap, and there is nowhere else to read the size
    // off. This does take Ctrl+0 from the far end; almost nothing binds it,
    // and every browser and terminal that zooms uses it for exactly this.
    if (watched == terminal_ && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_0 &&
            key->modifiers() == Qt::ControlModifier) {
            resetZoom();
            return true;
        }
    }

    if (watched == terminal_ && event->type() == QEvent::KeyPress &&
        pasteQueue_.hasNext()) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape && key->modifiers() == Qt::NoModifier) {
            cancelPaste();
            return true;
        }
    }
    // --- Ctrl + wheel: temporary font size for this tab ---------------------
    //
    // FIRST, ahead of the alternate-screen branch below. Ctrl+wheel has to
    // mean the same thing everywhere -- at a prompt and inside vi alike -- and
    // checking it second would mean a full-screen application swallowed it as
    // arrow keys, so zoom would work at a shell and mysteriously not in the
    // editor where you most want it.
    //
    // Deliberately not gated on wheelAltScreen_: that setting is about what
    // an unmodified wheel does, and someone who turned it off did not ask to
    // lose zoom.
    if (watched == terminal_->viewport() && event->type() == QEvent::Wheel) {
        auto *wheel = static_cast<QWheelEvent *>(event);
        if (wheel->modifiers().testFlag(Qt::ControlModifier)) {
            zoomAccum_ += wheel->angleDelta().y();
            const int notches = zoomAccum_ / 120;
            if (notches != 0) {
                zoomAccum_ -= notches * 120;
                zoomBy(notches);
            }
            // Consumed either way, including the sub-notch case: letting a
            // partial Ctrl+wheel fall through would scroll the scrollback,
            // which is not what the modifier asked for.
            return true;
        }
    }

    // --- wheel over a full-screen application -------------------------------
    //
    // ON THE VIEWPORT, NOT THE WIDGET. TerminalWidget is a QAbstractScrollArea
    // and wheel events go to viewport(); a filter installed on the widget
    // itself never sees one. The Escape filter above is on the widget because
    // key events do go there, so the two watch different objects on purpose.
    //
    // Returning true here is what stops QAbstractScrollArea scrolling: the
    // base class has no wheelEvent override of its own in anytermqt, so the
    // default scrollback behaviour is the thing being suppressed.
    if (wheelAltScreen_ && watched == terminal_->viewport() &&
        event->type() == QEvent::Wheel && terminal_->alternateScreen()) {
        if (dispatchAltScreenWheel(static_cast<QWheelEvent *>(event))) {
            return true;
        }
    }

    return QWidget::eventFilter(watched, event);
}

void TerminalTab::beginThrottledPaste(const QString &text) {
    // A second paste replaces the first rather than interleaving with it.
    // Two blocks arriving line-alternately would be worse than either.
    cancelPaste();

    pasteQueue_.load(text);
    sendNextPasteChunk();
}

void TerminalTab::sendNextPasteChunk() {
    if (!pasteQueue_.hasNext()) {
        return;
    }

    const QString chunk = pasteQueue_.next();

    // send(), not paste(): this is emulating typing, one line at a time, and
    // send() applies the same newline conversion without the bracketed-paste
    // markers. Those markers wrap a paste, and there is no honest way to wrap
    // one that is being delivered in pieces -- the far end would see the
    // start marker, then a gap, then more input. Nothing that needs a console
    // cable asks for bracketed paste anyway; anything that does is on SSH,
    // where the delay is 0 and this path is not taken.
    terminal_->send(chunk);

    if (pasteQueue_.hasNext()) {
        emit statusChanged(tr("Pasting line %1 of %2 at %3 baud -- Esc to cancel")
                               .arg(pasteQueue_.sent())
                               .arg(pasteQueue_.total())
                               .arg(pasteBaud_));
        // Paced on what was just sent, not on a flat interval: a 4-character
        // line and an 80-character line do not cost the far end the same.
        pasteTimer_->start(
            pasteDelayMs(static_cast<int>(chunk.size()), pasteBaud_));
        return;
    }

    emit statusChanged(tr("Pasted %1 lines").arg(pasteQueue_.total()));
    pasteQueue_.clear();
}

void TerminalTab::copyPaste() {
    copy();
    paste();
}

void TerminalTab::setMultilinePasteThreshold(int lines) {
    pasteThreshold_ = qMax(0, lines);
}

void TerminalTab::setScrollbackLines(int lines) {
    terminal_->setScrollbackSize(qMax(0, lines));
}

void TerminalTab::setThemeOverride(const QString &name) {
    themeOverride_ = name.trimmed();
    // No re-apply here on purpose. The tab cannot resolve a name, so asking
    // it to repaint would mean repainting with the theme it already has --
    // which looks like the override being ignored. The window resolves and
    // calls applyTheme(), and openTab() sets this before it does.
}

void TerminalTab::setFontPointSize(int points) {
    baseFontPointSize_ = points > 0 ? points : 0;
    applyFontSize();
}

// The size the terminal is actually showing, base plus whatever Ctrl+wheel has
// added. Zero only while the base is zero and nothing has zoomed, which means
// "whatever the theme picked" and is what applyTheme() checks for.
int TerminalTab::effectiveFontPointSize() const {
    if (baseFontPointSize_ <= 0 && zoomDelta_ == 0) return 0;

    // A tab on the theme's own size still has to zoom from somewhere. The
    // widget's current font is that somewhere -- read rather than assumed, so
    // a theme with an unusual size zooms from where it actually is.
    const int base = baseFontPointSize_ > 0
                         ? baseFontPointSize_
                         : terminal_->terminalFont().pointSize();
    return qBound(kMinFontPointSize, base + zoomDelta_, kMaxFontPointSize);
}

void TerminalTab::applyFontSize() {
    // Re-applied through the theme path rather than by poking the font here,
    // so there is one description of what a terminal's font is made of.
    if (const theme::Theme *t = themeApplied_) {
        applyTheme(*t);
    }
}

void TerminalTab::zoomBy(int steps) {
    if (steps == 0) return;

    const int before = effectiveFontPointSize();
    zoomDelta_ += steps;

    // Clamp the DELTA against the bounds, not just the result. Without this,
    // scrolling far past the maximum builds up a delta that has to be unwound
    // notch by notch before the size moves again, which reads as the zoom
    // having stuck.
    const int base = baseFontPointSize_ > 0
                         ? baseFontPointSize_
                         : terminal_->terminalFont().pointSize();
    zoomDelta_ = qBound(kMinFontPointSize - base, zoomDelta_,
                        kMaxFontPointSize - base);

    const int after = effectiveFontPointSize();
    if (after == before) return;

    applyFontSize();

    // The only feedback there is: nothing else says what size the terminal is
    // now, and the change is not written anywhere the operator could look.
    emit statusChanged(zoomDelta_ == 0
                           ? tr("Font size %1 pt (default)").arg(after)
                           : tr("Font size %1 pt").arg(after));
}

void TerminalTab::resetZoom() {
    if (zoomDelta_ == 0) return;
    zoomDelta_ = 0;
    zoomAccum_ = 0;
    applyFontSize();
    emit statusChanged(
        tr("Font size %1 pt (default)").arg(effectiveFontPointSize()));
}

// -----------------------------------------------------------------------------
// Anti-idle
// -----------------------------------------------------------------------------

void TerminalTab::setAntiIdle(const AntiIdleConfig &config) {
    antiIdle_ = config;
    rearmAntiIdle();
}

void TerminalTab::noteOutbound() {
    // Every write pushes the deadline out, which is the whole mechanism: what
    // a device's idle timer counts is input from this end.
    rearmAntiIdle();
}

void TerminalTab::rearmAntiIdle() {
    // Armed on the CONFIG alone, not on antiIdleAllowed(). The suppressions --
    // alternate screen, paste in flight -- are conditions at the moment of
    // firing, and a timer stopped because htop was up would never restart when
    // htop exited: nothing writes on the way out of a full-screen application.
    if (!antiIdle_.enabled || antiIdle_.seconds <= 0 ||
        antiIdleBytes(antiIdle_).isEmpty()) {
        antiIdleTimer_->stop();
        return;
    }
    antiIdleTimer_->start(antiIdle_.seconds * 1000);
}

void TerminalTab::sendAntiIdle() {
    const bool connected = session_->state() == omegassh::State::Connected;
    if (antiIdleAllowed(antiIdle_, connected, terminal_->alternateScreen(),
                        isPasting())) {
        // Straight to the session rather than through terminal_->send(): send()
        // would emit dataReady, which the tap above would read as the operator
        // typing and the clock would restart on the tab's own keystroke. It
        // restarts below either way; going around the signal keeps the tap
        // meaning "the far end heard from a person".
        session_->write(antiIdleBytes(antiIdle_));
    }

    // Re-armed whether or not anything was sent, so a session that spends an
    // hour in htop starts sending again the moment it leaves.
    rearmAntiIdle();
}

// -----------------------------------------------------------------------------
// Capture
// -----------------------------------------------------------------------------

bool TerminalTab::startCapture(const QString &path) {
    if (capture_.start(path)) {
        return true;
    }
    error_ = capture_.error();
    return false;
}

void TerminalTab::stopCapture() { capture_.stop(); }

QString TerminalTab::suggestedCapturePath() const {
    const QString stem = host_.isEmpty() ? QStringLiteral("session")
                                         : filenameSafe(host_);
    return QDir::home().filePath(QStringLiteral("session_%1.log").arg(stem));
}

void TerminalTab::toggleCapture() {
    if (capture_.isCapturing()) {
        const QString finished = capture_.path();
        stopCapture();
        emit statusChanged(tr("Capture saved: %1").arg(finished));
        return;
    }

    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save Session Capture"), suggestedCapturePath(),
        tr("Log Files (*.log *.txt);;All Files (*)"));
    terminal_->setFocus();
    if (path.isEmpty()) {
        return;
    }

    if (!startCapture(path)) {
        emit statusChanged(tr("Capture failed: %1").arg(error_));
        return;
    }
    emit statusChanged(tr("Capturing to %1").arg(path));
}

// -----------------------------------------------------------------------------
// Context menu
// -----------------------------------------------------------------------------

void TerminalTab::showContextMenu(const QPoint &pos) {
    QMenu menu(this);

    // First, and only on a session that has ended. On a live tab it would be
    // an entry that does nothing, sitting above the three that people right
    // click for; on a dead one it is the only thing there is to do, and the
    // alternative has been closing the tab and losing its scrollback.
    if (canReconnect()) {
        connect(menu.addAction(tr("&Reconnect")), &QAction::triggered, this,
                &TerminalTab::reconnect);
        menu.addSeparator();
    }

    QAction *copyAction = menu.addAction(tr("&Copy"));
    copyAction->setEnabled(terminal_->hasSelection());
    connect(copyAction, &QAction::triggered, this, &TerminalTab::copy);

    QAction *pasteAction = menu.addAction(tr("&Paste"));
    connect(pasteAction, &QAction::triggered, this, &TerminalTab::paste);

    QAction *copyPasteAction = menu.addAction(tr("Copy && Paste"));
    copyPasteAction->setEnabled(terminal_->hasSelection());
    connect(copyPasteAction, &QAction::triggered, this, &TerminalTab::copyPaste);

    if (pasteQueue_.hasNext()) {
        QAction *cancel = menu.addAction(tr("Cancel Paste (%1 of %2 lines sent)")
                                             .arg(pasteQueue_.sent())
                                             .arg(pasteQueue_.total()));
        connect(cancel, &QAction::triggered, this, &TerminalTab::cancelPaste);
    }

    // Per tab and not persisted -- see setPasteLineDelay in the header for
    // why it cannot go in the shared config file.
    // The same choice the confirmation dialog offers, for a paste that is too
    // short to raise one.
    QMenu *speed = menu.addMenu(tr("Paste Speed"));
    auto *speedGroup = new QActionGroup(speed);
    speedGroup->setExclusive(true);
    static const int kSpeeds[] = {0, 1200, 9600, 19200, 38400, 115200};
    for (int baud : kSpeeds) {
        QAction *action = speed->addAction(baud == 0 ? tr("Unlimited")
                                                     : tr("%1 baud").arg(baud));
        action->setCheckable(true);
        action->setChecked(baud == pasteBaud_);
        speedGroup->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, baud] { setPasteBaud(baud); });
    }
    // A rate set from somewhere other than this menu -- a serial session
    // defaulted to its port's own baud -- may not be one of the six. Give it
    // an entry rather than showing six unchecked ones and leaving the current
    // value invisible.
    if (speedGroup->checkedAction() == nullptr) {
        QAction *current = speed->addAction(tr("%1 baud").arg(pasteBaud_));
        current->setCheckable(true);
        current->setChecked(true);
        speedGroup->addAction(current);
    }

    menu.addSeparator();

    QAction *captureAction = menu.addAction(
        capture_.isCapturing()
            ? tr("Stop Capture (%1)").arg(QFileInfo(capture_.path()).fileName())
            : tr("Start Capture..."));
    connect(captureAction, &QAction::triggered, this, &TerminalTab::toggleCapture);

    menu.addSeparator();

    connect(menu.addAction(tr("Select &All")), &QAction::triggered, terminal_,
            &qtpyte::TerminalWidget::selectAll);
    connect(menu.addAction(tr("Clear Terminal")), &QAction::triggered, this,
            [this] { terminal_->feed(kClearSequence); });

    menu.exec(terminal_->mapToGlobal(pos));

    // The menu took focus on the way in. Without this the next keystroke goes
    // nowhere, which is the same symptom as the Tab bug and easy to mistake
    // for it.
    terminal_->setFocus();
}

void TerminalTab::setOverlayVisible(bool visible) {
    overlay_->setVisible(visible);
    overlay_->raise();
}

// One place that decides what the tree row should say, and one signal out of
// it. Called from the state handler and from setNeedsInput, because a prompt
// going up does not change the transport's state and a state change does not
// clear a prompt.
void TerminalTab::publishLink() {
    // NeedsInput overrides whatever the transport thinks, and only while the
    // prompt is up: the transport is genuinely sitting in Authenticating
    // underneath, but "authenticating" on a row that is waiting for a
    // passphrase describes the machine rather than the person.
    const Link link = needsInput_ ? Link::NeedsInput
                                  : linkFromState(session_->state());
    if (link == link_) return;
    link_ = link;
    if (ribbon_) {
        ribbon_->setLink(link_);
        ribbon_->setReconnectVisible(canReconnect());
    }
    emit linkChanged(link_);
}

void TerminalTab::setNeedsInput(bool needsInput) {
    if (needsInput_ == needsInput) return;
    needsInput_ = needsInput;
    publishLink();
}

void TerminalTab::syncRemoteSize() {
    if (!session_->running()) {
        return;
    }
    if (lastCols_ <= 0 || lastRows_ <= 0) {
        // The widget has never reported a grid. Nothing to send that would be
        // better than what the dial already asked for, and resize(0, 0) is
        // dropped on the Go side anyway.
        return;
    }
    session_->resize(lastCols_, lastRows_);

    // So a reconnect in this tab dials at the size the tab is now rather than
    // the size it was when it first opened.
    config_.cols = lastCols_;
    config_.rows = lastRows_;
}

void TerminalTab::onStateChanged(omegassh::State s) {
    publishLink();

    // A session that just came up starts its idle clock now. Arming it at
    // start() instead would have the first keystroke land against a clock
    // that began during the dial, which on slow gear is most of the interval.
    if (s == omegassh::State::Connected) {
        rearmAntiIdle();

        // The credentials worked. A later disconnect on this same tab starts
        // its own three rather than inheriting a count from a password the
        // operator mistyped once and then got right.
        authPrompts_ = 0;
        authError_.clear();

        // The answers got us in and are spent. A later disconnect on this tab
        // must ask again rather than resubmit a used one-time password.
        keyboardPrompts_ = 0;
        keyboardError_.clear();
        lastKeyboardQuestion_.clear();
        config_.keyboardAnswers.clear();

        // Before the redialing_ return below, deliberately: a host-key re-dial
        // is a new pty on the far end and needs telling the same as a first
        // one does.
        syncRemoteSize();
        sizeSyncTimer_->start();
    } else if (s == omegassh::State::Failed ||
               s == omegassh::State::Disconnected) {
        antiIdleTimer_->stop();
    }

    // The disconnect between the failed dial and the re-dial is bookkeeping,
    // not something to report.
    if (redialing_) {
        return;
    }

    if (s == omegassh::State::Connected) {
        setOverlayVisible(false);
        spinnerTimer_->stop();
        emit statusChanged(stateLabel(s));
        return;
    }

    // A dial is in flight in exactly these three, and they are the three where
    // Cancel means something: terminate() returns immediately mid-dial, so the
    // button is honest rather than decorative.
    const bool dialing = s == omegassh::State::Connecting ||
                         s == omegassh::State::Authenticating ||
                         s == omegassh::State::Reconnecting;

    QString message = stateLabel(s);
    if (s == omegassh::State::Failed && !error().isEmpty()) {
        // First contact with a host is not really a failure, it is a question
        // nobody asked. If the answer is yes this tab dials again and the
        // overlay never settles on the error.
        if (!cancelled_ && offerHostKeyPrompt()) {
            return;
        }
        // And the second question nobody asked: credentials the far end
        // refused. Checked AFTER the host key, because a host key failure
        // also arrives during the handshake and answering it with a password
        // box would be a dialog over the wrong problem. isAuthFailure()
        // guarantees the two do not overlap -- see autherror.h.
        // Before the credential branch: a keyboard-interactive question is
        // not a rejection, and the two must not compete for the same failure.
        // sshcore excludes the marker from IsAuthFailure so they cannot both
        // match, but ordering it first means the specific prompt wins even if
        // that ever stopped being true.
        if (!cancelled_ && offerKeyboardPrompt()) {
            return;
        }
        if (!cancelled_ && offerCredentialPrompt()) {
            return;
        }
        message = error();
    }
    // A disconnect that somebody asked for is not news to them. Saying so
    // beats reporting it as though the far end had hung up.
    if (cancelled_ && !dialing) {
        message = tr("Cancelled");
    }

    // The overlay stays up when a session is not connected, Failed included:
    // the reason is the only thing on screen when a dial produced no output
    // to read.
    setOverlayMessage(message, dialing, dialing);
}

// -----------------------------------------------------------------------------
// Overlay
// -----------------------------------------------------------------------------

void TerminalTab::setOverlayMessage(const QString &text, bool spinning,
                                    bool cancellable) {
    overlayMessage_ = text;
    overlayCancel_->setVisible(cancellable);

    if (spinning) {
        if (!spinnerTimer_->isActive()) {
            spinnerFrame_ = 0;
            spinnerTimer_->start();
        }
    } else {
        spinnerTimer_->stop();
    }

    overlayText_->setText(spinning ? QStringLiteral("%1  %2")
                                         .arg(spinnerFrames().at(spinnerFrame_))
                                         .arg(text)
                                   : text);
    setOverlayVisible(true);
    emit statusChanged(text);
}

void TerminalTab::tickSpinner() {
    spinnerFrame_ = (spinnerFrame_ + 1) % spinnerFrames().size();
    overlayText_->setText(QStringLiteral("%1  %2")
                              .arg(spinnerFrames().at(spinnerFrame_))
                              .arg(overlayMessage_));
}

bool TerminalTab::offerHostKeyPrompt() {
    if (hostKeyPrompted_) {
        return false;
    }

    const HostKeyInfo info = parseHostKeyError(error());
    if (!info.unknownHost) {
        // Every other host key failure included -- a key that does not match
        // the pinned one is the case that must stay fatal.
        return false;
    }

    hostKeyPrompted_ = true;
    redialing_ = true;
    hostKeyError_ = error();
    setOverlayMessage(tr("Unknown host key"), false, false);

    // Deferred to a clean stack. This runs inside the session delivering its
    // own events, and finishHostKeyPrompt() tears that session down -- doing
    // that from within its call stack is re-entrancy waiting to bite. It is
    // also what makes the modal safe: exec() spins a nested event loop, and
    // spinning one inside a signal handler for the object about to be
    // destroyed is worse again.
    QTimer::singleShot(0, this, [this, info] { finishHostKeyPrompt(info); });
    return true;
}

void TerminalTab::finishHostKeyPrompt(const HostKeyInfo &info) {
    // start() refuses while a handle is open -- "session is already running"
    // -- which is what a re-dial raised from inside the failure hit the first
    // time this was written.
    //
    // By the time this runs the session has usually released the handle
    // itself: draining the failure event finishes it. So this is normally a
    // no-op, terminate() returning early on a handle that is already gone.
    // It stays because "usually" is doing work in that sentence, and the
    // failure mode without it is a prompt that leads nowhere.
    session_->terminate();

    HostKeyPromptDialog dialog(info, this);
    const bool accepted = dialog.exec() == QDialog::Accepted;
    terminal_->setFocus();

    if (!accepted) {
        redialing_ = false;
        error_ = hostKeyError_;
        setOverlayMessage(error_, false, false);
        // The session really is over now, so the window hears about it after
        // all -- the suppression above was only for the teardown in between.
        emit finished(session_->exitCode());
        return;
    }

    // TOFU on the re-dial is what writes the key: capi flattens the policy to
    // auto-accept, and sshcore appends the entry to known_hosts on the way
    // through. Nothing else about the configuration changes, so the second
    // dial is the first one with the question answered.
    error_.clear();
    omegassh::Config retry = config_;
    retry.hostKeyPolicy = omegassh::HostKeyPolicy::Tofu;

    // Cleared BEFORE start(), so the new dial's states are reported normally.
    redialing_ = false;
    setOverlayMessage(stateLabel(omegassh::State::Connecting), true, true);

    // The retry config, not the original: a host-key re-dial changes the
    // policy and the ribbon should describe what is actually being dialled.
    ribbon_->setSession(title(), retry);
    if (!session_->start(retry)) {
        error_ = session_->error();
        setOverlayMessage(error_, false, false);
    }
}

// -----------------------------------------------------------------------------
// Credential retry
// -----------------------------------------------------------------------------

QString TerminalTab::target() const {
    // title() already resolves display name over summary, which is exactly the
    // rule a prompt title wants. This exists as its own name because "what to
    // call the tab" and "what to tell the operator we are connecting to"
    // happen to agree today and are not the same question.
    return title();
}

bool TerminalTab::offerCredentialPrompt() {
    // A tab that has been answering keyboard-interactive questions never
    // offers a username and password box, even once the keyboard attempts are
    // spent. The far end on such a host may not accept a password at all --
    // publickey plus keyboard-interactive is the usual shape -- so the dialog
    // would ask for a credential that cannot work, on top of a failure whose
    // real cause is a refused one-time code. Settling on the error says more.
    if (!lastKeyboardQuestion_.isEmpty()) {
        return false;
    }

    if (authPrompts_ >= AuthPromptLimit) {
        // Asked as often as this tab may. The overlay settles on the rejection
        // and Reconnect is the way to try again, which resets the count.
        return false;
    }

    if (!isAuthFailure(error())) {
        return false;
    }

    ++authPrompts_;
    redialing_ = true;
    authError_ = error();

    // The tab is waiting on a person now. The transport has no state for that
    // -- see linkstate.h -- so this is the only thing that knows.
    setNeedsInput(true);
    setOverlayMessage(tr("Authentication failed"), false, false);

    // Deferred to a clean stack, for both the reasons offerHostKeyPrompt()
    // gives. This one is emitted rather than raised: the window owns the
    // vault, so the window owns the dialog.
    QTimer::singleShot(0, this, [this] {
        // start() refuses while a handle is open. By the time this runs the
        // session has usually released it -- draining the failure event
        // finishes it -- but "usually" is doing work in that sentence, and the
        // failure without this is a prompt that leads nowhere.
        session_->terminate();
        emit credentialsRejected(target());
    });
    return true;
}

bool TerminalTab::offerKeyboardPrompt() {
    if (keyboardPrompts_ >= KeyboardPromptLimit) {
        return false;
    }

    KeyboardPrompt prompt = parseKeyboardPrompt(error());

    // THE REJECTED-ANSWER CASE. The first dial fails with the question in it
    // and parses above. Every dial after that carries an answer, so if the
    // answer is wrong the far end refuses it and the failure comes back as an
    // ordinary credential rejection with no question in it -- indistinguishable,
    // from the message alone, from a bad password.
    //
    // What distinguishes it is local: this tab asked a question on this dial
    // and supplied the answer. So a rejection here means THAT answer was
    // wrong, and the thing to do is ask the same question again -- not offer
    // a username and password box, which on a key-plus-OTP host is a dialog
    // for credentials the far end does not even accept.
    if (!prompt.valid && !lastKeyboardQuestion_.isEmpty() &&
        isAuthFailure(error())) {
        // The answer is spent either way: a one-time code that was refused is
        // no more valid on the next attempt, and leaving it in would make the
        // re-dial resubmit it instead of what the operator types next.
        //
        // Only THIS question's answer. A stack that asked two takes two dials,
        // and the first answer is still needed to get back to the second
        // question.
        config_.keyboardAnswers.remove(lastKeyboardQuestion_);

        prompt.valid = true;
        prompt.question = lastKeyboardQuestion_;
        prompt.secret = lastKeyboardSecret_;
    }

    if (!prompt.valid) {
        return false;
    }

    lastKeyboardQuestion_ = prompt.question;
    lastKeyboardSecret_ = prompt.secret;
    ++keyboardPrompts_;
    redialing_ = true;
    keyboardError_ = error();

    setNeedsInput(true);
    setOverlayMessage(tr("Waiting for authentication"), false, false);

    QTimer::singleShot(0, this, [this, prompt] {
        session_->terminate();
        emit keyboardQuestion(target(), prompt.question, prompt.secret,
                              keyboardPrompts_, KeyboardPromptLimit);
    });
    return true;
}

void TerminalTab::retryWithKeyboardAnswer(const QString &question,
                                          const QString &answer) {
    setNeedsInput(false);
    terminal_->setFocus();

    // Keyed by the question verbatim -- the far end sends the same bytes on
    // the next dial and the library matches on them exactly.
    //
    // Accumulated rather than replaced: a PAM stack that asks two questions
    // takes two dials to get through, and the second dial has to answer the
    // first question again before it will be asked the second.
    config_.keyboardAnswers.insert(question, answer);

    error_.clear();
    redialing_ = false;
    setOverlayMessage(stateLabel(omegassh::State::Connecting), true, true);

    if (!session_->start(config_)) {
        error_ = session_->error();
        setOverlayMessage(
            error_.isEmpty() ? stateLabel(omegassh::State::Failed) : error_,
            false, false);
    }
}

void TerminalTab::abandonKeyboardRetry() {
    setNeedsInput(false);
    terminal_->setFocus();

    redialing_ = false;
    error_ = keyboardError_;
    setOverlayMessage(
        error_.isEmpty() ? stateLabel(omegassh::State::Failed) : error_, false,
        false);

    // The answers are per dial and the operator has stopped: keeping a spent
    // OTP would make the next Reconnect submit a dead token.
    config_.keyboardAnswers.clear();
    lastKeyboardQuestion_.clear();

    emit finished(session_->exitCode());
}

void TerminalTab::retryWithCredentials(const QString &credential,
                                       long long vaultHandle,
                                       const QString &username,
                                       const QString &password) {
    setNeedsInput(false);
    terminal_->setFocus();

    // Replacing the credentials, not rebuilding the config: a key path, a
    // jump host and the host key policy from the last attempt all still
    // apply. The vault reference is cleared when the operator typed
    // credentials instead, or the old entry would still be consulted for
    // whatever the typed fields left blank.
    config_.credential = credential;
    config_.vaultHandle = credential.isEmpty() ? 0 : vaultHandle;

    if (!username.isEmpty()) {
        config_.username = username;
    }
    if (!password.isEmpty()) {
        config_.password = password;
    }

    // Cleared BEFORE start(), so the new dial's states are reported normally.
    error_.clear();
    redialing_ = false;
    setOverlayMessage(stateLabel(omegassh::State::Connecting), true, true);

    // The ribbon describes what is actually being dialled, and the username
    // may have just changed.
    ribbon_->setSession(title(), config_);
    if (!session_->start(config_)) {
        error_ = session_->error();
        setOverlayMessage(
            error_.isEmpty() ? stateLabel(omegassh::State::Failed) : error_,
            false, false);
    }
}

void TerminalTab::abandonCredentialRetry() {
    setNeedsInput(false);
    terminal_->setFocus();

    redialing_ = false;
    error_ = authError_;
    setOverlayMessage(
        error_.isEmpty() ? stateLabel(omegassh::State::Failed) : error_, false,
        false);

    // The session really is over now, so the window hears about it after all
    // -- the suppression above was only for the teardown in between. Same
    // shape as a declined host key prompt.
    emit finished(session_->exitCode());
}

// Turns wheel motion over a full-screen application into arrow keys.
//
// WHY IT SYNTHESISES A KEY EVENT rather than writing "\033[A" itself. The
// bytes an arrow sends are not fixed: under DECCKM (CSI ?1h) cursor keys send
// SS3 -- ESC O A -- and every full-screen application that reads arrows sets
// it. anytermqt tracks that mode privately and has no getter, so a hand-built
// escape here would be right at a shell prompt and wrong inside the pager
// this feature exists for. Handing the widget a QKeyEvent makes it encode
// through its own keymap with its own modes, which is correct by
// construction and stays correct if those modes ever grow.
//
// Returns false when the motion did not amount to a whole line yet, in which
// case the event is left alone rather than swallowed.
bool TerminalTab::dispatchAltScreenWheel(QWheelEvent *event) {
    // angleDelta is in eighths of a degree; 120 is one notch on a mouse. A
    // trackpad sends much smaller values continuously, so the remainder is
    // CARRIED rather than rounded away -- discarding it means a trackpad
    // scrolls nothing at all, which is the usual way this kind of handler is
    // subtly broken.
    wheelAccum_ += event->angleDelta().y();

    const int linesPerNotch = QApplication::wheelScrollLines();
    const int notches = wheelAccum_ / 120;
    if (notches == 0) {
        // Consumed in the sense that we keep the remainder, but the base
        // class must not scroll the scrollback behind the application either.
        return true;
    }
    wheelAccum_ -= notches * 120;

    // Positive angleDelta is away from the user, which is up.
    const Qt::Key key = notches > 0 ? Qt::Key_Up : Qt::Key_Down;
    const int count = std::abs(notches) * std::max(1, linesPerNotch);

    for (int i = 0; i < count; ++i) {
        // Press and release both: an application reading through a terminal
        // only ever sees the encoded bytes, but sending a lone press leaves
        // the widget's own key handling half-fed if it ever grows state.
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
        QApplication::sendEvent(terminal_, &press);
        QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
        QApplication::sendEvent(terminal_, &release);
    }

    // Synthesised input is still input as far as an idle timer is concerned:
    // a person scrolling a pager for two minutes has not gone idle, and the
    // anti-idle keystroke that would otherwise fire lands at a pager prompt,
    // where any key advances or aborts the output.
    noteOutbound();
    return true;
}

void TerminalTab::setWheelAltScreen(bool enabled) {
    wheelAltScreen_ = enabled;
    wheelAccum_ = 0;
}

void TerminalTab::cancelConnect() {
    // Recorded before terminate(), because terminate() is what produces the
    // state change that reads it.
    cancelled_ = true;
    terminate();
}

void TerminalTab::applyTheme(const theme::Theme &t) {
    themeApplied_ = &t;
    // The ribbon's dot paints from tokens, which no stylesheet reaches.
    if (ribbon_) ribbon_->setTokens(theme::tokensFromTheme(t));
    theme::applyTheme(terminal_, t, selectionAlpha_);

    // The size settings asked for, over the family the theme asked for.
    // theme::applyTheme has just set the font, so this reads it back rather
    // than rebuilding it -- one place knows how a terminal font is assembled.
    const int points = effectiveFontPointSize();
    if (points > 0) {
        QFont font = terminal_->terminalFont();
        if (font.pointSize() != points) {
            font.setPointSize(points);
            terminal_->setTerminalFont(font);
        }
    }

    terminal_->refresh();
    overlay_->setStyleSheet(theme::overlayPanelStylesheet(t));
}

}  // namespace omega::app