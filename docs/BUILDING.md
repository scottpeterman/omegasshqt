# Building omegassh

## What you need

| | Version used | Notes |
|---|---|---|
| Go | 1.21+ | cgo enabled; a C compiler must be on PATH |
| CMake | 3.19+ | |
| Qt | 6.2+, Core only | optional — `--no-qt` skips it |
| C++ | C++17 | gcc, clang or MSVC |
| SQLite | 3.x | for `sessions/`; vendored amalgamation or a `-dev` package — see below |

The Qt wrapper needs **QtCore only**. No Widgets, no Gui. A headless tool can
link it without pulling in a display stack.

Qt 6.2 is the floor, which is what Ubuntu 22.04 ships (`apt install
qt6-base-dev`). Nothing in the build uses `qt_standard_project_setup()` or
the other `qt_add_*` helpers, since those arrived in Qt 6.3 and would make
`find_package(Qt6)` succeed and then fail on an unknown command.

## SQLite for the session store

`sessions/` is SQLite and the standard library, and it is the one dependency
the build scripts cannot satisfy for you. There are two routes and they are
not equally available.

**Vendor the amalgamation** (preferred, and the only route that works on
Windows). Take the current `sqlite-amalgamation-*.zip` from the *Source Code*
section of <https://sqlite.org/download.html>, and copy two files out of it
into `third_party/sqlite3/`:

```
third_party/sqlite3/sqlite3.c
third_party/sqlite3/sqlite3.h
```

The zip unpacks to one directory holding exactly four files: the two above
plus `sqlite3ext.h` and `shell.c`. Neither of the latter is used; copying them
is harmless.

**Not** the GitHub mirror, and not `sqlite-src-*.zip`. The amalgamation is a
*build product* and is absent from the raw source tree — what you get there is
`src/parse.y`, `src/sqlite.h.in` and the rest of the inputs, and turning them
into `sqlite3.c` needs Tcl and `Makefile.msc`. The tell is `.in` files and a
`src/` directory; the amalgamation has neither. A `sqlite-master` folder is
also unreleased trunk rather than a release, which is a second reason not to
build on it.

CMake picks the directory up with no flag, and says so:

```
-- sessions: using vendored SQLite amalgamation
```

Take it over HTTPS from sqlite.org rather than from a mirror or a package
index. This is a quarter of a million lines of C compiled into a binary that
holds credentials, so provenance is the whole point of the exercise. The
download page publishes a SHA3-256 per file, which neither `certutil` nor
PowerShell's `Get-FileHash` can compute; failing that, check the version the
file claims against the release page:

```bat
findstr /b /c:"#define SQLITE_VERSION" /c:"#define SQLITE_SOURCE_ID" third_party\sqlite3\sqlite3.c
```

**Or install the development package**, on Linux and macOS only:

```bash
sudo apt install libsqlite3-dev        # Debian, Ubuntu
sudo dnf install sqlite-devel          # Fedora
brew install sqlite                    # macOS
```

The `-dev` half is the part that gets missed: a machine with a working
`sqlite3` shell and a runtime `libsqlite3` still has no headers, and the
configure fails on a box that plainly has SQLite.

### When it goes wrong

**`No SQLite found, by either route.`** — on Linux or macOS, the `-dev`
package is missing. On Windows there is nothing to install: MSVC has no
`pkg-config` and no system SQLite to find, so the amalgamation is not a
preference there but the only route. Vendor it.

**`-- sessions: no vendored amalgamation, using system SQLite`** — a status
line, not a warning, and it means two different things by platform. On Linux
and macOS it is the fallback working as designed. On Windows it is the line
immediately before the failure, and the real message is that
`third_party/sqlite3/sqlite3.c` is not where CMake looked. The path is
printed absolute in the error for exactly that reason.

**The download has a `src/` directory in it** — that is the canonical source
tree, not the amalgamation. See above; the file you want is
`sqlite-amalgamation-*.zip`.

**`No CMAKE_C_COMPILER could be found`** when adding the amalgamation to a
project that built fine without it — the amalgamation is C, and a `project()`
declaring `LANGUAGES CXX` alone will configure and then fail on the first
`.c`. The top-level and standalone `project()` calls here both declare
`C CXX`.

## Quick start

```bash
git clone https://github.com/scottpeterman/omegasshqt
cd omegasshqt
./scripts/build.sh
```

On Windows, `scripts\build.bat`. Both take the same options:

| | |
|---|---|
| `--no-qt` | build the C surface alone; no Qt needed |
| `--anytermqt <path>` | build everything that needs a terminal widget: the `omega` application, `terminal_window`, `theme_gallery`, and the `terminal_probe` / `hostkey_flow_probe` self-checks |

The script runs `go mod download` itself when there is no `go.sum` yet, and
finds an anytermqt checkout without being told if one is sitting beside the
repo or under `~/github`.

`build.sh` builds and tests; it does not package. For something a person on
another machine can run, see [Packaging](#packaging).

The module requires `go.bug.st/serial` for the serial transport. Its port I/O
is pure Go on all three platforms; only the detailed enumerator is cgo, and
only on darwin — which is why `-framework IOKit` appears on the macOS link
line in the top-level `CMakeLists.txt`. A Go binary never needs that, because
cgo's LDFLAGS resolve during Go's own link; a c-archive that CMake links
does.

## By hand

```bash
# Go side: archive plus a generated header (the generated one is an artifact;
# include/omegassh/omegassh.h is the contract)
CGO_ENABLED=1 go build -buildmode=c-archive -o libomegassh.a ./capi

# C++ side
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Finding Qt

CMake locates Qt through `CMAKE_PREFIX_PATH`. For an aqtinstall layout —
matching the anytermqt setup:

```bash
# Linux
export CMAKE_PREFIX_PATH=$HOME/Qt/6.10.3/gcc_64
# macOS
export CMAKE_PREFIX_PATH=$HOME/Qt/6.10.3/macos
# Windows
set CMAKE_PREFIX_PATH=C:\Qt\6.10.3\msvc2022_64
```

## Platform notes

### Linux

Nothing special. gcc and `pthread`, both already there.

### macOS

cgo links `CoreFoundation` and `Security`; CMake adds them. Xcode command
line tools are enough — no full Xcode install.

For a universal binary, build each architecture separately and `lipo` them;
`GOARCH` cannot produce both at once. Note that `scripts/bundle-mac.sh` does
**not** do this — the shipped packaging is arm64 only. See
[Packaging](#macos-1).

```bash
GOARCH=arm64 go build -buildmode=c-archive -o libomegassh_arm64.a ./capi
GOARCH=amd64 go build -buildmode=c-archive -o libomegassh_amd64.a ./capi
lipo -create -output libomegassh.a libomegassh_arm64.a libomegassh_amd64.a
```

### Windows

**Read [WINDOWS.md](WINDOWS.md) before debugging anything here.** Four separate
problems live in the seam between cgo's mingw-w64 objects and the MSVC link,
and three of the four produce no error at the point of failure — the build is
clean, the link is clean, and the program hangs on the first call into Go.
They are all fixed in the tree; that document is for recognising them if one
comes back or appears in a project that borrows this arrangement.

Two toolchains are in play and this trips people up: **cgo compiles the Go
archive with a GCC-family compiler (mingw-w64), while the C++ side is usually
MSVC.** The resulting `.lib` links fine with MSVC, but mingw-w64 has to be on
PATH or `go build` fails with an unhelpful message about a missing C
compiler.

```
VS 2022 Build Tools   Desktop development with C++
mingw-w64             for cgo (winlibs or MSYS2 both fine)
Qt                    aqtinstall, e.g. 6.10.3 win64_msvc2022_64 --archives qtbase
```

cgo's runtime needs `ws2_32`, `winmm`, `ntdll` and `bcrypt`; CMake adds them.

## Cross-compiling

The Go archive cross-compiles cleanly with a matching C cross-compiler:

```bash
# Windows archive from Linux
GOOS=windows GOARCH=amd64 CGO_ENABLED=1 CC=x86_64-w64-mingw32-gcc \
  go build -buildmode=c-archive -o omegassh.lib ./capi
```

The C++ side is not cross-compiled — build that on the target.

## Testing

```bash
go test -race ./sshcore/...
```

The Go tests are pure unit tests: no network, no sshd, no fixtures. The
integration examples need something to talk to, and `tests/labsshd.sh`
stands up a throwaway sshd on `127.0.0.1:2222` with a key-only account so
they do not depend on any device being reachable:

```bash
sudo ./tests/labsshd.sh start     # prints the exact commands to run

./build/examples/c/shell_smoke      tests/.lab/clientkey /tmp/kh_empty
./build/examples/c/policy_suite     tests/.lab/clientkey /tmp/kh_lab tests/.lab/decoy.hostline
./build/examples/c/knownhosts_probe tests/.lab/clientkey /tmp/kh_lab
./build/examples/qt/qt_smoke        127.0.0.1 2222 labuser tests/.lab/clientkey

sudo ./tests/labsshd.sh clean
```

`shell_smoke` covers the session lifecycle: pty geometry, resize propagation,
escape sequences, exit detection. `policy_suite` covers host-key policy, TOFU
pinning, mismatch refusal, jump hosts, the legacy algorithm tail, and
concurrent sessions. `knownhosts_probe` isolates the malformed-line question.
`qt_smoke` runs the whole thing through `OmegaSshSession` and asserts every
signal arrives on the Qt thread.

The application side has two self-checks of its own, both built only with
`--anytermqt`. Neither needs a display or a window manager:

```bash
QT_QPA_PLATFORM=offscreen ./build/app/terminal_probe

sudo ./tests/labsshd.sh start
QT_QPA_PLATFORM=offscreen ./build/app/hostkey_flow_probe \
    tests/.lab/clientkey tests/.lab/decoy.hostline 127.0.0.1 2222 labuser
```

`terminal_probe` covers Tab reaching the emulator, capture stripping across a
chunk boundary, paste chunking and pacing, and host-key error classification —
it takes a subcommand (`focus`, `capture`, `paste`, `hostkey`) or runs all four.
`hostkey_flow_probe` does the accept / reject / mismatch sequence against a real
sshd, which is the part that lives between two objects and cannot be faked.

`scripts/build.sh` runs both, and skips `hostkey_flow_probe` loudly when nothing
is listening on the lab port — gating on `tests/.lab/` alone would run it
against a stopped server and report "no prompt", which reads like a bug in the
prompt rather than an absent server. `OMEGASSH_LAB_PORT` overrides 2222.

The full inventory of what has and has not been exercised, per platform, is
[VERIFIED.md](VERIFIED.md).

## Packaging

Three scripts, one per platform. Each produces something a person on another
machine can run without a toolchain, and each ends by checking the one failure
this kind of packaging actually has.

| | |
|---|---|
| `scripts/bundle-linux.sh` | relocatable directory, then a tarball |
| `scripts/bundle-appimage.sh` | a single-file AppImage |
| `scripts/bundle-mac.sh` | `Omega.app`, deployed with `macdeployqt` |
| `scripts\bundle-windows.bat` | self-contained folder, deployed with `windeployqt` |
| `scripts\bundle-windows-msix.bat` | optional MSIX, wrapping the folder above |

```bash
./scripts/bundle-linux.sh --anytermqt ../anytermqt
./scripts/bundle-mac.sh   --anytermqt ../anytermqt          # ad-hoc signed
./scripts/bundle-mac.sh   --anytermqt ../anytermqt --dmg --sign "Developer ID Application: ..."
```

```bat
scripts\bundle-windows.bat C:\path\to\anytermqt --zip
```

### Where the themes come from at runtime

Packaging works because `app/main.cpp` looks in more than one place, in this
order:

1. `--themes <dir>`
2. `$OMEGA_THEME_DIR`
3. `../Resources/themes`, relative to the executable
4. the path compiled in at build time
5. `./theme/themes`

**Three is before four on purpose.** The compiled path is an absolute path into
the build tree, so on the machine that built it a package with no themes in it
still finds them and looks perfect — and the same package on any other machine
falls through to the single built-in fallback and opens as a plain window, with
nothing on screen saying why. Looking beside the executable first means the
packaging is exercised by the machine doing the packaging.

Every layout below puts the themes where step 3 finds them.

### Linux

Output is `dist/omega-<version>-linux-<arch>.tar.gz`, unpacking to:

```
omega/
  bin/omega          launcher
  bin/omega.bin      the executable
  lib/               Qt and ICU
  plugins/           platforms, platformthemes, imageformats, tls, xcbglintegrations
  Resources/themes/
```

Not an AppImage — that is `scripts/bundle-appimage.sh`, below. The two are
alternatives, not stages: the tarball needs no tooling and no network, the
AppImage is one file with a desktop entry an environment will pick up. Which to
hand somebody depends on whether they would rather untar a directory or
`chmod +x` a file.

The launcher sets `LD_LIBRARY_PATH` and `QT_PLUGIN_PATH` rather than the binary
carrying an rpath. An rpath would be tidier, but it has to be right for a
layout that does not exist at configure time, and getting it wrong produces a
binary that silently loads the *host's* Qt — which works on the build machine
and fails everywhere else. Three lines in a shell script are checkable by
reading them.

Only `libQt6*` and `libicu*` are copied. libc and the graphics stack come from
the host on purpose: shipping them is how a package stops working on a
distribution newer than the one that built it.

### Linux: AppImage

```bash
./scripts/bundle-appimage.sh --anytermqt ../anytermqt
```

Produces `dist/Omega-<version>-x86_64.AppImage`, about 32 MB. `linuxdeploy` and
its Qt plugin are downloaded from GitHub on first run and cached in
`.linuxdeploy/`; set `LINUXDEPLOY_DIR` to use copies you already have, since
this is the one packaging path that needs network access.

The AppDir layout is the one `app/main.cpp` already looks for: the binary lands
at `AppDir/usr/bin/omega`, so `../Resources/themes` resolves to
`AppDir/usr/Resources/themes` and nothing in the application changed to be
packaged this way.

Two things worth knowing:

- **`linuxdeploy` is itself an AppImage, so it needs FUSE.** Containers and
  minimal CI images often have none, and the error is about a missing library
  rather than about FUSE. The script sets `APPIMAGE_EXTRACT_AND_RUN=1`, which
  sidesteps it by unpacking to a temporary directory.
- **Only the `xcb` platform plugin is bundled**, which is right for something
  people run on a desktop and means there is no `offscreen` to smoke-test with.
  Use a virtual display instead:
  `Xvfb :99 -screen 0 1280x900x24 & DISPLAY=:99 ./dist/Omega-*.AppImage`

The desktop entry is `packaging/linux/omega.desktop`, and the icon comes from
`art/omega-icon-256.png`. The plain tarball has neither, which is why it does
not appear in an application menu.

### macOS

arm64 only. A universal binary needs the Go archive built twice and `lipo`'d
(the commands are under [Platform notes](#macos) above) and the CMake side has
no seam for it; every Mac sold since 2020 is arm64.

The bundle is **opt-in**, via `-DOMEGA_MACOS_BUNDLE=ON`. Off by default because
turning it on moves the executable from `build/app/omega` to
`build/app/Omega.app/Contents/MacOS/Omega`, which is where `scripts/build.sh`
looks and where every other note here points. Packaging is a separate build
directory; the development loop does not change shape.

The order the script runs in is not obvious and each step depends on the one
before:

1. **`iconutil`, before CMake.** `app/CMakeLists.txt` tests `EXISTS` on
   `art/Omega.icns` at *configure* time, so an icns built afterwards is not in
   the bundle until the next configure — and the bundle looks fine apart from a
   generic icon.
2. **CMake configure and build.** Themes and icns go in as a `POST_BUILD` step,
   so the `.app` in the build tree is already complete.
3. **`macdeployqt`.** Copies the Qt frameworks and rewrites install names. It
   does *not* copy application resources, which is why step 2 does the themes.
   It is also not reliably idempotent: treat the build directory as disposable
   for packaging rather than incremental.
4. **`codesign`, last.** macdeployqt rewrites binaries, which invalidates any
   signature already on them.

`macdeployqt` must come from the **same Qt the application linked against**. A
Homebrew `macdeployqt` over an aqtinstall build, or the reverse, copies
frameworks whose install names do not match what is in the binary, and the
result runs on the build machine and fails on a clean one. Set
`CMAKE_PREFIX_PATH` explicitly rather than letting CMake pick; the script prints
what the binary is linked against before deploying.

Ad-hoc signing (`codesign -s -`, what the script does with no `--sign`) is
enough to run it yourself. Distribution to anyone else needs a Developer ID and
notarization, or Gatekeeper tells them the developer cannot be verified.

The keychain is not affected by any of this. `zalando/go-keyring` on darwin
shells out to `/usr/bin/security` rather than calling the Security framework
directly, so the keychain ACL belongs to that binary, not to Omega's code
signature — the vault behaves the same signed, unsigned, or run straight out of
`build/app`.

### Windows

```bat
scripts\bundle-windows.bat
scripts\bundle-windows.bat --qt C:\Qt\6.10.3\msvc2022_64 --zip
```

`windeployqt` copies the Qt DLLs beside the exe and the plugins into
`platforms\` and friends. It does not copy application resources, so the script
copies `theme/themes` into `Resources\themes` itself.

No bundle option is needed the way `OMEGA_MACOS_BUNDLE` is on macOS: the exe
stays where it was and packaging is a copy.

Every dependency takes a flag, falls back to an environment variable, then
guesses, and whatever it settles on is echoed before the build:

| | |
|---|---|
| anytermqt | `--anytermqt` · `%OMEGASSH_ANYTERMQT_DIR%` · beside the repo · `%USERPROFILE%\github` |
| Qt | `--qt` · `%CMAKE_PREFIX_PATH%` · `%Qt6_DIR%` · newest `C:\Qt\6.*\msvc*_64` |

**`WIN32_EXECUTABLE` is why there is no console window.** Without it the linker
produces a console-subsystem binary, so Windows allocates a console before the
GUI appears and leaves it behind the application for as long as it runs.
Launching from a Developer Command Prompt hides this completely — a console is
already there — so it surfaces the moment anybody double-clicks the exe or
launches it from the Start menu, which is to say the moment it is packaged. Set
on `omega` only; the probes are console programs whose output would otherwise
be discarded. The trade is that a `WIN32_EXECUTABLE` has no stdout or stderr,
so `main.cpp`'s diagnostics, including `no themes found in <dir>`, go nowhere
on Windows. Use `--themes` to check a path deliberately.

**Two Qt traps, both of which have already produced a wrong package.**

*Version directories do not sort as text.* `dir /o-n` puts `6.8.3` above
`6.10.3`, so the "newest" guess picked the wrong one and deployed 6.8.3 DLLs
beside a binary linked against 6.10.3. The script now sorts with PowerShell's
`[version]` cast, which compares the parts as numbers.

*A cached `Qt6_DIR` beats `-DCMAKE_PREFIX_PATH`.* An existing build directory
configured against a different Qt keeps using it and says nothing. CMake offers
no way to un-cache a `find_package` result, so the script compares
`CMakeCache.txt` against the chosen prefix and wipes the directory on a
mismatch. It also re-checks after `windeployqt` and fails if the two disagree —
that check is the one that catches this, because a `Qt6Core.dll` from the wrong
Qt is present, correctly named, and wrong, and the package still runs on the
build machine where the real Qt is on PATH.

**The anytermqt check.** anti-idle calls
`qtpyte::TerminalWidget::alternateScreen()`, which an older anytermqt does not
have. MSVC reports that as `'alternateScreen': is not a member of
'omega::app::TerminalView'` — naming the derived class in *this* repo, minutes
into a build, for a method that lives in the base class in the *other* repo.
Nothing in that error says "git pull anytermqt", so the script greps
`terminalwidget.h` up front and says it instead.

### Windows: MSIX

Optional, and layered on top of the folder above rather than replacing it.

```bat
scripts\bundle-windows.bat
scripts\bundle-windows-msix.bat --sign-self
```

The second script packages `dist\omega` and builds nothing, deliberately: the
MSIX is a wrapper around exactly the folder you have already run, so if the
folder works and the MSIX does not, the packaging is what changed.

Tile assets come from `python art/make-icons.py`, which writes `art/msix/` —
the square logos at scale-100 and scale-200, the unplated targetsize variants
the taskbar reads, and a wide tile centre-cropped from the banner. Full bleed
here, not the 80% macOS inset: Windows draws its own tile background and
expects the artwork to fill it.

**Signing is not optional for MSIX.** An MSI or an Inno installer runs unsigned
with a SmartScreen warning; Windows refuses to *install* an unsigned MSIX at
all. `--sign-self` generates a certificate and reuses it on later runs — it has
to be reused, because a new key means the `.cer` already imported no longer
matches and the install fails with `0x800B0109`, "terminated in a root
certificate which is not trusted", which reads like the import never worked
rather than like the certificate changed underneath it. Delete
`dist\omega-test.pfx` to force a fresh one.

`Publisher` in `packaging/msix/AppxManifest.xml` must equal the certificate
subject character for character. For the Store, Partner Center assigns both
`Name` and `Publisher` and re-signs the package, which is the one route that
needs no certificate of your own.

Installing, in an **elevated PowerShell**:

```powershell
Import-Certificate -FilePath dist\omega-test.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople
Add-AppxPackage -Path dist\Omega-0.1.0-x64.msix
```

Rebuilding at the same version fails with `0x80073CFB`, same identity and
different contents. For a test loop, register the unpacked layout instead and
skip packing, signing and version collisions entirely:

```powershell
Add-AppxPackage -Register dist\msix\AppxManifest.xml
```

**`~\.nterm` is not redirected, which was the question that decided whether any
of this was worth doing.** MSIX runs packaged applications with filesystem and
registry redirection, and Omega's whole premise is that `~\.nterm` is *shared*
with nterm-qt — the same `config.json`, the same `sessions.db`. Virtualized
writes would leave both applications working and silently unable to see each
other's sessions, which is worse than a crash.

Tested by deleting the directory and letting the packaged build recreate it: it
came back at the real `%USERPROFILE%\.nterm`, not under
`%LOCALAPPDATA%\Packages\ScottPeterman.Omega*\LocalCache\`. `%USERPROFILE%` is
not `%LOCALAPPDATA%` and the redirection does not reach it. The vault works
under package identity too — its keyring backend is `wincred`, and a credential
unlocked normally on a machine with no prior entry.

### What each script verifies

The failure mode all three share is loading the *host's* Qt instead of the
shipped one, which looks like success on the build machine. Checking at package
time is the only place it is cheap:

| | |
|---|---|
| Linux | `ldd` shows no Qt resolving outside the bundle; the staged tree starts under `QT_QPA_PLATFORM=offscreen` and finds its own themes |
| macOS | `otool -L` shows every Qt reference as `@executable_path`; the platform plugin is present; `lipo -archs` reports the architecture |
| Windows | `platforms\qwindows.dll`, `Qt6Widgets.dll` and `Resources\themes` are present, **and** the Qt the build linked against is the Qt windeployqt deployed from |

The platform plugin check earns its place on every platform: its absence is
fatal and the resulting error does not name it usefully.

If you write your own check that launches the binary, **bound it with a
timeout**. `omega` is a GUI application: a plain invocation enters its event
loop and never returns, so the check hangs rather than failing. What is being
read is what it prints on the way up, and being killed is the success case.

### Artwork

`art/omega-icon.svg` is the source for every icon. Everything else is derived:

```bash
pip install cairosvg pillow
python3 art/make-icons.py
```

That regenerates `Omega.iconset/`, `omega.ico`, both 1024px PNGs and the
downscaled banner the About dialog uses. On macOS, one more step turns the
iconset into the icon `bundle-mac.sh` picks up:

```bash
iconutil -c icns art/Omega.iconset -o art/Omega.icns
```

Not wired into CMake, deliberately: it needs a Python toolchain for assets that
change twice a year.

The macOS icons are inset to 80% of their canvas because Apple's own are — an
icon that fills its canvas edge to edge sits visibly larger than everything
around it in the Dock. Windows has no such convention, so `omega.ico` is full
bleed.

The banner and the window icon are compiled into the binary through
`app/omega.qrc` rather than found on disk. **`omega_shell` is a static library,
and a Qt resource in one does not self-register**: the object `rcc` generates is
referenced by nothing, the linker drops it, and every `QPixmap(":/omega/...")`
silently comes back null — a blank label, not an error.
`initOmegaResources()` in `app/helpdialog.h` exists for that, and anything else
added to that `.qrc` needs it called before use.

## Papercuts worth knowing

Small things that cost real time to rediscover. None is a bug in the tree; each
is a place where the obvious move is wrong.

**`vault_probe` wants a vault file that does not exist yet.** Point it at one a
previous run already populated and `scope survived` fails, because the
credential it expects to create is already there. That is the probe's
limitation, not a finding about the vault. Give it a fresh path every time:

```bash
rm -f /tmp/probe-vault.json
./build/qt/vault_probe /tmp/probe-vault.json
```

`session_tree_probe` and `ttyaml_probe` do not have this problem — both remove
their database on the way in. `shell_probe` reuses its config file on purpose,
since round-tripping an existing one is the thing it tests.

**A geometry or session probe must never be pointed at `~/.nterm`.** Both
`shell_probe` and `session_tree_probe` write. They default to throwaway paths,
and the defaults are there because the real `sessions.db` is the tree you
actually use.

**Keyboard shortcuts do not arrive under a bare X server.** With no window
manager the application never takes keyboard focus, so `Ctrl+N`, `Ctrl+I` and
`Ctrl+E` do nothing when driven by `xdotool key` under Xvfb — the events go to
the root window. Drive the menus with the mouse instead; clicks are delivered
by position and work fine. This is a property of the harness, not the
application, and it will waste an hour if you meet it fresh.

**The BSD notifier gap is a deferred decision, not an oversight.**
`capi/notify_linux.go` takes an implicit `GOOS=linux` constraint from its
filename that a build tag cannot widen, so freebsd, openbsd and netbsd have no
`newNotifier` — they had one under the old `!windows` file, and
`syscall.Pipe2` exists on all of them. The BSDs are not a target today. If they
become one, the fix is a rename to `notify_pipe2.go` with `//go:build
!windows && !darwin`, with no change to the contents.

**TerminalTelemetry import replaces `extras` on merge, rather than merging
into it.** A session's vendor, model and device-type keys are overwritten by
whatever the YAML carries, and any other key it held is dropped. This matches
`import_terminal_telemetry` deliberately, so the differential can compare the
two implementations — but it is destructive, and it is a divergence worth
making on purpose if you decide the differential is not worth that.
`sessions/sessionio.cpp` says so at the point of the write.

**Two of the YAML differential's skips are nterm-qt bugs, not gaps.** A
non-numeric port aborts `import_terminal_telemetry` mid-import, and an in-file
duplicate host is counted as imported but never written. Omega diverges from
both on purpose. `tests/compat/ttyaml_differential.py` carries the reasoning
next to the skip list; [VERIFIED.md](VERIFIED.md) has the evidence.

## Building the anytermqt example

`examples/qt/terminal_window` puts an anytermqt `TerminalWidget` on top of an
omegassh session — the two halves as one thing — over any of the three
transports:

```bash
terminal_window ssh    <host> <port> <user> <key-path>
terminal_window telnet <host> [port]      # port defaults to 23
terminal_window serial <device> [baud]    # baud defaults to 9600
terminal_window serial                    # list ports and exit
```

One binary rather than three on purpose. Three examples would each be this
file with one config block changed, and would prove less than one does: what
Phase 2 claims is that everything above the transport is identical, and three
copies quietly diverging would be evidence against it. Exactly one function in
that file — `buildConfig()` — knows which transport is open.

anytermqt installs no CMake package config, so there is nothing for
`find_package` to find; it is consumed with `add_subdirectory`. The build
script resolves the path in this order:

1. `--anytermqt <path>`
2. `$OMEGASSH_ANYTERMQT_DIR`
3. a sibling checkout, or `~/github/anytermqt`, or `~/src/anytermqt`

A checkout is recognized by containing `qtpyte/CMakeLists.txt`, not by its
directory name, so a fork or a differently named clone still works. A path
given explicitly that turns out to be wrong is an error; a guess that misses
is not — the example is skipped and the rest of the build proceeds.

```bash
./scripts/build.sh --anytermqt ../anytermqt
./build/examples/qt/terminal_window ssh eng-leaf-1.lab.local 22 labadmin ~/.ssh/id_ed25519
```

Or with CMake directly:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DOMEGASSH_ANYTERMQT_DIR=../anytermqt
cmake --build build -j
```

`qtpyte`'s own tests and host app are switched off for this build — omegassh
consumes the widget, it does not re-verify it.

Unlike `qt_smoke`, this example needs a display — though not a physical one.
It has been driven headless under Xvfb, which is also how the screenshots in
`docs/VERIFIED.md` were taken:

```bash
Xvfb :99 -screen 0 1280x800x24 &
export DISPLAY=:99 QT_QPA_PLATFORM=xcb
./build/examples/qt/terminal_window telnet 127.0.0.1 2323
```

`xdotool type` and `xdotool key Return` drive it from there, which is enough
to exercise the keyboard path as well as the render path.

## Using it from another project

### CMake

```cmake
add_subdirectory(external/omegassh)
target_link_libraries(myapp PRIVATE omegassh::qt)   # or omegassh::c
```

### Plain C or C++

```bash
g++ -std=c++17 -Iomegassh/include main.cpp libomegassh.a -lpthread -o myapp
```

Include `<omegassh/omegassh.h>`, not the cgo-generated header. Do not include
both in one translation unit — the declarations collide.

## Size

The Go runtime rides along in the archive. Measured on Linux x86-64:

| | |
|---|---|
| `libomegassh.a` | ~15 MB |
| linked C++ binary, stripped | ~4.3 MB |
| the same binary with no omegassh | ~14 KB |

About 4 MB of runtime in the shipped binary. Against a Qt app already
carrying QtCore and QtWidgets, that is noise.

`-buildmode=c-shared` also works if a `.so`/`.dll` suits the packaging better.