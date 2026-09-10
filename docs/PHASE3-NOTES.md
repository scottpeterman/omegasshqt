# Phase 3 — theme system

The theme types, the loader, the stylesheet generator and the gallery. Ported
from `ntermqt/theme/`, verified against it rather than against a reading of it.

## What is in this drop

New:

    theme/CMakeLists.txt
    theme/color.h                 theme/color.cpp
    theme/theme.h                 theme/theme.cpp
    theme/stylesheet.h            theme/stylesheet.cpp
    theme/themes/*.yaml           33 files
    integration/omegassh_theme_anytermqt.h
    examples/qt/theme_gallery.cpp
    tests/compat/theme_probe.cpp
    tests/compat/theme_differential.py
    docs/PHASE3-NOTES.md

Modified — these replace existing files rather than adding to them:

    CMakeLists.txt                OMEGASSH_BUILD_THEME, add_subdirectory(theme)
    examples/qt/CMakeLists.txt    the theme_gallery target
    scripts/build.sh              the theme differential, artifacts, gallery hint
    scripts/build.bat             the same, for Windows

Of the 33 theme files, 32 are nterm-qt's own, byte for byte. `clean.yaml` is
regenerated — see below.

## How it is verified

`tests/compat/theme_differential.py` runs both implementations over the same
files and compares. Same arrangement as the store's differential: neither side
asserts against its own idea of the answer, and the comparison is a third thing
that reads both. `theme_probe` prints and does not judge.

Per theme file: the loader's whole view of it, the generated QSS byte for byte,
and `lighten`/`darken` on its four chrome colours. Then two cases that are not
per-file — an empty document and one carrying an unknown top-level key, both of
which must be refused by *both* implementations.

233 checks across the 33 files, green.

Mutation-tested rather than trusted. Each guard was confirmed to fail when its
bug is introduced: `bg_darker` derived with `lighten` fails exactly the 32
stylesheet comparisons; rounding instead of truncating in `darken` fails 115
checks across derive and stylesheet; tolerating an unknown top-level key fails
exactly the one strictness case and nothing else.

The suite is gated on `NTERMQT_SRC`, like the store's, and skipped loudly. The
stylesheet is the one part of this with no runtime check on it at all — a wrong
derived colour renders, it just renders wrong — so a green build that skipped
this says nothing about whether the port still matches.

## Four things the source turned out to say

**`clean.yaml` is zero bytes.** `Theme.load` does `cls(**data)` on `None`,
throws, and `load_themes` swallows it as a warning. The theme still appears in
nterm-qt's list because `ThemeEngine.__init__` registers `Theme.clean()` as a
built-in *before* the YAML load runs. So "port the 33 files, drop the
classmethod bank" loses one theme. The file here is regenerated from
`Theme.clean()` and reloads to a dataclass identical to it; the bank is safe to
drop now.

**The classmethod bank is not dead code.** Twelve of those methods are
registered as built-ins and then overwritten by same-named YAML. Only the two
nested inside `enterprise_dark` are unreachable. Omega keeps one built-in,
`Theme::fallback()`, so `current()` resolves with an empty theme directory.

**The format is stricter than it looks.** `Theme(**data)` raises `TypeError` on
an unrecognised top-level key, and nterm-qt drops the whole theme with a
warning. Omega refuses the same files for the same reason: a theme that loads
in one application and vanishes in the other is the harder bug of the two. The
consequence is that Omega cannot add a key of its own to these files. Omega-only
theme data needs a separate file — the same call the settings work has to make
about `config.json`.

**Two documented keys have nowhere to go.** Every file carries `cursorAccent`
and `selectionForeground`. `qtpyte::Palette` holds 16 ANSI slots plus
foreground, background and cursor, and selection is one colour on the widget, so
neither key is applied — by either application. `Theme` carries them anyway,
because the files ship unchanged and a loader that quietly drops two documented
keys is a surprise later. `integration/omegassh_theme_anytermqt.h` is where it
stops.

## Two traps the gallery found

Both are things a real terminal tab will hit.

**Feed after layout, not in the constructor.** The widget derives its grid from
its size and its font, so a resize re-makes the screen and content written to a
zero-sized terminal is gone before anyone looks at it. Applying a theme changes
the font, which resizes the grid too — so the preview is refilled on every theme
change rather than once.

**Selection coordinates are absolute, not viewport-relative.** A widget that has
been resized has already pushed its initial blank screen into history, so line 1
is somewhere in the scrollback. The gallery anchors to `viewTopLine()`. Same
conversion a tab holding its own selection will need.

## The selection alpha

anytermqt paints the selection over the glyphs without swapping the foreground,
so a fully opaque `selectionBackground` erases the text under it and reads as a
broken font colour. Themes store it as solid hex, so the widget's own
translucent default is applied — but only when the theme's colour is opaque, so
a theme stating its own alpha still wins.

Read that default with `defaultSelectionAlpha()` **before** the first
`applyTheme` call. Reading it afterwards reads back the alpha that call wrote,
and the translucency is lost on the second switch.

## The gallery

    ./build/examples/qt/theme_gallery
    ./build/examples/qt/theme_gallery --theme dracula
    ./build/examples/qt/theme_gallery --shot shots     # one PNG per theme

The differential proves the two implementations agree. It cannot tell you that a
theme's border is invisible against its background, or that a foreground nobody
can read got shipped. Those are seen, not asserted — which is what `--shot` is
for: 33 themes reviewed in a batch rather than one screenshot at a time.

It also exercises the two behaviours the theme system has to get right before
any real widget is written: live switching with nothing restarting, and **one**
`applyTheme` path that reaches the main window, every terminal, every overlay
and the detached windows. nterm-qt's `_apply_theme` walks its tab widget only —
it keeps a list of detached session windows and never walks it, so a live theme
switch leaves them on the old palette. The Detach button exists to make that
failure visible if it is ever reintroduced: open one, switch theme, both windows
must move.

## Contrast, for whoever picks a default

A scan of all 33 files: chrome and terminal foreground-on-background contrast
ranges from 7.9:1 to 21:1, so nothing is unreadable at the level that matters.
Twenty-one individual ANSI slots are effectively invisible against their own
background, though — `brightWhite` on the light themes mostly, where `amiga` and
`light` are `#ffffff` on `#ffffff` and `clean` is 1.05:1, plus `pctools` blue at
1.20:1 and `1-2-3` blue at 1.31:1.

Theme content, not the port, and not obviously worth fixing in files shared with
another application. Recorded because it is the sort of thing that gets reported
as a rendering bug.

## One thing to know if you extend the gallery

`QMenu::addAction()`'s convenience overloads changed argument order between the
versions this repo supports: Qt 6.2 takes `(text, slot, shortcut)`, Qt 6.4 takes
`(text, shortcut, slot)` and deprecates the older spelling. Either one compiles
on one machine and fails on the other, and `qt/CMakeLists.txt` sets the floor at
6.2 — which is what Ubuntu 22.04 ships. The gallery creates the action and calls
`setShortcut()` on it instead, which has meant the same thing since Qt 4.

The other Qt symbols the new files use are all Qt 5 era or earlier:
`QFont::setFamilies` (5.13) is the newest of them.

## Still open

- `applyTheme()` reaching a real window, tab list and detached-window list —
  Phase 4b, once 4a's skeleton exists. The gallery's walker is the shape.
- `AppSettings.theme_name` defaults to `catppuccin_mocha` and no such theme
  exists in the shipped set, in either application. Omega should write a name
  that resolves; until it does, every existing `config.json` names this one.
- The top-level `find_program(GO_EXECUTABLE go REQUIRED)` runs unconditionally,
  so a fresh clone cannot configure without a Go toolchain even for targets that
  never touch the archive. `theme_gallery` links `omega::theme`, `Qt6::Widgets`
  and `qtpyte::core` and nothing else — moving that requirement under the
  archive target would buy a fast loop for all of Phase 3 and 4a.
