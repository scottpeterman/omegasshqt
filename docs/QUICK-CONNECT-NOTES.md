# Quick connect and the terminal tab

The first path in the shell that reaches the far end. Pulled forward out of 4e
and Phase 5 because it needs no session store, no vault and no tree — so it
exercises the tab, the transport and the theme walk with nothing else in the
way.

## What is in this drop

New:

    app/terminaltab.h             app/terminaltab.cpp
    app/quickconnectdialog.h      app/quickconnectdialog.cpp
    docs/QUICK-CONNECT-NOTES.md

Modified:

    CMakeLists.txt              anytermqt discovery moved here; ordering
    app/CMakeLists.txt          omega_shell now links omegassh::qt + qtpyte
    app/mainwindow.h  .cpp      quickConnect, openTab, closeTab, theme walk
    examples/qt/CMakeLists.txt  its own anytermqt discovery removed
    scripts/build.sh  .bat      the skip note now names the app too

## The build change, which was overdue

anytermqt was discovered *inside* `examples/qt/CMakeLists.txt`, which
`add_subdirectory`d `qtpyte` there. Two things need it now — the example and
the application — and adding the same subdirectory twice is a duplicate-target
error, while leaving it in `examples/` meant the app broke under
`-DOMEGASSH_BUILD_EXAMPLES=OFF`. Discovery is at the top level now, sets
`OMEGASSH_HAVE_ANYTERMQT`, and `examples/qt` just checks the flag.

The consequence: **the app now needs anytermqt and the Go archive.** It was
worth building without either for as long as it could be — 4a's window had no
terminal in it — but a tab that dials needs both. Without a checkout, the app,
`terminal_window` and `theme_gallery` are skipped and everything else still
builds.

`add_subdirectory(qt)` also moved ahead of `add_subdirectory(app)`, since the
shell links `omegassh::qt`.

## The tab

`TerminalTab` owns the widget, the session, the overlay and the theme applied
to all three. It does **not** own deciding what to connect to: the config
arrives built, from the dialog now and from the session tree later, so the class
never learns the difference.

Three details that are choices rather than mechanics:

**The overlay goes up before the dial, not on the first state event.** `start()`
is asynchronous and returns with the window already painted, so a tab waiting
for the first event would flash empty on a fast connect and look hung on a slow
one.

**The overlay is centred, not full-bleed.** There is nothing to hide before a
session has written anything — but there is once a reconnect is in flight over a
screen full of output somebody wants to keep reading.

**A finished session does not close its tab.** A failed dial leaves the reason
on the overlay and an ended session leaves its scrollback; both are things
people read after the fact. The tab is retitled `(closed)` instead.

The selection alpha is read once, before any theme lands — reading it later
reads back whatever the last theme wrote and the translucency is lost on the
second switch.

## The dialog

SSH and telnet share one page and differ by which rows are enabled: a telnet
target is a host and a port, and so is an SSH one, so two pages would mean
typing the address twice to change your mind. The auth rows are *disabled* for
telnet rather than merely unused — the library refuses credential and jump
fields on telnet outright, because a jump host silently dropped puts the session
on the wire in plaintext across a link the operator believed was tunneled.

Serial is a port and a baud rate. 8N1 is not offered: every console port in the
building is 8N1, the library defaults to it, and three combo boxes nobody
changes are three chances to get it wrong. Enumeration failure and "no adapter
plugged in" are told apart in the combo text, which saves somebody checking
their cable.

The port number follows the transport, but only while it still holds the other
transport's default — somebody who typed 2222 meant it.

## The theme walk now has two passes

The stylesheet does not reach inside the terminal: anytermqt paints from a
palette, not from QSS. So `applyTheme()` walks the window list for chrome and
then the tab list for terminals. Two passes, one walk — not a second place that
themes, which is the failure this was built to avoid.

New tabs are themed **before** they are shown, since the overlay is on screen
from the first frame and a tab that corrected its palette a frame later would be
visible every single time.

## How it was verified, and the honest gap

The Qt side was built and run, and the state machine reached `Connected` with
the tab titled, the overlay hidden, the status bar updated, and a theme switch
repainting both surfaces — the terminal background moved from `#0c0c0c` to
`#282828` and the chrome from `#1e1e1e` to `#282828`, which is the two-pass walk
doing its job.

**It was run against a stub transport, not the real one.** This sandbox has no
Go toolchain and no module proxy, so `libomegassh.a` cannot be built here. A
fake archive implementing the 19 C entry points, backed by a pipe that echoes
what is typed, stood in for it. That stub is **not** part of this drop and
should not be added to the repo.

What that means for what is proven: everything above the C boundary — the tab,
`omegassh::attach`, the overlay, the theme walk, the dialog, the tab lifecycle —
ran. Everything below it did not. The first real dial from this window is still
unrun, and the things most likely to be wrong are the ones the stub had to guess
at: whether a config the dialog builds is one the Go side accepts, and whether
serial enumeration returns what the combo box expects.

Two stub bugs worth recording anyway, because both are real contract details:
handles are `> 0` (the wrapper treats `<= 0` as invalid), and the read end must
be non-blocking, since the wrapper drains until `omegassh_read` returns `<= 0`
and a blocking read hangs the GUI thread on the first empty poll.

## Still open

- `Ctrl+N` could not be delivered under bare Xvfb — no window manager, and Qt
  ignored the synthetic key. The dialog was exercised through `openTab()`
  directly. Worth a real click before trusting the shortcut.
- No connect overlay spinner and no Cancel button yet. `terminate()` on a
  session that is still dialing returns immediately, so the button would be
  honest whenever someone wants to add it.
- Tab context menu (close others, close to the right) and detached windows are
  still Phase 5.
