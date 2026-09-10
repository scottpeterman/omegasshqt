// app/omegasettings.h
//
// Settings that are Omega's alone, in ~/.omega/omega.json.
//
// WHY A SECOND FILE. config.json is shared with nterm-qt, whose from_dict()
// filters to the fields its dataclass declares -- so a key Omega added there
// would be deleted the first time the other application saved. That is not a
// guess: tests/compat/settings_differential.py asserts it, in both
// directions. app/settings.h says as much and names this file as the answer;
// this is that file.
//
// The split is by OWNERSHIP, not by importance. A setting goes in config.json
// when nterm-qt has a field for it and the two applications should agree
// (theme, paste threshold, scrollback, geometry). It goes here when nterm-qt
// has no concept of it. auto_reconnect will straddle the two when it lands --
// the flag is nterm-qt's, the backoff and attempt cap will be ours.
//
// WRITING IS ORDINARY QJsonDocument HERE, deliberately unlike AppSettings.
// That class hand-rolls its writer to emit Python's exact bytes, so two
// applications alternating saves leave the file untouched. Nothing else reads
// this one, so there is no second writer to match and no reason to carry a
// serialiser that has to be kept honest by a differential.
//
// FORGIVING LOAD, same contract as SettingsManager: a missing file and an
// unparseable one both yield defaults, because neither is a reason to refuse
// to start. A field of the wrong type is left at its default and named in
// warnings() rather than failing the file.

#ifndef OMEGA_APP_OMEGASETTINGS_H
#define OMEGA_APP_OMEGASETTINGS_H

#include <QString>
#include <QStringList>

#include "app/antiidle.h"

namespace omega::app {

// What an SSH session that names no credential dials with.
//
// WHY THIS IS A SETTING. credential_name is NULL on every session imported
// from TerminalTelemetry and on every session saved without picking one, and
// the application used to answer that state one way for everybody. A store of
// a hundred sessions cannot be re-answered by editing a hundred rows, and the
// right answer differs by site: an operator with an agent wants the agent, an
// operator with a vault wants the vault.
//
// VaultDefault is the shipped default. It is the only one of the three that
// uses the default-credential machinery the vault already carries, and a
// vault holding no default falls through to Ask rather than dialing with
// nothing -- see MainWindow::openSession.
enum class SshDefaultAuth {
    VaultDefault,  // the vault's default credential, by name
    Ask,           // prompt for a credential each connect
    Agent,         // SSH agent (SSH_AUTH_SOCK; no-op on Windows for now)
};

// Canonical strings for omega.json. Round-trips both ways; an unrecognised
// name yields VaultDefault and sets *known false so the loader can say so.
const char *sshDefaultAuthName(SshDefaultAuth mode);
SshDefaultAuth sshDefaultAuthFromName(const QString &name, bool *known = nullptr);

// For a combo row or a label. Not the same strings as above: those are stored
// and must stay stable, these are read.
QString sshDefaultAuthLabel(SshDefaultAuth mode);

// Which chrome the window wears.
//
// WHY THIS IS A SETTING RATHER THAN A SWITCH FLIPPED ONCE. The two sheets are
// not two skins of one design; Classic is the byte-for-byte port of nterm-qt's
// stylesheet.py and Token is the redesign. Shipping both for a while is what
// lets the redesign be looked at on real sessions, on all 33 themes, without
// the old chrome becoming unreachable the day something in the new one turns
// out to be wrong on somebody's window manager.
//
// It lives in omega.json and not in config.json because nterm-qt has no
// concept of it and would delete the key -- the same reason everything else in
// this file is in this file.
//
// Token is the default. Classic is the fallback, and when it is finally
// removed this enum and theme/stylesheet.cpp go together.
enum class Chrome {
    Token,    // theme/tokenstylesheet.h, the redesign
    Classic,  // theme/stylesheet.h, the nterm-qt port
};

// Canonical strings for omega.json, same contract as the auth mode above: an
// unrecognised name yields the default and sets *known false.
const char *chromeName(Chrome chrome);
Chrome chromeFromName(const QString &name, bool *known = nullptr);
QString chromeLabel(Chrome chrome);

// Whether the window wears the compositor's title bar or its own.
//
// Merged puts the menus, the vault pill and the window buttons in one 40px
// strip via QMainWindow::setMenuWidget, and takes Qt::FramelessWindowHint --
// which also takes the resize handles, so MainWindow grows an event filter to
// put them back. Native leaves all of that to the window manager.
//
// THE DEFAULT IS PLATFORM-DEPENDENT and that is not hedging. On macOS the
// traffic lights are drawn by the system at a fixed position and a merged bar
// either collides with them or leaves a gap where they were; the platform's
// own frame is the right answer there and Native is the default. Everywhere
// else Merged is.
//
// Forced to Native when chrome is Classic. The merged bar is a set of widgets
// styled by role selectors that only the token sheet carries, so the
// combination is not a second look, it is an unstyled one.
enum class TitleBar {
    Merged,  // Omega's own bar, frameless window
    Native,  // the window manager's
};

const char *titleBarName(TitleBar bar);
TitleBar titleBarFromName(const QString &name, bool *known = nullptr);
QString titleBarLabel(TitleBar bar);

// What Merged means on this platform, before any file is read.
TitleBar defaultTitleBar();

struct OmegaSettings {
    // Sent to the far end after a period with nothing written, to stop a
    // device's own idle timer closing the session. See antiidle.h for why the
    // keystroke is a setting and not a constant.
    AntiIdleConfig anti_idle;

    // Wheel over a full-screen application scrolls THAT application rather
    // than the local scrollback behind it.
    //
    // ON by default, because the alternative is not "no wheel" but a wheel
    // that scrolls the wrong thing: the alternate screen keeps its own buffer
    // and leaves the scrollback untouched, so a notch inside vi or a pager
    // moves history from BEFORE the application was launched, under a screen
    // still showing the application. Nothing about that is useful and it is
    // not obviously a bug when it happens.
    //
    // Per-session override in sessions::Session::wheel_alt_screen, resolved in
    // effectiveconfig.cpp. Here rather than in config.json because it is
    // Omega's alone.
    bool wheel_alt_screen = true;

    // Applies to SSH only, and only when the session names no credential. A
    // session that names one is unaffected by this in every mode.
    SshDefaultAuth ssh_default_auth = SshDefaultAuth::VaultDefault;

    // Which stylesheet generator paints the window. See Chrome.
    Chrome chrome = Chrome::Token;

    // Merged or the window manager's. See TitleBar.
    TitleBar title_bar = defaultTitleBar();

    // Body text size for the CHROME -- menus, tree, dialogs, status bar. Not
    // the terminal, which has config.json's font_size and is nterm-qt's field.
    //
    // IN PIXELS, matching the unit QSS speaks. The token sheet used to carry
    // 13px as a literal, and a QSS font-size beats the widget's own font, so
    // QApplication::setFont() moved nothing the sheet reached. Both are driven
    // from this one number now: the sheet is generated with it and the
    // application font is set with QFont::setPixelSize, so they cannot drift
    // apart by a DPI conversion.
    //
    // 13 is what the sheet had hardcoded, so the default changes nothing.
    // Lives here rather than in config.json because nterm-qt has no concept of
    // it and its from_dict() would delete the key.
    int ui_font_size = 13;

    // The effective answer, which is not always the stored one: a merged bar
    // on the classic sheet would be unstyled, so Classic forces Native. One
    // function so the window, the settings dialog and the command line cannot
    // disagree about it.
    TitleBar effectiveTitleBar() const {
        return chrome == Chrome::Classic ? TitleBar::Native : title_bar;
    }

    QString toJson() const;

    static OmegaSettings fromJson(const QByteArray &text, QStringList *warnings);
};

class OmegaSettingsManager {
public:
    explicit OmegaSettingsManager(const QString &path = defaultFile());

    OmegaSettings &settings() { return settings_; }
    const OmegaSettings &settings() const { return settings_; }

    // Reads the file if it is there. Returns false only when the file existed
    // and could not be read or parsed; not finding one is success.
    bool load();

    // Creates ~/.omega if needed and writes the file.
    bool save();

    const QString &error() const { return error_; }
    const QStringList &warnings() const { return warnings_; }
    const QString &path() const { return path_; }

    // ~/.omega/omega.json -- beside config.json, sessions.db and the vault,
    // because a user with one nterm directory should not acquire a second.
    static QString defaultFile();

private:
    QString path_;
    OmegaSettings settings_;
    QString error_;
    QStringList warnings_;
};

}  // namespace omega::app

#endif  // OMEGA_APP_OMEGASETTINGS_H