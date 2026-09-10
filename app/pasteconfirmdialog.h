// app/pasteconfirmdialog.h
//
// The confirmation shown before a multi-line paste reaches the terminal, and
// the one place the paste rate is worth asking about.
//
// WHY THE RATE LIVES HERE. This dialog is already on screen at the only
// moment the answer is knowable: what is about to go out is visible, and so
// is how much of it there is. A rate buried in a settings page is set once
// and then wrong for whatever gets pasted next; a rate on the modal is
// decided against the actual block, and the estimate under it says what the
// choice costs before it is committed to.
//
// It is expressed in baud rather than in milliseconds because baud is the
// number already known about the far end. A console port is 9600; a terminal
// server's downstream line is whatever it was configured for. Pacing the
// paste to that figure is a decision that can be made from the label on the
// device rather than by guessing at a delay.
//
// CANCEL IS THE PRIMARY BUTTON, and here "primary" and "affirmative" are
// different buttons on purpose -- the same inversion as the host key prompt.
// The dialog exists to stop an accidental paste, so Return must not be the
// thing that confirms one, and the accent must not sit on the control the
// dialog is arguing against. See the note in dialogbuttons.h.
//
// THE PREVIEW IS NOT WRAPPED IN A WELL. The obvious reading of the redesign
// is a QFrame[role="well"] around the preview, and it is wrong: QPlainTextEdit
// derives from QFrame via QAbstractScrollArea, and the sheet already gives it
// the well's own surface -- bg.deep with a line.input border. Wrapping it
// draws that box twice, which is exactly the bug the notice frame had before
// its role was renamed. The preview IS the well.
//
// THE PREVIEW'S HEIGHT IS SET IN showEvent, for the same reason the host key
// prompt measures its fingerprint there: the mono face and its pixel size
// arrive from the generated stylesheet, and a widget that has not been
// polished yet reports the application font. Sizing it at construction gives
// a height that is right for a typeface the preview is not drawn in.

#ifndef OMEGA_APP_PASTECONFIRMDIALOG_H
#define OMEGA_APP_PASTECONFIRMDIALOG_H

#include <QDialog>
#include <QString>

class QComboBox;
class QFrame;
class QLabel;
class QPlainTextEdit;

namespace omega::app {

class ModalFrame;

class PasteConfirmDialog : public QDialog {
    Q_OBJECT

public:
    // preview is the text to show, already elided by the caller; lineCount
    // and charCount describe the whole paste, not the preview. currentBaud is
    // what the tab is set to now and is where the selector starts -- 0
    // meaning unlimited.
    PasteConfirmDialog(const QString &preview, int lineCount, int charCount,
                       int currentBaud, QWidget *parent = nullptr);

    // The rate chosen, in baud, or 0 for unlimited. Read after exec().
    int selectedBaud() const;

protected:
    // Where the preview is sized; see the header note.
    void showEvent(QShowEvent *event) override;

private:
    void updateEstimate();

    ModalFrame *frame_ = nullptr;
    QPlainTextEdit *preview_ = nullptr;
    QComboBox *rate_ = nullptr;
    QLabel *estimate_ = nullptr;
    QFrame *floodNotice_ = nullptr;
    int charCount_ = 0;
    bool sized_ = false;
};

}  // namespace omega::app

#endif  // OMEGA_APP_PASTECONFIRMDIALOG_H