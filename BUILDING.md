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
git clone https://github.com/scottpeterman/omegassh
cd omegassh
./scripts/build.sh
```

On Windows, `scripts\build.bat`. Both take the same options:

| | |
|---|---|
| `--no-qt` | build the C surface alone; no Qt needed |
| `--anytermqt <path>` | build the `terminal_window` example too |

The script runs `go mod download` itself when there is no `go.sum` yet, and
finds an anytermqt checkout without being told if one is sitting beside the
repo or under `~/github`.

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
`GOARCH` cannot produce both at once.

```bash
GOARCH=arm64 go build -buildmode=c-archive -o libomegassh_arm64.a ./capi
GOARCH=amd64 go build -buildmode=c-archive -o libomegassh_amd64.a ./capi
lipo -create -output libomegassh.a libomegassh_arm64.a libomegassh_amd64.a
```

### Windows

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