# qt/CMakeLists.txt
#
# QtCore only. The wrapper needs QObject, QSocketNotifier and QJsonObject and
# nothing else -- a headless tool can link it without pulling in QtWidgets.
#
# Nothing here uses qt_standard_project_setup(): that arrived in Qt 6.3, and
# Ubuntu 22.04 still ships 6.2.4, where find_package(Qt6) succeeds but the
# function does not exist. AUTOMOC is set on the target directly instead,
# which is all that call would have done for us.

find_package(Qt6 REQUIRED COMPONENTS Core)

if(Qt6_VERSION VERSION_LESS 6.2)
    message(FATAL_ERROR
        "omegassh needs Qt 6.2 or newer for QSocketNotifier; found ${Qt6_VERSION}")
endif()
message(STATUS "omegassh: building the Qt wrapper against Qt ${Qt6_VERSION}")

add_library(omegassh_qt STATIC
    omegasshsession.cpp
    omegasshsession.h
    omegasshvault.cpp
    omegasshvault.h)

target_include_directories(omegassh_qt PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(omegassh_qt PUBLIC Qt6::Core omegassh::c)

set_target_properties(omegassh_qt PROPERTIES
    AUTOMOC ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)

add_library(omegassh::qt ALIAS omegassh_qt)

option(OMEGASSH_QT_BUILD_PROBE
       "Build vault_probe, the headless exercise of the vault wrapper" ON)

if(OMEGASSH_QT_BUILD_PROBE)
    # QtCore only, like the wrapper: it runs on a build machine with no
    # display and no keyring, which is also the environment where the keyring
    # branch it checks is the one that fires.
    add_executable(vault_probe
        "${CMAKE_CURRENT_SOURCE_DIR}/../tests/compat/vault_probe.cpp")
    target_link_libraries(vault_probe PRIVATE omegassh::qt)
    set_target_properties(vault_probe PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON)
endif()
