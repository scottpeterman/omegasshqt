# Phase 2 — transports, logging, and the anytermqt examples

Drop-in tree for `github.com/scottpeterman/omegassh`, laid out at repo-relative
paths. Unzip over a clean checkout of `main` (d99442e).

`screenshots/` is evidence, not part of the tree — delete it or keep it, it
touches nothing.

## One thing to run first

```bash
go mod tidy
```

`go.mod` now requires `go.bug.st/serial v1.6.2` and picks up
`github.com/creack/goselect` behind it. `go.sum` is **not** in this tree — the
sandbox this was built in cannot reach the module proxy, so it was built
against local clones and any `go.sum` lines it produced would be guesses.

## Then

```bash
./scripts/build.sh --anytermqt ../anytermqt

# headless, no lab gear, no root
python3 tests/faketelnetd.py &
./build/examples/c/transport_probe 127.0.0.1 2323

# the real reason the serial leg exists — point it at your PL2303
./build/examples/c/transport_probe --serial /dev/ttyUSB0 9600

# enumeration and the refusal paths alone
./build/examples/c/transport_probe
```

The two session legs combine in one invocation.

## The three transports in a terminal widget

```bash
./build/examples/qt/terminal_window ssh    <host> <port> <user> <key-path>
./build/examples/qt/terminal_window telnet <host> [port]      # default 23
./build/examples/qt/terminal_window serial <device> [baud]    # default 9600
./build/examples/qt/terminal_window serial                    # list ports, exit
```

**One binary, not three, and that is the demonstration.** Three examples would
each be this file with one config block changed, and would prove less than one
does: what Phase 2 claims is that everything above the transport is identical,
and three copies quietly diverging would be evidence against it. Exactly one
function in `terminal_window.cpp` — `buildConfig()` — knows which transport is
open. The attach, the widget, the resize path, the status bar and the teardown
are the same code for all three. Splitting it later is mechanical if you'd
rather have three binaries.

All three were run headless under Xvfb and driven with `xdotool type`, so the
keyboard path is exercised too, not just the render path. Screenshots in
`screenshots/`.

- **SSH** against `tests/labsshd.sh` — coloured prompt, `stty size` reports
  `48 155`, matching the widget's grid exactly, `TERM=xterm-256color`
- **Telnet** against `tests/faketelnetd.py` — banner, a literal `0xFF`
  delivered upward as one byte, typed lines echoed, live prompt
- **Serial** against `tests/fakeconsole.py` over a socat pty pair — ANSI
  colour, device-side echo, bare-CR line endings, live prompt

Window titles come from `summary()` in all three, so the example never
assembles one itself.

## Two things running it turned up

**Two dialogs for one failed dial.** `errorOccurred` was connected before
`start()`, so a failed connect fired a modal warning *inside* `start()`,
blocking the thread mid-call, and was then followed by the critical box on the
return value. Fixed by connecting that handler after `start()`. The bug
predates this phase — it was latent until something actually failed, which is
what running it against a refused connection did.

**Three NAWS pushes on one window opening** — 155x50, then 157x48, then
155x48, as the layout settled and the scrollbar appeared. The first is wrong.
Three subnegotiations on telnet, three channel requests on SSH. Nothing has
objected, but a resize debounce belongs in the tab, not in the transport, so
it is filed under Phase 5 rather than fixed here.

## What changed since the last drop

```
examples/qt/terminal_window.cpp   rewritten: transport-aware, state in the
                                  status bar, serial port listing, the
                                  error-handler ordering fix
examples/c/transport_probe.cpp    + a serial leg (--serial <device> [baud]),
                                  including a TIMED close, which is the check
                                  a fake port cannot make meaningful
tests/fakeconsole.py              new — a device on the far end of a pty pair.
                                  Accepts bare CR only, deliberately: telnet
                                  expands CR to CRLF and serial must not, and
                                  a fixture that took LF would hide that
README.md ROADMAP.md docs/BUILDING.md docs/VERIFIED.md
```

Everything from the previous drop is unchanged and included.

## Verified here

`gofmt` clean, `go vet ./...` clean, `go test -race ./...` green. Clean CMake
configure and build against Qt 6.4.2 with anytermqt consumed by
`add_subdirectory` — **zero warnings** now that the stray characters after
`vault.h`'s `#endif` are gone. `transport_probe` green in all three modes: 28
checks with both legs running.

The existing SSH integration example built against a fresh clone of anytermqt
with no changes needed, which answers whether it had gone stale. It had not.

The Phase 1 session store (`sessions/`) was not built — this sandbox cannot
fetch `libsqlite3-dev`. Nothing here touches it.

## Still open

- **The dial is synchronous.** `WrapSSH` starts at connected, so
  `StateConnecting` and `StateAuthenticating` are never observed and the
  status bar in the example can only ever show connected / disconnected /
  failed. Moving the dial onto its own goroutine fixes that and is also what
  lets it park in authenticating, publish a question, and take an answer.
  `interactionRequired` is wired up in the example already and never fires.
  Note the ordering consequence: today `omegassh_open` returning -1 *is* the
  connect failure; afterwards a failed dial arrives as a state change.
- **Keepalive and reconnect.** `StateReconnecting` exists and nothing
  publishes it.
- **Serial against real gear.** Enumeration is confirmed against your PL2303
  and a CDC-ACM device. The session leg has only run against a pty — a pty
  cannot be unplugged, so the mid-session-unplug path is still only covered by
  the fake port in the unit suite.
- **Local shell** deferred — anytermqt's `PtySession` already does it on all
  three platforms.

## Two decisions I made rather than asked about

`OmegaSshSession` keeps its name now that it drives telnet and serial too. The
name is the library's, not the protocol's, and renaming churns the anytermqt
integration header and both Qt examples for no behavioural change.

`terminal_window` is one binary rather than three, for the reason above. Both
are cheap to reverse.
