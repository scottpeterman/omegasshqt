// tests/compat/token_differential.cpp
//
// The redesign's token ramp lives in omega_theme, which has no Qt in it -- so
// that the derivation configures, builds and tests without a display, the same
// property the rest of that library has. But the ramp was FITTED against
// QColor's arithmetic, and QColor's arithmetic is not the textbook one. This
// is what keeps the two in step.
//
// Unlike theme_differential.py and the session-store differential, the
// reference here is not another implementation of the same idea. It is QColor
// itself, so the comparison is a C++ program that links both rather than a
// Python script driving a probe. Same principle either way: neither side
// asserts against its own idea of the answer.
//
// WHAT IT PINS, and why each one is here rather than assumed:
//
//   toHsv / fromHsv    Qt 6 carries hue as hundredths of a degree and
//                      saturation and value as 16-bit, then rounds to bytes on
//                      the way out. A port that stores 8-bit HSV, or one that
//                      shifts instead of rounding, is wrong by one level on
//                      about a tenth of the cube -- and one level is the same
//                      order as the ramp's whole fit against the spec table.
//
//   lighter / darker   These scale the 16-bit VALUE with integer arithmetic,
//                      not the float one. Doing it in float moves pctools'
//                      and solarized' accent hover by a level. Both models
//                      look correct in isolation; only the sweep separates
//                      them.
//
// The sweep is the entire 24-bit cube, which is 100M comparisons and takes
// about 13 seconds. That is cheap enough to be unconditional, and the reason
// it is exhaustive rather than sampled is that the failures are rounding
// boundaries -- a lattice with any stride at all can step straight over them.
//
//   token_differential            sweep the cube, then the ramp check
//   token_differential <dir>      ...and derive every theme in <dir> first
//
// Exit status is 0 on a clean run and 1 on any mismatch.

#include <QColor>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "theme/theme.h"
#include "theme/tokens.h"

using namespace omega::theme;

namespace {

bool same(const QColor &a, const Rgba &b) {
    return a.red() == b.r && a.green() == b.g && a.blue() == b.b;
}

int reported = 0;
constexpr int kMaxReported = 12;

void report(const char *what, int r, int g, int b, const QColor &qt,
            const Rgba &mine) {
    if (++reported > kMaxReported) return;
    std::printf("  %-12s rgb(%3d,%3d,%3d)  qcolor=%s  omega=%s\n", what, r, g, b,
                qt.name().toLatin1().constData(), hex(mine).c_str());
}

// Every colour, every primitive. See the header comment on why this is not
// sampled.
int sweepCube() {
    long long checked = 0;
    int bad = 0;

    for (int r = 0; r < 256; ++r) {
        for (int g = 0; g < 256; ++g) {
            for (int b = 0; b < 256; ++b) {
                const QColor q(r, g, b);
                Rgba p{};
                p.r = r;
                p.g = g;
                p.b = b;
                p.a = 255;
                p.valid = true;

                // The two factors the ramp actually uses, plus one either side
                // so a change of amount is covered before it is made.
                struct Case {
                    const char *what;
                    QColor qt;
                    Rgba mine;
                };
                const Case cases[] = {
                    {"lighter115", q.lighter(115), lighterBy(p, 115)},
                    {"lighter140", q.lighter(140), lighterBy(p, 140)},
                    {"darker112", q.darker(112), darkerBy(p, 112)},
                    {"darker125", q.darker(125), darkerBy(p, 125)},
                };
                for (const Case &c : cases) {
                    ++checked;
                    if (same(c.qt, c.mine)) continue;
                    ++bad;
                    report(c.what, r, g, b, c.qt, c.mine);
                }

                ++checked;
                if (q.hue() != hueDegreesFor(p) ||
                    q.hsvSaturation() != saturationFor(p) ||
                    q.value() != valueFor(p)) {
                    ++bad;
                    if (reported++ < kMaxReported) {
                        std::printf(
                            "  %-12s rgb(%3d,%3d,%3d)  qcolor=(%d,%d,%d)  "
                            "omega=(%d,%d,%d)\n",
                            "hsv", r, g, b, q.hue(), q.hsvSaturation(),
                            q.value(), hueDegreesFor(p), saturationFor(p),
                            valueFor(p));
                    }
                }

                ++checked;
                const QColor rt =
                    QColor::fromHsv(q.hue(), q.hsvSaturation(), q.value());
                const Rgba mine =
                    fromHsvFor(hueDegreesFor(p), saturationFor(p), valueFor(p));
                if (!same(rt, mine)) {
                    ++bad;
                    report("fromHsv", r, g, b, rt, mine);
                }
            }
        }
    }

    std::printf("cube: %lld comparisons, %d mismatches\n", checked, bad);
    if (bad > kMaxReported) {
        std::printf("  (%d further mismatches not listed)\n",
                    bad - kMaxReported);
    }
    return bad;
}

// The spec's fourteen constants against what the derivation produces from its
// own four inputs. 2/255 is the fit the ramp was tuned to; anything worse is
// drift rather than noise.
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

    int worst = 0;
    for (const Row &r : rows) {
        const Rgba want = parseCssColor(r.spec);
        worst = std::max(
            worst, std::max(std::abs(r.got.r - want.r),
                            std::max(std::abs(r.got.g - want.g),
                                     std::abs(r.got.b - want.b))));
    }
    std::printf("ramp: worst channel error %d/255 against the spec table\n",
                worst);
    return worst > 2 ? 1 : 0;
}

// Derives every theme in a directory. Not a comparison -- there is nothing to
// compare it against -- but it is the only thing that exercises the two guards
// in deriveTokens on real input, and it reports the state-colour survey the
// delegate's "never colour alone" rule depends on.
int deriveAll(const std::string &dir) {
    ThemeEngine engine;
    engine.loadDirectory(dir);
    for (const std::string &w : engine.warnings()) {
        std::fprintf(stderr, "  theme warning: %s\n", w.c_str());
    }

    int keep = 0, moved = 0, textOnly = 0, flat = 0;
    for (const std::string &name : engine.names()) {
        const Theme *raw = engine.get(name);
        if (!raw) continue;
        const Tokens t = tokensFromTheme(*raw);

        // A ramp that collapsed. deriveTokens has a guard for exactly this,
        // so seeing one here means the guard did not fire.
        if (colorDistance(t.bgBase, t.bgSelected) < 8.0) {
            std::printf("  FLAT RAMP %s: bg.base %s == bg.selected %s\n",
                        name.c_str(), hex(t.bgBase).c_str(),
                        hex(t.bgSelected).c_str());
            ++flat;
        }

        if (!t.stateIsDistinct) {
            ++textOnly;
        } else if (t.stateSource != "accent") {
            ++moved;
        } else {
            ++keep;
        }
    }
    std::printf("themes: %d derived -- %d keep the accent, %d move off it, "
                "%d cannot carry the state in colour at all\n",
                keep + moved + textOnly, keep, moved, textOnly);
    return flat;
}

}  // namespace

int main(int argc, char **argv) {
    int failures = 0;
    if (argc > 1) failures += deriveAll(argv[1]);
    failures += rampCheck();
    failures += sweepCube();

    std::printf("%s\n", failures == 0 ? "PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
