#!/usr/bin/env bash
# scripts/bundle-mac.sh
#
# Packages Omega as Omega.app, deployed with macdeployqt.
#
#   ./scripts/bundle-mac.sh --anytermqt <path>
#   ./scripts/bundle-mac.sh --anytermqt <path> --sign "Developer ID Application: ..."
#   ./scripts/bundle-mac.sh --anytermqt <path> --dmg
#
# arm64 only. A universal binary would need the Go c-archive built twice and
# lipo'd, and the CMake side has no seam for that; every Mac sold since 2020 is
# arm64.
#
# NOT VERIFIED ON A MAC. This was written on Linux against the documented
# behaviour of iconutil, macdeployqt and codesign, and every step prints what it
# is doing so a wrong one is visible rather than silent. Treat the first run as
# the test.
#
# THE ORDER MATTERS AND IS NOT OBVIOUS:
#
#   1. iconutil, BEFORE cmake. app/CMakeLists.txt tests EXISTS on the .icns at
#      CONFIGURE time, so an icns built afterwards is not in the bundle until
#      the next configure -- and the bundle looks fine apart from a generic icon.
#   2. cmake configure and build. The themes and the icns go in as a POST_BUILD
#      step, so the .app in the build tree is already complete.
#   3. macdeployqt. Copies the Qt frameworks and rewrites install names.
#   4. codesign, LAST. macdeployqt rewrites binaries, which invalidates any
#      signature already on them.

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

ANYTERMQT="${OMEGASSH_ANYTERMQT_DIR:-}"
BUILD_DIR="build-mac"
SIGN_ID=""
MAKE_DMG=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --anytermqt) ANYTERMQT="$2"; shift 2 ;;
        --anytermqt=*) ANYTERMQT="${1#*=}"; shift ;;
        --sign) SIGN_ID="$2"; shift 2 ;;
        --dmg) MAKE_DMG=1; shift ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        -h|--help) sed -n '3,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

die() { echo "error: $*" >&2; exit 1; }
say() { echo "==> $*"; }

[[ "$(uname -s)" == "Darwin" ]] || die "this one is macOS only; see bundle-linux.sh"
[[ -n "${ANYTERMQT}" ]] || die "no anytermqt path (--anytermqt <path> or OMEGASSH_ANYTERMQT_DIR)"
[[ -f "${ANYTERMQT}/qtpyte/CMakeLists.txt" ]] || die "not an anytermqt checkout: ${ANYTERMQT}"

VERSION="$(sed -n 's/^project(omegassh VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)"
[[ -n "${VERSION}" ]] || die "could not read the version out of CMakeLists.txt"

# --- 1. the icon -----------------------------------------------------------

if [[ -d art/Omega.iconset ]]; then
    say "building art/Omega.icns"
    iconutil -c icns art/Omega.iconset -o art/Omega.icns
else
    echo "warning: no art/Omega.iconset; bundling without an icon" >&2
fi

# --- 2. build --------------------------------------------------------------

say "building ${VERSION} into ${BUILD_DIR}"
cmake -S . -B "${BUILD_DIR}" \
      -DOMEGA_MACOS_BUNDLE=ON \
      -DOMEGASSH_ANYTERMQT_DIR="${ANYTERMQT}" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES=arm64
# CHECKED HERE, BEFORE THE BUILD, and not after it. The option only exists in
# app/CMakeLists.txt from the packaging change onwards, and CMake does not
# complain about a -D nobody reads -- so a tree without it configures happily,
# builds a plain executable, and only fails minutes later with "no Omega.app",
# which reads as a macdeployqt problem and is not one.
if ! grep -q "^OMEGA_MACOS_BUNDLE:BOOL=ON" "${BUILD_DIR}/CMakeCache.txt"; then
    echo >&2
    echo "OMEGA_MACOS_BUNDLE did not reach CMake." >&2
    echo >&2
    if ! grep -q "OMEGA_MACOS_BUNDLE" app/CMakeLists.txt; then
        echo "  app/CMakeLists.txt has no OMEGA_MACOS_BUNDLE option in it, so" >&2
        echo "  this tree predates the packaging change. Update app/CMakeLists.txt" >&2
        echo "  and app/main.cpp, then run this again." >&2
    else
        echo "  app/CMakeLists.txt does define it, so something is overriding" >&2
        echo "  the -D. Try a clean configure: rm -rf ${BUILD_DIR}" >&2
    fi
    exit 1
fi

cmake --build "${BUILD_DIR}" --target omega -j"$(sysctl -n hw.ncpu)"

APP="${BUILD_DIR}/app/Omega.app"
[[ -d "${APP}" ]] || die "no ${APP} after the build"

# The post-build steps are what put these in. Checking here rather than after
# macdeployqt keeps the two failures apart: a missing theme is a CMake problem,
# a missing framework is a macdeployqt problem, and they look alike at the end.
[[ -d "${APP}/Contents/Resources/themes" ]] || die "themes did not reach the bundle"
say "themes: $(ls "${APP}/Contents/Resources/themes" | wc -l | tr -d ' ') files"

# --- 3. macdeployqt --------------------------------------------------------
#
# macdeployqt is not reliably idempotent: it rewrites install names in place,
# and running it twice over the same bundle can leave paths pointing at
# themselves. The build directory is therefore treated as disposable rather
# than incremental for packaging.

MACDEPLOYQT="$(command -v macdeployqt || true)"
if [[ -z "${MACDEPLOYQT}" ]]; then
    QTBIN="$(qmake6 -query QT_INSTALL_BINS 2>/dev/null || qmake -query QT_INSTALL_BINS 2>/dev/null || true)"
    [[ -n "${QTBIN}" && -x "${QTBIN}/macdeployqt" ]] \
        || die "macdeployqt not found; put your Qt's bin on PATH"
    MACDEPLOYQT="${QTBIN}/macdeployqt"
fi
# Which Qt this is, said before it matters. macdeployqt must come from the SAME
# Qt the application linked against: a Homebrew macdeployqt over an aqtinstall
# build (or the reverse) copies frameworks that do not match the install names
# in the binary, and the result runs here and fails on a clean Mac.
say "deploying with ${MACDEPLOYQT}"
otool -L "${APP}/Contents/MacOS/Omega" | awk '/QtCore/{print "    linked against: " $1}'

"${MACDEPLOYQT}" "${APP}" -verbose=1

# --- 4. verify -------------------------------------------------------------
#
# The failure this packaging has is a framework that did not get its install
# name rewritten: the app runs perfectly on the machine that built it and dies
# on a clean one. This is the only place that is cheap to catch.

say "verifying"
BIN="${APP}/Contents/MacOS/Omega"
STRAY="$(otool -L "${BIN}" | awk '/Qt/{print $1}' | grep -v '@executable_path' || true)"
if [[ -n "${STRAY}" ]]; then
    echo "${STRAY}" >&2
    die "Qt libraries above still resolve outside the bundle"
fi
say "every Qt reference in the binary is @executable_path"

[[ -d "${APP}/Contents/PlugIns/platforms" ]] \
    || die "no platform plugin in the bundle; it would not start on a clean Mac"

say "architecture: $(lipo -archs "${BIN}")"

# --- 5. sign ---------------------------------------------------------------

if [[ -n "${SIGN_ID}" ]]; then
    say "signing with ${SIGN_ID}"
    codesign --force --options runtime --timestamp \
             --sign "${SIGN_ID}" --deep "${APP}"
    codesign --verify --deep --strict --verbose=2 "${APP}"
    echo
    echo "signed, but NOT notarized. For distribution outside your own machine:"
    echo "      xcrun notarytool submit <zip> --keychain-profile <profile> --wait"
    echo "      xcrun stapler staple ${APP}"
else
    say "ad-hoc signing (local use only)"
    codesign --force --deep --sign - "${APP}"
fi

# --- 6. dmg ----------------------------------------------------------------

if [[ "${MAKE_DMG}" -eq 1 ]]; then
    mkdir -p dist
    DMG="dist/Omega-${VERSION}-arm64.dmg"
    say "writing ${DMG}"
    rm -f "${DMG}"
    hdiutil create -volname "Omega ${VERSION}" -srcfolder "${APP}" \
                   -ov -format UDZO "${DMG}"
    ls -lh "${DMG}"
fi

echo
echo "open it with:"
echo "      open ${APP}"
