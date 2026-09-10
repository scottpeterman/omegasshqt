// app/windowbuttons.h
//
// Minimise, maximise, restore and close -- painted, not typed.
//
// The obvious implementation is a QPushButton with a character in it, and it
// fails in a way that is easy to miss on the machine you wrote it on. U+2013,
// U+25A1 and U+2715 are not all present in IBM Plex Sans, so Qt substitutes
// per character from whatever fontconfig hands back: three glyphs from up to
// three families at three baselines, and on a box with no U+25A1 anywhere the
// maximise button renders as a second dash indistinguishable from minimise.
//
// Four short strokes derived from the widget's own rect remove the dependency
// on font coverage entirely, and get the DPI scaling right for free.
//
// NOT STYLED BY QSS. The stylesheet cannot express "a red wash on hover for
// this one button and a neutral one for the other two", and a QSS rule reaching
// these would also have to be undone on the native-frame path where they do not
// exist. Colours arrive through setTokens(), which MainWindow::applyTheme calls
// on every theme change -- the same reason the session delegate will paint from
// tokens rather than carry an inline sheet.

#ifndef OMEGA_APP_WINDOWBUTTONS_H
#define OMEGA_APP_WINDOWBUTTONS_H

#include <QAbstractButton>
#include <QColor>

#include "theme/tokens.h"

namespace omega::app {

class WindowButton : public QAbstractButton {
    Q_OBJECT

public:
    enum Glyph { Minimise, Maximise, Restore, Close };

    explicit WindowButton(Glyph glyph, QWidget *parent = nullptr);

    // Maximise <-> Restore. A no-op when the glyph has not changed, so the
    // window-state handler can call it on every state change without repainting
    // for a move or a focus change.
    void setGlyph(Glyph glyph);

    void setTokens(const theme::Tokens &tokens);

protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    Glyph glyph_;
    bool hover_ = false;

    // Defaults are the spec's dark ramp, so a button constructed before any
    // theme has landed still paints something sensible rather than black on
    // black.
    QColor ink_ = QColor(0x8b, 0x95, 0x9f);
    QColor inkHover_ = QColor(0xe6, 0xe9, 0xec);
    QColor hoverBg_ = QColor(0x26, 0x2b, 0x31);
    QColor closeBg_ = QColor(0x8c, 0x3b, 0x3b);
};

}  // namespace omega::app

#endif  // OMEGA_APP_WINDOWBUTTONS_H
