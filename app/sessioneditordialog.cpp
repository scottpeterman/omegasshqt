// app/sessioneditordialog.cpp

#include "app/sessioneditordialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include "app/modalframe.h"
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QButtonGroup>
#include <QFrame>
#include <QHBoxLayout>
#include <QShowEvent>
#include <QTabWidget>
#include <QVBoxLayout>

#include <functional>

#include <omegasshsession.h>

#include "app/antiidle.h"
#include "app/effectiveconfig.h"

namespace omega::app {
namespace {

QString toQ(const std::string &s) { return QString::fromStdString(s); }

// The credential combo's first row is an INHERIT row, the same idiom as the
// three below. It was two rows, "(SSH agent)" and "(Ask on connect)", which
// wrote an identical NULL and differed only in what they claimed would
// happen -- and neither claim was true, since nothing downstream turned an
// agent on or prompted for anything. One row now, labelled with what the
// global actually does. See omegasettings.h.

// The same list quick connect offers. Duplicated rather than shared: the two
// dialogs are independent, and a header existing only to hold six integers
// would be a worse trade than the six integers.
const int kBauds[] = {9600, 19200, 38400, 57600, 115200, 230400};

// --- the three inherit idioms ---------------------------------------------
//
// Each keeps "absent" distinct from every value the field can hold, because
// absent is what makes a session keep following the global instead of pinning
// whatever it happened to be on the day the session was saved.

// A number that can be absent: one step below the minimum, showing a label
// instead of a digit. QSpinBox renders specialValueText only at the minimum,
// which is exactly the one state that needs to read as words.
//
// STEPPING OFF INHERIT LANDS ON THE INHERITED VALUE, not on the range floor.
// Plain QSpinBox stepping from the special value goes to lo, so pressing up
// once on a font row reading "(inherit -- 14 pt)" gave 6 pt -- the terminal
// jumping from readable to unreadable on a single click, with 8 more clicks
// needed to get back to where it started. Every one of these rows had the same
// shape: "(inherit -- 10000)" scrollback stepped to 100.
//
// The value being inherited is the only sensible first stop, because pinning
// is nearly always "the global, but a bit different for this box". From there
// the ordinary steps do what they look like they do.
//
// Stepping DOWN off inherit is left alone: it is already at the minimum, so
// there is nowhere below to go, and the row stays on inherit.
class InheritSpinBox : public QSpinBox {
public:
    InheritSpinBox(int inherited, QWidget *parent = nullptr)
        : QSpinBox(parent), inherited_(inherited) {}

    void stepBy(int steps) override {
        if (steps > 0 && value() == minimum() && inherited_ > minimum()) {
            // One step is the jump itself. A caller stepping by more than one
            // -- page up, or a fast wheel -- still lands on the inherited
            // value first, which is the value it was reading a moment ago.
            setValue(qBound(minimum(), inherited_, maximum()));
            return;
        }
        QSpinBox::stepBy(steps);
    }

private:
    int inherited_;
};

// `inherited` is the value the label names, so a step up can land on it. Pass
// the same number that went into the label -- they are one fact, and a row
// whose words and whose first step disagree is worse than either alone.
QSpinBox *inheritSpin(int lo, int hi, const QString &label, int inherited) {
    auto *box = new InheritSpinBox(inherited);
    box->setRange(lo - 1, hi);
    box->setSpecialValueText(label);
    box->setValue(lo - 1);

    // Words, not a bare number, for the state that is not a number. Without
    // this the row reads as a value that happens to be spelled oddly.
    box->setToolTip(QObject::tr("Leave on %1 to follow Settings. Step or type "
                                "to pin a value for this session.")
                        .arg(label));
    return box;
}

std::optional<int> spinValue(const QSpinBox *box) {
    if (box->value() == box->minimum()) return std::nullopt;
    return box->value();
}

void setSpinValue(QSpinBox *box, const std::optional<int> &value) {
    box->setValue(value ? *value : box->minimum());
}

// A flag that can be absent. Three items rather than a tristate QCheckBox:
// partially-checked reads as "some of the things below are on", not "this one
// follows the global", and no user has ever guessed which.
QComboBox *flagCombo(const QString &inheritLabel) {
    auto *box = new QComboBox;
    box->addItem(inheritLabel, QVariant());
    box->addItem(QObject::tr("On"), true);
    box->addItem(QObject::tr("Off"), false);
    return box;
}

std::optional<bool> flagValue(const QComboBox *box) {
    const QVariant data = box->currentData();
    if (!data.isValid()) return std::nullopt;
    return data.toBool();
}

void setFlagValue(QComboBox *box, const std::optional<bool> &value) {
    box->setCurrentIndex(!value.has_value() ? 0 : (*value ? 1 : 2));
}

// Text that can be absent: empty means inherit, and the placeholder says what
// that currently resolves to. A blank field with no placeholder is the one
// arrangement that leaves the user guessing.
std::optional<std::string> textValue(const QLineEdit *edit) {
    const QString text = edit->text().trimmed();
    if (text.isEmpty()) return std::nullopt;
    return text.toStdString();
}

void setTextValue(QLineEdit *edit, const std::optional<std::string> &value) {
    edit->setText(value ? toQ(*value) : QString());
}

int transportIndex(sessions::SessionTransport transport) {
    switch (transport) {
        case sessions::SessionTransport::Telnet: return 1;
        case sessions::SessionTransport::Serial: return 2;
        case sessions::SessionTransport::Ssh:    break;
    }
    return 0;
}

sessions::SessionTransport transportAt(int index) {
    switch (index) {
        case 1:  return sessions::SessionTransport::Telnet;
        case 2:  return sessions::SessionTransport::Serial;
        default: return sessions::SessionTransport::Ssh;
    }
}

}  // namespace

SessionEditorDialog::SessionEditorDialog(const sessions::Session &session,
                                         sessions::SessionStore *store,
                                         omegassh::Vault *vault,
                                         const AppSettings &settings,
                                         const OmegaSettings &omega,
                                         const theme::ThemeEngine *themes,
                                         QWidget *parent)
    : QDialog(parent), store_(store), vault_(vault), themes_(themes),
      settings_(settings), omega_(omega), original_(session) {
    buildUi();
    populateCredentials(credential_, false);
    populateCredentials(jumpCredential_, true);
    connect(credential_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { onCredentialChanged(); });
    populateFolders();
    loadFrom(session);
    // After loadFrom, which is what selects the session's own credential.
    onCredentialChanged();
}

void SessionEditorDialog::buildUi() {
    // 620 rather than the old 470 minimum. This is the widest form in the
    // application -- ten forms across three tabs, sharing one label column
    // once alignFieldLabels has run -- and fitToContent only ever grows from
    // the floor it is given.
    frame_ = new ModalFrame(this,
                            original_.id.has_value() ? tr("Edit Session")
                                                     : tr("New Session"),
                            620);

    tabs_ = new QTabWidget(frame_->bodyWidget());
    tabs_->setDocumentMode(true);
    tabs_->addTab(buildConnectionPage(), tr("Connection"));
    tabs_->addTab(buildTerminalPage(), tr("Terminal"));
    tabs_->addTab(buildAdvancedPage(), tr("Advanced"));
    frame_->body()->addWidget(tabs_);

    // Below the tabs, not inside a page. A complaint about a field on the
    // Connection tab has to stay readable after showProblem has moved the
    // reader to it, and a notice that lived on one page would vanish the
    // moment the reader looked at another.
    problemBox_ = frame_->addNotice(QString());
    problem_ = problemBox_->findChild<QLabel *>();
    problemBox_->hide();

    frame_->body()->addStretch();

    QPushButton *cancel = frame_->addButton(tr("Cancel"), ModalFrame::Secondary);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);

    QPushButton *save = frame_->addButton(tr("Save"), ModalFrame::Primary);
    connect(save, &QPushButton::clicked, this, &SessionEditorDialog::onAccept);

    frame_->setFooterHint(tr("Return saves. Empty rows inherit from Settings."));

    name_->setFocus();
}

void SessionEditorDialog::showEvent(QShowEvent *event) {
    QDialog::showEvent(event);
    if (labelsAligned_) return;
    labelsAligned_ = true;

    ensurePolished();
    if (layout()) layout()->activate();
    // Ten forms across three tabs. Without this the field column lands in a
    // different place on each, and switching tabs is the one place a reader
    // compares two forms directly.
    alignFieldLabels(tabs_);

    // INVALIDATE BEFORE FITTING. alignFieldLabels has just put a new minimum
    // width on the label column of ten forms, but a layout caches its
    // minimumSizeHint and nothing above has told it the cache is stale. So
    // fitToContent -- which sizes the dialog from exactly that hint -- would
    // measure the column as it was BEFORE the alignment, size the window to
    // the old width, and only then would the wider column take effect. The
    // result is a dialog that looks right until something forces the pages to
    // re-lay-out, at which point the fields slide off the right edge inside a
    // window that never moved. Switching tabs is what forces it, which is why
    // it looked like a tab bug.
    //
    // Every page, not just the visible one: the stacked layout takes its
    // minimum from the widest page, and two of the three have never been
    // shown at this point.
    for (int i = 0; i < tabs_->count(); ++i) {
        if (QWidget *page = tabs_->widget(i)) {
            for (QLayout *l : page->findChildren<QLayout *>()) {
                l->invalidate();
            }
            if (QLayout *l = page->layout()) {
                l->invalidate();
                l->activate();
            }
        }
    }
    if (QLayout *l = tabs_->layout()) l->invalidate();
    if (layout()) layout()->activate();

    if (frame_) frame_->fitToContent();
}

int SessionEditorDialog::selectedTransport() const {
    const int id = transport_ ? transport_->checkedId() : -1;
    return id < 0 ? 0 : id;
}

void SessionEditorDialog::markInvalid(QWidget *field, bool invalid) {
    if (!field) return;
    if (field->property("invalid").toBool() == invalid) return;
    field->setProperty("invalid", invalid);
    repolish(field);
}

void SessionEditorDialog::showProblem(const QString &text, QWidget *field,
                                      int tab) {
    if (invalidField_ && invalidField_ != field) {
        markInvalid(invalidField_, false);
    }
    invalidField_ = field;
    markInvalid(field, true);

    // The tab move is still part of saying which field -- a field on a page
    // that is not showing is invisible, and no amount of text fixes that. What
    // it is no longer a substitute FOR is the text.
    if (tabs_ && tab >= 0) tabs_->setCurrentIndex(tab);
    if (field) field->setFocus();

    problem_->setText(text);
    const bool wasVisible = problemBox_->isVisible();
    problemBox_->setVisible(!text.isEmpty());
    if (frame_ && wasVisible != problemBox_->isVisible()) {
        frame_->fitToContent();
    }
}

void SessionEditorDialog::clearProblem() {
    if (!problemBox_ || !problemBox_->isVisible()) return;
    markInvalid(invalidField_, false);
    invalidField_ = nullptr;
    problem_->clear();
    problemBox_->hide();
    if (frame_) frame_->fitToContent();
}

QWidget *SessionEditorDialog::buildConnectionPage() {
    auto *page = new QWidget;
    auto *outer = new QVBoxLayout(page);

    auto *identityBox = new QGroupBox(tr("Session"));
    auto *identity = new QFormLayout(identityBox);

    name_ = new QLineEdit;
    name_->setPlaceholderText(tr("lab-core-1"));
    identity->addRow(fieldLabel(tr("Name"), identityBox), name_);

    description_ = new QLineEdit;
    description_->setPlaceholderText(tr("Optional"));
    identity->addRow(fieldLabel(tr("Description"), identityBox), description_);

    folder_ = new QComboBox;
    identity->addRow(fieldLabel(tr("Folder"), identityBox), folder_);

    // The segmented track sits in the field column like any other row, so the
    // label still names it and the form's alignment still holds. The well
    // supplies the recessed surface; the sheet paints the checked segment.
    auto *track = new QFrame(identityBox);
    track->setProperty("role", "well");
    auto *trackRow = new QHBoxLayout(track);
    trackRow->setContentsMargins(4, 4, 4, 4);
    trackRow->setSpacing(4);

    transport_ = new QButtonGroup(this);
    transport_->setExclusive(true);
    int transportId = 0;
    for (const QString &name : {tr("SSH"), tr("Telnet"), tr("Serial")}) {
        auto *segment = new QPushButton(name, track);
        segment->setProperty("segment", true);
        segment->setCheckable(true);
        segment->setChecked(transportId == 0);
        // Off, like every button outside the footer. A checkable button that
        // also answered Return would change transport on the keystroke meant
        // to save.
        segment->setAutoDefault(false);
        transport_->addButton(segment, transportId);
        trackRow->addWidget(segment, 1);
        ++transportId;
    }
    identity->addRow(fieldLabel(tr("Transport"), identityBox), track);

    outer->addWidget(identityBox);

    // The stack sits in a box of its own so the two halves of the page read
    // as equals. A bare form under a titled group looks like an afterthought
    // that outgrew its layout.
    auto *targetBox = new QGroupBox(tr("Target"));
    auto *targetLayout = new QVBoxLayout(targetBox);
    targetLayout->setContentsMargins(0, 0, 0, 0);

    pages_ = new QStackedWidget;

    // --- page 0: a network target -----------------------------------------
    auto *network = new QWidget;
    auto *networkForm = new QFormLayout(network);

    auto *hostRow = new QHBoxLayout;
    host_ = new QLineEdit;
    host_->setPlaceholderText(tr("hostname or address"));
    hostRow->addWidget(host_, 1);
    hostRow->addWidget(new QLabel(QStringLiteral(":")));

    port_ = new QSpinBox;
    port_->setRange(1, 65535);
    port_->setValue(22);
    // NO FIXED WIDTH. It was 80px, and 80px was never enough: a spin box sized
    // for five digits reports 114 at ui_font_size 13 and 139 at 20, both of
    // which include the style's own padding and the room the spin buttons
    // take. The field clipped its value at every chrome size -- a 65535 read
    // as 6553, and "22" survived only because two digits fit in what was left.
    //
    // The widget already knows the answer and it tracks the font, so it is
    // asked instead of being told. Added with no stretch, so it takes its
    // size hint and host_ keeps the slack.
    hostRow->addWidget(port_);
    networkForm->addRow(fieldLabel(tr("Host"), network), hostRow);

    credential_ = new QComboBox;
    networkForm->addRow(fieldLabel(tr("Credential"), network), credential_);

    username_ = new QLineEdit;
    // Set by onCredentialChanged() rather than fixed: with no credential
    // named there is nothing for a blank username to come from, and a
    // placeholder saying otherwise is wrong in exactly the case the inherit
    // row makes ordinary.
    username_->setToolTip(
        tr("Overrides the username on the credential. Explicit fields win "
           "over a reference, so a credential can supply the password while "
           "this supplies the user."));
    networkForm->addRow(fieldLabel(tr("Username"), network), username_);

    pages_->addWidget(network);

    // --- page 1: a serial line --------------------------------------------
    auto *serial = new QWidget;
    auto *serialForm = new QFormLayout(serial);

    // Editable, unlike quick connect's: a saved session names a port that may
    // not be plugged in right now, and a combo that could only offer what is
    // present would silently drop the name on the next save.
    serialPort_ = new QComboBox;
    serialPort_->setEditable(true);
    serialForm->addRow(fieldLabel(tr("Port"), serial), serialPort_);

    baud_ = new QComboBox;
    for (const int rate : kBauds) baud_->addItem(QString::number(rate), rate);
    serialForm->addRow(fieldLabel(tr("Baud"), serial), baud_);

    // 8N1 is not offered here for the same reason it is not offered in quick
    // connect: the columns exist, every console port in the building is 8N1,
    // and three combo boxes nobody changes are three chances to get it wrong.
    serialForm->addRow(QString(),
                       new QLabel(tr("8 data bits, no parity, 1 stop bit.")));

    pages_->addWidget(serial);
    targetLayout->addWidget(pages_);
    outer->addWidget(targetBox);
    outer->addStretch(1);

    connect(transport_, &QButtonGroup::idClicked, this,
            &SessionEditorDialog::onTransportChanged);

    return page;
}

QWidget *SessionEditorDialog::buildTerminalPage() {
    auto *page = new QWidget;
    auto *outer = new QVBoxLayout(page);

    auto *termBox = new QGroupBox(tr("Terminal"));
    auto *form = new QFormLayout(termBox);

    term_ = new QLineEdit;
    term_->setPlaceholderText(
        settings_.default_term_type.trimmed().isEmpty()
            ? tr("xterm-256color")
            : tr("%1  (inherited)").arg(settings_.default_term_type.trimmed()));
    term_->setToolTip(
        tr("Declared to the far end: the pty request on ssh, the answer to a "
           "TTYPE subnegotiation on telnet. Serial has nowhere to declare it."));
    form->addRow(fieldLabel(tr("Terminal type"), termBox), term_);

    scrollback_ = inheritSpin(100, 1000000,
                              tr("(inherit — %1)").arg(settings_.scrollback_lines),
                              settings_.scrollback_lines);
    scrollback_->setSingleStep(1000);
    form->addRow(fieldLabel(tr("Scrollback lines"), termBox), scrollback_);

    // --- appearance -------------------------------------------------------
    // Both rows inherit by default and say what they inherit, like every
    // other override on this form. They are here rather than in Settings
    // because the reason to pin either is a property of the far end -- a
    // console that needs 132 columns on this screen, a device that should not
    // be mistaken for the lab one next to it.
    fontSize_ = inheritSpin(kMinTerminalFontPointSize, kMaxTerminalFontPointSize,
                            tr("(inherit — %1 pt)").arg(settings_.font_size),
                            settings_.font_size);
    fontSize_->setSuffix(tr(" pt"));
    fontSize_->setToolTip(
        tr("The terminal font for this session, in points. Ctrl+wheel still "
           "zooms from here, and the zoom is not saved back."));
    form->addRow(fieldLabel(tr("Font size"), termBox), fontSize_);

    theme_ = new QComboBox;
    theme_->setToolTip(
        tr("The colour theme for this session's terminal. The window's chrome "
           "and every other tab keep the theme from Settings."));
    // Inherit first, labelled with the global it currently follows. An
    // uninstalled global name is shown as it is rather than corrected: see
    // SettingsDialog::loadFrom for why that is the ordinary case.
    theme_->addItem(settings_.theme_name.isEmpty()
                        ? tr("(inherit)")
                        : tr("(inherit — %1)").arg(settings_.theme_name),
                    QString());
    if (themes_) {
        for (const std::string &name : themes_->names()) {
            const QString q = QString::fromStdString(name);
            theme_->addItem(q, q);
        }
    }
    form->addRow(fieldLabel(tr("Theme"), termBox), theme_);

    wheelAltScreen_ = flagCombo(
        omega_.wheel_alt_screen ? tr("(default — on)") : tr("(default — off)"));
    wheelAltScreen_->setToolTip(
        tr("Inside a pager or a full-screen application, the wheel sends "
           "arrow keys to that application instead of scrolling the history "
           "behind it. At an ordinary prompt the wheel still scrolls "
           "scrollback either way."));
    // SHORT, because alignFieldLabels shares one label column across all ten
    // forms on all three tabs: the widest label anywhere sets the column
    // everywhere. "Wheel scrolls the application" measured 389px at
    // ui_font_size 20 against 254 for the next widest, and pushed every field
    // on every tab off the right edge of the dialog. The tooltip carries the
    // explanation, which is where the length belongs.
    form->addRow(fieldLabel(tr("Full-screen wheel"), termBox),
                 wheelAltScreen_);

    outer->addWidget(termBox);

    auto *pasteBox = new QGroupBox(tr("Paste"));
    auto *pasteForm = new QFormLayout(pasteBox);

    pasteThreshold_ = inheritSpin(
        1, 10000, tr("(inherit — %1)").arg(settings_.multiline_paste_threshold),
        settings_.multiline_paste_threshold);
    pasteThreshold_->setToolTip(
        tr("Lines in a paste before the confirmation appears."));
    pasteForm->addRow(fieldLabel(tr("Confirm above"), pasteBox), pasteThreshold_);

    // A combo rather than a spin box, because 0 is a real value here --
    // unpaced -- and a spin box cannot label two states with words.
    pasteBaud_ = new QComboBox;
    pasteBaud_->addItem(tr("(inherit — from the transport)"), QVariant());
    pasteBaud_->addItem(tr("Unpaced"), 0);
    for (const int rate : kBauds) {
        pasteBaud_->addItem(tr("%1 baud").arg(rate), rate);
    }
    pasteBaud_->setToolTip(
        tr("Paces a paste at the rate the far end is actually set to. What a "
           "terminal server or console line needs, and what it needs is a "
           "baud rate, not a delay."));
    pasteForm->addRow(fieldLabel(tr("Rate"), pasteBox), pasteBaud_);

    outer->addWidget(pasteBox);

    // --- anti-idle --------------------------------------------------------
    auto *idleBox = new QGroupBox(tr("Anti-idle"));
    auto *idleForm = new QFormLayout(idleBox);

    auto *idleNote = new QLabel(
        tr("A keystroke sent after silence, so the device's own exec-timeout "
           "does not close the session. Not keepalive: a timer that counts "
           "terminal input is not reset by packets."));
    idleNote->setWordWrap(true);
    idleForm->addRow(idleNote);

    antiIdleEnabled_ = flagCombo(tr("(inherit — %1)")
                                     .arg(omega_.anti_idle.enabled ? tr("on")
                                                                   : tr("off")));
    idleForm->addRow(fieldLabel(tr("Enabled"), idleBox), antiIdleEnabled_);

    antiIdleSeconds_ =
        inheritSpin(5, 86400, tr("(inherit — %1s)").arg(omega_.anti_idle.seconds),
                    omega_.anti_idle.seconds);
    antiIdleSeconds_->setSuffix(tr(" s"));
    idleForm->addRow(fieldLabel(tr("After"), idleBox), antiIdleSeconds_);

    antiIdleKeystroke_ = new QComboBox;
    antiIdleKeystroke_->addItem(
        tr("(inherit — %1)").arg(keystrokeName(omega_.anti_idle.keystroke)),
        QString());
    antiIdleKeystroke_->addItem(tr("Backspace"),
                                keystrokeName(AntiIdleKeystroke::Backspace));
    antiIdleKeystroke_->addItem(tr("Space + Backspace"),
                                keystrokeName(AntiIdleKeystroke::SpaceBackspace));
    antiIdleKeystroke_->addItem(tr("NUL"), keystrokeName(AntiIdleKeystroke::Nul));
    antiIdleKeystroke_->addItem(tr("Custom (hex)"),
                                keystrokeName(AntiIdleKeystroke::Custom));
    antiIdleKeystroke_->setToolTip(
        tr("Backspace is the convention and is a no-op at an idle prompt — but "
           "it deletes a character from a half-typed line. Space + Backspace "
           "is net zero on both."));
    idleForm->addRow(fieldLabel(tr("Keystroke"), idleBox), antiIdleKeystroke_);

    antiIdleCustom_ = new QLineEdit;
    antiIdleCustom_->setPlaceholderText(tr("hex, no prefix — 0d"));
    idleForm->addRow(fieldLabel(tr("Custom bytes"), idleBox), antiIdleCustom_);

    connect(antiIdleKeystroke_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { onKeystrokeChanged(); });

    outer->addWidget(idleBox);
    outer->addStretch(1);

    return page;
}

QWidget *SessionEditorDialog::buildAdvancedPage() {
    auto *page = new QWidget;
    auto *outer = new QVBoxLayout(page);

    auto *sshBox = new QGroupBox(tr("SSH"));
    auto *sshForm = new QFormLayout(sshBox);

    // No global for this one: the fallback is the library's own Strict, which
    // is what the inherit row says rather than pointing at a setting that
    // does not exist.
    hostKeyPolicy_ = new QComboBox;
    hostKeyPolicy_->addItem(tr("(default — strict)"), QString());
    hostKeyPolicy_->addItem(tr("Strict — known_hosts only"),
                            QString::fromLatin1(hostKeyPolicyName(
                                omegassh::HostKeyPolicy::Strict)));
    hostKeyPolicy_->addItem(tr("Trust on first use — pin, then refuse a change"),
                            QString::fromLatin1(hostKeyPolicyName(
                                omegassh::HostKeyPolicy::Tofu)));
    hostKeyPolicy_->addItem(tr("Insecure — no verification at all"),
                            QString::fromLatin1(hostKeyPolicyName(
                                omegassh::HostKeyPolicy::Insecure)));
    hostKeyPolicy_->setToolTip(
        tr("Per session, because a lab bench and a production edge do not want "
           "the same answer. Insecure is a real choice for gear that is "
           "reimaged weekly; it is not a default anywhere."));
    sshForm->addRow(fieldLabel(tr("Host key"), sshBox), hostKeyPolicy_);

    legacy_ = flagCombo(tr("(default — off)"));
    legacy_->setToolTip(
        tr("The old KEX, cipher and MAC set aging gear still requires. A "
           "property of the device, which is why it belongs on the session."));
    sshForm->addRow(fieldLabel(tr("Legacy algorithms"), sshBox), legacy_);

    outer->addWidget(sshBox);

    auto *jumpBox = new QGroupBox(tr("Jump host"));
    auto *jumpForm = new QFormLayout(jumpBox);

    auto *jumpRow = new QHBoxLayout;
    jumpHost_ = new QLineEdit;
    jumpHost_->setPlaceholderText(tr("none"));
    jumpRow->addWidget(jumpHost_, 1);
    jumpRow->addWidget(new QLabel(QStringLiteral(":")));

    jumpPort_ = new QSpinBox;
    jumpPort_->setRange(1, 65535);
    jumpPort_->setValue(22);
    // Same as the target port above, and for the same reason.
    jumpRow->addWidget(jumpPort_);
    jumpForm->addRow(fieldLabel(tr("Host"), jumpBox), jumpRow);

    jumpUsername_ = new QLineEdit;
    jumpForm->addRow(fieldLabel(tr("Username"), jumpBox), jumpUsername_);

    jumpCredential_ = new QComboBox;
    jumpForm->addRow(fieldLabel(tr("Credential"), jumpBox), jumpCredential_);

    // Everything but the host is ignored without one, in Config and here.
    auto *jumpNote =
        new QLabel(tr("Leave the host empty to connect directly."));
    jumpNote->setWordWrap(true);
    jumpForm->addRow(jumpNote);

    outer->addWidget(jumpBox);

    auto *telnetBox = new QGroupBox(tr("Telnet"));
    auto *telnetForm = new QFormLayout(telnetBox);

    telnetCrlf_ = flagCombo(tr("(default — on)"));
    telnetCrlf_->setToolTip(
        tr("Expands a lone CR on write to CR LF, which is what RFC 854 makes "
           "the telnet newline. Off only for a device that echoes a doubled "
           "newline."));
    telnetForm->addRow(fieldLabel(tr("CR becomes CR LF"), telnetBox), telnetCrlf_);

    outer->addWidget(telnetBox);
    outer->addStretch(1);

    return page;
}

void SessionEditorDialog::onCredentialChanged() {
    const bool named = !credential_->currentData().toString().isEmpty();
    username_->setPlaceholderText(named ? tr("from the credential") : QString());
}

void SessionEditorDialog::populateCredentials(QComboBox *box, bool isJumpHost) {
    box->clear();

    // The jump host has no global of its own -- SshDefaultAuth is the target
    // connection's -- so its inherit row says what it does rather than
    // borrowing a label for a setting that does not apply to it.
    box->addItem(isJumpHost
                     ? tr("(none)")
                     : tr("(default — %1)")
                           .arg(sshDefaultAuthLabel(omega_.ssh_default_auth)),
                 QString());

    if (!vault_ || !vault_->isOpen()) return;

    if (vault_->isLocked()) {
        // Said, not hidden -- the same reason the quick-connect picker says
        // it. An empty list and a locked vault look identical otherwise, and
        // the user's next move is different in each case.
        box->addItem(tr("(vault locked — unlock to choose one)"), QString());
        box->setItemData(box->count() - 1, false,
                         Qt::UserRole - 1);  // shown, not selectable
        return;
    }

    QVector<omegassh::CredentialMeta> entries;
    if (vault_->list(&entries) != omegassh::VaultError::Ok) return;

    for (const omegassh::CredentialMeta &meta : entries) {
        if (meta.disabled) continue;  // a disabled entry refuses at dial time
        box->addItem(meta.name, meta.name);
    }
}

void SessionEditorDialog::populateFolders() {
    folder_->clear();
    folder_->addItem(tr("(top level)"), QVariant());

    if (!store_) return;

    // Depth-first from root so the list reads in tree order, indented. The
    // store returns one level at a time, which is what makes the recursion the
    // natural shape here rather than a flat list plus a sort.
    std::function<void(std::optional<int64_t>, int)> walk =
        [&](std::optional<int64_t> parent, int depth) {
            for (const sessions::Folder &f : store_->listFolders(parent)) {
                if (!f.id.has_value()) continue;
                const QString label =
                    QString(depth * 4, QChar(' ')) + toQ(f.name);
                folder_->addItem(label, QVariant(static_cast<qint64>(*f.id)));
                walk(*f.id, depth + 1);
            }
        };
    walk(std::nullopt, 0);
}

// The DEVICE name currently chosen, which is not the same as the combo's
// text: enumeration labels an item "usbserial-A50285BI (0403:6001)" and a
// missing one "/dev/ttyUSB0  (not present)", while the name underneath both is
// what gets stored and dialed. Reading the text back instead is how a refresh
// appends its own suffix to a string that already carried one.
QString SessionEditorDialog::currentSerialName() const {
    const int at = serialPort_->currentIndex();
    if (at >= 0 && serialPort_->itemText(at) == serialPort_->currentText()) {
        const QString data = serialPort_->itemData(at).toString();
        if (!data.isEmpty()) return data;
    }
    // Typed by hand, and taken literally -- a device node this build cannot
    // enumerate is still a device node.
    return serialPort_->currentText().trimmed();
}

void SessionEditorDialog::refreshSerialPorts() {
    // The saved name is kept whatever enumeration finds. A session that names
    // a cable which is not plugged in right now is not a broken session, and
    // rewriting its port to whatever happens to be present would be.
    const QString saved = currentSerialName();
    serialPort_->clear();

    const QVector<omegassh::SerialPortInfo> ports =
        omegassh::OmegaSshSession::serialPorts();
    for (const omegassh::SerialPortInfo &p : ports) {
        serialPort_->addItem(p.displayName(), p.name);
    }

    if (saved.isEmpty()) return;

    const int at = serialPort_->findData(saved);
    if (at >= 0) {
        serialPort_->setCurrentIndex(at);
    } else {
        serialPort_->addItem(tr("%1  (not present)").arg(saved), saved);
        serialPort_->setCurrentIndex(serialPort_->count() - 1);
    }
}

void SessionEditorDialog::onTransportChanged(int index) {
    const bool serial = index == 2;
    const bool ssh = index == 0;

    pages_->setCurrentIndex(serial ? 1 : 0);

    if (serial) {
        refreshSerialPorts();
    } else {
        // The port follows the transport, but only while it still holds the
        // other transport's default. Somebody who typed 2222 meant it.
        if (ssh && port_->value() == 23) port_->setValue(22);
        if (!ssh && port_->value() == 22) port_->setValue(23);
    }

    // Telnet has no authentication step -- a login prompt on it is ordinary
    // session data arriving after the socket is up, so filling these in would
    // be a lie about where the credentials go, and the library refuses them
    // outright. Serial has no network identity at all.
    credential_->setEnabled(ssh);
    username_->setEnabled(ssh);
    hostKeyPolicy_->setEnabled(ssh);
    legacy_->setEnabled(ssh);
    jumpHost_->setEnabled(ssh);
    jumpPort_->setEnabled(ssh);
    jumpUsername_->setEnabled(ssh);
    jumpCredential_->setEnabled(ssh);

    telnetCrlf_->setEnabled(index == 1);
}

void SessionEditorDialog::onKeystrokeChanged() {
    const bool custom = antiIdleKeystroke_->currentData().toString() ==
                        keystrokeName(AntiIdleKeystroke::Custom);
    antiIdleCustom_->setEnabled(custom);
}

void SessionEditorDialog::loadFrom(const sessions::Session &session) {
    name_->setText(toQ(session.name));
    description_->setText(toQ(session.description));
    host_->setText(toQ(session.hostname));
    port_->setValue(session.port > 0 ? session.port : 22);

    if (session.credential_name.has_value()) {
        const int idx = credential_->findData(toQ(*session.credential_name));
        if (idx >= 0) {
            credential_->setCurrentIndex(idx);
        } else {
            // A name the vault no longer holds. Kept and shown rather than
            // silently reset to agent auth: losing the reference on the next
            // save is a worse outcome than a row that refuses at dial time and
            // says why.
            credential_->addItem(
                tr("%1 (not in vault)").arg(toQ(*session.credential_name)),
                toQ(*session.credential_name));
            credential_->setCurrentIndex(credential_->count() - 1);
        }
    }

    if (session.folder_id.has_value()) {
        const int idx =
            folder_->findData(QVariant(static_cast<qint64>(*session.folder_id)));
        if (idx >= 0) folder_->setCurrentIndex(idx);
    }

    setTextValue(username_, session.username);

    // --- serial -----------------------------------------------------------
    if (session.serial_port) {
        // Seeded as an item with the device name as its DATA, not as loose
        // text: refreshSerialPorts runs next and matches on data.
        serialPort_->addItem(toQ(*session.serial_port), toQ(*session.serial_port));
        serialPort_->setCurrentIndex(serialPort_->count() - 1);
    }
    if (session.serial_baud) {
        const int at = baud_->findData(*session.serial_baud);
        if (at >= 0) {
            baud_->setCurrentIndex(at);
        } else {
            baud_->addItem(QString::number(*session.serial_baud),
                           *session.serial_baud);
            baud_->setCurrentIndex(baud_->count() - 1);
        }
    }

    // Set last of the connection fields, and set explicitly even when it is
    // already 0: the handler is what enables and disables half this form, and
    // an SSH session would otherwise open with the telnet rows still greyed.
    if (QAbstractButton *b =
            transport_->button(transportIndex(session.transport))) {
        b->setChecked(true);
    }
    onTransportChanged(selectedTransport());

    // --- terminal ---------------------------------------------------------
    setTextValue(term_, session.term_type);
    setSpinValue(scrollback_, session.scrollback_lines);
    setSpinValue(pasteThreshold_, session.multiline_paste_threshold);
    setSpinValue(fontSize_, session.font_size);

    // A saved theme this installation does not have is OFFERED, not dropped.
    // Silently falling back to inherit would rewrite the column on the next
    // save and lose a setting the operator made on another machine -- and the
    // database travelling between machines is exactly how the name got here.
    if (session.theme_name && !session.theme_name->empty()) {
        const QString want = QString::fromStdString(*session.theme_name);
        const int at = theme_->findData(want);
        if (at >= 0) {
            theme_->setCurrentIndex(at);
        } else {
            theme_->addItem(tr("%1 (not installed)").arg(want), want);
            theme_->setCurrentIndex(theme_->count() - 1);
        }
    }

    if (session.paste_baud) {
        const int at = pasteBaud_->findData(*session.paste_baud);
        if (at >= 0) {
            pasteBaud_->setCurrentIndex(at);
        } else {
            pasteBaud_->addItem(tr("%1 baud").arg(*session.paste_baud),
                                *session.paste_baud);
            pasteBaud_->setCurrentIndex(pasteBaud_->count() - 1);
        }
    }

    // --- anti-idle --------------------------------------------------------
    setFlagValue(antiIdleEnabled_, session.anti_idle_enabled);
    setSpinValue(antiIdleSeconds_, session.anti_idle_seconds);
    if (session.anti_idle_keystroke) {
        const QString name = toQ(*session.anti_idle_keystroke);
        const int at = antiIdleKeystroke_->findData(name);
        if (at >= 0) {
            antiIdleKeystroke_->setCurrentIndex(at);
        } else {
            // A spelling this build does not know -- a typo in the column, or
            // a keystroke a later version added. Kept and shown rather than
            // silently dropped back to inherit, for the same reason a
            // credential the vault no longer holds is kept: losing it on the
            // next save is worse than a row that says it is not understood.
            // The resolver falls back to the default at dial time regardless.
            antiIdleKeystroke_->addItem(tr("%1 (not recognised)").arg(name), name);
            antiIdleKeystroke_->setCurrentIndex(antiIdleKeystroke_->count() - 1);
        }
    }
    setTextValue(antiIdleCustom_, session.anti_idle_custom);
    onKeystrokeChanged();

    // --- ssh --------------------------------------------------------------
    if (session.host_key_policy) {
        const int at = hostKeyPolicy_->findData(toQ(*session.host_key_policy));
        if (at >= 0) hostKeyPolicy_->setCurrentIndex(at);
    }
    setFlagValue(legacy_, session.legacy_algorithms);
    setFlagValue(telnetCrlf_, session.telnet_crlf);
    setFlagValue(wheelAltScreen_, session.wheel_alt_screen);

    // --- jump host --------------------------------------------------------
    setTextValue(jumpHost_, session.jump_host);
    if (session.jump_port) jumpPort_->setValue(*session.jump_port);
    setTextValue(jumpUsername_, session.jump_username);
    if (session.jump_credential) {
        const int at = jumpCredential_->findData(toQ(*session.jump_credential));
        if (at >= 0) {
            jumpCredential_->setCurrentIndex(at);
        } else {
            jumpCredential_->addItem(
                tr("%1 (not in vault)").arg(toQ(*session.jump_credential)),
                toQ(*session.jump_credential));
            jumpCredential_->setCurrentIndex(jumpCredential_->count() - 1);
        }
    }
}

void SessionEditorDialog::onAccept() {
    // Each rejection names the field, marks it, and raises its tab. The tab
    // move was always here and was always right -- a field on a page that is
    // not showing is invisible. What was missing was the sentence: the form
    // used to answer Save by silently changing tabs, which tells a reader that
    // SOMETHING is wrong and nothing about what.
    if (name_->text().trimmed().isEmpty()) {
        return showProblem(tr("A session needs a name. It is what the tree "
                              "and the tab are labelled with."),
                           name_, 0);
    }

    if (transportAt(selectedTransport()) == sessions::SessionTransport::Serial) {
        if (serialPort_->currentText().trimmed().isEmpty()) {
            return showProblem(tr("A serial session needs a port. If the list "
                                  "is empty, no adapter was found."),
                               serialPort_, 0);
        }
    } else if (host_->text().trimmed().isEmpty()) {
        return showProblem(tr("A network session needs a host."), host_, 0);
    }

    // A custom keystroke that silently loses its last nibble is worse than one
    // that visibly does nothing, which is why hexToBytes refuses an odd length
    // -- and why an empty result here is a rejection rather than a save.
    if (antiIdleKeystroke_->currentData().toString() ==
            keystrokeName(AntiIdleKeystroke::Custom) &&
        hexToBytes(antiIdleCustom_->text().trimmed()).isEmpty()) {
        return showProblem(tr("The anti-idle keystroke is not a whole number "
                              "of bytes. Hex takes two characters per byte."),
                           antiIdleCustom_, 1);
    }

    clearProblem();
    accept();
}

sessions::Session SessionEditorDialog::session() const {
    sessions::Session out = original_;

    out.name = name_->text().trimmed().toStdString();
    out.description = description_->text().trimmed().toStdString();

    const QVariant folder = folder_->currentData();
    if (folder.isValid() && !folder.isNull())
        out.folder_id = folder.toLongLong();
    else
        out.folder_id.reset();

    out.transport = transportAt(selectedTransport());
    const bool serial = out.transport == sessions::SessionTransport::Serial;
    const bool ssh = out.transport == sessions::SessionTransport::Ssh;

    if (serial) {
        const QString port = currentSerialName();
        out.serial_port = port.toStdString();
        out.serial_baud = baud_->currentData().toInt();

        // hostname is NOT NULL and it is what the tree renders, so a serial
        // session carries its device name there too. serial_port is the one
        // the resolver reads; this copy is a label, and a stale one is a
        // display bug rather than a connection that goes somewhere else.
        out.hostname = port.toStdString();
    } else {
        out.hostname = host_->text().trimmed().toStdString();
        out.port = port_->value();
        out.serial_port.reset();
        out.serial_baud.reset();
    }

    // Credentials are SSH's alone. Telnet has no authentication step and the
    // library refuses these outright; serial has no network identity. Saving a
    // session as either therefore drops a credential it used to carry, which
    // is a consequence of the change the user just made rather than a
    // surprise -- the rows grey out the moment the transport changes.
    if (ssh) {
        const QString cred = credential_->currentData().toString();
        if (cred.isEmpty())
            out.credential_name.reset();
        else
            out.credential_name = cred.toStdString();

        out.username = textValue(username_);

        const QString policy = hostKeyPolicy_->currentData().toString();
        if (policy.isEmpty())
            out.host_key_policy.reset();
        else
            out.host_key_policy = policy.toStdString();

        out.legacy_algorithms = flagValue(legacy_);

        out.jump_host = textValue(jumpHost_);
        if (out.jump_host) {
            out.jump_port = jumpPort_->value();
            out.jump_username = textValue(jumpUsername_);
            const QString jc = jumpCredential_->currentData().toString();
            out.jump_credential =
                jc.isEmpty() ? std::nullopt
                             : std::optional<std::string>(jc.toStdString());
        } else {
            // Written as absent rather than left behind: the resolver ignores
            // them without a host, and a column holding a bastion username for
            // a bastion that is not used reads as a bug in a sqlite3 shell.
            out.jump_port.reset();
            out.jump_username.reset();
            out.jump_credential.reset();
        }
    } else {
        out.credential_name.reset();
        out.username.reset();
        out.host_key_policy.reset();
        out.legacy_algorithms.reset();
        out.jump_host.reset();
        out.jump_port.reset();
        out.jump_username.reset();
        out.jump_credential.reset();
    }

    out.telnet_crlf = out.transport == sessions::SessionTransport::Telnet
                          ? flagValue(telnetCrlf_)
                          : std::nullopt;

    // --- terminal, on every transport --------------------------------------
    out.term_type = textValue(term_);
    out.scrollback_lines = spinValue(scrollback_);
    out.multiline_paste_threshold = spinValue(pasteThreshold_);
    out.paste_baud = pasteBaud_->currentData().isValid()
                         ? std::optional<int>(pasteBaud_->currentData().toInt())
                         : std::nullopt;

    out.font_size = spinValue(fontSize_);
    const QString themeName = theme_->currentData().toString();
    out.theme_name = themeName.isEmpty()
                         ? std::nullopt
                         : std::optional<std::string>(themeName.toStdString());

    // Not cleared on a transport change, unlike the ssh-only rows above: a
    // pager runs over ssh, telnet and a serial console alike.
    out.wheel_alt_screen = flagValue(wheelAltScreen_);

    out.anti_idle_enabled = flagValue(antiIdleEnabled_);
    out.anti_idle_seconds = spinValue(antiIdleSeconds_);
    const QString keystroke = antiIdleKeystroke_->currentData().toString();
    if (keystroke.isEmpty()) {
        out.anti_idle_keystroke.reset();
        out.anti_idle_custom.reset();
    } else {
        out.anti_idle_keystroke = keystroke.toStdString();
        out.anti_idle_custom =
            keystroke == keystrokeName(AntiIdleKeystroke::Custom)
                ? std::optional<std::string>(
                      antiIdleCustom_->text().trimmed().toStdString())
                : std::nullopt;
    }

    return out;
}

}  // namespace omega::app