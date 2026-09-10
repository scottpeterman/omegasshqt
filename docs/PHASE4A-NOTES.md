# Phase 4a — settings and the shell skeleton

A window that remembers where it was, has every menu item present, and takes its
colours from the theme system. Nothing in it connects to anything.

## What is in this drop

New:

    app/settings.h                app/settings.cpp
    app/mainwindow.h              app/mainwindow.cpp
    app/main.cpp                  app/CMakeLists.txt
    tests/compat/settings_probe.cpp
    tests/compat/settings_differential.py
    tests/compat/shell_probe.cpp
    docs/PHASE4A-NOTES.md

Modified:

    CMakeLists.txt      OMEGASSH_BUILD_APP, add_subdirectory(app)
    theme/theme.h       Theme::fallback() is now enterprise_dark
    theme/theme.cpp     the same
    scripts/build.sh    the settings suite and the shell probe
    scripts/build.bat   the same, for Windows

## enterprise_dark is the default, in two places

`AppSettings::theme_name` defaults to it, and `omega::theme::Theme::fallback()`
is now the same theme rather than the Catppuccin-ish `Theme.default()`. The
first is what a fresh `config.json` says; the second is what renders if the
theme directory is missing entirely. Keeping them in step means there is one
answer to "what does Omega look like out of the box" rather than two that differ
only when something has already gone wrong.

`default.yaml` still ships and still registers under the name `default`. The
fallback is a compiled last resort, not that theme.

This is a **deliberate divergence** from config.py, which defaults `theme_name`
to `catppuccin_mocha` — a theme that exists in neither application's set, so
nterm-qt's out-of-the-box default resolves to nothing and falls back. Writing
`enterprise_dark` into a shared file is safe: it ships in both, so nterm-qt
reading a config Omega wrote resolves it too. The differential asserts the
divergence rather than tolerating it, so it cannot drift into an accident.

Note the fallback is generated from `theme/themes/enterprise_dark.yaml` and
nothing compares the two. If that file changes, change both.

## The settings file is shared, which decides three things

**Geometry is five plain integers, not a `QByteArray`.** `saveGeometry()` is the
idiomatic Qt answer and writes an opaque blob config.py can neither read nor
write. The ints are the interchange format. A window restored from them loses
the multi-monitor edge cases `QByteArray` handles, and that is the price of one
file.

**Writing is hand-rolled.** `QJsonDocument` sorts keys and indents with four
spaces; Python writes dataclass order at two, with no trailing newline and
non-ASCII escaped as `\uXXXX`. Emitting Python's exact bytes means two
applications alternating saves leave the file untouched instead of rewriting it
past each other — and it makes the output byte-comparable in the differential.
Verified from the other direction too: nterm-qt loading a file Omega wrote and
re-saving it produces identical bytes.

**Omega's own settings do not go here.** `from_dict` filters to known fields and
`asdict` writes only those, so any key Omega added would be deleted the next
time nterm-qt saved. An Omega-only setting belongs in a sidecar
(`~/.nterm/omega.json`). Nothing needs one yet, which is why there is no sidecar
type. Same shape of constraint as the theme format, which is harsher: there, an
unknown key costs the whole theme.

## Two divergences in behaviour, both deliberate

**Wrong-typed values are refused, not adopted.** `from_dict` does no validation,
so a `font_size` of `"large"` survives into the Python dataclass and fails
wherever the value is finally used. Omega keeps the default and records a
warning. The differential asserts both behaviours rather than pretending they
match.

**A top-level JSON array yields defaults.** config.py catches `JSONDecodeError`
and `TypeError`; a list reaches `.items()` and raises `AttributeError`, which is
not caught. Defaults are the better answer to a file nobody can read.

## Five of the fifteen fields are inert in nterm-qt

`tree_width`, `recent_profiles`, `max_recent`, `default_term_type` and
`default_keepalive_interval` are stored, defaulted, and read by nothing — the
splitter is hardcoded and the rest have no reader at all. Omega honouring them
cannot make the two disagree: writing a field the other application ignores is
safe in a way that adding a new one is not.

`default_keepalive_interval` is the one worth noting. It has no UI in either
application and nothing reads it, and it is the setting that matters for gear
behind a firewall — Phase 2's keepalive item is still open, and this is the knob
it will want.

Also worth knowing for 4e: `manager/settings.py` exposes only **five** of the
fifteen. The dialog is smaller than the settings are; the other ten are
app-maintained state, not preferences.

## How it is verified

`tests/compat/settings_differential.py`, same arrangement as the store and theme
suites: the C++ probe prints, config.py prints, and the comparison is a third
thing that reads both. Per fixture it checks the fifteen parsed values and the
bytes written back, and separately checks `add_recent_profile`'s move-to-front
and trim, the unreadable-file cases, the wrong-typed values, and the defaults.
19 checks, green.

Mutation-tested rather than trusted. Leaving non-ASCII unescaped fails one
check; a four-space indent fails seven; writing null geometry as `0` fails four.
An earlier round of mutations appeared to survive and turned out not to have
applied at all — worth doing the grep before believing a mutant.

`tests/compat/shell_probe.cpp` covers the one claim no differential can: that
geometry survives a close and comes back. It opens the window, places it, closes
it, reopens it and reports both what was saved and what was restored. It runs
under `-platform offscreen`, so no display and — more to the point — no window
manager. Closing a window from outside needs a WM to deliver
`WM_DELETE_WINDOW`; requiring one on the build machine to check something the
application can do to itself is the worse trade. Both suites run from
`scripts/build.sh`.

## What the shell deliberately is not

No tree model, no tabs, no session, no dial. Every menu action lands in
`notImplemented()`, which names the phase that owns it in the status bar — a
menu that looks finished cannot be mistaken for one that is.

The one thing built for later rather than for now is `applyTheme()`. It walks
the window, the tab area and the detached-window list from the first version,
while two of those three are still empty. That is the stale-pane bug in
nterm-qt: `_apply_theme` walks its tab widget only, keeps a list of detached
session windows, and never walks it, so a live theme switch leaves every
detached window on the old palette. One list, walked in one place, from the
start.

## Still open

- `applyThemeByName()` returns false when a config names a theme that no longer
  ships, and the caller currently falls back quietly. Once there is a settings
  dialog, that should probably be visible.
- The top-level `find_program(GO_EXECUTABLE go REQUIRED)` still runs
  unconditionally. `omega` links `omega::shell`, `omega::settings`,
  `omega::theme` and Qt — nothing from the Go archive — so the whole shell could
  be built and run on a machine with no Go toolchain if that requirement moved
  under the archive target.
- Nothing reads `scrollback_lines`, `multiline_paste_threshold` or
  `auto_reconnect` yet; they load, save, and wait for Phase 5.
