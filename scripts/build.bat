@echo off
REM scripts\build.bat
REM
REM Windows build. Needs Go, CMake, and VS 2022 Build Tools (Desktop
REM development with C++), plus mingw-w64 on PATH for cgo -- two toolchains,
REM which is the usual surprise here. See docs\BUILDING.md.
REM
REM   scripts\build.bat                        everything it can find
REM   scripts\build.bat --no-qt                C surface only
REM   scripts\build.bat --anytermqt <path>     point at an anytermqt checkout
REM
REM examples\qt\terminal_window needs anytermqt, which installs no CMake
REM package config. The path is taken from --anytermqt, else
REM %OMEGASSH_ANYTERMQT_DIR%, else a sibling checkout if one is there.
REM Not finding it is not an error; the example is skipped.
REM
REM Qt is located through CMAKE_PREFIX_PATH, e.g.
REM   set CMAKE_PREFIX_PATH=C:\Qt\6.10.3\msvc2022_64

setlocal enabledelayedexpansion
cd /d "%~dp0.."
set "ROOT=%CD%"

set "BUILD_QT=ON"
set "ANYTERMQT=%OMEGASSH_ANYTERMQT_DIR%"

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="--no-qt" (set "BUILD_QT=OFF" & shift & goto parse)
if /i "%~1"=="--anytermqt" (
    if "%~2"=="" (echo error: --anytermqt needs a path 1>&2 & exit /b 2)
    set "ANYTERMQT=%~2" & shift & shift & goto parse
)
echo unknown option: %~1 1>&2
exit /b 2
:parsed

REM --- preflight -------------------------------------------------------------
where go >nul 2>&1 || (echo error: go is not on PATH 1>&2 & exit /b 1)
where cmake >nul 2>&1 || (echo error: cmake is not on PATH 1>&2 & exit /b 1)

if not exist "go.mod" (
    echo error: no go.mod at %ROOT% 1>&2
    echo   The module file is missing from this working copy. 1>&2
    echo   See docs\BUILDING.md for its contents. 1>&2
    exit /b 1
)

REM --- locate anytermqt ------------------------------------------------------
REM An explicit path that turns out to be wrong is worth stopping for; a guess
REM that misses is not. A checkout is recognized by its widget, not its name.
if not "%ANYTERMQT%"=="" (
    if not exist "%ANYTERMQT%\qtpyte\CMakeLists.txt" (
        echo error: %ANYTERMQT% does not look like an anytermqt checkout 1>&2
        exit /b 1
    )
) else (
    if "%BUILD_QT%"=="ON" (
        for %%D in ("%ROOT%\..\anytermqt" "%USERPROFILE%\github\anytermqt") do (
            if exist "%%~fD\qtpyte\CMakeLists.txt" (
                set "ANYTERMQT=%%~fD"
                echo ==^> found anytermqt at %%~fD
                goto found
            )
        )
    )
)
:found

if not exist "go.sum" (
    echo ==^> go mod download ^(no go.sum yet^)
    go mod download || exit /b 1
)

REM --- build -----------------------------------------------------------------
REM ./... rather than ./sshcore/...: the vault package has its own suite, and
REM scoping this to one package is how a second package's tests quietly stop
REM running the day it is added.
echo ==^> go vet
go vet ./... || exit /b 1

echo ==^> go test
go test -count=1 ./... || exit /b 1

set "CMAKE_ARGS=-S . -B build -DCMAKE_BUILD_TYPE=Release -DOMEGASSH_BUILD_QT=%BUILD_QT%"
if not "%ANYTERMQT%"=="" if "%BUILD_QT%"=="ON" (
    set "CMAKE_ARGS=!CMAKE_ARGS! -DOMEGASSH_ANYTERMQT_DIR=%ANYTERMQT%"
)

echo ==^> cmake configure
cmake !CMAKE_ARGS! || exit /b 1

echo ==^> cmake build
cmake --build build --config Release || exit /b 1

REM --- session store compatibility -------------------------------------------
REM Gated on NTERMQT_SRC, and the skip is loud: a quiet skip would let a green
REM build imply a compatibility check that never ran.
REM
REM The store has nothing platform-specific in it, so what this proves on
REM Windows is mostly that the toolchain and the SQLite wiring are right. The
REM behaviour is the same code the Linux run exercises.

set "PROBE=build\sessions\compat_probe.exe"
if not exist "%PROBE%" set "PROBE=build\sessions\Release\compat_probe.exe"

if not exist "%PROBE%" (
    echo ==^> compat suite: skipped ^(no compat_probe built^)
) else if "%NTERMQT_SRC%"=="" (
    echo ==^> compat suite: SKIPPED -- NTERMQT_SRC is not set
    echo     The store's compatibility with nterm-qt is unverified in this build.
    echo     set NTERMQT_SRC=C:\path\to\nterm-qt ^&^& scripts\build.bat
) else if not exist "%NTERMQT_SRC%\ntermqt\manager\models.py" if not exist "%NTERMQT_SRC%\nterm\manager\models.py" (
    REM Both layouts are valid -- the package was renamed from nterm to ntermqt.
    echo error: NTERMQT_SRC=%NTERMQT_SRC% has no session store module.
    echo   Looked for ntermqt\manager\models.py and nterm\manager\models.py.
    echo   Point it at the root of an nterm-qt checkout, or clear it to skip.
    exit /b 1
) else (
    echo ==^> compat suite ^(against %NTERMQT_SRC%^)
    REM python3 is the launcher's name on POSIX; on Windows it is python.
    where python >nul 2>&1 || (
        echo error: NTERMQT_SRC is set but python is not on PATH
        exit /b 1
    )
    python tests\compat\differential.py --probe "%PROBE%" || exit /b 1
)

REM --- settings compatibility ------------------------------------------------
REM Same gate as the other suites. The byte comparison is the one that matters:
REM both applications rewrite config.json, and a formatting difference would
REM make every alternating save look like an edit.

set "SETTINGS_PROBE=build\app\settings_probe.exe"
if not exist "%SETTINGS_PROBE%" set "SETTINGS_PROBE=build\app\Release\settings_probe.exe"

if not exist "%SETTINGS_PROBE%" (
    echo ==^> settings suite: skipped ^(no settings_probe built^)
) else if "%NTERMQT_SRC%"=="" (
    echo ==^> settings suite: SKIPPED -- NTERMQT_SRC is not set
    echo     config.json compatibility with nterm-qt is unverified in this build.
    echo     set NTERMQT_SRC=C:\path\to\nterm-qt ^&^& scripts\build.bat
) else if not exist "%NTERMQT_SRC%\ntermqt\config.py" (
    echo ==^> settings suite: SKIPPED -- no ntermqt\config.py under %NTERMQT_SRC%
) else (
    echo ==^> settings suite ^(against %NTERMQT_SRC%^)
    where python >nul 2>&1 || (
        echo error: NTERMQT_SRC is set but python is not on PATH
        exit /b 1
    )
    python tests\compat\settings_differential.py --probe "%SETTINGS_PROBE%" --ntermqt "%NTERMQT_SRC%" || exit /b 1
)

REM --- shell geometry --------------------------------------------------------
REM Opens the window, places it, closes it, reopens it and reports what came
REM back. Offscreen, so it needs no display and no window manager.

set "SHELL_PROBE=build\app\shell_probe.exe"
if not exist "%SHELL_PROBE%" set "SHELL_PROBE=build\app\Release\shell_probe.exe"

if not exist "%SHELL_PROBE%" (
    echo ==^> shell probe: skipped ^(not built^)
) else (
    echo ==^> shell probe ^(offscreen^)
    set "QT_QPA_PLATFORM=offscreen"
    "%SHELL_PROBE%" theme\themes "%TEMP%\omega-shell-probe.json" 900 600 137 91 320 || exit /b 1
    set "QT_QPA_PLATFORM="
)

REM --- terminal tab ----------------------------------------------------------
REM The same probe build.sh runs, which Windows was not running at all. Tab
REM reaching the emulator, capture stripping across a chunk boundary, paste
REM pacing, host key classification, the anti-idle suppressions and the
REM scrollback limit. Offscreen, and not gated on NTERMQT_SRC: it compares
REM Omega against itself.

set "TERMINAL_PROBE=build\app\terminal_probe.exe"
if not exist "%TERMINAL_PROBE%" set "TERMINAL_PROBE=build\app\Release\terminal_probe.exe"

if not exist "%TERMINAL_PROBE%" (
    echo ==^> terminal probe: skipped ^(not built^)
) else (
    echo ==^> terminal probe ^(offscreen^)
    set "QT_QPA_PLATFORM=offscreen"
    "%TERMINAL_PROBE%" || exit /b 1
    set "QT_QPA_PLATFORM="
)

REM --- theme compatibility ---------------------------------------------------
REM Same gate as the store suite. The stylesheet has no runtime check on it --
REM a wrong derived colour renders, it just renders wrong -- so a build that
REM skipped this says nothing about whether the port still matches the Python.

set "THEME_PROBE=build\theme\theme_probe.exe"
if not exist "%THEME_PROBE%" set "THEME_PROBE=build\theme\Release\theme_probe.exe"

if not exist "%THEME_PROBE%" (
    echo ==^> theme suite: skipped ^(no theme_probe built^)
) else if "%NTERMQT_SRC%"=="" (
    echo ==^> theme suite: SKIPPED -- NTERMQT_SRC is not set
    echo     The stylesheet port's agreement with nterm-qt is unverified in this build.
    echo     set NTERMQT_SRC=C:\path\to\nterm-qt ^&^& scripts\build.bat
) else if not exist "%NTERMQT_SRC%\ntermqt\theme\stylesheet.py" (
    REM Only the flattened ntermqt\ layout carries the theme package here. An
    REM older nterm\ checkout is a skip, not an error: it answers the store
    REM suite above and cannot answer this one.
    echo ==^> theme suite: SKIPPED -- no ntermqt\theme\stylesheet.py under %NTERMQT_SRC%
) else (
    echo ==^> theme suite ^(against %NTERMQT_SRC%^)
    where python >nul 2>&1 || (
        echo error: NTERMQT_SRC is set but python is not on PATH
        exit /b 1
    )
    python tests\compat\theme_differential.py --probe "%THEME_PROBE%" --themes theme\themes --ntermqt "%NTERMQT_SRC%" || exit /b 1
)

echo.
echo artifacts:
for %%F in (build\omegassh.lib build\Release\omegassh.lib) do if exist "%%F" echo   %%F
for %%F in (build\sessions\omega_sessions.lib build\sessions\Release\omega_sessions.lib) do if exist "%%F" echo   %%F
for %%F in (build\sessions\compat_probe.exe build\sessions\Release\compat_probe.exe) do if exist "%%F" echo   %%F
for %%F in (build\theme\omega_theme.lib build\theme\Release\omega_theme.lib) do if exist "%%F" echo   %%F
for %%F in (build\theme\theme_probe.exe build\theme\Release\theme_probe.exe) do if exist "%%F" echo   %%F
for %%F in (build\app\omega_settings.lib build\app\Release\omega_settings.lib) do if exist "%%F" echo   %%F
for %%F in (build\app\omega_shell.lib build\app\Release\omega_shell.lib) do if exist "%%F" echo   %%F
for %%F in (build\app\settings_probe.exe build\app\Release\settings_probe.exe) do if exist "%%F" echo   %%F
for %%F in (build\app\shell_probe.exe build\app\Release\shell_probe.exe) do if exist "%%F" echo   %%F
for %%F in (build\app\terminal_probe.exe build\app\Release\terminal_probe.exe) do if exist "%%F" echo   %%F
for %%F in (build\app\omega.exe build\app\Release\omega.exe) do if exist "%%F" echo   %%F
for /r build\examples %%F in (*.exe) do echo   %%F

if "%BUILD_QT%"=="ON" if "%ANYTERMQT%"=="" (
    echo.
    echo note: the app, terminal_window and theme_gallery were skipped -- no anytermqt checkout found.
    echo       scripts\build.bat --anytermqt C:\path\to\anytermqt
)

endlocal