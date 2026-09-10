// theme/tokens.h
//
// The redesign's token table, derived from a Theme rather than typed out.
//
// Section 1 of the spec says "define once; no other hex values in the
// codebase" and then lists fourteen hex values. Those are the same statement
// pointed in opposite directions, and this file resolves it the way the rest
// of the tree already resolves such things: the fourteen are a DERIVATION from
// the four chrome colours every theme file already carries -- background_color,
// foreground_color, border_color, accent_color -- and specTokens() reproduces
// the spec's own table from its own four inputs to within 2/255 per channel.
// `theme_probe rampcheck` prints the comparison.
//
// NO QT, for the same reason as the rest of omega_theme. The derivation is
// arithmetic on four colours; keeping it here means it configures and tests
// standalone --
//
//     cmake -S theme -B build/theme
//
// -- and that the 33-theme state survey runs headless in the probe rather than
// needing a QApplication and a display. The Qt side (fonts, QColor, the
// stylesheet) sits above this, in app/.
//
// NOTHING IS ADDED TO THE YAML. theme/theme.h is explicit that an unrecognised
// top-level key makes nterm-qt drop the whole file, so a `bg_raised:` entry
// would silently cost the other application the theme. Every surface here is
// derived; `warn` is borrowed from terminal_colors.yellow, which is the yellow
// the user has already read in every warning the emulator has printed.
//
// QT PARITY IS LOAD-BEARING. The ramp was fitted against the spec's table
// using QColor's integer HSV arithmetic, which is not the textbook one -- it
// carries 16 bits per channel internally and truncates on the way out. The
// helpers below reproduce QColor::toHsv, fromHsv, lighter and darker exactly,
// so a value derived here and a value derived from QColor agree bit for bit.
// tests/compat/token_differential.py checks that over all 33 themes. Change
// the arithmetic and that test is what tells you.

#ifndef OMEGA_THEME_TOKENS_H
#define OMEGA_THEME_TOKENS_H

#include <string>
#include <vector>

#include "theme/color.h"
#include "theme/theme.h"

namespace omega::theme {

// The token set one theme resolves to. Colours are Rgba rather than strings
// because half the consumers are painters and half are a stylesheet; hex()
// serves the second half.
struct Tokens {
    std::string name;
    bool dark = true;

    Rgba bgDeep;      // input wells
    Rgba bgBase;      // window body, dialog body
    Rgba bgRaised;    // sidebar, tab strip, status bar
    Rgba bgChrome;    // title bar, menus
    Rgba bgControl;   // secondary buttons, chips, pills
    Rgba bgSelected;  // selected row, active rail item
    Rgba bgHover;     // hovered row
    Rgba line;        // structural borders
    Rgba lineInput;   // input and secondary-button borders

    Rgba ink;
    Rgba inkDim;
    Rgba inkMuted;
    Rgba inkFaint;
    Rgba inkDetail;  // the session delegate's second line

    Rgba accent;
    Rgba onAccent;  // text on a primary button; derived, never assumed dark
    Rgba warn;

    // Destructive: the close button's hover wash, and whatever confirms a
    // delete later. Taken from terminal_colors "red" rather than invented,
    // for the same reason warn is taken from "yellow" -- it is the red the
    // user has already been reading as trouble in the emulator, and a chrome
    // red that disagrees with the terminal's is the kind of mismatch you
    // notice without being able to say why.
    Rgba danger;

    Rgba dead;

    // The live-state colour, which is NOT always the accent. Section 1
    // reserves the accent for two meanings -- "this connection is live" and
    // "this is the primary action" -- and those have different constraints. A
    // primary action only has to differ from other buttons. A state dot has to
    // differ from BODY TEXT, and on several shipped themes the accent does
    // not: vintage's #666666 sits three levels from its own foreground and
    // amiga's accent is the same black as its ink. So this is chosen by a
    // viability test and falls back to the accent only when the accent wins.
    Rgba state;

    // False when nothing in the theme cleared the threshold -- a genuinely
    // monochrome set like crt-green, where one phosphor is the whole palette.
    // On those, section 6's "never colour alone" stops being an accessibility
    // nicety and becomes the only channel left: a view that quietly dropped
    // the word beside its dot would be broken on three themes and fine on the
    // other thirty, which is the worst way for it to be broken. Read this
    // rather than assuming the dot carries meaning.
    bool stateIsDistinct = true;
    std::string stateSource = "accent";

    // The emulator's own colours, read and never derived. anytermqt already
    // paints from these, and a terminal whose background disagrees with the
    // palette it renders into is worse than one that does not match the
    // chrome.
    Rgba termBg;
    Rgba termFg;
};

// "#rrggbb". Lowercase, alpha dropped -- the same spelling QColor::name()
// produces, because the generated stylesheet is compared against Qt's.
std::string hex(const Rgba &c);

// "#aarrggbb", QColor::name(QColor::HexArgb)'s spelling. For the one token
// that carries alpha into the sheet.
std::string hexArgb(const Rgba &c);

// Channel-wise interpolation, rounded and clamped. t outside 0..1 extrapolates
// on purpose: `dead` sits at t=1.57.
Rgba mix(const Rgba &a, const Rgba &b, double t);

// Rec. 601 luma below 128. What every "is this dark" check in an emulator
// uses, and more than good enough for a branch.
bool isDark(const Rgba &c);

// Redmean, 0..765. Approximates perceptual distance well enough for a
// threshold and costs one sqrt. A plain channel difference calls #666666 and
// #666663 "3 apart" and also calls #000000 and #0000ff "255 apart", and those
// are not the same kind of 3 or the same kind of 255 -- which matters, because
// the decision this drives is "can this be told apart from body text".
double colorDistance(const Rgba &a, const Rgba &b);

// QColor::lighter / QColor::darker, reproduced including their 16-bit
// intermediate. Used for the accent's hover and pressed steps.
Rgba lighterBy(const Rgba &c, int factor);
Rgba darkerBy(const Rgba &c, int factor);

// The QColor primitives the ramp stands on, exposed for the differential
// rather than for callers -- pinning these against QColor over the whole
// colour cube is what stops the ramp drifting the next time someone tidies the
// arithmetic. Named *For so they do not read as "give me a colour".
int hueDegreesFor(const Rgba &c);   // QColor::hue(), -1 on a true grey
int saturationFor(const Rgba &c);   // QColor::hsvSaturation()
int valueFor(const Rgba &c);        // QColor::value()
Rgba fromHsvFor(int hueDeg, int sat, int val);  // QColor::fromHsv(int,int,int)

// Picks the live-state colour. Candidates are tried in order -- the accent
// first, then whatever the caller supplied -- and the FIRST that clears the
// floor wins, not the best-scoring one. Picking the maximum moves nord off a
// perfectly readable #88c0d0 and dracula off its purple onto green, which is a
// worse answer than the question asked: section 1 wants one accent, and a
// theme that does not need a second colour must not be given one.
//
// `source` names the winner and `distinct` is false when even the winner
// failed the floor. See Tokens::stateIsDistinct.
Rgba pickStateColor(const Rgba &bg, const Rgba &fg, const Rgba &accent,
                    const std::vector<Rgba> &extras,
                    const std::vector<std::string> &extraNames,
                    std::string *source, bool *distinct);

// The derivation proper. Surfaces interpolate bgBase toward `line`; ink fades
// toward a saturated version of the background's own hue. Neither needs a
// light/dark branch, because border_color is always one step from the
// background in the direction of the foreground whichever way that runs --
// gruvbox_light and enterprise_dark come out of the same amounts.
Tokens deriveTokens(const Rgba &bg, const Rgba &fg, const Rgba &border,
                    const Rgba &accent, const Rgba &warn, const Rgba &termBg,
                    const Rgba &termFg, const std::string &name,
                    const std::vector<Rgba> &stateCandidates = {},
                    const std::vector<std::string> &stateCandidateNames = {},
                    const Rgba *danger = nullptr);

// The whole job for one loaded theme: reads the four chrome colours, borrows
// `warn` from terminal_colors, assembles the state candidates and derives.
// This is what an application calls.
Tokens tokensFromTheme(const Theme &theme);

// The spec's own table, derived from the spec's own four inputs rather than
// typed. Also the last resort when no theme resolves.
Tokens specTokens();

}  // namespace omega::theme

#endif  // OMEGA_THEME_TOKENS_H
