// app/modalframe.cpp

#include "app/modalframe.h"

#include <QDialog>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QStyle>
#include <QStyleOption>
#include <QPushButton>
#include <QScreen>
#include <QSizePolicy>
#include <QScrollBar>
#include <QScrollArea>
#include <QFormLayout>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWindow>

#include "app/currenttokens.h"
#include "app/dialogbuttons.h"
#include "app/windowbuttons.h"

namespace omega::app {
namespace {

constexpr int kStripHeight = 40;
constexpr int kFooterHeight = 56;
constexpr int kBodyMargin = 18;
constexpr int kBodySpacing = 16;

// True when the window this dialog belongs to has had its title bar taken
// off. Reading the flag rather than the setting means this cannot disagree
// with what MainWindow actually did -- including the --native-frame override,
// which the setting does not know about.
bool parentWindowIsFrameless(const QDialog *dialog) {
    const QWidget *parent = dialog->parentWidget();
    if (!parent) return false;
    const QWidget *window = parent->window();
    return window && window->windowFlags().testFlag(Qt::FramelessWindowHint);
}

// The strip is a drag handle, so anything sitting on it that is not a control
// has to let presses through -- a QLabel consumes them the same way QMenuBar
// does, and a title you cannot drag by is the one part of the bar a user will
// actually aim at.
void makePassThrough(QWidget *w) {
    w->setAttribute(Qt::WA_TransparentForMouseEvents);
}

// The footer hint, which ELIDES rather than clipping.
//
// A QLabel given less width than its size hint draws as much as fits and cuts
// the rest off mid-glyph -- "Return rejects. Click Accept to contin". That is
// what happens as soon as the buttons are wide, which at ui_font_size 16 and
// up is every time. Word wrap is not the answer either: a wrapped label in a
// QHBoxLayout reports its UNWRAPPED width as its size hint, so it takes the
// space away from the buttons before it ever wraps.
//
// THE ELISION HAPPENS AT PAINT TIME, and text() is never touched. The obvious
// version -- recompute the elided string in resizeEvent and setText() it --
// does not work, and fails in a way worth recording. setText() calls
// updateGeometry(), which invalidates the layout from inside the layout's own
// resize pass; Qt will not re-enter, so the last relayout never runs and the
// label keeps a string elided for some earlier, narrower width. Measured: the
// label reporting sizeHint=240 and actual=240 and still drawing an ellipsis.
// It also makes sizeHint() self-referential, since QLabel computes it from
// whatever shortened text is currently set.
//
// Painting instead leaves text() as the full string, so QLabel::sizeHint() is
// honest for free and ModalFrame::fitToContent can ask the footer how wide it
// wants to be. The hint is optional information -- a keyboard note, never the
// only place something is said -- so losing its tail to an ellipsis when the
// screen genuinely cannot hold it is the right degradation. No Q_OBJECT: it
// adds no signals, so it needs no moc pass.
class HintLabel : public QLabel {
public:
    using QLabel::QLabel;

    void setHint(const QString &text) {
        setText(text);
        setVisible(!text.isEmpty());
    }

    // Zero, so a long hint yields to the buttons rather than pushing them off
    // the edge. The buttons are the thing that must never be squeezed;
    // sizeHint() is what asks for the room, minimumSizeHint() is what would
    // demand it.
    QSize minimumSizeHint() const override {
        QSize s = QLabel::minimumSizeHint();
        s.setWidth(0);
        return s;
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);

        // Lets the stylesheet paint whatever background and border it has for
        // this widget before the text goes on top. Without it a QSS rule on
        // the hint would simply not render, since this bypasses QLabel's own
        // paint entirely.
        QStyleOption opt;
        opt.initFrom(this);
        style()->drawPrimitive(QStyle::PE_Widget, &opt, &painter, this);

        const QRect r = contentsRect();
        const QString shown =
            fontMetrics().elidedText(text(), Qt::ElideRight, r.width());
        painter.setPen(opt.palette.color(foregroundRole()));
        painter.drawText(r, static_cast<int>(alignment()), shown);
    }

    void resizeEvent(QResizeEvent *event) override {
        QLabel::resizeEvent(event);
        // Safe here precisely because it does not touch text() or geometry.
        setToolTip(fontMetrics().horizontalAdvance(text()) >
                           contentsRect().width()
                       ? text()
                       : QString());
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// construction
// ---------------------------------------------------------------------------

ModalFrame::ModalFrame(QDialog *dialog, const QString &title,
                       int preferredWidth)
    : QObject(dialog), dialog_(dialog), preferredWidth_(preferredWidth) {
    Q_ASSERT(dialog_);
    if (dialog_->layout()) {
        qWarning("ModalFrame: %s already has a layout; the frame will not "
                 "install. Construct the frame first.",
                 dialog_->metaObject()->className());
        return;
    }

    dialog_->setWindowTitle(title);
    dialog_->setModal(true);

    frameless_ = parentWindowIsFrameless(dialog_);
    if (frameless_) {
        // Dialog rather than FramelessWindowHint alone: without the type the
        // window manager may give a frameless top-level a taskbar entry and
        // no transient-for relationship, so it does not stay above the window
        // it is modal to.
        dialog_->setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);

        // WHAT DRAWS THE EDGE. With no title bar there is nothing between the
        // dialog and the window behind it, and both paint bg.base -- so the
        // body's left, right and bottom edges dissolve and the modal reads as
        // a floating title strip with some fields under it. The strip and the
        // footer survive only because they carry their own surface colours.
        //
        // A property rather than a hard-coded border here, because what the
        // edge should look like is the sheet's business: see
        // QDialog[chromeless="true"] in theme/tokenstylesheet.cpp. Set on the
        // FRAMELESS PATH ONLY -- where the window manager draws a real frame,
        // a second one inside it is just a line.
        dialog_->setProperty("chromeless", true);
    }

    auto *root = new QVBoxLayout(dialog_);
    // ONE PIXEL, AND ONLY WHERE THERE IS A BORDER TO SHOW. The sheet's
    // chromeless rule draws the edge on the dialog itself, but the title
    // strip, the scroll area and the footer all paint opaque surfaces and
    // the layout hands them the full rect -- so with zero margins the border
    // is drawn and then covered on all four sides, which looks exactly like
    // no border at all. Reserving the line's width is what lets it show.
    const int edge = frameless_ ? 1 : 0;
    root->setContentsMargins(edge, edge, edge, edge);
    root->setSpacing(0);

    if (frameless_) {
        buildTitleStrip(title);
        root->addWidget(titleStrip_);
    }

    // --- body -------------------------------------------------------------
    bodyWidget_ = new QWidget(dialog_);
    body_ = new QVBoxLayout(bodyWidget_);
    body_->setContentsMargins(kBodyMargin, kBodyMargin, kBodyMargin,
                              kBodyMargin);
    body_->setSpacing(kBodySpacing);

    scroll_ = new QScrollArea(dialog_);
    scroll_->setWidget(bodyWidget_);
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    // Never horizontally. A modal is sized to its content by fitToContent();
    // a horizontal scrollbar means the width calculation was wrong, and
    // hiding it makes that visible as clipping rather than papering over it.
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // The viewport is a plain QWidget on a QScrollArea whose own rule paints
    // bg.base -- without this the blanket QWidget rule paints it a second
    // time and any role frame behind it shows through wrong. See the sheet's
    // note on `bare`.
    scroll_->viewport()->setProperty("bare", true);
    root->addWidget(scroll_, 1);

    // --- footer -----------------------------------------------------------
    footer_ = new QFrame(dialog_);
    footer_->setProperty("role", "bar");
    footerLayout_ = new QHBoxLayout(footer_);
    footerLayout_->setContentsMargins(kBodyMargin, 10, kBodyMargin, 10);
    footerLayout_->setSpacing(9);

    // One short line. Not word-wrapped on purpose: with wrapping on and no
    // stretch the label's size hint is its full unwrapped width anyway, and
    // with stretch it takes slack away from the buttons. The hint is a
    // keyboard note -- if it needs two lines it belongs in the body.
    hint_ = new HintLabel(footer_);
    hint_->setProperty("role", "desc");
    hint_->setWordWrap(false);
    hint_->hide();
    footerLayout_->addWidget(hint_, 0);
    // The stretch sits between the hint and the buttons, so the buttons stay
    // right-aligned whether or not there is a hint at all.
    footerLayout_->addStretch(1);

    root->addWidget(footer_);

    dialog_->setMinimumWidth(preferredWidth_);
    refreshTokens();
    dialog_->installEventFilter(this);
}

void ModalFrame::buildTitleStrip(const QString &title) {
    titleStrip_ = new QFrame(dialog_);
    titleStrip_->setProperty("role", "chrome");

    auto *lay = new QHBoxLayout(titleStrip_);
    lay->setContentsMargins(14, 0, 8, 0);
    lay->setSpacing(4);

    auto *label = new QLabel(title, titleStrip_);
    label->setProperty("role", "wordmark");
    makePassThrough(label);
    lay->addWidget(label);
    lay->addStretch();

    // Close only. A modal has nothing to minimise to and nothing worth
    // maximising -- and a maximised modal over a window it is modal to is a
    // shape no part of this design has an answer for.
    close_ = new WindowButton(WindowButton::Close, titleStrip_);
    // reject(), not close(): the two are the same for a modal today, and
    // saying which one this is means a dialog that later overrides reject()
    // to confirm an abandoned edit gets its confirmation.
    connect(close_, &QAbstractButton::clicked, dialog_, &QDialog::reject);
    lay->addWidget(close_);

    // Same reasoning as ChromeBar::updateBarHeight: the strip has to grow with
    // the body text size, and taking it from the layout counts the button's
    // own metrics rather than assuming them.
    titleStrip_->setFixedHeight(qMax(kStripHeight, lay->sizeHint().height()));
}

// ---------------------------------------------------------------------------
// content
// ---------------------------------------------------------------------------

void ModalFrame::setFooterHint(const QString &text) {
    static_cast<HintLabel *>(hint_)->setHint(text);
}

QPushButton *ModalFrame::addButton(const QString &text, ButtonKind kind) {
    auto *button = new QPushButton(text, footer_);
    button->setProperty("primary", kind == Primary);
    footerLayout_->addWidget(button);

    if (kind == Primary) {
        if (primary_) {
            qWarning("ModalFrame: second Primary button (\"%s\") on %s. "
                     "Exactly one button is the accent one and the one Return "
                     "activates; the last wins.",
                     qPrintable(text), dialog_->metaObject()->className());
            primary_->setProperty("primary", false);
        }
        primary_ = button;
    }

    // Re-run over EVERY footer button on every add, not just the new one.
    // setReturnActivates clears autoDefault across the whole set and then
    // sets it on one -- a button added afterwards would otherwise come in
    // with Qt's default autoDefault on and answer Return itself, which is the
    // exact trap dialogbuttons.h exists for.
    const QList<QPushButton *> buttons =
        footer_->findChildren<QPushButton *>(QString(),
                                             Qt::FindDirectChildrenOnly);
    setReturnActivates(buttons, primary_);

    return button;
}

QFrame *ModalFrame::addNotice(const QString &text) {
    // "noticebox" and not "notice". QLabel derives from QFrame, so a rule
    // written QFrame[role="notice"] matches the LABEL inside this frame too,
    // and the label draws a second border inside the first. The label rule
    // that follows it in the sheet can override the colour and still leave
    // the box, and whether it wins at all comes down to rule ordering, which
    // is not a thing to stake a layout on. Two roles that cannot collide is
    // the fix; one of them winning is not.
    auto *frame = new QFrame(bodyWidget_);
    frame->setProperty("role", "noticebox");

    auto *lay = new QHBoxLayout(frame);
    lay->setContentsMargins(13, 11, 13, 11);

    auto *label = new QLabel(text, frame);
    label->setProperty("role", "notice");
    label->setWordWrap(true);
    lay->addWidget(label);

    body_->addWidget(frame);
    return frame;
}

QFrame *ModalFrame::addWell() {
    auto *frame = new QFrame(bodyWidget_);
    frame->setProperty("role", "well");
    body_->addWidget(frame);
    return frame;
}

// ---------------------------------------------------------------------------
// sizing
// ---------------------------------------------------------------------------

void ModalFrame::fitToContent() {
    QLayout *layout = bodyWidget_->layout();
    if (!layout) return;

    // WIDTH FIRST, always. Every wrapped label in the body reports a height
    // that depends on the column it is given, so measuring height against the
    // current width and then changing the width answers the wrong question.
    //
    // The floor is whatever the dialog's own chrome needs at the CURRENT font.
    // The footer's hint elides rather than clipping, which is the right
    // behaviour when a screen genuinely cannot hold it -- but eliding while
    // the dialog could simply have been wider is the layout giving up early,
    // and at ui_font_size 16 and up that is every time. HintLabel::sizeHint()
    // reports the unelided width for exactly this: so the footer can ask.
    int wantW = qMax(preferredWidth_, dialog_->minimumWidth());
    wantW = qMax(wantW, footer_->sizeHint().width());
    if (titleStrip_) wantW = qMax(wantW, titleStrip_->sizeHint().width());

    // AND THE BODY. Horizontal scrolling is off, so a body wider than the
    // dialog does not scroll -- it is silently cut off at the right edge,
    // which is how a checkbox with a long unwrappable label takes the last
    // third of its own text off screen and takes the fields beside it too.
    //
    // minimumSizeHint and not sizeHint: a word-wrapped label's sizeHint is
    // its full unwrapped width, so asking for that would make every dialog as
    // wide as its longest paragraph. The minimum is the width below which
    // content is actually destroyed, which is the floor wanted here -- a
    // wrapped label contributes almost nothing to it, and an unwrappable
    // checkbox contributes all of itself.
    wantW = qMax(wantW, bodyWidget_->minimumSizeHint().width() +
                            2 * scroll_->frameWidth());

    // Capped against the screen the dialog is ON, which is only knowable once
    // it has a window handle -- see the note on the Show event. Before that,
    // screen() answers with the primary screen and a laptop docked to a
    // taller monitor gets both answers wrong.
    int capW = 1200;
    int capH = 760;
    if (QScreen *s = dialog_->screen()) {
        capW = s->availableGeometry().width() - 96;
        capH = s->availableGeometry().height() - 96;
    }
    wantW = qBound(320, wantW, qMax(320, capW));

    // setWidgetResizable(true) squeezes the body to its MINIMUM size hint
    // before it will show a scrollbar, and a word-wrapped label's minimum is
    // far below the height it actually needs -- so an over-tall dialog goes
    // cramped and overlapping first and only scrolls afterwards. Pinning the
    // minimum to what the layout needs at this width is what makes the
    // scrollbar appear instead of the squeeze.
    const int viewportW = wantW - 2 * scroll_->frameWidth();
    const int need = layout->hasHeightForWidth()
                         ? layout->totalHeightForWidth(viewportW)
                         : layout->totalSizeHint().height();
    bodyWidget_->setMinimumHeight(need);

    const int chrome = titleStrip_ ? titleStrip_->height() : 0;
    const int foot = footer_->sizeHint().height();

    dialog_->resize(wantW,
                    qBound(200, chrome + foot + need, qMax(200, capH)));
}

// ---------------------------------------------------------------------------
// theme and drag
// ---------------------------------------------------------------------------

void ModalFrame::refreshTokens() {
    if (close_) close_->setTokens(currentTokens());
}

bool ModalFrame::beginSystemMove() {
    // Never a hand-rolled delta: startSystemMove is the only form that works
    // under Wayland and across a mixed-DPI boundary. Same reasoning as
    // ChromeBar::beginSystemMove, and the same call.
    if (QWindow *handle = dialog_->windowHandle()) {
        handle->startSystemMove();
        return true;
    }
    return false;
}

bool ModalFrame::eventFilter(QObject *watched, QEvent *event) {
    if (watched != dialog_) return QObject::eventFilter(watched, event);

    switch (event->type()) {
        case QEvent::Show:
            if (!firstShowDone_) {
                firstShowDone_ = true;
                // The window handle exists by now -- QWidget::show() creates
                // it before delivering this -- so screen() is the real one and
                // fitToContent's cap means something.
                fitToContent();
            }
            break;

        case QEvent::StyleChange:
            refreshTokens();
            // AND RE-FIT. Tokens alone is not enough, and this is measured
            // rather than defensive: the generated sheet bakes font-size into
            // every selector, so a restyle that carries a different
            // ui_font_size changes the metrics of every widget under an open
            // dialog. The first exercise of this branch -- settings previewing
            // a theme, which is the only thing in the application that reaches
            // it -- went from 13px to 18px and the dialog stayed 600x408: the
            // footer hint elided to "previews as y..." and a scrollbar
            // appeared, on a dialog that could simply have grown.
            //
            // Safe to resize unconditionally because a modal has no resize
            // edges (see the header): there is no user-chosen size here to
            // overrule. Only after the first show, though -- before that there
            // is no window handle, so fitToContent's screen cap would measure
            // the primary screen rather than the one the dialog is on, and the
            // Show case below is about to run anyway.
            if (firstShowDone_) fitToContent();
            break;

        case QEvent::MouseButtonPress: {
            // The drag, for the frameless path. It lives here rather than on
            // the strip because the strip's children have to be transparent
            // to mouse events for the drag to work at all, and once they are,
            // the press arrives at the dialog anyway.
            if (!frameless_ || !titleStrip_) break;
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() != Qt::LeftButton) break;
            if (!titleStrip_->geometry().contains(me->position().toPoint()))
                break;
            if (beginSystemMove()) return true;
            break;
        }

        default:
            break;
    }
    return QObject::eventFilter(watched, event);
}

// ---------------------------------------------------------------------------
// label roles
// ---------------------------------------------------------------------------

QLabel *fieldLabel(const QString &text, QWidget *parent) {
    auto *label = new QLabel(text, parent);
    label->setProperty("role", "fieldlabel");
    return label;
}

QLabel *descLabel(const QString &text, QWidget *parent) {
    auto *label = new QLabel(text, parent);
    label->setProperty("role", "desc");
    label->setWordWrap(true);
    return label;
}

void repolish(QWidget *widget) {
    if (!widget || !widget->style()) return;
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

QLabel *chipLabel(const QString &text, QWidget *parent, const char *tone) {
    auto *label = new QLabel(text, parent);
    label->setProperty("chip", QString::fromLatin1(tone));
    // A chip is the size of its text and no larger. QLabel's default policy
    // lets it grow, and a chip has a background -- so dropped into a
    // QFormLayout field column, or any layout with slack, it stretches into a
    // filled bar the width of the dialog. It stops reading as a badge at that
    // point and starts reading as a status banner.
    label->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    return label;
}

void alignFieldLabels(QTabWidget *tabs) {
    if (!tabs) return;

    // THE FORM'S LABEL COLUMN, NOT EVERY LABEL WEARING THE ROLE. Finding them
    // by property looked equivalent and is not: role="fieldlabel" is also the
    // right role for a standalone caption sitting ABOVE its field, which is
    // what a label does when its text is a sentence too long for a column.
    // Settings has one -- "When a session names no credential" -- deliberately
    // lifted out of the label column for exactly that reason, and a
    // property-based sweep pulled it back into the measurement anyway. Every
    // other page then inherited a column as wide as that sentence: at
    // ui_font_size 18 the fields sat 175px further right than they needed to,
    // with nothing in the gap.
    //
    // Walking QFormLayout::LabelRole asks the question that was actually
    // meant -- which labels share a column -- and a caption that is not in one
    // is correctly ignored.
    QList<QLabel *> labels;
    int widest = 0;
    for (int i = 0; i < tabs->count(); ++i) {
        QWidget *page = tabs->widget(i);
        if (!page) continue;
        // Recursive, and it has to be: a form added with addLayout() is a
        // child of the enclosing LAYOUT, not of the page widget.
        for (QFormLayout *form : page->findChildren<QFormLayout *>()) {
            for (int row = 0; row < form->rowCount(); ++row) {
                QLayoutItem *item = form->itemAt(row, QFormLayout::LabelRole);
                if (!item) continue;
                auto *label = qobject_cast<QLabel *>(item->widget());
                if (!label) continue;
                labels << label;
                widest = qMax(widest, label->sizeHint().width());
            }
        }
    }
    if (widest <= 0) return;

    for (QLabel *label : labels) {
        label->setMinimumWidth(widest);
        label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    }
}

QLabel *monoLabel(const QString &text, QWidget *parent, const char *tone,
                  bool readout) {
    auto *label = new QLabel(text, parent);
    label->setProperty("mono", true);
    label->setProperty("tone", QString::fromLatin1(tone));
    if (readout) label->setProperty("role", "readout");
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

}  // namespace omega::app