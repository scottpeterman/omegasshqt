// app/pasteconfirmdialog.cpp

#include "app/pasteconfirmdialog.h"

#include <QComboBox>
#include <QAbstractTextDocumentLayout>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QShowEvent>
#include <QTextBlock>
#include <QTextDocument>
#include <QVBoxLayout>

#include "app/modalframe.h"
#include "app/pastequeue.h"

namespace omega::app {
namespace {

// The rates worth offering. Console ports and terminal servers land on these
// almost without exception, and a free-entry field would invite a number that
// is not a real line rate anywhere.
struct RateOption {
    const char *label;
    int baud;
};

const RateOption kRates[] = {
    {QT_TRANSLATE_NOOP("PasteConfirmDialog", "Unlimited"), 0},
    {"1200 baud", 1200},
    {"2400 baud", 2400},
    {"4800 baud", 4800},
    {"9600 baud", 9600},
    {"19200 baud", 19200},
    {"38400 baud", 38400},
    {"57600 baud", 57600},
    {"115200 baud", 115200},
};

// How many preview rows the dialog will grow to hold before the preview
// starts scrolling on its own. The caller already trims the text to its own
// preview length; this is the visual ceiling, so a 40-line paste does not
// produce a dialog the height of the screen with the buttons pushed off it.
constexpr int kMaxPreviewRows = 12;
constexpr int kMinPreviewRows = 3;

constexpr int kDialogWidth = 620;

}  // namespace

PasteConfirmDialog::PasteConfirmDialog(const QString &preview, int lineCount,
                                       int charCount, int currentBaud,
                                       QWidget *parent)
    : QDialog(parent), charCount_(charCount) {
    frame_ = new ModalFrame(this, tr("Paste to terminal"), kDialogWidth);

    // --- what is about to be sent -----------------------------------------
    // Chips rather than a sentence. The two numbers are the thing being
    // judged, and a reader checking whether this is the block they meant is
    // scanning for magnitude, not reading prose.
    auto *counts = new QWidget(frame_->bodyWidget());
    auto *countsRow = new QHBoxLayout(counts);
    countsRow->setContentsMargins(0, 0, 0, 0);
    countsRow->setSpacing(6);
    countsRow->addWidget(
        chipLabel(tr("%1 lines").arg(lineCount), counts, "accent"));
    countsRow->addWidget(
        chipLabel(tr("%1 characters").arg(charCount), counts));
    countsRow->addStretch();
    frame_->body()->addWidget(counts);

    // --- the block itself -------------------------------------------------
    // No well around it: QPlainTextEdit already carries that surface. See the
    // header.
    preview_ = new QPlainTextEdit(preview, frame_->bodyWidget());
    preview_->setReadOnly(true);
    preview_->setProperty("mono", true);
    // No wrapping. A pasted config line that wraps reads as two commands, and
    // the question being answered is "is this the right block", which needs
    // the line structure intact more than it needs every character visible.
    preview_->setLineWrapMode(QPlainTextEdit::NoWrap);
    preview_->setFocusPolicy(Qt::ClickFocus);
    // ALWAYS ON, not as-needed. The height below is an exact number of rows
    // plus the scrollbar, and whether the scrollbar appears is decided after
    // that height is applied -- so "as needed" makes the reservation a guess.
    // Guess high and a half-row of text shows under the last full one, which
    // reads as a clipping bug; guess low and the scrollbar eats the last row.
    // With wrapping off and config lines routinely wider than the dialog it
    // is nearly always shown anyway, so reserving it unconditionally costs a
    // strip of chrome and buys an exact fit.
    preview_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    preview_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    frame_->body()->addWidget(preview_);

    // --- the hazard, when it applies --------------------------------------
    // Conditional, and the condition is the rate. Paced to a line rate there
    // is no flood to warn about, and a warning that is always on screen stops
    // being read by the third time it appears.
    floodNotice_ = frame_->addNotice(
        tr("Sent all at once, a block this size can outrun a console port's "
           "input buffer and arrive mangled. Pick a rate to pace it to the "
           "line."));

    // --- the rate ---------------------------------------------------------
    auto *rateRow = new QWidget(frame_->bodyWidget());
    auto *rateLayout = new QHBoxLayout(rateRow);
    rateLayout->setContentsMargins(0, 0, 0, 0);
    rateLayout->setSpacing(10);
    rateLayout->addWidget(fieldLabel(tr("Send at"), rateRow));

    rate_ = new QComboBox(rateRow);
    for (const RateOption &option : kRates) {
        rate_->addItem(option.baud == 0 ? tr("Unlimited")
                                        : QString::fromLatin1(option.label),
                       option.baud);
    }

    // A rate the tab already carries that is not one of the nine -- a serial
    // session defaulted to its port's own baud, say 14400 -- gets an entry of
    // its own rather than being silently rounded to a neighbour.
    int index = rate_->findData(currentBaud);
    if (index < 0) {
        rate_->addItem(tr("%1 baud").arg(currentBaud), currentBaud);
        index = rate_->count() - 1;
    }
    rate_->setCurrentIndex(index);
    rateLayout->addWidget(rate_, 1);
    frame_->body()->addWidget(rateRow);

    estimate_ = descLabel(QString(), frame_->bodyWidget());
    frame_->body()->addWidget(estimate_);

    connect(rate_, &QComboBox::currentIndexChanged, this,
            &PasteConfirmDialog::updateEstimate);
    updateEstimate();

    // --- the decision -----------------------------------------------------
    // Cancel first and Primary: the accent button is the one Return presses
    // and the one this dialog recommends, which here is not the affirmative.
    QPushButton *cancel = frame_->addButton(tr("Cancel"), ModalFrame::Primary);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);

    QPushButton *paste =
        frame_->addButton(tr("Paste %1 lines").arg(lineCount),
                          ModalFrame::Secondary);
    connect(paste, &QPushButton::clicked, this, &QDialog::accept);

    frame_->setFooterHint(tr("Return cancels. Click Paste to send."));

    cancel->setFocus();
}

int PasteConfirmDialog::selectedBaud() const {
    return rate_ ? rate_->currentData().toInt() : 0;
}

void PasteConfirmDialog::showEvent(QShowEvent *event) {
    QDialog::showEvent(event);

    // Once. A second show must not re-measure against a height that already
    // includes the last answer.
    if (sized_ || !preview_) return;
    sized_ = true;

    // Both are needed before anything is measured: ensurePolished() applies
    // the stylesheet's mono face to the preview -- without it the metrics
    // below belong to the application font -- and activate() gives the widget
    // a laid-out size rather than its default.
    ensurePolished();
    if (QLayout *l = layout()) l->activate();

    const int rows = qBound(kMinPreviewRows,
                            preview_->document()->lineCount(),
                            kMaxPreviewRows);

    // MEASURED, not derived from font metrics. QFontMetrics::lineSpacing() is
    // the font's own line advance; QTextDocument lays a block out with its own
    // line height and margins, and the two differ by enough that a height
    // computed from lineSpacing leaves a half-row of text showing under the
    // last full one. Visible at ui_font_size 13, obvious at 20.
    //
    // blockBoundingRect on the first block is what the document will actually
    // use. The fallback matters only if the document is empty, which the
    // caller does not do -- the paste threshold is what got us here.
    qreal rowHeight = 0;
    if (QAbstractTextDocumentLayout *docLayout =
            preview_->document()->documentLayout()) {
        rowHeight =
            docLayout->blockBoundingRect(preview_->document()->firstBlock())
                .height();
    }
    if (rowHeight <= 0.0) {
        rowHeight = QFontMetrics(preview_->font()).lineSpacing();
    }

    // Everything between the widget's height and its viewport's: both frame
    // borders and the horizontal scrollbar, which is why that scrollbar is
    // ALWAYS ON. Taking the difference is exact and survives a change to the
    // sheet's border width without anyone remembering to update a constant.
    const int chrome = preview_->height() - preview_->viewport()->height();
    const int docMargin =
        static_cast<int>(preview_->document()->documentMargin()) * 2;

    preview_->setFixedHeight(
        static_cast<int>(rowHeight * rows) + docMargin + chrome);

    // The preview's height changed, so the frame has to re-measure.
    frame_->fitToContent();
}

void PasteConfirmDialog::updateEstimate() {
    const int baud = selectedBaud();

    // The warning tracks the rate; see the constructor.
    if (floodNotice_) {
        const bool wasVisible = floodNotice_->isVisible();
        floodNotice_->setVisible(baud <= 0);
        // Only when it actually changed. fitToContent resizes the dialog, and
        // doing that on every combo change would make the window twitch while
        // somebody is arrowing through the rates.
        if (frame_ && wasVisible != floodNotice_->isVisible()) {
            frame_->fitToContent();
        }
    }

    if (baud <= 0) {
        estimate_->setText(tr("%1 characters, sent at once").arg(charCount_));
        return;
    }

    const double seconds = pasteDurationSeconds(charCount_, baud);
    // Below a second the figure is noise -- timer granularity and the
    // scheduler dominate -- so say "under a second" rather than "0.3 s".
    const QString duration =
        seconds < 1.0 ? tr("under a second")
        : seconds < 60.0
            ? tr("about %1 s").arg(seconds, 0, 'f', seconds < 10.0 ? 1 : 0)
            : tr("about %1 min %2 s")
                  .arg(static_cast<int>(seconds) / 60)
                  .arg(static_cast<int>(seconds) % 60);

    estimate_->setText(tr("%1 characters, %2").arg(charCount_).arg(duration));
}

}  // namespace omega::app