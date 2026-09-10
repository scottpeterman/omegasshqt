// app/hostkeypromptdialog.cpp

#include "app/hostkeypromptdialog.h"

#include <QFontMetrics>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QLayout>
#include <QScreen>
#include <QShowEvent>

#include "app/modalframe.h"

namespace omega::app {
namespace {

// The floor the frame opens at. Wider than the 560 default because this
// dialog's content is a fingerprint rather than a form -- starting narrower
// and then widening in showEvent would open the dialog and immediately jump
// it, which reads as a glitch even though the end state is right.
constexpr int kPreferredWidth = 620;

}  // namespace

HostKeyPromptDialog::HostKeyPromptDialog(const HostKeyInfo &info,
                                         QWidget *parent)
    : QDialog(parent) {
    frame_ = new ModalFrame(this, tr("Unknown host key"), kPreferredWidth);

    // --- what happened ----------------------------------------------------
    // A notice rather than a plain label: this is the warn-tinted surface the
    // sheet already carries, and it is the one thing on screen that has to
    // read as "stop and look" before the fingerprint below it is compared.
    frame_->addNotice(
        tr("No key is stored for %1, so Omega cannot tell whether this is "
           "your device or something answering in its place.")
            .arg(info.hostname));

    // --- what was offered -------------------------------------------------
    QFrame *well = frame_->addWell();
    auto *form = new QFormLayout(well);
    form->setContentsMargins(14, 12, 14, 12);
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(8);
    // The fingerprint is the widest thing in the dialog and the reason the
    // dialog has a width at all, so the field column takes the slack rather
    // than the label column.
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);

    form->addRow(fieldLabel(tr("Host"), well),
                 monoLabel(info.hostname, well, "ink", /*readout=*/true));
    form->addRow(fieldLabel(tr("Key type"), well),
                 monoLabel(info.keyType, well, "ink"));

    fingerprint_ = monoLabel(info.fingerprint, well, "warn", /*readout=*/true);
    // The floor for the case the width calculation in showEvent cannot
    // satisfy -- a screen narrower than the fingerprint, or a UI font size
    // large enough that no reasonable dialog holds the line. Wrapping is the
    // degraded rendering, not the expected one.
    fingerprint_->setWordWrap(true);
    form->addRow(fieldLabel(tr("Fingerprint"), well), fingerprint_);

    // --- what accepting does ----------------------------------------------
    frame_->body()->addWidget(descLabel(
        tr("Accept only if the fingerprint above matches what the device "
           "itself reports. Omega stores the key in %1, and refuses the "
           "connection outright if this host ever presents a different one.")
            .arg(info.knownHostsPath),
        frame_->bodyWidget()));

    frame_->body()->addStretch();

    // --- the decision -----------------------------------------------------
    // Reject first, so it is the leftmost of the two, and Primary, so it is
    // both the accent button and the one Return activates. See the header.
    QPushButton *reject =
        frame_->addButton(tr("Reject"), ModalFrame::Primary);
    connect(reject, &QPushButton::clicked, this, &QDialog::reject);

    QPushButton *accept =
        frame_->addButton(tr("Accept and connect"), ModalFrame::Secondary);
    connect(accept, &QPushButton::clicked, this, &QDialog::accept);

    frame_->setFooterHint(tr("Return rejects. Click Accept to continue."));

    reject->setFocus();
}

void HostKeyPromptDialog::showEvent(QShowEvent *event) {
    QDialog::showEvent(event);

    // Once. A second show -- the dialog is re-exec'd, or the window manager
    // maps it again -- must not re-widen from a width that already includes
    // the last answer.
    if (widthApplied_ || !fingerprint_) return;
    widthApplied_ = true;

    // Both are needed before anything is measured. ensurePolished() is what
    // applies the stylesheet's font to the label -- without it the metrics
    // below are the application font's -- and activate() is what gives the
    // label a laid-out width rather than its default one.
    ensurePolished();
    if (QLayout *l = layout()) l->activate();

    // Measured HERE and not in the constructor: the mono face and its pixel
    // size come from the generated stylesheet, and a widget that has not been
    // polished yet still reports the application font. Measuring early gives
    // the right number for the wrong typeface.
    const QFontMetrics metrics(fingerprint_->font());
    const int text = metrics.horizontalAdvance(fingerprint_->text());

    // The label's own margins, the form's label column and spacing, the well's
    // padding and the body's. Measured off the laid-out widgets rather than
    // added up from the constants, so a change to any of them is followed
    // rather than remembered.
    const int chrome = width() - fingerprint_->width();

    int target = text + chrome + 8;
    if (QScreen *s = screen()) {
        // A fingerprint that will not fit the screen wraps, which is what
        // word wrap is still on for. Better a wrapped line than a dialog with
        // its buttons off the edge.
        target = qMin(target, s->availableGeometry().width() - 96);
    }

    // Raise the floor and let the frame settle the size. Resizing here
    // directly would fight fitToContent, which also has the footer's width to
    // satisfy and the screen cap to respect.
    if (target > minimumWidth()) setMinimumWidth(target);
    frame_->fitToContent();
}

}  // namespace omega::app