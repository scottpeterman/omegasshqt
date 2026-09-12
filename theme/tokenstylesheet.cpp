// theme/tokenstylesheet.cpp

#include "theme/tokenstylesheet.h"

#include <algorithm>
#include <string>
#include <vector>

namespace omega::theme {
namespace {

void substitute(std::string *s, const std::string &token,
                const std::string &value) {
    const std::string needle = "{" + token + "}";
    size_t at = 0;
    while ((at = s->find(needle, at)) != std::string::npos) {
        s->replace(at, needle.size(), value);
        at += value.size();
    }
}

// A colour laid over a surface at some fraction. Chips need an
// accent-flavoured background that still reads as a surface, and QSS has no
// colour arithmetic of its own -- so it is done here and the sheet gets a
// literal.
//
// Truncating rather than rounding, which is what the preview did and what the
// tinted values were eyeballed against. It is one level either way and it is
// not worth two behaviours.
std::string tint(const Rgba &c, const Rgba &over, double amount) {
    Rgba out;
    out.r = int(over.r + (c.r - over.r) * amount);
    out.g = int(over.g + (c.g - over.g) * amount);
    out.b = int(over.b + (c.b - over.b) * amount);
    out.a = 255;
    out.valid = true;
    return hex(out);
}

// The sheet.
//
// Ordered by how far a rule reaches: the blanket rules first, then inputs and
// buttons, then the role frames, then the specific widgets. A later rule of
// equal specificity wins in QSS, so the order is load-bearing in exactly the
// places it is called out.
// The font stacks.
//
// A bare font-family: "IBM Plex Mono" is not a request for a monospace face,
// it is a request for THAT face -- and when it is absent Qt's matcher falls
// through to the default family, which on a stock Ubuntu box is DejaVu SANS.
// Measured, not assumed: a probe reports requested=IBM Plex Mono
// resolved=DejaVu Sans, so the fingerprint the host key prompt exists to make
// comparable was being drawn in a proportional face. The generic `monospace`
// at the end of the chain is what fontconfig, CoreText and DirectWrite each
// resolve to a real fixed-pitch face, so the worst case is the platform's own
// terminal font rather than a proportional one.
constexpr const char *kSansStack =
    "\"IBM Plex Sans\", \"Segoe UI\", \"Helvetica Neue\", sans-serif";
constexpr const char *kMonoStack =
    "\"IBM Plex Mono\", \"DejaVu Sans Mono\", \"SF Mono\", Menlo, "
    "Consolas, monospace";

constexpr const char *kTemplate = R"QSS(
/* ---- blanket ---------------------------------------------------------- */
QWidget { background: {bg.base}; color: {ink};
          font-family: {ff.sans}; font-size: {fs.base}px; }

/* QLabel is a QWidget, so the rule above paints a rectangle behind every
   label sitting on a frame that is not bg.base -- the rail, the chrome bar,
   the status bar. Same for a plain container used only for layout, which opts
   out with setProperty("bare", true) because some containers do want a
   background and there is no way to tell them apart from a selector. */
QLabel { background: transparent; }
QWidget[bare="true"] { background: transparent; }

QMainWindow, QDialog { background: {bg.base}; }

/* A MODAL WITH NO TITLE BAR NEEDS AN EDGE OF ITS OWN. On the frameless path
   the dialog and the window behind it both paint bg.base, so there is nothing
   marking where one ends -- the body's left, right and bottom sides vanish and
   the modal reads as a title strip floating over the application with some
   fields loose beneath it. The strip and the footer look fine, which is what
   makes it hard to see as a missing frame: they carry bg.chrome and the bar
   surface, so only the middle disappears.

   A BORDER RATHER THAN A SHADE. Lifting the dialog a step off bg.base is the
   other obvious answer and it fails on the monochrome sets: crt-green's whole
   palette is one phosphor, so a derived surface a step lighter than #000000 is
   a colour nobody can see against #000000, and the modal is still unframed on
   exactly the themes with the least to work with. `line` is the token that
   already means "structural edge", it is chosen for contrast against the
   surfaces rather than derived from them, and it lands the same on a light
   theme as on a dark one.

   The property comes from ModalFrame, and only on the frameless path -- a
   border inside a window manager's own frame is just a line. */
QDialog[chromeless="true"] { background: {bg.base};
    border: 1px solid {line}; }

/* ---- inputs ----------------------------------------------------------- */
QLineEdit, QSpinBox, QComboBox, QPlainTextEdit, QTextEdit {
    background: {bg.deep}; border: 1px solid {line.input};
    border-radius: 7px; padding: 8px 11px; color: {ink};
    selection-background-color: {accent};
    selection-color: {on.accent}; }
QLineEdit:focus, QSpinBox:focus, QComboBox:focus, QPlainTextEdit:focus {
    border: 1px solid {accent}; }

/* The field an error notice is about. Set invalid=true alongside the notice
   and unpolish/polish the widget -- a property changed after the widget has
   been polished does not restyle on its own, which is the one thing about
   property selectors that bites after construction. Kept to the border: a
   tinted background behind a password field makes the dots harder to count,
   and the notice is already carrying the words. Both rules are needed: the
   :focus rule above would otherwise win back the accent border the moment
   the field somebody is being asked to correct takes focus. */
QLineEdit[invalid="true"], QComboBox[invalid="true"] {
    border: 1px solid {danger}; }
QLineEdit[invalid="true"]:focus, QComboBox[invalid="true"]:focus {
    border: 1px solid {danger}; }
QLineEdit:disabled, QSpinBox:disabled, QComboBox:disabled {
    color: {ink.disabled}; background: {bg.base}; }
QLineEdit[mono="true"], QPlainTextEdit[mono="true"], QSpinBox {
    font-family: {ff.mono}; }

/* The spin buttons and the combo drop-down are DELIBERATELY UNSTYLED.
   Giving QSpinBox::up-button a background and a width without also giving it
   an `image` makes the style stop drawing its arrow: the subcontrol renders as
   a flat block, and the widget loses the only thing that says it is a spin box
   rather than a line edit. Measured, not assumed -- the indicator strip goes
   from 20 distinct colours to 2. The ported sheet in theme/stylesheet.cpp
   carries the same rule and has the same problem; it is not introduced here.

   Supplying arrow images is the other way out, and it means shipping four
   PNGs per direction per state that no theme can recolour. Leaving the
   subcontrols alone costs nothing and the combo's arrow comes back -- the
   ported sheet draws no combo arrow at all, so this is already the better of
   the two.

   THE SPIN ARROWS ARE FIXED, and the diagnosis above was half wrong.
   QStyleSheetStyle does NOT refuse to draw them: it delegates
   PE_IndicatorSpinUp and PE_IndicatorSpinDown to the base style whenever the
   up-arrow subcontrol has no contents size of its own. What was missing was
   room. `padding: 8px 11px` above takes the field's width, the subcontrol rect
   came back five pixels wide, and the arrows were drawn -- unreadably, in the
   base palette, against the rounded border. The strip measured 23 distinct
   colours and every one of them was border antialiasing.

   So the rules below give the buttons a width, and app/spinarrowstyle.h
   paints into it from the theme's own ink. Neither half works alone.

   GEOMETRY ONLY. Do not add `background`, `border` or `margin` to either
   button. Any of them makes QStyleSheetStyle treat the subcontrol as drawable,
   at which point it stops delegating and paints the sheet's version instead --
   which is nothing. Verified in both directions: with width alone the
   primitive fires; adding `border: none; background: transparent` drops the
   call count to zero and the arrows disappear. `padding-right` on the field is
   what keeps a long value from running under them.

   The ported sheet in theme/stylesheet.cpp still has the narrow-strip problem.
   It is a byte-for-byte port and widening it there would end that, so it is
   left alone -- the proxy style is installed either way, so the arrows it
   draws are simply cramped rather than absent. */
QSpinBox { padding-right: 26px; }
QSpinBox::up-button { subcontrol-origin: border;
    subcontrol-position: top right; width: 26px; }
QSpinBox::down-button { subcontrol-origin: border;
    subcontrol-position: bottom right; width: 26px; }
QComboBox QAbstractItemView {
    background: {bg.chrome}; color: {ink};
    border: 1px solid {line};
    selection-background-color: {bg.selected};
    selection-color: {ink}; }

/* ---- buttons ---------------------------------------------------------- */
QPushButton { background: {bg.control}; border: 1px solid {line.input};
    border-radius: 7px; padding: 8px 18px; color: {ink.dim}; }
QPushButton:hover  { background: {bg.selected}; color: {ink}; }
QPushButton:pressed{ background: {bg.chrome}; }
/* ink.disabled is derived, not `line`. The old sheet used the border colour
   here, which on a light theme is a pale grey on a pale background -- the
   disabled Connect and New buttons in the session rail are legible in neither
   of the two shipped light themes. */
QPushButton:disabled { color: {ink.disabled}; border-color: {bg.selected};
    background: {bg.base}; }
QPushButton[primary="true"] { background: {accent}; color: {on.accent};
    border: none; font-weight: 600; }
QPushButton[primary="true"]:hover { background: {accent.hover}; }
QPushButton[primary="true"]:disabled { background: {bg.control};
    color: {ink.disabled}; }

/* The one button on a dialog that destroys something. Border and text rather
   than a fill: the filled button in a dialog is the primary one, the primary
   one is what Return activates, and a destructive action must never be what
   Return activates. The fill arrives on hover, by which point the pointer is
   already on the button and the colour is confirming rather than inviting.

   Disabled falls through to the ordinary QPushButton:disabled rule above --
   a greyed-out Delete is a Delete that cannot run, and painting it in the
   danger colour would say the opposite. */
QPushButton[destructive="true"] { background: {bg.control};
    border: 1px solid {danger}; color: {danger}; }
QPushButton[destructive="true"]:hover { background: {danger};
    color: {on.accent}; border-color: {danger}; }
QPushButton[destructive="true"]:pressed { background: {danger};
    color: {on.accent}; }

QPushButton[segment="true"] { background: transparent; border: none;
    border-radius: 6px; padding: 6px 0; color: {ink.dim}; }
QPushButton[segment="true"]:hover { background: {bg.hover}; color: {ink}; }
QPushButton[segment="true"]:checked { background: {accent};
    color: {on.accent}; font-weight: 600; }
QPushButton[flatlink="true"] { background: transparent; border: none;
    color: {ink.dim}; padding: 4px 0; text-align: left; }
QPushButton[flatlink="true"]:hover { color: {accent}; }

QCheckBox { color: {ink}; spacing: 8px; background: transparent; }
/* An explicit QSS colour beats the disabled palette, so the rule above would
   otherwise keep a disabled caption at full {ink} while its indicator dimmed --
   a row that is greyed out everywhere except the words naming it. QLabel has
   had this rule all along, which is why disabled FIELD LABELS looked right and
   made the checkboxes beside them look like the anomaly they were. */
QCheckBox:disabled { color: {ink.disabled}; }
QCheckBox::indicator { width: {ctl.indicator}px; height: {ctl.indicator}px;
    border-radius: 4px; border: 1px solid {line.input};
    background: {bg.deep}; }
QCheckBox::indicator:checked { background: {accent};
    border-color: {accent}; }
QCheckBox::indicator:hover { border-color: {accent}; }
QCheckBox::indicator:disabled { border-color: {bg.selected};
    background: {bg.control}; }
QCheckBox::indicator:checked:disabled { background: {bg.selected};
    border-color: {bg.selected}; }
QRadioButton { color: {ink}; spacing: 8px; background: transparent; }
QRadioButton:disabled { color: {ink.disabled}; }
QRadioButton::indicator { width: {ctl.indicator}px;
    height: {ctl.indicator}px; }

QGroupBox { color: {ink}; border: 1px solid {line}; border-radius: 8px;
    margin-top: 10px; padding-top: 10px; background: transparent; }
QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left;
    padding: 0 6px; color: {ink.muted}; }

/* ---- role frames ------------------------------------------------------ */
QFrame[role="chrome"] { background: {bg.chrome};
    border-bottom: 1px solid {line}; }
QFrame[role="rail"]   { background: {bg.raised};
    border-right: 1px solid {line}; }
QFrame[role="well"]   { background: {bg.deep};
    border: 1px solid {line.input}; border-radius: 8px; }
QFrame[role="bar"]    { background: {bg.raised};
    border-top: 1px solid {line}; }
QFrame[role="ribbon"] { background: {bg.base};
    border-bottom: 1px solid {line}; }
QFrame[role="tabhost"]{ background: {bg.raised};
    border-bottom: 1px solid {line}; }
QFrame[role="divider"] { background: {line}; }
QFrame[role="pill"] { background: {bg.control};
    border: 1px solid {line.input}; border-radius: 11px; }
QFrame[role="pill"] > QLabel { background: transparent; border: none; }
QFrame[role="noticebox"] { background: {notice.bg};
    border: 1px solid {notice.border}; border-radius: 9px; }

/* ---- menus ------------------------------------------------------------ */
/* The window's own menu bar sits on the chrome surface. When the merged title
   bar lands it is reparented into a QFrame[role="chrome"], which already
   paints that colour -- hence the second rule, which is dead today and correct
   the moment the bar exists. */
QMenuBar { background: {bg.chrome}; color: {ink.dim};
    border-bottom: 1px solid {line}; padding: 2px 4px; }
QFrame[role="chrome"] QMenuBar { background: transparent; border: none; }
QMenuBar::item { padding: 5px 10px; border-radius: 6px;
    color: {ink.dim}; background: transparent; }
QMenuBar::item:selected { background: {bg.selected}; color: {ink}; }
QMenu { background: {bg.chrome}; color: {ink}; border: 1px solid {line};
    border-radius: 8px; padding: 6px; }
QMenu::item { padding: 7px 14px; border-radius: 6px; }
QMenu::item:selected { background: {bg.selected}; }
QMenu::item:disabled { color: {ink.disabled}; }
QMenu::item:checked { color: {accent}; }
QMenu::separator { height: 1px; background: {line}; margin: 5px 10px; }

/* ---- tree ------------------------------------------------------------- */
QTreeView, QTreeWidget, QListView, QTableView, QTableWidget {
    background: {bg.raised}; color: {ink};
    border: none; outline: none; show-decoration-selected: 1; }
QTreeView::item, QTreeWidget::item { padding: 4px; border-radius: 6px; }
/* The table's rows, on the same terms. No ::item:selected rule here either --
   the palette's Highlight is what paints a selected row, and MainWindow sets
   it from bgSelected on the token path. A QSS rule would win over that and
   then disagree with the session tree, which has no such rule because the
   delegate paints it. */
QTableView::item { padding: 4px; }
QTableView::item:hover { background: {bg.hover}; }
QTreeView::item:hover, QTreeWidget::item:hover { background: {bg.hover}; }
/* No ::item:selected rule, and that is now correct rather than aspirational:
   the session delegate paints its own rounded pill and a styled square one
   underneath would show at the corners. It was here for one slice, while the
   delegate did not exist -- a QSS-styled QTreeView draws the item background
   from the sheet, so with no rule the row was highlighted for its branch
   column only. The delegate draws the whole row now, so the rule is gone. */
QHeaderView::section { background: {bg.chrome}; color: {ink.muted};
    border: none; border-bottom: 1px solid {line};
    padding: 7px 10px; }

/* ---- tabs ------------------------------------------------------------- */
QTabWidget::pane { background: {bg.base}; border: none;
    border-top: 1px solid {line}; }
QTabBar { background: transparent; qproperty-drawBase: 0; }
QTabBar::tab { background: transparent; color: {ink.dim}; padding: 8px 12px;
    border-top-left-radius: 7px; border-top-right-radius: 7px; }
QTabBar::tab:hover:!selected { background: {bg.hover}; color: {ink}; }
/* The selected tab has to match whatever is directly beneath the strip. Today
   that is the tab widget's pane at bg.base; if the ribbon is ever put between
   them this value follows the ribbon, not the terminal. It is a "must match
   the neighbour" value, not a constant. */
QTabBar::tab:selected { background: {bg.base}; color: {ink};
    border: 1px solid {line}; border-bottom-color: {bg.base}; }
/* Deliberately NO `image: none` here. The ported sheet carries that rule, and
   it does not merely hide the close button: with no image and no width the
   subcontrol collapses, so the button is unclickable as well as invisible.
   Leaving the subcontrol unstyled lets the style draw its own indicator. */
QTabBar::close-button:hover { background: {bg.selected}; border-radius: 4px; }

)QSS"
    R"QSS(/* ---- chrome furniture ------------------------------------------------- */
QStatusBar { background: {bg.raised}; color: {ink.muted};
    border-top: 1px solid {line}; }
QStatusBar::item { border: none; }
QStatusBar QLabel { color: {ink.muted}; }

QSplitter::handle { background: {line}; }
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical { height: 1px; }

QToolTip { background: {bg.chrome}; color: {ink};
    border: 1px solid {line}; border-radius: 6px; padding: 5px 9px; }

QSizeGrip { background: transparent; }

QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
QScrollBar::handle:vertical { background: {dead};
    border-radius: 5px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: {ink.faint}; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }
QScrollBar::handle:horizontal { background: {dead};
    border-radius: 5px; min-width: 30px; }
QScrollBar::handle:horizontal:hover { background: {ink.faint}; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QScrollArea { background: {bg.base}; border: none; }

/* ---- chips and label tones -------------------------------------------- */
QLabel[chip="true"] { background: {bg.control}; border-radius: 5px;
    padding: 3px 8px; font-family: {ff.mono}; font-size: {fs.mono}px;
    color: {ink.dim}; }
QLabel[chip="accent"] { background: {chip.accent}; border-radius: 5px;
    padding: 3px 8px; font-family: {ff.mono}; font-size: {fs.mono}px;
    color: {accent}; }
QLabel[chip="state"] { background: {chip.state}; border-radius: 5px;
    padding: 3px 8px; font-family: {ff.mono}; font-size: {fs.mono}px;
    color: {state}; }
QLabel[chip="warn"] { background: {chip.warn}; border-radius: 5px;
    padding: 3px 8px; font-family: {ff.mono}; font-size: {fs.mono}px;
    color: {warn}; }

QLabel[role="fieldlabel"], QLabel[role="desc"] {
    color: {ink.muted}; font-size: {fs.small}px; }
QLabel[role="wordmark"] { color: {ink}; font-size: {fs.base}px; font-weight: 500; }
QLabel[role="corner"] { color: {ink.muted}; font-size: {fs.small}px; }
QLabel[role="ribbonname"] { color: {ink}; font-size: {fs.base}px; font-weight: 500; }
QLabel[role="notice"] { color: {ink.dim};
    background: transparent; border: none; }
QLabel[mono="true"] { font-family: {ff.mono}; font-size: {fs.mono}px;
    color: {ink.muted}; }
QLabel[mono="true"][tone="ink"]    { color: {ink.dim}; }
QLabel[mono="true"][tone="accent"] { color: {accent}; }
QLabel[mono="true"][tone="state"]  { color: {state}; }
QLabel[mono="true"][tone="warn"]   { color: {warn}; }
QLabel[mono="true"][tone="hint"]   { color: {ink.faint}; font-size: {fs.tiny}px; }
QLabel[mono="true"][role="readout"] { font-size: {fs.base}px; }
QLabel[tone="accent"] { color: {accent}; }
QLabel[tone="state"]  { color: {state}; }
QLabel[tone="muted"]  { color: {ink.muted}; }
QLabel:disabled { color: {ink.disabled}; }

/* ---- dialogs ---------------------------------------------------------- */
QDialogButtonBox { button-layout: 3; }
QMessageBox { background: {bg.base}; }
QMessageBox QLabel { color: {ink}; }
)QSS";

}  // namespace

std::string generateTokenStylesheet(const Tokens &t, int baseFontPx) {
    std::string out = kTemplate;

    // The sheet used to carry 13/12/11/10 as literals, which is why the chrome
    // stayed the same size whatever QApplication::setFont() was told: a QSS
    // font-size beats the widget's font, so setting the application font moved
    // nothing that this sheet reaches.
    //
    // The four sizes keep their ORIGINAL RELATIONSHIP to each other rather
    // than being scaled by a ratio. A ratio rounds three of them onto the same
    // value somewhere around 10px, and the hierarchy -- body, label, chip,
    // hint -- is the whole reason there are four.
    const int base = std::max(8, baseFontPx);
    const auto px = [base](int delta) {
        return std::to_string(std::max(7, base + delta));
    };
    const std::string fsBase = px(0);
    const std::string fsSmall = px(-1);
    const std::string fsMono = px(-2);
    const std::string fsTiny = px(-3);

    // Checkbox and radio indicators, sized from the base font rather than
    // pinned. +3 lands on 16px at the default 13, which is exactly what the
    // literal used to be -- so the default rendering is unchanged and only the
    // ends of the 9-28 range move. Floored at 12 because below that the tick
    // inside a checked box stops being legible before the box stops being
    // visible.
    const std::string ctlIndicator =
        std::to_string(std::max(12, base + 3));

    // Longest first. `bg.base` is a prefix of nothing here, but `ink` is a
    // prefix of `ink.dim` and `accent` of `accent.hover` -- substituting the
    // short one first would leave "{#e6e9ec.dim}" behind.
    const std::vector<std::pair<std::string, std::string>> map = {
        {"ff.sans", kSansStack},
        {"ff.mono", kMonoStack},
        {"fs.small", fsSmall},
        {"fs.base", fsBase},
        {"fs.mono", fsMono},
        {"fs.tiny", fsTiny},
        {"ctl.indicator", ctlIndicator},
        {"bg.selected", hex(t.bgSelected)},
        {"bg.control", hex(t.bgControl)},
        {"bg.chrome", hex(t.bgChrome)},
        {"bg.raised", hex(t.bgRaised)},
        {"bg.hover", hex(t.bgHover)},
        {"bg.deep", hex(t.bgDeep)},
        {"bg.base", hex(t.bgBase)},
        {"line.input", hex(t.lineInput)},
        {"line", hex(t.line)},
        {"ink.disabled", hex(darkerBy(t.inkFaint, 125))},
        {"ink.detail", hex(t.inkDetail)},
        {"ink.muted", hex(t.inkMuted)},
        {"ink.faint", hex(t.inkFaint)},
        {"ink.dim", hex(t.inkDim)},
        {"ink", hex(t.ink)},
        // Lightened on a dark theme and darkened on a light one: a hover that
        // moves toward white is invisible on a light background, which is the
        // failure the old sheet's lighten-by-10% has on every light theme it
        // ships with.
        {"accent.hover",
         hex(t.dark ? lighterBy(t.accent, 115) : darkerBy(t.accent, 112))},
        {"accent", hex(t.accent)},
        {"on.accent", hex(t.onAccent)},
        {"chip.accent", tint(t.accent, t.bgBase, 0.16)},
        {"chip.state", tint(t.state, t.bgBase, 0.16)},
        {"chip.warn", tint(t.warn, t.bgBase, 0.16)},
        {"notice.border", tint(t.warn, t.bgBase, 0.34)},
        {"notice.bg", tint(t.warn, t.bgBase, 0.10)},
        {"state", hex(t.state)},
        {"warn", hex(t.warn)},
        {"danger", hex(t.danger)},
        {"dead", hex(t.dead)},
        {"term.bg", hex(t.termBg)},
        {"term.fg", hex(t.termFg)},
    };

    for (const auto &kv : map) substitute(&out, kv.first, kv.second);
    return out;
}

}  // namespace omega::theme
