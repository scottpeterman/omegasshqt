// app/credentialeditordialog.h
//
// The add/edit form, and the one place in the shell where the store-clears-
// omitted-secrets edge is decided.
//
// The rule it implements is the vault's own: a form populated from metadata
// carries no material, so saving it through store() would wipe the password.
// The form therefore has TWO outcomes rather than one, and the user's own
// action picks between them:
//
//   material() == false  the material section was left alone. Saves through
//                        updateMetadata(), which merges and cannot lose a
//                        secret.
//   material() == true   the user supplied material. Saves through store(),
//                        which replaces the record with exactly what is here.
//
// Adding is always the second case. Editing starts as the first and becomes
// the second only when the user says so, by ticking a box that exists purely
// to make that switch deliberate.
//
// The auth-method combo follows the same rule and is disabled until material
// is being supplied. A credential's method and its material move together: a
// record switched from password to publickey with no key is one that cannot
// authenticate, and the switch would have to go through store() anyway.
//
// This dialog never receives a stored secret and never asks for one. Anything
// in the password field is something the operator just typed.

#ifndef OMEGA_APP_CREDENTIALEDITORDIALOG_H
#define OMEGA_APP_CREDENTIALEDITORDIALOG_H

#include <QDialog>

#include <omegasshvault.h>

class QCheckBox;
class QComboBox;
class QFrame;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;

namespace omega::app {

class ModalFrame;

class CredentialEditorDialog : public QDialog {
    Q_OBJECT

public:
    // Adding: pass a default-constructed CredentialMeta with an empty id.
    // Editing: pass what the vault returned, redacted, as it came.
    explicit CredentialEditorDialog(const omegassh::CredentialMeta &existing,
                                    QWidget *parent = nullptr);

    // Which save path the manager should take. See the file comment.
    bool material() const;

    // Valid when material() is false. The edited metadata, id intact.
    omegassh::CredentialMeta metadata() const;

    // Valid when material() is true. The whole record, material included; its
    // id is empty when adding and set when replacing.
    //
    // Non-const because the caller is expected to wipeSecrets() it once the
    // vault has taken it.
    omegassh::CredentialInput input() const;

protected:
    // Where the label columns are measured; see alignFieldLabels().
    void showEvent(QShowEvent *event) override;

private:
    void buildUi();
    void onReplaceToggled(bool on);
    void onAuthChanged();
    void browseForKey();
    bool validate();

    // The failure state, the same shape the vault unlock established: words in
    // the notice, a mark on the field the words are about. `field` is null
    // when nothing that was typed is wrong -- the blank-password warning is
    // about what is ABSENT, and a red border there would assert that the empty
    // field is a mistake when it is a legitimate way to clear material.
    // `field` is marked invalid; `reveal` is the widget whose tab is brought
    // to the front. They are separate because the two questions are: which
    // input is wrong, and where should the user be looking. The blank-password
    // warning marks NOTHING -- the empty field is the thing being confirmed,
    // not a mistake -- but it still has to put the Material tab in front, or
    // the notice describes a field on a page the reader cannot see.
    void showProblem(const QString &text, QWidget *field = nullptr,
                     QWidget *reveal = nullptr);
    void clearProblem();
    void markInvalid(QWidget *field, bool invalid);

    // Shown only while the material section is live AND there is stored
    // material to lose, which is the only combination where store()'s
    // replace-everything contract can destroy something.
    void updateReplaceNotice();

    // Brings the tab containing `w` to the front. Walks up the parent chain
    // rather than keeping a field-to-page map, which is one more thing to
    // forget to update when a field moves between tabs.
    void revealField(QWidget *w);


    omegassh::CredentialMeta existing_;
    bool adding_ = false;

    QLineEdit *name_ = nullptr;
    QLineEdit *username_ = nullptr;
    QLineEdit *description_ = nullptr;
    QLineEdit *tags_ = nullptr;
    QSpinBox *priority_ = nullptr;

    QLineEdit *domainSuffix_ = nullptr;
    QLineEdit *cidrs_ = nullptr;
    QLineEdit *platforms_ = nullptr;

    QCheckBox *isDefault_ = nullptr;
    QCheckBox *disabled_ = nullptr;

    QGroupBox *materialBox_ = nullptr;
    QCheckBox *replace_ = nullptr;
    QComboBox *auth_ = nullptr;
    QLineEdit *password_ = nullptr;
    QLineEdit *keyPath_ = nullptr;
    QPushButton *browse_ = nullptr;
    QLineEdit *passphrase_ = nullptr;

    ModalFrame *frame_ = nullptr;
    QTabWidget *tabs_ = nullptr;
    bool labelsAligned_ = false;
    QFrame *replaceNotice_ = nullptr;
    QFrame *problemBox_ = nullptr;
    QLabel *problem_ = nullptr;
    QWidget *invalidField_ = nullptr;
    QPushButton *save_ = nullptr;

    // Whether the blank-password warning has already been given for the
    // material currently on screen.
    //
    // NOT "is the notice visible", which is what this replaced. The notice is
    // shared with the name error, so a run of: Save with no name, fix the
    // name, Save with no password -- found the notice still up from the first
    // failure, took it as the second press of the confirmation, and stored a
    // credential with its password cleared and nothing said. A flag that means
    // one thing cannot be satisfied by an unrelated message.
    //
    // Cleared whenever the material changes, because a warning about the
    // material on screen a moment ago is not consent about the material now.
    bool blankPasswordWarned_ = false;
};

}  // namespace omega::app

#endif  // OMEGA_APP_CREDENTIALEDITORDIALOG_H
