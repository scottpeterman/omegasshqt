// app/modalframe.h
//
// The shell every modal in Omega is built inside: an optional title strip, a
// scrolling body, and a footer that holds the hint and the buttons.
//
// NOT A BASE CLASS. Every dialog here is already a QDialog subclass with its
// own accept path, and a second base would mean either multiple inheritance
// or rewriting ten class declarations for the sake of a layout. This installs
// itself onto a dialog instead, so a dialog adopts the frame by constructing
// one and putting its content in body().
//
// THE TITLE STRIP FOLLOWS THE WINDOW, and that is the whole reason it is
// conditional. The redesign draws a modal with its own header bar, which is
// right when the window it belongs to has one too -- and wrong the moment the
// window manager is also drawing a title bar, because then the dialog says
// "Unknown host key" twice, once in each bar. So the strip appears only when
// the parent window is frameless, which is exactly when MainWindow decided to
// take the window manager's bar off. No settings are read here: the parent's
// window flags already carry the answer, and reading them cannot drift from
// what MainWindow actually did.
//
// A frameless dialog gets NO RESIZE EDGES. MainWindow puts them back with an
// application-wide event filter because its content is a terminal somebody
// will want bigger; a modal sizes itself to its content and the width is a
// design decision rather than a preference. If one ever needs to be resizable
// it wants MainWindow's filter, not a second copy of it here.
//
// ONE PRIMARY BUTTON, AND IT IS THE ONE RETURN PRESSES. addButton(Primary)
// sets the property the sheet paints the accent with AND registers the button
// with dialogbuttons.h, in one call, because the failure this prevents is the
// two disagreeing: the loudest button on screen not being the one Enter
// activates. Where the safe answer and the affirmative answer are different
// buttons -- the host key prompt, the paste confirmation -- Primary goes on
// the SAFE one. The accent means "this is what Return does and what this
// dialog recommends", not "this is the affirmative".
//
// Adding a second Primary is a programming error and warns; the last one
// wins, which is the least surprising recovery.

#ifndef OMEGA_APP_MODALFRAME_H
#define OMEGA_APP_MODALFRAME_H

#include <QObject>
#include <QString>

class QDialog;
class QEvent;
class QFrame;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QScrollArea;
class QTabWidget;
class QVBoxLayout;
class QWidget;

namespace omega::app {

class WindowButton;

class ModalFrame : public QObject {
    Q_OBJECT

public:
    enum ButtonKind {
        Secondary,  // the ordinary button: bg.control, a border
        Primary,    // accent-filled, and the button Return activates
    };

    // `dialog` must not have a layout yet -- this installs the root one.
    // `title` goes to setWindowTitle() always, and into the title strip as
    // well on the frameless path.
    //
    // `preferredWidth` is a floor and an opening width, not a fixed size: the
    // host key prompt widens itself so a SHA256 fingerprint fits on one line,
    // and a dialog that could not grow would wrap the one string it exists to
    // make comparable.
    explicit ModalFrame(QDialog *dialog, const QString &title,
                        int preferredWidth = 560);

    // Where content goes. Already carries the body margins and spacing, so a
    // dialog adds widgets and layouts to it directly.
    QVBoxLayout *body() const { return body_; }

    // The widget body() lays out. Pass it as the parent when building a
    // label or a container to add: a widget constructed with no parent is a
    // top-level window until the layout adopts it, which on some platforms is
    // long enough to flash.
    QWidget *bodyWidget() const { return bodyWidget_; }

    // The keyboard hint at the left of the footer. Empty by default, and the
    // label is hidden while it is empty rather than reserving a blank line.
    void setFooterHint(const QString &text);

    // Appended right-to-left in the order added, so the first call is the
    // leftmost button. Parented into the footer; the caller connects it.
    QPushButton *addButton(const QString &text, ButtonKind kind = Secondary);

    // Appends a QFrame[role="notice"] holding one wrapped label. Returned so
    // a dialog that shows it conditionally can hide it -- call fitToContent()
    // after, since the height changed.
    QFrame *addNotice(const QString &text);

    // Appends an empty QFrame[role="well"]. The caller puts a layout on it;
    // this does not guess which one, because the fingerprint block wants a
    // form and the paste preview wants a single widget.
    QFrame *addWell();

    // Re-measures the body and resizes the dialog to fit it, capped to the
    // screen the dialog is actually on. Call after showing or hiding anything
    // in the body; it is called once on the first show already.
    void fitToContent();

protected:
    // Watches the DIALOG, for two events:
    //
    //   Show         the first one is where the screen-capped resize can
    //                happen at all -- before it, the widget has no window
    //                handle and screen() answers with the primary screen,
    //                which is the wrong one on any docked laptop.
    //   StyleChange  arrives when an ancestor's stylesheet is replaced, which
    //                is what MainWindow::applyTheme does. The close button
    //                paints from tokens rather than from the sheet, so it has
    //                to be re-handed them; SettingsDialog previews themes
    //                while open, so this is reachable rather than theoretical.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildTitleStrip(const QString &title);
    void refreshTokens();
    bool beginSystemMove();

    QDialog *dialog_ = nullptr;
    bool frameless_ = false;
    int preferredWidth_ = 560;
    bool firstShowDone_ = false;

    QFrame *titleStrip_ = nullptr;
    WindowButton *close_ = nullptr;

    QScrollArea *scroll_ = nullptr;
    QWidget *bodyWidget_ = nullptr;
    QVBoxLayout *body_ = nullptr;

    QFrame *footer_ = nullptr;
    QHBoxLayout *footerLayout_ = nullptr;
    QLabel *hint_ = nullptr;
    QPushButton *primary_ = nullptr;
};

// --- the label roles, spelled once ----------------------------------------
//
// Free functions rather than ModalFrame members: they are useful in a well or
// a form that the frame knows nothing about, and a dialog should not have to
// reach through the frame to make a label.

// QLabel[role="fieldlabel"] -- the name of a field, above or beside it.
QLabel *fieldLabel(const QString &text, QWidget *parent);

// QLabel[role="desc"] -- secondary explanation. Word-wrapped.
QLabel *descLabel(const QString &text, QWidget *parent);

// Makes a property change actually repaint. Qt resolves a stylesheet against
// a widget's properties when it POLISHES the widget, and setting a property
// afterwards does not re-run that: the selector matches, and nothing changes
// on screen. Every property in this file so far has been set in a constructor,
// before the first polish, which is why the problem has not come up yet -- and
// why it will come up on the first dialog that marks a field invalid after a
// failed attempt.
//
// Call this after any setProperty() on a widget that is already visible.
void repolish(QWidget *widget);

// QLabel[chip="..."] -- a short badge, matching the session ribbon's. `tone`
// is "true" (neutral), "accent", "state" or "warn". Chips are already mono at
// caption size, so no readout flag: a chip is skimmed, not compared.
QLabel *chipLabel(const QString &text, QWidget *parent,
                  const char *tone = "true");

// QLabel[mono="true"] with an optional tone ("ink", "accent", "state",
// "warn", "hint"). Selectable, because everything shown in mono here --
// fingerprints, targets, paths -- exists to be compared or copied.
// `readout` is for a value read character by character rather than skimmed --
// a fingerprint, a target, a key path. Mono's default size is a caption size,
// which is right for a chip and wrong for the one string a dialog exists to
// make comparable.
QLabel *monoLabel(const QString &text, QWidget *parent,
                  const char *tone = "ink", bool readout = false);

// Gives every QLabel[role="fieldlabel"] across all of a tab widget's pages one
// column width, so the inputs do not jump sideways as the user switches tabs.
//
// WHY THIS IS NOT JUST setLabelAlignment. Each QFormLayout sizes its own label
// column from its own longest label, and QFormLayout::labelAlignment only
// positions the label WIDGET inside that column. Two pages with different
// longest labels therefore get different column widths whatever the alignment
// is, and a tab switch is a direct before/after comparison in a way two forms
// stacked on one page never is.
//
// AND THE ALIGNMENT HAS TO MOVE WITH IT. Widening each label to the common
// column leaves labelAlignment nothing to position, so the text falls back to
// the QLabel's own alignment. That is a silent regression -- the
// setLabelAlignment call in the caller still reads correctly and has stopped
// doing anything -- so this sets both, to the same value.
//
// LEFT, and the whole pass is left. Right-aligning labels against a SHARED
// column width puts a gutter between the left edge and the shortest label, and
// that gutter is as wide as the difference between the shortest label and the
// longest one -- which grows with the font. On the settings dialog at
// ui_font_size 18, "Theme" started 290px in from the edge with nothing to its
// left. Right alignment is only comfortable when each form sizes its own
// column to its own labels, which is exactly what this function stops doing.
//
// CALL FROM showEvent, NEVER THE CONSTRUCTOR. The width depends on the
// resolved face and pixel size, which arrive from the generated stylesheet; an
// unpolished label reports the application font. Same rule as every other
// font-dependent measurement in this pass.
void alignFieldLabels(QTabWidget *tabs);

}  // namespace omega::app

#endif  // OMEGA_APP_MODALFRAME_Hvi