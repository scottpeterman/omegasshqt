// theme/theme.h
//
// The nterm-qt theme format, read from C++.
//
// Same directory, same 33 files, neither application taught the other's
// serialiser -- the same arrangement the session store has, for the same
// reason. Everything here that looks arbitrary is matching what
// ntermqt/theme/engine.py does, and the ones that matter are called out at the
// member that implements them.
//
// No Qt. A Theme is strings, and generateStylesheet() turns strings into a
// string, so this library configures and tests on its own:
//
//     cmake -S theme -B build/theme
//
// Colours become QColor only where a widget needs one, in
// integration/omegassh_theme_anytermqt.h.
//
// THE FORMAT IS STRICTER THAN IT LOOKS. engine.py loads with Theme(**data), so
// an unrecognised top-level key is a TypeError and nterm-qt drops the whole
// theme with a warning. That means Omega cannot add a key of its own to these
// files without the other application silently losing the theme. Any Omega-only
// theme data belongs in a separate file, the same call the settings work has to
// make about config.json.

#ifndef OMEGA_THEME_THEME_H
#define OMEGA_THEME_THEME_H

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace omega::theme {

// The sixteen ANSI slots, in the order anytermqt's Palette indexes them. The
// names are xterm.js ITheme's, which is what the files are written against.
extern const char *const kAnsiKeys[16];

// Theme mirrors engine.py's dataclass field for field, defaults included.
//
// Only `name` is required. Every other field has a default there, so a partial
// file loads in both applications rather than failing -- which is why the
// defaults are repeated here rather than left zero-initialised.
struct Theme {
    std::string name;

    // terminal_colors is held as a map rather than a struct so that a key
    // neither application knows about survives a load, and so that a file
    // missing one behaves as it does in Python: the key is simply absent and
    // the consumer falls back.
    //
    // Two keys in every file have nowhere to go. anytermqt's Palette carries
    // 16 ANSI slots plus foreground, background and cursor, and selection is a
    // single colour on the widget -- so cursorAccent and selectionForeground
    // are read by nothing. They are carried anyway: the files ship unchanged,
    // and a loader that quietly drops two documented keys is a surprise later.
    std::map<std::string, std::string> terminal_colors;

    std::string font_family =
        "JetBrains Mono, Cascadia Code, Consolas, monospace";
    int font_size = 14;

    std::string background_color = "#1e1e2e";
    std::string foreground_color = "#cdd6f4";
    std::string border_color = "#313244";
    std::string accent_color = "#89b4fa";

    std::string overlay_background = "rgba(30, 30, 46, 0.9)";
    std::string overlay_text_color = "#cdd6f4";

    // colour("red", "#f38ba8") -- the port of tc.get(key, default).
    std::string color(const std::string &key,
                      const std::string &fallback = {}) const;

    bool hasColor(const std::string &key) const;

    // The built-in fallback: enterprise_dark, the same theme AppSettings names
    // as Omega's default, so a missing theme directory still opens in the
    // theme the settings asked for. One built-in, not the twelve engine.py
    // registers -- the other eleven are duplicates of YAML files that
    // overwrite them a moment later.
    static Theme fallback();
};

// Parses one theme document. Returns false and fills `error` on a document
// that is empty, is not a mapping, has no `name`, or carries a top-level key
// the format does not define -- the last one because nterm-qt refuses those
// too, and a theme that loads in one application and not the other is worse
// than one that loads in neither.
bool parseTheme(const std::string &text, Theme *out, std::string *error);

// Reads and parses a file. Same failure rules.
bool loadThemeFile(const std::string &path, Theme *out, std::string *error);

// ThemeEngine is the loaded set and the current selection.
//
// Keyed by the `name:` inside the file, not by filename. In the shipped set the
// two always agree; a file that disagrees registers under its contents, which
// is what engine.py does.
class ThemeEngine {
public:
    ThemeEngine();

    // Loads every *.yaml in dir, overwriting same-named entries. A file that
    // fails to parse is skipped and its reason appended to warnings(), never
    // thrown -- one broken file in a directory of 33 must not cost the other
    // 32. Returns the number loaded.
    int loadDirectory(const std::string &dir);

    // Adds or replaces one theme.
    void registerTheme(const Theme &theme);

    // Null when the name is unknown. The caller decides what to do about it,
    // because there is a real case: AppSettings.theme_name defaults to
    // "catppuccin_mocha" and no such theme exists in the shipped set, in either
    // application. Omega should write a name that resolves; until it does,
    // every existing config.json names this one.
    const Theme *get(const std::string &name) const;

    // Sorted, for a View > Theme menu.
    std::vector<std::string> names() const;

    // The current theme, or the fallback when nothing has been selected or the
    // selection went missing.
    const Theme &current() const;
    bool setCurrent(const std::string &name);

    const std::vector<std::string> &warnings() const { return warnings_; }

private:
    std::map<std::string, Theme> themes_;
    std::optional<std::string> current_;
    Theme fallback_;
    std::vector<std::string> warnings_;
};

}  // namespace omega::theme

#endif  // OMEGA_THEME_THEME_H
