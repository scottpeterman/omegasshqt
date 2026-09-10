// app/vaultunlockdialog.cpp

#include "app/vaultunlockdialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "app/modalframe.h"

namespace omega::app {

using omegassh::Vault;
using omegassh::VaultError;

namespace {
constexpr int kDialogWidth = 580;
}  // namespace

VaultUnlockDialog::VaultUnlockDialog(omegassh::Vault *vault, QWidget *parent)
    : QDialog(parent), vault_(vault) {
    creating_ = vault_ && !vault_->exists();
    if (vault_) vault_->keyringStatus(&keyring_);
    buildUi();
}

void VaultUnlockDialog::buildUi() {
    frame_ = new ModalFrame(
        this, creating_ ? tr("Create vault") : tr("Unlock vault"),
        kDialogWidth);

    // --- which vault ------------------------------------------------------
    // The path is a readout: two vaults on one machine differ only by it, and
    // the whole point of the keyring being keyed on the path is that they are
    // told apart. The chip says up front whether Remember is going to work,
    // which is otherwise only discoverable by finding the checkbox greyed.
    QFrame *well = frame_->addWell();
    auto *wellForm = new QFormLayout(well);
    wellForm->setContentsMargins(14, 12, 14, 12);
    wellForm->setHorizontalSpacing(14);
    wellForm->setVerticalSpacing(8);
    wellForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    wellForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    wellForm->addRow(
        fieldLabel(tr("Vault"), well),
        monoLabel(vault_ ? vault_->path() : QString(), well, "ink",
                  /*readout=*/true));

    const bool keyringUsable = keyring_.available && !keyring_.disabled;
    wellForm->addRow(
        QString(),
        chipLabel(keyringUsable ? tr("OS keyring available")
                                : tr("OS keyring unavailable"),
                  well, keyringUsable ? "state" : "true"));

    // --- the thing that cannot be undone ----------------------------------
    // Only when creating, and always then. This is the one irreversible
    // decision in the application: everything else is a session that can be
    // edited or a connection that can be made again.
    if (creating_) {
        frame_->addNotice(
            tr("There is no recovery. This password is the only thing that "
               "opens the vault, and nothing in Omega or on this machine can "
               "reconstruct it."));
    }

    // --- the answer -------------------------------------------------------
    auto *fields = new QWidget(frame_->bodyWidget());
    auto *form = new QFormLayout(fields);
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(8);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    password_ = new QLineEdit(fields);
    password_->setEchoMode(QLineEdit::Password);
    form->addRow(fieldLabel(tr("Master password"), fields), password_);

    confirm_ = new QLineEdit(fields);
    confirm_->setEchoMode(QLineEdit::Password);
    confirmLabel_ = fieldLabel(tr("Confirm"), fields);
    form->addRow(confirmLabel_, confirm_);
    confirm_->setVisible(creating_);
    confirmLabel_->setVisible(creating_);

    remember_ = new QCheckBox(tr("Remember this password in the OS keyring"),
                              fields);
    form->addRow(QString(), remember_);

    frame_->body()->addWidget(fields);

    // --- what went wrong, when something does -----------------------------
    // After the fields, not before them: it is a response to what was typed,
    // and a message that appears above the input it is about reads as a
    // standing warning instead of an answer.
    problemBox_ = frame_->addNotice(QString());
    problem_ = problemBox_->findChild<QLabel *>();
    problemBox_->hide();

    frame_->body()->addStretch();

    // --- the decision -----------------------------------------------------
    // "Continue without the vault" and not "Cancel". A locked vault is a
    // usable state -- quick connect with a typed password works with no vault
    // at all -- and the button should say what carrying on actually means
    // rather than implying the application is now broken.
    QPushButton *without = frame_->addButton(tr("Continue without the vault"),
                                             ModalFrame::Secondary);
    connect(without, &QPushButton::clicked, this, &QDialog::reject);

    ok_ = frame_->addButton(creating_ ? tr("Create") : tr("Unlock"),
                            ModalFrame::Primary);
    connect(ok_, &QPushButton::clicked, this, &VaultUnlockDialog::attempt);

    frame_->setFooterHint(creating_ ? tr("Return creates the vault.")
                                    : tr("Return unlocks."));

    connect(password_, &QLineEdit::returnPressed, this,
            &VaultUnlockDialog::attempt);
    connect(confirm_, &QLineEdit::returnPressed, this,
            &VaultUnlockDialog::attempt);
    // Typing is the answer to the complaint, so the complaint goes as soon as
    // typing starts. Leaving a red border under a field somebody is actively
    // correcting says the new text is wrong too, which is not yet known.
    connect(password_, &QLineEdit::textEdited, this,
            [this](const QString &) { clearProblem(); });
    connect(confirm_, &QLineEdit::textEdited, this,
            [this](const QString &) { clearProblem(); });

    refreshKeyringRow();
    password_->setFocus();
}

void VaultUnlockDialog::refreshKeyringRow() {
    const bool usable = keyring_.available && !keyring_.disabled;
    remember_->setEnabled(usable);
    if (usable) {
        remember_->setChecked(!keyring_.hasEntry || creating_);
        remember_->setToolTip(tr("Filed against %1, so a lab vault and another "
                                 "vault on this machine never share an entry.")
                                  .arg(keyring_.account));
        return;
    }

    remember_->setChecked(false);
    // Show the platform's own text and do not parse it: a locked Keychain, an
    // absent D-Bus session and an unreachable Credential Manager are three
    // different things to fix, and the library gives them no stable
    // discriminant.
    remember_->setToolTip(keyring_.error.isEmpty()
                              ? tr("The OS keyring is not available.")
                              : keyring_.error);
    // No "(unavailable on this machine)" suffix: the chip in the well
    // already says it, the checkbox is visibly disabled, and the tooltip
    // carries the platform's own reason. A third statement of the same fact
    // was also the widest unwrappable thing in the dialog.
}

void VaultUnlockDialog::setQuietResult(omegassh::VaultError code) {
    switch (code) {
        case VaultError::KeyringStale:
            // The user typed nothing. Saying "wrong password" here is the
            // failure this whole branch exists to avoid -- and for the same
            // reason no field is marked: nothing that was typed is wrong,
            // because nothing was typed.
            showProblem(tr("The password saved in the OS keyring no longer opens "
                           "this vault — it was probably re-keyed elsewhere. "
                           "Enter the current one and it will be re-filed."));
            if (remember_->isEnabled()) remember_->setChecked(true);
            break;
        case VaultError::KeyringUnavailable:
            showProblem(tr("The OS keyring could not be reached, so the vault "
                           "could not open itself. %1")
                            .arg(keyring_.error));
            break;
        default:
            break;
    }
}

void VaultUnlockDialog::markInvalid(QLineEdit *field, bool invalid) {
    if (!field) return;
    if (field->property("invalid").toBool() == invalid) return;
    field->setProperty("invalid", invalid);
    // Without this the selector matches and nothing changes on screen. See
    // the note on repolish() in modalframe.h.
    repolish(field);
}

void VaultUnlockDialog::showProblem(const QString &text, QLineEdit *field) {
    if (invalidField_ && invalidField_ != field) {
        markInvalid(invalidField_, false);
    }
    invalidField_ = field;
    markInvalid(field, true);

    problem_->setText(text);
    const bool wasVisible = problemBox_->isVisible();
    problemBox_->setVisible(!text.isEmpty());
    // The notice grew or appeared, so the dialog has to make room. Only on a
    // change of visibility: replacing one message with another of the same
    // shape does not move anything.
    if (frame_ && wasVisible != problemBox_->isVisible()) {
        frame_->fitToContent();
    }
}

void VaultUnlockDialog::clearProblem() {
    if (!problemBox_ || !problemBox_->isVisible()) return;
    markInvalid(invalidField_, false);
    invalidField_ = nullptr;
    problem_->clear();
    problemBox_->hide();
    if (frame_) frame_->fitToContent();
}

void VaultUnlockDialog::attempt() {
    if (!vault_) return;

    const QString master = password_->text();
    if (master.isEmpty()) {
        showProblem(tr("Enter the master password."), password_);
        password_->setFocus();
        return;
    }
    if (creating_ && master != confirm_->text()) {
        showProblem(tr("The two passwords do not match."), confirm_);
        confirm_->clear();
        confirm_->setFocus();
        return;
    }

    // Argon2id: tens of milliseconds on this machine, well over a hundred on
    // one core. Short enough that a worker thread and a signal would be more
    // machinery than the problem deserves, long enough that a cursor that did
    // not change would read as a dead button.
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const VaultError code =
        creating_ ? vault_->create(master) : vault_->unlock(master);
    QApplication::restoreOverrideCursor();

    if (code != VaultError::Ok) {
        // The password field, except when the vault itself is the problem --
        // a corrupt file or an unreadable path is not something retyping
        // fixes, and a red border on the input says it is.
        const bool aboutTheInput = code == VaultError::WrongPassword ||
                                   code == VaultError::WeakPassword ||
                                   code == VaultError::NeedsPassword;
        showProblem(Vault::describe(code, vault_->lastError()),
                    aboutTheInput ? password_ : nullptr);
        password_->selectAll();
        password_->setFocus();
        return;
    }

    if (remember_->isEnabled() && remember_->isChecked()) {
        // Verified first, by definition: this line is only reached because the
        // password just opened the vault. Filing an unverified string is how
        // an entry becomes a lockout.
        const VaultError filed = vault_->keyringSet(master);
        if (filed != VaultError::Ok) {
            // The vault IS open. Failing to file the entry is worth saying and
            // not worth refusing the unlock over, so it goes out as a warning
            // on the way through rather than as a reason to stay here.
            qWarning("omega: vault unlocked, but the keyring entry was not "
                     "written: %s",
                     qPrintable(vault_->lastError()));
        }
    }

    accept();
}

bool VaultUnlockDialog::unlocked() const {
    return vault_ && !vault_->isLocked();
}

}  // namespace omega::app