// app/credentialmanagerdialog.h
//
// Phase 4f. List, add, edit, delete, set default, enable/disable.
//
// Built entirely on the metadata surface: the table holds no secret it did not
// just receive from the user, and there is no call it could make to fetch one.
// The vault has no getter for material, which is not an oversight to route
// around here.
//
// Three write paths, and which one runs is decided by what the user did rather
// than by which fields happen to be filled in:
//
//   rename / setDisabled / setDefault  field-at-a-time, read-modify-write in
//                                      Go, cannot touch material
//   updateMetadata                     the editor was saved without material
//   store                              the editor was saved with material
//
// It opens against a LOCKED vault deliberately. Refusing to open would mean
// the only route to unlocking is a menu item somebody has to know about; this
// shows the vault's state and offers the unlock, which is also where a person
// looking for their credentials will go first.

#ifndef OMEGA_APP_CREDENTIALMANAGERDIALOG_H
#define OMEGA_APP_CREDENTIALMANAGERDIALOG_H

#include <QDialog>
#include <QVector>

#include <omegasshvault.h>

class QFrame;
class QLabel;
class QPushButton;
class QTableWidget;

namespace omega::app {

class ModalFrame;

class CredentialManagerDialog : public QDialog {
    Q_OBJECT

public:
    explicit CredentialManagerDialog(omegassh::Vault *vault, QWidget *parent = nullptr);

protected:
    // Where the table is measured; see sizeTable().
    void showEvent(QShowEvent *event) override;

private:
    void buildUi();

    // Re-reads the vault and rebuilds the table. Every action ends here rather
    // than patching the row it touched: the vault decides what the list looks
    // like -- default-first, then by name -- and a locally patched row is a
    // second opinion about that ordering waiting to disagree.
    void refresh();

    void unlock();
    void addCredential();
    void editCredential();
    void renameCredential();
    void deleteCredential();
    void toggleDisabled();
    void makeDefault();
    void clearDefault();

    // Reports a failed call with the code's own sentence plus the detail text.
    // One place, so every action says the same thing about the same code.
    //
    // AN INLINE NOTICE AND NOT A QMessageBox. A failure report is not a
    // question, and a second modal stacked on this one has to be dismissed
    // before the table underneath -- which is the thing the message is about
    // -- can be looked at. The delete confirmation stays a QMessageBox
    // because that one IS a question.
    void report(const QString &action, omegassh::VaultError code);

    // Puts text in the notice under the table, or clears it. Every refresh()
    // clears it first: a complaint about the previous action outliving the
    // action is how a stale message ends up describing a row that no longer
    // exists.
    void showProblem(const QString &text);
    void clearProblem();

    // Sized from the ROW HEIGHT, in showEvent. A QTableWidget's sizeHint is
    // small and ModalFrame sizes the dialog to its body, so without this the
    // manager opens as a two-row slot. Measured rather than a constant
    // because the row height follows ui_font_size, which runs 9 to 28.
    void sizeTable();

    // The selected row's record, or an empty one when nothing is selected.
    omegassh::CredentialMeta selected() const;

    omegassh::Vault *vault_ = nullptr;
    QVector<omegassh::CredentialMeta> rows_;

    ModalFrame *frame_ = nullptr;
    QLabel *vaultPath_ = nullptr;
    QLabel *stateChip_ = nullptr;
    QLabel *defaultName_ = nullptr;
    QFrame *problemBox_ = nullptr;
    QLabel *problem_ = nullptr;
    QTableWidget *table_ = nullptr;
    bool tableSized_ = false;
    QPushButton *unlock_ = nullptr;
    QPushButton *add_ = nullptr;
    QPushButton *edit_ = nullptr;
    QPushButton *rename_ = nullptr;
    QPushButton *delete_ = nullptr;
    QPushButton *default_ = nullptr;
    QPushButton *disable_ = nullptr;
};

}  // namespace omega::app

#endif  // OMEGA_APP_CREDENTIALMANAGERDIALOG_H
