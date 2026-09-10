// app/mainwindow.cpp

#include "app/mainwindow.h"

#include "app/effectiveconfig.h"
#include "app/currenttokens.h"

#include "app/chromebar.h"
#include "app/windowbuttons.h"
#include "app/credentialmanagerdialog.h"
#include "app/helpdialog.h"
#include "app/quickconnectdialog.h"
#include "app/sessiontransfer.h"
#include "app/sessiontreepane.h"
#include "app/tabclose.h"
#include "app/settingsdialog.h"
#include "app/statusdot.h"
#include "app/sizegrip.h"
#include "app/terminaltab.h"
#include "app/credentialpromptdialog.h"
#include "app/keyboardpromptdialog.h"
#include "app/vaultunlockdialog.h"

#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QLabel>
#include <QMenu>
#include <QApplication>
#include <QMenuBar>
#include <QDir>
#include <QInputDialog>
#include <QMessageBox>
#include <QSplitter>
#include <QPushButton>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QMouseEvent>
#include <QPalette>
#include <QCursor>
#include <QFont>
#include <QGuiApplication>
#include <QScreen>
#include <QTimer>
#include <QWindow>
#include <QVBoxLayout>

#include "theme/stylesheet.h"
#include "theme/tokens.h"
#include "theme/tokenstylesheet.h"

namespace omega::app {
namespace {

// "enterprise_dark" reads as "Enterprise Dark" in the menu. Same transform
// nterm-qt applies, so the two applications list the same names.
QString titleCase(const QString &slug) {
    QString out = slug;
    out.replace(QLatin1Char('_'), QLatin1Char(' '));
    bool atStart = true;
    for (QChar &c : out) {
        if (atStart) c = c.toUpper();
        atStart = c.isSpace() || c == QLatin1Char('-');
    }
    return out;
}

// The frameless window's invisible resize border, in logical pixels. Six is
// what every frameless application converges on: three is unhittable on a
// touchpad and ten starts eating clicks meant for the widget underneath.
constexpr int kResizeMargin = 6;

// The height of the strip a person grabs to move the window: the ChromeBar on
// the frameless path, the window manager's title bar on the native one. Used
// only to ask whether enough of it is on a screen to be clicked, so an
// approximation is fine and a generous one is the safe direction -- it makes
// the check stricter, not looser.
constexpr int kGrabBandHeight = 36;

// How much of that strip has to be reachable. 120 logical pixels is roughly
// "the wordmark and one menu", which is enough to hit without aiming.
constexpr int kMinGrabWidth = 120;

// The inset used when the window is sized to a screen rather than merely moved
// onto one, so it does not sit flush against the work area on all four sides.
constexpr int kFitMargin = 40;

// Docking, undocking and resolution changes emit several screen events in a
// burst. One check after the burst, not one per event.
constexpr int kScreenSettleMs = 400;

// Set by --native-frame before the window is built. A file-scope flag rather
// than a constructor argument because MainWindow's constructor already takes
// six, and this one is a run-once override that must not reach omega.json.
bool g_forceNativeFrame = false;

QAction *addStub(QMenu *menu, const QString &text, const QString &shortcut) {
    // Spelled the long way: QMenu's convenience overloads that take a shortcut
    // and a functor together disagree about argument order between Qt 6.2 and
    // 6.4, and this has to build on both.
    QAction *action = menu->addAction(text);
    if (!shortcut.isEmpty()) {
        action->setShortcut(QKeySequence(shortcut));
    }
    return action;
}

}  // namespace

void MainWindow::forceNativeFrame() { g_forceNativeFrame = true; }

QString MainWindow::defaultVaultFile() {
    // Beside config.json and the session store, and named for its format: the
    // Go vault is JSON with Argon2 over the whole list, which is not nterm-qt's
    // SQLite vault.db. Two files rather than one because they are unrelated
    // formats -- sharing a name would mean one application opening the other's
    // file and reporting it corrupt.
    return QDir(SettingsManager::defaultConfigDir()).filePath(
        QStringLiteral("vault.json"));
}

QString MainWindow::defaultSessionFile() {
    // The name nterm-qt's DEFAULT_DB_PATH uses, in the directory it uses. The
    // schema is created IF NOT EXISTS with matching DDL at both ends, so
    // whichever application reaches a fresh file first, the other finds what it
    // expects.
    return QDir(SettingsManager::defaultConfigDir()).filePath(
        QStringLiteral("sessions.db"));
}

MainWindow::MainWindow(SettingsManager *settings, theme::ThemeEngine *themes,
                       QWidget *parent, const QString &vaultPath,
                       const QString &sessionsPath,
                       OmegaSettingsManager *omegaSettings)
    : QMainWindow(parent), settings_(settings), omegaSettings_(omegaSettings),
      themes_(themes),
      vault_(std::make_unique<omegassh::Vault>(vaultPath)) {
    // A store that would not open leaves the tree empty and every action in it
    // silently doing nothing, which reads as a broken tree rather than as a
    // broken file. The message is kept for the status bar, which does not exist
    // until buildUi has run.
    sessions::Status sessionStatus;
    sessions_ = sessions::SessionStore::open(sessionsPath.toStdString(),
                                            &sessionStatus);
    if (!sessionStatus) {
        sessionError_ = tr("Sessions: %1")
                            .arg(QString::fromStdString(sessionStatus.message));
    }
    setWindowTitle(QStringLiteral("Omega"));

    // Decided before anything is built, because it changes what buildUi and
    // buildMenus construct. effectiveTitleBar() is what folds in the "Classic
    // forces Native" rule, so this window and the settings dialog cannot
    // disagree about it.
    const OmegaSettings &os =
        omegaSettings_ ? omegaSettings_->settings() : fallbackOmega_;
    frameless_ = !g_forceNativeFrame &&
                 os.effectiveTitleBar() == TitleBar::Merged;
    if (frameless_) {
        setWindowFlag(Qt::FramelessWindowHint, true);
        // On the application, not on this widget: every child that reaches the
        // window border consumes the mouse events first. See eventFilter.
        qApp->installEventFilter(this);
    }

    registerWindow(this);

    buildUi();
    buildMenus();

    // Built before restoreGeometryFromSettings so scheduleScreenCheck() is
    // never called against a null timer, and subscribed after it so the
    // restore's own correction is not chased by a second one.
    screenCheckTimer_ = new QTimer(this);
    screenCheckTimer_->setSingleShot(true);
    screenCheckTimer_->setInterval(kScreenSettleMs);
    connect(screenCheckTimer_, &QTimer::timeout, this,
            &MainWindow::checkWindowReachable);

    restoreGeometryFromSettings();

    // Screens coming and going, and screens changing shape underneath a window
    // that is not moving. Watched on the application rather than on this
    // window, because QWindow::screenChanged only fires for a window that is
    // still ON a screen -- which is precisely not the case being caught here.
    connect(qApp, &QGuiApplication::screenAdded, this, [this](QScreen *s) {
        watchScreen(s);
        scheduleScreenCheck();
    });
    connect(qApp, &QGuiApplication::screenRemoved, this,
            [this](QScreen *) { scheduleScreenCheck(); });
    connect(qApp, &QGuiApplication::primaryScreenChanged, this,
            [this](QScreen *) { scheduleScreenCheck(); });
    const QList<QScreen *> screens = QGuiApplication::screens();
    for (QScreen *screen : screens) {
        watchScreen(screen);
    }

    quietUnlock();

    if (!applyThemeByName(settings_->settings().theme_name)) {
        // An old config.json can name a theme that no longer ships -- every
        // one written by nterm-qt's own default names catppuccin_mocha, which
        // has never existed in either theme set. Fall back rather than start
        // unthemed, and say which name was missing.
        statusBar()->showMessage(
            tr("Theme \"%1\" not found; using %2")
                .arg(settings_->settings().theme_name,
                     QString::fromStdString(themes_->current().name)),
            8000);
        applyTheme(themes_->current());
    }
}

void MainWindow::buildUi() {
    buildChrome();

    splitter_ = new QSplitter(Qt::Horizontal, this);

    treePane_ = new SessionTreePane(sessions_.get(), vault_.get(),
                                    settings_->settings(),
                                    omegaSettings_ ? omegaSettings_->settings()
                                                   : fallbackOmega_,
                                    themes_);
    connect(treePane_, &SessionTreePane::connectRequested, this,
            [this](const sessions::Session &session,
                   SessionTreePane::ConnectMode mode) {
                if (mode == SessionTreePane::InWindow) {
                    // Phase 5 owns the detached window. Named rather than
                    // silently opening a tab instead: a menu item that does
                    // the other thing is worse than one that says it is not
                    // here yet.
                    notImplemented(tr("Connect in a separate window"),
                                   QStringLiteral("5"));
                    return;
                }
                openSession(session);
            });
    connect(treePane_, &SessionTreePane::quickConnectRequested, this,
            &MainWindow::quickConnect);
    treePane_->setLiveRegistry(&live_);
    splitter_->addWidget(treePane_);

    tabs_ = new QTabWidget;
    tabs_->setTabsClosable(true);
    connect(tabs_, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
    tabs_->setDocumentMode(true);

    // On the tab BAR, not on the QTabWidget. A right-click on the strip is
    // delivered to the bar, and a policy set on the tab widget would only
    // catch clicks that landed on the page below it -- where the terminal's
    // own menu already answers.
    tabs_->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tabs_->tabBar(), &QWidget::customContextMenuRequested, this,
            &MainWindow::showTabContextMenu);

    splitter_->addWidget(tabs_);

    splitter_->setStretchFactor(0, 0);
    splitter_->setStretchFactor(1, 1);
    setCentralWidget(splitter_);

    statusBar()->showMessage(sessionError_.isEmpty() ? tr("Ready")
                                                     : sessionError_);

    if (frameless_) {
        // QStatusBar draws its own grip and it is the platform style's grey
        // pips, so that one is turned off and a themed one added as a
        // permanent widget. QSizeGrip resizes its window(), not its parent, so
        // living in the status bar is fine.
        statusBar()->setSizeGripEnabled(false);
        sizeGrip_ = new ThemedSizeGrip(statusBar());
        statusBar()->addPermanentWidget(sizeGrip_);
    }
}

void MainWindow::buildChrome() {
    if (!frameless_) {
        // The ordinary path: QMainWindow's own menu bar, in the window
        // manager's frame. menuBar_ is still the one handle, so buildMenus
        // does not have to know which path it is on.
        menuBar_ = menuBar();
        return;
    }

    chromeBar_ = new ChromeBar(/*withWindowButtons=*/true, this);
    menuBar_ = chromeBar_->menuBar();

    // setMenuWidget, not setMenuBar. The window keeps its central widget, its
    // status bar and its dock areas; only the strip along the top changes.
    // After this, menuBar() would CREATE A SECOND, EMPTY BAR -- see
    // chromebar.h.
    setMenuWidget(chromeBar_);
}

void MainWindow::buildMenus() {
    QMenuBar *bar = menuBar_;

    // --- File ------------------------------------------------------------
    QMenu *file = bar->addMenu(tr("&File"));
    // Not a stub. Quick connect is the first path in this window that reaches
    // the far end, and it is deliberately the first one built: it needs no
    // session store, no vault and no tree, so it exercises the tab, the
    // transport and the theme walk with nothing else in the way.
    QAction *quickConnect = file->addAction(tr("&Quick Connect..."));
    quickConnect->setShortcut(QKeySequence(QStringLiteral("Ctrl+N")));
    connect(quickConnect, &QAction::triggered, this, &MainWindow::quickConnect);
    file->addSeparator();
    QAction *closeTabAction = file->addAction(tr("Close Tab"));
    closeTabAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+W")));
    connect(closeTabAction, &QAction::triggered, this,
            [this] { closeTab(tabs_->currentIndex()); });
    QAction *closeAllAction = file->addAction(tr("Close All Tabs"));
    closeAllAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+W")));
    // Not closeAllTabs() directly: that one is the unconditional API. This is
    // a person asking, so it goes through the route that counts what is still
    // connected first.
    connect(closeAllAction, &QAction::triggered, this,
            [this] { closeTabSet(tabsExcept(-1)); });
    file->addSeparator();
    QAction *importAction = file->addAction(tr("&Import Sessions..."));
    importAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+I")));
    connect(importAction, &QAction::triggered, this, [this] {
        if (importSessions(this, sessions_.get()) && treePane_)
            treePane_->refresh();
    });
    QAction *exportAction = file->addAction(tr("&Export Sessions..."));
    exportAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+E")));
    connect(exportAction, &QAction::triggered, this,
            [this] { exportSessions(this, sessions_.get()); });
    file->addSeparator();
    QAction *quit = file->addAction(tr("&Quit"));
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &MainWindow::close);

    // No Dev menu, here or ever. The scripting REPL, the TextFSM tester and
    // the template downloader exist because Python was already in the process,
    // and Omega's premise is that it is not.

    // --- Edit ------------------------------------------------------------
    QMenu *edit = bar->addMenu(tr("&Edit"));
    QAction *settingsAction = edit->addAction(tr("&Settings..."));
    settingsAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+,")));
    connect(settingsAction, &QAction::triggered, this,
            &MainWindow::openSettings);
    edit->addSeparator();
    QAction *credentials = edit->addAction(tr("&Credential Manager..."));
    credentials->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+C")));
    connect(credentials, &QAction::triggered, this,
            &MainWindow::openCredentialManager);

    // --- Vault -----------------------------------------------------------
    //
    // Its own menu rather than three more items under Edit. Unlocking is not
    // an edit, and the state it reports -- locked, unlocked, no vault at all
    // -- is worth having one place to look at.
    QMenu *vaultMenu = bar->addMenu(tr("&Vault"));
    QAction *unlockAction = vaultMenu->addAction(tr("&Unlock..."));
    connect(unlockAction, &QAction::triggered, this, [this] {
        if (ensureVaultUnlocked()) {
            statusBar()->showMessage(tr("Vault unlocked."), 4000);
            refreshVaultPill();
        }
    });
    QAction *lockAction = vaultMenu->addAction(tr("&Lock"));
    connect(lockAction, &QAction::triggered, this, &MainWindow::lockVault);
    vaultMenu->addSeparator();
    QAction *changeMaster = vaultMenu->addAction(tr("Change &Master Password..."));
    connect(changeMaster, &QAction::triggered, this,
            &MainWindow::changeMasterPassword);
    QAction *forget = vaultMenu->addAction(tr("&Forget Saved Password"));
    connect(forget, &QAction::triggered, this, &MainWindow::forgetKeyringEntry);

    // The two that only make sense in one state say so at the moment the menu
    // opens, rather than being wired once at startup and going stale the first
    // time anything unlocks.
    connect(vaultMenu, &QMenu::aboutToShow, this,
            [this, unlockAction, lockAction, changeMaster] {
                const bool locked = !vault_ || vault_->isLocked();
                refreshVaultPill();
                unlockAction->setEnabled(locked);
                lockAction->setEnabled(!locked);
                changeMaster->setEnabled(!locked);
            });

    // --- View ------------------------------------------------------------
    //
    // Not a stub. The theme submenu is live from 4a because it is the only
    // way to exercise the walk, and because a theme switch is the one thing
    // this window can already do end to end.
    QMenu *view = bar->addMenu(tr("&View"));

    // The manual half of the stranded-window handling. The automatic half runs
    // on screen changes and only acts when the window is actually unreachable;
    // this is for the rest -- a window dragged mostly off an edge, or one on a
    // screen that is technically still there and inconvenient anyway.
    //
    // Ctrl+Shift+Home rather than Ctrl+W: Ctrl+W is Close Tab above, and two
    // QActions on one sequence is an ambiguous shortcut overload, which fires
    // NEITHER of them. Ctrl+Shift is also the modifier a terminal conventially
    // keeps for itself, so this does not take a key off the far end.
    QAction *fitWindow = view->addAction(tr("&Fit Window to Screen"));
    fitWindow->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+Home")));
    connect(fitWindow, &QAction::triggered, this, [this] {
        placeOnScreen(rescueTargetScreen(), /*fitToScreen=*/true);
    });

    QAction *centreWindow = view->addAction(tr("&Centre Window Here"));
    centreWindow->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+End")));
    connect(centreWindow, &QAction::triggered, this, [this] {
        placeOnScreen(rescueTargetScreen(), /*fitToScreen=*/false);
    });

    view->addSeparator();
    QMenu *themeMenu = view->addMenu(tr("&Theme"));

    auto *group = new QActionGroup(this);
    group->setExclusive(true);
    for (const std::string &name : themes_->names()) {
        const QString slug = QString::fromStdString(name);
        QAction *action = themeMenu->addAction(titleCase(slug));
        action->setCheckable(true);
        action->setData(slug);
        group->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, slug] { applyThemeByName(slug); });
    }

    // --- Help ------------------------------------------------------------
    // Last, which is where every platform's convention puts it.
    QMenu *help = bar->addMenu(tr("&Help"));

    QAction *helpAction = help->addAction(tr("Omega &Help"));
    helpAction->setShortcut(QKeySequence::HelpContents);  // F1
    connect(helpAction, &QAction::triggered, this,
            [this] { HelpDialog::open(this); });

    // Deep links into the same window. Worth having as menu items rather than
    // leaving people to find the tabs: the two questions they answer are the
    // two people actually ask, and neither is obviously "help" until you have
    // already opened it.
    for (const HelpTopic &topic : helpTopics()) {
        const QString id = topic.id;
        connect(help->addAction(topic.title), &QAction::triggered, this,
                [this, id] { HelpDialog::open(this, id); });
    }

    help->addSeparator();

    QAction *aboutAction = help->addAction(tr("&About Omega"));
    connect(aboutAction, &QAction::triggered, this, [this] {
        // The accent travels into the markup because a widget carrying a
        // stylesheet ignores a palette set on it, and the theme QSS says
        // nothing about anchors -- so Qt's default dark blue link is very
        // nearly invisible on a dark background. An offscreen grab is what
        // caught that; neither the HTML nor the stylesheet mentions links, so
        // reading either one says nothing is wrong.
        //
        // This is the one place that knows which theme is applied, which is
        // why the colour is chosen here and not in aboutHtml().
        QString accent;
        if (const theme::Theme *t = themes_->get(themeName_.toStdString())) {
            accent = QString::fromStdString(t->accent_color);
        }
        AboutDialog(accent, this).exec();
    });
}

void MainWindow::restoreGeometryFromSettings() {
    const AppSettings &s = settings_->settings();

    resize(s.window_width, s.window_height);
    if (s.window_x && s.window_y) {
        move(*s.window_x, *s.window_y);
    }
    // The saved position names a point on the monitor layout that existed the
    // last time this window closed. If that layout is gone -- undocked, second
    // head unplugged, a dock that handed back a different arrangement -- the
    // window has just been placed somewhere nobody can reach, and every launch
    // from here on repeats it, because closeEvent saves the same coordinates
    // straight back.
    //
    // Checked before showMaximized() below, and before the window is shown at
    // all: correcting it here means it is never wrong on screen, rather than
    // appearing off-screen and jumping.
    if (!windowIsGrabbable()) {
        placeOnScreen(rescueTargetScreen(), /*fitToScreen=*/false);
    }

    if (s.window_maximized) {
        showMaximized();
    }

    // tree_width is stored by both applications and read by neither: nterm-qt
    // hardcodes its splitter to 250/950 and never looks at the field. Omega
    // honours it. Writing a field the other application ignores cannot make
    // the two disagree, and a splitter that forgets where it was is the sort
    // of thing that gets noticed every single launch.
    const int tree = qMax(80, s.tree_width);
    splitter_->setSizes({tree, qMax(200, s.window_width - tree)});
}

// --- stranded windows --------------------------------------------------------

bool MainWindow::windowIsGrabbable() const {
    // frameGeometry() equals geometry() until the window has been shown and the
    // window manager has answered with its frame. That is fine here: on the
    // frameless path they are equal forever, and on the native path the only
    // consequence before the first show is that the band checked is the menu
    // bar rather than the title bar directly above it. Both are on the same
    // screen or neither is.
    const QRect frame = frameGeometry();
    const QRect band(frame.left(), frame.top(), frame.width(),
                     qMin(kGrabBandHeight, frame.height()));

    const QList<QScreen *> screens = QGuiApplication::screens();
    for (const QScreen *screen : screens) {
        const QRect hit = screen->availableGeometry().intersected(band);
        if (hit.width() >= qMin(kMinGrabWidth, band.width()) &&
            hit.height() >= qMin(kGrabBandHeight / 2, band.height())) {
            return true;
        }
    }
    return false;
}

QScreen *MainWindow::rescueTargetScreen() {
    // Where the pointer is, which is where the person looking for the window
    // is looking. Falling back to the primary rather than to the window's own
    // screen(): its own screen is the one that just went away.
    if (QScreen *under = QGuiApplication::screenAt(QCursor::pos())) {
        return under;
    }
    return QGuiApplication::primaryScreen();
}

void MainWindow::placeOnScreen(QScreen *screen, bool fitToScreen) {
    if (!screen) {
        return;
    }

    // A maximised or fullscreen window ignores setGeometry outright -- the call
    // succeeds, the window does not move, and the bug reads as "the shortcut
    // does nothing". Drop to normal first.
    if (isMaximized() || isFullScreen()) {
        showNormal();
    }

    const QRect avail = screen->availableGeometry();

    // What the window manager adds around the client area. Zero on the
    // frameless path. Sampled BEFORE the geometry changes: frameGeometry() is
    // whatever the window manager last told us, and it does not update until
    // it has answered the resize.
    const QRect frame = frameGeometry();
    const QRect client = geometry();
    const int frameW = qMax(0, frame.width() - client.width());
    const int frameH = qMax(0, frame.height() - client.height());
    const int frameLeft = qMax(0, client.left() - frame.left());
    const int frameTop = qMax(0, client.top() - frame.top());

    // The largest client area that fits on this screen with its frame.
    const QSize ceiling(qMax(1, avail.width() - frameW),
                        qMax(1, avail.height() - frameH));

    QSize target = fitToScreen
                       ? QSize(ceiling.width() - kFitMargin * 2,
                               ceiling.height() - kFitMargin * 2)
                       : client.size();

    // Order matters. Grow to what the layout needs first, then clamp to what
    // the screen has -- doing it the other way round hands back a size larger
    // than the screen whenever the minimum exceeds it, which is exactly the
    // 4K-saved-size-onto-a-laptop-panel case this exists for. A window clamped
    // below its minimum is one Qt will grow again on show; clamped to the
    // screen it is at least reachable.
    target = target.expandedTo(minimumSizeHint()).boundedTo(ceiling);

    const QPoint topLeft(
        avail.x() + (avail.width() - (target.width() + frameW)) / 2 + frameLeft,
        avail.y() + (avail.height() - (target.height() + frameH)) / 2 + frameTop);

    setGeometry(QRect(topLeft, target));
}

void MainWindow::watchScreen(QScreen *screen) {
    if (!screen) {
        return;
    }
    // `this` as the context object, so these disconnect when the window dies --
    // and Qt deletes the QScreen after screenRemoved, which disconnects the
    // other end. Nothing here has to be undone by hand.
    //
    // Both signals, not just one: unplugging a head fires geometryChanged on
    // the survivors as the layout is repacked, while a taskbar or menu bar
    // appearing only moves availableGeometry.
    connect(screen, &QScreen::geometryChanged, this,
            [this](const QRect &) { scheduleScreenCheck(); });
    connect(screen, &QScreen::availableGeometryChanged, this,
            [this](const QRect &) { scheduleScreenCheck(); });
}

void MainWindow::scheduleScreenCheck() {
    if (screenCheckTimer_) {
        screenCheckTimer_->start();
    }
}

void MainWindow::checkWindowReachable() {
    // Only when it is actually unreachable. A window that is merely half off
    // the edge is where somebody put it, and moving it back would be the
    // application arguing with them every time a screen blinks.
    if (isFullScreen() || windowIsGrabbable()) {
        return;
    }

    placeOnScreen(rescueTargetScreen(), /*fitToScreen=*/false);
    statusBar()->showMessage(
        tr("Display layout changed; window moved back on screen."), 6000);
}

bool MainWindow::applyThemeByName(const QString &name, bool persist) {
    const theme::Theme *t = themes_->get(name.toStdString());
    if (!t) {
        return false;
    }
    themes_->setCurrent(name.toStdString());
    if (persist) {
        settings_->settings().theme_name = name;
    }
    applyTheme(*t);

    // Keep the radio group honest when the theme was set from somewhere other
    // than the menu -- at startup, or later from the settings dialog.
    const QList<QAction *> actions = menuBar_->findChildren<QAction *>();
    for (QAction *action : actions) {
        if (action->isCheckable() && action->data().toString() == name) {
            action->setChecked(true);
        }
    }
    return true;
}

void MainWindow::applyTheme(const theme::Theme &t) {
    themeName_ = QString::fromStdString(t.name);

    // Which sheet. Classic is theme/stylesheet.cpp, the byte-for-byte port of
    // nterm-qt's; Token is theme/tokenstylesheet.cpp, the redesign. Read from
    // omega.json rather than config.json -- see OmegaSettings.
    const Chrome chrome =
        omegaSettings_ ? omegaSettings_->settings().chrome : fallbackOmega_.chrome;
    const bool token = chrome == Chrome::Token;

    const int uiFontPx = omegaSettings_ ? omegaSettings_->settings().ui_font_size
                                        : fallbackOmega_.ui_font_size;

    // Set on the application, and set on BOTH paths. The token sheet needs it
    // as well as the font: the sheet covers the widgets it names and the
    // application font covers the rest -- menus and popups it does not reach,
    // and every dialog. The classic sheet names no font-size at all, so there
    // the application font is the only lever.
    //
    // setPixelSize rather than setPointSize: the sheet is in px, and asking
    // for the same number in two units puts the chrome and the menus a
    // fraction apart at any DPI other than 96.
    QFont appFont = QApplication::font();
    if (appFont.pixelSize() != uiFontPx) {
        appFont.setPixelSize(uiFontPx);
        QApplication::setFont(appFont);
    }

    const theme::Tokens tokens = theme::tokensFromTheme(t);
        setCurrentTokens(tokens);
    const QString qss = QString::fromStdString(
        token ? theme::generateTokenStylesheet(tokens, uiFontPx)
              : theme::generateStylesheet(t));

    // Three roles a stylesheet cannot reach, set on the application rather
    // than on this window because a modal dialog is not a child of it:
    //
    //   PlaceholderText  QLineEdit::placeholder is not a Qt pseudo-element.
    //                    Qt ignores unknown ones silently, so a rule for it
    //                    looks right in the sheet and does nothing on screen;
    //                    the filter box's placeholder is otherwise drawn in a
    //                    50%-alpha copy of the text colour, which on a dark
    //                    theme is close enough to the real text to read as one.
    //   Highlight        The session tree's selection is painted by the style
    //                    until the delegate lands, and the style reads the
    //                    palette, not the sheet.
    //   ToolTipBase/Text QToolTip is a top-level window on some platforms and
    //                    does not always inherit the application sheet.
    QPalette pal = QApplication::palette();
    if (token) {
        const auto q = [](const theme::Rgba &c) {
            return QColor(c.r, c.g, c.b, c.a);
        };
        pal.setColor(QPalette::PlaceholderText, q(tokens.inkFaint));
        pal.setColor(QPalette::Highlight, q(tokens.bgSelected));
        pal.setColor(QPalette::HighlightedText, q(tokens.ink));
        pal.setColor(QPalette::ToolTipBase, q(tokens.bgChrome));
        pal.setColor(QPalette::ToolTipText, q(tokens.ink));
        QApplication::setPalette(pal);
    }

    // ONE walk, over every window this application owns. Right now that is
    // this window alone, and the loop is here so that a detached window added
    // in Phase 5 is themed by existing code rather than by someone remembering
    // to add a second list.
    for (auto it = windows_.begin(); it != windows_.end();) {
        if (it->isNull()) {
            it = windows_.erase(it);  // a window that has since closed
            continue;
        }
        (*it)->setStyleSheet(qss);
        ++it;
    }

    // Cached per state, so the previous theme's dots would otherwise stay on
    // the tabs. Dropped before the tabs are re-iconed below.
    clearDotCache();

    // The painted widgets, which no stylesheet reaches: their colours are
    // values rather than roles. Same walk, one more pass.
    if (chromeBar_) chromeBar_->setTokens(tokens);
    if (treePane_) {
        treePane_->setTokens(tokens);
        // The one widget in the window a stylesheet font-size does not reach:
        // SessionDelegate paints its two lines itself. Same number the sheet
        // and the application font were given above.
        treePane_->setBaseFontSize(uiFontPx);
    }
    // The per-tab close buttons paint from tokens too. They live on the tab
    // bar rather than on the tab, because that is where setTabButton parents
    // them.
    if (tabs_) {
        const QList<WindowButton *> closers =
            tabs_->tabBar()->findChildren<WindowButton *>();
        for (WindowButton *b : closers) b->setTokens(tokens);
    }
    if (sizeGrip_) sizeGrip_->setTokens(tokens);

    // And every terminal. The stylesheet does not reach inside the widget --
    // anytermqt paints from a palette, not from QSS -- so this is a second
    // pass over the same one walk rather than a second place that themes.
    for (int i = 0; i < tabs_->count(); ++i) {
        if (auto *tab = qobject_cast<TerminalTab *>(tabs_->widget(i))) {
            // Not `t`: a tab that pinned a theme keeps it across a window
            // theme change, which is the whole point of pinning one.
            tab->applyTheme(themeForTab(tab, t));

            // The dot stays on the WINDOW's tokens even for a pinned tab. It
            // is painted on the tab bar, which is chrome -- a row of dots in
            // assorted palettes reads as a rendering fault, and the dot's job
            // is to be comparable across tabs.
            tabs_->setTabIcon(i, linkDotIcon(tab->link(), tokens));
        }
    }
}

// --- the frameless window's resize edges -----------------------------------
//
// Qt::FramelessWindowHint takes the window manager's resize handles along with
// its title bar, so they have to be put back. The window cannot simply override
// its own mouseMoveEvent: the session tree, the terminal and the status bar all
// reach the window border and each consumes the events in the hit zone before
// the window sees them. Hence an application-wide filter, installed in the
// constructor and only on the frameless path.

Qt::Edges MainWindow::edgesAt(const QPoint &global) const {
    const QRect r = frameGeometry();
    if (!r.adjusted(-kResizeMargin, -kResizeMargin, kResizeMargin,
                    kResizeMargin)
             .contains(global)) {
        return {};
    }

    Qt::Edges edges;
    if (global.x() <= r.left() + kResizeMargin) edges |= Qt::LeftEdge;
    if (global.x() >= r.right() - kResizeMargin) edges |= Qt::RightEdge;
    if (global.y() <= r.top() + kResizeMargin) edges |= Qt::TopEdge;
    if (global.y() >= r.bottom() - kResizeMargin) edges |= Qt::BottomEdge;
    return edges;
}

void MainWindow::applyResizeCursor(Qt::Edges edges) {
    Qt::CursorShape shape = Qt::ArrowCursor;
    if ((edges & Qt::LeftEdge && edges & Qt::TopEdge) ||
        (edges & Qt::RightEdge && edges & Qt::BottomEdge)) {
        shape = Qt::SizeFDiagCursor;
    } else if ((edges & Qt::RightEdge && edges & Qt::TopEdge) ||
               (edges & Qt::LeftEdge && edges & Qt::BottomEdge)) {
        shape = Qt::SizeBDiagCursor;
    } else if (edges & (Qt::LeftEdge | Qt::RightEdge)) {
        shape = Qt::SizeHorCursor;
    } else if (edges & (Qt::TopEdge | Qt::BottomEdge)) {
        shape = Qt::SizeVerCursor;
    }

    if (shape == Qt::ArrowCursor) {
        if (cursorOverridden_) {
            QApplication::restoreOverrideCursor();
            cursorOverridden_ = false;
        }
        return;
    }

    // EXACTLY ONE override at a time. setOverrideCursor pushes onto a stack;
    // pushing a second without popping the first is how a resize cursor gets
    // stuck over the whole application with no way back.
    if (cursorOverridden_) {
        QApplication::changeOverrideCursor(QCursor(shape));
    } else {
        QApplication::setOverrideCursor(QCursor(shape));
        cursorOverridden_ = true;
    }
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    // A maximised window has no edges to drag, and a fullscreen one has none
    // either -- offering the cursor there is a lie.
    if (!frameless_ || isMaximized() || isFullScreen()) {
        return QMainWindow::eventFilter(watched, event);
    }

    // Only events belonging to this window. A modal dialog on top has its own
    // borders and must not be resized by the shell's filter -- and without
    // this the settings dialog's own edge is a handle for the window behind
    // it.
    if (auto *w = qobject_cast<QWidget *>(watched)) {
        if (w->window() != this) {
            return QMainWindow::eventFilter(watched, event);
        }
    }

    switch (event->type()) {
        case QEvent::MouseMove: {
            auto *me = static_cast<QMouseEvent *>(event);
            // Only with no button down: during a drag the cursor belongs to
            // whatever started the drag.
            if (me->buttons() == Qt::NoButton) {
                applyResizeCursor(edgesAt(me->globalPosition().toPoint()));
            }
            break;
        }
        case QEvent::MouseButtonPress: {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() != Qt::LeftButton) break;
            const Qt::Edges edges = edgesAt(me->globalPosition().toPoint());
            if (!edges) break;
            if (QWindow *handle = windowHandle()) {
                handle->startSystemResize(edges);
                // Swallowed: the child under the cursor must not also react,
                // or a drag on the tree's left edge starts a resize AND a
                // selection.
                return true;
            }
            break;
        }
        case QEvent::Leave:
        case QEvent::WindowDeactivate:
            applyResizeCursor({});
            break;
        default:
            break;
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::changeEvent(QEvent *event) {
    if (event->type() == QEvent::WindowStateChange) {
        if (chromeBar_) chromeBar_->setMaximised(isMaximized());
        // A maximised window has no edges, so a cursor left over from the
        // moment before it maximised would stay a resize cursor forever.
        if (isMaximized()) applyResizeCursor({});
    }
    QMainWindow::changeEvent(event);
}

// The applied theme, or the engine's fallback when the name resolves to
// nothing. Small, and it exists because three call sites were each doing the
// lookup and each deciding differently what to do when it failed.
const theme::Theme &MainWindow::currentThemeOrFallback() const {
    if (const theme::Theme *t = themes_->get(themeName_.toStdString())) {
        return *t;
    }
    return themes_->current();
}

const theme::Theme &MainWindow::themeForTab(const TerminalTab *tab,
                                            const theme::Theme &windowTheme) const {
    if (!tab) return windowTheme;
    const QString pinned = tab->themeOverride();
    if (pinned.isEmpty()) return windowTheme;
    if (const theme::Theme *t = themes_->get(pinned.toStdString())) {
        return *t;
    }
    // Named a theme this installation does not have. The window's theme is a
    // better answer than the engine's fallback -- the session came from a
    // machine that had it, and dropping the tab to enterprise_dark would look
    // like a bug rather than like a missing file.
    return windowTheme;
}

void MainWindow::openSettings() {
    // Held so Cancel can put it back. The preview applies to the real window
    // rather than to a swatch -- seeing a theme means seeing it on the tree,
    // the tabs and a live terminal at once -- so undoing it is this side's
    // job, not the dialog's.
    const QString themeBefore = themeName_;

    SettingsDialog dialog(settings_->settings(),
                          omegaSettings_ ? omegaSettings_->settings()
                                         : OmegaSettings(),
                          themes_, this);
    connect(&dialog, &SettingsDialog::themePreviewRequested, this,
            [this](const QString &name) { applyThemeByName(name); });

    if (dialog.exec() != QDialog::Accepted) {
        applyThemeByName(themeBefore);
        return;
    }

    settings_->settings() = dialog.appSettings();
    if (!settings_->save()) {
        statusBar()->showMessage(settings_->error(), 8000);
    }

    if (omegaSettings_) {
        omegaSettings_->settings() = dialog.omegaSettings();
        if (!omegaSettings_->save()) {
            statusBar()->showMessage(omegaSettings_->error(), 8000);
        }
    }

    // TABS FIRST, THEN THE THEME. applySettingsToTabs refreshes each tab's
    // theme pin from the store but does not paint; applyThemeByName's walk is
    // what paints, and it reads the pins. The other order leaves a session
    // whose pin changed while its tab was open showing the old theme until
    // something else triggers a walk.
    applySettingsToTabs();

    // The theme is already on screen from the preview; this settles the View
    // menu's check mark on it, which the preview does too but which a
    // not-installed theme name would have left on the old entry.
    applyThemeByName(settings_->settings().theme_name);
}

void MainWindow::applySettingsToTabs() {
    // Live rather than on the next connect. Most of these are read once when
    // a tab is built, so a settings dialog that only affected new tabs would
    // be a dialog whose effect nobody could see.
    //
    // RE-RESOLVED, NOT COPIED. This used to assign the globals straight onto
    // every tab, which threw away the per-session overrides on every tab that
    // had any -- pressing Save in Settings reset a session's scrollback,
    // paste threshold, anti-idle and wheel behaviour to the globals until the
    // next reconnect. The database still held the overrides, so the session
    // came back correct the following day and the fault looked intermittent.
    //
    // Going back through resolveTabSettings is what makes that impossible:
    // the live path and the open path now inherit by the same description
    // rather than by two, and a field added to one is a field both get.
    const AppSettings &s = settings_->settings();
    const OmegaSettings &o =
        omegaSettings_ ? omegaSettings_->settings() : fallbackOmega_;

    for (int i = 0; i < tabs_->count(); ++i) {
        auto *tab = qobject_cast<TerminalTab *>(tabs_->widget(i));
        if (!tab) continue;

        // Re-read rather than cached: the session may have been edited since
        // the tab was opened, and the store is the only thing that knows. A
        // quick-connect tab has no id and a saved session may have been
        // deleted while its tab stayed open -- both land on the globals,
        // which is what they were opened with.
        TabSettings resolved = resolveTabSettings(s, o);
        if (tab->sessionId() >= 0 && sessions_) {
            if (auto row = sessions_->getSession(tab->sessionId())) {
                resolved = resolveTabSettings(*row, s, o);
            }
        }

        tab->setMultilinePasteThreshold(resolved.multilinePasteThreshold);
        tab->setScrollbackLines(resolved.scrollbackLines);
        tab->setFontPointSize(resolved.fontPointSize);
        tab->setAntiIdle(resolved.antiIdle);
        tab->setWheelAltScreen(resolved.wheelAltScreen);

        // The theme pin, so a session that gained or lost one since the tab
        // opened is not stuck with the old answer. Painting is left to the
        // applyThemeByName() walk that runs just before this -- calling
        // applyTheme() here as well would theme every tab twice on every
        // save.
        tab->setThemeOverride(resolved.themeName);

        // pasteBaud is deliberately NOT pushed. -1 means "let the tab pick
        // from the transport", and the tab picked at start() from a transport
        // this function cannot see; re-asserting it here would overwrite a
        // serial line's own rate with a sentinel.
        if (resolved.pasteBaud >= 0) {
            tab->setPasteBaud(resolved.pasteBaud);
        }
    }
}

void MainWindow::registerWindow(QWidget *window) {
    if (!window) return;
    for (const auto &w : windows_) {
        if (w.data() == window) return;
    }
    windows_.append(QPointer<QWidget>(window));
}

QString MainWindow::currentThemeName() const {
    return themeName_;
}

int MainWindow::treeWidth() const {
    const QList<int> sizes = splitter_->sizes();
    return sizes.isEmpty() ? 0 : sizes.first();
}

void MainWindow::setTreeWidth(int width) {
    // qMax rather than the raw remainder: a splitter asked for a right pane of
    // zero or less collapses it, and a collapsed tab area looks like a broken
    // window rather than a narrow one.
    splitter_->setSizes({width, qMax(200, this->width() - width)});
}

void MainWindow::quickConnect() {
    // The dialog gets the vault, not a copy of anything in it: it lists names
    // to choose from, and the name is all that crosses into Config. A refused
    // or locked vault leaves the picker empty and the manual fields working,
    // which is the whole point of quick connect.
    QuickConnectDialog dialog(vault_.get(), this);
    if (dialog.exec() != QDialog::Accepted) return;

    // Through the resolver like every other route. The dialog chose the
    // transport fields explicitly; the tab settings still come from the
    // globals, and there is one place that decides that.
    openTab(resolveQuickConnect(dialog.config(), settings_->settings(),
                                omegaSettings_ ? omegaSettings_->settings()
                                               : OmegaSettings{}));
}

void MainWindow::openSession(const sessions::Session &session) {
    // Everything the session says, everything it does not say from the
    // globals. See effectiveconfig.h -- this function used to hold that logic
    // and now holds none of it.
    const long long handle = vault_ ? vault_->handle() : 0;
    const OmegaSettings omega =
        omegaSettings_ ? omegaSettings_->settings() : OmegaSettings{};

    // --- the credential a session does not name ---------------------------
    //
    // Only SSH sessions with no credential of their own reach any of this;
    // resolveSession applies the mode and every other session comes through
    // it untouched.
    const bool needsDefault =
        session.transport == sessions::SessionTransport::Ssh &&
        !(session.credential_name && !session.credential_name->empty());

    QString defaultCredential;
    if (needsDefault && omega.ssh_default_auth == SshDefaultAuth::VaultDefault &&
        vault_ && vault_->isOpen()) {
        // A locked vault WAS left alone here, on the grounds that the user
        // may have meant to connect with an agent key. That reasoning ends
        // where this mode begins: the connection now depends on a credential
        // only the unlocked vault can name, so asking is the honest move and
        // refusing to ask would be a connect that fails for a reason the
        // application knew in advance. Cancel falls through to the prompt
        // below rather than aborting -- the answer to "I do not want to
        // unlock" is still a connection, with typed credentials.
        if (!vault_->isLocked() || ensureVaultUnlocked()) {
            vault_->defaultName(&defaultCredential);
        }
    }

    Resolved resolved =
        resolveSession(session, settings_->settings(), omega, handle,
                       defaultCredential);

    // Nothing resolved and the mode wants one: ask. Under Ask this is every
    // time; under VaultDefault only when the vault named nothing, which is
    // the ordinary state of a vault nobody has marked a default in.
    if (needsDefault && resolved.config.credential.isEmpty() &&
        omega.ssh_default_auth != SshDefaultAuth::Agent) {
        const QString target = QStringLiteral("%1 (%2)")
                                   .arg(QString::fromStdString(session.name),
                                        QString::fromStdString(session.hostname));
        CredentialPromptDialog dialog(target, vault_.get(), this);
        if (dialog.exec() != QDialog::Accepted) {
            // No tab. A cancelled credential prompt is a cancelled connect,
            // not a connect with nothing -- which would fail at the far end
            // and leave a tab saying so.
            statusBar()->showMessage(tr("Connection cancelled."), 4000);
            return;
        }
        const QString chosen = dialog.credential();
        if (!chosen.isEmpty()) {
            resolved.config.credential = chosen;
            resolved.config.vaultHandle = handle;
        }
        // Typed fields win over the reference, the same rule the library
        // applies: what is left blank comes from the credential.
        if (!dialog.username().isEmpty()) {
            resolved.config.username = dialog.username();
        }
        if (!dialog.password().isEmpty()) {
            resolved.config.password = dialog.password();
        }
    }

    openTab(resolved, session.id ? *session.id : -1);
}

void MainWindow::openTab(const Resolved &resolved, qint64 sessionId) {
    auto *tab = new TerminalTab(tabs_);
    tab->setSessionId(sessionId);

    // Registered before it dials, so the row shows a dot from the first frame
    // rather than staying idle until the first state arrives.
    live_.attach(sessionId, tab);
    connect(tab, &TerminalTab::linkChanged, this,
            [this, sessionId, tab](Link link) {
                live_.setLink(sessionId, tab, link);
                // The tab's own dot. Looked up by widget rather than captured
                // as an index, because a tab that was moved or one closed
                // before it has an index that no longer means anything.
                const int at = tabs_->indexOf(tab);
                if (at >= 0) {
                    tabs_->setTabIcon(
                        at, linkDotIcon(link, theme::tokensFromTheme(
                                                  currentThemeOrFallback())));
                }
            });
    connect(tab, &QObject::destroyed, this,
            [this, sessionId](QObject *o) { live_.detach(sessionId, o); });

    const omegassh::Config &withDefaults = resolved.config;

    // Themed BEFORE it is shown and before it dials. A tab that appeared in
    // the default palette and corrected itself a frame later would be visible
    // every single time, and the overlay is on screen from the first frame.
    //
    // The pin goes on FIRST. themeForTab reads it, so setting it after this
    // would paint the window's theme now and the session's only at the next
    // theme change -- a pinned tab that looks like it ignored its own setting
    // until you touched Settings.
    tab->setThemeOverride(resolved.tab.themeName);
    tab->applyTheme(themeForTab(tab, currentThemeOrFallback()));

    // Same ownership as the theme, and now one source: the window resolved
    // these before building the tab, so a saved session's overrides and the
    // global defaults arrive by the same path.
    // Before start(), so the first tab text the window writes is already the
    // session's name rather than the address it corrects a moment later.
    tab->setDisplayName(resolved.displayName);

    tab->setMultilinePasteThreshold(resolved.tab.multilinePasteThreshold);
    tab->setScrollbackLines(resolved.tab.scrollbackLines);
    tab->setAntiIdle(resolved.tab.antiIdle);
    tab->setFontPointSize(resolved.tab.fontPointSize);

    // -1 leaves the tab to pick from the transport, which is where the serial
    // line's own rate and telnet's 9600 come from. Only an explicit override
    // is pushed, so start() keeps choosing for every session that has none.
    if (resolved.tab.pasteBaud >= 0) {
        tab->setPasteBaud(resolved.tab.pasteBaud);
    }

    tab->setWheelAltScreen(resolved.tab.wheelAltScreen);

    const int index = tabs_->addTab(tab, tr("connecting..."));

    // The tab's own close button. setTabsClosable(true) alone gives you
    // QStyle::SP_TabCloseButton, which under Fusion is a red cross that
    // belongs to no theme -- and the ported sheet's answer to that,
    // `image: none`, does not remove it so much as make it invisible: measured
    // at 20x20, still clickable, one distinct colour drawn. A widget of our
    // own is the only way to theme it, so the same painted glyph the title bar
    // uses does the job at a smaller size.
    auto *closeButton = new WindowButton(WindowButton::Close, tabs_->tabBar());
    closeButton->setFixedSize(18, 18);
    connect(closeButton, &QAbstractButton::clicked, this, [this, tab] {
        const int at = tabs_->indexOf(tab);
        if (at >= 0) closeTab(at);
    });
    tabs_->tabBar()->setTabButton(index, QTabBar::RightSide, closeButton);
    if (const theme::Theme *t = themes_->get(themeName_.toStdString())) {
        closeButton->setTokens(theme::tokensFromTheme(*t));
    }

    tabs_->setCurrentIndex(index);

    connect(tab, &TerminalTab::statusChanged, this,
            [this, tab](const QString &text) {
                // Only the visible tab writes to the status bar. Four tabs
                // dialing at once would otherwise fight over it.
                if (tabs_->currentWidget() == tab) {
                    statusBar()->showMessage(text, 4000);
                }
            });

    connect(tab, &TerminalTab::finished, this, [this, tab](int) {
        const int at = tabs_->indexOf(tab);
        if (at < 0) return;
        // The tab is NOT closed on its own. A session that failed leaves the
        // reason on the overlay, and a session that ended leaves its
        // scrollback -- both are things people read after the fact. Closing
        // here would take the answer away at the moment it was wanted.
        tabs_->setTabText(at, tr("%1 (closed)").arg(tab->title()));
    });

    // The other direction. Nothing else takes "(closed)" back off, so without
    // this a reconnected tab runs a live session under a title saying it
    // ended -- and the tab text is the only place a tab that is not in front
    // says anything at all.
    connect(tab, &TerminalTab::restarted, this, [this, tab] {
        const int at = tabs_->indexOf(tab);
        if (at < 0) return;
        tabs_->setTabText(at, tab->title());
    });

    // --- a question only a person can answer --------------------------------
    //
    // Wired beside the credential prompt and for the same reason: every route
    // into a tab goes through openTab, so a quick-connect tab and a saved
    // session get the same prompt from the same code.
    connect(tab, &TerminalTab::keyboardQuestion, this,
            [this, tab](const QString &target, const QString &question,
                        bool secret, int attempt, int limit) {
                if (tabs_->indexOf(tab) < 0) return;
                tabs_->setCurrentWidget(tab);

                KeyboardPromptDialog dialog(target, question, secret, attempt,
                                            limit, this);
                if (dialog.exec() != QDialog::Accepted) {
                    tab->abandonKeyboardRetry();
                    statusBar()->showMessage(tr("Connection cancelled."), 4000);
                    return;
                }
                if (tabs_->indexOf(tab) < 0) return;

                // An empty answer is sent as given rather than treated as a
                // cancel: some stacks accept an empty response for an
                // optional factor, and second-guessing that here would make
                // the dialog refuse an answer the far end would have taken.
                tab->retryWithKeyboardAnswer(question, dialog.answer());
            });

    // --- credentials the far end refused -----------------------------------
    //
    // Wired HERE rather than in quickConnect() and openSession(), which is
    // what makes this one behaviour instead of two: every route into a tab
    // goes through openTab, so a quick-connect tab and a saved-session tab get
    // the same prompt from the same code. The tab does not know which it is,
    // and after this it still does not.
    //
    // The tab is mid-teardown until one of the two calls below lands, so both
    // arms of every branch must end in exactly one of them.
    connect(tab, &TerminalTab::credentialsRejected, this,
            [this, tab](const QString &target) {
                // The window may have gone away under a modal, and the tab may
                // have been closed while the prompt was being raised.
                if (tabs_->indexOf(tab) < 0) return;

                // The tab it happened on, brought to the front first. Four
                // tabs dialing at once means the prompt has to say which one
                // it belongs to, and moving to it says so better than the
                // title alone.
                tabs_->setCurrentWidget(tab);

                CredentialPromptDialog dialog(
                    target, vault_.get(),
                    CredentialPromptDialog::Reason::Rejected, this);
                if (dialog.exec() != QDialog::Accepted) {
                    tab->abandonCredentialRetry();
                    statusBar()->showMessage(tr("Connection cancelled."), 4000);
                    return;
                }

                // Closed under the modal. Nothing to retry into.
                if (tabs_->indexOf(tab) < 0) return;

                tab->retryWithCredentials(
                    dialog.credential(), vault_ ? vault_->handle() : 0,
                    dialog.username(), dialog.password());
            });

    if (!tab->start(withDefaults)) {
        // A configuration mistake, decided before anything was opened. The
        // reason is already on the overlay; the tab stays so it can be read.
        tabs_->setTabText(index, tr("failed"));
        statusBar()->showMessage(tab->error(), 8000);
        return;
    }

    // summary() is populated the moment start() returns rather than when the
    // dial lands, so the tab is titled while it is still connecting.
    tabs_->setTabText(index, tab->title());
}

void MainWindow::closeTab(int index) {
    if (index < 0 || index >= tabs_->count()) return;
    auto *tab = qobject_cast<TerminalTab *>(tabs_->widget(index));
    tabs_->removeTab(index);
    if (tab) {
        // terminate() returns immediately even mid-dial; the socket may
        // outlive the tab by up to the connect timeout, which is invisible
        // here and the right trade for never blocking a close.
        tab->terminate();
        tab->deleteLater();
    }
}

void MainWindow::closeAllTabs() {
    while (tabs_->count() > 0) closeTab(tabs_->count() - 1);
}

// ---------------------------------------------------------------------------
// Tab bar context menu

QVector<TerminalTab *> MainWindow::tabsExcept(int keep) const {
    QVector<TerminalTab *> out;
    for (int i = 0; i < tabs_->count(); ++i) {
        if (i == keep) continue;
        if (auto *tab = qobject_cast<TerminalTab *>(tabs_->widget(i))) {
            out << tab;
        }
    }
    return out;
}

QVector<TerminalTab *> MainWindow::tabsAfter(int index) const {
    QVector<TerminalTab *> out;
    for (int i = index + 1; i < tabs_->count(); ++i) {
        if (auto *tab = qobject_cast<TerminalTab *>(tabs_->widget(i))) {
            out << tab;
        }
    }
    return out;
}

bool MainWindow::closeTabSet(const QVector<TerminalTab *> &victims) {
    if (victims.isEmpty()) return false;

    TabCloseSet set;
    set.total = static_cast<int>(victims.size());
    for (TerminalTab *tab : victims) {
        if (tab->isRunning()) {
            set.liveTitles << tab->title();
        }
    }

    if (set.needsConfirmation()) {
        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(tr("Close Tabs"));
        box.setText(tabCloseQuestion(set));
        box.setInformativeText(tabCloseDetail(set));
        // Cancel is the default and the escape route both. The expensive
        // mistake here is losing a session somebody was part way through, and
        // Return on a dialog nobody read should not be what does it.
        QPushButton *go = box.addButton(tr("Close Tabs"), QMessageBox::AcceptRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(QMessageBox::Cancel);
        box.setEscapeButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() != go) {
            return false;
        }
    }

    // By widget, never by index: removeTab() renumbers the strip, so the
    // second index in a list collected beforehand no longer means what it did.
    for (TerminalTab *tab : victims) {
        closeTab(tabs_->indexOf(tab));
    }
    return true;
}

void MainWindow::showTabContextMenu(const QPoint &pos) {
    QTabBar *bar = tabs_->tabBar();
    const int index = bar->tabAt(pos);
    if (index < 0) {
        // The empty part of the strip. Every item below is about one tab.
        return;
    }

    // The clicked tab, not the current one. Right-clicking a tab that is not
    // in front and getting an action against the one that is would be the
    // worst kind of correct-looking.
    bar->setCurrentIndex(index);

    QMenu menu(this);

    // Reconnect first, and only on a tab whose session has ended -- the same
    // rule the terminal's own menu applies, and the same action. It is worth
    // having in both places for opposite reasons: the terminal menu is where
    // somebody looking at the dead session right-clicks, and this one is where
    // somebody looking at a strip of tabs marked "(closed)" does, without
    // having to bring each one to the front first.
    if (auto *clicked = qobject_cast<TerminalTab *>(tabs_->widget(index))) {
        if (clicked->canReconnect()) {
            connect(menu.addAction(tr("&Reconnect")), &QAction::triggered,
                    clicked, &TerminalTab::reconnect);
            menu.addSeparator();
        }
    }

    connect(menu.addAction(tr("&Close Tab")), &QAction::triggered, this,
            [this, index] { closeTab(index); });

    QAction *others = menu.addAction(tr("Close &Other Tabs"));
    others->setEnabled(tabs_->count() > 1);
    connect(others, &QAction::triggered, this,
            [this, index] { closeTabSet(tabsExcept(index)); });

    QAction *right = menu.addAction(tr("Close Tabs to the &Right"));
    right->setEnabled(index < tabs_->count() - 1);
    connect(right, &QAction::triggered, this,
            [this, index] { closeTabSet(tabsAfter(index)); });

    menu.addSeparator();

    connect(menu.addAction(tr("Close &All Tabs")), &QAction::triggered, this,
            [this] { closeTabSet(tabsExcept(-1)); });

    menu.exec(bar->mapToGlobal(pos));

    // The menu took focus on the way in, the same way the terminal's does.
    // Without this the next keystroke goes nowhere on a tab that was in the
    // middle of a session.
    if (auto *tab = currentTab()) {
        tab->setFocus();
    }
}

TerminalTab *MainWindow::currentTab() const {
    return qobject_cast<TerminalTab *>(tabs_->currentWidget());
}

// ---------------------------------------------------------------------------
// Vault

// The chrome bar's pill is the only thing in the window that reports the
// vault's state without being asked, so every transition has to reach it.
// Cheap enough to call speculatively rather than tracking edges.
void MainWindow::refreshVaultPill() {
    if (chromeBar_) {
        chromeBar_->setVaultUnlocked(vault_ && !vault_->isLocked());
    }
}

void MainWindow::quietUnlock() {
    if (!vault_ || !vault_->isOpen() || !vault_->exists()) return;

    const omegassh::VaultError code = vault_->unlockQuiet();
    switch (code) {
        case omegassh::VaultError::Ok:
            statusBar()->showMessage(tr("Vault unlocked from the OS keyring."), 4000);
            refreshVaultPill();
            return;
        case omegassh::VaultError::NeedsPassword:
            // The ordinary answer on a machine that has never filed one. Not
            // worth a message: nothing is wrong, and the prompt arrives when
            // something actually needs a credential.
            return;
        default:
            break;
    }

    // A keyring that answered wrongly IS worth saying, once, quietly. The
    // stale case in particular: auto-unlock stopped working and the person is
    // about to be asked for a password they thought was saved.
    statusBar()->showMessage(omegassh::Vault::describe(code), 8000);
}

bool MainWindow::ensureVaultUnlocked() {
    if (!vault_ || !vault_->isOpen()) return false;
    if (!vault_->isLocked()) return true;

    VaultUnlockDialog dialog(vault_.get(), this);

    // Ask the quiet path again rather than remembering what startup found. The
    // keyring may have been unlocked since -- a Keychain prompt answered, a
    // login session that came up late -- and the dialog opens with whatever
    // the answer is now, not with a stale explanation from launch.
    dialog.setQuietResult(vault_->unlockQuiet());
    if (!vault_->isLocked()) return true;  // the retry opened it

    dialog.exec();
    return dialog.unlocked();
}

void MainWindow::openCredentialManager() {
    // Deliberately not gated on ensureVaultUnlocked(). The manager opens
    // against a locked vault, shows the state and offers the unlock itself --
    // which is where somebody looking for their credentials will go anyway.
    CredentialManagerDialog dialog(vault_.get(), this);
    dialog.exec();
}

void MainWindow::lockVault() {
    if (!vault_) return;
    vault_->lock();
    // What locking guarantees: the derived key is gone and the list is
    // released. What it does not: that secret bytes are gone from memory. Go
    // strings are immutable, so clearing a field drops a reference and leaves
    // the bytes for the collector. Locked means "cannot be read through this
    // API", not "scrubbed", and the message does not claim otherwise.
    statusBar()->showMessage(tr("Vault locked."), 4000);
    refreshVaultPill();
}

void MainWindow::changeMasterPassword() {
    if (!vault_ || vault_->isLocked()) return;

    bool ok = false;
    const QString oldMaster =
        QInputDialog::getText(this, tr("Change Master Password"),
                              tr("Current master password"), QLineEdit::Password,
                              QString(), &ok);
    if (!ok || oldMaster.isEmpty()) return;

    const QString newMaster =
        QInputDialog::getText(this, tr("Change Master Password"),
                              tr("New master password"), QLineEdit::Password,
                              QString(), &ok);
    if (!ok || newMaster.isEmpty()) return;

    const QString confirm =
        QInputDialog::getText(this, tr("Change Master Password"),
                              tr("Confirm the new password"), QLineEdit::Password,
                              QString(), &ok);
    if (!ok) return;
    if (confirm != newMaster) {
        QMessageBox::warning(this, tr("Change Master Password"),
                             tr("The two passwords do not match. Nothing changed."));
        return;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const omegassh::VaultError code = vault_->changeMaster(oldMaster, newMaster);
    QApplication::restoreOverrideCursor();

    if (code != omegassh::VaultError::Ok) {
        QMessageBox::warning(this, tr("Change Master Password"),
                             omegassh::Vault::describe(code, vault_->lastError()));
        return;
    }

    // Re-file, or auto-unlock is now silently broken: the entry still holds
    // the old master, which no longer opens anything. The C header calls this
    // out by name, and it is the sort of thing that surfaces a week later as
    // "it keeps asking me for the password now".
    omegassh::KeyringStatus status;
    vault_->keyringStatus(&status);
    if (status.available && !status.disabled && status.hasEntry) {
        const omegassh::VaultError filed = vault_->keyringSet(newMaster);
        if (filed != omegassh::VaultError::Ok) {
            QMessageBox::warning(
                this, tr("Change Master Password"),
                tr("The master password was changed, but the saved keyring "
                   "entry could not be updated — it still holds the old one, "
                   "so the vault will ask on the next launch.\n\n%1")
                    .arg(vault_->lastError()));
            return;
        }
    }
    statusBar()->showMessage(tr("Master password changed."), 4000);
}

void MainWindow::forgetKeyringEntry() {
    if (!vault_) return;
    const omegassh::VaultError code = vault_->keyringClear();
    statusBar()->showMessage(
        code == omegassh::VaultError::Ok
            ? tr("Saved master password removed from the OS keyring.")
            : omegassh::Vault::describe(code, vault_->lastError()),
        6000);
}

void MainWindow::notImplemented(const QString &what, const QString &phase) {
    statusBar()->showMessage(tr("%1 — phase %2").arg(what, phase), 4000);
}

void MainWindow::closeEvent(QCloseEvent *event) {
    AppSettings &s = settings_->settings();

    // Only when not maximized, matching nterm-qt: a maximized window's size is
    // the screen's, and storing it would lose the size to restore to.
    if (!isMaximized()) {
        s.window_width = width();
        s.window_height = height();

        // The size always, the position only when it is somewhere a person
        // could reach. Writing an unreachable x/y is what makes the stranded
        // window permanent: the next launch restores it, closes there, and
        // saves it again. Clearing them instead leaves the window manager to
        // place the window, which it does sensibly.
        //
        // The check can fire without a screen ever having been unplugged --
        // closing while dragged mostly off the edge does it -- and losing the
        // position in that case is the right trade against being unable to
        // find the window at all.
        if (windowIsGrabbable()) {
            s.window_x = x();
            s.window_y = y();
        } else {
            s.window_x.reset();
            s.window_y.reset();
        }
    }
    s.window_maximized = isMaximized();

    const QList<int> sizes = splitter_->sizes();
    if (!sizes.isEmpty() && sizes.first() > 0) {
        s.tree_width = sizes.first();
    }

    if (!settings_->save()) {
        // Worth saying out loud rather than swallowing: the next launch will
        // silently open somewhere else, which reads as the window forgetting
        // rather than as a file that could not be written.
        qWarning("omega: could not save settings: %s",
                 qPrintable(settings_->error()));
    }

    QMainWindow::closeEvent(event);
}

}  // namespace omega::app