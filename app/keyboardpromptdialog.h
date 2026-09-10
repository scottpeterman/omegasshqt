// app/keyboardpromptdialog.h
//
// Asking the question the far end asked, in its own words.
//
// WHY THE SERVER'S WORDING IS SHOWN VERBATIM and not replaced with something
// tidier like "One-time password". The question identifies which credential is
// wanted, and on a bastion that matters: "YubiKey for `speterman':" tells the
// operator which token to touch and as whom. Rewriting it would throw away the
// only part of this dialog that carries information, and there is no
// enumeration of what a PAM stack might ask -- the next one could be an SMS
// code, a push confirmation or a security question.
//
// So the question is the label AND the placeholder, and the dialog itself has
// nothing to say beyond the target it belongs to.
//
// SINGLE QUESTION AT A TIME. A keyboard-interactive round can carry several,
// but this flow answers one per dial: the library reports the first question
// it cannot answer and fails there, so the second is not known yet. A stack
// that asks two at once will ask the first, be answered, dial again, and ask
// the second -- more round trips than a live callback would need, and each one
// costs a fresh OTP. Acceptable for the one-question case this is for; it is
// the honest limit of answering through a re-dial rather than a callback.

#ifndef OMEGA_APP_KEYBOARDPROMPTDIALOG_H
#define OMEGA_APP_KEYBOARDPROMPTDIALOG_H

#include <QDialog>
#include <QString>

class QLineEdit;

namespace omega::app {

class ModalFrame;

class KeyboardPromptDialog : public QDialog {
    Q_OBJECT

public:
    // question is shown verbatim. secret masks the field -- it comes from the
    // server's own echo flag and defaults to masking when unknown, because an
    // unmasked one-time password is worse than a masked username.
    KeyboardPromptDialog(const QString &target, const QString &question,
                         bool secret, int attempt, int limit,
                         QWidget *parent = nullptr);

    // What the operator typed. NOT trimmed: an OTP is a fixed-length token and
    // trimming would be harmless, but a passphrase answer may legitimately end
    // in a space, and this dialog does not know which it is holding.
    QString answer() const;

private:
    void buildUi(const QString &target, const QString &question, bool secret,
                 int attempt, int limit);

    ModalFrame *frame_ = nullptr;
    QLineEdit *answer_ = nullptr;
};

}  // namespace omega::app

#endif  // OMEGA_APP_KEYBOARDPROMPTDIALOG_H
