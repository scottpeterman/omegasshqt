// theme/color.h
//
// Colour handling for the theme system, with no Qt in it.
//
// Two jobs, and they are deliberately separate. lighten() and darken() work on
// hex strings and feed the stylesheet generator, which is string-to-string all
// the way down -- that is what lets the generator be diff-tested against the
// Python it was ported from without a display, a QApplication or a linker
// argument. parseCssColor() produces numbers, for the callers that have to
// hand a colour to a widget.
//
// The parse accepts the two forms the theme files actually use:
//
//   #rrggbb                     every terminal_colors entry and every chrome
//                               colour
//   rgba(30, 30, 46, 0.9)       overlay_background, in all 33 files
//
// The second one is why this exists rather than passing the string to Qt. A Qt
// stylesheet reads rgba()'s fourth argument as 0..255, so 0.9 arrives as alpha
// 0 and the overlay is invisible. The theme files write it as a CSS float, so
// the value is parsed here and handed over as a number.
//
// Named CSS colours ("red", "cornflowerblue") are NOT handled. No theme file
// uses one, and supporting them means shipping a colour table to duplicate
// something QColor already has -- so a caller with Qt in reach falls back to
// QColor when this reports invalid. See integration/omegassh_theme_anytermqt.h.

#ifndef OMEGA_THEME_COLOR_H
#define OMEGA_THEME_COLOR_H

#include <string>

namespace omega::theme {

// Rgba is a parsed colour. valid is false when the text was not a form this
// understands, in which case the channels hold the fallback.
struct Rgba {
    int r = 0;
    int g = 0;
    int b = 0;
    int a = 255;
    bool valid = false;
};

// Parses "#rgb", "#rrggbb", "#rrggbbaa" or "rgb()/rgba()". The alpha argument
// of rgba() is read as a float when it is <= 1.0 and as 0..255 otherwise,
// which is what the Python does and what the files mean.
//
// On failure the fallback is parsed instead and valid is false, so a caller
// can tell "the theme said something odd" from "the theme said black".
Rgba parseCssColor(const std::string &text,
                   const std::string &fallback = "#000000");

// Moves a hex colour toward white or black by amount (0..1), channel by
// channel. Ported from the Python arithmetic exactly, truncation included:
// lighten is r + (255 - r) * amount, darken is r * (1 - amount), both floored.
//
// Input that is not #rrggbb is returned unchanged rather than throwing. The
// Python raises here; a stylesheet is not worth an exception, and a chrome
// colour that fails to derive shows up as a flat one rather than a crash.
std::string lighten(const std::string &hex, double amount);
std::string darken(const std::string &hex, double amount);

}  // namespace omega::theme

#endif  // OMEGA_THEME_COLOR_H
