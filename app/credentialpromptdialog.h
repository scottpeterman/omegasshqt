// app/credentialpromptdialog.h
//
// Credentials for a session that does not carry any.
//
// WHY IT EXISTS. Every session imported from TerminalTelemetry, and every one
// saved without picking an entry, has credential_name NULL. Under
// SshDefaultAuth::Ask that is a question rather than a failure, and this asks
// it -- once, at connect time, without editing the session.
//
// It is also the fallback under SshDefaultAuth::VaultDefault when the vault
// holds no default credential. That combination is ordinary on a first run:
// the mode is the shipped one and a new vault has nothing marked default, so
// dialing with nothing would be the common case rather than the rare one.
//
// SAME BOUNDARY AS QUICK CONNECT. A chosen credential crosses as a NAME and
// is read on the Go side during the dial; a typed password is one the
// operator just typed. Nothing here reads secret material out of the vault,
// which is why it takes the vault and not a credential.
//
// NOTHING IS SAVED. The answer applies to this connection. Writing it back to
// the session would be a store write from a connect path, and the session
// editor is where a session is edited.
//
// CONNECT IS THE PRIMARY BUTTON, and this is the first modal in the pass
// where the accent and the affirmative land on the same control. That is not
// a departure from the host key prompt and the paste confirmation -- it is
// the same rule reaching a different answer. Primary marks what Return does
// and what the dialog recommends; here the dialog is asking a question rather
// than guarding against an action, so the recommended answer IS the
// affirmative one. Nothing is lost by pressing Return: an empty password
// against a target that wanted one is a failed connect, not a mangled device.
//
// THE LOCKED VAULT GETS A NOTICE, not a greyed row in the picker. A locked
// vault and an empty one produce the same empty list, and the next move is
// different in each case -- one is "unlock it", the other is "type something".
// A disabled combo entry says which, but only to someone who opens the combo
// to look, which is the one thing a person who thinks the vault is empty will
// not do.

#ifndef OMEGA_APP_CREDENTIALPROMPTDIALOG_H
#define OMEGA_APP_CREDENTIALPROMPTDIALOG_H

#include <QDialog>
#include <QHash>
#include <QString>

#include <omegasshvault.h>

class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;

namespace omega::app {

class ModalFrame;

class CredentialPromptDialog : public QDialog {
    Q_OBJECT

public:
    // target is what the connection is TO, shown so a prompt that arrives
    // over a tab somebody opened by accident says which one it is.
    // vault may be null, locked or empty; all three leave the picker on
    // "(type credentials below)" with the reason shown, as elsewhere.
    // Why the prompt appeared. The dialog asks the same question either way
    // and returns the same three things; only what it says about itself
    // differs, and it differs in the two places that would otherwise be
    // actively wrong after a rejection -- a chip reading "session names no
    // credential" when the session named one and it was refused, and a footer
    // advising the operator to set a credential when a credential is exactly
    // what just failed.
    enum class Reason {
        // No credential to dial with: the session names none and the mode
        // wants one. Asked BEFORE any dial. The original case.
        NoCredential,

        // The far end refused what was offered. Asked AFTER a dial failed,
        // and possibly more than once. See autherror.h.
        Rejected,
    };

    CredentialPromptDialog(const QString &target, omegassh::Vault *vault,
                           QWidget *parent = nullptr)
        : CredentialPromptDialog(target, vault, Reason::NoCredential, parent) {}

    CredentialPromptDialog(const QString &target, omegassh::Vault *vault,
                           Reason reason, QWidget *parent = nullptr);

    // The chosen vault entry by name, or empty when the operator typed
    // credentials instead. Only meaningful after exec() returned Accepted.
    QString credential() const;

    // Typed overrides. Both may be empty: a credential supplies what these
    // leave blank, which is the same rule the library applies everywhere
    // else -- explicit fields win over a reference.
    QString username() const;
    QString password() const;

private:
    void buildUi(const QString &target, Reason reason);
    void populateCredentials();
    void onCredentialChanged();

    omegassh::Vault *vault_ = nullptr;

    ModalFrame *frame_ = nullptr;
    QComboBox *credential_ = nullptr;
    QLabel *credentialDetail_ = nullptr;
    QLineEdit *username_ = nullptr;
    QLineEdit *password_ = nullptr;
    QFrame *lockedNotice_ = nullptr;

    // What each vault entry provides, by name, so the detail line under the
    // picker can be filled without a second list() call per selection.
    QHash<QString, QString> detail_;
    QString defaultName_;
};

}  // namespace omega::app

#endif  // OMEGA_APP_CREDENTIALPROMPTDIALOG_H