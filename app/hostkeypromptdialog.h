// app/hostkeypromptdialog.h
//
// The fingerprint prompt shown on first contact with a host.
//
// What it is NOT: a yes/no on a scary message. The whole value of this dialog
// is the fingerprint being readable and comparable against what the device
// reports, so the fingerprint is the largest thing in it and is selectable
// for copying.
//
// REJECT IS THE PRIMARY BUTTON, and on this dialog "primary" and "affirmative"
// are different buttons on purpose. Accepting an unverified host key is a
// decision; Enter should not be how it gets made, and the accent should not be
// on the control the dialog is arguing against. See the note in
// dialogbuttons.h -- the accent means "this is what Return does and what this
// dialog recommends", which here is Reject.
//
// THE WIDTH IS SET FROM THE FINGERPRINT, in showEvent rather than in the
// constructor. A SHA256 fingerprint broken across two lines is materially
// harder to compare against what the device printed, which is the only job
// this dialog has -- so the dialog widens to fit one. It cannot be measured at
// construction: the mono face and its size arrive from the generated
// stylesheet, and the sheet has not been applied to the widget yet, so
// QFontMetrics would measure the application font and answer for the wrong
// face. Word wrap stays on underneath as a floor for a pathological key type
// or a screen too narrow to hold the line, not as the expected rendering.

#ifndef OMEGA_APP_HOSTKEYPROMPTDIALOG_H
#define OMEGA_APP_HOSTKEYPROMPTDIALOG_H

#include <QDialog>

#include "app/hostkeyerror.h"

class QLabel;

namespace omega::app {

class ModalFrame;

class HostKeyPromptDialog : public QDialog {
    Q_OBJECT

public:
    explicit HostKeyPromptDialog(const HostKeyInfo &info,
                                 QWidget *parent = nullptr);

protected:
    // Where the fingerprint is measured; see the header note.
    void showEvent(QShowEvent *event) override;

private:
    ModalFrame *frame_ = nullptr;
    QLabel *fingerprint_ = nullptr;
    bool widthApplied_ = false;
};

}  // namespace omega::app

#endif  // OMEGA_APP_HOSTKEYPROMPTDIALOG_H