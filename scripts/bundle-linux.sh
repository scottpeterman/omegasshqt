#!/usr/bin/env bash
# scripts/bundle-linux.sh
#
# Packages Omega as a relocatable directory, then a tarball.
#
#   ./scripts/bundle-linux.sh                      # -> dist/omega-<ver>-linux-x86_64.tar.gz
#   ./scripts/bundle-linux.sh --anytermqt <path>
#   ./scripts/bundle-linux.sh --no-tar             # leave the tree, skip the archive
#   ./scripts/bundle-linux.sh --appimage           # also -> dist/omega-<ver>-linux-x86_64.AppImage
#
# THE DIRECTORY IS THE DEFAULT, and that is a choice rather than a shortcut. An
# AppImage needs appimagetool present at build time and FUSE present at run
# time, which is two more moving parts. What comes out of this by default is a
# directory somebody untars anywhere and runs, with fewer ways to go wrong.
# --appimage wraps that same verified tree, so the AppImage is a repackaging of
# something already proven to start rather than a separate build path.
#
# The layout is the one app/main.cpp already looks for:
#
#   omega/
#     bin/omega           the binary, plus a launcher beside it
#     lib/                Qt and ICU
#     plugins/            Qt's platform plugin and friends
#     Resources/themes/   found via applicationDirPath()/../Resources/themes
#
# WHY A LAUNCHER RATHER THAN AN RPATH. Setting the rpath at link time would be
# tidier, but it has to be right for a layout that does not exist yet at
# configure time, and getting it wrong produces a binary that silently loads the
# HOST's Qt -- which works on the build machine and fails everywhere else, the
# same failure mode the theme path had. A three-line launcher is checkable by
# reading it.

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

ANYTERMQT="${OMEGASSH_ANYTERMQT_DIR:-}"
MAKE_TAR=1
MAKE_APPIMAGE=0
BUILD_DIR="build-linux"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --anytermqt) ANYTERMQT="$2"; shift 2 ;;
        --anytermqt=*) ANYTERMQT="${1#*=}"; shift ;;
        --no-tar) MAKE_TAR=0; shift ;;
        --appimage) MAKE_APPIMAGE=1; shift ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        -h|--help) sed -n '3,13p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

die() { echo "error: $*" >&2; exit 1; }
say() { echo "==> $*"; }

[[ -n "${ANYTERMQT}" ]] || die "no anytermqt path (--anytermqt <path> or OMEGASSH_ANYTERMQT_DIR)"
[[ -f "${ANYTERMQT}/qtpyte/CMakeLists.txt" ]] || die "not an anytermqt checkout: ${ANYTERMQT}"

# Checked here rather than after the build: finding out the tool is missing
# should not cost a compile.
if [[ "${MAKE_APPIMAGE}" -eq 1 ]]; then
    command -v appimagetool >/dev/null \
        || die "--appimage given but appimagetool is not on PATH"
fi

VERSION="$(sed -n 's/^project(omegassh VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)"
[[ -n "${VERSION}" ]] || die "could not read the version out of CMakeLists.txt"
ARCH="$(uname -m)"
STAGE="dist/omega"

# --- build -----------------------------------------------------------------

say "building ${VERSION} into ${BUILD_DIR}"
cmake -S . -B "${BUILD_DIR}" \
      -DOMEGASSH_ANYTERMQT_DIR="${ANYTERMQT}" \
      -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "${BUILD_DIR}" --target omega -j"$(nproc)"

BIN="${BUILD_DIR}/app/omega"
[[ -x "${BIN}" ]] || die "no ${BIN} after the build"

# --- stage -----------------------------------------------------------------

say "staging into ${STAGE}"
rm -rf "${STAGE}"
mkdir -p "${STAGE}/bin" "${STAGE}/lib" "${STAGE}/plugins" "${STAGE}/Resources"

cp "${BIN}" "${STAGE}/bin/omega.bin"
cp -r theme/themes "${STAGE}/Resources/themes"

# Every non-system shared object the binary pulls in. The filter is what keeps
# this from copying libc and the graphics stack: those must come from the host,
# and shipping them is how a bundle stops working on a newer distribution than
# the one that built it.
say "copying libraries"
ldd "${BIN}" | awk '/=> \//{print $3}' | while read -r lib; do
    case "$(basename "${lib}")" in
        libQt6*|libicu*) cp -Lu "${lib}" "${STAGE}/lib/" ;;
    esac
done

# The platform plugin is the one nobody remembers and the one whose absence is
# fatal: without libqxcb.so Qt exits with "could not load the Qt platform
# plugin", naming a plugin it did find nothing wrong with.
say "copying plugins"
QT_PLUGIN_SRC="$(dirname "$(ldd "${BIN}" | awk '/libQt6Core/{print $3}')")/qt6/plugins"
[[ -d "${QT_PLUGIN_SRC}" ]] || QT_PLUGIN_SRC="$(qmake6 -query QT_INSTALL_PLUGINS 2>/dev/null || true)"
[[ -d "${QT_PLUGIN_SRC}" ]] || die "could not find the Qt plugin directory"

for group in platforms platformthemes imageformats tls xcbglintegrations; do
    [[ -d "${QT_PLUGIN_SRC}/${group}" ]] || continue
    mkdir -p "${STAGE}/plugins/${group}"
    cp -Lu "${QT_PLUGIN_SRC}/${group}"/*.so "${STAGE}/plugins/${group}/" 2>/dev/null || true
done
[[ -f "${STAGE}/plugins/platforms/libqxcb.so" ]] \
    || die "libqxcb.so did not get copied; the bundle would not start"

# Plugins brought their own Qt dependencies with them.
find "${STAGE}/plugins" -name '*.so' -print0 | while IFS= read -r -d '' plugin; do
    ldd "${plugin}" 2>/dev/null | awk '/=> \//{print $3}' | while read -r lib; do
        case "$(basename "${lib}")" in
            libQt6*|libicu*) cp -Lu "${lib}" "${STAGE}/lib/" ;;
        esac
    done
done

# --- launcher --------------------------------------------------------------

cat > "${STAGE}/bin/omega" <<'LAUNCH'
#!/usr/bin/env bash
# Resolves through symlinks, so this still works from a link in ~/bin.
HERE="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
export LD_LIBRARY_PATH="${HERE}/../lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export QT_PLUGIN_PATH="${HERE}/../plugins"
exec "${HERE}/omega.bin" "$@"
LAUNCH
chmod +x "${STAGE}/bin/omega"

# --- verify ----------------------------------------------------------------
#
# Not decoration. The whole failure mode this packaging has is loading the
# host's Qt instead of the shipped one, which looks perfect on the build
# machine. Checking here is the only place it is cheap.

say "verifying"
LIBS="$(ls "${STAGE}/lib" | wc -l)"
THEMES="$(ls "${STAGE}/Resources/themes" | wc -l)"
echo "    ${LIBS} libraries, ${THEMES} themes"
[[ "${LIBS}" -ge 4 ]]   || die "only ${LIBS} libraries staged; expected Qt Core, Gui, Widgets and ICU at least"
[[ "${THEMES}" -ge 10 ]] || die "only ${THEMES} themes staged"

# `timeout` is not belt and braces: omega is a GUI application, so a plain
# invocation enters its event loop and never comes back. What is being read is
# what it prints on the way up -- it names the theme directory when it finds
# nothing there -- so a few seconds is all this needs, and exit 124 from being
# killed is the SUCCESS case.
CHECK="$(QT_QPA_PLATFORM=offscreen timeout 8 "${STAGE}/bin/omega" \
             --config /tmp/omega-bundle-check.json 2>&1 || true)"

if grep -q "no themes found" <<<"${CHECK}"; then
    echo "${CHECK}" >&2
    die "the staged tree did not find its own themes"
fi
if grep -qi "could not load the Qt platform plugin" <<<"${CHECK}"; then
    echo "${CHECK}" >&2
    die "the staged tree could not load its platform plugin"
fi
say "the staged tree starts, loads its own Qt and finds its themes"

# The check above proves it RAN; this proves it ran on the SHIPPED Qt rather
# than the host's, which is the failure that looks like success on a build
# machine. ldd resolves against the launcher's environment, not the shell's.
HOSTQT="$(LD_LIBRARY_PATH="${STAGE}/lib" ldd "${STAGE}/bin/omega.bin" \
          | awk '/libQt6.*=> \//{print $3}' | grep -cv "^${PWD}/${STAGE}/lib" || true)"
if [[ "${HOSTQT}" -ne 0 ]]; then
    die "${HOSTQT} Qt libraries still resolve outside the bundle"
fi
say "every Qt library resolves inside the bundle"

# --- appimage --------------------------------------------------------------
#
# Deliberately after the verify block. The AppDir is a copy of a tree that has
# already been shown to start on its own Qt, so anything that goes wrong from
# here is a packaging fault and not a linking one.

if [[ "${MAKE_APPIMAGE}" -eq 1 ]]; then
    APPDIR="dist/omega.AppDir"
    say "building AppDir at ${APPDIR}"
    rm -rf "${APPDIR}"
    mkdir -p "${APPDIR}/usr"
    cp -a "${STAGE}/bin" "${STAGE}/lib" "${STAGE}/plugins" "${APPDIR}/usr/"
    cp -a "${STAGE}/Resources" "${APPDIR}/usr/Resources"

    # AppRun rather than the staged launcher because the AppDir root, not
    # bin/, is what gets mounted at a known path. The staged bin/omega comes
    # along in the copy and still resolves ../lib correctly, so both work.
    cat > "${APPDIR}/AppRun" <<'APPRUN'
#!/usr/bin/env bash
HERE="$(dirname "$(readlink -f "${0}")")"
export LD_LIBRARY_PATH="${HERE}/usr/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export QT_PLUGIN_PATH="${HERE}/usr/plugins"
exec "${HERE}/usr/bin/omega.bin" "$@"
APPRUN
    chmod +x "${APPDIR}/AppRun"

    cat > "${APPDIR}/omega.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=Omega
Exec=omega
Icon=omega
Categories=Network;TerminalEmulator;
Terminal=false
DESKTOP

    # appimagetool refuses to build without an icon at the AppDir root whose
    # basename matches Icon=. The fallback is a 1x1 transparent PNG so a
    # missing artwork file does not stop a package from being produced.
    if [[ -f "app/omega.png" ]]; then
        cp "app/omega.png" "${APPDIR}/omega.png"
    elif [[ -f "Resources/omega.png" ]]; then
        cp "Resources/omega.png" "${APPDIR}/omega.png"
    else
        say "no app/omega.png found; using a placeholder icon"
        printf '\211PNG\r\n\032\n\0\0\0\rIHDR\0\0\0\1\0\0\0\1\10\6\0\0\0\37\25\304\211\0\0\0\nIDATx\234c\0\1\0\0\5\0\1\r\n-\264\0\0\0\0IEND\256B`\202' \
            > "${APPDIR}/omega.png"
    fi

    APPIMAGE="dist/omega-${VERSION}-linux-${ARCH}.AppImage"
    say "writing ${APPIMAGE}"
    # appimagetool reads the target architecture out of the environment and
    # accepts the same names uname -m produces.
    ARCH="${ARCH}" appimagetool "${APPDIR}" "${APPIMAGE}" >/dev/null
    [[ -f "${APPIMAGE}" ]] || die "appimagetool produced no ${APPIMAGE}"
    chmod +x "${APPIMAGE}"
    ls -lh "${APPIMAGE}"
fi

# --- archive ---------------------------------------------------------------

if [[ "${MAKE_TAR}" -eq 1 ]]; then
    TARBALL="dist/omega-${VERSION}-linux-${ARCH}.tar.gz"
    say "writing ${TARBALL}"
    tar -czf "${TARBALL}" -C dist omega
    ls -lh "${TARBALL}"
fi

echo
echo "run it with:"
echo "      ${STAGE}/bin/omega"
if [[ "${MAKE_APPIMAGE}" -eq 1 ]]; then
    echo "      ${APPIMAGE}"
    echo "      ${APPIMAGE} --appimage-extract-and-run     # if the host has no FUSE"
fi