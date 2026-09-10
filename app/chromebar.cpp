// app/chromebar.cpp

#include "app/chromebar.h"

#include "app/windowbuttons.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMouseEvent>
#include <QWindow>

namespace omega::app {
namespace {

constexpr int kBarHeight = 40;

QString swatchStyle(const theme::Rgba &fill, int radius) {
    return QStringLiteral("background-color: %1; border-radius: %2px;")
        .arg(QString::fromStdString(theme::hex(fill)))
        .arg(radius);
}

}  // namespace

ChromeBar::ChromeBar(bool withWindowButtons, QWidget *parent) : QFrame(parent) {
    // Read by the generated stylesheet: QFrame[role="chrome"] paints the
    // surface and the bottom rule, and the descendant rule under it puts the
    // menu bar on a transparent background so it sits ON the bar rather than
    // in a strip of its own.
    setProperty("role", "chrome");
    setFixedHeight(kBarHeight);

    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(10, 0, 10, 0);
    lay->setSpacing(4);

    logo_ = new QFrame(this);
    logo_->setFixedSize(15, 15);
    // Decoration, not a control. Without this the swatch and the wordmark are
    // two more dead spots in the drag strip -- QFrame consumes a press the
    // same way QMenuBar does.
    logo_->setAttribute(Qt::WA_TransparentForMouseEvents);
    lay->addWidget(logo_);

    auto *word = new QLabel(QStringLiteral("Omega"), this);
    word->setProperty("role", "wordmark");
    word->setContentsMargins(8, 0, 10, 0);
    word->setAttribute(Qt::WA_TransparentForMouseEvents);
    lay->addWidget(word);

    menus_ = new QMenuBar(this);
    // Required, and not only for macOS. A QMenuBar that is allowed to go
    // native hands its items to the system menu and leaves an empty strip
    // here, which on the frameless path is an empty strip with no menus
    // anywhere the user can reach on Linux.
    menus_->setNativeMenuBar(false);

    // QMenuBar constructs itself MinimumExpanding horizontally. With the
    // addStretch() below, the leftover width in this bar is therefore SPLIT
    // between the menu bar and the stretch -- so the menu bar runs to roughly
    // mid-window, hundreds of pixels past the last menu, and every one of
    // those pixels is a dead spot. QMenuBar::mousePressEvent does not call
    // ignore() when the press lands on no action, so the event is consumed
    // where it sits and never reaches this frame's mousePressEvent.
    //
    // Maximum caps it at sizeHint(): the width of the items and nothing more.
    menus_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);

    // And the belt to that pair of braces. Sizing alone leaves the few pixels
    // of trailing slack a QMenuBar keeps for itself, plus whatever a different
    // Qt version's sizeHint decides to include; the filter makes any press
    // that is not ON a menu item a drag, whatever the geometry turns out to
    // be.
    menus_->installEventFilter(this);
    lay->addWidget(menus_);

    lay->addStretch();

    auto *pill = new QFrame(this);
    pill->setProperty("role", "pill");
    // Read-only status, not a button. Transparent on the pill AND on its two
    // children: the attribute is per-widget, so setting it on the parent alone
    // still leaves the dot and the text consuming presses.
    pill->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto *pillLay = new QHBoxLayout(pill);
    pillLay->setContentsMargins(11, 3, 11, 3);
    pillLay->setSpacing(6);

    pillDot_ = new QFrame(pill);
    pillDot_->setFixedSize(7, 7);
    pillDot_->setAttribute(Qt::WA_TransparentForMouseEvents);
    pillLay->addWidget(pillDot_);

    pillText_ = new QLabel(tr("vault locked"), pill);
    pillText_->setProperty("role", "corner");
    pillText_->setAttribute(Qt::WA_TransparentForMouseEvents);
    pillLay->addWidget(pillText_);
    lay->addWidget(pill);

    if (withWindowButtons) {
        auto *buttons = new QWidget(this);
        // bare="true" is not decoration. The sheet's blanket QWidget rule
        // paints bg.base behind every widget, so a plain container parented
        // onto the bg.chrome bar shows as a lighter rectangle behind the three
        // buttons. Any layout-only container on a non-bg.base frame needs
        // this.
        buttons->setProperty("bare", true);
        auto *bLay = new QHBoxLayout(buttons);
        bLay->setContentsMargins(10, 0, 0, 0);
        bLay->setSpacing(2);

        auto *minimise = new WindowButton(WindowButton::Minimise, buttons);
        connect(minimise, &QAbstractButton::clicked, this,
                [this] { window()->showMinimized(); });
        bLay->addWidget(minimise);

        maxButton_ = new WindowButton(
            window() && window()->isMaximized() ? WindowButton::Restore
                                                : WindowButton::Maximise,
            buttons);
        connect(maxButton_, &QAbstractButton::clicked, this, [this] {
            QWidget *w = window();
            if (w->isMaximized()) {
                w->showNormal();
            } else {
                w->showMaximized();
            }
        });
        bLay->addWidget(maxButton_);

        auto *closeButton = new WindowButton(WindowButton::Close, buttons);
        // close(), not qApp->quit(): the window's closeEvent is what saves
        // geometry and asks about live sessions.
        connect(closeButton, &QAbstractButton::clicked, this,
                [this] { window()->close(); });
        bLay->addWidget(closeButton);

        lay->addWidget(buttons);
    }

    updateBarHeight();
}

void ChromeBar::updateBarHeight() {
    // kBarHeight is right at the shipped 13px body text and wrong above it:
    // the menus, the wordmark and the pill all grow with the font and a strip
    // pinned at 40 clips them. Taking the height from the layout rather than
    // from the font metrics means the pill's own padding is counted too.
    const int needed = layout() ? layout()->sizeHint().height() : 0;
    setFixedHeight(qMax(kBarHeight, needed));
}

void ChromeBar::changeEvent(QEvent *event) {
    QFrame::changeEvent(event);
    // FontChange arrives when QApplication::setFont lands; StyleChange when the
    // stylesheet is replaced, which is where the sheet's own font-size comes
    // from. Both move the height this bar needs.
    if (event->type() == QEvent::FontChange ||
        event->type() == QEvent::StyleChange) {
        updateBarHeight();
    }
}

void ChromeBar::setMaximised(bool maximised) {
    if (maxButton_) {
        maxButton_->setGlyph(maximised ? WindowButton::Restore
                                       : WindowButton::Maximise);
    }
}

void ChromeBar::setVaultUnlocked(bool unlocked) {
    if (vaultUnlocked_ == unlocked && pillDot_->styleSheet().size()) return;
    vaultUnlocked_ = unlocked;
    pillText_->setText(unlocked ? tr("vault unlocked") : tr("vault locked"));
    repaintDot();
}

void ChromeBar::repaintDot() {
    // The dot is the state, so it is re-set from the cached tokens on both a
    // theme change and a vault transition. Holding the tokens is what lets the
    // two arrive in either order.
    pillDot_->setStyleSheet(
        swatchStyle(vaultUnlocked_ ? tokens_.state : tokens_.dead, 3));
}

void ChromeBar::setTokens(const theme::Tokens &t) {
    tokens_ = t;

    // Two swatches the stylesheet cannot reach, because their colour is a
    // value rather than a role. An inline sheet is right here precisely
    // because this function is what re-runs on a theme change -- the objection
    // to inline sheets is that they freeze at construction, and these do not.
    logo_->setStyleSheet(swatchStyle(t.accent, 4));
    repaintDot();

    const QList<WindowButton *> buttons = findChildren<WindowButton *>();
    for (WindowButton *b : buttons) b->setTokens(t);
}

bool ChromeBar::beginSystemMove() {
    // Never hand-roll the delta. startSystemMove is the only form that works
    // under Wayland and across a mixed-DPI boundary; see the header.
    if (QWindow *handle = window()->windowHandle()) {
        handle->startSystemMove();
        return true;
    }
    return false;
}

void ChromeBar::toggleMaximised() {
    QWidget *w = window();
    if (w->isMaximized()) {
        w->showNormal();
    } else {
        w->showMaximized();
    }
}

bool ChromeBar::eventFilter(QObject *watched, QEvent *event) {
    if (watched != menus_) {
        return QFrame::eventFilter(watched, event);
    }

    const QEvent::Type type = event->type();
    if (type != QEvent::MouseButtonPress &&
        type != QEvent::MouseButtonDblClick) {
        return QFrame::eventFilter(watched, event);
    }

    auto *me = static_cast<QMouseEvent *>(event);
    if (me->button() != Qt::LeftButton) {
        return QFrame::eventFilter(watched, event);
    }

    // ON a menu item, the menu bar keeps the event: opening File must not also
    // start dragging the window. actionAt() answers exactly that question and
    // it is public API, so this does not depend on the menu bar's geometry
    // matching its size hint.
    if (menus_->actionAt(me->position().toPoint())) {
        return QFrame::eventFilter(watched, event);
    }

    if (type == QEvent::MouseButtonDblClick) {
        toggleMaximised();
        return true;
    }
    return beginSystemMove();
}

void ChromeBar::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && beginSystemMove()) {
        event->accept();
        return;
    }
    QFrame::mousePressEvent(event);
}

void ChromeBar::mouseDoubleClickEvent(QMouseEvent *event) {
    toggleMaximised();
    event->accept();
}

}  // namespace omega::app
