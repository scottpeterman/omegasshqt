// app/chromebar.h
//
// The merged title bar: wordmark, menu bar, vault pill, window buttons.
//
// Installed with QMainWindow::setMenuWidget(), which is the whole reason this
// slice is not a rewrite. QMainWindow lets an arbitrary widget stand where the
// menu bar goes, so the window keeps its central widget, its status bar and its
// dock areas, and only the strip along the top changes. The alternative --
// reparenting everything into a plain QWidget, as the preview harness does --
// throws away machinery that already works because the harness had none of it
// to throw away.
//
// CONSEQUENCE, and it is the one thing to remember when editing MainWindow:
// QMainWindow::menuBar() and setMenuWidget() are alternatives, not layers.
// Calling menuBar() after this is installed CREATES A SECOND, EMPTY MENU BAR
// and puts it nowhere. So MainWindow holds the QMenuBar this class owns and
// never calls menuBar() again. There is one call site left, in
// applyThemeByName, and it walks menuBar_ for the same reason.
//
// THE DRAG IS startSystemMove(), NOT DELTA TRACKING. Hand-rolled dragging
// works on X11 and breaks on Wayland, where a client is not told where its own
// window is, and it breaks again across a mixed-DPI boundary. The compositor
// knows both; ask it.
//
// This class is created only on the frameless path. With the window manager's
// own title bar the menu bar is installed the ordinary way and none of this
// exists -- see MainWindow::buildChrome.

#ifndef OMEGA_APP_CHROMEBAR_H
#define OMEGA_APP_CHROMEBAR_H

#include <QFrame>

#include "theme/tokens.h"

class QLabel;
class QMenuBar;

namespace omega::app {

class WindowButton;

class ChromeBar : public QFrame {
    Q_OBJECT

public:
    // `withWindowButtons` is false on the native-frame path, where the window
    // manager supplies them and a second set would be two ways to close one
    // window.
    explicit ChromeBar(bool withWindowButtons, QWidget *parent = nullptr);

    // The bar owns it; MainWindow builds its menus into it. Never null.
    QMenuBar *menuBar() const { return menus_; }

    // Maximise <-> Restore, driven by the window's state change rather than by
    // the click, so a double-click on the bar and a keyboard maximise both
    // update the glyph.
    void setMaximised(bool maximised);

    // "vault unlocked" / "vault locked", with the dot beside it. Called
    // whenever the vault's state changes; the pill is the only place in the
    // window that reports it at a glance.
    void setVaultUnlocked(bool unlocked);

    // Repaints everything that does not come from the stylesheet: the logo
    // swatch, the pill dot, and the window buttons' glyph colours.
    void setTokens(const theme::Tokens &tokens);

protected:
    // Both belong to the bar rather than to the window: pressing anywhere on
    // the window would start a system move from inside the terminal too.
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

    // Watches the menu bar only. A QMenuBar consumes a press that lands on no
    // action -- it does not call ignore() -- so the strip to the right of the
    // last menu is dead to the drag unless the press is intercepted before the
    // menu bar sees it. See the note at the installEventFilter call.
    bool eventFilter(QObject *watched, QEvent *event) override;

    // The bar's height follows the body text size, which the settings dialog
    // can now change while the window is open.
    void changeEvent(QEvent *event) override;

private:
    void repaintDot();

    // Asks the compositor to move the window. False when there is no window
    // handle yet, which is the case before the window is first shown.
    bool beginSystemMove();
    void toggleMaximised();
    void updateBarHeight();

    QMenuBar *menus_ = nullptr;
    QFrame *logo_ = nullptr;
    QFrame *pillDot_ = nullptr;
    QLabel *pillText_ = nullptr;
    WindowButton *maxButton_ = nullptr;
    bool vaultUnlocked_ = false;

    // Held so a vault transition can recolour the dot without the window
    // having to hand the theme down a second time -- the same arrangement
    // TerminalTab has with themeApplied_.
    theme::Tokens tokens_ = theme::specTokens();
};

}  // namespace omega::app

#endif  // OMEGA_APP_CHROMEBAR_H
