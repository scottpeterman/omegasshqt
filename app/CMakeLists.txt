# app/CMakeLists.txt
#
# The shell. A library plus a thin entry point, so the window can be built and
# driven without one -- the same arrangement sessions/ and theme/ have, and the
# reason settings_probe can exercise the settings layer with no display.
#
# omega_settings is QtCore only. The window needs QtWidgets; nothing here needs
# the Go archive, so the shell builds and runs before any of it connects to
# anything.

find_package(Qt6 REQUIRED COMPONENTS Core Widgets)

# --- settings: QtCore, no widgets ------------------------------------------
# QtCore only, both of them. antiidle and omegasettings sit here rather than in
# the shell for the same reason settings does: the differential and the probes
# reach them with no display and no widgets in the link.
add_library(omega_settings STATIC
    antiidle.cpp
    antiidle.h
    omegasettings.cpp
    omegasettings.h
    settings.cpp
    settings.h
    tabclose.cpp
    tabclose.h)

target_include_directories(omega_settings PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(omega_settings PUBLIC Qt6::Core)
set_target_properties(omega_settings PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)
add_library(omega::settings ALIAS omega_settings)

# --- settings resolution ---------------------------------------------------
# Its own target rather than part of the shell: it needs the transport Config
# and the session store but no widgets and no display, which is what lets
# session_attrs_probe link it and check every inheritance branch headless.
add_library(omega_resolve STATIC
    effectiveconfig.cpp
    effectiveconfig.h)

target_include_directories(omega_resolve PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(omega_resolve PUBLIC
    omega::settings omega::sessions omegassh::qt)
set_target_properties(omega_resolve PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)
add_library(omega::resolve ALIAS omega_resolve)

# --- the shell window ------------------------------------------------------
# The shell needs the terminal widget and the transport now that quick connect
# exists, so anytermqt stops being optional for the application -- it is still
# optional for everything else in the repo. omegassh::qt brings the Go archive
# with it, which is why the app was worth building without either for as long
# as it could be.
add_library(omega_shell STATIC
    sessionribbon.cpp
    sessionribbon.h
    statusdot.cpp
    statusdot.h
    linkstate.h
    livesessions.cpp
    livesessions.h
    sessiondelegate.cpp
    sessiondelegate.h
    chromebar.cpp
    chromebar.h
    sizegrip.cpp
    sizegrip.h
    windowbuttons.cpp
    windowbuttons.h
    capturewriter.cpp
    capturewriter.h
    credentialeditordialog.cpp
    credentialeditordialog.h
    autherror.cpp
    autherror.h
    kbdprompt.cpp
    kbdprompt.h
    keyboardpromptdialog.cpp
    keyboardpromptdialog.h
    hostkeyerror.cpp
    hostkeyerror.h
    hostkeypromptdialog.cpp
    hostkeypromptdialog.h
    credentialmanagerdialog.cpp
    credentialmanagerdialog.h
    dialogbuttons.cpp
    dialogbuttons.h
    credentialpromptdialog.cpp
    credentialpromptdialog.h
    helpdialog.cpp
    helpdialog.h
    mainwindow.cpp
    mainwindow.h
    pasteconfirmdialog.cpp
    pasteconfirmdialog.h
    pastequeue.cpp
    pastequeue.h
    quickconnectdialog.cpp
    quickconnectdialog.h
    sessioneditordialog.cpp
    sessioneditordialog.h
    sessionfilterproxy.cpp
    sessionfilterproxy.h
    sessiontransfer.cpp
    sessiontransfer.h
    sessiontreemodel.cpp
    sessiontreemodel.h
    sessiontreepane.cpp
    sessiontreepane.h
    settingsdialog.cpp
    settingsdialog.h
    spinarrowstyle.cpp
    spinarrowstyle.h
    vaultunlockdialog.cpp
    vaultunlockdialog.h
    terminaltab.cpp
    terminaltab.h
    terminalview.cpp
    terminalview.h
    modalframe.cpp
    modalframe.h
    currenttokens.cpp
    currenttokens.h

    omega.qrc)

target_include_directories(omega_shell PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${CMAKE_CURRENT_SOURCE_DIR}/../integration")
# omega::sessions joins the shell at 4d: the tree pane owns a model over the
# store, so the window can no longer be built without it.
target_link_libraries(omega_shell PUBLIC
    omega::settings omega::theme omega::sessions omega::resolve omegassh::qt
    qtpyte::core Qt6::Widgets)
set_target_properties(omega_shell PROPERTIES
    AUTOMOC ON
    AUTORCC ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)
# The version the About box reports, from the CMake project version. Defined on
# the library rather than on the executable because helpdialog.cpp is in the
# library -- and stated once here rather than as a literal in the source, since
# a version that has to be edited in two places is one that will disagree with
# itself.
target_compile_definitions(omega_shell PRIVATE
    OMEGA_VERSION="${PROJECT_VERSION}")

# The version the About box reports, from the CMake project version. Defined on
# the library rather than on the executable because helpdialog.cpp is in the
# library -- and stated once here rather than as a literal in the source, since
# a version that has to be edited in two places is one that will disagree with
# itself.
target_compile_definitions(omega_shell PRIVATE
    OMEGA_VERSION="${PROJECT_VERSION}")

add_library(omega::shell ALIAS omega_shell)

# --- the application -------------------------------------------------------
add_executable(omega main.cpp)
target_link_libraries(omega PRIVATE omega::shell)
target_compile_definitions(omega PRIVATE
    OMEGA_THEME_DIR_DEFAULT="${OMEGA_THEME_DIR}")
set_target_properties(omega PROPERTIES
    AUTOMOC ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)

# --- Windows subsystem -----------------------------------------------------
#
# Without this the linker produces a CONSOLE subsystem binary, so Windows
# allocates a console before the GUI appears: a black window flashes up and
# then sits behind the application for as long as it runs. Launching from a
# Developer Command Prompt hides the symptom, because a console is already
# there -- it shows up the moment anybody double-clicks the exe from Explorer
# or launches it from the Start menu, which is to say the moment it is
# packaged.
#
# Only on `omega`. The probes are console programs that print check results and
# are read from a shell, and a GUI subsystem would silently discard every line
# they write.
#
# THE TRADE: a WIN32_EXECUTABLE has no stdout or stderr attached. main.cpp's
# diagnostics -- notably "no themes found in <dir>", which is how a packaging
# mistake announces itself -- go nowhere on Windows. Run the exe from a shell
# with a redirect, or use --themes to force a path, when that matters.
if(WIN32)
    set_target_properties(omega PROPERTIES WIN32_EXECUTABLE ON)

    # Qt supplies the WinMain that calls main(), so main() stays portable and
    # nothing in the source changes. Qt6::Core normally brings this in through
    # its usage requirements; naming it is belt and braces against a link
    # failure on unresolved WinMain, and harmless when it is already there.
    if(TARGET Qt6::EntryPointPrivate)
        target_link_libraries(omega PRIVATE Qt6::EntryPointPrivate)
    endif()
endif()

# --- macOS bundle ----------------------------------------------------------
#
# OFF by default, and deliberately: turning it on moves the executable from
# build/app/omega to build/app/Omega.app/Contents/MacOS/Omega, which is where
# scripts/build.sh looks and where every note in docs/ points. The development
# loop should not change shape because packaging exists.
#
# Packaging is therefore a separate configure:
#
#   cmake -S . -B build-mac -DOMEGA_MACOS_BUNDLE=ON \
#         -DOMEGASSH_ANYTERMQT_DIR=../anytermqt -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-mac -j
#   $QT_ROOT/bin/macdeployqt build-mac/app/Omega.app
#
# macdeployqt copies the Qt frameworks in and rewrites their install names. It
# does NOT copy application resources, which is why the themes are put in by
# the post-build step below rather than left for it.
option(OMEGA_MACOS_BUNDLE
       "Build omega as a macOS .app bundle, for macdeployqt" OFF)

if(APPLE AND OMEGA_MACOS_BUNDLE)
    set_target_properties(omega PROPERTIES
        MACOSX_BUNDLE ON
        OUTPUT_NAME "Omega"
        MACOSX_BUNDLE_BUNDLE_NAME "Omega"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.scottpeterman.omega"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_COPYRIGHT "")

    # The icon, if it has been generated. art/ ships the .iconset; the .icns
    # itself is built from it with iconutil, which is macOS-only, so a checkout
    # that has not run that step still configures and builds -- with a default
    # icon rather than a configure error.
    set(OMEGA_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/../art/Omega.icns")
    if(EXISTS "${OMEGA_ICNS}")
        set_target_properties(omega PROPERTIES
            MACOSX_BUNDLE_ICON_FILE "Omega.icns")
        add_custom_command(TARGET omega POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy "${OMEGA_ICNS}"
                    "$<TARGET_BUNDLE_CONTENT_DIR:omega>/Resources/Omega.icns"
            COMMENT "omega: copying Omega.icns into the bundle"
            VERBATIM)
    else()
        message(STATUS
            "omega: no ${OMEGA_ICNS} -- bundling without an icon "
            "(build it with: iconutil -c icns art/Omega.iconset -o art/Omega.icns)")
    endif()

    # At BUILD time, not install time. The thing macdeployqt is pointed at is
    # build-mac/app/Omega.app, so that bundle has to be complete where it is
    # built -- an install() rule would leave the themes out of the only copy
    # anybody runs.
    add_custom_command(TARGET omega POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${OMEGA_THEME_DIR}"
                "$<TARGET_BUNDLE_CONTENT_DIR:omega>/Resources/themes"
        COMMENT "omega: copying themes into Omega.app/Contents/Resources"
        VERBATIM)
endif()

option(OMEGA_APP_BUILD_PROBE
       "Build settings_probe, the dumper the differential test drives" ON)

if(OMEGA_APP_BUILD_PROBE)
    add_executable(settings_probe
        "${CMAKE_CURRENT_SOURCE_DIR}/../tests/compat/settings_probe.cpp")
    target_link_libraries(settings_probe PRIVATE omega::settings)
    set_target_properties(settings_probe PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON)

    # The token ramp against QColor. Lives here rather than beside theme_probe
    # because it is the one thing in the theme system that needs Qt, and
    # theme/CMakeLists.txt configuring without Qt is a property worth keeping
    # -- see the note at the top of that file. Qt6::Gui only: QColor is a GUI
    # type but not a widget, so this runs with no display and no platform
    # plugin.
    add_executable(token_differential
        "${CMAKE_CURRENT_SOURCE_DIR}/../tests/compat/token_differential.cpp")
    target_link_libraries(token_differential PRIVATE omega::theme Qt6::Gui)
    set_target_properties(token_differential PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON)

    # The per-session attribute columns and the resolution that reads them.
    # Links omega::resolve rather than omega::shell: no widgets, no display,
    # no vault -- the resolver is a pure function and the store is a file.
    add_executable(session_attrs_probe
        "${CMAKE_CURRENT_SOURCE_DIR}/../tests/compat/session_attrs_probe.cpp")
    target_link_libraries(session_attrs_probe PRIVATE omega::resolve)
    set_target_properties(session_attrs_probe PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON)

    # The editor's round trip. Needs QtWidgets and a QApplication because it
    # builds the form; needs no display, no store and no vault.
    add_executable(session_editor_probe
        "${CMAKE_CURRENT_SOURCE_DIR}/../tests/compat/session_editor_probe.cpp")
    target_link_libraries(session_editor_probe PRIVATE omega::shell)
    set_target_properties(session_editor_probe PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON)

    # The shell probe needs QtWidgets, but not a display: it runs under
    # -platform offscreen. Closing a window from outside would need a window
    # manager to deliver WM_DELETE_WINDOW, and requiring one on the build
    # machine to check geometry persistence is a worse trade than a probe that
    # closes the window itself.
    # The tree model and its filter, exercised with no display and no mouse: a
    # drop is a model call, so the writes it makes are checkable without a
    # window manager. Runs against a throwaway database, never ~/.omega.
    add_executable(session_tree_probe
        "${CMAKE_CURRENT_SOURCE_DIR}/../tests/compat/session_tree_probe.cpp")
    target_link_libraries(session_tree_probe PRIVATE omega::shell)
    set_target_properties(session_tree_probe PROPERTIES
        AUTOMOC ON
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON)

    # The terminal tab's two unreadable claims: that Tab reaches the emulator
    # rather than the focus chain, and that escape stripping survives a chunk
    # boundary. Offscreen -- the interception being tested is QWidget's, not
    # the window system's.
    add_executable(terminal_probe
        "${CMAKE_CURRENT_SOURCE_DIR}/../tests/compat/terminal_probe.cpp")
    target_link_libraries(terminal_probe PRIVATE omega::shell)
    set_target_properties(terminal_probe PROPERTIES
        AUTOMOC ON
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON)

    # The host key prompt end to end. Needs the lab sshd (tests/labsshd.sh),
    # so scripts/build.sh skips it when the server is not up -- the sequencing
    # it covers is between two live objects and cannot be faked.
    add_executable(hostkey_flow_probe
        "${CMAKE_CURRENT_SOURCE_DIR}/../tests/compat/hostkey_flow_probe.cpp")
    target_link_libraries(hostkey_flow_probe PRIVATE omega::shell)
    set_target_properties(hostkey_flow_probe PROPERTIES
        AUTOMOC ON
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON)

    # The config directory is defined twice on purpose -- once here, once in
    # transport/logtap.go below the C boundary -- and the auth classifier
    # decides whether the login modal re-opens. Neither claim is checkable by
    # reading, and both fail quietly: logs in the wrong folder, or a password
    # box over a host key mismatch. No display and no network.
    add_executable(config_dir_probe
        "${CMAKE_CURRENT_SOURCE_DIR}/../tests/compat/config_dir_probe.cpp")
    target_link_libraries(config_dir_probe PRIVATE omega::shell)
    set_target_properties(config_dir_probe PROPERTIES
        AUTOMOC ON
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON)

    add_executable(shell_probe
        "${CMAKE_CURRENT_SOURCE_DIR}/../tests/compat/shell_probe.cpp")
    target_link_libraries(shell_probe PRIVATE omega::shell)
    set_target_properties(shell_probe PROPERTIES
        AUTOMOC ON
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON)
endif()