// app/credentialeditordialog.cpp

#include "app/credentialeditordialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QShowEvent>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

#include "app/modalframe.h"

namespace omega::app {
namespace {

constexpr int kDialogWidth = 600;

// Comma-separated in the form, a list on the wire. Tags and CIDRs are short
// and few; a list widget for three strings is more UI than the data deserves.
QStringList splitList(const QString &text) {
    QStringList out;
    for (const QString &part : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString trimmed = part.trimmed();
        if (!trimmed.isEmpty()) out << trimmed;
    }
    return out;
}

QString joinList(const QStringList &list) {
    return list.join(QStringLiteral(", "));
}

// Spacing, growth and label alignment for the four QFormLayouts here, spelled
// once. Four copies of the same four calls is four chances for one to drift.
//
// WHAT THIS DOES NOT DO IS ALIGN THE LABEL COLUMNS, and it cannot: each
// QFormLayout sizes its own column from its own longest label, and two of the
// four sit inside a QGroupBox that adds its own inset on top. Measured at
// ui_font_size 20, the field column starts at x=143 in the identity form, 199
// in the material box and 179 in the scope box. Making them agree would mean
// measuring the widest label across all four in showEvent -- constructor-time
// QFontMetrics is wrong here for the usual reason -- and then subtracting each
// group box's inset back off, which is deriving a layout from numbers instead
// of letting the layout produce it. Left ragged on purpose: the group boxes
// are visually separate containers, and their border is what the eye reads as
// the reason the indent changed.
void styleForm(QFormLayout *form) {
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(8);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
}

}  // namespace

using omegassh::AuthMethod;
using omegassh::CredentialInput;
using omegassh::CredentialMeta;

CredentialEditorDialog::CredentialEditorDialog(const CredentialMeta &existing,
                                               QWidget *parent)
    : QDialog(parent), existing_(existing), adding_(existing.id.isEmpty()) {
    buildUi();
}

void CredentialEditorDialog::buildUi() {
    // setWindowTitle happens inside the frame, so the title strip and the
    // window manager's bar cannot end up saying different things.
    frame_ = new ModalFrame(this,
                            adding_ ? tr("Add credential") : tr("Edit credential"),
                            kDialogWidth);

    // --- what is already stored -------------------------------------------
    // Editing only. Adding has no record behind it, and a well reading "none"
    // and "never" is furniture.
    //
    // The two facts here are the ones that appear NOWHERE ELSE in the dialog.
    // Name, username, method and every scope field are editable below, and
    // repeating an editable value in a read-only well is the mistake the host
    // key prompt still carries -- it states the hostname twice. What the form
    // genuinely cannot show is whether material is stored, because this dialog
    // never receives a secret and a blank password field therefore means
    // nothing on its own.
    if (!adding_) {
        QFrame *well = frame_->addWell();
        auto *wellForm = new QFormLayout(well);
        wellForm->setContentsMargins(14, 12, 14, 12);
        styleForm(wellForm);

        wellForm->addRow(
            fieldLabel(tr("Stored"), well),
            chipLabel(existing_.hasSecret
                          ? tr("Material stored — %1").arg(existing_.authLabel)
                          : tr("No material stored"),
                      well, existing_.hasSecret ? "state" : "warn"));

        // Never used is a fact, not a blank. A credential that has never
        // authenticated and one whose last use predates a re-key are the two
        // cases somebody opens this form to fix.
        wellForm->addRow(
            fieldLabel(tr("Last used"), well),
            monoLabel(existing_.lastUsed.isValid()
                          ? QLocale().toString(existing_.lastUsed.toLocalTime(),
                                               QLocale::ShortFormat)
                          : tr("Never"),
                      well, existing_.lastUsed.isValid() ? "ink" : "hint",
                      /*readout=*/true));
    }

    // --- the three pages --------------------------------------------------
    // TABS AND NOT STACKED GROUP BOXES. The stacked form ran to 953px at
    // ui_font_size 13 and 1304 at 28, which is past the height of a 1080p
    // screen once ModalFrame's 96px margin comes off -- so the dialog opened
    // scrolling, with the two flag checkboxes and the failure notice below the
    // fold. A modal that has to be scrolled to be read is a modal whose Save
    // button the reader has not seen.
    //
    // The split is by WHEN a field is touched, not by what type it is.
    // Details is filled in every time; Material is the deliberate act behind a
    // checkbox; Scope is optional and usually left alone. That ordering also
    // means the tab the dialog opens on is the one that always has work in it.
    //
    // QTabWidget's sizeHint is the maximum over its pages, so the dialog does
    // not resize as tabs are switched -- which is why the pages do not each
    // need fitToContent.
    tabs_ = new QTabWidget(frame_->bodyWidget());
    tabs_->setDocumentMode(true);

    // --- page 1: details --------------------------------------------------
    auto *details = new QWidget(tabs_);
    details->setProperty("bare", true);
    auto *detailsLayout = new QVBoxLayout(details);
    detailsLayout->setContentsMargins(14, 16, 14, 14);
    detailsLayout->setSpacing(12);

    auto *form = new QFormLayout;
    styleForm(form);

    name_ = new QLineEdit(existing_.name, details);
    name_->setPlaceholderText(tr("lab-admin"));
    form->addRow(fieldLabel(tr("Name"), details), name_);

    username_ = new QLineEdit(existing_.username, details);
    form->addRow(fieldLabel(tr("Username"), details), username_);

    description_ = new QLineEdit(existing_.description, details);
    form->addRow(fieldLabel(tr("Description"), details), description_);

    tags_ = new QLineEdit(joinList(existing_.tags), details);
    tags_->setPlaceholderText(tr("lab, access"));
    form->addRow(fieldLabel(tr("Tags"), details), tags_);

    priority_ = new QSpinBox(details);
    priority_->setRange(-100, 100);
    priority_->setValue(existing_.priority);
    priority_->setToolTip(tr("Higher wins when more than one credential fits a host."));
    form->addRow(fieldLabel(tr("Priority"), details), priority_);

    detailsLayout->addLayout(form);

    isDefault_ = new QCheckBox(tr("Use as the default credential"), details);
    isDefault_->setChecked(existing_.isDefault);
    detailsLayout->addWidget(isDefault_);

    // The widest unwrappable string in the dialog, and a checkbox label does
    // not wrap. fitToContent takes its width floor from
    // bodyWidget_->minimumSizeHint() precisely so this one does not get its
    // last third cut off at the right edge with horizontal scrolling disabled.
    disabled_ = new QCheckBox(
        tr("Disabled (kept, but never chosen automatically)"), details);
    disabled_->setChecked(existing_.disabled);
    detailsLayout->addWidget(disabled_);

    detailsLayout->addStretch();
    tabs_->addTab(details, tr("Details"));

    // --- page 2: material -------------------------------------------------
    auto *material = new QWidget(tabs_);
    material->setProperty("bare", true);
    auto *materialLayout = new QVBoxLayout(material);
    materialLayout->setContentsMargins(14, 16, 14, 14);
    materialLayout->setSpacing(12);

    auto *materialForm = new QFormLayout;
    styleForm(materialForm);

    replace_ = new QCheckBox(tr("Replace the stored material"), material);
    replace_->setToolTip(
        tr("Editing the fields above keeps whatever is stored. Tick this only "
           "to enter new material."));
    materialForm->addRow(QString(), replace_);

    auth_ = new QComboBox(material);
    for (const AuthMethod m : {AuthMethod::Password, AuthMethod::PublicKey,
                               AuthMethod::KeyboardInteractive, AuthMethod::Agent}) {
        auth_->addItem(omegassh::authLabel(m), static_cast<int>(m));
    }
    // BY DATA, NOT BY TEXT. findText is an exact string compare against the
    // item labels, so it makes the combo's starting position depend on two
    // separately-maintained tables -- Go's AuthMethod.String() and Qt's
    // authLabel() -- agreeing character for character. They did not: an agent
    // credential opened showing "Password", because Go says "Agent" and this
    // combo said "SSH Agent" and a missed match falls silently to index 0.
    // That is a wrong method armed and ready to be written the moment somebody
    // ticks Replace. authMethodFromLabel matches loosely and returns the enum
    // the item data already carries, so the lookup no longer cares what either
    // table calls it.
    const int at = auth_->findData(
        static_cast<int>(omegassh::authMethodFromLabel(existing_.authLabel)));
    auth_->setCurrentIndex(at >= 0 ? at : 0);
    materialForm->addRow(fieldLabel(tr("Method"), material), auth_);

    password_ = new QLineEdit(material);
    password_->setEchoMode(QLineEdit::Password);
    materialForm->addRow(fieldLabel(tr("Password"), material), password_);

    // A plain container used only for layout, so it opts out of the blanket
    // QWidget background rule.
    auto *keyRow = new QWidget(material);
    keyRow->setProperty("bare", true);
    auto *keyLayout = new QHBoxLayout(keyRow);
    keyLayout->setContentsMargins(0, 0, 0, 0);
    keyLayout->setSpacing(8);

    keyPath_ = new QLineEdit(keyRow);
    keyPath_->setPlaceholderText(tr("~/.ssh/id_ed25519"));
    keyPath_->setProperty("mono", true);
    keyLayout->addWidget(keyPath_);

    browse_ = new QPushButton(tr("Browse\u2026"), keyRow);
    // OFF, EXPLICITLY, and this is a live bug rather than tidying.
    // QPushButton turns autoDefault on for itself inside a QDialog, and this
    // button is not in the footer, so ModalFrame's pass over the footer
    // buttons -- which is what clears autoDefault across the set -- never
    // reaches it. Left alone it answers Return whenever it holds focus: the
    // probe on the previous version of this dialog reported
    // `button "Browse...": default=false autoDefault=true`, which is a file
    // chooser opening on Return from the key path field.
    browse_->setAutoDefault(false);
    keyLayout->addWidget(browse_);
    materialForm->addRow(fieldLabel(tr("Private key"), material), keyRow);

    passphrase_ = new QLineEdit(material);
    passphrase_->setEchoMode(QLineEdit::Password);
    materialForm->addRow(fieldLabel(tr("Key passphrase"), material),
                         passphrase_);

    materialLayout->addLayout(materialForm);
    materialLayout->addStretch();
    tabs_->addTab(material, tr("Material"));

    // --- page 3: scope ----------------------------------------------------
    auto *scope = new QWidget(tabs_);
    scope->setProperty("bare", true);
    auto *scopeLayout = new QVBoxLayout(scope);
    scopeLayout->setContentsMargins(14, 16, 14, 14);
    scopeLayout->setSpacing(12);

    scopeLayout->addWidget(descLabel(
        tr("Narrows which hosts this credential is offered for. Leave every "
           "field empty and it is offered for any host, which is what most "
           "lab credentials want."),
        scope));

    auto *scopeForm = new QFormLayout;
    styleForm(scopeForm);

    // Mono on all three: a domain suffix, a CIDR list and a platform list are
    // compared against something else character by character, and a
    // proportional face is where a 10.20.0.0/16 that should have been /24
    // hides.
    domainSuffix_ = new QLineEdit(existing_.scope.domainSuffix, scope);
    domainSuffix_->setPlaceholderText(QStringLiteral("lab.local"));
    domainSuffix_->setProperty("mono", true);
    scopeForm->addRow(fieldLabel(tr("Domain suffix"), scope), domainSuffix_);

    cidrs_ = new QLineEdit(joinList(existing_.scope.cidrs), scope);
    cidrs_->setPlaceholderText(QStringLiteral("10.20.0.0/16, 192.0.2.0/24"));
    cidrs_->setProperty("mono", true);
    scopeForm->addRow(fieldLabel(tr("CIDRs"), scope), cidrs_);

    platforms_ = new QLineEdit(joinList(existing_.scope.platforms), scope);
    platforms_->setPlaceholderText(QStringLiteral("ios, nxos"));
    platforms_->setProperty("mono", true);
    scopeForm->addRow(fieldLabel(tr("Platforms"), scope), platforms_);

    scopeLayout->addLayout(scopeForm);
    scopeLayout->addStretch();
    tabs_->addTab(scope, tr("Scope"));

    frame_->body()->addWidget(tabs_);

    // The hazard, and it is a real one: store() writes exactly what is in the
    // Material tab, so a field left alone is a field stored blank. It used to
    // live in the checkbox's tooltip, which is to say it was invisible until
    // somebody hovered a control they had already decided to tick.
    //
    // BELOW THE TABS AND NOT INSIDE THE MATERIAL PAGE. A warning about
    // something armed and destructive must not be hideable by clicking a
    // different tab -- the state persists across the switch, so the statement
    // of it has to as well.
    replaceNotice_ = frame_->addNotice(
        tr("Saving now stores exactly what is in the Material tab. Any field "
           "left blank is stored blank, replacing what this credential had."));
    replaceNotice_->hide();

    // --- what went wrong, when something does -----------------------------
    // After the fields, for the same reason as the vault unlock: it is a
    // response to what was typed, and a message that appears above the input
    // it is about reads as a standing warning instead of an answer.
    problemBox_ = frame_->addNotice(QString());
    problem_ = problemBox_->findChild<QLabel *>();
    problemBox_->hide();

    frame_->body()->addStretch();

    // --- the decision -----------------------------------------------------
    // Primary on Save. This dialog asks a question rather than guarding an
    // irreversible action -- the destructive case, storing over material, is
    // opt-in behind a checkbox and stated in the body while it is armed -- so
    // the affirmative and the recommended answer coincide, as they do on the
    // credential prompt and the vault unlock and unlike the host key prompt.
    QPushButton *cancel = frame_->addButton(tr("Cancel"), ModalFrame::Secondary);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);

    save_ = frame_->addButton(tr("Save"), ModalFrame::Primary);
    connect(save_, &QPushButton::clicked, this, [this] {
        if (validate()) accept();
    });

    frame_->setFooterHint(tr("Return saves."));

    connect(browse_, &QPushButton::clicked, this,
            &CredentialEditorDialog::browseForKey);
    connect(replace_, &QCheckBox::toggled, this,
            &CredentialEditorDialog::onReplaceToggled);
    connect(auth_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { onAuthChanged(); });

    // Typing is the answer to the complaint, so the complaint goes as soon as
    // typing starts. A red border under a field somebody is actively
    // correcting asserts the new text is wrong, which is not yet known.
    connect(name_, &QLineEdit::textEdited, this,
            [this](const QString &) { clearProblem(); });
    // The password field clears the blank-password consent as well as the
    // notice. A warning given about an empty field is not consent to store
    // whatever is typed next, and it is not consent to clear the material
    // again after the field is emptied a second time.
    connect(password_, &QLineEdit::textEdited, this, [this](const QString &) {
        blankPasswordWarned_ = false;
        clearProblem();
    });

    // Adding always supplies material and has nothing to preserve, so the
    // choice does not exist and the box is not offered.
    replace_->setVisible(!adding_);
    replace_->setChecked(adding_);
    onReplaceToggled(adding_);

    name_->setFocus();
}

void CredentialEditorDialog::onReplaceToggled(bool on) {
    auth_->setEnabled(on);
    password_->setEnabled(on);
    keyPath_->setEnabled(on);
    passphrase_->setEnabled(on);
    if (!on) {
        // Cleared as well as disabled. A greyed-out field holding something
        // the user typed before changing their mind is a promise that it will
        // be saved, and it will not be.
        password_->clear();
        keyPath_->clear();
        passphrase_->clear();
        auth_->setToolTip(tr("The method moves with the material. Tick "
                             "\"Replace the stored material\" to change it."));
    } else {
        auth_->setToolTip(QString());
    }
    // The material on screen just changed wholesale, so any consent given
    // about the previous contents is spent.
    blankPasswordWarned_ = false;
    onAuthChanged();
    updateReplaceNotice();
}

void CredentialEditorDialog::onAuthChanged() {
    const auto method =
        static_cast<AuthMethod>(auth_->currentData().toInt());
    const bool on = replace_->isChecked();
    password_->setEnabled(on && method == AuthMethod::Password);
    keyPath_->setEnabled(on && method == AuthMethod::PublicKey);
    browse_->setEnabled(on && method == AuthMethod::PublicKey);
    passphrase_->setEnabled(on && method == AuthMethod::PublicKey);
    // Switching method changes which field the blank-material question is
    // even about, so a previous answer to it does not carry.
    blankPasswordWarned_ = false;
}

void CredentialEditorDialog::updateReplaceNotice() {
    if (!replaceNotice_) return;
    // Adding stores into nothing, so there is nothing to lose and no hazard to
    // state; a record with no material stored has nothing to clear either.
    const bool want = !adding_ && existing_.hasSecret && replace_->isChecked();
    if (replaceNotice_->isVisible() == want) return;
    replaceNotice_->setVisible(want);
    // The body got taller or shorter. Only on an actual change of visibility,
    // or the window twitches every time the method combo is arrowed through.
    if (frame_) frame_->fitToContent();
}

void CredentialEditorDialog::browseForKey() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Private key"));
    if (!path.isEmpty()) keyPath_->setText(path);
}

void CredentialEditorDialog::markInvalid(QWidget *field, bool invalid) {
    if (!field) return;
    if (field->property("invalid").toBool() == invalid) return;
    field->setProperty("invalid", invalid);
    // Without this the selector matches and nothing changes on screen. See
    // the note on repolish() in modalframe.h.
    repolish(field);
}

void CredentialEditorDialog::showProblem(const QString &text, QWidget *field,
                                         QWidget *reveal) {
    if (invalidField_ && invalidField_ != field) {
        markInvalid(invalidField_, false);
    }
    invalidField_ = field;
    markInvalid(field, true);

    // Before the notice is shown, so the page change and the message land in
    // the same repaint rather than as two steps.
    revealField(reveal ? reveal : field);

    problem_->setText(text);
    const bool wasVisible = problemBox_->isVisible();
    problemBox_->setVisible(!text.isEmpty());
    // The notice appeared or went, so the dialog has to make room. Only on a
    // change of visibility: replacing one message with another of the same
    // shape does not move anything.
    if (frame_ && wasVisible != problemBox_->isVisible()) {
        frame_->fitToContent();
    }
}

void CredentialEditorDialog::clearProblem() {
    if (!problemBox_ || !problemBox_->isVisible()) return;
    markInvalid(invalidField_, false);
    invalidField_ = nullptr;
    problem_->clear();
    problemBox_->hide();
    if (frame_) frame_->fitToContent();
}

bool CredentialEditorDialog::validate() {
    if (name_->text().trimmed().isEmpty()) {
        showProblem(tr("A credential needs a name."), name_);
        name_->setFocus();
        return false;
    }

    // Not an error, and not silent either. A password credential saved with an
    // empty password is a legitimate thing to want -- it is how material gets
    // cleared -- but it is also exactly what the accidental case looks like,
    // so it gets said out loud once and accepted on the second press.
    //
    // NO FIELD IS MARKED. The empty password is not a mistake to correct, it
    // is the thing being confirmed, and a red border would say otherwise.
    // Same rule as the stale-keyring case on the vault unlock.
    if (replace_->isChecked() &&
        static_cast<AuthMethod>(auth_->currentData().toInt()) == AuthMethod::Password &&
        password_->text().isEmpty() && !blankPasswordWarned_) {
        blankPasswordWarned_ = true;
        // No field marked, Material tab revealed. The empty password is not a
        // mistake to correct, it is the thing being confirmed -- but the
        // reader still has to be able to see the field the sentence is about.
        showProblem(tr("No password entered. Saving now stores this "
                       "credential with no password, clearing anything "
                       "it had. Press Save again to do that."),
                    nullptr, password_);
        return false;
    }
    return true;
}

void CredentialEditorDialog::revealField(QWidget *w) {
    if (!tabs_ || !w) return;
    for (QWidget *p = w; p; p = p->parentWidget()) {
        const int index = tabs_->indexOf(p);
        if (index >= 0) {
            tabs_->setCurrentIndex(index);
            return;
        }
    }
}

void CredentialEditorDialog::showEvent(QShowEvent *event) {
    QDialog::showEvent(event);
    if (labelsAligned_) return;
    labelsAligned_ = true;

    // After polish, so sizeHint() is measured in the face the labels are
    // actually drawn in rather than in the application font.
    ensurePolished();
    if (layout()) layout()->activate();
    // The well's labels are deliberately outside this: the well has its own
    // inset and sits above the tabs, so aligning it to them would be aligning
    // two things the eye never compares.
    omega::app::alignFieldLabels(tabs_);
    if (frame_) frame_->fitToContent();
}

bool CredentialEditorDialog::material() const {
    return replace_->isChecked();
}

CredentialMeta CredentialEditorDialog::metadata() const {
    CredentialMeta m = existing_;  // keeps id, auth label, has_secret, last used
    m.name = name_->text().trimmed();
    m.username = username_->text().trimmed();
    m.description = description_->text();
    m.priority = priority_->value();
    m.tags = splitList(tags_->text());
    m.scope.domainSuffix = domainSuffix_->text().trimmed();
    m.scope.cidrs = splitList(cidrs_->text());
    m.scope.platforms = splitList(platforms_->text());
    m.isDefault = isDefault_->isChecked();
    m.disabled = disabled_->isChecked();
    return m;
}

CredentialInput CredentialEditorDialog::input() const {
    CredentialInput c;
    c.id = existing_.id;  // empty when adding, which is what makes it an add
    c.name = name_->text().trimmed();
    c.username = username_->text().trimmed();
    c.auth = static_cast<AuthMethod>(auth_->currentData().toInt());
    c.password = password_->text();
    c.keyPath = keyPath_->text().trimmed();
    c.keyPassphrase = passphrase_->text();
    c.description = description_->text();
    c.priority = priority_->value();
    c.tags = splitList(tags_->text());
    c.scope.domainSuffix = domainSuffix_->text().trimmed();
    c.scope.cidrs = splitList(cidrs_->text());
    c.scope.platforms = splitList(platforms_->text());
    c.isDefault = isDefault_->isChecked();
    c.disabled = disabled_->isChecked();
    return c;
}

}  // namespace omega::app
