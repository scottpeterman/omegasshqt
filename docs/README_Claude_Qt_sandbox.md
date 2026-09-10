# README_Claude_Qt_Sandbox.md

Recipe for standing omegassh up in a Claude sandbox, so changes get **built and
tested** there rather than reasoned about from reading the source.

The Fyne equivalent for PathfinderSSH is `README_Claude_Fyne_Sandbox.md`. This
one is harder in exactly two places — the Go module cannot reach its proxy, and
the Qt half needs a checkout that is not in this repo — and both have a fixed
answer below. Everything else is ordinary CMake.

Verified end to end on Ubuntu 24.04 with Go 1.22.2, CMake 3.28.3 and Qt 6.4.2.

---

## 1. Toolchain

```bash
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y golang-go cmake qt6-base-dev
```

Three packages, nothing else. `qt6-base-dev` brings QtCore, QtGui and QtWidgets
plus the offscreen platform plugin, which is what makes the GUI testable with
no display. No Qt Creator, no full SDK, no aqtinstall.

Ubuntu 24.04 ships Qt **6.4.2**. `qt/CMakeLists.txt` requires 6.2 or newer and
avoids `qt_standard_project_setup()` (6.3+) on purpose, so this is inside the
supported range rather than merely working by luck.

If `apt-get update` reports a 403 on a third-party repo (nodesource, for
instance), ignore it — the Ubuntu archives are what matter and they are on the
allowlist.

## 2. The Go module cannot use its proxy

`proxy.golang.org`, `sum.golang.org` and `golang.org` itself are **not** on the
sandbox egress allowlist; `github.com` is. So the module has to be fetched
straight from GitHub, and the three `golang.org/x/*` vanity paths have to be
pointed at their GitHub mirrors:

```bash
export GOPROXY=direct GOSUMDB=off GOPRIVATE='*' GOFLAGS=-mod=mod

go mod edit \
  -replace golang.org/x/crypto=github.com/golang/crypto@v0.31.0 \
  -replace golang.org/x/sys=github.com/golang/sys@v0.28.0 \
  -replace go.bug.st/serial=github.com/bugst/go-serial@v1.6.2
```

`go.bug.st/serial` is a vanity path too, and resolving it is what pulls in
`golang.org/x/sys` — miss that one and the serial package alone fails to build
while everything else succeeds, which reads as a serialx problem and is not.

**Revert before delivering anything.** These replaces are a property of the
sandbox, not of the project:

```bash
git checkout go.mod go.sum
```

Check `git status` before handing files over. A `go.mod` with mirror replaces in
it is a bad delivery.

**Put them back before building again.** Reverting and then continuing to work
is the trap: the Go archive step fails, `cmake --build` stops before it reaches
anything C++, and every binary in `build/` stays at its previous contents.
Nothing crashes and nothing looks obviously wrong — you get a stale executable
that runs, so a change you just made appears to have had no effect, and the
next hour goes into debugging code that was never compiled.

Two habits make it cheap. Grep the build output for `error` rather than
skimming it, since the Go failure scrolls past above the CMake summary. And
when a change seems not to have taken, check the binary's timestamp before
touching the source:

```bash
ls -la build/app/omega        # older than your edit? it never rebuilt
```

## 3. anytermqt

The terminal widget lives in its own repo and is consumed by
`add_subdirectory`, not by `find_package`. Clone it beside this one:

```bash
cd ..
git clone --depth 1 https://github.com/scottpeterman/anytermqt.git
```

Without it, `OMEGASSH_BUILD_APP` refuses to configure and says so. Everything
else — the Go archive, the C surface, the Qt session wrapper, the vault
wrapper, the session store, the theme system — builds without it.

## 4. Configure and build

```bash
cd omegassh
cmake -S . -B build \
  -DOMEGASSH_ANYTERMQT_DIR=/absolute/path/to/anytermqt \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Full build from cold is a couple of minutes, most of it the Go c-archive and
Qt's AUTOMOC.

**Re-run the configure step after adding a source file or a target.** CMake
does not notice a new `add_executable` on its own, and the failure looks like
`No rule to make target`, which reads as a broken CMakeLists rather than a
stale cache.

## 5. Test battery

Run all of it after any change to the vault, the shim or the shell. Each one is
a different layer, so a failure says where to look:

```bash
go test ./...                                    # Go: vault, sshcore, transports
./build/examples/c/vault_smoke /tmp/lab-vault.json      # the C boundary
./build/qt/vault_probe /tmp/probe-vault.json            # the C++ wrapper
./build/examples/c/credential_probe                     # credential refusals, pre-socket
QT_QPA_PLATFORM=offscreen \
  ./build/app/shell_probe theme/themes /tmp/probe-config.json 900 640 30 50 260
```

If `vault_smoke` passes and `vault_probe` fails, the marshalling in
`qt/omegasshvault.cpp` is where to look, not the shim. That layering is the
point of having both.

`shell_probe` prints a `saved` line and a `restored` line; they must match.

## 6. Looking at the GUI with no display

Two levels, and the cheap one covers most of it.

**Offscreen grabs.** Enough for any dialog, form or window that does not need
input. A throwaway harness that constructs the widget, shows it, and grabs it:

```cpp
w->show();
qApp->processEvents();
w->grab().save("/tmp/shot.png");
```

run with `QT_QPA_PLATFORM=offscreen`. This is how the vault dialogs were
checked — it caught a `QVBoxLayout` where a `QHBoxLayout` belonged and a
word-wrapped `QLabel` in a `QFormLayout` row that was sitting underneath the
row after it, neither of which is visible by reading.

Two things to know about it. A wrapped label reports the height the layout
believed before it wrapped, so overlap in a grab is real, not an artifact. And
`propagateSizeHints()` warnings from the offscreen plugin are noise.

**Keep the harness out of the repo.** Write it under `/tmp`, add its target to
`app/CMakeLists.txt` temporarily, and strip the target before delivering — the
same rule as no demo code in the tree. `tests/compat/*_probe.cpp` is where
something worth keeping goes instead.

**Xvfb plus xdotool** is only needed for behaviour that requires real input:
drag-and-drop, a window manager delivering a close event, focus. Nothing so far
has needed it; the session tree's drop handling will.

## 7. What the sandbox cannot tell you

- **No OS keyring.** `dbus-launch` is absent, so every keyring call returns
  `KEYRING_UNAVAILABLE`. That makes the unavailable branch easy to exercise and
  the `KEYRING_STALE` and successful-auto-unlock branches impossible. Both need
  a real box.
- **No hardware and no lab.** Serial enumeration finds nothing, and a dial gets
  as far as DNS. `credential_probe` is built around that limit deliberately —
  it decides every refusal before a socket opens.
- **No Windows and no macOS.** The MSVC seams in the root `CMakeLists.txt`
  (`.CRT$XCU` init shim, `.pdata` sort, `legacy_stdio_definitions`) are not
  exercised here at all. See `docs/WINDOWS.md`.

A change that touches any of those gets built and unit-tested here, and
confirmed on the real machine before it counts as working.