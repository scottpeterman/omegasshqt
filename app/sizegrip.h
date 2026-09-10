// app/sizegrip.h
//
// The corner thumb, for the frameless path only.
//
// A frameless window loses the compositor's resize handles, and the 6px
// invisible border MainWindow's event filter provides is a poor substitute on
// its own: nothing on screen says the window can be resized, and on a touchpad
// the target is close to unhittable. The grip is the affordance that says so.
//
// QSizeGrip already does the hard part. Qt 6's calls startSystemResize(), which
// is the only thing that works under Wayland, where a client is not told where
// its own window is and cannot move it by tracking deltas. It also handles the
// maximised case and mirrors itself under right-to-left layouts. The single
// thing wrong with it is that it paints the platform style's dotted triangle --
// three grey pips belonging to no theme -- so this subclasses it for paintEvent
// and nothing else. Do not reimplement the drag.
//
// Added only on the frameless path. With the compositor's own frame there are
// already handles on four edges and a corner grip is noise.

#ifndef OMEGA_APP_SIZEGRIP_H
#define OMEGA_APP_SIZEGRIP_H

#include <QColor>
#include <QSizeGrip>

#include "theme/tokens.h"

namespace omega::app {

class ThemedSizeGrip : public QSizeGrip {
    Q_OBJECT

public:
    explicit ThemedSizeGrip(QWidget *parent);

    void setTokens(const theme::Tokens &tokens);

protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    bool hover_ = false;
    QColor idle_ = QColor(0x3a, 0x41, 0x48);
    QColor active_ = QColor(0x8b, 0x95, 0x9f);
};

}  // namespace omega::app

#endif  // OMEGA_APP_SIZEGRIP_H
