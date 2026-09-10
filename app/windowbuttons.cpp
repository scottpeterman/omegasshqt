// app/windowbuttons.cpp

#include "app/windowbuttons.h"

#include <QPainter>
#include <QPen>

namespace omega::app {
namespace {

QColor toQColor(const theme::Rgba &c) { return QColor(c.r, c.g, c.b, c.a); }

}  // namespace

WindowButton::WindowButton(Glyph glyph, QWidget *parent)
    : QAbstractButton(parent), glyph_(glyph) {
    setFixedSize(30, 24);
    // The arrow, not the pointing hand: these are window furniture, and every
    // platform's own title bar uses the arrow over them.
    setCursor(Qt::ArrowCursor);
    // Refused on purpose. Tab moving focus into the close button, from a
    // terminal, is a way to lose a session to a space bar.
    setFocusPolicy(Qt::NoFocus);

    switch (glyph) {
        case Minimise: setAccessibleName(tr("Minimise")); break;
        case Maximise: setAccessibleName(tr("Maximise")); break;
        case Restore:  setAccessibleName(tr("Restore")); break;
        case Close:    setAccessibleName(tr("Close")); break;
    }
}

void WindowButton::setGlyph(Glyph glyph) {
    if (glyph_ == glyph) return;
    glyph_ = glyph;
    setAccessibleName(glyph == Restore ? tr("Restore") : tr("Maximise"));
    update();
}

void WindowButton::setTokens(const theme::Tokens &t) {
    ink_ = toQColor(t.inkMuted);
    inkHover_ = toQColor(t.ink);
    hoverBg_ = toQColor(t.bgSelected);

    // Derived from the theme's own red rather than fixed at #8c3b3b, so it
    // stays in the family on borland or gruvbox instead of being the one
    // colour in the window that belongs to no theme. Mixed most of the way
    // toward the chrome surface, because a full-strength ANSI red under a
    // 30x24 button reads as an error state rather than as a hover.
    closeBg_ = toQColor(theme::mix(t.bgChrome, t.danger, 0.62));
    update();
}

void WindowButton::enterEvent(QEnterEvent *event) {
    hover_ = true;
    update();
    QAbstractButton::enterEvent(event);
}

void WindowButton::leaveEvent(QEvent *event) {
    hover_ = false;
    update();
    QAbstractButton::leaveEvent(event);
}

void WindowButton::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    if (hover_) {
        p.setPen(Qt::NoPen);
        // Close gets the red wash. That is the one platform convention worth
        // keeping here: it is the only one of the three that loses work.
        p.setBrush(glyph_ == Close ? closeBg_ : hoverBg_);
        p.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 6, 6);
    }

    QPen pen(hover_ ? inkHover_ : ink_);
    pen.setWidthF(1.2);
    pen.setCapStyle(Qt::FlatCap);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    // A 10px glyph box on a half-pixel origin, so a 1.2px stroke lands on a
    // whole device pixel at 100% and scales without blurring at 125 and 200.
    const qreal side = 10.0;
    const QRectF g(qRound((width() - side) / 2.0) + 0.5,
                   qRound((height() - side) / 2.0) + 0.5, side, side);

    switch (glyph_) {
        case Minimise:
            p.drawLine(QPointF(g.left(), g.center().y()),
                       QPointF(g.right(), g.center().y()));
            break;

        case Maximise:
            p.drawRect(g.adjusted(0.5, 0.5, -0.5, -0.5));
            break;

        case Restore: {
            const QRectF front = g.adjusted(0, 2.5, -2.5, 0);
            p.drawRect(front);
            // The back rectangle is two strokes rather than a whole rect, so
            // its lower-left corner does not show through the front one.
            p.drawLine(QPointF(g.left() + 2.5, g.top() + 0.5),
                       QPointF(g.right() - 0.5, g.top() + 0.5));
            p.drawLine(QPointF(g.right() - 0.5, g.top() + 0.5),
                       QPointF(g.right() - 0.5, g.bottom() - 2.5));
            break;
        }

        case Close:
            pen.setCapStyle(Qt::RoundCap);
            p.setPen(pen);
            p.drawLine(g.topLeft(), g.bottomRight());
            p.drawLine(g.topRight(), g.bottomLeft());
            break;
    }
}

}  // namespace omega::app
