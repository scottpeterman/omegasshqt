#!/usr/bin/env bash
# scripts/build.sh
#
# Full build for Linux and macOS: Go tests, c-archive, Qt wrapper, examples.
#
#   ./scripts/build.sh                      # everything it can find
#   ./scripts/build.sh --no-qt              # C surface only, no Qt needed
#   ./scripts/build.sh --anytermqt <path>   # point at an anytermqt checkout
#
# Two differential suites run against an nterm-qt checkout when NTERMQT_SRC
# points at one -- the session store's and the theme system's. Both are skipped
# loudly rather than quietly when it is unset.
#
# examples/qt/terminal_window needs anytermqt, which installs no CMake package
# config and so cannot be found with find_package. The path is discovered in
# this order:
#
#   1. --anytermqt <path>
#   2. $OMEGASSH_ANYTERMQT_DIR
#   3. a sibling or ~/github checkout, if one is sitting there
#
# Not finding it is not an error: the rest builds and the example is skipped.
#
# Qt is located through CMAKE_PREFIX_PATH. For an aqtinstall layout:
#   export CMAKE_PREFIX_PATH=$HOME/Qt/6.10.3/gcc_64

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

BUILD_QT=ON
ANYTERMQT="${OMEGASSH_ANYTERMQT_DIR:-}"

usage() {
    sed -n '3,21p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-qt) BUILD_QT=OFF; shift ;;
        --anytermqt)
            [[ $# -ge 2 ]] || { echo "error: --anytermqt needs a path" >&2; exit 2; }
            ANYTERMQT="$2"; shift 2 ;;
        --anytermqt=*) ANYTERMQT="${1#*=}"; shift ;;
        -h|--help) usage 0 ;;
        *) echo "unknown option: $1" >&2; usage 2 ;;
    esac
done

die() { echo "error: $*" >&2; exit 1; }

# An anytermqt checkout is recognized by its widget, not by its directory
# name: a fork or a differently named clone is still the right thing.
is_anytermqt() { [[ -f "$1/qtpyte/CMakeLists.txt" ]]; }

# --- preflight -------------------------------------------------------------
# Go reports a missing module as "directory prefix sshcore does not contain
# main module", which reads like a problem with the source tree rather than a
# missing file. Check the obvious things first and say so plainly.

command -v go >/dev/null || die "go is not on PATH"
command -v cmake >/dev/null || die "cmake is not on PATH"

# Deliberately not reproducing go.mod here. It used to be inlined, and went
# stale the moment the vault added dependencies -- a recovery instruction that
# reconstructs the wrong file is worse than none. build.bat already points at
# the doc; both do now.
[[ -f go.mod ]] || die "no go.mod at ${ROOT}
  The module file is missing from this working copy.
  See docs/BUILDING.md for its contents."

# A go.work anywhere above the repo silently replaces the main module set. If
# one exists and does not list this directory, every ./... pattern misses.
WORKFILE="$(go env GOWORK 2>/dev/null || true)"
if [[ -n "${WORKFILE}" ]] && ! go list ./sshcore >/dev/null 2>&1; then
    die "a Go workspace is shadowing this module: ${WORKFILE}
  Either add this repo to it, or build with GOWORK=off:

    GOWORK=off ./scripts/build.sh"
fi

# --- locate anytermqt ------------------------------------------------------
# An explicit path that turns out to be wrong is a mistake worth stopping for.
# A guess that misses is not.

if [[ -n "${ANYTERMQT}" ]]; then
    is_anytermqt "${ANYTERMQT}" || die \
        "${ANYTERMQT} does not look like an anytermqt checkout (no qtpyte/CMakeLists.txt)"
    ANYTERMQT="$(cd "${ANYTERMQT}" && pwd)"
elif [[ "${BUILD_QT}" == "ON" ]]; then
    for candidate in \
        "${ROOT}/../anytermqt" \
        "${ROOT}/../AnyTermQt" \
        "${HOME}/github/anytermqt" \
        "${HOME}/src/anytermqt"
    do
        if is_anytermqt "${candidate}"; then
            ANYTERMQT="$(cd "${candidate}" && pwd)"
            echo "==> found anytermqt at ${ANYTERMQT}"
            break
        fi
    done
fi

if [[ ! -f go.sum ]]; then
    echo "==> go mod download (no go.sum yet)"
    go mod download
fi

CMAKE_VER="$(cmake --version | head -1 | awk '{print $3}')"
printf '==> toolchain: %s, cmake %s\n' "$(go version | awk '{print $3}')" "${CMAKE_VER}"

# --- build -----------------------------------------------------------------
# ./... rather than ./sshcore/...: the vault package has its own suite, and
# scoping this to one package is how a second package's tests quietly stop
# running the day it is added. capi has no tests but vets clean, and vetting
# the cgo shim is worth the second it costs.
echo "==> go vet"
go vet ./...

echo "==> go test (race)"
go test -race -count=1 ./...

CMAKE_ARGS=(-S . -B build -DCMAKE_BUILD_TYPE=Release -DOMEGASSH_BUILD_QT="${BUILD_QT}")
if [[ -n "${ANYTERMQT}" && "${BUILD_QT}" == "ON" ]]; then
    CMAKE_ARGS+=(-DOMEGASSH_ANYTERMQT_DIR="${ANYTERMQT}")
fi

echo "==> cmake configure"
cmake "${CMAKE_ARGS[@]}"

echo "==> cmake build"
cmake --build build -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

# --- session store compatibility -------------------------------------------
# The differential suite runs the C++ store and nterm-qt's Python store against
# copies of one database and compares. That needs an nterm-qt checkout, which
# is not a dependency of this repo -- a fresh clone on a machine that has never
# seen nterm-qt must still build clean.
#
# So it is gated on NTERMQT_SRC, and the skip is loud. A quiet skip would mean
# a green build says nothing about compatibility, which is worse than not
# having the suite at all.

PROBE="build/sessions/compat_probe"

if [[ ! -x "${PROBE}" ]]; then
    echo "==> compat suite: skipped (no compat_probe -- built with -DOMEGASSH_BUILD_SESSIONS=OFF?)"
elif [[ -z "${NTERMQT_SRC:-}" ]]; then
    echo "==> compat suite: SKIPPED -- NTERMQT_SRC is not set"
    echo "    The store's compatibility with nterm-qt is unverified in this build."
    echo "    NTERMQT_SRC=/path/to/nterm-qt ./scripts/build.sh"
elif [[ ! -f "${NTERMQT_SRC}/ntermqt/manager/models.py" \
     && ! -f "${NTERMQT_SRC}/nterm/manager/models.py" ]]; then
    # Both layouts are valid: the package was flattened and renamed from
    # nterm to ntermqt, and checkouts of either vintage work as an oracle.
    die "NTERMQT_SRC=${NTERMQT_SRC} has no session store module.
  Looked for ntermqt/manager/models.py and nterm/manager/models.py.
  Point it at the root of an nterm-qt checkout, or unset it to skip the suite."
else
    PY="$(command -v python3 || command -v python || true)"
    [[ -n "${PY}" ]] || die "NTERMQT_SRC is set but no python interpreter is on PATH"
    echo "==> compat suite (against ${NTERMQT_SRC})"
    NTERMQT_SRC="${NTERMQT_SRC}" "${PY}" tests/compat/differential.py --probe "${PROBE}"
fi

# --- settings compatibility ------------------------------------------------
# The settings differential runs the C++ layer and config.py over the same
# config.json content and compares: the fifteen parsed values, and the bytes
# each writes back. The byte comparison is the one that matters -- both
# applications rewrite this file, and a formatting difference would make every
# alternating save look like an edit.
#
# Same NTERMQT_SRC gate as the other two suites, skipped just as loudly.

SETTINGS_PROBE="build/app/settings_probe"

if [[ ! -x "${SETTINGS_PROBE}" ]]; then
    echo "==> settings suite: skipped (no settings_probe -- built with -DOMEGASSH_BUILD_APP=OFF?)"
elif [[ -z "${NTERMQT_SRC:-}" ]]; then
    echo "==> settings suite: SKIPPED -- NTERMQT_SRC is not set"
    echo "    config.json compatibility with nterm-qt is unverified in this build."
    echo "    NTERMQT_SRC=/path/to/nterm-qt ./scripts/build.sh"
elif [[ ! -f "${NTERMQT_SRC}/ntermqt/config.py" ]]; then
    echo "==> settings suite: SKIPPED -- no ntermqt/config.py under ${NTERMQT_SRC}"
else
    PY="$(command -v python3 || command -v python || true)"
    [[ -n "${PY}" ]] || die "NTERMQT_SRC is set but no python interpreter is on PATH"
    echo "==> settings suite (against ${NTERMQT_SRC})"
    "${PY}" tests/compat/settings_differential.py \
        --probe "${SETTINGS_PROBE}" \
        --ntermqt "${NTERMQT_SRC}"
fi

# --- shell geometry --------------------------------------------------------
# The one claim 4a makes that no differential can check: that the window's
# geometry survives a close and comes back on the next open. shell_probe does
# it -- open, place, close, reopen, report -- under the offscreen platform, so
# it needs no display and no window manager.
#
# Not gated on NTERMQT_SRC: this compares Omega against itself.

SHELL_PROBE="build/app/shell_probe"

if [[ ! -x "${SHELL_PROBE}" ]]; then
    echo "==> shell probe: skipped (not built)"
else
    echo "==> shell probe (offscreen)"
    SHELL_CONFIG="$(mktemp -d)/config.json"
    QT_QPA_PLATFORM=offscreen "${SHELL_PROBE}" \
        theme/themes "${SHELL_CONFIG}" 900 600 137 91 320
fi

# --- terminal tab ----------------------------------------------------------
# Tab reaching the emulator instead of the focus chain, and capture stripping
# surviving a chunk boundary. Neither is checkable by reading the code, and the
# focus one has a control case in it: a bare qtpyte::TerminalWidget is asserted
# to LOSE focus, so if anytermqt ever fixes it upstream this is what says so.
#
# Offscreen, and not gated on NTERMQT_SRC: it compares Omega against itself.

TERMINAL_PROBE="build/app/terminal_probe"

if [[ ! -x "${TERMINAL_PROBE}" ]]; then
    echo "==> terminal probe: skipped (not built)"
else
    echo "==> terminal probe (offscreen)"
    QT_QPA_PLATFORM=offscreen "${TERMINAL_PROBE}"
    # focus, capture, paste pacing, host key classification, the anti-idle
    # keystroke and its suppressions, and the scrollback limit
fi

# --- host key prompt, end to end -------------------------------------------
# Needs the lab sshd: tests/labsshd.sh start. What it covers is the sequencing
# between a failed session and the re-dial that follows an accepted key, which
# is between two live objects and cannot be faked -- it broke once in exactly
# the way no unit test could see.
#
# Skipped, loudly, when the server is not up.

HOSTKEY_PROBE="build/app/hostkey_flow_probe"
LAB_DIR="tests/.lab"

LAB_PORT="${OMEGASSH_LAB_PORT:-2222}"

# Listening, not merely generated. tests/.lab/ outlives a stopped server --
# labsshd.sh stop leaves the keys in place -- so gating on the files alone
# runs the probe against nothing and reports it as "no prompt", which reads
# like a bug in the prompt rather than an absent server.
lab_is_up() {
    (exec 3<>"/dev/tcp/127.0.0.1/${LAB_PORT}") >/dev/null 2>&1
}

if [[ ! -x "${HOSTKEY_PROBE}" ]]; then
    echo "==> host key flow: skipped (not built)"
elif [[ ! -f "${LAB_DIR}/clientkey" || ! -f "${LAB_DIR}/decoy.hostline" ]]; then
    echo "==> host key flow: skipped (no lab material -- ./tests/labsshd.sh start)"
elif ! lab_is_up; then
    echo "==> host key flow: skipped (nothing listening on 127.0.0.1:${LAB_PORT}"
    echo "                            -- ./tests/labsshd.sh start)"
else
    echo "==> host key flow (offscreen, against the lab sshd)"
    QT_QPA_PLATFORM=offscreen "${HOSTKEY_PROBE}" \
        "${LAB_DIR}/clientkey" "${LAB_DIR}/decoy.hostline" \
        127.0.0.1 "${LAB_PORT}" labuser
fi

# --- theme compatibility ---------------------------------------------------
# The theme differential runs generateStylesheet() and nterm-qt's
# generate_stylesheet() over every theme file and compares the QSS byte for
# byte, plus the loader's view of each file and the lighten/darken helpers.
#
# Gated on the same NTERMQT_SRC as the store suite, and skipped just as loudly.
# The stylesheet is the one part of the theme system with no runtime check on
# it at all: a wrong derived colour renders, it just renders wrong, so a green
# build that skipped this says nothing about whether the port still matches.

THEME_PROBE="build/theme/theme_probe"
THEME_DIR="theme/themes"

if [[ ! -x "${THEME_PROBE}" ]]; then
    echo "==> theme suite: skipped (no theme_probe -- built with -DOMEGASSH_BUILD_THEME=OFF?)"
elif [[ -z "${NTERMQT_SRC:-}" ]]; then
    echo "==> theme suite: SKIPPED -- NTERMQT_SRC is not set"
    echo "    The stylesheet port's agreement with nterm-qt is unverified in this build."
    echo "    NTERMQT_SRC=/path/to/nterm-qt ./scripts/build.sh"
elif [[ ! -f "${NTERMQT_SRC}/ntermqt/theme/stylesheet.py" ]]; then
    # Only the flattened ntermqt/ layout carries the theme package at this
    # path. An older nterm/ checkout is a skip rather than an error: it is a
    # valid oracle for the store suite above and simply cannot answer this one.
    echo "==> theme suite: SKIPPED -- no ntermqt/theme/stylesheet.py under ${NTERMQT_SRC}"
else
    PY="$(command -v python3 || command -v python || true)"
    [[ -n "${PY}" ]] || die "NTERMQT_SRC is set but no python interpreter is on PATH"
    echo "==> theme suite (against ${NTERMQT_SRC})"
    "${PY}" tests/compat/theme_differential.py \
        --probe "${THEME_PROBE}" \
        --themes "${THEME_DIR}" \
        --ntermqt "${NTERMQT_SRC}"
fi

# --- report ----------------------------------------------------------------
# Libraries and executables only. A plain glob over build/examples/*/ pulls in
# CMakeFiles/ and cmake_install.cmake, and ls then recurses into them, burying
# the handful of things actually worth running.
#
# -perm -u+x rather than -executable: the latter is GNU-only, and this script
# also runs on macOS.

echo
echo "artifacts:"
{
    ls -1 build/libomegassh.a 2>/dev/null || true
    if [[ "${BUILD_QT}" == "ON" ]]; then
        ls -1 build/qt/libomegassh_qt.a 2>/dev/null || true
    fi
    ls -1 build/sessions/libomega_sessions.a 2>/dev/null || true
    ls -1 build/sessions/compat_probe 2>/dev/null || true
    ls -1 build/theme/libomega_theme.a 2>/dev/null || true
    ls -1 build/theme/theme_probe 2>/dev/null || true
    ls -1 build/app/libomega_settings.a 2>/dev/null || true
    ls -1 build/app/libomega_shell.a 2>/dev/null || true
    ls -1 build/app/settings_probe 2>/dev/null || true
    ls -1 build/app/shell_probe 2>/dev/null || true
    ls -1 build/app/terminal_probe 2>/dev/null || true
    ls -1 build/app/hostkey_flow_probe 2>/dev/null || true
    ls -1 build/app/omega 2>/dev/null || true
    find build/examples -maxdepth 2 -type f -perm -u+x 2>/dev/null || true
} | sort | sed 's/^/  /'

if [[ "${BUILD_QT}" == "ON" && -z "${ANYTERMQT}" ]]; then
    echo
    echo "note: the app, terminal_window and theme_gallery were skipped -- no"
    echo "      anytermqt checkout found. The shell needs the terminal widget"
    echo "      now that quick connect can open one."
    echo "      ./scripts/build.sh --anytermqt /path/to/anytermqt"
fi

if [[ -x "build/app/omega" ]]; then
    echo
    echo "the shell: a window that remembers where it was."
    echo "      ./build/app/omega"
    echo "      ./build/app/omega --themes theme/themes --config /tmp/omega.json"
fi

if [[ -x "build/examples/qt/theme_gallery" ]]; then
    echo
    echo "theme gallery: every styled control against a chosen theme."
    echo "      ./build/examples/qt/theme_gallery"
    echo "      ./build/examples/qt/theme_gallery --theme dracula"
    echo
    echo "      With no display -- writes one PNG per theme and exits, which is"
    echo "      how 33 themes get reviewed in a batch rather than one at a time:"
    echo "      Xvfb :99 & DISPLAY=:99 ./build/examples/qt/theme_gallery --shot shots"
fi

echo
echo "next: sudo ./tests/labsshd.sh start   # prints the commands to run"
echo "      python3 tests/faketelnetd.py &  # then:"
echo "      ./build/examples/c/transport_probe 127.0.0.1 2323"
echo "      ./build/examples/c/transport_probe --serial /dev/ttyUSB0 9600"