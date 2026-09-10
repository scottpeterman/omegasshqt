# examples/qt/CMakeLists.txt
#
# qt_smoke needs QtCore only, so it builds anywhere the wrapper does.
#
# terminal_window and theme_gallery need anytermqt. That project installs no
# CMake package config, so there is nothing for find_package to find; it is
# consumed by add_subdirectory instead. Point OMEGASSH_ANYTERMQT_DIR at a
# checkout:
#
#   cmake -S . -B build -DOMEGASSH_ANYTERMQT_DIR=../anytermqt
#
# Without it both are skipped rather than failing the build.

find_package(Qt6 REQUIRED COMPONENTS Core)

add_executable(qt_smoke qt_smoke.cpp)
target_link_libraries(qt_smoke PRIVATE omegassh::qt)
set_target_properties(qt_smoke PROPERTIES AUTOMOC ON)

# anytermqt is discovered and added at the top level now, because the
# application needs it too and a second add_subdirectory of the same path is a
# duplicate-target error. What is left here is the check that it was found.
if(NOT OMEGASSH_HAVE_ANYTERMQT)
    message(STATUS
        "omegassh: no anytermqt checkout -- skipping terminal_window and "
        "theme_gallery")
    return()
endif()

add_executable(terminal_window terminal_window.cpp)
target_include_directories(terminal_window PRIVATE
    "${CMAKE_SOURCE_DIR}/integration")
target_link_libraries(terminal_window PRIVATE
    omegassh::qt Qt6::Widgets qtpyte::core)
set_target_properties(terminal_window PROPERTIES AUTOMOC ON)

# The theme gallery. Note what it does NOT link: omegassh::qt, and therefore
# not the Go archive either. It is the theme library, the widget and Qt, which
# means it builds and runs with no Go toolchain present -- the fast loop while
# the stylesheet is being compared against the Python.
#
# (The top-level CMakeLists still calls find_program(GO_EXECUTABLE go REQUIRED)
# unconditionally, so that loop only pays off once the requirement moves under
# the target that actually needs it.)
if(TARGET omega::theme)
    add_executable(theme_gallery theme_gallery.cpp)
    target_include_directories(theme_gallery PRIVATE
        "${CMAKE_SOURCE_DIR}/integration")
    target_compile_definitions(theme_gallery PRIVATE
        OMEGA_THEME_DIR_DEFAULT="${OMEGA_THEME_DIR}")
    target_link_libraries(theme_gallery PRIVATE
        omega::theme Qt6::Widgets qtpyte::core)
    set_target_properties(theme_gallery PROPERTIES AUTOMOC ON)
else()
    message(STATUS
        "omegassh: omega::theme not configured -- skipping theme_gallery")
endif()
