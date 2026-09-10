// theme/color.cpp

#include "theme/color.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace omega::theme {
namespace {

std::string trim(const std::string &s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Reads a run of hex digits as one channel. Returns false on any non-hex
// character, which is what keeps "#gggggg" from parsing as black.
bool hexPair(const std::string &s, size_t at, int *out) {
    const int hi = hexDigit(s[at]);
    const int lo = hexDigit(s[at + 1]);
    if (hi < 0 || lo < 0) return false;
    *out = hi * 16 + lo;
    return true;
}

bool parseHex(const std::string &text, Rgba *out) {
    if (text.empty() || text[0] != '#') return false;
    const std::string d = text.substr(1);

    if (d.size() == 3) {
        // #abc means #aabbcc.
        for (int i = 0; i < 3; ++i) {
            const int v = hexDigit(d[i]);
            if (v < 0) return false;
            (i == 0 ? out->r : i == 1 ? out->g : out->b) = v * 17;
        }
        out->a = 255;
        return true;
    }
    if (d.size() == 6 || d.size() == 8) {
        if (!hexPair(d, 0, &out->r) || !hexPair(d, 2, &out->g) ||
            !hexPair(d, 4, &out->b)) {
            return false;
        }
        out->a = 255;
        if (d.size() == 8 && !hexPair(d, 6, &out->a)) return false;
        return true;
    }
    return false;
}

bool parseNumber(const std::string &s, double *out) {
    const std::string t = trim(s);
    if (t.empty()) return false;
    char *end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (end == t.c_str() || *end != '\0') return false;
    *out = v;
    return true;
}

bool parseFunctional(const std::string &text, Rgba *out) {
    const std::string t = lower(text);
    const bool isRgba = t.rfind("rgba(", 0) == 0;
    const bool isRgb = t.rfind("rgb(", 0) == 0;
    if (!isRgba && !isRgb) return false;

    const auto open = text.find('(');
    const auto close = text.find(')', open);
    if (open == std::string::npos || close == std::string::npos) return false;

    const std::string inner = text.substr(open + 1, close - open - 1);
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= inner.size()) {
        const auto comma = inner.find(',', start);
        if (comma == std::string::npos) {
            parts.push_back(inner.substr(start));
            break;
        }
        parts.push_back(inner.substr(start, comma - start));
        start = comma + 1;
    }
    if (parts.size() < 3) return false;

    double c[3];
    for (int i = 0; i < 3; ++i) {
        if (!parseNumber(parts[i], &c[i])) return false;
    }
    out->r = static_cast<int>(c[0]);
    out->g = static_cast<int>(c[1]);
    out->b = static_cast<int>(c[2]);

    out->a = 255;
    if (parts.size() > 3) {
        double alpha = 0.0;
        if (!parseNumber(parts[3], &alpha)) return false;
        // 0.9 is a CSS float; 230 is already 0..255. The files use the former
        // and Qt's stylesheet parser assumes the latter, which is the whole
        // reason this function exists.
        const int scaled = alpha <= 1.0 ? static_cast<int>(alpha * 255)
                                        : static_cast<int>(alpha);
        out->a = std::max(0, std::min(255, scaled));
    }
    return true;
}

}  // namespace

Rgba parseCssColor(const std::string &text, const std::string &fallback) {
    Rgba out;
    const std::string t = trim(text);

    if (parseFunctional(t, &out) || parseHex(t, &out)) {
        out.valid = true;
        return out;
    }

    Rgba fb;
    if (!parseHex(trim(fallback), &fb) && !parseFunctional(trim(fallback), &fb)) {
        fb = Rgba{};
        fb.a = 255;
    }
    fb.valid = false;
    return fb;
}

namespace {

// The derive helpers take a hex string and give one back, so they compose in
// the stylesheet without a colour type ever appearing in it.
std::string deriveHex(const std::string &hex, double amount, bool up) {
    const std::string t = trim(hex);
    Rgba c;
    if (!parseHex(t, &c) || t.size() != 7) {
        // Not #rrggbb. The Python raises; returning the input keeps a broken
        // chrome colour from taking the window down with it.
        return hex;
    }

    int ch[3] = {c.r, c.g, c.b};
    for (int &v : ch) {
        if (up) {
            v = std::min(255, static_cast<int>(v + (255 - v) * amount));
        } else {
            v = std::max(0, static_cast<int>(v * (1 - amount)));
        }
    }

    static const char *kHex = "0123456789abcdef";
    std::string out = "#";
    for (const int v : ch) {
        out += kHex[(v >> 4) & 0xF];
        out += kHex[v & 0xF];
    }
    return out;
}

}  // namespace

std::string lighten(const std::string &hex, double amount) {
    return deriveHex(hex, amount, true);
}

std::string darken(const std::string &hex, double amount) {
    return deriveHex(hex, amount, false);
}

}  // namespace omega::theme
