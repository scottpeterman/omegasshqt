// app/quickconnectdialog.h
//
// Quick connect: the transport, the target, and whatever credentials the
// operator types.
//
// Quick connect is REAL USE, not a fallback. It is how a device that is not in
// the session store gets reached -- a console server nobody has added yet, a
// box someone read an address off a label for. That is why the manual fields
// are first-class here rather than a degraded path around the vault, and it is
// the same reason Phase 0 kept explicit credentials winning over a reference.
//
// SERIAL AND TELNET ARE QUICK CONNECT ONLY. Neither is a saved session type,
// which is what keeps the session store's schema unchanged in both directions
// and the verified store verified. This dialog is the only way to reach them.
//
// Nothing here holds a stored secret. A password typed into this form is one
// the operator just typed, which is the case the boundary was always going to
// carry; a credential from the vault crosses as a NAME and is read on the Go
// side during the dial.

#ifndef OMEGA_APP_QUICKCONNECTDIALOG_H
#define OMEGA_APP_QUICKCONNECTDIALOG_H

#include <QDialog>

#include <omegasshsession.h>
#include <omegasshvault.h>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QFormLayout;
class QLineEdit;
class QSpinBox;
class QStackedWidget;

namespace omega::app {

class QuickConnectDialog : public QDialog {
    Q_OBJECT

public:
    // vault may be null, locked, or hold nothing. All three leave the picker
    // showing "(type credentials below)" and the manual fields working, which
    // is quick connect's whole premise: it is how a device that is not in the
    // store gets reached, not a degraded path around the vault.
    explicit QuickConnectDialog(omegassh::Vault *vault, QWidget *parent = nullptr);

    // The assembled config. Only meaningful after exec() returned Accepted.
    omegassh::Config config() const;

private:
    void buildUi();
    void onTransportChanged(int index);
    void refreshSerialPorts();
    void populateCredentials();
    void onCredentialChanged();

    // Which transport is selected, as the index config() has always switched
    // on: 0 SSH, 1 Telnet, 2 Serial. Read from the button group rather than a
    // combo's currentIndex, so the switch statement below is untouched.
    int transportIndex() const;

    omegassh::Vault *vault_ = nullptr;

    // A SEGMENTED CONTROL, NOT A COMBO BOX. This is the one question the
    // dialog asks every single time it opens, it has exactly three answers,
    // and a combo box hid two of them behind a click. QPushButton[segment]
    // has been in the sheet since the redesign with no consumer; this is what
    // it was written for.
    QButtonGroup *transport_ = nullptr;
    QStackedWidget *pages_ = nullptr;

    // Kept so the label of a disabled row can be greyed with it. QFormLayout
    // knows which label belongs to which field; nothing else does.
    QFormLayout *networkForm_ = nullptr;

    // --- ssh and telnet ---------------------------------------------------
    QLineEdit *host_ = nullptr;
    QSpinBox *port_ = nullptr;
    QLineEdit *username_ = nullptr;
    QLineEdit *password_ = nullptr;
    QLineEdit *keyPath_ = nullptr;

    // Holds a NAME, never a secret. The name goes down with the connection
    // parameters and the material is read on the Go side during the dial.
    QComboBox *credential_ = nullptr;

    // Off by default and named for what it is. Trust-on-first-use pins the key
    // and still refuses a later mismatch, which is the honest middle; the
    // library's own default is strict.
    QCheckBox *tofu_ = nullptr;

    // The old KEX/cipher/MAC tail aging gear still requires. Harmless against
    // a modern server, and the difference between connecting and not against
    // a switch that has been in a rack for eleven years.
    QCheckBox *legacy_ = nullptr;

    // --- serial -------------------------------------------------------------
    QComboBox *serialPort_ = nullptr;
    QComboBox *baud_ = nullptr;

    mutable omegassh::Config config_;
};

}  // namespace omega::app

#endif  // OMEGA_APP_QUICKCONNECTDIALOG_H
