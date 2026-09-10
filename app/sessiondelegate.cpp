// app/sessiondelegate.cpp

#include "app/sessiondelegate.h"

#include "app/linkstate.h"
#include "app/sessiontreemodel.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QTreeView>
#include <QVariantAnimation>

namespace omega::app {
namespace {

QColor q(const theme::Rgba &c) { return QColor(c.r, c.g, c.b); }

Link linkOf(const QModelIndex &i) {
    return static_cast<Link>(i.data(SessionTreeModel::LinkRole).toInt());
}

bool isFolder(const QModelIndex &i) {
    return i.data(SessionTreeModel::KindRole).toInt() ==
           static_cast<int>(SessionTreeModel::FolderItem);
}

}  // namespace

SessionDelegate::SessionDelegate(QTreeView *view, QObject *parent)
    : QStyledItemDelegate(parent), view_(view) {
    pulse_ = new QVariantAnimation(this);
    pulse_->setDuration(1400);
    pulse_->setLoopCount(-1);
    pulse_->setKeyValueAt(0.0, 0.45);
    pulse_->setKeyValueAt(0.5, 1.0);
    pulse_->setKeyValueAt(1.0, 0.45);
    pulse_->setEasingCurve(QEasingCurve::InOutSine);
    connect(pulse_, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &v) {
                pulseValue_ = v.toReal();
                if (view_) view_->viewport()->update();
            });
    pulse_->start();
}

void SessionDelegate::setTokens(const theme::Tokens &t) {
    tokens_ = t;
    if (view_) view_->viewport()->update();
}

void SessionDelegate::setBaseFontSize(int px) {
    const int clamped = qMax(8, px);
    if (baseFontPx_ == clamped) return;
    baseFontPx_ = clamped;

    // NOT viewport()->update(). sizeHint() moves with the font, so the rows
    // change height and the view has to lay them out again -- a repaint alone
    // draws the new text into the old row rectangles, which clips the detail
    // line at every size above the default.
    if (view_) {
        view_->doItemsLayout();
        view_->viewport()->update();
    }
}

// The three sizes this delegate draws, one point apart in the same order the
// stylesheet's are: the name at the body size, the folder label and the mono
// detail below it. Kept as offsets rather than ratios for the same reason the
// sheet keeps its four that way -- a ratio collapses them onto each other at
// the small end, and the difference between the name and the address is the
// only thing separating the two lines of a row.
//
// At the shipped 13 these come to 13 / 11 / 10.5, which is what was hardcoded
// here before, so the default row is unchanged to the pixel.
int SessionDelegate::namePx() const { return baseFontPx_; }
int SessionDelegate::folderPx() const { return qMax(7, baseFontPx_ - 2); }
qreal SessionDelegate::detailPx() const {
    return qMax(7.0, baseFontPx_ - 2.5);
}

QFont SessionDelegate::uiFont(int px, int weight) const {
    QFont f(QStringLiteral("IBM Plex Sans"));
    f.setStyleHint(QFont::SansSerif);
    f.setPixelSize(px);
    f.setWeight(static_cast<QFont::Weight>(weight));
    return f;
}

QFont SessionDelegate::monoFont(qreal px) const {
    QFont f(QStringLiteral("IBM Plex Mono"));
    f.setStyleHint(QFont::Monospace);
    f.setFixedPitch(true);
    // Pixel size as a real is not available, so this goes through points at
    // the conventional 96dpi ratio -- which is what the sheet's font-size in
    // px resolves to as well, so the two agree.
    f.setPointSizeF(px * 0.75);
    return f;
}

void SessionDelegate::paintChip(QPainter *p, const QRect &r,
                                const QString &text, const QColor &fg) const {
    p->setPen(Qt::NoPen);
    p->setBrush(q(tokens_.bgControl));
    p->drawRoundedRect(r, 5, 5);
    p->setPen(fg);
    p->setFont(monoFont(detailPx()));
    p->drawText(r, Qt::AlignCenter, text);
}

void SessionDelegate::paintFolder(QPainter *p, const QStyleOptionViewItem &o,
                                  const QModelIndex &i) const {
    const QRect r = o.rect;

    // The chevron. With indentation 0 the view draws no branch indicator, so
    // the folder row owns it. isExpanded lives on QTreeView; see the header.
    const bool open = view_ && view_->isExpanded(i);
    const int cx = r.left() + 14;
    const int cy = r.center().y();

    QPainterPath chevron;
    if (open) {
        chevron.moveTo(cx - 4, cy - 2);
        chevron.lineTo(cx, cy + 2);
        chevron.lineTo(cx + 4, cy - 2);
    } else {
        chevron.moveTo(cx - 2, cy - 4);
        chevron.lineTo(cx + 2, cy);
        chevron.lineTo(cx - 2, cy + 4);
    }

    QPen pen(q(tokens_.inkMuted));
    pen.setWidthF(1.4);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p->setPen(pen);
    p->setBrush(Qt::NoBrush);
    p->drawPath(chevron);

    p->setPen(q(tokens_.inkMuted));
    p->setFont(uiFont(folderPx(), QFont::Normal));
    p->drawText(QRect(r.left() + 24, r.top(), r.width() - 34, r.height()),
                Qt::AlignVCenter | Qt::AlignLeft,
                i.data(Qt::DisplayRole).toString());
}

void SessionDelegate::paint(QPainter *p, const QStyleOptionViewItem &o,
                            const QModelIndex &i) const {
    p->save();
    p->setRenderHint(QPainter::Antialiasing);

    if (isFolder(i)) {
        paintFolder(p, o, i);
        p->restore();
        return;
    }

    const QRect r = o.rect.adjusted(4, 1, -4, -1);
    const QFontMetrics ui(uiFont(namePx(), QFont::Medium));
    const QFontMetrics mono(monoFont(detailPx()));

    if (o.state & QStyle::State_Selected) {
        p->setBrush(q(tokens_.bgSelected));
        p->setPen(Qt::NoPen);
        p->drawRoundedRect(r, 7, 7);
        // A 2px accent bar at the leading edge. The pill alone reads as a
        // hover on the themes where bg.selected and bg.hover are close.
        p->setBrush(q(tokens_.accent));
        p->drawRect(QRect(r.left(), r.top() + 2, 2, r.height() - 4));
    } else if (o.state & QStyle::State_MouseOver) {
        p->setBrush(q(tokens_.bgHover));
        p->setPen(Qt::NoPen);
        p->drawRoundedRect(r, 7, 7);
    }

    const Link link = linkOf(i);

    // Opacity is set on the painter rather than baked into the colour, so the
    // dot fades against whichever row background is underneath it rather than
    // against an assumed one.
    const qreal previousOpacity = p->opacity();
    if (linkIsTransient(link)) p->setOpacity(pulseValue_);
    p->setBrush(dotColor(link, tokens_));
    p->setPen(Qt::NoPen);
    p->drawEllipse(QRect(r.left() + 10, r.center().y() - 3, 7, 7));
    p->setOpacity(previousOpacity);

    // Width is reserved for the chip only when there is one. Reserving it
    // always would elide every address in the tree to buy space for a number
    // that is usually absent.
    const int rtt = i.data(SessionTreeModel::RttRole).toInt();
    const bool hasRtt = rtt >= 0;
    const QString rttText = hasRtt ? QStringLiteral("%1ms").arg(rtt) : QString();
    const int chipWidth =
        hasRtt ? qMax(38, mono.horizontalAdvance(rttText) + 14) : 0;

    const QRect text = r.adjusted(29, 0, -(chipWidth ? chipWidth + 14 : 10), 0);

    p->setPen(q(tokens_.ink));
    p->setFont(uiFont(namePx(), QFont::Medium));
    p->drawText(QRect(text.left(), r.top() + 6, text.width(), ui.height()),
                Qt::AlignVCenter | Qt::AlignLeft,
                ui.elidedText(i.data(Qt::DisplayRole).toString(),
                              Qt::ElideRight, text.width()));

    p->setPen(linkIsWarn(link) ? q(tokens_.warn) : q(tokens_.inkDetail));
    p->setFont(monoFont(detailPx()));
    // ElideMiddle so the port survives. ElideRight gives
    // "lab-admin@10.20.4..." with the port gone, and the port is the half of
    // that string most likely to be the reason somebody is looking.
    p->drawText(QRect(text.left(), r.top() + 6 + ui.height() - 1, text.width(),
                      mono.height()),
                Qt::AlignVCenter | Qt::AlignLeft,
                mono.elidedText(i.data(SessionTreeModel::DetailRole).toString(),
                                Qt::ElideMiddle, text.width()));

    if (hasRtt) {
        // 18 at the default, and the mono metrics above it: a chip pinned at
        // 18 clips its own digits once the detail font passes about 13px.
        const int chipHeight = qMax(18, mono.height() + 6);
        paintChip(p,
                  QRect(r.right() - chipWidth - 8,
                        r.center().y() - chipHeight / 2, chipWidth, chipHeight),
                  rttText, rttColor(rtt, tokens_));
    }

    p->restore();
}

QSize SessionDelegate::sizeHint(const QStyleOptionViewItem &,
                                const QModelIndex &i) const {
    const QFontMetrics ui(uiFont(namePx(), QFont::Medium));
    const QFontMetrics mono(monoFont(detailPx()));
    if (isFolder(i)) {
        return QSize(0,
                     QFontMetrics(uiFont(folderPx(), QFont::Normal)).height() + 12);
    }
    return QSize(0, ui.height() + mono.height() + 11);
}

}  // namespace omega::app
