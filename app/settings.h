// app/settings.h
//
// The nterm-qt application settings, read and written from C++.
//
// Same arrangement as the session store and the theme files: one
// ~/.omega/config.json, opened by both applications, with neither taught the
// other's serialiser. Port of ntermqt/config.py, field for field.
//
// QtCore, and no more than that. The theme library hand-rolled a reader because
// there was no YAML parser to hand and taking one meant a package manager;
// JSON has QJsonDocument sitting right there, and QtCore needs no display, so
// the differential still runs headless. Nothing here touches QtWidgets.
//
// WRITING IS HAND-ROLLED ANYWAY, and that is deliberate. QJsonDocument sorts
// keys and indents with four spaces; Python writes them in dataclass order with
// two. Emitting Python's exact bytes means two applications alternating saves
// leave the file untouched rather than rewriting it past each other, and it
// makes the output byte-comparable in tests/compat/settings_differential.py.
//
// FIVE OF THE FIFTEEN FIELDS ARE INERT IN nterm-qt. tree_width,
// recent_profiles, max_recent, default_term_type and default_keepalive_interval
// are stored, defaulted, and read by nothing -- the splitter is hardcoded to
// 250/950 and the rest have no reader at all. Omega is free to start honouring
// them: writing a field the other application ignores cannot make the two
// disagree. keepalive in particular should have been exposed, and is the one
// that matters for gear behind a firewall.
//
// UNKNOWN KEYS ARE DROPPED, matching from_dict's filter. Preserving them would
// be friendlier and would be a lie: nterm-qt deletes them on its next save, so
// a key Omega preserved would vanish the first time the other application ran.
// Omega-only settings belong in a separate file for that reason, not in here.

#ifndef OMEGA_APP_SETTINGS_H
#define OMEGA_APP_SETTINGS_H

#include <QString>
#include <QStringList>

#include <optional>

namespace omega::app {

// AppSettings mirrors config.py's dataclass, in its field order -- which is
// also the order they are written, since Python does not sort keys.
struct AppSettings {
    // --- appearance -----------------------------------------------------
    //
    // nterm-qt defaults this to "catppuccin_mocha", and there is no
    // catppuccin_mocha.yaml in either theme set: the default names a theme
    // that does not exist and silently falls back. Omega defaults to a name
    // that resolves, and picks one that ships in both applications, so a
    // config.json written here still means something when nterm-qt opens it.
    QString theme_name = QStringLiteral("enterprise_dark");
    int font_size = 14;

    // --- terminal behaviour ---------------------------------------------
    //
    // A threshold of 1 means every multi-line paste is confirmed. That reads
    // like a placeholder and is not one: pasting two lines into a device in
    // config mode is how a change goes in half-applied.
    int multiline_paste_threshold = 1;
    int scrollback_lines = 10000;

    // --- connection defaults --------------------------------------------
    QString default_term_type = QStringLiteral("xterm-256color");
    int default_keepalive_interval = 30;
    bool auto_reconnect = true;

    // --- window state ---------------------------------------------------
    //
    // Integers, not a saveGeometry() blob. QByteArray geometry is the
    // idiomatic Qt answer and nterm-qt could not read a word of it; the
    // shared file decides the representation.
    int window_width = 1200;
    int window_height = 800;
    std::optional<int> window_x;  // null until the window has been placed
    std::optional<int> window_y;
    bool window_maximized = false;

    // --- session tree state ---------------------------------------------
    int tree_width = 250;

    // --- recent connections ---------------------------------------------
    // Names only. Never a credential, and never anything resolved from one.
    QStringList recent_profiles;
    int max_recent = 10;

    // Moves an existing entry to the front rather than duplicating it, then
    // trims to max_recent. Port of add_recent_profile.
    void addRecentProfile(const QString &name);

    // The file's exact bytes: two-space indent, dataclass order, no trailing
    // newline, non-ASCII escaped as \uXXXX -- json.dumps(data, indent=2).
    QString toJson() const;

    // Parses whatever of the known fields are present, leaving the rest at
    // their defaults and ignoring everything else. A field of the wrong type
    // is left at its default and named in `warnings` rather than refusing the
    // file: nterm-qt's json.loads would accept it and coerce nothing, and a
    // settings file is not worth failing to start over.
    static AppSettings fromJson(const QByteArray &text, QStringList *warnings);
};

// SettingsManager owns the file. Port of config.py's class of the same name,
// including its forgiving load: a missing file and an unparseable one both
// yield defaults, because neither is a reason to refuse to start.
class SettingsManager {
public:
    explicit SettingsManager(const QString &configPath = defaultConfigFile());

    AppSettings &settings() { return settings_; }
    const AppSettings &settings() const { return settings_; }

    // Reads the file if it exists. Returns false only when the file was there
    // and could not be read or parsed -- in which case settings() holds
    // defaults and the reason is in error(). Not finding a file is success.
    bool load();

    // Creates the directory if needed and writes the file. Returns false and
    // fills error() on failure.
    bool save();

    const QString &error() const { return error_; }
    const QStringList &warnings() const { return warnings_; }
    const QString &path() const { return path_; }

    // ~/.omega and ~/.omega/config.json. The same directory the session
    // store, the vault and the session logs use.
    //
    // THIS IS THE ONE DEFINITION. defaultVaultFile(), defaultSessionFile() and
    // OmegaSettingsManager::defaultFile() all build on it, so the directory
    // moves in one place. transport/logtap.go has its own copy because it is
    // below the C boundary and cannot call this -- the two are asserted
    // against each other in tests/compat/config_dir_probe.cpp.
    static QString defaultConfigDir();
    static QString defaultConfigFile();

private:
    QString path_;
    AppSettings settings_;
    QString error_;
    QStringList warnings_;
};

}  // namespace omega::app

#endif  // OMEGA_APP_SETTINGS_H
