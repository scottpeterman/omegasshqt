// app/vaultunlockdialog.h
//
// Phase 4c. The unlock path, and the place the vault's error codes finally pay
// for themselves.
//
// It exists because one string cannot carry the decision a person has to make.
// "Could not open the vault" is true of a mistyped password, of a keyring
// entry that has gone stale, and of a keyring that is not running at all --
// and the right next action is different in each case: retype it, re-file it,
// or carry on without it. So this dialog branches on the code and says which
// one happened.
//
// The stale case is the one worth being careful about. The user typed nothing,
// so telling them the password was wrong sends them looking in the wrong
// place: they go and check the password they never entered, and the thing that
// actually changed -- the vault was re-keyed, or the entry was written by
// another machine -- goes unexamined. It gets its own message and offers to
// re-file the entry after a successful unlock.
//
// THE FAILURE GOES IN A NOTICE AND ON THE FIELD. The words live in the notice
// -- there is no room on a line edit for "the password saved in the OS keyring
// no longer opens this vault" -- but a notice alone leaves the reader to work
// out which of three inputs it is about. So the field the notice names is
// marked invalid=true and gets a danger border, and clearing the notice clears
// the mark. This is the first modal in the pass with a failure state, and the
// pattern is the one the credential editor and the session editor will copy.
//
// It is also the first property set AFTER construction rather than in it,
// which needs ModalFrame's repolish(): Qt resolves the stylesheet when it
// polishes a widget, so a property changed later matches the selector and
// changes nothing on screen.
//
// LOCKED AND UNAVAILABLE ARE BOTH USABLE STATES. Rejecting this dialog is not
// a failure path. Quick connect with a typed password works with no vault at
// all, which is why the second button says what carrying on actually costs
// rather than "Cancel".

#ifndef OMEGA_APP_VAULTUNLOCKDIALOG_H
#define OMEGA_APP_VAULTUNLOCKDIALOG_H

#include <QDialog>

#include <omegasshvault.h>

class QCheckBox;
class QFrame;
class QLabel;
class QLineEdit;
class QPushButton;

namespace omega::app {

class ModalFrame;

class VaultUnlockDialog : public QDialog {
    Q_OBJECT

public:
    // The dialog picks its own mode from the vault: a path with no file on it
    // offers to create one, a file offers to unlock it. Nothing else decides,
    // so a caller cannot ask for the create form against an existing vault.
    explicit VaultUnlockDialog(omegassh::Vault *vault, QWidget *parent = nullptr);

    // Tells the dialog what a prior quiet unlock reported, so it can open with
    // the right explanation already on screen. Call before exec(). Anything
    // outside the keyring range is ignored -- an ordinary locked vault needs
    // no preamble.
    void setQuietResult(omegassh::VaultError code);

    // True when the vault ended up unlocked, which is the only thing a caller
    // needs to know. Accepted() means the same, and this reads better at the
    // call site.
    bool unlocked() const;

private:
    void buildUi();
    void refreshKeyringRow();
    void attempt();

    // `field` is the input the message is about, marked invalid until the
    // next clearProblem(). Null when the problem belongs to no single field.
    void showProblem(const QString &text, QLineEdit *field = nullptr);
    void clearProblem();
    void markInvalid(QLineEdit *field, bool invalid);

    omegassh::Vault *vault_ = nullptr;
    bool creating_ = false;
    omegassh::KeyringStatus keyring_;

    ModalFrame *frame_ = nullptr;
    QFrame *problemBox_ = nullptr;
    QLabel *problem_ = nullptr;
    QLineEdit *invalidField_ = nullptr;
    QLineEdit *password_ = nullptr;
    QLineEdit *confirm_ = nullptr;
    QLabel *confirmLabel_ = nullptr;
    QCheckBox *remember_ = nullptr;
    QPushButton *ok_ = nullptr;
};

}  // namespace omega::app

#endif  // OMEGA_APP_VAULTUNLOCKDIALOG_H