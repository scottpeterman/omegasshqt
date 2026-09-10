// app/effectiveconfig.h
//
// Where a session's settings actually come from.
//
// Three layers, and the order never varies: the SESSION's own value if it has
// one, else the APPLICATION SETTINGS, else the struct's own default. A field
// the session leaves NULL keeps following the global for the life of the
// session -- which is the whole reason sessions::Session holds optionals
// rather than values, and the reason this resolution is a function rather
// than a handful of if-statements spread through MainWindow.
//
// WHY IT IS ONE FUNCTION. Before this, openTab() applied term type, paste
// threshold, scrollback, font size and anti-idle to the tab one call at a
// time, and openSession() built the Config separately. Adding per-session
// overrides that way would have meant two places to remember for every new
// field, and the failure mode is silent: a field wired in one path and missed
// in the other looks like it works, until it is reached from the other menu.
//
// WHY IT TAKES NO VAULT. The handle is an argument, so this is a pure
// function of three structs and an integer -- which is what makes
// session_attrs_probe able to check every branch without a vault, a database
// or a display.
//
// Quick connect resolves through here too. Its dialog produces a Config where
// every field was chosen explicitly, so there is nothing to inherit from the
// session layer -- but the TAB settings (scrollback, threshold, font,
// anti-idle) still come from the globals, and those are the same globals. One
// function, two entry points.

#ifndef OMEGA_APP_EFFECTIVECONFIG_H
#define OMEGA_APP_EFFECTIVECONFIG_H

#include <omegasshsession.h>

#include "app/antiidle.h"
#include "app/omegasettings.h"
#include "app/settings.h"
#include "sessions/store.h"

namespace omega::app {

// The bounds the terminal font is clamped to, in points.
//
// Here rather than in terminaltab.cpp, where they used to be file-local,
// because three things now need to agree about them: the tab that clamps
// Ctrl+wheel against them, the session editor's font row, and anything that
// validates a stored font_size. A spin box that offers 60 pt and a zoom that
// refuses to reach it is the disagreement this prevents.
//
// Settings > Font size hard-codes the same 6 and 48 in settingsdialog.cpp and
// is left alone here -- worth folding in, but it is a separate edit.
constexpr int kMinTerminalFontPointSize = 6;
constexpr int kMaxTerminalFontPointSize = 48;

// The settings a TerminalTab takes that are not part of the transport config.
// Grouped rather than passed as five arguments so a new one is added in one
// place and every caller gets it.
struct TabSettings {
    int scrollbackLines = 10000;
    int multilinePasteThreshold = 1;
    int fontPointSize = 14;
    AntiIdleConfig antiIdle;

    // -1 means "let the tab pick from the transport" -- see
    // defaultPasteBaud() in terminaltab.cpp, which takes the serial line's own
    // rate, 9600 for telnet, and unpaced for ssh. A session that overrides it
    // yields 0 or more, and 0 is a real value meaning unpaced.
    int pasteBaud = -1;

    // Wheel over a full-screen application scrolls that application rather
    // than the scrollback behind it. See OmegaSettings::wheel_alt_screen.
    bool wheelAltScreen = true;

    // The terminal's colour theme BY NAME, or empty to follow the window.
    //
    // A name rather than a resolved theme because this function has no
    // ThemeEngine and should not need one -- resolution belongs to the window
    // that owns the engine, and keeping it out of here is what lets
    // session_attrs_probe check the whole resolution without a theme
    // directory. See MainWindow::themeForTab for the other half.
    //
    // Empty is not "the default theme", it is "follow the window", and the
    // difference shows the moment the window's theme changes: a tab that
    // inherits repaints with it, a tab that pinned the same name by hand does
    // not. That is the same absent-is-not-a-value rule as everywhere here,
    // and it is why this is a QString that can be empty rather than a name
    // defaulted to settings.theme_name.
    QString themeName;
};

// Everything openTab needs. The Config goes to the transport, the TabSettings
// to the widget, and nothing else is read from settings downstream of here.
struct Resolved {
    omegassh::Config config;
    TabSettings tab;

    // What the tab is CALLED, which is not what it connects to. A saved
    // session has a name somebody chose; the session summary the transport
    // builds is user@host:port, and on a store addressed by IP that makes
    // every tab a number. Empty for quick connect, which has no name to use
    // and falls back to the summary -- see TerminalTab::title().
    QString displayName;
};

// A saved session. vaultHandle is paired with a credential name and ignored
// without one: a reference the Go side cannot resolve is refused before the
// socket opens, so the two are set together or not at all.
//
// defaultCredential is the vault's default credential BY NAME, or empty. It
// is a string rather than a vault pointer for the reason at the top of this
// file -- the caller reads it (MainWindow::openSession does, from
// Vault::defaultName()) and this function stays checkable without one.
//
// It is used only under SshDefaultAuth::VaultDefault, only for SSH, and only
// when the session names no credential of its own. Empty there is not an
// error: it means the vault is locked, absent, or holds no default, and the
// caller prompts instead. This function does not prompt, so a Resolved that
// comes back with no credential under that mode is the signal to.
// The tab half of resolveSession, on its own.
//
// It exists because the settings dialog pushes new globals over tabs that are
// ALREADY OPEN, and did so by copying six fields straight out of AppSettings
// -- which discarded every per-session override on every open tab the moment
// anybody pressed Save in Settings, and put them back only on reconnect. The
// override was not lost from the database, so the session looked correct next
// time and the report was always "it forgets sometimes".
//
// Splitting this out is what makes the live path and the open path share one
// description of inheritance rather than two. See applySettingsToTabs().
//
// No vault and no Config: the tab settings do not need either, and the live
// path should not be resolving credentials to change a font.
TabSettings resolveTabSettings(const sessions::Session &session,
                               const AppSettings &settings,
                               const OmegaSettings &omega);

// The globals alone, for a tab with no saved session behind it -- quick
// connect. Same values resolveTabSettings starts from.
TabSettings resolveTabSettings(const AppSettings &settings,
                               const OmegaSettings &omega);

Resolved resolveSession(const sessions::Session &session,
                        const AppSettings &settings,
                        const OmegaSettings &omega,
                        long long vaultHandle,
                        const QString &defaultCredential = QString());

// Quick connect. The dialog's Config passes through untouched except for term
// type, which the dialog does not ask for; the tab settings come from the
// globals exactly as they did before this file existed.
Resolved resolveQuickConnect(const omegassh::Config &dialogConfig,
                             const AppSettings &settings,
                             const OmegaSettings &omega);

// Text spellings for the host_key_policy column. Round-trips both ways, and
// an unrecognised name yields Strict -- the library's own default, and the
// safe direction to fail in.
const char *hostKeyPolicyName(omegassh::HostKeyPolicy policy);
omegassh::HostKeyPolicy hostKeyPolicyFromName(const std::string &name,
                                              bool *known = nullptr);

}  // namespace omega::app

#endif  // OMEGA_APP_EFFECTIVECONFIG_H