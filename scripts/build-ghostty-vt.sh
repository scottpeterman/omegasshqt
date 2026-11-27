#!/usr/bin/env bash
# scripts/build-ghostty-vt.sh
#
# Builds libghostty-vt -- the optional terminal core under anytermqt -- into an
# install prefix that the bundle scripts take with --ghostty <prefix>. Linux
# and macOS; scripts\build-ghostty-vt.bat is the Windows equivalent.
#
#   scripts/build-ghostty-vt.sh --prefix ~/ghostty-vt
#   scripts/bundle-appimage.sh --ghostty ~/ghostty-vt        (or bundle-mac.sh)
#
# Options:
#   --prefix <dir>   where to install (required); gets include/, lib/ and
#                    lib/cmake/ghostty-vt, the package qtpyte's CMake finds
#   --ref <tag>      Ghostty tag to build, default GHOSTTY_VT_REF below
#   --src <dir>      checkout to use or create, default .ghostty-src
#   --zig <path>     zig binary, default zig on PATH
#
# PINNED, BOTH OF THEM. The tag is a libghostty-vt release (vt-X.Y.Z), not
# Ghostty's main: 1.0 froze the C ABI, and a release tag is what that promise
# is attached to. The Zig version is pinned because Ghostty requires an exact
# minimum (build.zig.zon) and Zig changes enough between minors to break the
# build outright. Bump them together, deliberately.
#
# BASELINE CPU. -Dcpu=baseline, always. Without it Zig optimises for the CPU
# of the machine doing the build, and on Linux Ghostty's build does not
# override that (it does on macOS, and an explicit Windows target gets a
# generic CPU anyway). A CI runner has AVX-512; a Chromebook does not, and the
# library died with "Illegal instruction" the moment a session fed it bytes.
# Baseline costs nothing measurable: Ghostty's SIMD layer picks AVX2/AVX-512
# code paths at runtime on CPUs that have them (within noise, measured).
#
# Takes about five minutes the first time. Zig caches what it can; CI caches
# the whole prefix keyed on the tag and the Zig version.
set -euo pipefail

GHOSTTY_VT_REF="vt-1.0.2"
ZIG_VERSION="0.17.1"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX=""
REF="${GHOSTTY_VT_REF}"
SRC="${ROOT}/.ghostty-src"
ZIG="zig"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix) PREFIX="$2"; shift 2 ;;
        --ref) REF="$2"; shift 2 ;;
        --src) SRC="$2"; shift 2 ;;
        --zig) ZIG="$2"; shift 2 ;;
        -h|--help) sed -n '3,22p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

die() { echo "error: $*" >&2; exit 1; }
say() { echo "==> $*"; }

[[ -n "${PREFIX}" ]] || die "--prefix <dir> is required"
command -v "${ZIG}" >/dev/null || die "no zig (${ZIG}); install Zig ${ZIG_VERSION} or pass --zig"
HAVE_ZIG="$("${ZIG}" version)"
[[ "${HAVE_ZIG}" == "${ZIG_VERSION}" ]] \
    || die "zig is ${HAVE_ZIG}; this pin wants ${ZIG_VERSION} (https://ziglang.org/download/)"
mkdir -p "${PREFIX}"
PREFIX="$(cd "${PREFIX}" && pwd)"

# A checkout of the wrong tag is replaced rather than updated: --depth 1 clones
# cannot move between tags cheaply, and a stale one is the failure worth avoiding.
if [[ -d "${SRC}/.git" ]] && [[ "$(git -C "${SRC}" describe --tags --exact-match 2>/dev/null)" != "${REF}" ]]; then
    say "${SRC} is not at ${REF}; replacing it"
    rm -rf "${SRC}"
fi
if [[ ! -d "${SRC}/.git" ]]; then
    say "cloning ghostty ${REF}"
    git clone --quiet --depth 1 --branch "${REF}" https://github.com/ghostty-org/ghostty.git "${SRC}"
fi

say "building libghostty-vt ${REF} with zig ${HAVE_ZIG} into ${PREFIX}"
(cd "${SRC}" && "${ZIG}" build -Demit-lib-vt -Demit-xcframework=false -Doptimize=ReleaseFast -Dcpu=baseline --prefix "${PREFIX}")

case "$(uname -s)" in
    Darwin) LIB="$(ls "${PREFIX}"/lib/libghostty-vt*.dylib 2>/dev/null | head -1 || true)" ;;
    *) LIB="$(ls "${PREFIX}"/lib/libghostty-vt.so.* 2>/dev/null | head -1 || true)" ;;
esac
[[ -n "${LIB}" ]] || die "no shared libghostty-vt under ${PREFIX}/lib after the build"
[[ -f "${PREFIX}/lib/cmake/ghostty-vt/ghostty-vt-config.cmake" ]] \
    || die "no CMake package config under ${PREFIX}/lib/cmake/ghostty-vt"
say "installed $(basename "${LIB}") and its CMake package"
