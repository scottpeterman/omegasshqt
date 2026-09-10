// app/spinarrowstyle.h
//
// Puts the spin box arrows back.
//
// WHAT WAS WRONG. Styling QSpinBox at all -- and the token sheet styles it, so
// it matches the other inputs -- switches the widget to QStyleSheetStyle. The
// arrows were still being drawn: QStyleSheetStyle delegates PE_IndicatorSpinUp
// and PE_IndicatorSpinDown to the base style whenever the up-arrow subcontrol
// has no contents size of its own. What it did NOT do was leave them any room.
// The sheet's `padding: 8px 11px` took the field's width, the subcontrol rect
// came back five pixels wide, and Fusion drew two arrows into a strip too
// narrow to read at the very edge of a rounded border. Measured, not guessed:
// the strip held 23 distinct colours, all of them border antialiasing.
//
// So the fix is two halves and neither works alone. tokenstylesheet.cpp gives
// the buttons a width; this paints into it, in the theme's own ink rather than
// the base style's palette.
//
// WHY A PROXY STYLE AND NOT IMAGES. The obvious alternative is
// `QSpinBox::up-arrow { image: url(...) }`, and it means shipping arrow
// artwork per direction per state that no theme can recolour -- on a set that
// runs from crt-green to a paper-white, a fixed grey arrow is wrong on most of
// them. Painting from tokens costs one file and recolours with everything
// else.
//
// WHY NOT A CUSTOM WIDGET. The tab close button needed one because there was
// no subcontrol to delegate to. Here there is, and it already works; the
// widget would be re-implementing stepping, auto-repeat and accessibility to
// change a colour.
//
// THE SHEET MUST NOT GIVE THE BUTTONS A BOX. Setting `background`, `border` or
// `margin` on up-button or down-button makes QStyleSheetStyle treat the
// subcontrol as drawable and stop delegating -- the arrows vanish again, and
// this style is never called. Verified both ways: geometry only, and the
// primitive fires; add `border: none; background: transparent`, and the call
// count drops to zero. That is the trap the note in tokenstylesheet.cpp used
// to describe as unfixable, and it is only the box half that was.

#ifndef OMEGA_APP_SPINARROWSTYLE_H
#define OMEGA_APP_SPINARROWSTYLE_H

#include <QProxyStyle>

namespace omega::app {

class SpinArrowStyle : public QProxyStyle {
    Q_OBJECT

public:
    explicit SpinArrowStyle(QStyle *base = nullptr);

    void drawPrimitive(PrimitiveElement element, const QStyleOption *option,
                       QPainter *painter,
                       const QWidget *widget = nullptr) const override;
};

}  // namespace omega::app

#endif  // OMEGA_APP_SPINARROWSTYLE_H
