// theme/theme.cpp

#include "theme/theme.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace omega::theme {

const char *const kAnsiKeys[16] = {
    "black",       "red",         "green",       "yellow",
    "blue",        "magenta",     "cyan",        "white",
    "brightBlack", "brightRed",   "brightGreen", "brightYellow",
    "brightBlue",  "brightMagenta", "brightCyan", "brightWhite",
};

namespace {

std::string trim(const std::string &s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

// Strips one layer of matching quotes. The files quote every colour and the
// font list; nothing relies on YAML's escape handling inside them.
std::string unquote(const std::string &s) {
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') &&
        s.back() == s.front()) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

size_t indentOf(const std::string &line) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    return i;
}

}  // namespace

std::string Theme::color(const std::string &key,
                         const std::string &fallback) const {
    const auto it = terminal_colors.find(key);
    return it == terminal_colors.end() ? fallback : it->second;
}

bool Theme::hasColor(const std::string &key) const {
    return terminal_colors.find(key) != terminal_colors.end();
}

Theme Theme::fallback() {
    // enterprise_dark, matching AppSettings' default theme name.
    //
    // This is the compiled last resort -- what renders when the theme
    // directory is missing entirely. It is NOT the theme named "default";
    // default.yaml still ships and still registers under that name. Keeping
    // this in step with the settings default means there is one answer to
    // "what does Omega look like out of the box" rather than two that differ
    // only when something has gone wrong.
    //
    // Generated from theme/themes/enterprise_dark.yaml. If that file changes,
    // the differential will not catch it -- nothing compares a compiled
    // fallback against a file -- so change both.
    Theme t;
    t.name = "enterprise_dark";
    t.terminal_colors = {
        {"background", "#0c0c0c"},             {"foreground", "#cccccc"},
        {"cursor", "#ffffff"},                 {"cursorAccent", "#0c0c0c"},
        {"selectionBackground", "#264f78"},    {"selectionForeground", "#ffffff"},
        {"black", "#0c0c0c"},                  {"red", "#c50f1f"},
        {"green", "#13a10e"},                  {"yellow", "#c19c00"},
        {"blue", "#0037da"},                   {"magenta", "#881798"},
        {"cyan", "#3a96dd"},                   {"white", "#cccccc"},
        {"brightBlack", "#767676"},            {"brightRed", "#e74856"},
        {"brightGreen", "#16c60c"},            {"brightYellow", "#f9f1a5"},
        {"brightBlue", "#3b78ff"},             {"brightMagenta", "#b4009e"},
        {"brightCyan", "#61d6d6"},             {"brightWhite", "#f2f2f2"},
    };
    t.font_family = "Cascadia Code, Consolas, JetBrains Mono, monospace";
    t.font_size = 14;
    t.background_color = "#1e1e1e";
    t.foreground_color = "#d4d4d4";
    t.border_color = "#3c3c3c";
    t.accent_color = "#0078d4";
    t.overlay_background = "rgba(30, 30, 30, 0.95)";
    t.overlay_text_color = "#d4d4d4";
    return t;
}

// A reader for the subset of YAML these files are written in: a flat mapping
// of scalars plus one nested mapping, `terminal_colors`. No anchors, no lists,
// no multi-line scalars, no flow style.
//
// Deliberately not yaml-cpp. Adding a package-managed dependency to read 33
// files of `key: value` is the trade the session store already declined when it
// took the SQLite amalgamation instead of a wrapper, and this is a smaller
// problem than that one was. The parse is one function; if a theme ever needs
// real YAML, replacing it touches this file and nothing else.
//
// It is strict on purpose. A file it cannot read is reported, not guessed at --
// the shipped clean.yaml is zero bytes, and the failure that matters is the one
// that says so rather than the one that yields a black-on-black theme.
bool parseTheme(const std::string &text, Theme *out, std::string *error) {
    auto fail = [&](const std::string &why) {
        if (error) *error = why;
        return false;
    };

    Theme t;
    bool sawKey = false;
    bool sawName = false;
    bool inColors = false;

    std::istringstream in(text);
    std::string line;
    int lineNo = 0;

    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        const std::string body = trim(line);
        if (body.empty() || body[0] == '#' || body == "---" || body == "...") {
            continue;
        }

        const size_t indent = indentOf(line);
        const auto colon = body.find(':');
        if (colon == std::string::npos) {
            return fail("line " + std::to_string(lineNo) +
                        ": expected 'key: value'");
        }

        const std::string key = trim(body.substr(0, colon));
        const std::string value = unquote(trim(body.substr(colon + 1)));

        if (indent > 0) {
            if (!inColors) {
                return fail("line " + std::to_string(lineNo) +
                            ": indented key '" + key +
                            "' outside terminal_colors");
            }
            t.terminal_colors[key] = value;
            continue;
        }

        inColors = false;
        sawKey = true;

        if (key == "terminal_colors") {
            if (!value.empty()) {
                return fail("line " + std::to_string(lineNo) +
                            ": terminal_colors must be a nested mapping");
            }
            inColors = true;
        } else if (key == "name") {
            t.name = value;
            sawName = true;
        } else if (key == "font_family") {
            t.font_family = value;
        } else if (key == "font_size") {
            char *end = nullptr;
            const long n = std::strtol(value.c_str(), &end, 10);
            if (end == value.c_str() || *end != '\0') {
                return fail("line " + std::to_string(lineNo) +
                            ": font_size is not an integer");
            }
            t.font_size = static_cast<int>(n);
        } else if (key == "background_color") {
            t.background_color = value;
        } else if (key == "foreground_color") {
            t.foreground_color = value;
        } else if (key == "border_color") {
            t.border_color = value;
        } else if (key == "accent_color") {
            t.accent_color = value;
        } else if (key == "overlay_background") {
            t.overlay_background = value;
        } else if (key == "overlay_text_color") {
            t.overlay_text_color = value;
        } else {
            // Matches engine.py, where Theme(**data) raises TypeError on an
            // unknown keyword and load_themes drops the file. Accepting it here
            // would produce a theme that works in Omega and vanishes in
            // nterm-qt, which is the harder bug to find of the two.
            return fail("line " + std::to_string(lineNo) +
                        ": unknown key '" + key + "'");
        }
    }

    if (!sawKey) {
        return fail("empty document");
    }
    if (!sawName) {
        return fail("no 'name' key");
    }

    *out = std::move(t);
    if (error) error->clear();
    return true;
}

bool loadThemeFile(const std::string &path, Theme *out, std::string *error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    std::ostringstream buf;
    buf << f.rdbuf();
    return parseTheme(buf.str(), out, error);
}

ThemeEngine::ThemeEngine() : fallback_(Theme::fallback()) {
    themes_[fallback_.name] = fallback_;
}

int ThemeEngine::loadDirectory(const std::string &dir) {
    // Directory iteration is std::filesystem, which is the one place this
    // library touches anything platform-shaped -- and it is the standard
    // library's problem, not ours.
    namespace fs = std::filesystem;

    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        warnings_.push_back("theme directory not found: " + dir);
        return 0;
    }

    // Sorted, so the outcome of two files claiming the same name does not
    // depend on directory order.
    std::vector<std::string> paths;
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".yaml") continue;
        paths.push_back(entry.path().string());
    }
    std::sort(paths.begin(), paths.end());

    int loaded = 0;
    for (const std::string &path : paths) {
        Theme t;
        std::string err;
        if (!loadThemeFile(path, &t, &err)) {
            warnings_.push_back(path + ": " + err);
            continue;
        }
        themes_[t.name] = t;
        ++loaded;
    }
    return loaded;
}

void ThemeEngine::registerTheme(const Theme &theme) {
    themes_[theme.name] = theme;
}

const Theme *ThemeEngine::get(const std::string &name) const {
    const auto it = themes_.find(name);
    return it == themes_.end() ? nullptr : &it->second;
}

std::vector<std::string> ThemeEngine::names() const {
    std::vector<std::string> out;
    out.reserve(themes_.size());
    for (const auto &kv : themes_) out.push_back(kv.first);
    return out;  // std::map is already sorted by key
}

const Theme &ThemeEngine::current() const {
    if (current_) {
        const auto it = themes_.find(*current_);
        if (it != themes_.end()) return it->second;
    }
    const auto def = themes_.find(fallback_.name);
    return def == themes_.end() ? fallback_ : def->second;
}

bool ThemeEngine::setCurrent(const std::string &name) {
    if (themes_.find(name) == themes_.end()) return false;
    current_ = name;
    return true;
}

}  // namespace omega::theme
