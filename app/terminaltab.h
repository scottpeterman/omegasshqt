// app/terminaltab.h
//
// One tab: an anytermqt terminal widget with an omegassh session behind it,
// and a connection overlay stacked over both.
//
// This is where the three projects finally meet. examples/qt/terminal_window
// proved the wiring against real gear; this is the same three connections with
// a tab around them, which is what Phase 5 said it would add.
//
// WHAT IT OWNS: the widget, the session, the overlay, and the theme applied to
// all three. WHAT IT DOES NOT: deciding what to connect to. The config arrives
// built, from the quick-connect dialog now and from the session tree later, so
// this class never learns the difference between the two.
//
// The overlay renders the transport's six states verbatim rather than
// inventing its own vocabulary -- the whole reason those states are the
// transport's and not the UI's.

#ifndef OMEGA_APP_TERMINALTAB_H
#define OMEGA_APP_TERMINALTAB_H

#include <QPoint>
#include <QString>
#include <QWidget>

#include <omegasshsession.h>

#include "app/antiidle.h"
#include "app/linkstate.h"
#include "app/capturewriter.h"
#include "app/hostkeyerror.h"
#include "app/pastequeue.h"
#include "theme/theme.h"
#include "theme/tokens.h"

class QFrame;
class QLabel;
class QPushButton;
class QTimer;

namespace omega::app {

class TerminalView;

class TerminalTab : public QWidget {
    Q_OBJECT

public:
    explicit TerminalTab(QWidget *parent = nullptr);

    // Starts the session. Returns false when the CONFIGURATION is wrong -- a
    // credential that names nothing, an impossible parity -- with the reason
    // in error(). Returning true means the dial has started, not that it
    // landed: watch stateChanged.
    bool start(const omegassh::Config &config);

    // What the tab should be called: the display name when one was set,
    // else the transport's own summary.
    //
    // The summary is user@host:port, built on the Go side. That is the right
    // answer for quick connect, where the address is the only thing anybody
    // typed, and the wrong one for a saved session in a store addressed by IP
    // -- a row of tabs reading 10.20.30.41, 10.20.30.42 identifies nothing.
    // Either way it is available the moment start() returns rather than when
    // the dial lands, so a tab is titled while it is still connecting.
    QString title() const;

    // The saved session's name, or empty to use the summary. Set before
    // start() by the window, which is the only thing that knows a tab came
    // from the store rather than from quick connect.
    void setDisplayName(const QString &name);

    QString error() const;
    bool isRunning() const;
    omegassh::State state() const;

    // Colours the widget, the selection and the overlay. Called by the
    // window's one applyTheme walk, never by the tab itself -- a tab that
    // themed itself would be a second place for the walk to miss.
    void applyTheme(const theme::Theme &theme);

    // The theme this tab asked for BY NAME, or empty to follow the window.
    //
    // The tab stores the name and resolves nothing: it has no ThemeEngine,
    // and giving it one would make it the second place that themes. The walk
    // reads this and hands down whichever theme it resolves to -- see
    // MainWindow::themeForTab -- so applyTheme() above still has exactly one
    // caller and the tab still cannot theme itself.
    //
    // Set before the first applyTheme(), from TabSettings::themeName.
    void setThemeOverride(const QString &name);
    QString themeOverride() const { return themeOverride_; }

    // Ends the session. Safe on one that never connected and on one still
    // dialing, which is what makes a Cancel button honest.
    void terminate();

    // Same, but records that a person asked for it, so the overlay says
    // "Cancelled" rather than reporting the disconnect as if the far end had
    // done it. Wired to the overlay's own button.
    void cancelConnect();

    // --- reconnect ---------------------------------------------------------
    //
    // Dials the SAME config again in the SAME tab, so the scrollback the last
    // session left is still there to read underneath. A tab is deliberately
    // kept open after its session ends -- see the finished() handler in
    // mainwindow.cpp -- and until now the only thing to do with one was close
    // it and start over somewhere else, which threw that scrollback away.
    //
    // False while a session is running: this is for a connection that has
    // ended, not a way to bounce a live one. Nothing here disconnects.
    bool canReconnect() const;

    // No-op when canReconnect() is false. Failure to start again leaves the
    // reason on the overlay, the same as the first dial did.
    void reconnect();

    // --- credential retry --------------------------------------------------
    //
    // When a dial is refused for the credentials, the tab does NOT settle on
    // the failure. It holds the session in a re-dialing state and emits
    // credentialsRejected(); the window answers with exactly one of the two
    // calls below. Nothing else may be called in between -- the tab is
    // mid-teardown until one of them arrives.
    //
    // The window and not the tab, because the prompt needs the vault and this
    // class deliberately has no idea where its config came from. See
    // autherror.h.

    // Dial again with these credentials replacing what the last attempt used.
    //
    // credential names a vault entry and needs vaultHandle alongside it; both
    // may be empty/0 when the operator typed credentials instead. username and
    // password are applied only when non-empty, which is the same rule the
    // library applies everywhere: explicit fields win over a reference, and
    // what is left blank comes from the credential.
    //
    // A key path or passphrase from the previous attempt is left alone. This
    // replaces the credentials, it does not rebuild the config.
    void retryWithCredentials(const QString &credential, long long vaultHandle,
                              const QString &username, const QString &password);

    // The operator cancelled, or the tab has asked as often as it may. Settles
    // the overlay on the rejection and lets finished() through, which is what
    // the window was waiting on to mark the tab closed.
    void abandonCredentialRetry();

    // --- keyboard-interactive retry ------------------------------------------
    //
    // Same contract as the credential pair above: the tab holds the session in
    // a re-dialing state and emits keyboardQuestion(); the window answers with
    // exactly one of the two calls below.
    //
    // The answer is filed under the question verbatim, because that is the key
    // the far end will match on the next dial.
    void retryWithKeyboardAnswer(const QString &question, const QString &answer);
    void abandonKeyboardRetry();

    // What the prompt should say it is connecting to. The display name when
    // the tab has one, else the transport's summary -- so a saved session says
    // its own name rather than an address nobody typed.
    QString target() const;

    // --- clipboard ---------------------------------------------------------
    //
    // Public because the context menu is not the only caller: a window-level
    // Edit menu or a shortcut reaches the same three.

    void copy();

    // Confirms first when the clipboard holds more lines than the threshold.
    // See PasteConfirmDialog for why that default is 1.
    void paste();

    // Copy the selection, then paste it. One gesture for the thing people
    // actually do with a terminal selection.
    void copyPaste();

    // Lines above which paste() confirms; 0 disables the confirmation
    // entirely. Set by the window from settings, the same way the theme is --
    // a tab that read SettingsManager itself would be a second owner of it.
    void setMultilinePasteThreshold(int lines);

    // Rows of scrollback the emulator retains. Set by the window from
    // scrollback_lines, which has been in the shared settings file since 4a
    // and read by nothing until now -- so every tab has been running on
    // anytermqt's own default of 1000 while the file advertised 10000.
    //
    // Live: pyte trims immediately, so lowering this drops rows out of a tab
    // that is already open. That is the intended behaviour and it is worth
    // knowing before wiring it to a settings dialog that applies as you type.
    void setScrollbackLines(int lines);

    // Terminal font size in points; 0 means take the theme's. nterm-qt reads
    // font_size out of config.json and Omega did not, so a size set there was
    // silently ignored here -- this is the gap being closed, not a new
    // preference. Applied over the theme's font rather than instead of it:
    // the family, style hint and fixed pitch all still come from the theme.
    // The size settings resolved for this tab. Ctrl+wheel zooms RELATIVE to
    // this, and the offset survives a later call -- a settings save that
    // pushes the same base over every open tab must not silently undo a zoom
    // the operator is looking at.
    void setFontPointSize(int points);

    // --- temporary per-tab zoom ---------------------------------------------
    //
    // Ctrl+wheel, and Ctrl+0 to go back. Deliberately NOT persisted: it is a
    // "lean in and read this" control, not a preference, and writing it to the
    // session would make every accidental Ctrl+scroll permanent. Dies with the
    // tab.
    void zoomBy(int steps);
    void resetZoom();
    int effectiveFontPointSize() const;

    // --- anti-idle ---------------------------------------------------------
    //
    // See antiidle.h. Off unless switched on, and suppressed while a
    // full-screen application is up or a paced paste is running.
    void setAntiIdle(const AntiIdleConfig &config);
    const AntiIdleConfig &antiIdle() const { return antiIdle_; }

    // --- paste rate limiting -----------------------------------------------
    //
    // The rate a pasted block is paced at, in baud; 0 sends the whole thing
    // at once, which is what a host with flow control wants. See pastequeue.h
    // for why anything reached over a console port does not, and
    // pasteconfirmdialog.h for why the unit is baud.
    //
    // NOT read from settings, deliberately: config.json is shared with
    // nterm-qt, whose from_dict() drops keys it does not know, so an
    // Omega-only field there would be deleted the first time the other
    // application saved. start() picks a default from the transport instead,
    // and the confirmation dialog is where it is normally chosen.
    void setPasteBaud(int baud);
    int pasteBaud() const { return pasteBaud_; }

    // --- wheel over a full-screen application -------------------------------
    //
    // When on, a wheel notch inside vi, a pager or anything else on the
    // alternate screen sends arrow keys to that application instead of
    // scrolling the local scrollback behind it. Off restores the plain
    // QAbstractScrollArea behaviour, which is what shipped before.
    //
    // Set by the window from resolved settings, the same way the theme and
    // scrollback are -- a tab that read the settings itself would be a second
    // owner of them. Applies to the ALTERNATE SCREEN only: at an ordinary
    // prompt the wheel still scrolls scrollback, which is what it is for.
    void setWheelAltScreen(bool enabled);
    bool wheelAltScreen() const { return wheelAltScreen_; }

    bool isPasting() const;

    // Stops a paste part way. The lines already sent stay sent -- there is no
    // taking those back -- but nothing further goes out.
    void cancelPaste();

    // --- capture -----------------------------------------------------------

    bool isCapturing() const { return capture_.isCapturing(); }

    // Received bytes, escape sequences stripped. Returns false with the
    // reason in error() when the file cannot be opened.
    bool startCapture(const QString &path);
    void stopCapture();

    // The stored session this tab was opened from, or -1 for quick connect.
    // Set once by the window; the tab never looks it up, because a tab that
    // could resolve its own id would be a tab that depends on the store.
    void setSessionId(qint64 id) { sessionId_ = id; }
    qint64 sessionId() const { return sessionId_; }
    Link link() const { return link_; }

    // Pushed in by the window while a credential or host-key prompt is up.
    // The transport has no state for "waiting for a person" -- see
    // linkstate.h -- so the only thing that knows is whoever raised the
    // dialog.
    void setNeedsInput(bool needsInput);

signals:
    // The UI-level state, which is not omegassh::State: see linkstate.h. The
    // window forwards it to the live registry, which is what puts a dot on the
    // tree row this tab came from.
    void linkChanged(Link link);

    // The session ended, however it ended: cleanly, refused, or closed here.
    // The tab container listens so it can mark or close the tab.
    void finished(int exitCode);

    // The overlay text changed. The window puts it in the status bar rather
    // than the tab reaching up for a status bar it does not own.
    void statusChanged(const QString &text);

    // A new dial has started on a tab that had finished. Emitted when the dial
    // STARTS, not when it lands, for the same reason start() titles the tab
    // immediately: the title is right from the first frame either way. The
    // window listens because it wrote "(closed)" into the tab text when the
    // last session ended and nothing else would take it back off.
    void restarted();

    // The far end refused the credentials, and this tab is holding a dial open
    // for a second answer. The window MUST reply with retryWithCredentials()
    // or abandonCredentialRetry() -- a listener that ignores this leaves the
    // tab spinning on an overlay that never settles.
    //
    // Emitted from a clean stack, not from inside the session's own event
    // delivery, so raising a modal in the slot is safe. See
    // offerCredentialPrompt().
    void credentialsRejected(const QString &target);

    // The far end asked something only a person can answer. The window MUST
    // reply with retryWithKeyboardAnswer() or abandonKeyboardRetry().
    //
    // question is the server's own wording and must reach the operator
    // unchanged; secret says whether to mask the field.
    void keyboardQuestion(const QString &target, const QString &question,
                          bool secret, int attempt, int limit);

protected:
    // Watches the terminal for one key and one condition: Escape while a
    // rate-limited paste is in flight. See the note in terminaltab.cpp for
    // why that key is worth taking and why only then.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setOverlayVisible(bool visible);
    void onStateChanged(omegassh::State state);
    void publishLink();
    void showContextMenu(const QPoint &pos);

    // On a first-contact host key failure: takes over the failure, and asks.
    // Returns true when it has, in which case the overlay must not settle on
    // the error -- the answer arrives later.
    //
    // The asking itself is deferred rather than done here: this is called
    // from inside the session's own event delivery, and tearing that session
    // down from within its call stack is re-entrant. finishHostKeyPrompt()
    // runs once that stack has unwound.
    bool offerHostKeyPrompt();
    void finishHostKeyPrompt(const HostKeyInfo &info);

    // On a credential rejection: takes over the failure and asks the window to
    // prompt. Returns true when it has, in which case the overlay must not
    // settle on the error -- the answer arrives later, through
    // retryWithCredentials() or abandonCredentialRetry().
    //
    // Deferred for the same two reasons offerHostKeyPrompt() defers: this runs
    // inside the session delivering its own events, and the window is going to
    // spin a nested event loop for a modal.
    bool offerCredentialPrompt();

    // On an unanswered keyboard-interactive question: takes over the failure
    // and asks the window to prompt. Deferred for the same reasons as the
    // other two.
    bool offerKeyboardPrompt();

    // Translates one wheel event into arrow keys for the application on the
    // alternate screen. True when the event was handled and must not reach
    // QAbstractScrollArea.
    bool dispatchAltScreenWheel(class QWheelEvent *event);

    void applyFontSize();
    void setOverlayMessage(const QString &text, bool spinning, bool cancellable);
    void tickSpinner();
    void toggleCapture();

    // Sends the next chunk and re-arms, or finishes.
    void sendNextPasteChunk();

    // --- window size -------------------------------------------------------
    //
    // attach() wires the widget's resized() straight to the session, which
    // covers every size CHANGE. It does not cover the one that matters most:
    // the size the widget already is when a session comes up. The widget only
    // emits resized() when the grid changes, and by the time a dial lands the
    // grid has usually stopped changing -- so the far end keeps whatever
    // Config::cols/rows asked for at dial time, which was 80x24.
    //
    // syncRemoteSize() pushes the current grid unconditionally. Idempotent on
    // all three transports: SSH sends another window-change, telnet another
    // NAWS subnegotiation, serial ignores it.
    void syncRemoteSize();

    // Restarts the idle clock. Called for every byte this end sends, from the
    // one tap on the widget's outbound signal -- keystrokes and paste alike,
    // since a paced paste writes through the same signal.
    void noteOutbound();

    // Re-arms or stops the idle timer to match the current config and state.
    void rearmAntiIdle();

    // The timer fired. Sends the keystroke if it is still allowed to, and
    // re-arms either way.
    void sendAntiIdle();
    void beginThrottledPaste(const QString &text);

    // Where the Save dialog starts, named after what is being captured rather
    // than "session.log" in whatever directory Qt last remembered.
    QString suggestedCapturePath() const;

    TerminalView *terminal_ = nullptr;
    omegassh::OmegaSshSession *session_ = nullptr;

    // The overlay is a panel now, not a bare label: message, spinner and a
    // Cancel button on one background.
    QFrame *overlay_ = nullptr;
    QLabel *overlayText_ = nullptr;
    QPushButton *overlayCancel_ = nullptr;
    QTimer *spinnerTimer_ = nullptr;
    QString overlayMessage_;
    int spinnerFrame_ = 0;
    bool cancelled_ = false;

    // The config this tab dialed, kept so an accepted host key can be
    // re-dialed with the policy changed and nothing else touched -- and, since
    // reconnect(), so the tab can dial the same thing again with nothing
    // changed at all.
    omegassh::Config config_;

    // Whether config_ holds anything. A tab that has never been started has a
    // default-constructed Config, and dialing that would be a connect to
    // nowhere presented as a reconnect.
    bool haveConfig_ = false;

    // One prompt per tab. A re-dial that fails the same way again means
    // something other than an unseen key -- a race with another client
    // writing known_hosts, or a path that cannot be written -- and a dialog
    // that reappears every attempt is a loop, not a prompt.
    bool hostKeyPrompted_ = false;

    // True while the session is being torn down and rebuilt on purpose: from
    // the moment a host key prompt is raised until that re-dial is under way,
    // and across reconnect()'s own teardown. While it is set the disconnect
    // that teardown produces is not news -- the overlay ignores it and
    // finished() is not forwarded, or the window would mark the tab closed a
    // moment before it reconnects.
    bool redialing_ = false;

    // The failure that raised the prompt, kept because terminate() and the
    // re-dial both move on from it and a rejected key has to be able to say
    // what went wrong.
    QString hostKeyError_;

    // How many times this tab has re-asked for credentials, against
    // AuthPromptLimit. Not a bool like hostKeyPrompted_: a mistyped password
    // deserves another go, an unknown host key does not become known by
    // asking twice.
    //
    // Reset by reconnect() and by a successful connect, so a tab that comes up
    // and is later disconnected gets a fresh three rather than inheriting the
    // count from a login the operator got right in the end.
    int authPrompts_ = 0;

    // The rejection that raised the prompt, kept for the same reason
    // hostKeyError_ is: cancelling has to be able to say what went wrong, and
    // by then the session has moved on from it.
    QString authError_;

    // How many keyboard-interactive questions this tab has put to the
    // operator, against KeyboardPromptLimit. Reset by reconnect() and by a
    // successful connect, like the credential count.
    int keyboardPrompts_ = 0;
    QString keyboardError_;

    // The last question put to the operator, and whether it was secret.
    //
    // Kept because a REJECTED answer does not say what it was answering. Once
    // an answer is supplied the dial no longer fails with the keyboard marker
    // -- it fails with an ordinary "unable to authenticate", which carries no
    // question at all. Without this the tab could only offer the credential
    // dialog, which cannot fix a mistyped one-time code.
    QString lastKeyboardQuestion_;
    bool lastKeyboardSecret_ = true;

    // The widget's own translucent selection alpha, read once before any theme
    // lands. Reading it later reads back whatever the last theme wrote, and
    // the translucency is lost on the second switch -- anytermqt paints the
    // selection over the glyphs, so an opaque colour erases the text.
    int selectionAlpha_ = 110;

    CaptureWriter capture_;

    // Kept from the Config rather than parsed back out of summary(), so the
    // capture filename is right for serial and telnet too.
    QString host_;

    // Empty for a quick-connect tab. Deliberately NOT cleared by start() or
    // reconnect(): the name belongs to the session this tab was opened from,
    // and a re-dial of the same config is still that session.
    QString displayName_;

    class SessionRibbon *ribbon_ = nullptr;

    qint64 sessionId_ = -1;
    bool needsInput_ = false;
    Link link_ = Link::Idle;

    int pasteThreshold_ = 1;

    // 0 = the theme decides. Kept because applyTheme() runs again on every
    // theme switch and would otherwise put the theme's size back.
    int baseFontPointSize_ = 0;

    // Points added by Ctrl+wheel, signed. Separate from the base so the two
    // can be reasoned about independently: the window owns the base, the
    // operator owns this.
    int zoomDelta_ = 0;

    // Sub-notch Ctrl+wheel motion, kept apart from wheelAccum_ so that
    // trackpad zooming and trackpad scrolling cannot consume each other's
    // remainders.
    int zoomAccum_ = 0;

    // The theme last applied, so a font size changed from the settings dialog
    // can be re-applied without the window having to hand the theme down a
    // second time. Owned by the ThemeEngine, which outlives every tab.
    const theme::Theme *themeApplied_ = nullptr;

    // The name this tab pinned, or empty. Not resolved here; see
    // setThemeOverride().
    QString themeOverride_;

    PasteQueue pasteQueue_;
    QTimer *pasteTimer_ = nullptr;
    int pasteBaud_ = 0;

    bool wheelAltScreen_ = true;

    // Leftover wheel motion below one notch, in eighths of a degree. A
    // trackpad delivers a stream of small deltas and rounding each to zero
    // would mean it never scrolls at all.
    int wheelAccum_ = 0;

    AntiIdleConfig antiIdle_;
    QTimer *antiIdleTimer_ = nullptr;

    // The grid the widget last reported, cached from its own resized() signal
    // rather than read back out of it. Reading it back would mean naming the
    // widget's accessors here; taking it off the signal means the only shape
    // this file depends on is the one attach() already depends on --
    // resized(cols, rows), the same pair OmegaSshSession::resize takes.
    //
    // 0 until the widget has been laid out at least once, which is the state a
    // tab is in during its own constructor.
    int lastCols_ = 0;
    int lastRows_ = 0;

    // A second push a moment after the session comes up. Telnet negotiates
    // NAWS after the socket is open, so a resize that arrives at Connected can
    // land before the far end has agreed to hear about window sizes -- the
    // backend records it and only sends when NAWS is on. This one is sent
    // after that has settled. On SSH it is a duplicate, and a duplicate
    // window-change costs a packet.
    QTimer *sizeSyncTimer_ = nullptr;

    QString error_;
};

}  // namespace omega::app

#endif  // OMEGA_APP_TERMINALTAB_H