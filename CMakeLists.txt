# CMakeLists.txt
#
# Top-level build for omegassh.
#
# The Go archive is built by a custom command, so `cmake --build` is the only
# entry point a C++ consumer needs. That does make the Go toolchain a hard
# dependency for everyone downstream; see docs/BUILDING.md for the prebuilt
# alternative if that trade stops being worth it.

cmake_minimum_required(VERSION 3.19)
project(omegassh VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(OMEGASSH_BUILD_QT "Build the Qt session wrapper" ON)
option(OMEGASSH_BUILD_EXAMPLES "Build the C and Qt examples" ON)
option(OMEGASSH_BUILD_SESSIONS "Build the session store" ON)
option(OMEGASSH_BUILD_THEME "Build the theme system" ON)
option(OMEGASSH_BUILD_APP "Build the Omega shell application" ON)

find_program(GO_EXECUTABLE go REQUIRED)

# The generated header's name follows -o, not the platform: `go build
# -buildmode=c-archive -o omegassh.lib` writes omegassh.h beside it. Naming it
# libomegassh.h unconditionally was right on POSIX and wrong on Windows, where
# MSBuild then reported a declared output that "has not been created" (MSB8065)
# and re-ran the Go build every time, since a missing output never looks
# up to date.
if(WIN32)
    set(OMEGASSH_ARCHIVE "${CMAKE_CURRENT_BINARY_DIR}/omegassh.lib")
    set(OMEGASSH_GENERATED_HEADER "${CMAKE_CURRENT_BINARY_DIR}/omegassh.h")
else()
    set(OMEGASSH_ARCHIVE "${CMAKE_CURRENT_BINARY_DIR}/libomegassh.a")
    set(OMEGASSH_GENERATED_HEADER "${CMAKE_CURRENT_BINARY_DIR}/libomegassh.h")
endif()

# Two toolchains meet in this archive: mingw-w64 compiles it, MSVC links it.
# mingw-w64 defaults __USE_MINGW_ANSI_STDIO on, which redirects printf and
# friends to its own C99-conformant implementations -- so cgo's generated
# fprintf calls become __mingw_vfprintf, a symbol that lives in libmingwex and
# is not on an MSVC link line. Every consumer of the archive then fails with
# `unresolved external symbol __mingw_vfprintf` at LINK time, long after the
# archive itself built cleanly.
#
# Turning it off makes those calls resolve against the UCRT, which MSVC links
# by default. Nothing in the archive relies on the C99 printf behaviour; the
# only calls involved are cgo's own error paths. Note this REPLACES the default
# CGO_CFLAGS, so -O2 -g are restated.
if(WIN32)
    # -gdwarf-4 pins the debug format in cgo's own objects. gcc 12+ defaults
    # to DWARF 5, which is implicated in malformed Windows images -- they link,
    # and the loader then refuses them with 0xc000007b or ERROR_BAD_EXE_FORMAT,
    # which reads as an architecture mismatch and is not one. It costs nothing
    # under MSVC, which ignores the archive's debug info entirely.
    #
    # It does NOT cover the DWARF Go's own linker emits into go.o; only
    # -ldflags=-w removes that. So if a mingw-linked binary is still refused by
    # the loader, that is the next thing to try, not evidence against this.
    set(OMEGASSH_GO_ENV CGO_ENABLED=1
        "CGO_CFLAGS=-O2 -g -gdwarf-4 -D__USE_MINGW_ANSI_STDIO=0")
else()
    set(OMEGASSH_GO_ENV CGO_ENABLED=1)
endif()

# MSVC requires the .pdata RUNTIME_FUNCTION array to be ordered by function
# start address and rejects the object outright when it is not:
#
#   go.o : fatal error LNK1223: invalid or corrupt file:
#          file contains invalid .pdata contributions
#
# Go's linker does not guarantee that order. It is usually sorted by accident,
# which is why this reads as an intermittent, Go-version-dependent failure
# rather than a rule -- one toolchain produced a sorted archive and the next
# produced 35 inversions out of 5,006 records from the same source.
#
# scripts/pdatafix sorts them in place. It runs only for MSVC: mingw's ld and
# every POSIX linker accept either order, and the pass is a no-op on an archive
# that is already sorted, so this costs a file read on the platforms that do
# not need it. Written in Go so it adds no build dependency -- producing the
# archive already required the toolchain that runs it.
if(MSVC)
    set(OMEGASSH_PDATA_FIX
        COMMAND ${GO_EXECUTABLE} run ./scripts/pdatafix ${OMEGASSH_ARCHIVE})
else()
    set(OMEGASSH_PDATA_FIX "")
endif()

file(GLOB OMEGASSH_GO_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/sshcore/*.go"
    "${CMAKE_CURRENT_SOURCE_DIR}/transport/*.go"
    "${CMAKE_CURRENT_SOURCE_DIR}/telnetx/*.go"
    "${CMAKE_CURRENT_SOURCE_DIR}/serialx/*.go"
    "${CMAKE_CURRENT_SOURCE_DIR}/vault/*.go"
    "${CMAKE_CURRENT_SOURCE_DIR}/vault/keyring/*.go"
    "${CMAKE_CURRENT_SOURCE_DIR}/vault/sshsecrets/*.go"
    "${CMAKE_CURRENT_SOURCE_DIR}/capi/*.go"
    "${CMAKE_CURRENT_SOURCE_DIR}/go.mod")

add_custom_command(
    OUTPUT ${OMEGASSH_ARCHIVE} ${OMEGASSH_GENERATED_HEADER}
    COMMAND ${CMAKE_COMMAND} -E env ${OMEGASSH_GO_ENV}
            ${GO_EXECUTABLE} build -buildmode=c-archive
            -o ${OMEGASSH_ARCHIVE} ./capi
    ${OMEGASSH_PDATA_FIX}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    DEPENDS ${OMEGASSH_GO_SOURCES}
    COMMENT "Building Go c-archive (libomegassh)"
    VERBATIM)

add_custom_target(omegassh_archive DEPENDS ${OMEGASSH_ARCHIVE})

# The C surface. Consumers link this and include <omegassh/omegassh.h>; the
# cgo-generated header in the build dir is an artifact, not the contract.
add_library(omegassh_c INTERFACE)
add_dependencies(omegassh_c omegassh_archive)
target_include_directories(omegassh_c INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(omegassh_c INTERFACE ${OMEGASSH_ARCHIVE})

if(UNIX AND NOT APPLE)
    target_link_libraries(omegassh_c INTERFACE pthread)
elseif(APPLE)
    # CoreFoundation and Security are the keyring backend's; IOKit is the
    # serial enumerator's. go.bug.st/serial's detailed enumerator is cgo on
    # darwin (the port I/O itself is pure Go on all three platforms).
    # Pathfinder ships the same dependency and never needs this, because
    # Pathfinder is a Go binary and cgo's LDFLAGS resolve during Go's own
    # link. Here CMake links the archive, so the frameworks have to reach the
    # final C++ link line explicitly or omegassh_serial_ports is an undefined
    # symbol at packaging time.
    target_link_libraries(omegassh_c INTERFACE
        "-framework CoreFoundation" "-framework IOKit" "-framework Security")
elseif(WIN32)
    # cgo's runtime pulls these in on Windows.
    target_link_libraries(omegassh_c INTERFACE ws2_32 winmm ntdll bcrypt)

    # And legacy_stdio_definitions, for MSVC only. Since VS2015 the UCRT
    # defines fprintf, printf and friends as inline functions in <stdio.h>
    # rather than exporting them, so nothing on a normal MSVC link line
    # provides an out-of-line `fprintf` symbol. Code compiled by any OTHER
    # toolchain -- here, cgo's objects built by mingw-w64 -- emits a plain
    # call and fails to link. legacy_stdio_definitions.lib exists precisely to
    # supply those out-of-line definitions; it ships with MSVC and pulls in
    # nothing else.
    #
    # This is the other half of -D__USE_MINGW_ANSI_STDIO=0 above. That flag
    # stops the call becoming __mingw_vfprintf, which nothing could resolve;
    # this makes the plain fprintf it becomes instead resolvable.
    if(MSVC)
        target_link_libraries(omegassh_c INTERFACE legacy_stdio_definitions)

        # And the runtime initializer MSVC would otherwise never run.
        #
        # c-archive registers _rt0_amd64_windows_lib in a .ctors section --
        # the GNU convention. link.exe does not know that section, so it links
        # the archive cleanly and never starts Go's runtime, and the first
        # cgo call blocks forever in _cgo_wait_runtime_init_done with no
        # diagnostic anywhere. golang/go#42190.
        #
        # msvc/runtime_init.c calls the same function through .CRT$XCU, which
        # link.exe does honour. Its own /include: pragma keeps the object from
        # being discarded. Not built under mingw, where .ctors already works
        # and this would initialize twice.
        add_library(omegassh_msvc_init STATIC msvc/runtime_init.c)
        target_link_libraries(omegassh_c INTERFACE omegassh_msvc_init)
        add_dependencies(omegassh_msvc_init omegassh_archive)

        # /INCLUDE: has to be HERE, on the link line, and not only in the
        # #pragma inside runtime_init.c. Nothing references that object, so
        # without this the linker never pulls it out of the static library --
        # and a pragma in an object that was never linked cannot ask to be
        # linked. The pragma covers the case where someone compiles the file
        # directly into their own target; this covers the library case, which
        # is how omegassh ships it.
        target_link_options(omegassh_c INTERFACE
            "/INCLUDE:omegassh_go_runtime_init")
    endif()
endif()

add_library(omegassh::c ALIAS omegassh_c)

# The session store. Deliberately not linked against omegassh_c: it is SQLite
# and the standard library, so it also configures on its own --
#
#     cmake -S sessions -B build/sessions
#
# -- with no Go toolchain, which is a fast loop while schema behaviour is
# still moving. If a dependency on the cgo header or a Qt type ever creeps in,
# that standalone configure breaks and says so.
if(OMEGASSH_BUILD_SESSIONS)
    add_subdirectory(sessions)
endif()

# The theme system. Same arrangement as the store and for the same reasons: no
# Go, no Qt, no third-party dependency, and it configures on its own --
#
#     cmake -S theme -B build/theme
#
# -- which is the fast loop while the stylesheet is being compared against the
# Python it was ported from. It is added BEFORE examples/, because
# examples/qt/theme_gallery links omega::theme and reads OMEGA_THEME_DIR from
# it.
if(OMEGASSH_BUILD_THEME)
    add_subdirectory(theme)
endif()

if(OMEGASSH_BUILD_QT)
    add_subdirectory(qt)
endif()

# --- anytermqt -------------------------------------------------------------
#
# Discovered HERE rather than inside examples/qt, because two things need it
# now: the example and the application. Adding the same subdirectory from two
# places is a duplicate-target error, and leaving it in examples/ meant the app
# broke the moment someone configured with -DOMEGASSH_BUILD_EXAMPLES=OFF.
#
# The project installs no CMake package config, so there is nothing for
# find_package to find; it is consumed by add_subdirectory. Not finding it is
# not an error -- the theme system, the store and the C surface all build
# without it, and only the terminal-facing targets are skipped.
set(OMEGASSH_ANYTERMQT_DIR "" CACHE PATH
    "Path to an anytermqt checkout, for the terminal widget")

set(OMEGASSH_HAVE_ANYTERMQT OFF)
if(OMEGASSH_ANYTERMQT_DIR AND OMEGASSH_BUILD_QT)
    if(NOT EXISTS "${OMEGASSH_ANYTERMQT_DIR}/qtpyte/CMakeLists.txt")
        message(FATAL_ERROR
            "OMEGASSH_ANYTERMQT_DIR=${OMEGASSH_ANYTERMQT_DIR} does not look like "
            "an anytermqt checkout (no qtpyte/CMakeLists.txt)")
    endif()
    find_package(Qt6 REQUIRED COMPONENTS Widgets)

    # Consume the widget, do not re-verify it: anytermqt tests itself in its
    # own build, and its host app and test suite are not ours to build here.
    set(QTPYTE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(QTPYTE_BUILD_APP OFF CACHE BOOL "" FORCE)
    set(PYTE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(PYTE_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${OMEGASSH_ANYTERMQT_DIR}/qtpyte"
                     "${CMAKE_CURRENT_BINARY_DIR}/qtpyte")

    # utf8proc arrives with qtpyte, and it is a C library that puts -std=c99 on
    # its own sources. In THIS build it also acquires AUTOMOC, so a C++
    # mocs_compilation.cpp is generated inside a C target and the C flag lands
    # on a C++ file:
    #
    #   error: invalid argument '-std=c99' not allowed with 'C++'
    #
    # GCC treats that as a warning and carries on, AppleClang makes it fatal --
    # which is why Linux built this for weeks and the first Mac did not.
    #
    # It does not happen when anytermqt builds itself: that build has no
    # utf8proc_autogen directory at all. The difference is the line above. We
    # add qtpyte/ directly rather than anytermqt's top-level CMakeLists, so
    # utf8proc is created in a different scope here than there, and inherits a
    # different AUTOMOC setting. Nothing in a C library needs moc.
    #
    # No if(TARGET) guard, deliberately. If utf8proc is renamed or stops being
    # fetched, this should fail loudly at configure time rather than quietly
    # stop protecting anything -- quiet is how the original bug survived.
    set_target_properties(utf8proc PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF)

    set(OMEGASSH_HAVE_ANYTERMQT ON)
    message(STATUS "omegassh: anytermqt at ${OMEGASSH_ANYTERMQT_DIR}")
endif()

# The shell: settings, the main window, the terminal tab and the application.
# After theme/ and qt/, both of which it links, and after anytermqt, which it
# needs now that quick connect can open a terminal.
if(OMEGASSH_BUILD_APP)
    if(NOT OMEGASSH_BUILD_THEME)
        message(FATAL_ERROR
            "OMEGASSH_BUILD_APP needs OMEGASSH_BUILD_THEME: the shell takes its "
            "colours from omega::theme.")
    endif()
    if(NOT OMEGASSH_BUILD_QT OR NOT OMEGASSH_HAVE_ANYTERMQT)
        message(STATUS
            "omegassh: skipping the app -- it needs the Qt wrapper and an "
            "anytermqt checkout (-DOMEGASSH_ANYTERMQT_DIR=<path>)")
    else()
        add_subdirectory(app)
    endif()
endif()

if(OMEGASSH_BUILD_EXAMPLES)
    add_subdirectory(examples/c)
    if(OMEGASSH_BUILD_QT)
        add_subdirectory(examples/qt)
    endif()
endif()