// app/sizegrip.cpp

#include "app/sizegrip.h"

#include <QPainter>
#include <QPen>

namespace omega::app {

ThemedSizeGrip::ThemedSizeGrip(QWidget *parent) : QSizeGrip(parent) {
    setFixedSize(16, 16);
    setToolTip(tr("Drag to resize"));
}

void ThemedSizeGrip::setTokens(const theme::Tokens &t) {
    idle_ = QColor(t.dead.r, t.dead.g, t.dead.b);
    active_ = QColor(t.inkMuted.r, t.inkMuted.g, t.inkMuted.b);
    update();
}

void ThemedSizeGrip::enterEvent(QEnterEvent *event) {
    hover_ = true;
    update();
    QSizeGrip::enterEvent(event);
}

void ThemedSizeGrip::leaveEvent(QEvent *event) {
    hover_ = false;
    update();
    QSizeGrip::leaveEvent(event);
}

void ThemedSizeGrip::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    QPen pen(hover_ ? active_ : idle_);
    pen.setWidthF(1.4);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);

    // Three diagonals of increasing length in the bottom-right corner, laid
    // out from the widget's own rect rather than from constants, so it grows
    // with the status bar instead of sitting at a fixed 16px on a 200%
    // display.
    const qreal right = width() - 3.5;
    const qreal bottom = height() - 3.5;
    const qreal step = width() / 4.0;
    for (int i = 0; i < 3; ++i) {
        const qreal off = i * step;
        p.drawLine(QPointF(right - off, bottom), QPointF(right, bottom - off));
    }
}

}  // namespace omega::app
