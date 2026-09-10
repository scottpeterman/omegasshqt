// integration/omegassh_theme_anytermqt.h
//
// Applying an omega::theme::Theme to an anytermqt terminal widget.
//
// Sits beside omegassh_anytermqt.h and for the same reason: it is the only
// place the theme library and the widget meet, and it is deliberately NOT part
// of any library target. omega_theme links nothing but the standard library and
// omegassh_qt links QtCore alone; putting a QtWidgets dependency in either
// would cost every headless consumer a Qt module. Include this from an
// application that already has both.
//
// Ported from ntermqt/widget.py's _apply_theme, _theme_font and
// _apply_overlay_style. What is worth knowing before changing anything here:
//
// TWO THEME KEYS HAVE NOWHERE TO GO. Every theme file carries cursorAccent and
// selectionForeground. qtpyte::Palette holds 16 ANSI slots plus foreground,
// background and cursor, and the widget's selection is a single colour -- so
// neither key is applied, by either application. They are carried in Theme
// anyway; this is where that stops.
//
// THE SELECTION ALPHA IS NOT DECORATION. anytermqt paints the selection over
// the glyphs without swapping the foreground, so a fully opaque
// selectionBackground erases the text under it and reads as a broken font
// colour. The widget ships with a translucent default (alpha 110 in 0.1.0), and
// applyTheme restores that alpha when -- and only when -- the theme's colour is
// opaque, so a theme that states its own alpha still wins. Capture the default
// with defaultSelectionAlpha() BEFORE the first applyTheme call, or the second
// call reads back the alpha the first one wrote.

#ifndef OMEGASSH_INTEGRATION_THEME_ANYTERMQT_H
#define OMEGASSH_INTEGRATION_THEME_ANYTERMQT_H

#include <QColor>
#include <QFont>
#include <QString>
#include <QStringList>

#include <qtpyte/palette.h>
#include <qtpyte/terminalwidget.h>

#include "theme/color.h"
#include "theme/theme.h"

namespace omega::theme {

// A theme colour as a QColor.
//
// parseCssColor handles the two forms the files use, including the rgba() one
// Qt would misread. Anything else -- a named CSS colour, say -- is handed to
// QColor, which has the table for it. That order matters: QColor would accept
// "rgba(30, 30, 46, 0.9)" in a stylesheet and read the alpha as 0..255, so 0.9
// becomes fully transparent.
inline QColor toQColor(const std::string &value,
                       const QColor &fallback = QColor(Qt::black)) {
    const Rgba c = parseCssColor(value);
    if (c.valid) {
        return QColor(c.r, c.g, c.b, c.a);
    }
    const QColor named(QString::fromStdString(value).trimmed());
    return named.isValid() ? named : fallback;
}

// A QFont from the theme's CSS-style family list.
//
// The generic "monospace" is dropped rather than passed on: it is a CSS
// keyword, not a family, and Qt would look for a face by that name and miss.
// StyleHint carries the same intent in the form Qt acts on.
inline QFont themeFont(const Theme &theme) {
    QStringList families;
    for (const QString &part :
         QString::fromStdString(theme.font_family).split(QLatin1Char(','))) {
        QString name = part.trimmed();
        while (name.size() >= 2 &&
               ((name.startsWith(QLatin1Char('"')) && name.endsWith(QLatin1Char('"'))) ||
                (name.startsWith(QLatin1Char('\'')) && name.endsWith(QLatin1Char('\''))))) {
            name = name.mid(1, name.size() - 2).trimmed();
        }
        if (name.isEmpty()) continue;
        if (name.compare(QLatin1String("monospace"), Qt::CaseInsensitive) == 0) {
            continue;
        }
        families << name;
    }

    QFont font;
    if (!families.isEmpty()) {
        font.setFamilies(families);
    }
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    font.setPointSize(theme.font_size);
    return font;
}

// The widget's own selection alpha, to be read once before any theme is
// applied and passed back to every applyTheme call afterwards.
inline int defaultSelectionAlpha(const qtpyte::TerminalWidget *widget) {
    return widget ? widget->selectionColor().alpha() : 110;
}

// Translates the theme's xterm.js colours into the widget's palette.
//
// A key the theme does not carry is left alone rather than defaulted, which is
// what the Python does: the widget keeps whatever it had, and a partial theme
// changes what it names and nothing else.
inline void applyTheme(qtpyte::TerminalWidget *widget, const Theme &theme,
                       int selectionAlpha) {
    if (!widget) return;

    qtpyte::Palette palette = widget->terminalPalette();

    if (theme.hasColor("background")) {
        palette.set_background(toQColor(theme.color("background"), QColor("#000000")));
    }
    if (theme.hasColor("foreground")) {
        palette.set_foreground(toQColor(theme.color("foreground"), QColor("#d0d0d0")));
    }
    if (theme.hasColor("cursor")) {
        palette.set_cursor(toQColor(theme.color("cursor"), QColor("#d0d0d0")));
    }
    for (int i = 0; i < 16; ++i) {
        const char *key = kAnsiKeys[i];
        if (theme.hasColor(key)) {
            palette.set_ansi(i, toQColor(theme.color(key)));
        }
    }

    // terminalPalette() hands back a copy, so nothing above has landed yet.
    widget->setTerminalPalette(palette);

    if (theme.hasColor("selectionBackground")) {
        QColor selection = toQColor(theme.color("selectionBackground"));
        if (selection.alpha() == 255) {
            selection.setAlpha(selectionAlpha);
        }
        widget->setSelectionColor(selection);
    }

    widget->setTerminalFont(themeFont(theme));
}

// The stylesheet for a connection overlay -- the themed label a terminal tab
// stacks over itself while a dial is in flight.
//
// The alpha is spelled out in 0..255 here because the theme states it as a CSS
// float and a Qt stylesheet would read 0.9 as very nearly transparent. That is
// the same trap toQColor exists for, arriving from the other direction: the
// value goes out through the parser and comes back as an integer.
inline QString overlayStylesheet(const Theme &theme) {
    const QColor bg = toQColor(theme.overlay_background, QColor(30, 30, 46, 230));
    const QColor fg = toQColor(theme.overlay_text_color, QColor("#cdd6f4"));

    return QStringLiteral("QLabel {"
                          " background-color: rgba(%1, %2, %3, %4);"
                          " color: %5;"
                          " font-size: %6px;"
                          " padding: 12px;"
                          "}")
        .arg(bg.red())
        .arg(bg.green())
        .arg(bg.blue())
        .arg(bg.alpha())
        .arg(fg.name())
        .arg(theme.font_size + 1);
}

// The overlay as a PANEL rather than a bare label: a message, a spinner and a
// Cancel button share one background, so the button reads as part of the
// overlay instead of floating on the terminal behind it.
//
// Kept beside overlayStylesheet() rather than replacing it -- that one styles
// a lone QLabel and is what a caller with no button wants.
//
// Scoped to #overlayPanel throughout. Applied to a child of the tab, an
// unscoped QPushButton rule would repaint every button under it.
inline QString overlayPanelStylesheet(const Theme &theme) {
    const QColor bg = toQColor(theme.overlay_background, QColor(30, 30, 46, 230));
    const QColor fg = toQColor(theme.overlay_text_color, QColor("#cdd6f4"));
    const QColor border = toQColor(theme.border_color, QColor("#313244"));
    const QColor accent = toQColor(theme.accent_color, QColor("#89b4fa"));

    return QStringLiteral(
               "#overlayPanel {"
               " background-color: rgba(%1, %2, %3, %4);"
               " border: 1px solid %5;"
               " border-radius: 6px;"
               "}"
               "#overlayPanel QLabel {"
               " background: transparent;"
               " color: %6;"
               " font-size: %7px;"
               " padding: 12px;"
               "}"
               "#overlayPanel QPushButton {"
               " background: transparent;"
               " color: %6;"
               " border: 1px solid %5;"
               " border-radius: 4px;"
               " padding: 4px 18px;"
               "}"
               "#overlayPanel QPushButton:hover {"
               " border-color: %8;"
               " color: %8;"
               "}")
        .arg(bg.red())
        .arg(bg.green())
        .arg(bg.blue())
        .arg(bg.alpha())
        .arg(border.name())
        .arg(fg.name())
        .arg(theme.font_size + 1)
        .arg(accent.name());
}

}  // namespace omega::theme

#endif  // OMEGASSH_INTEGRATION_THEME_ANYTERMQT_H
