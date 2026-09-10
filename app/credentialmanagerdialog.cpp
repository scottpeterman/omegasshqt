// app/credentialmanagerdialog.cpp

#include "app/credentialmanagerdialog.h"

#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QScrollBar>
#include <QStyle>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QShowEvent>
#include <QTableWidget>
#include <QVBoxLayout>

#include "app/credentialeditordialog.h"
#include "app/modalframe.h"
#include "app/vaultunlockdialog.h"

namespace omega::app {
namespace {

// Wider than the form dialogs, and it has to be: six columns of a table, not
// a label column and a field column. Still a floor rather than a fixed size --
// fitToContent widens past it when the action row needs more, which at
// ui_font_size 20 and up it does.
constexpr int kDialogWidth = 860;

// How many rows the table opens showing. Not a pixel height: the row height
// follows the font, so the constant that survives a font change is a count of
// rows and the height is measured from one.
constexpr int kVisibleRows = 9;

// Scope, in the header order below. Named because two places need it and a
// bare 3 in a setSectionResizeMode call is the kind of thing that silently
// starts pointing at Tags when a column is inserted.
constexpr int kScopeColumn = 3;

}  // namespace

using omegassh::CredentialInput;
using omegassh::CredentialMeta;
using omegassh::Vault;
using omegassh::VaultError;

CredentialManagerDialog::CredentialManagerDialog(omegassh::Vault *vault,
                                                 QWidget *parent)
    : QDialog(parent), vault_(vault) {
    buildUi();
    refresh();
}

void CredentialManagerDialog::buildUi() {
    frame_ = new ModalFrame(this, tr("Credentials"), kDialogWidth);

    // --- which vault ------------------------------------------------------
    // What the status label used to say in one wrapped sentence, split into
    // the three things it was actually carrying: which file, what state it is
    // in, and which credential is the default. The path is a readout for the
    // same reason it is one on the vault unlock -- two vaults on one machine
    // differ only by it.
    QFrame *well = frame_->addWell();
    auto *wellForm = new QFormLayout(well);
    wellForm->setContentsMargins(14, 12, 14, 12);
    wellForm->setHorizontalSpacing(14);
    wellForm->setVerticalSpacing(8);
    wellForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    wellForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    vaultPath_ = monoLabel(QString(), well, "ink", /*readout=*/true);
    wellForm->addRow(fieldLabel(tr("Vault"), well), vaultPath_);

    stateChip_ = chipLabel(QString(), well, "true");
    wellForm->addRow(QString(), stateChip_);

    defaultName_ = monoLabel(QString(), well, "ink", /*readout=*/true);
    wellForm->addRow(fieldLabel(tr("Default"), well), defaultName_);

    // --- the list ---------------------------------------------------------
    table_ = new QTableWidget(0, 6, frame_->bodyWidget());
    table_->setHorizontalHeaderLabels({tr("Name"), tr("Username"), tr("Method"),
                                       tr("Scope"), tr("Tags"), tr("Last used")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->setVisible(false);

    // COLUMN SIZING, AND SCOPE IS THE ONE THAT GIVES. resizeColumnsToContents
    // plus stretchLastSection was the previous arrangement and it clips: the
    // scope summary is the one cell with unbounded content -- a domain suffix
    // plus every CIDR plus every platform -- so sizing it to its contents
    // pushes the total past the viewport, and what falls off the right edge is
    // the LAST column, which stretchLastSection had just promised to widen.
    // Rendered at 860px with three CIDRs it read "Last us" and grew a
    // horizontal scrollbar.
    //
    // So: every column sized to its contents except Scope, which takes the
    // slack and elides. That is the right column to lose characters from --
    // Name, Username, Method, Tags and Last used are all short and all
    // identify the row, while Scope is a summary that is already lossy.
    QHeaderView *header = table_->horizontalHeader();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(QHeaderView::ResizeToContents);
    header->setSectionResizeMode(kScopeColumn, QHeaderView::Stretch);
    // The sheet paints the surface; a frame on top of it draws a second edge
    // inside the one the well and noticebox already establish. Same reasoning
    // as never wrapping a QPlainTextEdit in a well.
    table_->setFrameShape(QFrame::NoFrame);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(false);
    frame_->body()->addWidget(table_, 1);

    // --- the actions ------------------------------------------------------
    // In the body and not the footer, deliberately. The footer holds what
    // closes the dialog; these operate on the row that is selected, and a
    // footer that mixed the two would put Delete beside Close.
    auto *actions = new QWidget(frame_->bodyWidget());
    actions->setProperty("bare", true);
    auto *row = new QHBoxLayout(actions);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);

    unlock_ = new QPushButton(tr("Unlock…"), actions);
    add_ = new QPushButton(tr("Add…"), actions);
    edit_ = new QPushButton(tr("Edit…"), actions);
    rename_ = new QPushButton(tr("Rename…"), actions);
    delete_ = new QPushButton(tr("Delete"), actions);
    default_ = new QPushButton(tr("Set default"), actions);
    disable_ = new QPushButton(tr("Disable"), actions);

    for (QPushButton *b : {unlock_, add_, edit_, rename_, delete_, default_,
                           disable_}) {
        // EVERY ONE OF THEM, EXPLICITLY. QPushButton turns autoDefault on for
        // itself inside a QDialog, and ModalFrame's pass -- the thing that
        // clears it across a set -- only walks the FOOTER's direct children.
        // Left alone, the first of these in the focus chain becomes what
        // Return activates by construction order alone. That is what this
        // dialog did before the pass: `Unlock…` carried default=true purely
        // because it was built first, and Return did nothing in the open
        // state only because that button happens to be disabled exactly when
        // the destructive ones are enabled. Two swapped constructor lines and
        // Return is Delete.
        b->setAutoDefault(false);
        b->setDefault(false);
        row->addWidget(b);
    }
    // Danger colours, no fill until hover. The accent belongs to the button
    // Return activates and this must never be that button; see the rule in
    // patch-tokenstylesheet-4.py.
    delete_->setProperty("destructive", true);

    row->addStretch(1);
    frame_->body()->addWidget(actions);

    // --- what went wrong, when something does -----------------------------
    problemBox_ = frame_->addNotice(QString());
    problem_ = problemBox_->findChild<QLabel *>();
    problemBox_->hide();

    // No addStretch() here, unlike the form dialogs: the table already has the
    // stretch factor, so a second stretch would take the space away from the
    // one widget in this dialog that can use it.

    // --- the decision -----------------------------------------------------
    // Close, and Primary on it. Every other button here acts on a row; the
    // only thing this dialog can be said to recommend is leaving, and it is
    // the only answer that is safe in all four vault states. Unlock stays in
    // the action row rather than being promoted to Primary when locked --
    // ModalFrame fixes the accent at addButton time, and a footer button that
    // changed kind with the vault's state would mean the loud button and the
    // Return key disagreeing during the moment they were being re-decided.
    QPushButton *close = frame_->addButton(tr("Close"), ModalFrame::Primary);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    frame_->setFooterHint(tr("Return closes. Double-click a row to edit it."));

    connect(unlock_, &QPushButton::clicked, this, &CredentialManagerDialog::unlock);
    connect(add_, &QPushButton::clicked, this, &CredentialManagerDialog::addCredential);
    connect(edit_, &QPushButton::clicked, this, &CredentialManagerDialog::editCredential);
    connect(rename_, &QPushButton::clicked, this,
            &CredentialManagerDialog::renameCredential);
    connect(delete_, &QPushButton::clicked, this,
            &CredentialManagerDialog::deleteCredential);
    connect(disable_, &QPushButton::clicked, this,
            &CredentialManagerDialog::toggleDisabled);
    connect(default_, &QPushButton::clicked, this, &CredentialManagerDialog::makeDefault);
    connect(table_, &QTableWidget::itemDoubleClicked, this,
            [this](QTableWidgetItem *) { editCredential(); });
    connect(table_, &QTableWidget::itemSelectionChanged, this, [this] {
        const CredentialMeta m = selected();
        const bool have = !m.id.isEmpty();
        edit_->setEnabled(have);
        rename_->setEnabled(have);
        delete_->setEnabled(have);
        default_->setEnabled(have);
        disable_->setEnabled(have);
        disable_->setText(m.disabled ? tr("Enable") : tr("Disable"));
        // Set default becomes Clear default on the credential that already is
        // one, so the only way to end up with no default is on the record it
        // affects rather than on a button that is always live.
        default_->setText(m.isDefault ? tr("Clear default") : tr("Set default"));
    });
}

void CredentialManagerDialog::showEvent(QShowEvent *event) {
    QDialog::showEvent(event);
    sizeTable();
}

void CredentialManagerDialog::sizeTable() {
    if (tableSized_) return;
    tableSized_ = true;

    // AFTER POLISH, NOT IN THE CONSTRUCTOR. The face and pixel size arrive
    // from the generated stylesheet, and an unpolished widget reports the
    // application font -- so a row height measured at construction is the
    // height of a row in a typeface the table is not drawn in.
    table_->ensurePolished();
    if (table_->layout()) table_->layout()->activate();

    // verticalHeader()->defaultSectionSize() is the row height the view will
    // actually use, which is the style's, not the font's line spacing. Asking
    // the widget rather than deriving it from QFontMetrics is the same
    // distinction the paste preview's height had to make.
    QHeaderView *header = table_->horizontalHeader();

    const int rowH = table_->verticalHeader()->defaultSectionSize();
    const int headerH = header->sizeHint().height();
    table_->setMinimumHeight(headerH + kVisibleRows * rowH);

    // NO SECTION NARROWER THAN ITS OWN HEADER, and this is not cosmetic.
    // QHeaderView::Stretch ignores the section's size hint, so the Scope
    // column -- the one deliberately given the slack -- collapses to whatever
    // the other five leave it. Its CELLS elide correctly, but the header text
    // does not: QHeaderView cuts the label mid-glyph with no ellipsis. At
    // ui_font_size 28 the column headed "Scope" rendered as "cop". Invisible
    // at 13, which is the whole argument for sweeping the range.
    //
    // Measured here rather than in the constructor for the usual reason: the
    // header's font arrives from the generated stylesheet, so a width computed
    // before the first polish is a width in the wrong typeface.
    //
    // The pad is the style's own header margin, doubled for both sides, plus
    // one em of slack for the sort indicator the style reserves whether or not
    // sorting is on. Taken from the style rather than assumed, so a sheet that
    // changes QHeaderView::section padding does not silently re-break this.
    const int margin =
        header->style()->pixelMetric(QStyle::PM_HeaderMargin, nullptr, header);
    const int pad = 2 * margin + header->fontMetrics().horizontalAdvance(QLatin1Char('m'));

    int widest = 0;
    for (int col = 0; col < table_->columnCount(); ++col) {
        const QTableWidgetItem *h = table_->horizontalHeaderItem(col);
        if (!h) continue;
        widest = qMax(widest,
                      header->fontMetrics().horizontalAdvance(h->text()) + pad);
    }
    // A floor for every section, which is what QHeaderView offers -- there is
    // no per-section minimum. The cost is that the four short columns are also
    // held at this width; the benefit is that no header in the table can be
    // cut, including the stretching one. Where the total then exceeds the
    // viewport the table scrolls horizontally, which is a visible failure a
    // user can navigate rather than a silent one that eats characters.
    if (widest > 0) header->setMinimumSectionSize(widest);

    // AND THE DIALOG WIDENS TO FIT THE TABLE. The floor above stops a header
    // being cut, but it cannot conjure room: at ui_font_size 28 the six
    // columns want more than the width the ACTION ROW asks for, so holding
    // Scope at its header width just moved the clipping onto Last used. A
    // table dialog has to be as wide as its table.
    //
    // Summed from sectionSize AND NOT sectionSizeHint. The hint looked like
    // the right call and is not: on a ResizeToContents section it reports the
    // header's MINIMUM, not the width the content needs. Measured at
    // ui_font_size 28, the Name column read hint=174 against an actual
    // sectionSize of 365, so a sum built from hints came out 1067 against a
    // table that wanted 1248 and the dialog never widened. sectionSize is the
    // width the section is actually using, and for a ResizeToContents column
    // that is content-derived and independent of the viewport, so reading it
    // here is not circular.
    //
    // Scope is the exception and contributes only its MINIMUM -- it is the
    // column chosen to elide, and letting its content into this sum is how a
    // credential scoped to ten CIDRs drags the dialog off the screen. So this
    // is "the width at which nothing except Scope is clipped", which is the
    // design. Scope IS the stretching section, so its sectionSize is the
    // answer being computed and reading it would be circular; the minimum is
    // not.
    int need = 0;
    for (int col = 0; col < table_->columnCount(); ++col) {
        need += (col == kScopeColumn) ? header->minimumSectionSize()
                                      : header->sectionSize(col);
    }
    // Room for the vertical scrollbar, which appears as soon as there are more
    // credentials than kVisibleRows and takes its width out of the viewport --
    // not out of the dialog. Without this the table starts scrolling
    // horizontally on the row that makes it scroll vertically.
    need += table_->verticalScrollBar()->sizeHint().width();
    table_->setMinimumWidth(need + 2 * table_->frameWidth());

    // fitToContent reads bodyWidget_->minimumSizeHint(), which the line above
    // has just changed, so this has to follow it.
    if (frame_) frame_->fitToContent();
}

void CredentialManagerDialog::showProblem(const QString &text) {
    if (!problemBox_) return;
    problem_->setText(text);
    const bool wasVisible = problemBox_->isVisible();
    problemBox_->setVisible(!text.isEmpty());
    if (frame_ && wasVisible != problemBox_->isVisible()) {
        frame_->fitToContent();
    }
}

void CredentialManagerDialog::clearProblem() { showProblem(QString()); }

void CredentialManagerDialog::refresh() {
    rows_.clear();
    table_->setRowCount(0);
    // Cleared before anything else. A complaint about the previous action
    // outliving it is how a stale message ends up describing a row that has
    // since been deleted.
    clearProblem();

    const bool locked = !vault_ || vault_->isLocked();
    const bool open = vault_ && vault_->isOpen();

    vaultPath_->setText(vault_ ? vault_->path() : tr("(none)"));

    // The chip carries the state and the tone carries how much it matters:
    // open is `state`, locked is `warn`, and a vault that does not exist yet
    // is neither -- it is the ordinary condition on a machine that has not
    // been set up.
    const char *tone = "true";
    QString stateText;
    if (!open) {
        stateText = tr("No vault handle");
        tone = "warn";
        // The only branch with a reason worth reading, and it is the
        // platform's own. Shown, not parsed.
        showProblem(tr("The vault at %1 could not be opened. %2")
                        .arg(vault_ ? vault_->path() : QString(),
                             vault_ ? vault_->lastError() : QString()));
    } else if (!vault_->exists()) {
        stateText = tr("No vault yet — Unlock creates one");
    } else if (locked) {
        stateText = tr("Locked");
        tone = "warn";
    } else {
        stateText = tr("Unlocked");
        tone = "state";
    }
    stateChip_->setText(stateText);
    stateChip_->setProperty("chip", QString::fromLatin1(tone));
    // The chip's tone changes with the vault's state, which is by definition
    // after the widget was polished -- so the selector matches and nothing
    // changes on screen without this. See the note on repolish() in
    // modalframe.h.
    repolish(stateChip_);

    QString defaultName;
    if (open && !locked) vault_->defaultName(&defaultName);
    defaultName_->setText(defaultName.isEmpty() ? tr("None") : defaultName);
    defaultName_->setProperty("tone", defaultName.isEmpty()
                                          ? QStringLiteral("hint")
                                          : QStringLiteral("ink"));
    repolish(defaultName_);

    unlock_->setEnabled(open && locked);
    add_->setEnabled(open && !locked);
    for (QPushButton *b : {edit_, rename_, delete_, default_, disable_}) {
        b->setEnabled(false);  // until a row is selected
    }

    if (locked) return;

    const VaultError code = vault_->list(&rows_);
    if (code != VaultError::Ok) {
        report(tr("List credentials"), code);
        return;
    }

    table_->setRowCount(rows_.size());
    for (int i = 0; i < rows_.size(); ++i) {
        const CredentialMeta &m = rows_.at(i);

        // The default is marked in the name cell rather than in a column of
        // its own: one row in this table is different from the others, and a
        // whole column of empty cells is a lot of table to say so.
        QString name = m.name;
        if (m.isDefault) name = tr("%1  (default)").arg(name);
        if (!m.hasSecret) name = tr("%1  (no material)").arg(name);

        const QString used = m.lastUsed.isValid()
                                 ? m.lastUsed.toLocalTime().toString(
                                       QStringLiteral("yyyy-MM-dd hh:mm"))
                                 : tr("never");

        const QStringList cells{name, m.username, m.authLabel, m.scope.summary(),
                                m.tags.join(QStringLiteral(", ")), used};
        for (int col = 0; col < cells.size(); ++col) {
            auto *item = new QTableWidgetItem(cells.at(col));
            if (m.disabled) {
                QFont f = item->font();
                f.setItalic(true);
                item->setFont(f);
                item->setToolTip(tr("Disabled: kept and reachable by name, but "
                                    "never chosen automatically."));
            }
            table_->setItem(i, col, item);
        }
    }

    // No resizeColumnsToContents() here: the header's resize modes do it, and
    // calling it as well would set the sections to Interactive and undo the
    // Stretch on Scope that stops the last column being clipped.

    // Select something. A table that opens with every action button greyed out
    // reads as a manager that cannot do anything, and the first row is the
    // default credential when there is one -- the row most likely to be wanted.
    if (table_->rowCount() > 0) table_->selectRow(0);
}

CredentialMeta CredentialManagerDialog::selected() const {
    const int row = table_->currentRow();
    if (row < 0 || row >= rows_.size()) return CredentialMeta();
    if (table_->selectedItems().isEmpty()) return CredentialMeta();
    return rows_.at(row);
}

void CredentialManagerDialog::report(const QString &action, VaultError code) {
    if (code == VaultError::Ok) return;
    showProblem(tr("%1: %2").arg(
        action,
        Vault::describe(code, vault_ ? vault_->lastError() : QString())));
}

void CredentialManagerDialog::unlock() {
    VaultUnlockDialog dialog(vault_, this);
    dialog.exec();
    refresh();
}

void CredentialManagerDialog::addCredential() {
    CredentialEditorDialog editor{CredentialMeta(), this};
    if (editor.exec() != QDialog::Accepted) return;

    CredentialInput input = editor.input();
    const VaultError code = vault_->store(input);
    input.wipeSecrets();
    // refresh() clears the notice, so the report has to follow it rather than
    // precede it -- the previous arrangement worked only because the report
    // was a QMessageBox and outlived the rebuild.
    refresh();
    report(tr("Add credential"), code);
}

void CredentialManagerDialog::editCredential() {
    const CredentialMeta current = selected();
    if (current.id.isEmpty()) return;

    CredentialEditorDialog editor(current, this);
    if (editor.exec() != QDialog::Accepted) return;

    VaultError code;
    if (editor.material()) {
        // The user supplied material, so the record is replaced with exactly
        // what the form holds. That is store()'s contract and the reason the
        // form made them ask for it.
        CredentialInput input = editor.input();
        code = vault_->store(input);
        input.wipeSecrets();
    } else {
        code = vault_->updateMetadata(editor.metadata());
    }
    refresh();
    report(tr("Edit credential"), code);
}

void CredentialManagerDialog::renameCredential() {
    const CredentialMeta current = selected();
    if (current.id.isEmpty()) return;

    bool ok = false;
    const QString name =
        QInputDialog::getText(this, tr("Rename credential"), tr("New name"),
                              QLineEdit::Normal, current.name, &ok);
    if (!ok || name.trimmed().isEmpty() || name == current.name) return;

    // Not the editor's job even though the editor can do it: rename is a
    // read-modify-write inside Go, so it is the one path that cannot lose a
    // secret no matter what the vault holds.
    const VaultError code = vault_->rename(current.id, name);
    refresh();
    report(tr("Rename credential"), code);
}

void CredentialManagerDialog::deleteCredential() {
    const CredentialMeta current = selected();
    if (current.id.isEmpty()) return;

    // STAYS A QMessageBox. This one is a question, and the answer decides
    // whether anything happens at all -- unlike report(), which describes
    // something that already did. An inline notice cannot take an answer.
    const auto answer = QMessageBox::question(
        this, tr("Delete credential"),
        tr("Delete \"%1\"?\n\nAny session that names this credential will stop "
           "resolving. There is no undo.")
            .arg(current.name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;

    const VaultError code = vault_->remove(current.id);
    refresh();
    report(tr("Delete credential"), code);
}

void CredentialManagerDialog::toggleDisabled() {
    const CredentialMeta current = selected();
    if (current.id.isEmpty()) return;
    const VaultError code = vault_->setDisabled(current.id, !current.disabled);
    refresh();
    report(tr("Change credential"), code);
}

void CredentialManagerDialog::makeDefault() {
    const CredentialMeta current = selected();
    if (current.id.isEmpty()) return;
    if (current.isDefault) {
        clearDefault();
        return;
    }
    const VaultError code = vault_->setDefault(current.id);
    refresh();
    report(tr("Set default"), code);
}

void CredentialManagerDialog::clearDefault() {
    const VaultError code = vault_->clearDefault();
    refresh();
    report(tr("Clear default"), code);
}

}  // namespace omega::app
