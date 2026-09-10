// app/credentialpromptdialog.cpp

#include "app/credentialpromptdialog.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVector>

#include "app/modalframe.h"

namespace omega::app {
namespace {

constexpr int kDialogWidth = 560;

// What a credential offers, in one line, for the detail under the picker.
// authLabel first because it is the part that decides whether a password
// field is even relevant -- a key-based entry will not be asking for one.
QString describe(const omegassh::CredentialMeta &meta, bool isDefault) {
    QStringList parts;
    if (!meta.authLabel.isEmpty()) parts << meta.authLabel;
    if (!meta.username.isEmpty()) parts << meta.username;
    if (!meta.description.isEmpty()) parts << meta.description;
    if (isDefault) {
        parts << QCoreApplication::translate("CredentialPromptDialog",
                                             "vault default");
    }
    return parts.join(QStringLiteral(" · "));
}

}  // namespace

CredentialPromptDialog::CredentialPromptDialog(const QString &target,
                                               omegassh::Vault *vault,
                                               Reason reason, QWidget *parent)
    : QDialog(parent), vault_(vault) {
    buildUi(target, reason);
    onCredentialChanged();
}

void CredentialPromptDialog::buildUi(const QString &target, Reason reason) {
    const bool rejected = reason == Reason::Rejected;

    // The title carries the difference too. "Credentials" is a neutral
    // heading for a question asked before anything was tried; after a
    // rejection the operator needs to know at a glance that a dial happened
    // and failed, because the tab underneath still shows the target and
    // nothing else has said so yet.
    frame_ = new ModalFrame(
        this, rejected ? tr("Authentication failed") : tr("Credentials"),
        kDialogWidth);

    // --- what is being connected to ---------------------------------------
    // The target in the well, mono and readout-sized, because it is the one
    // string here somebody reads character by character -- a prompt that
    // arrived over a tab opened by accident is identified by this and nothing
    // else. The chip beside it says WHY the prompt appeared, which is the
    // other half of that question.
    QFrame *well = frame_->addWell();
    auto *wellForm = new QFormLayout(well);
    wellForm->setContentsMargins(14, 12, 14, 12);
    wellForm->setHorizontalSpacing(14);
    wellForm->setVerticalSpacing(8);
    wellForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    wellForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    wellForm->addRow(fieldLabel(tr("Connecting to"), well),
                     monoLabel(target, well, "ink", /*readout=*/true));
    wellForm->addRow(
        QString(),
        chipLabel(rejected ? tr("the far end refused these credentials")
                           : tr("session names no credential"),
                  well, "warn"));

    // --- the locked vault, when that is what happened ---------------------
    // Created hidden and shown by populateCredentials, so the decision lives
    // in one place with the rest of the vault-state handling.
    lockedNotice_ = frame_->addNotice(
        tr("The vault is locked, so its credentials are not listed. Unlock it "
           "from the Vault menu, or type credentials below for this "
           "connection."));
    lockedNotice_->hide();

    // --- the answer -------------------------------------------------------
    auto *fields = new QWidget(frame_->bodyWidget());
    auto *form = new QFormLayout(fields);
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(8);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    credential_ = new QComboBox(fields);
    populateCredentials();
    form->addRow(fieldLabel(tr("Credential"), fields), credential_);

    // Spans both columns, under the picker rather than beside it: what an
    // entry provides is a description of the row above, not a field of its
    // own, and giving it a label column would make it read as one.
    credentialDetail_ = descLabel(QString(), fields);
    form->addRow(QString(), credentialDetail_);

    username_ = new QLineEdit(fields);
    form->addRow(fieldLabel(tr("Username"), fields), username_);

    password_ = new QLineEdit(fields);
    password_->setEchoMode(QLineEdit::Password);
    form->addRow(fieldLabel(tr("Password"), fields), password_);

    frame_->body()->addWidget(fields);

    frame_->body()->addWidget(descLabel(
        rejected
            ? tr("Applies to this connection only; the session is not "
                 "changed. Cancel to stop retrying and read the full error "
                 "on the tab.")
            : tr("Applies to this connection only. Set a credential on the "
                 "session to stop being asked, or mark one default in the "
                 "vault."),
        frame_->bodyWidget()));

    frame_->body()->addStretch();

    // --- the decision -----------------------------------------------------
    // Connect is Primary here; see the header for why that is the same rule
    // as the host key prompt reaching a different answer.
    QPushButton *cancel = frame_->addButton(tr("Cancel"), ModalFrame::Secondary);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);

    QPushButton *connectButton =
        frame_->addButton(tr("Connect"), ModalFrame::Primary);
    connect(connectButton, &QPushButton::clicked, this, &QDialog::accept);

    frame_->setFooterHint(tr("Return connects."));

    connect(credential_, &QComboBox::currentIndexChanged, this,
            [this](int) { onCredentialChanged(); });

    // Focus follows the answer: with a vault to choose from the picker is the
    // first move, without one the username is.
    if (credential_->count() > 1) {
        credential_->setFocus();
    } else {
        username_->setFocus();
    }
}

void CredentialPromptDialog::populateCredentials() {
    credential_->clear();
    detail_.clear();
    defaultName_.clear();
    credential_->addItem(tr("(type credentials below)"), QString());

    if (!vault_ || !vault_->isOpen()) return;

    if (vault_->isLocked()) {
        // Said in a notice rather than as a greyed row; see the header. The
        // notice may not exist yet on the first call, since populate runs
        // from inside buildUi.
        if (lockedNotice_) lockedNotice_->show();
        return;
    }
    if (lockedNotice_) lockedNotice_->hide();

    QVector<omegassh::CredentialMeta> creds;
    if (vault_->list(&creds) != omegassh::VaultError::Ok) return;

    vault_->defaultName(&defaultName_);

    for (const omegassh::CredentialMeta &c : creds) {
        if (c.disabled) continue;  // refuses at dial time; not worth offering
        credential_->addItem(c.name, c.name);
        detail_.insert(c.name, describe(c, c.name == defaultName_));
        // Preselected rather than merely listed. Under VaultDefault this
        // dialog only opens when there IS no default, so this matters under
        // Ask -- where the default is still the best guess at the answer.
        if (!defaultName_.isEmpty() && c.name == defaultName_) {
            credential_->setCurrentIndex(credential_->count() - 1);
        }
    }
}

void CredentialPromptDialog::onCredentialChanged() {
    const QString name = credential_->currentData().toString();
    const bool named = !name.isEmpty();

    // Enabled beside a chosen credential rather than greyed, for the reason
    // quick connect gives: naming a credential for the password and typing
    // the username over it is a real thing to do, and the placeholder says
    // what a blank field will use.
    username_->setPlaceholderText(named ? tr("from the credential") : QString());
    password_->setPlaceholderText(named ? tr("from the credential") : QString());

    if (!credentialDetail_) return;

    const QString detail = named ? detail_.value(name) : QString();
    credentialDetail_->setText(detail);
    const bool wasVisible = credentialDetail_->isVisible();
    // Hidden rather than blank when there is nothing to say: an empty label
    // still occupies a form row, and a gap under the picker reads as a
    // missing field rather than as an absent description.
    credentialDetail_->setVisible(!detail.isEmpty());

    // Only on a change. fitToContent resizes the dialog, and doing that on
    // every selection would make the window twitch while somebody arrows
    // through the list.
    if (frame_ && wasVisible != credentialDetail_->isVisible()) {
        frame_->fitToContent();
    }
}

QString CredentialPromptDialog::credential() const {
    return credential_->currentData().toString();
}

QString CredentialPromptDialog::username() const {
    return username_->text().trimmed();
}

QString CredentialPromptDialog::password() const {
    // NOT trimmed. A password is bytes somebody typed, and a trailing space
    // in one is the operator's business.
    return password_->text();
}

}  // namespace omega::app