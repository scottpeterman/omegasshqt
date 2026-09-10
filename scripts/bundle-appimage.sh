#!/usr/bin/env bash
# scripts/bundle-appimage.sh
#
# Packages Omega as an AppImage: one executable file, no install, runs on any
# reasonably current distribution.
#
#   ./scripts/bundle-appimage.sh --anytermqt ../anytermqt
#   ./scripts/bundle-appimage.sh --anytermqt ../anytermqt --keep-appdir
#
# THIS IS THE OTHER LINUX PACKAGE, not a replacement for bundle-linux.sh. The
# tarball needs no tooling and no network; this needs linuxdeploy downloaded
# from GitHub, and gives back a single file with a desktop entry and an icon
# that a desktop environment will pick up. Which one to hand somebody depends
# on whether they would rather untar a directory or chmod +x one file.
#
# THE DOWNLOAD IS THE CATCH. linuxdeploy and its Qt plugin are fetched at build
# time from github.com, so this does not work offline and the "continuous"
# release it pulls is a moving target. Set LINUXDEPLOY_DIR to a directory
# holding both AppImages to use copies you already have.
#
# THE LAYOUT IS THE ONE app/main.cpp ALREADY LOOKS FOR. The binary lands at
# AppDir/usr/bin/omega, so ../Resources/themes is AppDir/usr/Resources/themes
# and nothing in the application had to change to be packaged this way.

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

ANYTERMQT="${OMEGASSH_ANYTERMQT_DIR:-}"
BUILD_DIR="build-linux"
KEEP_APPDIR=0
LD_DIR="${LINUXDEPLOY_DIR:-}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --anytermqt) ANYTERMQT="$2"; shift 2 ;;
        --anytermqt=*) ANYTERMQT="${1#*=}"; shift ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --keep-appdir) KEEP_APPDIR=1; shift ;;
        -h|--help) sed -n '3,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

die() { echo "error: $*" >&2; exit 1; }
say() { echo "==> $*"; }

if [[ -z "${ANYTERMQT}" ]]; then
    for candidate in "${ROOT}/../anytermqt" "${HOME}/github/anytermqt"; do
        [[ -f "${candidate}/qtpyte/CMakeLists.txt" ]] && { ANYTERMQT="$(cd "${candidate}" && pwd)"; break; }
    done
fi
[[ -n "${ANYTERMQT}" ]] || die "no anytermqt path (--anytermqt <path> or OMEGASSH_ANYTERMQT_DIR)"
[[ -f "${ANYTERMQT}/qtpyte/CMakeLists.txt" ]] || die "not an anytermqt checkout: ${ANYTERMQT}"

# The same check bundle-windows.bat makes, for the same reason: anti-idle calls
# qtpyte::TerminalWidget::alternateScreen(), and an older anytermqt fails with
# an error naming a class in THIS repo for a method that lives in the other one.
grep -q alternateScreen "${ANYTERMQT}/qtpyte/include/qtpyte/terminalwidget.h" \
    || die "anytermqt at ${ANYTERMQT} is out of date (no alternateScreen); git pull it"

VERSION="$(sed -n 's/^project(omegassh VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)"
[[ -n "${VERSION}" ]] || die "could not read the version out of CMakeLists.txt"

# --- linuxdeploy -----------------------------------------------------------

if [[ -z "${LD_DIR}" ]]; then
    LD_DIR="${ROOT}/.linuxdeploy"
    mkdir -p "${LD_DIR}"
fi

fetch() {
    local name="$1" url="$2"
    if [[ -x "${LD_DIR}/${name}" ]]; then
        say "using ${LD_DIR}/${name}"
        return
    fi
    say "downloading ${name}"
    curl -sSfL -o "${LD_DIR}/${name}" "${url}" \
        || die "could not download ${name}. No network? Set LINUXDEPLOY_DIR to a directory holding it."
    chmod +x "${LD_DIR}/${name}"
}

fetch linuxdeploy-x86_64.AppImage \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
fetch linuxdeploy-plugin-qt-x86_64.AppImage \
    "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage"

# linuxdeploy is itself an AppImage, so it needs FUSE. Containers and minimal
# CI images often have none, and the error it gives is about a missing library
# rather than about FUSE. --appimage-extract-and-run sidesteps it entirely at
# the cost of unpacking to a temporary directory each run.
export APPIMAGE_EXTRACT_AND_RUN=1

# The Qt plugin finds Qt through qmake. Naming it beats letting the plugin
# guess, for the same reason the Windows script takes windeployqt from the Qt
# prefix: the deploy tool has to match the Qt the binary linked against.
if [[ -z "${QMAKE:-}" ]]; then
    QMAKE="$(command -v qmake6 || command -v qmake || true)"
fi
[[ -n "${QMAKE}" ]] || die "no qmake6 on PATH; set QMAKE to your Qt's qmake"
export QMAKE
say "Qt via ${QMAKE}"

# --- build -----------------------------------------------------------------

say "building ${VERSION}"
cmake -S . -B "${BUILD_DIR}" \
      -DOMEGASSH_ANYTERMQT_DIR="${ANYTERMQT}" \
      -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "${BUILD_DIR}" --target omega -j"$(nproc)"

BIN="${BUILD_DIR}/app/omega"
[[ -x "${BIN}" ]] || die "no ${BIN} after the build"

# --- AppDir ----------------------------------------------------------------

APPDIR="${ROOT}/AppDir"
say "assembling ${APPDIR}"
rm -rf "${APPDIR}"
mkdir -p "${APPDIR}/usr/bin" "${APPDIR}/usr/Resources"

cp "${BIN}" "${APPDIR}/usr/bin/omega"
cp -r theme/themes "${APPDIR}/usr/Resources/themes"
cp packaging/linux/omega.desktop "${APPDIR}/omega.desktop"
cp art/omega-icon-256.png "${APPDIR}/omega.png"

# --- pack ------------------------------------------------------------------

say "running linuxdeploy"
mkdir -p dist
OUTPUT="dist/Omega-${VERSION}-x86_64.AppImage" \
"${LD_DIR}/linuxdeploy-x86_64.AppImage" \
    --appdir "${APPDIR}" \
    --plugin qt \
    --desktop-file "${APPDIR}/omega.desktop" \
    --icon-file "${APPDIR}/omega.png" \
    --output appimage

APPIMAGE="$(ls -t dist/Omega*.AppImage 2>/dev/null | head -1 || true)"
[[ -n "${APPIMAGE}" ]] || {
    APPIMAGE="$(ls -t Omega*.AppImage 2>/dev/null | head -1 || true)"
    [[ -n "${APPIMAGE}" ]] || die "linuxdeploy produced no AppImage"
    mv "${APPIMAGE}" "dist/Omega-${VERSION}-x86_64.AppImage"
    APPIMAGE="dist/Omega-${VERSION}-x86_64.AppImage"
}

# --- verify ----------------------------------------------------------------
#
# Only what can be checked without a display. The AppImage bundles the xcb
# platform plugin and NOT offscreen -- correct for something people run on a
# desktop, and it means the headless smoke test the tarball script does is not
# available here. Run it under Xvfb if you want that:
#
#   Xvfb :99 -screen 0 1280x900x24 & DISPLAY=:99 ./dist/Omega-*.AppImage

say "verifying"
"${APPIMAGE}" --appimage-extract >/dev/null 2>&1 || die "the AppImage will not extract"
THEMES="$(ls squashfs-root/usr/Resources/themes 2>/dev/null | wc -l)"
PLUGIN="squashfs-root/usr/plugins/platforms/libqxcb.so"
[[ -f "${PLUGIN}" ]] || PLUGIN="$(find squashfs-root -name libqxcb.so | head -1)"
rm -rf squashfs-root

[[ "${THEMES}" -ge 10 ]] || die "only ${THEMES} themes inside the AppImage"
[[ -n "${PLUGIN}" ]] || die "no xcb platform plugin inside the AppImage; it would not start"
say "${THEMES} themes and the xcb platform plugin are inside"

[[ "${KEEP_APPDIR}" -eq 1 ]] || rm -rf "${APPDIR}"

ls -lh "${APPIMAGE}"
echo
echo "run it with:"
echo "      ${APPIMAGE}"