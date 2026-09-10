# theme/CMakeLists.txt
#
# The theme system: no Qt, no Go, no third-party dependency. Like sessions/, it
# also configures on its own --
#
#     cmake -S theme -B build/theme
#
# -- which is a fast loop while the port is being compared against the Python,
# and a standing check that Qt has not crept in. If a QColor or a QString ever
# appears in this library, that standalone configure breaks and says so.
#
# The theme FILES are copied, not generated. All 33 are nterm-qt's, unchanged,
# and both applications read the same directory.

cmake_minimum_required(VERSION 3.19)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(omega_theme VERSION 0.1.0 LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(OMEGA_THEME_STANDALONE ON)
endif()

add_library(omega_theme STATIC
    color.cpp
    color.h
    stylesheet.cpp
    stylesheet.h
    theme.cpp
    theme.h
    tokens.cpp
    tokens.h
    tokenstylesheet.cpp
    tokenstylesheet.h)

# Headers are included as "theme/theme.h", so the include root is the parent
# directory -- the same spelling whether this is built here or from the top.
target_include_directories(omega_theme PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")

set_target_properties(omega_theme PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)

# GCC and Clang before 9 put std::filesystem in a separate library.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
   CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
    target_link_libraries(omega_theme PUBLIC stdc++fs)
endif()

add_library(omega::theme ALIAS omega_theme)

# Where the shipped themes live at build time. An application resolves the
# runtime path itself; this is here so the probe and any example can find the
# set without being told.
set(OMEGA_THEME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/themes"
    CACHE PATH "Directory of shipped theme YAML files")

option(OMEGA_THEME_BUILD_PROBE
       "Build theme_probe, the dumper the differential test drives" ON)

if(OMEGA_THEME_BUILD_PROBE)
    add_executable(theme_probe "${CMAKE_CURRENT_SOURCE_DIR}/../tests/compat/theme_probe.cpp")
    target_link_libraries(theme_probe PRIVATE omega_theme)
    set_target_properties(theme_probe PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON)
endif()
