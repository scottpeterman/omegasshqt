// app/spinarrowstyle.cpp

#include "app/spinarrowstyle.h"

#include <QPainter>
#include <QPainterPath>
#include <QStyleOption>

#include "app/currenttokens.h"
#include "theme/tokens.h"

namespace omega::app {
namespace {

// Inset in the PAINTER rather than in the sheet. A margin on the subcontrol
// counts as a box and stops the delegation, so every pixel of positioning that
// is not the button's width has to happen here. The numbers pull the arrow off
// the field's rounded right border and off its top and bottom edges; they are
// what the offscreen grabs settled on at ui_font_size 13 through 20.
constexpr int kRightInset = 4;
constexpr int kEdgeInset = 2;

// A wide, shallow triangle. Tall enough to read at 13px chrome, shallow enough
// that two of them stack inside one field without touching.
constexpr int kMaxArrowWidth = 11;
constexpr int kMinArrowWidth = 7;

QColor arrowInk(const QStyleOption *option) {
    // ink.dim rather than ink: the arrows are an affordance, not content, and
    // at full ink they pull the eye off the value. Disabled falls to
    // ink.disabled so a greyed spin box does not advertise buttons that do
    // nothing.
    const theme::Tokens &tokens = currentTokens();
    const bool enabled = option && (option->state & QStyle::State_Enabled);
    const std::string hex = enabled ? theme::hex(tokens.inkDim)
                                    : theme::hex(tokens.inkFaint);
    return QColor(QString::fromStdString(hex));
}

}  // namespace

SpinArrowStyle::SpinArrowStyle(QStyle *base) : QProxyStyle(base) {}

void SpinArrowStyle::drawPrimitive(PrimitiveElement element,
                                   const QStyleOption *option,
                                   QPainter *painter,
                                   const QWidget *widget) const {
    if (element != PE_IndicatorSpinUp && element != PE_IndicatorSpinDown) {
        QProxyStyle::drawPrimitive(element, option, painter, widget);
        return;
    }
    if (!option || !painter) return;

    const bool up = element == PE_IndicatorSpinUp;

    QRect r = option->rect.adjusted(0, 0, -kRightInset, 0);
    if (up) {
        r.adjust(0, kEdgeInset, 0, 0);
    } else {
        r.adjust(0, 0, 0, -kEdgeInset);
    }

    // A rect too small to hold a legible triangle gets nothing rather than a
    // smear. It should not happen with the sheet's width in place, and if the
    // sheet is ever changed so that it does, an absent arrow is a clearer
    // report than a two-pixel one.
    if (r.width() < kMinArrowWidth || r.height() < 4) return;

    const int aw = qBound(kMinArrowWidth, r.width() - 2, kMaxArrowWidth);
    const int ah = qMax(4, aw / 2);
    QRect a(0, 0, aw, ah);
    a.moveCenter(r.center());

    QPainterPath path;
    if (up) {
        path.moveTo(a.left(), a.bottom());
        path.lineTo(a.center().x() + 1, a.top());
        path.lineTo(a.right(), a.bottom());
    } else {
        path.moveTo(a.left(), a.top());
        path.lineTo(a.center().x() + 1, a.bottom());
        path.lineTo(a.right(), a.top());
    }
    path.closeSubpath();

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(Qt::NoPen);
    painter->setBrush(arrowInk(option));
    painter->drawPath(path);
    painter->restore();
}

}  // namespace omega::app
