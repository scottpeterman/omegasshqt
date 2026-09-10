// app/mainwindow.h
//
// The shell: a window that remembers where it was, has every menu item present,
// and takes its colours from the theme system.
//
// Phase 4a, and deliberately nothing more. There is no tree model, no tab, no
// session and no dial in here. Every menu action is wired to a stub that says
// which phase owns it, so the shortcut map is real and the shape is visible
// before any of it connects to anything.
//
// The one thing built for the future rather than for now is applyTheme(). It
// walks the window, the tab area and the detached-window list from the first
// version, while two of those three are still empty. Retrofitting that walk is
// the stale-pane bug: nterm-qt's _apply_theme walks its tab widget only, keeps
// a list of detached session windows, and never walks it -- so a live theme
// switch leaves every detached window on the old palette.

#ifndef OMEGA_APP_MAINWINDOW_H
#define OMEGA_APP_MAINWINDOW_H

#include <QMainWindow>
#include <QPointer>
#include <QVector>

#include <memory>
#include "app/currenttokens.h"
#include <omegasshsession.h>
#include <omegasshvault.h>

#include "app/livesessions.h"
#include "app/omegasettings.h"
#include "app/settings.h"
#include "app/effectiveconfig.h"
#include "sessions/store.h"
#include "theme/theme.h"

class QSplitter;
class QMenuBar;
class QScreen;
class QTabWidget;
class QTimer;

namespace omega::app {

class ChromeBar;
class ThemedSizeGrip;

class SessionTreePane;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    // vaultPath defaults to ~/.omega/vault.json, beside config.json and the
    // session store. Overridable so a probe can point at a throwaway file
    // rather than at whoever is running the build machine's real vault.
    //
    // omegaSettings carries the settings nterm-qt has no field for -- see
    // omegasettings.h. Null is tolerated rather than required: the probes
    // construct a window to check geometry and the tree, and neither needs a
    // second settings file on the build machine.
    MainWindow(SettingsManager *settings, theme::ThemeEngine *themes,
               QWidget *parent = nullptr,
               const QString &vaultPath = defaultVaultFile(),
               const QString &sessionsPath = defaultSessionFile(),
               OmegaSettingsManager *omegaSettings = nullptr);

    static QString defaultVaultFile();

    // ~/.omega/sessions.db -- nterm-qt's SessionStore schema, in Omega's own
    // directory, by
    // name, so both applications open the same one. Overridable for the same
    // reason vaultPath is: a probe must not reorder whoever is running the
    // build machine's real tree.
    static QString defaultSessionFile();

    // The vault this window owns, for the life of the process. Non-owning
    // callers pass handle() into Config::vaultHandle; nobody else closes it.
    omegassh::Vault *vault() { return vault_.get(); }

    // Puts the unlock dialog up if the vault is locked, and answers whether it
    // ended up unlocked. Called before anything that needs credentials --
    // never at startup, see the note on quietUnlock().
    bool ensureVaultUnlocked();

    void openCredentialManager();

    // The settings form, and the live-apply pass that follows an accepted
    // one. Separate because the second half is what a future Apply button
    // would call on its own.
    void openSettings();
    void applySettingsToTabs();

    // Applies a theme by name. Returns false when the name resolves to
    // nothing, in which case nothing changed -- the caller decides whether
    // that is worth reporting, because the common case is an old config.json
    // naming a theme that no longer ships.
    //
    // `persist` writes the name into AppSettings, which closeEvent then saves.
    // It is false for a --theme on the command line: that flag is for looking
    // at a theme against real sessions, and a flag that quietly rewrote
    // ~/.omega/config.json every time it was used would be a poor way to
    // compare two of them. Choosing a theme from the View menu or the settings
    // dialog still persists, because that is a person deciding rather than
    // a person looking.
    bool applyThemeByName(const QString &name, bool persist = true);

    // Forces the window manager's frame for this run, whatever omega.json
    // says, without writing the choice back. --native-frame; see main.cpp.
    // Must be called before the window is constructed, so it is a static that
    // the constructor reads.
    static void forceNativeFrame();

    // Every window this application owns, for the walk above. A detached
    // session window registers here when Phase 5 grows one; the list is empty
    // now and the walk still runs over it.
    void registerWindow(QWidget *window);

    // Opens the quick-connect dialog and, if it is accepted, adds a tab and
    // starts dialing. Public because Ctrl+N is not the only future caller: the
    // session tree's "connect in tab" lands here too, with a config built from
    // a stored session instead of from the form.
    void quickConnect();

    // Builds a Config from a stored session and opens it. The tree's connect
    // path and the tab it produces go through openTab like every other route,
    // so theming, titling and the close path cannot diverge between them.
    void openSession(const sessions::Session &session);

    // Adds a tab for an already-built config and starts it. The one place a
    // tab is created, so theming, titling and the close path cannot diverge
    // between the dialog's route and the tree's.
    // Takes a RESOLVED pair rather than a Config: every caller has already
    // decided what the session overrides and what it inherits, so this
    // function reads no settings of its own. See effectiveconfig.h.
    void openTab(const Resolved &resolved, qint64 sessionId = -1);

    // Tab management. Public for the same reason openTab is: the tab bar's
    // close button and Ctrl+W reach these now, and a tree context menu and a
    // tab context menu will reach them later.
    //
    // closeTab() and closeAllTabs() do NOT ask. Single close is the tab bar's
    // own button and Ctrl+W, which have never asked; closeAllTabs() stays
    // unconditional because it is API -- shell_probe and the tree pane reach
    // it, and neither can answer a modal. The routes a person drives go
    // through closeTabSet(), which does ask. See tabclose.h for why the line
    // is drawn between one tab and several rather than between live and dead.
    void closeTab(int index);
    void closeAllTabs();
    class TerminalTab *currentTab() const;

    // The theme currently applied, by name. Phase 5's detached window needs it
    // to open in the theme the main window is already using rather than in
    // whatever the settings last recorded.
    QString currentThemeName() const;

    // The splitter's left pane, which is what tree_width means. Both exist for
    // the same reason the geometry fields do: the value is settings state, so
    // something other than a mouse drag has to be able to set it -- restoring
    // it at startup, and tests/compat/shell_probe.cpp checking that it comes
    // back.
    int treeWidth() const;
    void setTreeWidth(int width);

protected:
    // Writes geometry back to settings and saves. The size and position are
    // recorded only when the window is NOT maximized, so un-maximizing
    // restores the size it had before -- which is what nterm-qt does, and
    // means the two applications agree about what the stored numbers mean.
    void closeEvent(QCloseEvent *event) override;

    // Resize edges have to be caught application-wide rather than on this
    // widget. The session tree, the terminal and the status bar all reach the
    // window border, and each consumes the mouse events that land in the 6px
    // hit zone before the window ever sees them.
    bool eventFilter(QObject *watched, QEvent *event) override;

    // Keeps the maximise glyph honest when the state changed from somewhere
    // other than the button -- a double-click on the bar, or the WM.
    void changeEvent(QEvent *event) override;

private:
    void buildUi();
    void buildMenus();

    // --- tab bar context menu ----------------------------------------------

    // Right-click on the tab strip. Does nothing on the empty part of the
    // strip: every item here is about a particular tab, and a menu that
    // appeared beside no tab would have to guess which one.
    void showTabContextMenu(const QPoint &pos);

    // Closes a set of tabs after ONE question covering all of them, and
    // answers whether it went ahead. Collecting the widgets first is not
    // tidiness: removeTab() renumbers everything after it, so a loop over
    // indexes closes the wrong tabs from the second one on.
    bool closeTabSet(const QVector<class TerminalTab *> &victims);

    // Every tab in the strip except one; pass -1 to get all of them.
    QVector<class TerminalTab *> tabsExcept(int keep) const;

    // Every tab after index, in strip order.
    QVector<class TerminalTab *> tabsAfter(int index) const;

    // Keyring, then environment, no prompt and no dialog. Startup does not
    // block on a password: a locked vault is a usable state, quick connect
    // with typed credentials works without one, and an application that opens
    // with a modal is an application that cannot be used for the thing it was
    // opened for until the modal is answered. What the attempt found goes to
    // the status bar, and the prompt happens when the vault is first needed.
    // --- stranded windows ---------------------------------------------------
    //
    // A saved x/y is a position on the monitor layout that existed when the
    // window last closed. Undock the laptop, unplug the second head, or let a
    // dock hand back a different arrangement than it took, and those
    // coordinates name a place that is not on any screen any more. The window
    // opens there, invisibly.
    //
    // With a native frame that is survivable -- the window manager has its own
    // rescue gestures, Alt+Space on Windows, Alt+drag on most of X11. On the
    // frameless path there is no title bar for any of them to act on, and the
    // only handle the window has is the ChromeBar it is currently drawing off
    // the edge of the world.
    //
    // So the fix is not a gesture. Gestures need a window you can already see.

    // Whether the strip a person can actually grab -- the ChromeBar, or the
    // window manager's title bar on the native path -- has enough of itself on
    // some screen to be hit with a mouse. This is deliberately NOT "does the
    // window intersect a screen": a window whose bottom-right corner is just
    // on-screen is visible and still cannot be moved.
    bool windowIsGrabbable() const;

    // Puts the window on `screen`, centred. fitToScreen sizes it to the work
    // area; otherwise it keeps its current size, shrunk if the destination
    // cannot hold it -- which is the whole point when the destination is a
    // 1920x1080 laptop panel and the size was saved on a 4K monitor.
    void placeOnScreen(QScreen *screen, bool fitToScreen);

    // The screen the mouse is on, else the primary. Where a person is looking
    // is a better guess than where the window used to be.
    static QScreen *rescueTargetScreen();

    // Subscribes to one screen's geometry changes. Re-run for every screen
    // whenever the set of them changes.
    void watchScreen(QScreen *screen);

    // Coalesces a burst of screen events into one check. Docking emits
    // several -- added, primary changed, geometry changed, available geometry
    // changed -- and reacting to each one moves the window repeatedly while
    // the layout is still settling.
    void scheduleScreenCheck();
    void checkWindowReachable();

    void refreshVaultPill();
    void quietUnlock();
    void lockVault();
    void changeMasterPassword();
    void forgetKeyringEntry();
    const theme::Theme &currentThemeOrFallback() const;

    // Which theme a given tab paints in: its own pinned one when it has one
    // and that name resolves, otherwise `windowTheme`.
    //
    // Both the open path and the applyTheme walk go through here, because a
    // pinned theme applied on open and then overwritten by the next walk is
    // the exact failure this is shaped to prevent -- and it would only show
    // when somebody changed the theme with a pinned tab already open, which
    // is not the path anyone tests by hand.
    //
    // An unresolvable name falls back to the window rather than to the
    // engine's fallback: see the note on Session::theme_name.
    const theme::Theme &themeForTab(const class TerminalTab *tab,
                                    const theme::Theme &windowTheme) const;

    void buildChrome();
    void restoreGeometryFromSettings();
    void applyTheme(const theme::Theme &theme);

    // The frameless path's resize edges. See the note on eventFilter.
    Qt::Edges edgesAt(const QPoint &globalPos) const;
    void applyResizeCursor(Qt::Edges edges);

    // Every menu action lands here until its phase arrives. It names the
    // phase rather than doing nothing quietly, so a menu that looks finished
    // cannot be mistaken for one that is.
    void notImplemented(const QString &what, const QString &phase);

    SettingsManager *settings_ = nullptr;
    OmegaSettingsManager *omegaSettings_ = nullptr;

    // Stable storage for the omegaSettings_ == nullptr case. The tree pane
    // holds a REFERENCE for its lifetime, so the temporary that serves the
    // resolver call sites will not do here.
    OmegaSettings fallbackOmega_;
    theme::ThemeEngine *themes_ = nullptr;

    // What the tabs know, keyed by what the tree shows. Owned here because it
    // outlives every tab and is read by the model, and both of those are this
    // window's children.
    LiveSessionRegistry live_;

    // The merged title bar, or null on the native-frame path. When it exists
    // it owns the QMenuBar; when it does not, menuBar_ is QMainWindow's own.
    // Either way menuBar_ is the ONE handle -- see chromebar.h on why
    // menuBar() must not be called after setMenuWidget.
    ChromeBar *chromeBar_ = nullptr;
    QMenuBar *menuBar_ = nullptr;
    ThemedSizeGrip *sizeGrip_ = nullptr;
    bool frameless_ = false;
    bool cursorOverridden_ = false;

    // See scheduleScreenCheck(). Null until the constructor has built it.
    QTimer *screenCheckTimer_ = nullptr;

    QSplitter *splitter_ = nullptr;
    SessionTreePane *treePane_ = nullptr;
    QTabWidget *tabs_ = nullptr;

    // The applied theme's name, which is not the same thing as the settings'
    // theme_name: the settings may name a theme that no longer ships, and this
    // is what actually got applied.
    QString themeName_;

    // Constructed before the menus, since the Vault menu reads its state.
    std::unique_ptr<omegassh::Vault> vault_;

    // Constructed before buildUi, which hands it to the tree pane. A store
    // that failed to open leaves the pane with a null and an empty tree rather
    // than taking the window down with it -- a broken sessions.db must not
    // stop quick connect from working.
    std::unique_ptr<sessions::SessionStore> sessions_;

    // Empty unless the store failed to open; shown in the status bar instead
    // of "Ready" so a broken sessions.db is visible rather than inferred.
    QString sessionError_;

    QVector<QPointer<QWidget>> windows_;
};

}  // namespace omega::app

#endif  // OMEGA_APP_MAINWINDOW_H
