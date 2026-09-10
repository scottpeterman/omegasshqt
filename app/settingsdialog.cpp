// app/settingsdialog.cpp

#include "app/settingsdialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QShowEvent>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

#include "app/modalframe.h"
#include "theme/theme.h"

namespace omega::app {
namespace {

// "enterprise_dark" reads as "Enterprise Dark". The same transform the View
// menu applies, so one theme is named one way in both places.
QString titleCase(const QString &slug) {
    QString out = slug;
    out.replace(QLatin1Char('_'), QLatin1Char(' '));
    bool atStart = true;
    for (QChar &c : out) {
        if (atStart) c = c.toUpper();
        atStart = c.isSpace() || c == QLatin1Char('-');
    }
    return out;
}

// What a terminal declares itself as: the pty request on ssh, the answer to a
// TTYPE subnegotiation on telnet. Editable rather than fixed, because the list
// of things a device might want is not one anybody can finish -- but these
// four cover nearly all of it, and typing a term type from memory is how a
// session comes up with no colour for a reason nobody finds.
const char *const kTermTypes[] = {"xterm-256color", "xterm", "vt100", "ansi"};

constexpr int kDialogWidth = 600;

// The four forms, spelled once. See alignFieldLabels() in modalframe.h for the
// part this deliberately does NOT do -- making the four label columns agree,
// which needs a measurement and therefore happens in showEvent.
void styleForm(QFormLayout *form) {
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(8);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
}

// One tab page: a bare container with the page margins already on it. Every
// page in this dialog is a form plus some prose, so the shape is the same four
// lines four times over.
QWidget *makePage(QTabWidget *tabs, QVBoxLayout **layoutOut) {
    auto *page = new QWidget(tabs);
    page->setProperty("bare", true);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(14, 16, 14, 14);
    layout->setSpacing(12);
    *layoutOut = layout;
    return page;
}

}  // namespace

SettingsDialog::SettingsDialog(const AppSettings &shared,
                               const OmegaSettings &omega,
                               theme::ThemeEngine *themes, QWidget *parent)
    : QDialog(parent), shared_(shared), omega_(omega) {
    // setWindowTitle and setModal both happen inside ModalFrame, so the title
    // strip and the window manager's bar cannot end up saying different things.
    buildUi(themes);
    loadFrom(shared, omega);
    updateEnabledState();
    updateHexFeedback();
}

void SettingsDialog::buildUi(theme::ThemeEngine *themes) {
    frame_ = new ModalFrame(this, tr("Settings"), kDialogWidth);

    // TABS AND NOT STACKED GROUP BOXES, for the reason the credential editor
    // was tabbed: the stacked form measured 548x814 at ui_font_size 13, and
    // ui_font_size runs to 28 -- from a dialog whose own first field is the
    // control that sets it. A settings dialog that cannot show its Save button
    // after you raise the font is a settings dialog you cannot get out of by
    // the route you came in.
    //
    // One tab per group box, in the order the groups were already in. That
    // ordering was not arbitrary -- Appearance first because it is what people
    // open this for -- and tabbing is not the moment to relitigate it.
    tabs_ = new QTabWidget(frame_->bodyWidget());
    tabs_->setDocumentMode(true);

    QVBoxLayout *pageLayout = nullptr;

    // --- appearance --------------------------------------------------------
    QWidget *appearance = makePage(tabs_, &pageLayout);
    auto *appearanceForm = new QFormLayout;
    styleForm(appearanceForm);

    theme_ = new QComboBox(appearance);
    if (themes) {
        for (const std::string &name : themes->names()) {
            const QString slug = QString::fromStdString(name);
            theme_->addItem(titleCase(slug), slug);
        }
    }
    // currentIndexChanged rather than activated: the arrow keys move the
    // selection too, and a preview that only fired on a mouse click would
    // make keyboard browsing show nothing.
    connect(theme_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0) return;
        emit themePreviewRequested(theme_->itemData(index).toString());
    });
    appearanceForm->addRow(fieldLabel(tr("Theme"), appearance), theme_);

    chrome_ = new QComboBox(appearance);
    for (const Chrome c : {Chrome::Token, Chrome::Classic}) {
        chrome_->addItem(chromeLabel(c), static_cast<int>(c));
    }
    chrome_->setToolTip(
        tr("Which stylesheet paints the window. Omega is the current\n"
           "design; Classic is the sheet shared with nterm-qt, kept so the\n"
           "old chrome stays reachable.\n\n"
           "Unlike the theme above, this one applies when the dialog is\n"
           "accepted rather than as you browse: the two sheets style\n"
           "different widgets, and swapping them under an open dialog\n"
           "relayouts the dialog you are reading."));
    appearanceForm->addRow(fieldLabel(tr("Chrome"), appearance), chrome_);

    titleBar_ = new QComboBox(appearance);
    for (const TitleBar b : {TitleBar::Merged, TitleBar::Native}) {
        titleBar_->addItem(titleBarLabel(b), static_cast<int>(b));
    }
    titleBar_->setToolTip(
        tr("Merged puts the menus, the vault pill and the window buttons in\n"
           "one strip and drops the window manager's title bar.\n\n"
           "Forced to the system bar while Chrome is Classic: the merged bar\n"
           "is styled by selectors only the Omega sheet carries.\n\n"
           "Takes effect on the next launch -- the menu bar is installed\n"
           "differently on the two paths and cannot be swapped underneath a\n"
           "running window."));
    appearanceForm->addRow(fieldLabel(tr("Title bar"), appearance), titleBar_);

    uiFontSize_ = new QSpinBox(appearance);
    uiFontSize_->setRange(9, 28);
    uiFontSize_->setSuffix(tr(" px"));
    uiFontSize_->setToolTip(
        tr("Body text size for the menus, session tree, dialogs and status\n"
           "bar. Not the terminal -- that is \"Terminal font size\" below.\n"
           "\n"
           "In pixels rather than points because the stylesheet is in pixels,\n"
           "and asking for the same number in two units puts the chrome and\n"
           "the menus a fraction apart at any DPI other than 96.\n"
           "\n"
           "Applies immediately."));
    appearanceForm->addRow(fieldLabel(tr("Interface font size"), appearance),
                           uiFontSize_);

    fontSize_ = new QSpinBox(appearance);
    fontSize_->setRange(6, 48);
    fontSize_->setSuffix(tr(" pt"));
    fontSize_->setToolTip(
        tr("The emulator's font, in points -- config.json's font_size, which\n"
           "nterm-qt reads too. Separate from the interface size above: a\n"
           "terminal is read at a different distance from a menu."));
    appearanceForm->addRow(fieldLabel(tr("Terminal font size"), appearance),
                           fontSize_);

    pageLayout->addLayout(appearanceForm);
    pageLayout->addStretch();
    tabs_->addTab(appearance, tr("Appearance"));

    // --- terminal ----------------------------------------------------------
    QWidget *terminal = makePage(tabs_, &pageLayout);
    auto *terminalForm = new QFormLayout;
    styleForm(terminalForm);

    scrollback_ = new QSpinBox(terminal);
    scrollback_->setRange(0, 1000000);
    scrollback_->setSingleStep(1000);
    scrollback_->setToolTip(
        tr("Rows kept above the screen. Applies to open tabs immediately;\n"
           "lowering it discards history those tabs are already holding."));
    terminalForm->addRow(fieldLabel(tr("Scrollback (lines)"), terminal),
                         scrollback_);

    pasteThreshold_ = new QSpinBox(terminal);
    // 0 reads as "never ask", which is a real choice and not a disabled
    // control -- so it gets a word rather than a zero.
    pasteThreshold_->setRange(0, 1000);
    pasteThreshold_->setSpecialValueText(tr("Never ask"));
    pasteThreshold_->setToolTip(
        tr("Confirm before pasting more lines than this. The default of 1\n"
           "asks about every multi-line paste, which is deliberate for\n"
           "network gear: two lines into a device in config mode is how a\n"
           "change goes in half-applied."));
    // Unit in the label rather than as a suffix: a spin box suffix cannot
    // agree with its number, and the default value of 1 rendered "1 lines".
    terminalForm->addRow(
        fieldLabel(tr("Confirm paste above (lines)"), terminal),
        pasteThreshold_);

    termType_ = new QComboBox(terminal);
    termType_->setEditable(true);
    for (const char *type : kTermTypes) {
        termType_->addItem(QString::fromLatin1(type));
    }
    termType_->setToolTip(
        tr("Declared to the far end on ssh and telnet. Serial has nowhere to\n"
           "declare it, so this does not reach a console port."));
    terminalForm->addRow(fieldLabel(tr("Terminal type"), terminal), termType_);

    pageLayout->addLayout(terminalForm);

    wheelAltScreen_ = new QCheckBox(
        tr("Wheel scrolls the application on a full screen"), terminal);
    wheelAltScreen_->setToolTip(
        tr("Inside a pager or a full-screen application -- vi, less, a\n"
           "device's own pager over a Linux host -- a wheel notch sends\n"
           "arrow keys to that application. Off scrolls the local history\n"
           "behind it instead, which is what a terminal does by default and\n"
           "is rarely what was wanted: the alternate screen keeps its own\n"
           "buffer, so the history being scrolled is from before the\n"
           "application started.\n\n"
           "At an ordinary prompt the wheel scrolls scrollback either way.\n"
           "Overridable per session in the session editor."));
    pageLayout->addWidget(wheelAltScreen_);

    pageLayout->addStretch();
    tabs_->addTab(terminal, tr("Terminal"));

    // --- connection --------------------------------------------------------
    QWidget *connection = makePage(tabs_, &pageLayout);
    auto *connectionForm = new QFormLayout;
    styleForm(connectionForm);

    sshDefaultAuth_ = new QComboBox(connection);
    for (const SshDefaultAuth mode :
         {SshDefaultAuth::VaultDefault, SshDefaultAuth::Ask,
          SshDefaultAuth::Agent}) {
        sshDefaultAuth_->addItem(sshDefaultAuthLabel(mode),
                                 static_cast<int>(mode));
    }
    sshDefaultAuth_->setToolTip(
        tr("Applies to SSH sessions that name no credential of their own,\n"
           "which is every session imported from TerminalTelemetry. A session\n"
           "that names one is unaffected.\n\n"
           "The vault's default credential falls back to asking when the\n"
           "vault holds no default."));
    // The label is a sentence, and a sentence in a QFormLayout label column
    // makes that column as wide as the sentence -- which alignFieldLabels then
    // propagates to every other page. Above the field instead, where it can be
    // as long as it needs to be and costs no other page anything.
    pageLayout->addWidget(
        fieldLabel(tr("When a session names no credential"), connection));
    connectionForm->addRow(QString(), sshDefaultAuth_);
    pageLayout->addLayout(connectionForm);

    // descLabel rather than a QLabel with setEnabled(false). Disabling a label
    // to grey it also removes it from what a screen reader reports as live
    // content, and the sheet has a role that means "secondary explanation"
    // already.
    pageLayout->addWidget(descLabel(
        tr("Telnet and serial are unaffected: neither has an authentication "
           "step."),
        connection));
    pageLayout->addStretch();
    tabs_->addTab(connection, tr("Connection"));

    // --- anti-idle ---------------------------------------------------------
    QWidget *idle = makePage(tabs_, &pageLayout);

    antiIdleEnabled_ = new QCheckBox(
        tr("Send a keystroke after a period with nothing typed"), idle);
    antiIdleEnabled_->setToolTip(
        tr("Stops a device's own idle timer closing the session. Distinct\n"
           "from keepalive: exec-timeout and its equivalents count terminal\n"
           "input, not packets, so no keepalive at any layer resets one."));
    connect(antiIdleEnabled_, &QCheckBox::toggled, this,
            &SettingsDialog::updateEnabledState);
    pageLayout->addWidget(antiIdleEnabled_);

    auto *idleForm = new QFormLayout;
    styleForm(idleForm);
    antiIdleForm_ = idleForm;

    antiIdleSeconds_ = new QSpinBox(idle);
    antiIdleSeconds_->setRange(5, 3600);
    idleForm->addRow(fieldLabel(tr("After (seconds)"), idle), antiIdleSeconds_);

    antiIdleKeystroke_ = new QComboBox(idle);
    antiIdleKeystroke_->addItem(tr("Backspace"),
                                static_cast<int>(AntiIdleKeystroke::Backspace));
    antiIdleKeystroke_->addItem(
        tr("Space then backspace"),
        static_cast<int>(AntiIdleKeystroke::SpaceBackspace));
    antiIdleKeystroke_->addItem(tr("NUL"),
                                static_cast<int>(AntiIdleKeystroke::Nul));
    antiIdleKeystroke_->addItem(tr("Custom\u2026"),
                                static_cast<int>(AntiIdleKeystroke::Custom));
    antiIdleKeystroke_->setToolTip(
        tr("Backspace is the convention and a no-op at an idle prompt. It is\n"
           "not one on a half-typed line, where it deletes the last character\n"
           "somebody left there; space-then-backspace is net zero on both."));
    connect(antiIdleKeystroke_, &QComboBox::currentIndexChanged, this, [this] {
        updateEnabledState();
        updateHexFeedback();
    });
    idleForm->addRow(fieldLabel(tr("Send"), idle), antiIdleKeystroke_);

    antiIdleHex_ = new QLineEdit(idle);
    antiIdleHex_->setPlaceholderText(tr("hex, e.g. 0d for a bare CR"));
    antiIdleHex_->setProperty("mono", true);
    // Hex only, and an even count is checked on the way out rather than here:
    // a validator refusing an odd length would refuse every first digit typed.
    antiIdleHex_->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[0-9a-fA-F]*")), antiIdleHex_));
    connect(antiIdleHex_, &QLineEdit::textChanged, this,
            &SettingsDialog::updateHexFeedback);
    idleForm->addRow(fieldLabel(tr("Bytes"), idle), antiIdleHex_);

    pageLayout->addLayout(idleForm);

    // The success case: what a valid string will send. Secondary information,
    // so a desc label.
    antiIdleHexNote_ = descLabel(QString(), idle);
    pageLayout->addWidget(antiIdleHexNote_);

    pageLayout->addWidget(descLabel(
        tr("Suppressed while a full-screen application is up and while a "
           "paced paste is running."),
        idle));
    pageLayout->addStretch();
    tabs_->addTab(idle, tr("Anti-idle"));

    frame_->body()->addWidget(tabs_);

    // The failure case, and it is genuinely one: a custom keystroke that
    // silently sends nothing is anti-idle configured and not running. It used
    // to share antiIdleHexNote_, which meant "anti-idle will not fire"
    // rendered in the same dim grey as "2 bytes will be sent".
    //
    // BELOW THE TABS, like the credential editor's hazard: the condition
    // survives a tab switch, so the statement of it has to as well.
    hexProblem_ = frame_->addNotice(QString());
    hexProblemText_ = hexProblem_->findChild<QLabel *>();
    hexProblem_->hide();

    frame_->body()->addStretch();

    // Save rather than OK. Every other dialog in this pass names the verb it
    // performs, and "OK" on a form that writes two config files says nothing
    // about which of the two buttons keeps the changes.
    QPushButton *cancel = frame_->addButton(tr("Cancel"), ModalFrame::Secondary);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);

    QPushButton *save = frame_->addButton(tr("Save"), ModalFrame::Primary);
    connect(save, &QPushButton::clicked, this, &QDialog::accept);

    frame_->setFooterHint(tr("Return saves. The theme previews as you browse."));
}

void SettingsDialog::showEvent(QShowEvent *event) {
    QDialog::showEvent(event);
    if (labelsAligned_) return;
    labelsAligned_ = true;

    ensurePolished();
    if (layout()) layout()->activate();
    alignFieldLabels(tabs_);
    if (frame_) frame_->fitToContent();
}

void SettingsDialog::loadFrom(const AppSettings &shared,
                              const OmegaSettings &omega) {
    // Blocked, or populating the combo would fire a preview for a theme
    // nobody chose -- and on a rejected dialog the window would then be asked
    // to revert to a theme it was already on.
    const QSignalBlocker blockTheme(theme_);
    const int themeIndex = theme_->findData(shared.theme_name);
    if (themeIndex >= 0) {
        theme_->setCurrentIndex(themeIndex);
    } else if (!shared.theme_name.isEmpty()) {
        // A config.json naming a theme that is not installed. Shown rather
        // than silently corrected: nterm-qt's own default names a theme that
        // ships in neither application, so this is the ordinary case on a
        // config that application wrote.
        theme_->addItem(tr("%1 (not installed)").arg(shared.theme_name),
                        shared.theme_name);
        theme_->setCurrentIndex(theme_->count() - 1);
    }

    const int chromeIndex = chrome_->findData(static_cast<int>(omega.chrome));
    chrome_->setCurrentIndex(chromeIndex >= 0 ? chromeIndex : 0);

    const int barIndex = titleBar_->findData(static_cast<int>(omega.title_bar));
    titleBar_->setCurrentIndex(barIndex >= 0 ? barIndex : 0);

    uiFontSize_->setValue(qBound(9, omega.ui_font_size, 28));
    fontSize_->setValue(shared.font_size);
    scrollback_->setValue(shared.scrollback_lines);
    pasteThreshold_->setValue(shared.multiline_paste_threshold);

    const int termIndex = termType_->findText(shared.default_term_type);
    if (termIndex >= 0) {
        termType_->setCurrentIndex(termIndex);
    } else {
        termType_->setCurrentText(shared.default_term_type);
    }

    const int authIndex =
        sshDefaultAuth_->findData(static_cast<int>(omega.ssh_default_auth));
    sshDefaultAuth_->setCurrentIndex(authIndex >= 0 ? authIndex : 0);

    antiIdleEnabled_->setChecked(omega.anti_idle.enabled);
    wheelAltScreen_->setChecked(omega.wheel_alt_screen);
    antiIdleSeconds_->setValue(qBound(5, omega.anti_idle.seconds, 3600));

    const int keyIndex = antiIdleKeystroke_->findData(
        static_cast<int>(omega.anti_idle.keystroke));
    antiIdleKeystroke_->setCurrentIndex(keyIndex >= 0 ? keyIndex : 0);
    antiIdleHex_->setText(bytesToHex(omega.anti_idle.custom));
}

void SettingsDialog::updateEnabledState() {
    const bool on = antiIdleEnabled_->isChecked();
    const bool custom =
        antiIdleKeystroke_->currentData().toInt() ==
        static_cast<int>(AntiIdleKeystroke::Custom);

    // Field and label together. The themes style :disabled subtly -- which is
    // right for a form where one control is unavailable, and not enough on
    // its own for three rows that are switched off as a group.
    const auto setRow = [this](QWidget *field, bool enabled) {
        field->setEnabled(enabled);
        if (QWidget *label = antiIdleForm_->labelForField(field)) {
            label->setEnabled(enabled);
        }
    };

    setRow(antiIdleSeconds_, on);
    setRow(antiIdleKeystroke_, on);
    setRow(antiIdleHex_, on && custom);
    // Visibility is no longer toggled here. updateHexFeedback() clears the
    // text when the row is irrelevant, and an empty desc label occupies no
    // height -- so the note and the notice are driven from ONE place instead
    // of this function hiding a label the other function had just filled in.
    antiIdleHexNote_->setVisible(on && custom);
}

void SettingsDialog::updateHexFeedback() {
    // Asks the same question updateEnabledState() asked rather than reading
    // isVisible(): a widget in a dialog that has not been shown yet reports
    // false, so building the form with a custom keystroke already configured
    // produced an empty note until something else nudged it.
    const bool custom = antiIdleKeystroke_->currentData().toInt() ==
                        static_cast<int>(AntiIdleKeystroke::Custom);
    const bool relevant = antiIdleEnabled_->isChecked() && custom;

    // The two labels are driven together and always cleared first, so a
    // problem from a string that has since been fixed cannot survive under a
    // note that now says the opposite.
    const auto setProblem = [this](const QString &text) {
        if (!hexProblem_) return;
        hexProblemText_->setText(text);
        const bool wasVisible = hexProblem_->isVisible();
        hexProblem_->setVisible(!text.isEmpty());
        // Only on an actual change of visibility, or the window twitches on
        // every keystroke in the hex field.
        if (frame_ && wasVisible != hexProblem_->isVisible()) {
            frame_->fitToContent();
        }
    };

    if (!relevant) {
        // Not an error, just not applicable. Both go quiet rather than the
        // notice staying up over a control nobody can reach.
        antiIdleHexNote_->clear();
        setProblem(QString());
        return;
    }

    const QString text = antiIdleHex_->text().trimmed();
    if (text.isEmpty()) {
        antiIdleHexNote_->clear();
        setProblem(tr("Nothing to send — anti-idle is on but will not fire."));
        return;
    }
    const QByteArray bytes = hexToBytes(text);
    if (bytes.isEmpty()) {
        antiIdleHexNote_->clear();
        setProblem(tr("Not a whole number of bytes — anti-idle is on but will "
                      "not fire. Hex takes two characters per byte."));
        return;
    }
    setProblem(QString());
    // Spelled out rather than tr()'s %n form: with no translator loaded Qt
    // cannot pluralise English, so "%n byte(s)" reaches the screen verbatim.
    antiIdleHexNote_->setText(bytes.size() == 1
                                  ? tr("1 byte will be sent.")
                                  : tr("%1 bytes will be sent.")
                                        .arg(bytes.size()));
}

AppSettings SettingsDialog::appSettings() const {
    // Copied from what was read rather than default-constructed, so the ten
    // fields this form does not show -- window state, the recent list,
    // keepalive, auto_reconnect -- go back exactly as they came.
    AppSettings out = shared_;
    out.theme_name = theme_->currentData().toString();
    out.font_size = fontSize_->value();
    out.scrollback_lines = scrollback_->value();
    out.multiline_paste_threshold = pasteThreshold_->value();

    const QString term = termType_->currentText().trimmed();
    if (!term.isEmpty()) {
        out.default_term_type = term;
    }
    return out;
}

OmegaSettings SettingsDialog::omegaSettings() const {
    OmegaSettings out = omega_;
    out.chrome = static_cast<Chrome>(chrome_->currentData().toInt());
    out.title_bar = static_cast<TitleBar>(titleBar_->currentData().toInt());
    out.ui_font_size = uiFontSize_->value();
    out.ssh_default_auth =
        static_cast<SshDefaultAuth>(sshDefaultAuth_->currentData().toInt());
    out.anti_idle.enabled = antiIdleEnabled_->isChecked();
    out.wheel_alt_screen = wheelAltScreen_->isChecked();
    out.anti_idle.seconds = antiIdleSeconds_->value();
    out.anti_idle.keystroke = static_cast<AntiIdleKeystroke>(
        antiIdleKeystroke_->currentData().toInt());
    out.anti_idle.custom = hexToBytes(antiIdleHex_->text());
    return out;
}

}  // namespace omega::app