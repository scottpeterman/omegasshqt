// theme/stylesheet.h
//
// Qt stylesheet generation from a Theme. Port of ntermqt/theme/stylesheet.py.
//
// Returns a std::string and takes no Qt type, which is not an accident: the
// Python it was ported from is string-to-string too, so the two can be run over
// all 33 themes and byte-compared. tests/compat/theme_differential.py does
// exactly that, and it is the reason this file can be trusted before a single
// widget exists to look at.
//
// The QSS body is the Python's, carried across character for character
// including its leading newline, its indentation and its trailing spaces --
// the parts a human would tidy up are the parts that would break the
// comparison. Only the braces changed: an f-string doubles them and a raw
// string literal does not.

#ifndef OMEGA_THEME_STYLESHEET_H
#define OMEGA_THEME_STYLESHEET_H

#include <string>

#include "theme/theme.h"

namespace omega::theme {

// The application-wide stylesheet for one theme. Covers about twenty widget
// classes; hover is the accent lightened 10%, pressed is darkened 10%, and two
// background tints come from the same two helpers.
std::string generateStylesheet(const Theme &theme);

}  // namespace omega::theme

#endif  // OMEGA_THEME_STYLESHEET_H
