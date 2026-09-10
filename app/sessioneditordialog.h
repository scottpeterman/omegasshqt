// app/sessioneditordialog.h
//
// The saved-session form: identity, transport, and the per-session overrides.
//
// A port of manager/editor.py's SessionEditorDialog, and no longer a close
// one. That form had six fields because the schema had six worth editing;
// this one has the attribute columns added in the same pass as
// app/effectiveconfig.h, which is most of what it does.
//
// The credential picker holds NAMES. nterm-qt's editor stored a name too, but
// its connect path then resolved the credential in-process and built a profile
// holding the password; Omega passes the name down with a vault handle and the
// material is read on the Go side during the dial. So this form never asks the
// vault for anything but a list of names.
//
// ABSENT IS A CHOICE HERE, NOT AN EMPTY FIELD. Every override row can say
// "inherit", and inherit is what it says -- with the value being inherited
// shown beside it, so the form answers "what will this actually do" without a
// trip to Settings. That is why the constructor takes the two settings
// structs: not to write them, only to label the inherit rows with what they
// currently hold. The three widget idioms are all in the .cpp, and each one
// keeps absence distinct from every value the field can take:
//
//   text     empty line edit, with the inherited value as placeholder
//   number   spin box one below its minimum, with a special value text
//   flag     three-item combo -- inherit, on, off -- never a two-state check
//
// A tristate QCheckBox would have been the obvious answer for the last one and
// is the wrong one: partially-checked reads as "some of the things below are
// on", not "this follows the global", and nobody guesses which.
//
// SERIAL AND TELNET ARE SAVED SESSION TYPES NOW. They were quick-connect only
// while the schema had to stay unchanged for nterm-qt, which is retired. The
// transport row switches the form between a network target and a serial line.

#ifndef OMEGA_APP_SESSIONEDITORDIALOG_H
#define OMEGA_APP_SESSIONEDITORDIALOG_H

#include <QDialog>

#include <optional>

#include <omegasshvault.h>

#include "app/omegasettings.h"
#include "app/settings.h"
#include "sessions/store.h"
#include "theme/theme.h"

class QButtonGroup;
class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QSpinBox;
class QStackedWidget;
class QTabWidget;

namespace omega::app {

class ModalFrame;

class SessionEditorDialog : public QDialog {
    Q_OBJECT

public:
    // `session` is read, never written -- session() returns the edited copy,
    // so a cancelled dialog cannot have touched anything. `store` is read for
    // the folder list only.
    //
    // vault may be null, locked, or empty. All three leave the picker on its
    // inherit row with the reason shown, the same as the quick-connect
    // dialog: a picker that is empty because the vault is locked must not look
    // like a picker that is empty because there are no credentials.
    //
    // settings and omega are the globals, and they are LABELS. Nothing here
    // writes to either; they exist so an inherit row can say what it inherits.
    // themes is READ, like settings and omega: the theme row needs the list of
    // installed names to offer, and the inherit row needs the global's name to
    // label itself with. Null is allowed and leaves the row with inherit as
    // its only entry -- a picker with nothing in it must not look like an
    // installation with no themes.
    SessionEditorDialog(const sessions::Session &session,
                        sessions::SessionStore *store, omegassh::Vault *vault,
                        const AppSettings &settings, const OmegaSettings &omega,
                        const theme::ThemeEngine *themes = nullptr,
                        QWidget *parent = nullptr);

    // The edited session. id, extras, created_at, last_connected and
    // connect_count are carried through from what was passed in: the form does
    // not own them, and extras in particular is opaque by contract -- the
    // attribute columns are columns precisely so nothing has to go in it.
    sessions::Session session() const;

protected:
    // Where the three tabs' label columns are measured; see alignFieldLabels()
    // in modalframe.h for why it cannot be the constructor.
    void showEvent(QShowEvent *event) override;

private:
    void buildUi();
    QWidget *buildConnectionPage();
    QWidget *buildTerminalPage();
    QWidget *buildAdvancedPage();

    void populateCredentials(QComboBox *box, bool isJumpHost);

    // Keeps the username placeholder honest: it says "from the credential"
    // only when there is one.
    void onCredentialChanged();
    void populateFolders();
    void refreshSerialPorts();
    QString currentSerialName() const;
    void onTransportChanged(int index);
    void onKeystrokeChanged();
    void loadFrom(const sessions::Session &session);
    void onAccept();

    // The failure state, the shape the rest of the pass settled on: words in
    // the notice, a mark on the field, and the tab holding that field brought
    // to the front.
    //
    // THIS REPLACES A SILENT REJECTION. onAccept used to answer an invalid
    // form by switching tab and moving focus and saying nothing at all -- the
    // argument in the old comment was that a modal complaining about a field
    // you can see is empty adds a click and no information, which is a good
    // argument against a QMessageBox and not one against an inline notice.
    // Pressing Save and having the dialog silently change tabs is the worst
    // of both: something happened, nothing was said.
    void showProblem(const QString &text, QWidget *field, int tab);
    void clearProblem();
    void markInvalid(QWidget *field, bool invalid);

    // Which transport is selected: 0 SSH, 1 Telnet, 2 Serial, the same index
    // transportAt() has always taken.
    int selectedTransport() const;

    sessions::SessionStore *store_ = nullptr;
    omegassh::Vault *vault_ = nullptr;
    const theme::ThemeEngine *themes_ = nullptr;
    const AppSettings &settings_;
    const OmegaSettings &omega_;

    // The row as it arrived, for the fields the form does not edit.
    sessions::Session original_;

    ModalFrame *frame_ = nullptr;
    QTabWidget *tabs_ = nullptr;
    QFrame *problemBox_ = nullptr;
    QLabel *problem_ = nullptr;
    QWidget *invalidField_ = nullptr;
    bool labelsAligned_ = false;

    // --- identity and filing ----------------------------------------------
    QLineEdit *name_ = nullptr;
    QLineEdit *description_ = nullptr;
    QComboBox *folder_ = nullptr;

    // --- transport --------------------------------------------------------
    // A SEGMENTED CONTROL, as on quick connect. Three answers, chosen on
    // nearly every visit to this form, and the one control here that decides
    // which other half of the page is even reachable -- a combo box put two of
    // the three behind a click and gave no hint that choosing changed the
    // page below it.
    QButtonGroup *transport_ = nullptr;
    QStackedWidget *pages_ = nullptr;

    // network page
    QLineEdit *host_ = nullptr;
    QSpinBox *port_ = nullptr;
    QComboBox *credential_ = nullptr;
    QLineEdit *username_ = nullptr;

    // serial page
    QComboBox *serialPort_ = nullptr;
    QComboBox *baud_ = nullptr;

    // --- terminal ---------------------------------------------------------
    QLineEdit *term_ = nullptr;
    QSpinBox *scrollback_ = nullptr;
    QSpinBox *fontSize_ = nullptr;
    QComboBox *theme_ = nullptr;
    QSpinBox *pasteThreshold_ = nullptr;
    QComboBox *pasteBaud_ = nullptr;

    // --- anti-idle --------------------------------------------------------
    QComboBox *antiIdleEnabled_ = nullptr;
    QSpinBox *antiIdleSeconds_ = nullptr;
    QComboBox *antiIdleKeystroke_ = nullptr;
    QLineEdit *antiIdleCustom_ = nullptr;

    // --- ssh and telnet ---------------------------------------------------
    QComboBox *hostKeyPolicy_ = nullptr;
    QComboBox *legacy_ = nullptr;
    QComboBox *telnetCrlf_ = nullptr;
    QComboBox *wheelAltScreen_ = nullptr;

    // --- jump host --------------------------------------------------------
    QLineEdit *jumpHost_ = nullptr;
    QSpinBox *jumpPort_ = nullptr;
    QLineEdit *jumpUsername_ = nullptr;
    QComboBox *jumpCredential_ = nullptr;
};

}  // namespace omega::app

#endif  // OMEGA_APP_SESSIONEDITORDIALOG_H