# sessions/CMakeLists.txt
#
# The session store. No Qt, no Go, no omegassh core -- it is SQLite and the
# standard library, so it builds and tests on its own:
#
#     cmake -S sessions -B build/sessions && cmake --build build/sessions
#
# That standalone path exists because the top-level build requires the Go
# toolchain, and nothing here needs it. It is also how the differential tests
# run in an environment that has no Go.
#
# SQLite: a vendored amalgamation is preferred, since "one .c file, no package
# manager" is the reason the amalgamation was chosen in the first place. Drop
# sqlite3.c and sqlite3.h into third_party/sqlite3/ and it is picked up
# automatically. Failing that, the system library is used -- same API, and it
# keeps a checkout without the amalgamation buildable.

cmake_minimum_required(VERSION 3.19)

if(NOT DEFINED PROJECT_NAME)
    project(omega_sessions VERSION 0.1.0 LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(OMEGA_SESSIONS_STANDALONE TRUE)
else()
    set(OMEGA_SESSIONS_STANDALONE FALSE)
endif()

# Normalised so the path in the not-found message below is one a person can
# copy, rather than sessions/../third_party.
get_filename_component(SQLITE_AMALGAMATION
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sqlite3" ABSOLUTE)

if(EXISTS "${SQLITE_AMALGAMATION}/sqlite3.c")
    message(STATUS "sessions: using vendored SQLite amalgamation")
    add_library(omega_sqlite3 STATIC "${SQLITE_AMALGAMATION}/sqlite3.c")
    target_include_directories(omega_sqlite3 PUBLIC "${SQLITE_AMALGAMATION}")
    set_target_properties(omega_sqlite3 PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(UNIX)
        target_link_libraries(omega_sqlite3 PUBLIC ${CMAKE_DL_LIBS})
    endif()
    set(OMEGA_SQLITE omega_sqlite3)
else()
    message(STATUS "sessions: no vendored amalgamation, using system SQLite")
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SQLITE3 IMPORTED_TARGET sqlite3)
    endif()
    if(TARGET PkgConfig::SQLITE3)
        set(OMEGA_SQLITE PkgConfig::SQLITE3)
    else()
        find_library(SQLITE3_LIBRARY NAMES sqlite3)
        find_path(SQLITE3_INCLUDE_DIR NAMES sqlite3.h)
        if(NOT SQLITE3_LIBRARY OR NOT SQLITE3_INCLUDE_DIR)
            message(FATAL_ERROR
"No SQLite found, by either route.
Preferred: vendor the amalgamation -- copy sqlite3.c and sqlite3.h from https://sqlite.org/download.html into ${SQLITE_AMALGAMATION}
Or install the development package: libsqlite3-dev (Debian/Ubuntu), sqlite-devel (Fedora), brew install sqlite (macOS).
The -dev part matters: a runtime libsqlite3 ships no headers, which is why a machine that plainly has SQLite can still fail here.")
        endif()
        add_library(omega_sqlite3_system INTERFACE)
        target_link_libraries(omega_sqlite3_system INTERFACE ${SQLITE3_LIBRARY})
        target_include_directories(omega_sqlite3_system INTERFACE
            ${SQLITE3_INCLUDE_DIR})
        set(OMEGA_SQLITE omega_sqlite3_system)
    endif()
endif()

add_library(omega_sessions STATIC store.cpp ttyaml.cpp sessionio.cpp)
target_include_directories(omega_sessions PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(omega_sessions PUBLIC ${OMEGA_SQLITE})
set_target_properties(omega_sessions PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(omega::sessions ALIAS omega_sessions)

# The TerminalTelemetry YAML reader, writer and the import/export rules over
# them. Same reasoning as compat_probe: built in both configurations, needs no
# Qt and no Go, and is the executable statement of what the subset accepts.
add_executable(ttyaml_probe
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/compat/ttyaml_probe.cpp")
target_link_libraries(ttyaml_probe PRIVATE omega_sessions ${OMEGA_SQLITE})
set_target_properties(ttyaml_probe PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)

# The differential probe. Built in both configurations on purpose: a target
# that only builds when someone remembers to ask for it rots, and this one is
# the only executable proof that the store still agrees with nterm-qt.
# Building it costs nothing; RUNNING the suite needs an nterm-qt checkout, and
# that gate lives in scripts/build.sh.
option(OMEGA_SESSIONS_BUILD_TESTS "Build the compat probe" ON)

if(OMEGA_SESSIONS_BUILD_TESTS)
    add_executable(compat_probe
        "${CMAKE_CURRENT_SOURCE_DIR}/../tests/compat/compat_probe.cpp")
    target_link_libraries(compat_probe PRIVATE omega_sessions ${OMEGA_SQLITE})
endif()
