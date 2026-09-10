// tests/compat/theme_probe.cpp
//
// The C++ half of the theme differential. It prints and does not judge; the
// comparison lives in theme_differential.py, so that neither side is asserting
// against its own idea of what the answer should be.
//
//   theme_probe stylesheet <file.yaml>   the generated QSS, on stdout, verbatim
//   theme_probe dump       <file.yaml>   parsed fields, one per line
//   theme_probe list       <dir>         theme names, then warnings on stderr
//   theme_probe derive     <hex>         lighten/darken at 0.1, both directions
//   theme_probe color      <text>        parseCssColor as "r g b a valid"
//
// Exit status is 0 on success and 1 on a load failure, with the reason on
// stderr -- the Python needs the failure to be visible rather than inferred
// from empty output, because a file that fails to load in BOTH applications is
// a pass and a file that fails in one is the bug this is looking for.

#include "theme/color.h"
#include "theme/stylesheet.h"
#include "theme/theme.h"
#include "theme/tokens.h"
#include "theme/tokenstylesheet.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace omega::theme;

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: theme_probe {stylesheet|tokenstylesheet|dump|tokens}"
                 " <file.yaml>\n"
                 "       theme_probe list <dir>\n"
                 "       theme_probe statecheck <dir>\n"
                 "       theme_probe rampcheck -\n"
                 "       theme_probe derive <hex>\n"
                 "       theme_probe color <text>\n");
    return 2;
}

void printTokens(const Tokens &t) {
    std::printf("name=%s\n", t.name.c_str());
    std::printf("dark=%d\n", t.dark ? 1 : 0);
    std::printf("bg.deep=%s\n", hex(t.bgDeep).c_str());
    std::printf("bg.base=%s\n", hex(t.bgBase).c_str());
    std::printf("bg.raised=%s\n", hex(t.bgRaised).c_str());
    std::printf("bg.chrome=%s\n", hex(t.bgChrome).c_str());
    std::printf("bg.control=%s\n", hex(t.bgControl).c_str());
    std::printf("bg.selected=%s\n", hex(t.bgSelected).c_str());
    std::printf("bg.hover=%s\n", hex(t.bgHover).c_str());
    std::printf("line=%s\n", hex(t.line).c_str());
    std::printf("line.input=%s\n", hex(t.lineInput).c_str());
    std::printf("ink=%s\n", hex(t.ink).c_str());
    std::printf("ink.dim=%s\n", hex(t.inkDim).c_str());
    std::printf("ink.detail=%s\n", hex(t.inkDetail).c_str());
    std::printf("ink.muted=%s\n", hex(t.inkMuted).c_str());
    std::printf("ink.faint=%s\n", hex(t.inkFaint).c_str());
    std::printf("accent=%s\n", hex(t.accent).c_str());
    std::printf("on.accent=%s\n", hex(t.onAccent).c_str());
    std::printf("accent.hover=%s\n",
                hex(t.dark ? lighterBy(t.accent, 115) : darkerBy(t.accent, 112))
                    .c_str());
    std::printf("disabled.ink=%s\n", hex(darkerBy(t.inkFaint, 125)).c_str());
    std::printf("warn=%s\n", hex(t.warn).c_str());
    std::printf("dead=%s\n", hex(t.dead).c_str());
    std::printf("state=%s\n", hex(t.state).c_str());
    std::printf("state.source=%s\n", t.stateSource.c_str());
    std::printf("state.distinct=%d\n", t.stateIsDistinct ? 1 : 0);
    std::printf("term.bg=%s\n", hex(t.termBg).c_str());
    std::printf("term.fg=%s\n", hex(t.termFg).c_str());
}

// The spec's fourteen constants beside what the derivation produces from its
// own four inputs. This is the evidence that table 1 was a derivation all
// along; it fails the build if the ramp drifts.
int rampCheck() {
    const Tokens d = specTokens();
    struct Row {
        const char *token;
        const char *spec;
        Rgba got;
    };
    const Row rows[] = {
        {"bg.deep", "#0e1012", d.bgDeep},
        {"bg.base", "#16181b", d.bgBase},
        {"bg.raised", "#1a1d21", d.bgRaised},
        {"bg.chrome", "#1c1f23", d.bgChrome},
        {"bg.control", "#22262b", d.bgControl},
        {"bg.selected", "#262b31", d.bgSelected},
        {"line", "#2c3238", d.line},
        {"line.input", "#313840", d.lineInput},
        {"ink", "#e6e9ec", d.ink},
        {"ink.dim", "#b7c0c8", d.inkDim},
        {"ink.muted", "#8b959f", d.inkMuted},
        {"ink.faint", "#6c7883", d.inkFaint},
        {"accent", "#56cfa1", d.accent},
        {"warn", "#e0a458", d.warn},
        {"dead", "#3a4148", d.dead},
    };

    std::printf("token        spec      derived   dR dG dB\n");
    int worst = 0;
    for (const Row &r : rows) {
        const Rgba want = parseCssColor(r.spec);
        const int dr = r.got.r - want.r;
        const int dg = r.got.g - want.g;
        const int db = r.got.b - want.b;
        worst = std::max(worst, std::max(std::abs(dr),
                                         std::max(std::abs(dg), std::abs(db))));
        std::printf("%-12s %-9s %-9s %3d %3d %3d\n", r.token, r.spec,
                    hex(r.got).c_str(), dr, dg, db);
    }
    std::printf("\nworst channel error: %d/255\n", worst);
    // 2 is the fit the ramp was tuned to. Anything worse is drift, not noise.
    if (worst > 2) {
        std::fprintf(stderr, "ramp drifted: worst %d exceeds 2\n", worst);
        return 1;
    }
    return 0;
}

int loadOrDie(const std::string &path, Theme *out) {
    std::string err;
    if (!loadThemeFile(path, out, &err)) {
        std::fprintf(stderr, "load failed: %s\n", err.c_str());
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3) return usage();

    const std::string mode = argv[1];
    const std::string arg = argv[2];

    if (mode == "stylesheet") {
        Theme t;
        if (const int rc = loadOrDie(arg, &t)) return rc;
        const std::string qss = generateStylesheet(t);
        std::fwrite(qss.data(), 1, qss.size(), stdout);
        return 0;
    }

    if (mode == "dump") {
        Theme t;
        if (const int rc = loadOrDie(arg, &t)) return rc;
        // Sorted colours first so the two dumps are comparable line for line
        // without either side sorting the other's output.
        for (const auto &kv : t.terminal_colors) {
            std::printf("color.%s=%s\n", kv.first.c_str(), kv.second.c_str());
        }
        std::printf("name=%s\n", t.name.c_str());
        std::printf("font_family=%s\n", t.font_family.c_str());
        std::printf("font_size=%d\n", t.font_size);
        std::printf("background_color=%s\n", t.background_color.c_str());
        std::printf("foreground_color=%s\n", t.foreground_color.c_str());
        std::printf("border_color=%s\n", t.border_color.c_str());
        std::printf("accent_color=%s\n", t.accent_color.c_str());
        std::printf("overlay_background=%s\n", t.overlay_background.c_str());
        std::printf("overlay_text_color=%s\n", t.overlay_text_color.c_str());
        return 0;
    }

    if (mode == "list") {
        ThemeEngine engine;
        const int n = engine.loadDirectory(arg);
        for (const std::string &name : engine.names()) {
            std::printf("%s\n", name.c_str());
        }
        for (const std::string &w : engine.warnings()) {
            std::fprintf(stderr, "warning: %s\n", w.c_str());
        }
        std::fprintf(stderr, "loaded %d\n", n);
        return 0;
    }

    if (mode == "derive") {
        std::printf("lighten=%s\n", lighten(arg, 0.1).c_str());
        std::printf("darken=%s\n", darken(arg, 0.1).c_str());
        return 0;
    }

    if (mode == "color") {
        const Rgba c = parseCssColor(arg);
        std::printf("%d %d %d %d %d\n", c.r, c.g, c.b, c.a, c.valid ? 1 : 0);
        return 0;
    }

    // The redesign's token ramp. Same contract as `dump`: prints and does not
    // judge, one key=value per line, so a differential can compare it against
    // whatever else claims to derive the same table.
    if (mode == "tokens") {
        Theme t;
        if (const int rc = loadOrDie(arg, &t)) return rc;
        printTokens(tokensFromTheme(t));
        return 0;
    }

    // The redesign's sheet. Same contract as `stylesheet`: verbatim on stdout,
    // so a selector change can be diffed without compiling a widget.
    if (mode == "tokenstylesheet") {
        Theme t;
        if (const int rc = loadOrDie(arg, &t)) return rc;
        const std::string qss = generateTokenStylesheet(tokensFromTheme(t));
        std::fwrite(qss.data(), 1, qss.size(), stdout);
        return 0;
    }

    if (mode == "rampcheck") {
        // arg is ignored; the spec's table is compiled in. Kept in the
        // two-argument shape so the dispatch above stays uniform.
        return rampCheck();
    }

    if (mode == "statecheck") {
        ThemeEngine engine;
        engine.loadDirectory(arg);
        for (const std::string &w : engine.warnings()) {
            std::fprintf(stderr, "warning: %s\n", w.c_str());
        }
        int keep = 0, moved = 0, textOnly = 0;
        std::printf("%-22s %-9s %-9s %-13s %4s  %s\n", "theme", "accent",
                    "state", "from", "sep", "verdict");
        for (const std::string &name : engine.names()) {
            const Theme *raw = engine.get(name);
            if (!raw) continue;
            const Tokens t = tokensFromTheme(*raw);
            const double sep = std::min(colorDistance(t.state, t.ink),
                                        colorDistance(t.state, t.bgBase));
            const char *verdict;
            if (!t.stateIsDistinct) {
                verdict = "TEXT ONLY";
                ++textOnly;
            } else if (t.stateSource != "accent") {
                verdict = "moved off accent";
                ++moved;
            } else {
                verdict = "accent holds";
                ++keep;
            }
            std::printf("%-22s %-9s %-9s %-13s %4d  %s\n", name.c_str(),
                        hex(t.accent).c_str(), hex(t.state).c_str(),
                        t.stateSource.c_str(), int(sep + 0.5), verdict);
        }
        std::printf("\n%d themes: %d keep the accent, %d move off it, "
                    "%d have no colour that can carry the state at all.\n",
                    keep + moved + textOnly, keep, moved, textOnly);
        return textOnly > 3 ? 1 : 0;
    }

    return usage();
}
