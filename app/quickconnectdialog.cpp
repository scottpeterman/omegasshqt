// app/quickconnectdialog.cpp

#include "app/quickconnectdialog.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "app/modalframe.h"

namespace omega::app {
namespace {

constexpr int kDialogWidth = 560;

// The rates worth offering. 9600 first because it is what a console port is
// set to until somebody changes it, and 115200 second because it is what they
// change it to.
const int kBauds[] = {9600, 19200, 38400, 57600, 115200, 230400};

void styleForm(QFormLayout *form) {
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(8);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
}

// Enables a form row -- the field AND the label that names it.
//
// setEnabled on the field alone leaves a fully lit "Username" beside a greyed
// box, which reads as a rendering fault rather than as a row that does not
// apply. Telnet greys six rows at once, so on that page it was six of them.
// QFormLayout is the only thing that knows which label belongs to which
// field, which is why the layout is kept as a member.
void setRowEnabled(QFormLayout *form, QWidget *field, bool on) {
    if (!form || !field) return;
    field->setEnabled(on);
    if (QWidget *label = form->labelForField(field)) label->setEnabled(on);
}

}  // namespace

QuickConnectDialog::QuickConnectDialog(omegassh::Vault *vault, QWidget *parent)
    : QDialog(parent), vault_(vault) {
    buildUi();
    onTransportChanged(0);
}

int QuickConnectDialog::transportIndex() const {
    const int id = transport_ ? transport_->checkedId() : -1;
    return id < 0 ? 0 : id;
}

void QuickConnectDialog::buildUi() {
    auto *frame = new ModalFrame(this, tr("Quick Connect"), kDialogWidth);

    // --- transport --------------------------------------------------------
    // Three buttons in a well: the well supplies the recessed track and the
    // sheet paints the checked segment in {accent}. One click, and all three
    // options readable without opening anything -- which matters here more
    // than anywhere else in the application, because this dialog exists to be
    // opened, answered and dismissed in a few seconds.
    QFrame *track = frame->addWell();
    auto *trackRow = new QHBoxLayout(track);
    trackRow->setContentsMargins(4, 4, 4, 4);
    trackRow->setSpacing(4);

    transport_ = new QButtonGroup(this);
    transport_->setExclusive(true);

    int id = 0;
    for (const QString &name : {tr("SSH"), tr("Telnet"), tr("Serial")}) {
        auto *segment = new QPushButton(name, track);
        segment->setProperty("segment", true);
        segment->setCheckable(true);
        segment->setChecked(id == 0);
        // OFF, like every other button outside the footer. A checkable button
        // that also answered Return would switch transport on the keystroke
        // meant to connect.
        segment->setAutoDefault(false);
        transport_->addButton(segment, id);
        trackRow->addWidget(segment, 1);
        ++id;
    }

    pages_ = new QStackedWidget(frame->bodyWidget());

    // --- page 0: ssh and telnet share a page ------------------------------
    // They differ by which rows are enabled, not by which page is shown: a
    // telnet target is a host and a port, and so is an SSH one. Two pages
    // would mean typing the address twice to change your mind.
    auto *network = new QWidget(pages_);
    network->setProperty("bare", true);
    networkForm_ = new QFormLayout(network);
    networkForm_->setContentsMargins(0, 12, 0, 0);
    styleForm(networkForm_);

    host_ = new QLineEdit(network);
    host_->setPlaceholderText(tr("hostname or address"));
    host_->setProperty("mono", true);
    networkForm_->addRow(fieldLabel(tr("Host"), network), host_);

    port_ = new QSpinBox(network);
    port_->setRange(1, 65535);
    port_->setValue(22);
    networkForm_->addRow(fieldLabel(tr("Port"), network), port_);

    credential_ = new QComboBox(network);
    networkForm_->addRow(fieldLabel(tr("Credential"), network), credential_);
    populateCredentials();

    username_ = new QLineEdit(network);
    networkForm_->addRow(fieldLabel(tr("Username"), network), username_);

    password_ = new QLineEdit(network);
    password_->setEchoMode(QLineEdit::Password);
    networkForm_->addRow(fieldLabel(tr("Password"), network), password_);

    keyPath_ = new QLineEdit(network);
    keyPath_->setPlaceholderText(tr("~/.ssh/id_ed25519 (optional)"));
    keyPath_->setProperty("mono", true);
    networkForm_->addRow(fieldLabel(tr("Private key"), network), keyPath_);

    tofu_ = new QCheckBox(tr("Accept and pin an unknown host key"), network);
    tofu_->setToolTip(
        tr("Trust on first use. A later MISMATCH is still refused."));
    networkForm_->addRow(QString(), tofu_);

    legacy_ = new QCheckBox(tr("Allow legacy algorithms"), network);
    legacy_->setToolTip(
        tr("Adds the old KEX, cipher and MAC set that aging gear requires."));
    networkForm_->addRow(QString(), legacy_);

    pages_->addWidget(network);

    // --- page 1: serial ---------------------------------------------------
    auto *serial = new QWidget(pages_);
    serial->setProperty("bare", true);
    auto *serialForm = new QFormLayout(serial);
    serialForm->setContentsMargins(0, 12, 0, 0);
    styleForm(serialForm);

    serialPort_ = new QComboBox(serial);
    serialForm->addRow(fieldLabel(tr("Port"), serial), serialPort_);

    baud_ = new QComboBox(serial);
    for (const int rate : kBauds) baud_->addItem(QString::number(rate), rate);
    serialForm->addRow(fieldLabel(tr("Baud"), serial), baud_);

    // 8N1 is not offered. Every console port in the building is 8N1, the
    // library defaults to it, and a form with three combo boxes nobody changes
    // is three chances to get it wrong. When something needs 7E1 it can grow a
    // row. Stated rather than left silent, so the absence reads as a decision.
    serialForm->addRow(QString(),
                       descLabel(tr("8 data bits, no parity, 1 stop bit."),
                                 serial));

    pages_->addWidget(serial);
    frame->body()->addWidget(pages_);
    frame->body()->addStretch();

    // --- the decision -----------------------------------------------------
    // Primary on Connect. This dialog asks a question rather than guarding an
    // action -- the same case as the credential prompt -- so the affirmative
    // and the recommended answer are the same button.
    QPushButton *cancel = frame->addButton(tr("Cancel"), ModalFrame::Secondary);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);

    QPushButton *go = frame->addButton(tr("Connect"), ModalFrame::Primary);
    connect(go, &QPushButton::clicked, this, &QDialog::accept);

    frame->setFooterHint(tr("Return connects."));

    connect(transport_, &QButtonGroup::idClicked, this,
            &QuickConnectDialog::onTransportChanged);
    connect(credential_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { onCredentialChanged(); });

    host_->setFocus();
}

void QuickConnectDialog::populateCredentials() {
    credential_->clear();
    credential_->addItem(tr("(type credentials below)"), QString());

    if (!vault_ || !vault_->isOpen()) return;

    if (vault_->isLocked()) {
        // Said, not hidden. A picker that was empty because the vault is
        // locked looks exactly like a picker that is empty because there are
        // no credentials, and the fix for the two is different.
        credential_->addItem(tr("(vault locked — unlock from the Vault menu)"),
                             QString());
        return;
    }

    QVector<omegassh::CredentialMeta> creds;
    if (vault_->list(&creds) != omegassh::VaultError::Ok) return;

    QString defaultName;
    vault_->defaultName(&defaultName);

    for (const omegassh::CredentialMeta &c : creds) {
        // Disabled credentials are still listed and still reachable by name --
        // that is what disabled means in this vault: skipped by automatic
        // selection, not withdrawn. Naming one here is an explicit choice.
        QString label = c.name;
        if (c.name == defaultName) label = tr("%1  (default)").arg(label);
        if (c.disabled) label = tr("%1  (disabled)").arg(label);
        if (!c.hasSecret) label = tr("%1  (no material)").arg(label);
        credential_->addItem(label, c.name);
        if (!c.username.isEmpty()) {
            credential_->setItemData(credential_->count() - 1, c.username,
                                     Qt::ToolTipRole);
        }
    }
}

void QuickConnectDialog::onCredentialChanged() {
    const bool named = !credential_->currentData().toString().isEmpty();

    // The typed fields stay ENABLED beside a chosen credential rather than
    // being greyed out, because explicit fields win over a reference on the Go
    // side -- that is the documented precedence, and a form that disabled them
    // would be hiding a real thing you can do: name a credential for the
    // password and override the username on it.
    username_->setPlaceholderText(named ? tr("from the credential") : QString());
    password_->setPlaceholderText(named ? tr("from the credential") : QString());
}

void QuickConnectDialog::onTransportChanged(int index) {
    const bool serial = index == 2;
    pages_->setCurrentIndex(serial ? 1 : 0);

    if (serial) {
        refreshSerialPorts();
        return;
    }

    const bool ssh = index == 0;

    // The port follows the transport, but only while it still holds the other
    // transport's default. Somebody who typed 2222 meant it.
    if (ssh && port_->value() == 23) port_->setValue(22);
    if (!ssh && port_->value() == 22) port_->setValue(23);

    // Telnet has no authentication step. A login prompt on it is ordinary
    // session data arriving after the socket is up, so these rows are not
    // merely unused -- filling them in would be a lie about where the
    // credentials go. The library refuses them outright.
    //
    // Labels grey with their fields; see setRowEnabled.
    setRowEnabled(networkForm_, credential_, ssh);
    setRowEnabled(networkForm_, username_, ssh);
    setRowEnabled(networkForm_, password_, ssh);
    setRowEnabled(networkForm_, keyPath_, ssh);
    tofu_->setEnabled(ssh);
    legacy_->setEnabled(ssh);
}

void QuickConnectDialog::refreshSerialPorts() {
    const QString previous = serialPort_->currentText();
    serialPort_->clear();

    const QVector<omegassh::SerialPortInfo> ports =
        omegassh::OmegaSshSession::serialPorts();

    for (const omegassh::SerialPortInfo &p : ports) {
        // displayName folds in the vendor and product IDs: two identical FTDI
        // cables in one laptop are otherwise told apart only by which /dev
        // node the kernel happened to hand out.
        serialPort_->addItem(p.displayName(), p.name);
    }

    if (ports.isEmpty()) {
        const QString err = omegassh::OmegaSshSession::lastEnumerationError();
        // Empty is a valid answer and means no adapter, not a failure. The
        // error string is what tells the two apart, and saying which one it is
        // saves somebody checking their cable for ten minutes.
        serialPort_->addItem(err.isEmpty() ? tr("(no serial ports found)")
                                           : tr("(enumeration failed: %1)").arg(err),
                             QString());
        serialPort_->setEnabled(false);
    } else {
        serialPort_->setEnabled(true);
        const int at = serialPort_->findText(previous);
        if (at >= 0) serialPort_->setCurrentIndex(at);
    }
}

omegassh::Config QuickConnectDialog::config() const {
    omegassh::Config cfg;

    switch (transportIndex()) {
        case 1:
            cfg.transport = omegassh::Transport::Telnet;
            cfg.host = host_->text().trimmed();
            cfg.port = port_->value();
            break;
        case 2: {
            cfg.transport = omegassh::Transport::Serial;
            cfg.serialPort = serialPort_->currentData().toString();
            cfg.baud = baud_->currentData().toInt();
            break;
        }
        default:
            cfg.transport = omegassh::Transport::Ssh;
            cfg.host = host_->text().trimmed();
            cfg.port = port_->value();
            cfg.credential = credential_->currentData().toString();
            // Only meaningful together: a name with no handle is a reference
            // the Go side cannot resolve, and it refuses the dial rather than
            // quietly connecting without credentials.
            if (!cfg.credential.isEmpty() && vault_) {
                cfg.vaultHandle = vault_->handle();
            }
            cfg.username = username_->text().trimmed();
            cfg.password = password_->text();
            cfg.privateKeyPath = keyPath_->text().trimmed();
            cfg.hostKeyPolicy = tofu_->isChecked() ? omegassh::HostKeyPolicy::Tofu
                                                   : omegassh::HostKeyPolicy::Strict;
            cfg.legacyAlgorithms = legacy_->isChecked();
            break;
    }

    return cfg;
}

}  // namespace omega::app
