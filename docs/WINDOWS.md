# What has actually been run

A record of what was exercised and where, so nobody — including future me —
has to guess which parts are proven and which are merely written. Nothing in
the "not verified" list is known broken; it is untested, which is a different
and more dangerous thing.

Last updated: 2026-08-30 — adds the tab context menu and multi-tab close, the
reconnect action, the help and About windows, and packaging on all three
platforms: a macOS `.app`, a relocatable Linux tarball, a Windows folder and an
MSIX, each run on the platform it targets — and the MSIX installed and run on a
clean Windows laptop with no Qt or toolchain, vault and saved session included,
with `~/.nterm` confirmed unredirected under package identity. Earlier the same day: the per-session attribute columns, the
three-layer resolution in `app/effectiveconfig.h`, and the session editor
rebuilt around them, with telnet and a jump host confirmed from saved sessions
on real hosts. Previously, 2026-08-29: the terminal tab (Phase 5): the context menu,
session capture, paste confirmation and rate-limited paste, Tab reaching the
far end, and the host-key prompt, with the two probes that cover them. Earlier
the same day: macOS as the third platform that builds and runs, the session
manager and editor (Phase 4d/4e), TerminalTelemetry YAML import and export, and
the credential-management dialogs. Previously: the theme system (Phase 3), the
settings layer and shell skeleton (Phase 4a), quick connect, the credential
vault (Phase 0), the telnet and serial transports (Phase 2), the asynchronous
dial (Phase 2b), and the anytermqt example covering all three transports.

Four of the sections below are *differential* rather than assertive: they run
Omega's implementation and nterm-qt's over the same input and compare, so a
pass means the two applications agree, not that either matches somebody's idea
of correct. Those suites need `NTERMQT_SRC` pointed at an nterm-qt checkout and
are skipped loudly without it. The YAML one additionally needs PyYAML, and
loads only `manager/models.py` and `manager/io.py` by path with the Qt names
stubbed, so it does not need PySide6.

## Verified on Linux (x86-64)

**Go core** — `go vet ./...` clean, `gofmt` clean. 48 unit tests over the
output buffer, the known_hosts filter, secret resolution, the vault, and the
vault-to-sshcore adapter. No network, no fixtures.

`go test -race ./...` is clean with one exception, recorded here rather than
left to be rediscovered: `TestOutputBufferConcurrentDrain` fails
intermittently. It is a bug in the test, not in the buffer — the drain loop
checks `n == 0 && b.Done()`, and between the drain returning zero and `Done`
returning true the pump can append a chunk and *then* set eof, so the goroutine
exits leaving bytes behind. It fails more often on a loaded machine.

A real race in `OutputBuffer` was found and fixed at the same time, and it was
not a test bug: `notify` read `closed` under the lock, released it, and only
then called `wake()`, so a wake could land after `Close` returned — poking a
descriptor the consumer had already torn down. `Close` now takes the same
`wakeMu` the callback is made under, so on return no wake is running and none
will start. The window is narrow enough that it would not reproduce in 700
runs on one core; widening it artificially made it deterministic.

**C API** (`examples/c/`, against a live sshd)

| | |
|---|---|
| Interactive shell on a pty | pass |
| Requested geometry reaches the remote tty (`stty size`) | pass |
| `WindowChange` resize propagates | pass |
| Escape sequences survive the channel | pass |
| Remote exit surfaces through the notify handle | pass |
| `close` idempotent | pass |
| Strict policy refuses an unknown host | pass |
| TOFU accepts first contact and pins the key | pass |
| Strict then succeeds against the pinned key | pass |
| Key **mismatch** fails closed even under TOFU | pass |
| Malformed known_hosts line tolerated, warned, not silent | pass |
| Jump-host dial and round-trip through a bastion | pass |
| Legacy algorithm tail negotiates on a modern server | pass |
| Two concurrent sessions stay isolated | pass |

**Credential vault** (`examples/c/vault_smoke`, 72 assertions, no server needed)

| | |
|---|---|
| Create, unlock, lock, re-open; wrong master refused | pass |
| Weak master refused; create over an existing vault refused | pass |
| Store, list, fetch, rename, delete, default, enable/disable | pass |
| Duplicate and empty names refused; malformed JSON refused | pass |
| No secret appears in any metadata payload; `has_secret` reports presence | pass |
| Rename preserves a secret the caller never received | pass |
| Reads and writes refused while locked | pass |
| Re-key on a never-unlocked handle keeps every credential | pass |
| Keyring status reports reachability without returning the secret | pass |
| Quiet unlock: keyring, then environment, then needs-password | pass |
| Quiet unlock never reports a wrong password for an untyped one | pass |
| Stale keyring entry after a re-key reports STALE, not WRONG_PASSWORD | pass |
| Re-filing the entry restores quiet unlock; clear is idempotent | pass |

The last four ran against a real Secret Service (GNOME Keyring over D-Bus).
The stale-entry block is opt-in — `OMEGASSH_VAULT_SMOKE_KEYRING=1` — because it
is the only part that writes to a live login keyring.

`ChangeMasterPassword` destroyed the vault before this was written: it verified
the old master against the file, then sealed `v.creds` under the new key, and
on a handle that had never been unlocked that slice is nil. Every credential
gone, no error returned, and the old master no longer opens the file. That is
the exact shape a C++ caller has — construct handle, call function — which is
why it was found here and not in the CLI.

**Credential reference** (`examples/c/credential_probe`, 20 assertions, no server needed)

Every case below is decided before a socket opens, so the whole join is
verifiable with nothing listening.

| | |
|---|---|
| Credential named with no vault handle — refused, and says so | pass |
| Unknown vault handle — refused | pass |
| Jump credential with no jump host — refused | pass |
| Unknown credential name — refused, naming the credential | pass |
| Disabled credential — refused as *disabled*, not as *not found* | pass |
| Dial against a locked vault — refused as *locked*, not as auth failure | pass |
| Manual credentials reach the network without consulting a vault | pass |

The last row is the one that matters: quick connect with a typed password must
fail on DNS, not on anything mentioning a credential. The invariant this
establishes is that **stored** secrets never cross the C boundary — a secret
the operator just typed obviously does, and that path stays first-class.

**Qt wrapper** (`examples/qt/qt_smoke`) — two runs. A live session: connect,
command round-trip through `dataReceived`, resize reaching the remote tty,
`finished(int)` after exit, teardown, and every signal asserted to arrive on
the Qt thread. Then a dial that cannot succeed, against a black hole
(10.255.255.1, two-second timeout) rather than a refused port, because a
refusal returns too fast to prove anything: `start()` returns true, the state
is *connecting* on return, a heartbeat timer keeps firing throughout — which
is the evidence the caller's thread was never held — and the run ends in
`Failed`, `errorOccurred` with the reason, and `finished(-1)`.

The live run is now driven off `stateChanged(Connected)` rather than a timer
long enough to cover a dial. A fixed wait is a guess that holds on loopback
and stops holding on a satellite link.

**Exit status** — `exit 42` reports 42; `kill -9 $$` reports 137 (128+9);
clean exit reports 0.

**anytermqt integration** (`examples/qt/terminal_window`) — a live SSH session
rendering in a `qtpyte::TerminalWidget`. Confirmed against a remote Ubuntu
22.04 host over a real network path: login banner with colour, `htop -t` on a
24-core box with tree mode and the F-key bar pinned to the last row across the
full width, and interactive resize reflowing correctly.

Qt versions used: 6.2.4 (Ubuntu 22.04, apt) and 6.4.2. The CMake floor is 6.2.

## Verified on Windows (x86-64, MSVC 2022)

2026-08-28. Windows 11 (build 26200), VS 2022 Build Tools 14.43, Go 1.27,
mingw-w64 gcc 13 for cgo, Qt 6.10.3 `msvc2022_64`, vendored SQLite 3.53.4.

**Build** — `scripts\build.bat` completes: `go vet ./...` and `go test ./...`
clean, the vendored SQLite amalgamation, `omega_sessions` and `compat_probe`,
the Go c-archive, `omegassh_qt`, the anytermqt integration (`pyte_core`,
`qtpyte_core`, `utf8proc`), and all seven example binaries.

**The Go runtime under MSVC** — this is the part that was in doubt, and it is
the reason for [WINDOWS.md](WINDOWS.md). A c-archive built by mingw and linked
by MSVC needs four things the toolchains do not agree on, three of which fail
with no diagnostic at the point of failure. All four are now handled in the
tree: `msvc/runtime_init.c` (`.CRT$XCU` vs `.ctors`, golang/go#42190),
`scripts/pdatafix` (`.pdata` ordering, LNK1223),
`-D__USE_MINGW_ANSI_STDIO=0`, and `legacy_stdio_definitions.lib`.

**Vault** — `vault_smoke.exe` passes its full surface: handle lifecycle,
create and re-key, store and read-back with no plaintext in the listing,
rename, default selection, delete, lock and re-open, wrong-master rejection,
and credentials surviving a master-password change. The keyring backend is
reached and reports status through `wincred`; writing an entry is opt-in and
was not exercised.

**Serial enumeration** — `terminal_window.exe serial` lists a real COM port
through `serialx.ListDetailed`, which is the Go runtime, a package with real
initialisation, JSON across the C boundary and `omegassh_free` back again.

**SSH end to end** — a live session to a lab host in `terminal_window.exe`:
dial, authentication, the asynchronous state changes clearing the connecting
overlay, the loopback socket-pair notifier in `capi/notify_windows.go` waking
Qt through `QSocketNotifier`, and output rendering in a
`qtpyte::TerminalWidget`. That notifier had never executed on any machine
before this run.

**File permissions** — `internal/privatefile` passes. Windows has no mode
bits, so the vault and session logs get an explicit owner-only DACL; the tests
assert privacy in the terms each platform actually uses rather than comparing
`Mode().Perm()`, which on Windows reports `0666` for any writable file.

## Verified on macOS

2026-08-29. The third platform, brought up in one sitting, and the two things
in the way were both toolchain seams rather than logic.

Architecture, OS version and Qt version are not recorded here yet: this section
is written from the build succeeding and from watching the application run, not
from a `scripts/build.sh` transcript. Fill the header in when one exists, the
way the Linux and Windows sections above are written.

**Build** — the tree configures and builds, and the application runs.

**No `pipe2(2)` on darwin.** `capi/notify_posix.go` was one `//go:build
!windows` file using `syscall.Pipe2`, which does not exist on darwin at all —
so the wakeup primitive could not compile there, and nothing downstream of it
could either. Now split into `capi/notify_linux.go` and
`capi/notify_darwin.go`.

The darwin file holds `syscall.ForkLock` for read across `Pipe` and both
`CloseOnExec` calls, because those are two syscalls here rather than one and a
fork in any other goroutine between them inherits both descriptors. That race
is reachable in this program, not theoretical: the vault's macOS keyring
backend shells out to `/usr/bin/security`, and a leaked *write* end means the
read end never reports EOF when ours is closed — a hang, not a leak. This is
what `os.Pipe` does on the platforms without `pipe2`, and the reason the Linux
file can skip it.

**`-std=c99` on a C++ file, fatal under AppleClang.** utf8proc arrives with
qtpyte as a C library that puts `-std=c99` on its own sources. In *this* build
it also acquired AUTOMOC, so a C++ `mocs_compilation.cpp` was generated inside
a C target and the C flag landed on a C++ file. GCC treats that as a warning
and carries on; AppleClang makes it an error — which is why Linux built it for
weeks and the first Mac did not. It does not happen when anytermqt builds
itself, because that build has no `utf8proc_autogen` at all: the top-level
CMakeLists adds `qtpyte/` directly rather than anytermqt's own top-level file,
so utf8proc is created in a different scope and inherits a different AUTOMOC
setting. Fixed by turning AUTOMOC, AUTOUIC and AUTORCC off on the target —
nothing in a C library needs moc — with no `if(TARGET)` guard, deliberately, so
a rename fails loudly at configure time rather than quietly ceasing to protect
anything.

**SSH observed running, by hand.** The session tree loaded from a populated
`~/.nterm/sessions.db` with nested folders; the filter matched across folder
boundaries, including a folder matched on its own name showing its contents;
several tabs open at once against a real host, with a live shell and closed
tabs retained in the tab bar.

**Serial end to end, including a hot unplug.** A session on
`/dev/cu.usbserial-1130` at 9600 8N1 into a Cisco 2911 console port: `show
ver`, the `--More--` pager, and the banner art rendering. Then the adapter was
pulled mid-session, which ended the session without hanging the tab or the
application.

That one run covers more than it looks like. `go.bug.st/serial`'s detailed
enumerator is cgo on darwin, so it exercises the `-framework IOKit` link line
that had never been linked on a Mac; it is the first time any platform other
than Linux has opened a serial port at all; and the unplug drives the teardown
path in `capi/notify_darwin.go` under a real failure rather than a clean close,
which for a file written the same day is the case worth having.

**All three transports from the application's own quick connect.** SSH, telnet
and serial each opened through Omega's dialog rather than through
`terminal_window` or a C probe, which makes macOS the first platform where the
dialog's telnet and serial config paths have run at all — they are code the SSH
route never touches, and the serial form's port combo is filled by the real
enumerator here rather than by a stub.

**Not yet run on macOS:** `scripts/build.sh` and its suites. Everything in this
section is a build result or a hand observation. The probes — including the two
Phase 5 ones — the differentials and the keyring backend are all still
unexercised on darwin; see Not verified.

## Transports (Phase 2)

**Folded-in suites, unmodified.** `telnetx` and `serialx` came from
PathfinderSSH coupled to that project's `internal/term` by three symbols.
Those are redeclared in `transport/`, the import line changed, and nothing
else did — so the existing suites are the evidence the move broke nothing.
Both pass under `-race`: 14 telnet, 21 serial. The serial suite is
hardware-free, through the programmable fake port that came with it.

**The session layer** (`transport/session_test.go`) is new code and was
mutation-tested rather than trusted. Two of its tests passed against a
deliberately broken build on the first attempt and were rewritten: the
local-close test used a fake that returned a clean EOF where a real teardown
returns "use of closed network connection", and the repeat-state test drove a
backend that only ever reaches its terminal state once, so it tested the
idempotence of `finish` instead of the guard in `setState`. Each guard is now
confirmed to fail when its bug is introduced:

| Guard removed | Test that fails |
|---|---|
| local-close verdict (`b.closing`) | `TestLocalCloseSuppressesTheTeardownError` |
| clean-EOF verdict | `TestSSHSessionReadsAndReaps` |
| repeat-state suppression | `TestStateChangeIsNotRepublished` |
| zero-`Size` drop before resize | `TestSSHSessionDropsInvalidResize` |
| backend `Done` bridge in `Wrap` | `TestBackendSessionEndsWithoutAReadLoop` |

**C surface** (`examples/c/transport_probe`, against `tests/faketelnetd.py`)

| | |
|---|---|
| `omegassh_serial_ports` returns a JSON array | pass |
| Unknown transport is named, not silently defaulted | pass |
| Telnet with no host, serial with no port, bad parity — all refused | pass |
| Telnet refuses a jump host rather than ignoring it | pass |
| Telnet refuses a vault credential | pass |
| An unopenable log path fails the dial | pass |
| Telnet dial, `omegassh_transport` reports telnet | pass |
| `omegassh_state` is *connecting* the moment open returns | pass |
| The dial then reaches connected | pass |
| `omegassh_resize` reaches the wire as NAWS (132x40, confirmed server-side) | pass |
| Configured terminal type answers a TTYPE SEND (confirmed server-side) | pass |
| A doubled `0xFF` arrives upward as one byte | pass |
| An IAC sequence split across two TCP segments parses | pass |
| `omegassh_write` reports the logical, not on-wire, byte count | pass |
| Telnet reports no exit status (-1, not 0) | pass |
| The session log is written and has content | pass |

All seven refusals are decided before a socket opens.

**Serial** (`transport_probe --serial`, against a socat pty pair driven by
`tests/fakeconsole.py`)

| | |
|---|---|
| `omegassh_open(serial)` | pass |
| `omegassh_transport` reports serial | pass |
| The port opens and reports connected | pass |
| Serial publishes neither connecting nor authenticating — asserted over the event queue, not assumed | pass |
| Summary names the device and the mode (`/dev/x 9600 8N1`) | pass |
| `omegassh_resize` succeeds and does nothing | pass |
| A bare CR reaches the far end unexpanded (unlike telnet) | pass |
| Close unblocks a blocked read — timed, 0ms | pass |
| Serial reports no exit status (-1, not 0) | pass |
| The session log is created | pass |

The close-unblocks-a-blocked-read check is the one worth having on real
hardware. `Read` is called without the mutex held precisely so a blocked read
cannot deadlock `Close`, and on an idle line the read loop *is* blocked at
that moment. A regression there hangs rather than failing, so the probe times
it.

A pty is not an adapter. What this does not answer is whether a given driver
enumerates or a real cable behaves, which is why the probe reports whether the
device it was handed appeared in the enumerator's own list.

**Enumeration against real hardware** has run: a PL2303 USB-serial bridge
(`067b:2303`) and a CDC-ACM device, both listed with vendor and product IDs.
The PL2303 reports no serial number — normal for that chip, and worth knowing
before a quick-connect form tries to tell two identical cables apart by one.

## anytermqt integration (all three transports)

`examples/qt/terminal_window` is one binary with one transport-aware function
in it. All three were run headless under Xvfb, driven with `xdotool type`, so
the keyboard path is exercised as well as the render path.

| | |
|---|---|
| SSH against `labsshd.sh` — coloured prompt, `stty size` reports 48 155 matching the widget grid exactly, `TERM=xterm-256color` | pass |
| Telnet against `faketelnetd.py` — banner, a literal `0xFF` delivered as one byte, typed lines echoed, live prompt | pass |
| Serial against `fakeconsole.py` — ANSI colour, device-side echo, bare-CR line endings, live prompt | pass |
| Serial against **real gear** — a Cisco 2911 console port over a PL2303 cable, ROMMON output rendering | pass |
| **Hot unplug of the USB adapter mid-session** — the session ends, the error surfaces, the window stays usable; no hung session reproducible | pass |
| Window title comes from `summary()` in all three (`127.0.0.1:2323`, `/tmp/omega-console 9600 8N1`) | pass |
| Status bar tracks `stateChanged` | pass |
| A connection overlay stacked over the widget shows *Connecting…* against an unreachable host, then *Failed* with the reason in the status bar and a dialog | pass |

Two things running it turned up that reading it would not have:

**The example showed two dialogs for one failed dial.** `errorOccurred` was
connected before `start()`, so a failed connect fired a modal warning *inside*
`start()`, blocking the thread mid-call, and was then followed by the critical
box on the return value. Fixed by connecting that handler after `start()`.
The bug predates this phase; it was latent until something actually failed.

That fix then sprang a leak of its own when the dial became asynchronous, and
running it is again what found it. `start()` drains whatever the dial has
already produced before it returns, and a refused connection on loopback fails
*inside* that window — so the error was emitted before the handler existed, and
the window sat there saying "Failed" with no reason anywhere on screen. The
handler is connected before `start()` again, and the critical box on the false
return is gone instead: one handler, one dialog, whether the refusal is
configuration (synchronous) or the far end's answer (later, on the event loop).
Confirmed by pointing it at a closed port and watching the box appear.

**The adapter can be pulled out.** A PL2303 cable into a Cisco 2911 console
port, unplugged with the session live: `serialx`'s read fails, `base.finish`
settles the verdict, and it arrives as an error and a terminal state through
the notifier like any other end. The window stays responsive and the
scrollback is intact. This is the case the unit suite covers with a
programmable fake port and a pty cannot cover at all — a pty cannot be
unplugged — so it was on the not-verified list until now.

Worth recording from the same run, because it is a Phase 5 decision and not a
bug: the status bar still read "Connected" behind the modal. The error event
and the state change are drained in one pass, and `QMessageBox::warning`
blocks inside the `errorOccurred` emit, so the `Failed` transition is still
queued behind the dialog until it is dismissed. And once dismissed, the
overlay covers the widget — which hides the console scrollback an operator
would actually want to read after a cable falls out. A failure *after* a
session has connected should not blank the screen the way a failure *before*
one should; the full-screen overlay belongs to the never-connected case, and a
banner to the rest. The example does the naive thing ("not connected → cover
the widget") on purpose, being an example.

**Three NAWS pushes on one window opening** — 155x50, then 157x48, then
155x48, as the layout settled and the scrollbar appeared, the first of them
wrong. On telnet that is three subnegotiations; on SSH it would be three
channel requests. Harmless against anything that has been met so far, but a
resize debounce belongs in the widget or in Phase 5 rather than in the
transport, and it is recorded here so it is not rediscovered as a mystery.

## The asynchronous dial (Phase 2b)

`omegassh_open` no longer waits for the far end. It validates, files a handle
and returns; the dial runs on a goroutine and publishes its progress as state
changes. The line it draws is between what the caller can fix and what the
network decides, and both halves are checked.

| | |
|---|---|
| SSH publishes connecting → authenticating → connected (`shell_smoke`, asserted over the event queue) | pass |
| Telnet publishes connecting, then connected | pass |
| Serial publishes neither, and goes disconnected → connected | pass |
| A configuration mistake is still refused synchronously, as -1 plus `omegassh_last_error` | pass |
| An unknown or disabled credential name is still refused synchronously | pass |
| A failed dial arrives as `EventError` then `StateFailed`, with the reason in `omegassh_error` | pass |
| `omegassh_alive` reports 1 while dialing, 0 for a dial that failed | pass |
| `omegassh_transport` and `omegassh_summary` answer before the session exists | pass |
| `omegassh_write` is refused while connecting rather than buffered | pass |
| A resize during the dial is remembered and applied when the session lands | pass |
| `omegassh_close` mid-dial returns promptly — 20 closes, half mid-handshake and half against a black hole, slowest 0ms | pass |
| The Qt event loop keeps running throughout a dial (`qt_smoke` heartbeat) | pass |

**`omegassh_error` is new**, and it exists because the dial fails on a thread
the caller has never run on. `omegassh_last_error` is per-thread by design —
that was the fix for two Qt workers overwriting each other — so it cannot carry
a failure that happened on a goroutine of the library's. The per-session
message is kept so a caller polling `omegassh_state` need not have been
draining events at the moment it happened.

**What the probes cost.** The refusal that used to be a `-1` from
`omegassh_open` is now, for anything past configuration, a state to wait for.
`examples/c/probe_connect.h` holds that wait in one place rather than five
variants of it, and it polls `omegassh_state` rather than watching the
notifier: one wake covers bytes *and* events, so draining it to learn the state
would consume the wake the banner assertion two lines later is waiting on.

Writing that helper produced the one real bug of the change, and it was in the
helper. It treated `DISCONNECTED` as terminal, which is wrong for serial:
disconnected is where a serial handle *starts*, because opening the port is the
whole handshake and there is nothing to report until it is open. The wait
declared the session dead a microsecond before it came up, and the write that
followed then raced the dial and was refused. `omegassh_alive` is the authority
on whether a session is over; the state is not, and the two are different
questions.

## Theme system (Phase 3)

`tests/compat/theme_differential.py`, against nterm-qt's `theme/engine.py` and
`theme/stylesheet.py`. **233 checks, all passing**, over all 33 shipped themes.

| | |
|---|---|
| Every field the two loaders parse out of each file | pass, 33 files |
| Generated Qt stylesheet, **byte for byte** against `generate_stylesheet` | pass, 33 files |
| `lighten` / `darken` on each theme's four chrome colours | pass, 132 checks |
| An empty document is refused by both | pass |
| An unknown top-level key is refused by both | pass |

Mutation-tested rather than trusted. Deriving `bg_darker` with `lighten`
instead of `darken` fails exactly the 32 stylesheet comparisons; rounding
instead of truncating in `darken` fails 115 checks across derive and
stylesheet; tolerating an unknown top-level key fails exactly the one
strictness case.

An earlier round of mutations appeared to survive and had in fact never
applied — the sed targets did not exist in the file being mutated. A surviving
mutant is only evidence once the mutation is confirmed to have landed.

**Rendered and looked at**, which the differential cannot do: all 33 themes
through `examples/qt/theme_gallery --shot`, one PNG each, covering the tree,
tabs, menus, a live terminal with an ANSI chart, the overlay and the full
control strip. Live theme switching and the detached-window walk were exercised
by hand. The two keys with nowhere to go — `cursorAccent` and
`selectionForeground` — are carried by the loader and applied by nothing, in
both applications.

Twenty-one ANSI slots across the set are effectively invisible against their
own background (`brightWhite` on the light themes; `amiga` and `light` are
`#ffffff` on `#ffffff`). Theme content, not a port defect, recorded because it
gets reported as a rendering bug.

## Settings (Phase 4a)

`tests/compat/settings_differential.py`, against `ntermqt/config.py`. **19
checks, all passing**, over a generated corpus — full, partial, empty, null
geometry, a recent list at its cap, unknown keys, non-ASCII, and compact
unindented input.

| | |
|---|---|
| All fifteen parsed values, per fixture | pass |
| The bytes written back, **byte for byte** against `json.dumps(indent=2)` | pass |
| `addRecentProfile` move-to-front and `max_recent` trim | pass |
| Truncated, non-JSON and top-level-array files fall back to defaults | pass |
| Wrong-typed values refused and defaulted (a documented divergence) | pass |
| Defaults match except `theme_name`, which diverges deliberately | pass |

Confirmed from the other direction too: nterm-qt loading a `config.json` Omega
wrote and re-saving it produces **identical bytes**, so alternating saves do not
rewrite the file past each other.

Mutation-tested: leaving non-ASCII unescaped fails 1 check, a four-space indent
fails 7, writing null geometry as `0` fails 4.

Two deliberate divergences, asserted rather than tolerated. `theme_name`
defaults to `enterprise_dark` (config.py's `catppuccin_mocha` names a theme
that exists in neither application's set). And a wrong-typed value is refused
and the default kept, where `from_dict` does no validation and lets a
`font_size` of `"large"` through to fail later.

## Shell and quick connect

**Geometry** — `tests/compat/shell_probe.cpp`, offscreen. Opens the window,
places it at 900×600 +137+91 with a 320px tree, closes it, reopens, and reports
both what was saved and what was restored. Both correct. Deliberately not
driven with xdotool: closing a window from outside needs a window manager to
deliver `WM_DELETE_WINDOW`, and requiring one on the build machine to check
something the application does to itself is the worse trade.

**Quick connect against real gear** — a live SSH session opened from the
dialog, in a tab, on **Linux and Windows both**, with `htop` rendering
full-screen and redrawing on resize. That last part is the only place a wrong
NAWS is visible, so it is the check that `omegassh::attach`'s third connection
is wired.

**The theme walk reaches the terminal**, not just the chrome. Switching from
`enterprise_dark` to `gruvbox_dark` moved the terminal background from
`#0c0c0c` to `#282828` and the chrome from `#1e1e1e` to `#282828` — two passes
in one walk, since the stylesheet does not reach inside a widget that paints
from a palette.

Before real hardware was available, the Qt side above the C boundary was run
against a **stub archive** implementing the 19 C entry points over a pipe. It
is not in the repo and should not be added. Two contract details it surfaced
are worth keeping: session handles must be `> 0`, since the Qt wrapper treats
`<= 0` as invalid; and the notify read end must be non-blocking, since the
wrapper drains until `omegassh_read` returns `<= 0` and a blocking read hangs
the GUI thread on the first empty poll.

## Credential management (the C++ wrapper and the dialogs)

**The wrapper** — `tests/compat/vault_probe.cpp`, 39 checks against a
throwaway vault file. Handle lifecycle and the handle being usable as
`Config::vaultHandle`; create, unlock, and a wrong master reported as
`WRONG_PASSWORD` with detail attached; store, metadata by name, and the auth
label coming from Go rather than being reconstructed in C++; editing metadata
**keeping the secret**, and a replace with no material clearing it as
documented; rename keeping the secret; set-default, `defaultName`, and a
disabled default no longer being offered; and the refusals — unknown id, empty
name, and every call against a locked vault.

The layering is the point of having this as well as `vault_smoke`: if
`vault_smoke` passes and `vault_probe` fails, the marshalling in
`qt/omegasshvault.cpp` is where to look, not the shim.

Note the probe wants a **fresh vault file**. Re-run against one it has already
populated and "scope survived" fails, because the credential it expects to
create is already there. That is the probe's own limitation, not a finding.

**The dialogs** — the vault unlock dialog and the credential manager have been
driven by hand on Linux and Windows: a real `~/.nterm/vault.json` listed with
its default credential, method, and last-used timestamp, and the add, edit,
rename, disable, clear-default and delete actions reachable from it.

**The name-only boundary holds in the UI.** The session editor's credential
picker is populated from `Vault::list()`, which returns metadata; there is no
call in the vault surface that returns material, so the form has no way to
obtain a secret even by mistake. A saved session stores
`credential_name`, and `MainWindow::openSession` puts the name and the vault
handle into `Config` together — a name with no handle is refused on the Go side
before a socket opens, which `credential_probe` asserts.

Startup does not block on a password: `quietUnlock` tries the keyring, then the
environment, and reports to the status bar. A locked vault is a usable state,
and the prompt happens when the vault is first needed.

## Session manager (Phase 4d) and the session editor (4e)

**The model, headless** — `tests/compat/session_tree_probe.cpp`, 43 checks
under the offscreen platform. Tree shape and ordering; reordering inside a
folder; moving between folders and back out to root; the refusals (a folder
into its own subtree, anything onto a session); `folders.expanded` surviving a
reopen; the filter; edit, duplicate and delete-folder-reparents.

A drop is a model call — `dropMimeData` with a mime payload, a row and a parent
— so what a drag WRITES is checkable with no display, no mouse and no window
manager. The ordering check is deliberately made twice: once against the model
that performed the drop, and once against a model built fresh from the file,
because only the second one proves the order came out of the database.

**Real drags, under Xvfb and xdotool.** The model calls above are a different
code path from a mouse, and this is the first thing in the tree that needed a
real one. Driven with an actual pointer: appending onto a folder, inserting
between two rows, reparenting a folder that has children, the refusal of a
folder onto its own child, and a drag attempt with the filter active — which
must write nothing, and does not. The application survived all of them, which
also settles the open question about resetting the model from inside the view's
drop handling: it is safe, and needs no deferral.

**Against nterm-qt, on one file, both directions.** Omega wrote a tree with
positions and expansion state; `ntermqt/manager/models.py` opened the same file
and read every folder, session, position and `extras` blob intact, then added a
folder and a session of its own, which Omega read back. This is also where the
ordering divergence is visible: nterm-qt lists a folder's sessions by name
while their `position` column reads 1, 2, 0.

**The ordering divergence.** `get_tree` returns sessions `ORDER BY name`, so
nterm-qt writes `sessions.position` on every drop and never reads it — drag a
session within a folder, reload, and it snaps back to alphabetical. Folders do
not have this problem; they are listed `ORDER BY position, name`. Omega sorts
both by position then name. Nothing about that is incompatible: nterm-qt keeps
ignoring the column exactly as it did.

**The filter divergence.** `_apply_filter` decides a folder's visibility only
from its children, so a folder whose *name* matches but whose sessions do not
comes back empty. Omega's proxy accepts on self-match, descendant-match or
ancestor-match, so a matched folder shows its contents.

**On real gear.** The tree loaded from a populated `~/.nterm/sessions.db`,
opened live SSH sessions in tabs from the context menu on **Linux and Windows
both**, and folders were created through the GUI on Windows.

## Per-session attributes and the resolution

**The columns and the resolution, headless** — `tests/compat/session_attrs_probe.cpp`,
65 checks in three groups, with no display, no vault and no network.

*Migration onto a pre-existing database.* A database is written by hand with
raw sqlite3 carrying the twelve original columns and nothing else — the shape a
real nterm-qt leaves behind — then opened through `SessionStore`, which adds
the attribute columns. Every existing row survives with its `extras` blob
intact.

*Round trip of every attribute.* Each field is written with a distinct value
and read back. This one earns its keep: `kSessionCols` and `readSession()`
state the column order twice, and a column added to one and not the other reads
its neighbour's value — which for `anti_idle_seconds` beside
`anti_idle_keystroke` is a wrong number rather than a crash.

*Three-layer resolution.* The whole inheritance matrix, because the resolver is
a pure function of three structs and a handle. Including the cases that are
easy to get backwards: a session switching anti-idle OFF against a global that
has it on; overriding the interval alone and leaving `enabled` still following
the global; and a paste rate of zero resolving as unpaced rather than as
absence.

**The editor's round trip** — `tests/compat/session_editor_probe.cpp`, 47
checks under the offscreen platform. The weight is on absence: a session that
overrides nothing, opened in the form and saved again, must come back with
every column still NULL. A form full of spin boxes and combo boxes has a
default position for every widget, and the failure being guarded against is a
row nobody touched saving the value its widget happened to be showing — which
turns "follows the global" into "pinned to whatever the global was that day"
and looks correct until the global changes months later.

The transport cases are the other half: saving as telnet or serial must clear
the credential, host key policy and jump host rather than leave columns behind
that the resolver will never read.

This probe found a real bug rather than confirming one. An anti-idle keystroke
spelling the build does not recognise was being silently erased on save; it is
now kept and shown as `(not recognised)`, the same treatment a credential the
vault no longer holds already had.

**The form, offscreen.** All three tabs grabbed under `-platform offscreen`,
which is what caught the serial port label acquiring a second `(not present)`
suffix on a refresh — a consequence of reading the combo's display text back
instead of its item data, and invisible by reading.

**On real gear (Linux).** A saved SSH session and a saved *telnet* session to
the same lab host, both dialed from the tree, in tabs side by side — the thing
the schema concession used to prevent. And a saved session through a **jump
host**, with a key for the hop and a password for the target: two credentials
resolved independently in one dial, confirmed by `who` on the far end
reporting the login as arriving from the bastion's address rather than the
laptop's. nterm-qt had no bastion concept at all, so that path is new rather
than ported.

## TerminalTelemetry YAML (import and export)

**The formats, headless** — `tests/compat/ttyaml_probe.cpp`, 56 checks.
Parsing, quoting, comments and embedded hashes; the refusals, each asserted
against the line number it reports; emitting and round-tripping; the import
rules; export, including nesting flattened to a joined path and loose sessions
under `Ungrouped`; and export → import → export being stable.

**Against PyYAML and `manager/io.py`** —
`tests/compat/ttyaml_differential.py`, 26 checks. `sessions/ttyaml.cpp` is a
restricted subset reader: it is allowed to REFUSE what PyYAML accepts, and
never to accept it and read it differently. Every sample where both succeed
agrees field for field. The importer is compared by running both against the
same file into two fresh databases and diffing the rows.

Four samples are skipped, each for a stated reason. One is PyYAML's type
resolution, which a text-only reader cannot and should not reproduce. The other
three are **two nterm-qt bugs the differential found**:

- A non-numeric port raises `ValueError` out of `import_terminal_telemetry`
  entirely — after earlier rows have been committed, leaving a half-imported
  tree and no summary. Omega falls back to 22 for that row and carries on.
- A duplicate host inside one file is silently dropped but counted.
  `existing_sessions[hostname]` stores the object the importer built rather
  than one carrying the id `add_session` returned, so the tracked row has
  `id=None`; the next entry with that host then runs `update_session` with
  `WHERE id IS NULL`, which matches nothing. Confirmed by running it:
  `import_terminal_telemetry` returns `(2, 2, 0)` and leaves one row. Omega
  writes the id back, so the last entry wins and the count is true.

**Through the GUI.** Import driven end to end with a real pointer — File menu,
file dialog, the duplicate-policy prompt, the summary, and the tree refreshing
— landing three sessions across two folders with the right ports, descriptions
and `extras`.

## Terminal tab (Phase 5)

Two probes here rather than a differential: there is no second implementation
to compare against, so these judge. Both run offscreen — no display, no window
manager, no mouse — and both are Linux-only so far.

**`app/terminal_probe`**, 57 checks across eight groups. It takes `focus`,
`capture`, `paste`, `hostkey`, `antiidle`, `scroll`, `tabclose` or `reconnect`,
or runs all eight with no argument.

- *Focus.* Tab and Shift+Tab reach the emulator instead of the focus chain.
  `QWidget::event()` hands both to `focusNextPrevChild()` before
  `keyPressEvent()` ever runs, so in a window with anything else focusable in
  it the remote saw nothing. Offscreen is sufficient because the interception
  being tested is Qt's, not the server's. The group carries a **control case**:
  a bare `qtpyte::TerminalWidget` is asserted to *lose* focus, so if anytermqt
  ever fixes this upstream that line is what says so.
- *Capture.* Escape stripping survives a chunk boundary — a sequence split
  across two reads is invisible to a per-chunk regex and lands in the log as
  garbage, which is why `CaptureWriter` is a state machine. UTF-8 split across
  reads is checked the same way.
- *Paste.* Chunking on lines, the 256-character cap on a pathological long
  line, byte fidelity over a mixed block, the baud delay arithmetic (10 bits
  per character, 8N1), unlimited meaning no delay, and the duration estimate
  the confirmation dialog shows.
- *Host key.* The marker published by `omegassh_unknown_hostkey_marker()` is
  non-empty and current against the fixture; first contact parses as unknown
  with the fingerprint intact; and a mismatch, an unrelated failure and a
  malformed error each **refuse** to parse as first contact. That last group is
  the point of the whole thing — parsing a mismatch as first contact would
  offer to trust exactly what the check refused.

The Go half of the same claim is `sshcore/hostkey_message_test.go`, which
drives the real callback for each case, so the wording is asserted from both
sides of the boundary rather than copied to one of them.

**`app/hostkey_flow_probe`**, 9 checks against the lab sshd, on a throwaway
known_hosts per run:

- *accept* — first contact prompts, accepting re-dials and **connects**, and
  one entry is left behind.
- *reject* — first contact prompts, and rejecting leaves no entry and no
  session.
- *mismatch* — a host pinned to a different key does not prompt, does not
  connect, and the file is untouched.

The accept case is here because it broke once in a way no unit test could see:
the failed session still held its handle, so the re-dial came back "session is
already running" and the prompt led nowhere. That is a sequencing bug between
two live objects, and the only thing that catches it is doing the sequence.
`scripts/build.sh` skips this one loudly when nothing is listening on the lab
port — `tests/.lab/` outlives a stopped server, so gating on the files alone
runs it against nothing and reports "no prompt", which reads like a bug in the
prompt rather than an absent server.

**By hand, on Linux.** The context menu against a live session — copy, paste,
copy & paste, the Paste Speed submenu, capture start and stop, select all,
clear. The policy has to go on the scroll area rather than on `viewport()`:
QAbstractScrollArea's filter consumes viewport events ahead of
`QWidget::event()`, so set on the viewport it compiles and produces no menu.
The paste confirmation was driven with a real multi-line block, with the baud
selection and the duration estimate on screen — `screenshots/` has both.

- *Anti-idle.* The keystroke bytes for each of the four choices, the hex
  round trip refusing an odd or non-hex string rather than truncating it, the
  config file round trip, and every condition that suppresses a send —
  not connected, alternate screen, mid-paste, disabled, zero interval, and a
  Custom with nothing in it.
- *Scrollback.* The emulator honours a limit the tab hands it and trims
  history to it. Also pins anytermqt's own default at 5000, which is neither
  pyte's 1000 nor config.json's 10000 — the gap that made wiring this up worth
  doing, and an upstream change to it should surface here rather than as tabs
  quietly holding a different amount of history.
- *Tab close.* A multi-tab close asks only when a session in the set is still
  live, names the live ones, and caps the list at eight with the remainder
  counted rather than dropped.
- *Reconnect.* The guard (a tab that never dialed refuses; a dial in flight
  withdraws the offer), a re-dial refused before a socket opens leaving the tab
  text saying "(closed)", and a re-dial that reached the network, failed on a
  name lookup, and started again in the same tab.

**Not covered here:** the overlay's Cancel button is exercised by hand only,
and neither probe has run on Windows or macOS. The Windows caveat is sharper
than it looks — as of 2026-08-30 that machine's anytermqt checkout predated
`alternateScreen()`, so it had never compiled anti-idle at all, let alone run
the probe group that covers it.

## Tab context menu, reconnect and the help system

2026-08-30, Linux for the probes and macOS by hand.

**Tab bar context menu.** Close Tab, Close Other Tabs, Close Tabs to the Right,
Close All Tabs, plus Reconnect on a tab whose session has ended. The warning
fires only on the multi-tab routes: single close is the tab bar's own button
and Ctrl+W, which have never asked, and a confirmation on the common case is a
confirmation nobody reads. `closeTabSet()` collects widgets before closing any
of them, because `removeTab()` renumbers the strip and a loop over indexes
closes the wrong tabs from the second one on.

**Confirmed by hand on macOS** with a genuinely live set behind it — the
warning appeared, named the connected sessions, and Cancel left everything
open.

**Reconnect** dials the same config in the same tab, keeping the previous
session's scrollback underneath. The prompt-once host-key guard is reset on an
explicit reconnect: that guard exists so the *automatic* re-dial cannot loop
inside one attempt, not to make an unknown key unanswerable for the tab's life.

**Not covered:** reconnect against a session that died on its own — an
exec-timeout, a device reload, a bastion dropping the connection. The probe
only covers a dial that failed at DNS.

**Help menu.** F1 opens a topic window; deep links per topic; About. The topic
bodies were written against the code rather than from memory — the session
fields are the rows `sessioneditordialog.cpp` actually builds, and the file list
is the paths named in `settings.cpp`, `omegasettings.cpp`, `mainwindow.cpp` and
`omegasshsession.h`. Nothing makes them follow along when those change.

**The About link colour was caught by an offscreen grab and by nothing else.**
Qt renders an anchor in `QPalette::Link`, but a widget carrying a stylesheet
resolves its palette from that stylesheet and ignores one set on the widget —
and the theme QSS says nothing about anchors, so the link came out Qt's default
dark blue on a dark background, very nearly invisible. Neither the HTML nor the
stylesheet mentions links, so reading either one says nothing is wrong. The
accent colour now travels into the markup.

## Packaging, all three platforms

2026-08-30. Each script produces something a person on another machine can run,
and each was run on the platform it targets.

### macOS

`scripts/bundle-mac.sh` produces `Omega.app`, and it opens.

**What that one screenshot actually proves**, since each of these is a separate
step that could have failed silently:

- the qrc resource initialised, or the About dialog would have had a blank
  space where the banner is;
- the bundle-relative theme lookup won over the compiled path, or the window
  would have opened in the built-in fallback rather than `enterprise_dark`;
- `iconutil` ran *before* CMake configure, or `MACOSX_BUNDLE_ICON_FILE` would
  have pointed at nothing and the Dock would show a generic icon.

**A Qt resource in a static library does not self-register.** `omega_shell` is
static, so `rcc` generated the object, nothing referenced it, the linker dropped
it, and every `QPixmap(":/omega/...")` came back null — a blank label, not an
error. `initOmegaResources()` exists for that.

**Not verified: a Mac with no Qt and no toolchain.** `otool -L` reports every
Qt reference as `@executable_path`, which is good evidence and is not the same
thing. Only one Mac is available, so this needs a borrowed machine or a VM. It
is the last thing standing between this and something distributable.

**The project folder was renamed to `-freeze` and the bundle still opened
themed.** A better test than it looks, and free: the compiled theme path points
into the build tree by absolute path, so renaming the folder breaks it. The
window came up in `enterprise_dark` rather than the built-in fallback, which
means the bundle read its own `Contents/Resources/themes`. That is the search
order proven by breaking the alternative rather than by reading the code.

It does **not** prove Qt independence. `/opt/homebrew` is still on the machine,
so a framework macdeployqt failed to rewrite would be quietly satisfied by the
host copy. The script checks `otool -L` on `Contents/MacOS/Omega` only; the
frameworks and plugins have their own dependencies and are where rewriting can
come up short. A fuller check, which has not been run:

```bash
find build-mac/app/Omega.app -type f \( -perm +111 -o -name '*.dylib' \) \
  -exec otool -L {} + | grep -i qt | grep -v '@executable_path\|@loader_path\|@rpath'
```

**Qt provenance is unsettled here.** Successive configures on the same Mac found
6.13 (Homebrew), then 6.11.1, then 6.10.3 — `CMAKE_PREFIX_PATH` is not pinned,
so CMake picks. macdeployqt has to come from the Qt the binary linked against,
and a moving target is how a bundle ends up working locally and failing
elsewhere. Pin it before the clean-machine test, or that test measures the
wrong thing.

**Dragged into `/Applications` and run from there.** Together with the rename
above, that is as far as a single Mac goes: the bundle moves off the build tree
and still works. What it cannot show is Qt independence, since `/opt/homebrew`
is on that machine and would quietly satisfy any framework macdeployqt failed
to rewrite. Hiding the Homebrew keg
(`sudo mv /opt/homebrew/Cellar/qt /opt/homebrew/Cellar/qt-hidden`) would settle
it without a second machine; it has not been done.

**Not verified:** notarization. Ad-hoc signing only.

### Linux

`scripts/bundle-linux.sh` ran end to end on a real desktop (XPS 13), producing
`dist/omega-0.1.0-linux-x86_64.tar.gz` at 30 MB: 11 libraries, 33 themes, every
Qt library resolving inside the bundle, and the staged tree starting and finding
its own themes.

Eleven libraries where the sandbox staged thirteen — different distribution,
different ICU soname and Qt dependency set. Both pass the same check, which is
the point of checking rather than counting.

The sandbox run additionally relocated the tarball to a different path, emptied
the copy's `Resources/themes`, and confirmed the warning named
`/tmp/relocate/omega/Resources/themes` — proving the moved copy reads its own
themes rather than falling through to the build tree.

**Not verified:** a GUI launch from the *relocated* tarball. The sandbox could
only run offscreen, so the xcb platform plugin has never driven a display from a
staged tarball.

### Linux: AppImage

`scripts/bundle-appimage.sh` produces `dist/Omega-0.1.0-x86_64.AppImage`, 32 MB,
containing 33 themes and the xcb platform plugin.

**Run under Xvfb with the build tree's `theme/themes` renamed away**, and it
started and stayed running with nothing to report. That is the theme lookup
proven by removing the alternative: the compiled path is absolute into the build
tree, so with it gone the only remaining source is the AppImage's own
`usr/Resources/themes`. Unlike the tarball, this exercised the **xcb** plugin
against a real X display rather than offscreen.

`linuxdeploy` bundles `xcb` and not `offscreen`, which is correct for a desktop
application and means the headless smoke test the tarball script runs is not
available here.

**Built and run on a real desktop** (XPS 13), not only under Xvfb. The
`linuxdeploy` output shows the Qt dependencies being pulled into the AppDir,
which is the step the tarball script does by hand with `ldd` and a copy loop.

**Not verified:** the AppImage on a machine that did not build it, and whether a
desktop environment picks up the embedded `.desktop` entry and icon from a
double-click.

### Windows

`scripts\bundle-windows.bat` produces `dist\omega\`, and it runs. Qt 6.10.3,
matching Linux.

**Three things this cost, all now fixed and all worth remembering.**

*The exe was console-subsystem.* Windows allocated a console before the GUI and
left it behind the application. A Developer Command Prompt hides this completely
— a console is already there — so it surfaced only once the thing was packaged
and launched from the Start menu. `WIN32_EXECUTABLE` on the `omega` target, and
`dumpbin /headers ... | findstr /i subsystem` is how you check: `Windows GUI`,
not `Windows CUI`.

*Version directories do not sort as text.* The script's "newest Qt" guess used
`dir /o-n`, which puts `6.8.3` above `6.10.3`, and it deployed 6.8.3 DLLs beside
a binary linked against 6.10.3. It runs on the build machine, where the real Qt
is on PATH, and would fail anywhere else. Now sorted with PowerShell's
`[version]` cast.

*A cached `Qt6_DIR` beats `-DCMAKE_PREFIX_PATH`.* The same run had a build
directory configured against a different Qt, and `find_package` kept using the
cached one without a word. CMake offers no way to un-cache that, so the script
wipes the directory on a mismatch and re-checks after `windeployqt`. That last
check is what catches it: a `Qt6Core.dll` from the wrong Qt is present,
correctly named, and wrong.

**The anytermqt checkout was behind**, which cost a build before any of the
above. anti-idle calls `qtpyte::TerminalWidget::alternateScreen()`; MSVC reports
its absence as `'alternateScreen': is not a member of
'omega::app::TerminalView'`, naming the derived class in this repo for a method
that lives in the base class in the other one. The script now greps
`terminalwidget.h` up front. Worth noting what it implies: the Windows box had
never built anti-idle, so anything this file claims about Windows from before
2026-08-30 was tested against a tree predating that feature.

### Windows: MSIX

`scripts\bundle-windows-msix.bat --sign-self` packs, signs and installs. Tile
assets come from `art/msix/`, generated by `art/make-icons.py`.

Two error codes met and understood:

- `0x800B0109`, root certificate not trusted — the script had been generating a
  *new* self-signed certificate every run, so the `.cer` already imported no
  longer matched. It reuses `dist/omega-test.pfx` now.
- `0x80073CFB`, same identity and different contents — MSIX will not replace a
  package at the same version. `Add-AppxPackage -Register dist\msix\AppxManifest.xml`
  against the unpacked layout skips packing, signing and the version check, and
  is the right test loop.

**Installed and run on a clean Windows laptop** — no Qt, no Visual Studio, no
toolchain of any kind, a machine that had never built this. The MSIX installed,
the application ran, a vault credential unlocked and a saved session connected
over SSH.

That is the strongest version of the clean-machine test and it covers more than
packaging. It also exercises first run on a bare profile: `~\.nterm` created
from nothing, the `sessions.db` schema built, and `wincred` reached with no
prior entry — none of which had been tested anywhere before.

What it proves about packaging specifically: `windeployqt` rewrote nothing
wrong, the platform plugin loaded from inside the package, `Resources\themes`
was found beside the executable rather than through the compiled build-tree
path, and the qrc resources initialised from a static library on a machine that
never compiled them.

**`~\.nterm` is NOT redirected under MSIX.** The question that decided whether
MSIX was viable for this application at all, and the answer is that it is: the
directory was deleted, the packaged build recreated it, and it came back at the
real `%USERPROFILE%\.nterm` rather than under
`%LOCALAPPDATA%\Packages\ScottPeterman.Omega*\LocalCache\`.

That is worth more than a no-redirection result on an existing directory,
because it covers creation as well as writes: a packaged process with package
identity makes the folder in the right place from nothing.

The shared-directory design therefore survives packaging. A packaged Omega and
an installed nterm-qt read the same `config.json` and the same `sessions.db`,
which is what the arrangement was for. `%USERPROFILE%` is not `%LOCALAPPDATA%`
and the redirection does not reach it. Omega shares
that directory with nterm-qt by design. If those writes get virtualized into the
package's private store, both applications keep working and silently stop seeing
each other's sessions — worse than a crash, because nothing fails. Save a
session from the packaged build and check `%USERPROFILE%\.nterm\sessions.db`
moved. The vault needs the same check: its keyring backend is `wincred`, and
package identity can change what a process sees in Credential Manager.

**Also not verified:** Store submission, or any signing other than self-signed.

## Not verified

- **A packaged build on a clean machine — done for Windows/MSIX only.** See the
  MSIX section: a laptop with no Qt and no toolchain installed and ran it, vault
  and saved session included. The macOS `.app` and the Linux tarball have still
  only run on the machines that produced them, where the host Qt would quietly
  satisfy anything the packaging failed to bundle.
- **A GUI launch from the relocated Linux tarball.** The relocation was tested
  offscreen; the xcb platform plugin has never driven a display from a staged
  tree.
- **Qt pinned on macOS.** Three successive configures found three different Qt
  versions. macdeployqt must match what the binary linked against, so this
  wants settling before the clean-machine test rather than after.
- **Notarization**, and any signing beyond ad-hoc on macOS or self-signed on
  Windows. Gatekeeper will refuse the bundle on anybody else's Mac; MSIX will
  not install at all without a trusted certificate.
- **Reconnect against a session the far end dropped** — an exec-timeout, a
  device reload, a bastion closing the connection. The probe covers a dial that
  failed at DNS and a re-dial refused before a socket opened; neither is a live
  session going away underneath you.
- **Serial as a saved session type.** The column, the resolver branch and the
  editor's round trip are all covered by the two probes, but no serial session
  has been saved and dialed. The editor's serial page has only ever been
  exercised against its `(not present)` branch, because the sandbox enumerates
  no ports — the path where enumeration *finds* the device and the saved name
  matches an item has not been run anywhere.
- **The migration against a database a real nterm-qt wrote.** The probe builds
  a legacy-schema database by hand, which is the right shape and the wrong
  provenance. Worth doing once against a store the Python actually created,
  with a backup taken first, since `ALTER TABLE` on somebody's real session
  list is the one step here that is not reversible by deleting a file.
- **Switching a saved session's transport on real data.** Saving as telnet or
  serial clears the credential, host key policy and jump host by design. The
  probe asserts it; nobody has yet done it to a session they cared about.
- **`QComboBox` and `QSpinBox` under the dark themes.** Both render without a
  visible dropdown indicator or spin arrows, so they read as line edits. Known
  before this work — they are the first widgets of those classes in the shell —
  but the session editor is mostly those two classes, which raises it from a
  blemish to something that misreads: a picker whose one-of-the-choices is
  "inherit" looks like a field you type into. Placeholder dimming in the dark
  themes wants checking at the same time, since every inherit-by-placeholder
  row depends on it.
- **Telnet and serial through the quick-connect dialog on Linux and Windows.**
  Both transports are exercised there through the C boundary and through
  `terminal_window`, but the dialog builds their configs on code paths the SSH
  run never touched. macOS has now opened all three from the dialog — see the
  macOS section — which is what retired the older, broader version of this
  entry, but that says nothing about the other two: serial on Windows is the
  interesting one, since enumeration there is a different path again.
- **`config.json` line endings on Windows.** Python's `Path.write_text` opens
  in text mode, so nterm-qt writing this file on Windows produces CRLF. Omega's
  writer emits LF and `QSaveFile` is opened without `QIODevice::Text`. Both
  applications read either without complaint, so nothing breaks — but every
  alternating save would rewrite all sixteen lines, which is exactly what the
  hand-rolled byte-identical writer exists to prevent. The differential runs
  where both sides write LF and will therefore never notice. Unconfirmed: check
  what a Windows nterm-qt actually wrote before changing anything.
- **Keyboard shortcuts under a bare X server.** With no window manager the
  application never takes keyboard focus, so `Ctrl+N`, `Ctrl+I` and `Ctrl+E`
  were all driven through the File menu with the mouse in the automated runs.
  Each has since been used by hand on both platforms. This is a property of the
  harness, not of the application, and it is worth knowing before writing
  another one.
- **A theme file that names a theme no longer shipped.** `applyThemeByName`
  returns false and the caller falls back quietly. The path is written and has
  not been provoked.
- **Windows telnet, and opening a serial port.** Enumeration works — see the
  Windows section above — but no port has been opened there, and telnet has
  not been tried at all. `transport_probe.exe` builds and has not been run to
  completion, which means the notifier teardown path in
  `capi/notify_windows.go` is still only half exercised.
- **Writing a Windows keyring entry.** `wincred` is reached and reports
  status, but the smoke test's write paths are opt-in and were skipped.
- **The suites on macOS.** The tree builds there and the application runs, but
  `scripts/build.sh` has not been run on a Mac, so no probe and no differential
  has executed on darwin. Nothing in the macOS section above rests on an
  automated check.
- **The macOS keyring backend.** It shells out to `/usr/bin/security` and has
  still never been executed. The application starts with the vault locked and
  `quietUnlock` reporting to the status bar, which is a state that does not
  require it.
- **The BSDs.** `capi/notify_linux.go` takes an implicit `GOOS=linux`
  constraint from its filename that a build tag cannot widen, so freebsd,
  openbsd and netbsd now have no notifier — they had one under the old
  `!windows` file, and `syscall.Pipe2` exists on all of them. Confirmed by
  building the two files against six platforms: linux and darwin compile, the
  three BSDs fail with `undefined: newNotifier`. Not a target today; the fix if
  it becomes one is a rename to `notify_pipe2.go` with `//go:build !windows &&
  !darwin`, with no change to the contents.
- **Agent auth.** The POSIX path is written but has not been run against a
  live agent. Windows is deliberately unimplemented (named pipe, needs
  `go-winio`).
- **Real network gear.** Everything above ran against OpenSSH. The legacy
  algorithm tail has not been tested against hardware that actually requires
  it, which is the case it exists for.
- **Keyboard-interactive** against a real MFA or RADIUS prompt. `Config.AuthPrompt`
  exists and is reachable from Go, but there is no way for a C caller to supply
  it — that needs a state machine over the notifier rather than a callback, and
  is not written.
- **A successful dial using a vault credential.** `credential_probe` covers
  every refusal, all of which short-circuit before the network. The path where
  resolution succeeds and the session actually opens has only been exercised
  with manual credentials.
- **Agent and public-key credentials through the vault.** The adapter maps them
  and is unit-tested, but no dial has used one.
- **Long sessions.** Nothing has run for more than a few minutes. Keepalive
  and reconnect are not implemented, and `StateReconnecting` is consequently
  a state nothing publishes.
- **Serial on Windows.** Linux and macOS are both covered end to end now —
  enumeration against real USB hardware, a session against a real console cable
  into a Cisco 2911, and a hot unplug mid-session on each. Windows enumerates
  ports but has never opened one.
- **Telnet against real gear.** Everything above ran against
  `tests/faketelnetd.py`, which is a fixture shaped to provoke the parts the
  client has to get right. A console server, a reverse-telnet console port on
  GNS3 or dynamips, and a switch with no SSH stack are all still untried from
  here — though this is the same code PathfinderSSH ships and runs against
  that gear daily.
- **Interactive authentication.** `EventInteractionRequired` is declared,
  crosses the boundary, and reaches Qt as `interactionRequired`. Nothing emits
  it. The blocker is gone — the dial can park in `StateAuthenticating` now, and
  that state *is* observed on SSH — but the answer has no way back yet: that
  needs one more call alongside `omegassh_write`, with the prompt wired to
  `Config.AuthPrompt`.
- **Cancelling a dial in flight.** `omegassh_close` retires the handle at once
  and the dial tidies up behind it, which is verified — and the tab's overlay
  now has a Cancel button on top of it, shown only while a dial is actually in
  flight, exercised by hand and not by a probe. What is *not* verified,
  because it is not implemented, is interrupting the dial itself: `sshcore.Dial`
  has a timeout and no context, so a socket and a log file can outlive their
  handle by up to `timeout_seconds`. Invisible to a caller, and it is why
  closing a tab against unreachable gear is instant.

## Reproducing

Two of the examples need no server at all, and are the quickest check that a
build is sound:

```bash
# the four compatibility suites, which need an nterm-qt checkout
export NTERMQT_SRC=/path/to/nterm-qt
python3 tests/compat/differential.py          --probe build/sessions/compat_probe
python3 tests/compat/theme_differential.py    --probe build/theme/theme_probe \
                                              --themes theme/themes --ntermqt "$NTERMQT_SRC"
python3 tests/compat/settings_differential.py --probe build/app/settings_probe \
                                              --ntermqt "$NTERMQT_SRC"
python3 tests/compat/ttyaml_differential.py   --probe build/sessions/ttyaml_probe \
                                              --ntermqt "$NTERMQT_SRC"   # needs PyYAML

# the self-checks -- none of these needs a display, a WM or a mouse
QT_QPA_PLATFORM=offscreen ./build/app/shell_probe theme/themes /tmp/omega.json \
    900 600 137 91 320
QT_QPA_PLATFORM=offscreen ./build/app/session_tree_probe /tmp/tree.db
QT_QPA_PLATFORM=offscreen ./build/app/terminal_probe      # or: focus|capture|paste|hostkey
./build/sessions/ttyaml_probe /tmp/ttyaml.db
./build/qt/vault_probe /tmp/fresh-vault.json      # wants a file that does not exist yet

# the themes as pictures
Xvfb :99 & DISPLAY=:99 ./build/examples/qt/theme_gallery --shot shots
```

`scripts/build.sh` runs all of the above except the gallery, so the usual way
to reproduce this document is to set `NTERMQT_SRC` and build.

```bash
./build/examples/c/vault_smoke
./build/examples/c/credential_probe
./build/examples/c/transport_probe        # enumeration and refusals only

# the keyring paths, opt-in — writes to your real login keyring
OMEGASSH_VAULT_SMOKE_KEYRING=1 ./build/examples/c/vault_smoke

# the environment fallback must be set before the process starts: Go snapshots
# the environment at runtime init, so a setenv() from C is invisible to it
OMEGASSH_VAULT_PASSWORD=labmaster02 ./build/examples/c/vault_smoke
```

The telnet leg needs a listener, but not a real one and not root:

```bash
python3 tests/faketelnetd.py &                    # 127.0.0.1:2323
./build/examples/c/transport_probe 127.0.0.1 2323
```

The serial leg needs a device. A real adapter is the point of it, but a socat
pty pair will exercise everything above the driver:

```bash
socat -d -d pty,raw,echo=0,link=/tmp/omega-console \
            pty,raw,echo=0,link=/tmp/omega-device &
python3 tests/fakeconsole.py /tmp/omega-device &
./build/examples/c/transport_probe --serial /tmp/omega-console 9600
```

The rest need sshd:

```bash
sudo ./tests/labsshd.sh start     # prints the commands with paths filled in
```

`policy_suite` needs an empty known_hosts to start and leaves entries behind,
so truncate between runs:

```bash
: > /tmp/kh_lab
```

The host key flow probe wants the same server, and makes its own throwaway
known_hosts per case, so it needs no truncation:

```bash
QT_QPA_PLATFORM=offscreen ./build/app/hostkey_flow_probe \
    tests/.lab/clientkey tests/.lab/decoy.hostline 127.0.0.1 2222 labuser
```

`scripts/build.sh` runs it too, but only when something is actually listening
on the lab port; `OMEGASSH_LAB_PORT` overrides 2222.