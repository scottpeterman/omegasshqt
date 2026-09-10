// theme/tokens.cpp

#include "theme/tokens.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace omega::theme {
namespace {

// --- Qt parity -----------------------------------------------------------
//
// Qt 6's QColor carries HSV as FLOAT, not as the 16-bit integers Qt 5 used,
// and converts back to bytes by rounding rather than by shifting. The
// difference is one level and it is not cosmetic: it moves nine of the shipped
// themes' hover and disabled colours, and the ramp was fitted against Qt's
// answers. #0078d4 darkened 112 is #006bbd through the float path and #006bbe
// through the 16-bit one -- the second is what a textbook port produces, and
// it is wrong here.
//
// So these five reproduce QColor::toHsv, fromHsv, hue, lighter and darker on
// the float arithmetic. `float` and not `double` on purpose: QColor's own
// intermediates are single precision, and the rounding boundary is where the
// two disagree. tests/compat/token_differential pins all five against QColor
// over a sweep; do not "clean this up" to double without running it.

constexpr int kU16 = 65535;
constexpr int kNoHue = 65535;  // USHRT_MAX, QColor's "no hue"

int roundHalfUp(float d) {
    return d >= 0.0f ? int(d + 0.5f) : int(d - 0.5f);
}

// The double overload is for mix(), which interpolates in double and is not
// part of the QColor parity surface.
int roundHalfUp(double d) {
    return d >= 0.0 ? int(d + 0.5) : int(d - 0.5);
}

// A 16-bit channel down to a byte. NOT `>> 8`. The shift is what a textbook
// port writes and it is wrong by one level on nine of the shipped themes --
// see the note above.
int to255(int x16) {
    return std::max(0, std::min(255, roundHalfUp(x16 * 255.0f / kU16)));
}

struct Hsv16 {
    int hue = kNoHue;  // degrees * 100, or kNoHue
    int sat = 0;       // 0..65535
    int val = 0;       // 0..65535
};

Hsv16 toHsv(const Rgba &c) {
    const float r = c.r / 255.0f;
    const float g = c.g / 255.0f;
    const float b = c.b / 255.0f;

    const float maxv = std::max(r, std::max(g, b));
    const float minv = std::min(r, std::min(g, b));
    const float delta = maxv - minv;

    Hsv16 out;
    out.val = roundHalfUp(maxv * kU16);

    // Compared on the bytes rather than on the floats: three equal channels
    // are exactly equal after the divide, so this asks the same question
    // qFuzzyIsNull(delta) asks without needing the epsilon.
    if (c.r == c.g && c.g == c.b) {
        out.hue = kNoHue;
        out.sat = 0;
        return out;
    }

    float h;
    if (r == maxv) {
        h = (g - b) / delta;
    } else if (g == maxv) {
        h = 2.0f + (b - r) / delta;
    } else {
        h = 4.0f + (r - g) / delta;
    }
    h *= 60.0f;
    if (h < 0.0f) h += 360.0f;

    out.hue = roundHalfUp(h * 100.0f);
    out.sat = roundHalfUp((delta / maxv) * kU16);
    return out;
}

Rgba fromHsv16(const Hsv16 &c) {
    Rgba out;
    out.a = 255;
    out.valid = true;

    if (c.sat == 0 || c.hue == kNoHue) {
        out.r = out.g = out.b = to255(c.val);
        return out;
    }

    const float h = c.hue / 6000.0f;  // hundredths of a degree into sextants
    const float s = c.sat / float(kU16);
    const float v = c.val / float(kU16);

    const int i = int(h);
    const float f = h - i;
    const float p = v * (1.0f - s);

    int r16 = 0, g16 = 0, b16 = 0;
    if (i & 1) {
        const float q = v * (1.0f - (s * f));
        switch (i) {
            case 1: r16 = roundHalfUp(q * kU16); g16 = c.val; b16 = roundHalfUp(p * kU16); break;
            case 3: r16 = roundHalfUp(p * kU16); g16 = roundHalfUp(q * kU16); b16 = c.val; break;
            case 5: r16 = c.val; g16 = roundHalfUp(p * kU16); b16 = roundHalfUp(q * kU16); break;
            default: break;
        }
    } else {
        const float t = v * (1.0f - (s * (1.0f - f)));
        switch (i) {
            case 0: r16 = c.val; g16 = roundHalfUp(t * kU16); b16 = roundHalfUp(p * kU16); break;
            case 2: r16 = roundHalfUp(p * kU16); g16 = c.val; b16 = roundHalfUp(t * kU16); break;
            case 4: r16 = roundHalfUp(t * kU16); g16 = roundHalfUp(p * kU16); b16 = c.val; break;
            case 6: r16 = c.val; g16 = roundHalfUp(t * kU16); b16 = roundHalfUp(p * kU16); break;
            default: break;
        }
    }

    out.r = to255(r16);
    out.g = to255(g16);
    out.b = to255(b16);
    return out;
}

// QColor::fromHsv(int, int, int). The 8-bit arguments widen by *0x101 and not
// by <<8, so 255 maps to 65535 rather than to 65280.
Rgba fromHsvInt(int h, int s, int v) {
    Hsv16 c;
    c.hue = h < 0 ? kNoHue : (h % 360) * 100;
    c.sat = h < 0 ? 0 : s * 0x101;
    c.val = v * 0x101;
    return fromHsv16(c);
}

// QColor::hue(): whole degrees, -1 on a true grey.
int hueDegrees(const Rgba &c) {
    const Hsv16 h = toHsv(c);
    return h.hue == kNoHue ? -1 : h.hue / 100;
}

int hsvSaturation8(const Rgba &c) { return to255(toHsv(c).sat); }
int value8(const Rgba &c) { return to255(toHsv(c).val); }

// Where ink fades to. Saturating the background's own hue is what keeps the
// muted steps cool on a cool theme and warm on a warm one; fading toward
// neutral grey is the version that looks dead, and fading toward the
// background is the version that loses contrast too fast.
Rgba inkFadeTarget(const Rgba &bg, bool dark) {
    int hue = hueDegrees(bg);
    int sat = std::min(255, hsvSaturation8(bg) * 5);
    if (hue < 0) {  // a true grey has no hue; do not invent one
        hue = 0;
        sat = 0;
    }
    const int v = value8(bg);
    const int val = dark ? std::min(90, int(v * 1.6))
                         : std::max(200, 255 - int((255 - v) * 1.6));
    return fromHsvInt(hue, sat, val);
}

Rgba parseOr(const std::string &text, const char *fallback) {
    Rgba c = parseCssColor(text, fallback);
    c.valid = true;
    return c;
}

}  // namespace

// --- small helpers -------------------------------------------------------

std::string hex(const Rgba &c) {
    static const char *kHex = "0123456789abcdef";
    std::string out = "#";
    for (const int v : {c.r, c.g, c.b}) {
        out += kHex[(v >> 4) & 0xF];
        out += kHex[v & 0xF];
    }
    return out;
}

std::string hexArgb(const Rgba &c) {
    static const char *kHex = "0123456789abcdef";
    std::string out = "#";
    for (const int v : {c.a, c.r, c.g, c.b}) {
        out += kHex[(v >> 4) & 0xF];
        out += kHex[v & 0xF];
    }
    return out;
}

Rgba mix(const Rgba &a, const Rgba &b, double t) {
    const auto ch = [t](int x, int y) {
        return std::max(0, std::min(255, roundHalfUp(x + (y - x) * t)));
    };
    Rgba out;
    out.r = ch(a.r, b.r);
    out.g = ch(a.g, b.g);
    out.b = ch(a.b, b.b);
    out.a = 255;
    out.valid = true;
    return out;
}

bool isDark(const Rgba &c) {
    return (0.299 * c.r + 0.587 * c.g + 0.114 * c.b) < 128.0;
}

double colorDistance(const Rgba &a, const Rgba &b) {
    const double rm = (a.r + b.r) / 2.0;
    const double dr = a.r - b.r;
    const double dg = a.g - b.g;
    const double db = a.b - b.b;
    return std::sqrt((2 + rm / 256.0) * dr * dr + 4 * dg * dg +
                     (2 + (255 - rm) / 256.0) * db * db);
}

int hueDegreesFor(const Rgba &c) { return hueDegrees(c); }
int saturationFor(const Rgba &c) { return hsvSaturation8(c); }
int valueFor(const Rgba &c) { return value8(c); }
Rgba fromHsvFor(int hueDeg, int sat, int val) {
    return fromHsvInt(hueDeg, sat, val);
}

Rgba lighterBy(const Rgba &c, int factor) {
    if (factor <= 0) return c;
    if (factor < 100) return darkerBy(c, 10000 / factor);
    Hsv16 h = toHsv(c);
    // Integer, on the 16-bit intermediate. Scaling the float value instead
    // rounds the other way on pctools and solarized.
    long long v = (long long)factor * h.val / 100;
    if (v > kU16) {
        // Overflowed the value; spend the excess out of saturation instead,
        // which is what keeps a lightened saturated colour heading for white
        // rather than clipping to a flat one.
        h.sat -= int(v - kU16);
        if (h.sat < 0) h.sat = 0;
        v = kU16;
    }
    h.val = int(v);
    return fromHsv16(h);
}

Rgba darkerBy(const Rgba &c, int factor) {
    if (factor <= 0) return c;
    if (factor < 100) return lighterBy(c, 10000 / factor);
    Hsv16 h = toHsv(c);
    h.val = int((long long)h.val * 100 / factor);
    return fromHsv16(h);
}

// --- the state colour ----------------------------------------------------

Rgba pickStateColor(const Rgba &bg, const Rgba &fg, const Rgba &accent,
                    const std::vector<Rgba> &extras,
                    const std::vector<std::string> &extraNames,
                    std::string *source, bool *distinct) {
    // A state colour has two jobs at once: it must not read as body text, and
    // it must be visible against the surface it sits on. Scoring by the
    // SMALLER of the two distances is what stops a candidate that aces one and
    // fails the other from winning.
    //
    // 110 is calibrated against the shipped set: every theme whose accent is a
    // genuinely different colour from its text clears it, and the ones where
    // the accent IS the text do not.
    constexpr double kFloor = 110.0;

    const auto score = [&](const Rgba &c) {
        return std::min(colorDistance(c, fg), colorDistance(c, bg));
    };

    struct Cand {
        Rgba c;
        std::string name;
    };
    std::vector<Cand> cands;
    cands.push_back({accent, "accent"});
    for (size_t i = 0; i < extras.size(); ++i) {
        cands.push_back(
            {extras[i], i < extraNames.size() ? extraNames[i] : "extra"});
    }

    // Last resort: the accent's own hue, saturated and pushed away from the
    // background. Rescues a theme whose accent is the right idea at the wrong
    // intensity; cannot rescue one that has no second colour at all.
    {
        const int ah = hueDegrees(accent);
        const int hue = ah >= 0 ? ah : hueDegrees(bg);
        if (hue >= 0) {
            cands.push_back(
                {fromHsvInt(hue, 235, isDark(bg) ? 225 : 150), "synthesized"});
        }
    }

    for (const Cand &c : cands) {
        if (score(c.c) < kFloor) continue;
        if (source) *source = c.name;
        if (distinct) *distinct = true;
        return c.c;
    }

    // Nothing cleared it. Hand back whatever came closest so the dot still
    // renders, and say so.
    Rgba best = accent;
    std::string bestName = "accent";
    double bestScore = -1.0;
    for (const Cand &c : cands) {
        const double sc = score(c.c);
        if (sc > bestScore) {
            bestScore = sc;
            best = c.c;
            bestName = c.name;
        }
    }
    if (source) *source = bestName;
    if (distinct) *distinct = false;
    return best;
}

// --- the ramp ------------------------------------------------------------

Tokens deriveTokens(const Rgba &bg, const Rgba &fg, const Rgba &border,
                    const Rgba &accent, const Rgba &warn, const Rgba &termBg,
                    const Rgba &termFg, const std::string &name,
                    const std::vector<Rgba> &stateCandidates,
                    const std::vector<std::string> &stateCandidateNames,
                    const Rgba *danger) {
    Tokens t;
    t.name = name;
    t.dark = isDark(bg);

    // A theme whose border is its background collapses the whole ramp to one
    // flat colour. None of the 33 shipped files does this -- the closest is
    // well clear -- but a hand-written one can, and a flat window is a worse
    // failure than an approximated border.
    Rgba line = border;
    const int spread = std::max({std::abs(bg.r - border.r),
                                 std::abs(bg.g - border.g),
                                 std::abs(bg.b - border.b)});
    if (spread < 10) line = mix(bg, fg, 0.12);

    // The other end of the same problem. borland.yaml puts a light grey border
    // on a saturated blue background: a legitimate 1px border and a ruinous
    // ramp, because bgSelected lands most of the way to white and the white
    // text on it disappears. So the ramp runs toward a CLAMPED copy of the
    // border while real borders keep the theme's declared colour.
    //
    // 0.22 leaves the spec's own ramp untouched -- its border sits at 0.11 of
    // the way from bgBase to ink -- while capping `dead`, the furthest step at
    // t=1.57, at roughly a third of that distance.
    Rgba rampLine = line;
    {
        const double dr = fg.r - bg.r, dg = fg.g - bg.g, db = fg.b - bg.b;
        const double denom = dr * dr + dg * dg + db * db;
        if (denom > 1.0) {
            const double a = ((line.r - bg.r) * dr + (line.g - bg.g) * dg +
                              (line.b - bg.b) * db) /
                             denom;
            if (a > 0.22) rampLine = mix(bg, fg, 0.22);
        }
    }

    t.bgBase = bg;
    t.bgDeep = mix(bg, rampLine, -0.36);
    t.bgRaised = mix(bg, rampLine, 0.19);
    t.bgChrome = mix(bg, rampLine, 0.26);
    t.bgHover = mix(bg, rampLine, 0.47);
    t.bgControl = mix(bg, rampLine, 0.54);
    t.bgSelected = mix(bg, rampLine, 0.71);
    t.dead = mix(bg, rampLine, 1.57);

    t.line = line;
    t.lineInput = mix(line, fg, 0.04);

    const Rgba fade = inkFadeTarget(bg, t.dark);
    t.ink = fg;
    t.inkDim = mix(fg, fade, 0.198);
    t.inkDetail = mix(fg, fade, 0.324);
    t.inkMuted = mix(fg, fade, 0.396);
    t.inkFaint = mix(fg, fade, 0.534);

    // Defaulted rather than required, so the older six-colour call sites keep
    // working and a theme with no red still gets a plausible one.
    t.danger = danger ? *danger : parseOr("#c05050", "#c05050");

    t.accent = accent;
    // Never assume the accent is light. #56cfa1 takes dark text; enterprise
    // dark's #0078d4 takes light text, and hardcoding bgDeep here is how the
    // primary button goes unreadable on a third of the shipped themes.
    t.onAccent = isDark(accent) ? parseOr("#ffffff", "#ffffff") : t.bgDeep;
    t.warn = warn;

    t.state = pickStateColor(bg, fg, accent, stateCandidates,
                             stateCandidateNames, &t.stateSource,
                             &t.stateIsDistinct);

    t.termBg = termBg;
    t.termFg = termFg;
    return t;
}

Tokens tokensFromTheme(const Theme &theme) {
    const Rgba bg = parseOr(theme.background_color, "#16181b");
    const Rgba fg = parseOr(theme.foreground_color, "#e6e9ec");
    const Rgba border = parseOr(theme.border_color, "#2c3238");
    const Rgba accent = parseOr(theme.accent_color, "#56cfa1");

    // The needs-input colour. Nothing in the format names one and inventing a
    // key is not available -- see the header. terminal_colors already carries
    // a yellow the user has looked at in every warning the emulator has ever
    // printed.
    Rgba warn;
    if (theme.hasColor("yellow")) {
        warn = parseOr(theme.color("yellow"), "#e0a458");
    } else if (theme.hasColor("brightYellow")) {
        warn = parseOr(theme.color("brightYellow"), "#e0a458");
    } else {
        warn = parseOr("#e0a458", "#e0a458");
    }

    // Read, never derived.
    const Rgba termBg = parseOr(theme.color("background", "#0e1012"), "#0e1012");
    const Rgba termFg = parseOr(theme.color("foreground", "#b7c0c8"), "#b7c0c8");

    // Candidates for the live-state colour, in the order they should be tried.
    // Green first because it is what the emulator already prints success in,
    // so the user has been reading it as "up" for years; cyan after it because
    // a handful of themes have no usable green at all.
    std::vector<Rgba> extras;
    std::vector<std::string> names;
    for (const char *key : {"green", "brightGreen", "cyan", "brightCyan"}) {
        if (!theme.hasColor(key)) continue;
        extras.push_back(parseOr(theme.color(key), "#000000"));
        names.emplace_back(key);
    }

    Rgba danger;
    if (theme.hasColor("red")) {
        danger = parseOr(theme.color("red"), "#c05050");
    } else if (theme.hasColor("brightRed")) {
        danger = parseOr(theme.color("brightRed"), "#c05050");
    } else {
        danger = parseOr("#c05050", "#c05050");
    }

    return deriveTokens(bg, fg, border, accent, warn, termBg, termFg,
                        theme.name, extras, names, &danger);
}

Tokens specTokens() {
    // The four inputs are the spec's own bgBase, ink, line and accent. Every
    // other value in its table comes out of the derivation above.
    return deriveTokens(parseOr("#16181b", "#16181b"),
                        parseOr("#e6e9ec", "#e6e9ec"),
                        parseOr("#2c3238", "#2c3238"),
                        parseOr("#56cfa1", "#56cfa1"),
                        parseOr("#e0a458", "#e0a458"),
                        parseOr("#0e1012", "#0e1012"),
                        parseOr("#b7c0c8", "#b7c0c8"), "omega-dark");
}

}  // namespace omega::theme
