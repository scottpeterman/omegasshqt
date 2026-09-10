// app/keyboardpromptdialog.cpp

#include "app/keyboardpromptdialog.h"

#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

#include "app/modalframe.h"

namespace omega::app {
namespace {

constexpr int kDialogWidth = 560;

}  // namespace

KeyboardPromptDialog::KeyboardPromptDialog(const QString &target,
                                           const QString &question,
                                           bool secret, int attempt, int limit,
                                           QWidget *parent)
    : QDialog(parent) {
    buildUi(target, question, secret, attempt, limit);
}

QString KeyboardPromptDialog::answer() const {
    return answer_ ? answer_->text() : QString();
}

void KeyboardPromptDialog::buildUi(const QString &target,
                                   const QString &question, bool secret,
                                   int attempt, int limit) {
    frame_ = new ModalFrame(this, tr("Authentication"), kDialogWidth);

    // --- what is being connected to -----------------------------------------
    // Same well as the credential prompt: a prompt that arrives on a tab
    // opened minutes ago is identified by this and nothing else, and here it
    // matters more than usual -- the question may have come from a BASTION
    // rather than the host in the tab title.
    QFrame *well = frame_->addWell();
    auto *wellForm = new QFormLayout(well);
    wellForm->setContentsMargins(14, 12, 14, 12);
    wellForm->setHorizontalSpacing(14);
    wellForm->setVerticalSpacing(8);
    wellForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    wellForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    wellForm->addRow(fieldLabel(tr("Connecting to"), well),
                     monoLabel(target, well, "ink", /*readout=*/true));

    // Only from the second attempt: on the first, a count would suggest
    // something had already gone wrong when nothing has.
    if (attempt > 1) {
        wellForm->addRow(QString(),
                         chipLabel(tr("attempt %1 of %2").arg(attempt).arg(limit),
                                   well, "warn"));
    }

    // --- the question -------------------------------------------------------
    auto *fields = new QWidget(frame_->bodyWidget());
    auto *form = new QFormLayout(fields);
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(8);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    answer_ = new QLineEdit(fields);
    if (secret) {
        // Password, not NoEcho: the operator gets to see that their touch
        // registered, which for a token that types 44 characters at once is
        // the difference between waiting and touching it again.
        answer_->setEchoMode(QLineEdit::Password);
    }

    // The server's own words, as the label AND inside the field. The label
    // column is shared and can be narrow; repeating it as placeholder means
    // the question is readable even when the label elides.
    answer_->setPlaceholderText(question);
    form->addRow(fieldLabel(question, fields), answer_);
    frame_->body()->addWidget(fields);

    frame_->body()->addWidget(descLabel(
        tr("Asked by the far end. The answer is used for this connection "
           "only and is not stored."),
        frame_->bodyWidget()));

    frame_->body()->addStretch();

    QPushButton *cancel = frame_->addButton(tr("Cancel"), ModalFrame::Secondary);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);

    QPushButton *send = frame_->addButton(tr("Send"), ModalFrame::Primary);
    connect(send, &QPushButton::clicked, this, &QDialog::accept);

    frame_->setFooterHint(tr("Return sends."));

    // A hardware token types its own answer and ends with Return, so the
    // field must already hold focus when this opens or the first characters
    // are lost.
    answer_->setFocus();
    connect(answer_, &QLineEdit::returnPressed, this, &QDialog::accept);
}

}  // namespace omega::app
