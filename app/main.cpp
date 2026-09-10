// app/main.cpp
//
// Omega's entry point.
//
// Thin on purpose: locate the themes, load the settings, open the window. The
// shell itself is app/mainwindow.cpp, which is a library so that it can be
// built and driven without an entry point -- the same arrangement sessions/
// and theme/ have.
//
// Usage:
//   omega [--themes <dir>] [--config <file>] [--omega-config <file>]
//
// The flags exist for running against something other than the installed
// layout: a checkout's theme/themes, a config.json that is not the real
// ~/.omega/config.json, or an omega.json that is not the real one. Without
// them it uses the shipped theme directory, the file both applications share,
// and Omega's own file beside it.

#include <QApplication>
#include <QDir>
#include <QIcon>
#include <QFileInfo>
#include <QStringList>

#include "app/helpdialog.h"
#include "app/mainwindow.h"
#include "app/spinarrowstyle.h"
#include "app/settings.h"
#include "theme/theme.h"

#include <cstdio>

namespace {

QString argValue(const QStringList &args, const QString &flag,
                 const QString &fallback = {}) {
    const int at = args.indexOf(flag);
    return (at >= 0 && at + 1 < args.size()) ? args.at(at + 1) : fallback;
}

// Where the themes are, in the order worth trying:
//
//   --themes            an explicit answer, for a checkout or a test
//   OMEGA_THEME_DIR     the environment, for the same reason
//   ../Resources/themes beside the executable -- a macOS .app bundle, and any
//                       other layout that ships its resources alongside
//   the compiled path   where the build found them
//   ./theme/themes      running from a source tree
//
// THE BUNDLE PATH GOES BEFORE THE COMPILED ONE, and the order is the whole
// point of it. The compiled path is an absolute path into the build tree, so
// on the machine that built it a bundle with NO themes in it still finds them
// and looks fine -- and the same bundle on any other machine falls through to
// the one built-in fallback and opens as a plain window, with nothing on
// screen saying why. Looking in the bundle first means the packaging is
// exercised on the machine doing the packaging.
//
// Not guarded by Q_OS_MACOS: the same layout is what a portable Linux or
// Windows tree would use, and a directory that is not there costs one stat.
QString findThemeDir(const QStringList &args) {
    const QString explicitDir = argValue(args, QStringLiteral("--themes"));
    if (!explicitDir.isEmpty()) return explicitDir;

    const QByteArray env = qgetenv("OMEGA_THEME_DIR");
    if (!env.isEmpty()) return QString::fromLocal8Bit(env);

    // Omega.app/Contents/MacOS/Omega -> Omega.app/Contents/Resources/themes
    const QString beside = QDir::cleanPath(
        QCoreApplication::applicationDirPath() +
        QStringLiteral("/../Resources/themes"));
    if (QFileInfo(beside).isDir()) return beside;

#ifdef OMEGA_THEME_DIR_DEFAULT
    const QString compiled = QStringLiteral(OMEGA_THEME_DIR_DEFAULT);
    if (QFileInfo(compiled).isDir()) return compiled;
#endif

    return QStringLiteral("theme/themes");
}

// Every flag this binary understands, and whether it takes a value.
//
// Spelled out rather than left implicit because argValue() is an exact
// indexOf: a flag nobody looks for is not a no-op, it is silently dropped
// along with its value. `--theme gruvbox` -- which is what theme_gallery and
// the preview harness both take -- used to open the window on whatever
// config.json named and say nothing, which reads as a broken theme rather than
// as a flag that does not exist.
struct Flag {
    const char *name;
    bool takesValue;
};
constexpr Flag kFlags[] = {
    {"--themes", true},        {"--theme", true},
    {"--config", true},        {"--omega-config", true},
    {"--list-themes", false},  {"--native-frame", false},
    {"--reset-window", false},
    {"--help", false},
    {"-h", false},
};

void printUsage(std::FILE *to) {
    std::fprintf(to,
        "usage: omega [options]\n"
        "\n"
        "  --themes <dir>        directory of theme YAML files to load\n"
        "  --theme <name>        open in this theme, without recording it\n"
        "                        in config.json -- for looking at one, not\n"
        "                        for choosing one\n"
        "  --config <file>       config.json to use instead of ~/.omega's\n"
        "  --omega-config <file> omega.json to use instead of ~/.omega's\n"
        "  --native-frame        use the window manager's title bar for this\n"
        "                        run, whatever omega.json says\n"
        "  --reset-window        forget the saved window position and size and\n"
        "                        open centred on the primary screen. For a\n"
        "                        window left on a monitor that is no longer\n"
        "                        there -- the one case a shortcut cannot fix,\n"
        "                        because the window has to be focused first\n"
        "  --list-themes         print the loaded theme names and exit\n"
        "  -h, --help            this\n"
        "\n"
        "Qt's own options (-platform, -style, ...) are consumed before these\n"
        "and are not listed here.\n");
}

// Returns false and explains when an argument is not one of the above. Qt has
// already removed its own flags from arguments() by the time this runs, so
// anything left that starts with a dash is genuinely ours to accept or refuse.
bool checkArgs(const QStringList &args) {
    for (int i = 1; i < args.size(); ++i) {
        const QString &a = args.at(i);
        if (!a.startsWith(QLatin1Char('-'))) {
            std::fprintf(stderr, "omega: unexpected argument \"%s\"\n",
                         qPrintable(a));
            printUsage(stderr);
            return false;
        }
        const Flag *found = nullptr;
        for (const Flag &f : kFlags) {
            if (a == QLatin1String(f.name)) {
                found = &f;
                break;
            }
        }
        if (!found) {
            std::fprintf(stderr, "omega: unknown option \"%s\"\n",
                         qPrintable(a));
            printUsage(stderr);
            return false;
        }
        if (found->takesValue) {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "omega: %s needs a value\n",
                             qPrintable(a));
                return false;
            }
            ++i;  // skip the value, so it is not read as a flag
        }
    }
    return true;
}

// Sorted, for --list-themes and for the "no such theme" message. The engine
// keys by the name inside the file, so this is what --theme has to match.
QStringList themeNames(const omega::theme::ThemeEngine &engine) {
    QStringList out;
    for (const std::string &n : engine.names()) {
        out.append(QString::fromStdString(n));
    }
    return out;
}

}  // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Omega"));

    // Before any stylesheet, because QStyleSheetStyle wraps whatever style is
    // current when the sheet is set and delegates the spin arrows down to it.
    // Installed unconditionally: it only touches two primitives, and the
    // classic sheet needs them as much as the token one. See
    // app/spinarrowstyle.h.
    QApplication::setStyle(new omega::app::SpinArrowStyle);

    // From the compiled-in resource rather than a file: this is what Linux and
    // Windows use for the title bar and task switcher. macOS ignores it and
    // takes the bundle's .icns instead, which is why both exist.
    omega::app::initOmegaResources();
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/omega/icon.png")));
    const QStringList args = app.arguments();
    if (!checkArgs(args)) return 2;
    if (args.contains(QStringLiteral("--help")) ||
        args.contains(QStringLiteral("-h"))) {
        printUsage(stdout);
        return 0;
    }

    omega::theme::ThemeEngine themes;
    const QString themeDir = findThemeDir(args);
    const int loaded = themes.loadDirectory(themeDir.toStdString());
    for (const std::string &w : themes.warnings()) {
        std::fprintf(stderr, "omega: theme warning: %s\n", w.c_str());
    }
    if (loaded == 0) {
        // Not fatal. ThemeEngine carries one compiled fallback precisely so a
        // missing theme directory produces a plain window rather than no
        // window, and a packaging mistake is something to see rather than
        // something to guess at from an empty screen.
        std::fprintf(stderr,
                     "omega: no themes found in %s -- using the built-in\n",
                     qPrintable(themeDir));
    }

    if (args.contains(QStringLiteral("--list-themes"))) {
        for (const QString &n : themeNames(themes)) {
            std::printf("%s\n", qPrintable(n));
        }
        std::fprintf(stderr, "%d themes from %s\n", loaded,
                     qPrintable(themeDir));
        return 0;
    }

    // Validated BEFORE the window is built, so a typo costs a message rather
    // than a window that opens in the wrong theme and looks like the flag was
    // ignored -- which is exactly what it used to do.
    const QString wantTheme = argValue(args, QStringLiteral("--theme"));
    if (!wantTheme.isEmpty() && !themes.get(wantTheme.toStdString())) {
        std::fprintf(stderr, "omega: no such theme \"%s\"\n",
                     qPrintable(wantTheme));
        std::fprintf(stderr, "omega: --list-themes shows the %d loaded from %s\n",
                     loaded, qPrintable(themeDir));
        return 2;
    }

    const QString configPath =
        argValue(args, QStringLiteral("--config"),
                 omega::app::SettingsManager::defaultConfigFile());
    omega::app::SettingsManager settings(configPath);

    // Omega's own file, beside config.json. Separate because config.json is
    // shared with nterm-qt, which deletes keys it does not recognise; see
    // omegasettings.h. A missing file is a first run, not an error.
    const QString omegaConfigPath =
        argValue(args, QStringLiteral("--omega-config"),
                 omega::app::OmegaSettingsManager::defaultFile());
    omega::app::OmegaSettingsManager omegaSettings(omegaConfigPath);
    if (!omegaSettings.load()) {
        qWarning("%s", qPrintable(omegaSettings.error()));
    }
    for (const QString &warning : omegaSettings.warnings()) {
        qWarning("%s: %s", qPrintable(omegaSettings.path()), qPrintable(warning));
    }
    if (!settings.load()) {
        std::fprintf(stderr, "omega: %s\n", qPrintable(settings.error()));
    }
    for (const QString &w : settings.warnings()) {
        std::fprintf(stderr, "omega: settings: %s\n", qPrintable(w));
    }

    // Before the window is constructed: the constructor decides frameless or
    // not, and cannot be told afterwards without rebuilding the menu bar.
    // Deliberately not written back to omega.json, for the same reason
    // --theme is not -- see MainWindow::applyThemeByName.
    if (args.contains(QStringLiteral("--native-frame"))) {
        omega::app::MainWindow::forceNativeFrame();
    }

    // Also before construction: restoreGeometryFromSettings runs inside it, and
    // clearing the position afterwards would be clearing it after it was used.
    //
    // Only the position and the maximised flag. The size is kept, because a
    // window that cannot be found is not evidence that its size was wrong, and
    // restoreGeometryFromSettings clamps it to the destination screen anyway.
    // Not saved back here either -- closeEvent writes wherever it ends up, so
    // one run with this flag is enough and the flag does not have to be
    // remembered.
    if (args.contains(QStringLiteral("--reset-window"))) {
        settings.settings().window_x.reset();
        settings.settings().window_y.reset();
        settings.settings().window_maximized = false;
    }

    omega::app::MainWindow window(&settings, &themes,
                                  nullptr,
                                  omega::app::MainWindow::defaultVaultFile(),
                                  omega::app::MainWindow::defaultSessionFile(),
                                  &omegaSettings);

    // After construction, which has already applied config.json's theme. The
    // false is the whole point of the flag: see MainWindow::applyThemeByName.
    if (!wantTheme.isEmpty()) {
        window.applyThemeByName(wantTheme, /*persist=*/false);
    }

    window.show();
    return app.exec();
}