// theme/tokenstylesheet.h
//
// The redesign's Qt stylesheet, generated from a token set.
//
// SEPARATE FROM stylesheet.h ON PURPOSE, and not because two generators is
// tidy. generateStylesheet() is a byte-for-byte port of ntermqt's
// stylesheet.py and tests/compat/theme_differential.py compares the two over
// all 33 themes -- that comparison is the only thing keeping Omega's chrome
// and nterm-qt's chrome the same artifact. This sheet is a different sheet:
// different selectors, nine surface tokens where the other has four, and
// property-driven roles. The two cannot be converged, so they sit side by side
// and the application picks.
//
// That makes retiring the old one a decision somebody makes on purpose rather
// than a merge conflict. When it goes, theme_differential.py goes with it, and
// so does the last thing tying Omega's appearance to the other application.
//
// STILL NO QT. Same reason as the rest of the library: this is string to
// string, so `theme_probe tokenstylesheet <file.yaml>` prints the whole sheet
// with no display, no QApplication and no platform plugin, which is how a
// selector change gets looked at before it is compiled into anything.
//
// WHAT THIS SHEET DELIBERATELY DOES NOT COVER:
//
//   The terminal. anytermqt paints from a qtpyte::Palette, not from QSS, so
//   the emulator's colours travel through integration/omegassh_theme_anytermqt.h
//   as they always have. A QSS rule aimed at it would be dead at best and
//   would fight the palette at worst. Tokens carries termBg and termFg for
//   the callers that need to match something to the terminal -- a splitter
//   handle beside it, say -- not for painting the terminal itself.

#ifndef OMEGA_THEME_TOKENSTYLESHEET_H
#define OMEGA_THEME_TOKENSTYLESHEET_H

#include <string>

#include "theme/tokens.h"

namespace omega::theme {

// The application-wide sheet for one token set.
//
// Widgets opt into the role selectors by property rather than by carrying an
// inline stylesheet, which is not a style preference: an inline sheet captures
// its colours at construction and no repolish revisits it, so a widget styled
// that way is a widget that stops changing with the theme. The properties this
// sheet reads are:
//
//   role     chrome, rail, well, bar, ribbon, tabhost, divider, pill, notice
//            plus the label roles fieldlabel, desc, wordmark, corner,
//            ribbonname, notice
//   primary  "true" on the one button that is the default action
//   segment  "true" on a member of a segmented control
//   flatlink "true" on a button that should read as a link
//   chip     "true", or accent / state / warn for a tinted one
//   tone     ink, accent, state, warn, hint, muted
//   mono     "true" for the monospace face
//   bare     "true" on a plain QWidget used only as a layout container
//
// `bare` is the one that looks like noise and is not. The blanket QWidget rule
// paints bg.base behind every widget, including a container parented onto the
// chrome bar, which then shows as a rectangle of the wrong colour. Labels are
// handled with a blanket transparent rule; containers cannot be, because some
// of them genuinely want a background. The real fix is to stop putting a
// background on the blanket QWidget selector at all and set it on QMainWindow,
// QDialog and the role frames instead -- a bigger change than this slice, and
// the reason it is called out here rather than left as a puzzle.
// baseFontPx is the body text size; the label, chip and hint sizes are derived
// from it one point apart each, preserving the hierarchy the literals had.
// Default 13 is what the sheet carried hardcoded, so an existing caller that
// does not pass one gets byte-identical output.
//
// PX, NOT PT, and deliberately: QSS speaks px, so a caller that also sets
// QApplication::setFont() can use QFont::setPixelSize with the same number and
// have the two agree exactly instead of drifting by the DPI conversion.
std::string generateTokenStylesheet(const Tokens &tokens, int baseFontPx = 13);

}  // namespace omega::theme

#endif  // OMEGA_THEME_TOKENSTYLESHEET_H
