// app/settingsdialog.h
//
// The settings form. Roadmap 4e, and the last piece of the app shell.
//
// TWO FILES BEHIND ONE DIALOG. Everything here writes either ~/.omega/
// config.json, which keeps nterm-qt's schema, or ~/.omega/omega.json, which is
// Omega's alone -- see omegasettings.h for why a single file is not an
// option. The split is not shown to the user, because "which of two
// applications owns this preference" is not a question anybody opening a
// settings dialog is asking. It is visible in the code: appSettings() and
// omegaSettings() come back separately and the caller saves both.
//
// WHAT IS DELIBERATELY NOT HERE. config.json carries fifteen fields and this
// form offers five of them:
//
//   window_*, tree_width      Window state, not preferences. A dialog that
//                             let you type a window position would be a
//                             worse way to move a window.
//   recent_profiles,          Nothing populates the recent list yet.
//   max_recent                addRecentProfile() exists and only the probe
//                             calls it, so a cap on it would cap nothing.
//   default_keepalive_        Keepalive is a socket option set in
//   interval                  sshcore/dial.go and nothing reads this field.
//                             A control here would be a number with no
//                             effect, which is worse than an absent one.
//   auto_reconnect            Nothing implements reconnect yet. Same
//                             reasoning.
//
// THE CONNECTION GROUP is omega.json's, and it is here rather than in the
// session editor because it answers for every session at once. A store
// imported from TerminalTelemetry has credential_name NULL on all of them,
// and the alternative to a global was editing each one.
//
// The last two are stored and preserved on save either way -- fromJson keeps
// what it read -- so nterm-qt's values survive a trip through this dialog
// untouched. They get controls when the features behind them exist.
//
// LIVE THEME PREVIEW is a signal rather than a return value, because seeing
// the theme means seeing it on the whole window and not on a swatch. The
// dialog emits as the selection moves; the window applies. Cancel is
// therefore the window's problem to undo, and MainWindow::openSettings holds
// the name to go back to.

#ifndef OMEGA_APP_SETTINGSDIALOG_H
#define OMEGA_APP_SETTINGSDIALOG_H

#include <QDialog>
#include <QString>

#include "app/omegasettings.h"
#include "app/settings.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QFormLayout;
class QFrame;
class QSpinBox;
class QTabWidget;

namespace omega::theme {
class ThemeEngine;
}

namespace omega::app {

class ModalFrame;

class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    SettingsDialog(const AppSettings &shared, const OmegaSettings &omega,
                   theme::ThemeEngine *themes, QWidget *parent = nullptr);

    // The edited copies. Only meaningful after exec() returned Accepted; on
    // reject they hold whatever was typed before Cancel, which is why the
    // caller must check.
    AppSettings appSettings() const;
    OmegaSettings omegaSettings() const;

signals:
    // Emitted as the theme selection moves, including on the way back to
    // where it started. The window applies it to itself and every tab.
    void themePreviewRequested(const QString &themeName);

protected:
    // Where the tab pages' label columns are measured; see
    // alignFieldLabels() in modalframe.h for why it cannot be the constructor.
    void showEvent(QShowEvent *event) override;

private:
    void buildUi(theme::ThemeEngine *themes);
    void loadFrom(const AppSettings &shared, const OmegaSettings &omega);

    // Custom-hex is only meaningful for the custom keystroke, and an interval
    // is only meaningful when anti-idle is on. Greying rather than hiding:
    // a form that changes height as you tick boxes is harder to use than one
    // with a dim row in it.
    void updateEnabledState();

    // Says what the hex means, or that it is not usable. A custom keystroke
    // that silently sends nothing is the failure this exists to prevent.
    void updateHexFeedback();

    // --- appearance ---
    QComboBox *theme_ = nullptr;
    QComboBox *chrome_ = nullptr;
    QComboBox *titleBar_ = nullptr;
    // omega.json's, and the chrome's. fontSize_ below is config.json's and
    // the terminal's -- two different fonts read at two different distances,
    // which is why they are two rows and not one.
    QSpinBox *uiFontSize_ = nullptr;
    QSpinBox *fontSize_ = nullptr;

    // --- terminal ---
    QSpinBox *scrollback_ = nullptr;
    QSpinBox *pasteThreshold_ = nullptr;
    QComboBox *termType_ = nullptr;

    // --- connection ---
    QComboBox *sshDefaultAuth_ = nullptr;

    // --- anti-idle ---
    QCheckBox *antiIdleEnabled_ = nullptr;
    QCheckBox *wheelAltScreen_ = nullptr;
    QSpinBox *antiIdleSeconds_ = nullptr;
    QComboBox *antiIdleKeystroke_ = nullptr;
    QLineEdit *antiIdleHex_ = nullptr;
    QLabel *antiIdleHexNote_ = nullptr;

    // Held so the row LABELS can be greyed alongside their fields. Disabling
    // a QSpinBox alone leaves "After (seconds)" at full contrast beside it,
    // and under a light theme's stylesheet that reads as an enabled row.
    QFormLayout *antiIdleForm_ = nullptr;

    // The hex problem notice, distinct from antiIdleHexNote_. The note says
    // what a valid hex string WILL send; the notice says the string cannot
    // send anything. Two different sentences with two different urgencies, and
    // one label carrying both meant the failure case rendered in the same dim
    // grey as the success case.
    QFrame *hexProblem_ = nullptr;
    QLabel *hexProblemText_ = nullptr;

    ModalFrame *frame_ = nullptr;
    QTabWidget *tabs_ = nullptr;
    bool labelsAligned_ = false;

    // Everything the form does not show, kept so save() writes back what was
    // read rather than what this dialog happens to know about.
    AppSettings shared_;
    OmegaSettings omega_;
};

}  // namespace omega::app

#endif  // OMEGA_APP_SETTINGSDIALOG_H