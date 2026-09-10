// tests/compat/shell_probe.cpp
//
// Drives the shell window with no display, so the one claim 4a makes that
// cannot be checked by reading -- that geometry survives a close and a reopen
// -- is checked by doing it.
//
//   shell_probe <themes-dir> <config.json> <width> <height> <x> <y> <tree>
//
// Opens the window, applies that geometry, closes it, then reopens and prints
// what the second window came back with. Compare the two lines: a settings
// layer that saved nothing, or a window that ignored what it loaded, shows up
// as a mismatch rather than as a file nobody read.
//
// Runs under -platform offscreen. Deliberately not xdotool and a window
// manager: closing a window from outside needs a WM to deliver
// WM_DELETE_WINDOW, and requiring one to test the shell would put a package on
// the build machine to check something the application already does to itself.
//
// It also prints what the settings dialog gives back for settings it was
// handed and nobody touched. A form has one failure mode that reading it will
// not show -- a control wired to the wrong field, or one the accept path
// forgets -- and both come out here as a value that changed on the way
// through. No input is simulated: construct, read back, compare.

#include <QApplication>
#include <QDir>
#include <QSplitter>
#include <QStringList>

#include "app/mainwindow.h"
#include "app/omegasettings.h"
#include "app/settings.h"
#include "app/settingsdialog.h"
#include "theme/theme.h"

#include <cstdio>

using namespace omega::app;

static QString probeVaultPath() {
    return QDir::temp().filePath(QStringLiteral("omega-shell-probe-vault.json"));
}

// Likewise for the session store. 4d gave the window one, and its default is
// ~/.omega/sessions.db -- the file the user's real tree lives in. A geometry
// test has no business opening it, for the same reason it does not open the
// real vault.
static QString probeSessionPath() {
    return QDir::temp().filePath(QStringLiteral("omega-shell-probe-sessions.db"));
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 8) {
        std::fprintf(stderr,
                     "usage: shell_probe <themes> <config> <w> <h> <x> <y> <tree>\n");
        return 2;
    }

    const QString themesDir = args.at(1);
    const QString configPath = args.at(2);

    omega::theme::ThemeEngine themes;
    themes.loadDirectory(themesDir.toStdString());

    // --- first window: place it, then close it ---------------------------
    {
        SettingsManager settings(configPath);
        settings.load();
        // A throwaway vault path, never the real one. The window quiet-unlocks
        // on construction, and a geometry test has no business reaching into
        // whoever is running it's OS keyring.
        MainWindow w(&settings, &themes, nullptr, probeVaultPath(),
                     probeSessionPath());
        w.show();
        w.resize(args.at(3).toInt(), args.at(4).toInt());
        w.move(args.at(5).toInt(), args.at(6).toInt());
        w.setTreeWidth(args.at(7).toInt());
        app.processEvents();
        w.close();
    }

    // --- second window: report what it restored ---------------------------
    SettingsManager settings(configPath);
    settings.load();
    for (const QString &warning : settings.warnings()) {
        std::fprintf(stderr, "warning: %s\n", qPrintable(warning));
    }
    MainWindow w(&settings, &themes, nullptr, probeVaultPath(),
                     probeSessionPath());
    w.show();
    app.processEvents();

    const AppSettings &s = settings.settings();
    std::printf("saved width=%d height=%d x=%d y=%d tree=%d maximized=%s\n",
                s.window_width, s.window_height,
                s.window_x ? *s.window_x : -1, s.window_y ? *s.window_y : -1,
                s.tree_width, s.window_maximized ? "True" : "False");
    std::printf("restored width=%d height=%d tree=%d theme=%s\n",
                w.width(), w.height(), w.treeWidth(),
                qPrintable(w.currentThemeName()));

    // --- the settings form, untouched -------------------------------------
    OmegaSettings omega;
    omega.anti_idle.enabled = true;
    omega.anti_idle.seconds = 45;
    omega.anti_idle.keystroke = AntiIdleKeystroke::SpaceBackspace;
    omega.ssh_default_auth = SshDefaultAuth::Ask;

    SettingsDialog dialog(s, omega, &themes);
    const AppSettings back = dialog.appSettings();
    const OmegaSettings omegaBack = dialog.omegaSettings();

    std::printf("dialog theme=%s font=%d scrollback=%d paste=%d term=%s\n",
                qPrintable(back.theme_name), back.font_size,
                back.scrollback_lines, back.multiline_paste_threshold,
                qPrintable(back.default_term_type));

    // The fields the form does not show must come back as they went in, or a
    // trip through the dialog silently resets somebody's window position.
    std::printf("dialog untouched keepalive=%d reconnect=%s recent=%d tree=%d\n",
                back.default_keepalive_interval,
                back.auto_reconnect ? "True" : "False",
                static_cast<int>(back.recent_profiles.size()), back.tree_width);

    std::printf("dialog antiidle enabled=%s seconds=%d keystroke=%s\n",
                omegaBack.anti_idle.enabled ? "True" : "False",
                omegaBack.anti_idle.seconds,
                qPrintable(keystrokeName(omegaBack.anti_idle.keystroke)));

    std::printf("dialog sshauth %s\n",
                sshDefaultAuthName(omegaBack.ssh_default_auth));
    return 0;
}