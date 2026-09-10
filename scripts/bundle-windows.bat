@echo off
REM scripts\bundle-windows.bat
REM
REM Packages Omega as a self-contained folder using windeployqt.
REM
REM   scripts\bundle-windows.bat
REM   scripts\bundle-windows.bat --anytermqt C:\path\to\anytermqt
REM   scripts\bundle-windows.bat --qt C:\Qt\6.10.3\msvc2022_64 --zip
REM
REM Run it from a Developer Command Prompt so cl.exe is on PATH.
REM
REM DEPENDENCIES, and how each is found. Every one takes a flag, falls back to
REM an environment variable, and then guesses; whatever it settles on is echoed
REM before the build, so a wrong guess is visible at the top rather than
REM inferred from a compiler error two hundred lines down.
REM
REM   anytermqt   --anytermqt <path>
REM               %OMEGASSH_ANYTERMQT_DIR%
REM               ..\anytermqt beside this repo
REM               %USERPROFILE%\github\anytermqt
REM
REM   Qt          --qt <prefix>
REM               %CMAKE_PREFIX_PATH%
REM               %Qt6_DIR%  (walked back up from lib\cmake\Qt6)
REM               the newest C:\Qt\6.*\msvc*_64
REM
REM windeployqt is taken from the Qt prefix, NOT from PATH. It must come from
REM the Qt the application linked against: a windeployqt from a different Qt
REM copies DLLs that do not match what the binary asks for, and the result runs
REM on the build machine and fails on a clean one. One variable feeding both
REM makes them agree by construction rather than by luck.
REM
REM Options:
REM   --zip              also write dist\omega-windows-x64.zip
REM   --build-dir <dir>  default build-win
REM   --help
REM
REM WHAT windeployqt DOES: copies the Qt DLLs beside the exe, and the plugins
REM into platforms\ and friends. What it does NOT do is copy application
REM resources, which is why the themes are copied here into Resources\themes,
REM the layout app\main.cpp looks for beside the executable.
REM
REM No bundle option is needed the way OMEGA_MACOS_BUNDLE is on macOS: the exe
REM stays where it always was and the packaging is a copy.

setlocal enabledelayedexpansion

set "ROOT=%~dp0.."
pushd "%ROOT%"

set "ANYTERMQT="
set "QTPREFIX="
set "MAKEZIP=0"
set "BUILD_DIR=build-win"
set "STAGE=dist\omega"

REM --- options ---------------------------------------------------------------

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="--zip" (set "MAKEZIP=1" & shift & goto parse)
if /i "%~1"=="--anytermqt" (
    if "%~2"=="" (echo error: --anytermqt needs a path 1>&2 & goto fail)
    set "ANYTERMQT=%~f2" & shift & shift & goto parse
)
if /i "%~1"=="--qt" (
    if "%~2"=="" (echo error: --qt needs a path 1>&2 & goto fail)
    set "QTPREFIX=%~f2" & shift & shift & goto parse
)
if /i "%~1"=="--build-dir" (
    if "%~2"=="" (echo error: --build-dir needs a path 1>&2 & goto fail)
    set "BUILD_DIR=%~2" & shift & shift & goto parse
)
if /i "%~1"=="--help" goto usage
if /i "%~1"=="-h" goto usage
REM A bare path is still taken as anytermqt: that is how this script was called
REM before it grew flags, and breaking it would be gratuitous.
if "%ANYTERMQT%"=="" (set "ANYTERMQT=%~f1" & shift & goto parse)
echo error: unknown option: %~1 1>&2
goto fail
:parsed

REM --- anytermqt -------------------------------------------------------------

if "%ANYTERMQT%"=="" if not "%OMEGASSH_ANYTERMQT_DIR%"=="" (
    set "ANYTERMQT=%OMEGASSH_ANYTERMQT_DIR%"
)
if "%ANYTERMQT%"=="" (
    for %%D in ("%ROOT%\..\anytermqt" "%USERPROFILE%\github\anytermqt") do (
        if exist "%%~fD\qtpyte\CMakeLists.txt" (
            if "!ANYTERMQT!"=="" set "ANYTERMQT=%%~fD"
        )
    )
)
if "%ANYTERMQT%"=="" (
    echo error: no anytermqt checkout found. Either:
    echo         scripts\bundle-windows.bat --anytermqt C:\path\to\anytermqt
    echo     or  set OMEGASSH_ANYTERMQT_DIR=C:\path\to\anytermqt
    echo     Looked beside this repo and in %%USERPROFILE%%\github.
    goto fail
)
if not exist "%ANYTERMQT%\qtpyte\CMakeLists.txt" (
    echo error: not an anytermqt checkout: %ANYTERMQT% 1>&2
    echo     Expected %ANYTERMQT%\qtpyte\CMakeLists.txt
    goto fail
)

REM CHECKED BECAUSE IT HAS ALREADY COST A BUILD. anti-idle calls
REM qtpyte::TerminalWidget::alternateScreen(), which an older anytermqt does not
REM have. The compiler reports that as "'alternateScreen': is not a member of
REM 'omega::app::TerminalView'" -- naming the DERIVED class in this repo, for a
REM method that lives in the base class in the OTHER repo, after a couple of
REM minutes of compiling. Nothing in that error says "git pull anytermqt".
findstr /c:"alternateScreen" "%ANYTERMQT%\qtpyte\include\qtpyte\terminalwidget.h" >nul 2>nul
if errorlevel 1 (
    echo error: the anytermqt checkout at %ANYTERMQT% is out of date. 1>&2
    echo     qtpyte::TerminalWidget has no alternateScreen^(^), which anti-idle
    echo     needs. Update it:
    echo         cd /d %ANYTERMQT% ^&^& git pull
    goto fail
)

REM --- Qt --------------------------------------------------------------------

if "%QTPREFIX%"=="" if not "%CMAKE_PREFIX_PATH%"=="" set "QTPREFIX=%CMAKE_PREFIX_PATH%"

REM Qt6_DIR points at <prefix>\lib\cmake\Qt6, so walk back up three.
if "%QTPREFIX%"=="" if not "%Qt6_DIR%"=="" (
    for %%P in ("%Qt6_DIR%\..\..\..") do set "QTPREFIX=%%~fP"
)

REM Newest first, and NOT with dir /o-n. That sorts as TEXT, which puts 6.8.3
REM above 6.10.3 -- and the resulting mismatch is the exact thing this script
REM is supposed to prevent, because it deploys one Qt's DLLs beside a binary
REM linked against another. PowerShell's [version] cast compares the parts as
REM numbers, which is the only correct way to order these.
if "%QTPREFIX%"=="" if exist "C:\Qt" (
    for /f "delims=" %%D in ('powershell -NoProfile -Command ^
        "Get-ChildItem 'C:\Qt' -Directory -Filter '6.*' ^| Where-Object { $_.Name -match '^^6\.' } ^| Sort-Object { [version]$_.Name } -Descending ^| Select-Object -ExpandProperty Name" 2^>nul') do (
        if "!QTPREFIX!"=="" (
            for /f "delims=" %%E in ('dir /b /ad "C:\Qt\%%D\msvc*_64" 2^>nul') do (
                if "!QTPREFIX!"=="" set "QTPREFIX=C:\Qt\%%D\%%E"
            )
        )
    )
    if not "!QTPREFIX!"=="" (
        echo ==^> guessed Qt at !QTPREFIX!
        echo     ^(pass --qt or set CMAKE_PREFIX_PATH to choose a different one^)
    )
)

if "%QTPREFIX%"=="" (
    echo error: no Qt prefix. Either:
    echo         scripts\bundle-windows.bat --qt C:\Qt\6.10.3\msvc2022_64
    echo     or  set CMAKE_PREFIX_PATH=C:\Qt\6.10.3\msvc2022_64
    echo     Nothing matching C:\Qt\6.*\msvc*_64 was found either.
    goto fail
)
if not exist "%QTPREFIX%\bin\windeployqt.exe" (
    echo error: no windeployqt.exe under %QTPREFIX%\bin 1>&2
    echo     That does not look like a Qt prefix. It should be the directory
    echo     holding bin\, lib\ and include\ -- e.g. C:\Qt\6.10.3\msvc2022_64,
    echo     not C:\Qt and not the lib\cmake\Qt6 inside it.
    goto fail
)

REM --- toolchain -------------------------------------------------------------

where cl.exe >nul 2>nul
if errorlevel 1 (
    echo error: cl.exe not on PATH. 1>&2
    echo     Run this from an "x64 Native Tools Command Prompt for VS 2022",
    echo     or run vcvars64.bat in this shell first.
    goto fail
)

where go.exe >nul 2>nul
if errorlevel 1 (
    echo error: go.exe not on PATH; the transport core is Go. 1>&2
    goto fail
)

REM --- what it settled on ----------------------------------------------------

echo ==^> anytermqt  %ANYTERMQT%
echo ==^> Qt         %QTPREFIX%
echo ==^> build dir  %BUILD_DIR%

REM --- build -----------------------------------------------------------------

REM A CACHED Qt6_DIR BEATS -DCMAKE_PREFIX_PATH, so an existing build directory
REM configured against a different Qt keeps using it and says nothing: the
REM binary links one Qt while windeployqt below deploys another. Wiping is the
REM only reliable answer -- CMake offers no way to un-cache a find_package
REM result -- and a reconfigure costs seconds next to a mismatched package that
REM fails on somebody else's machine.
if exist "%BUILD_DIR%\CMakeCache.txt" (
    findstr /c:"Qt6_DIR:PATH=" "%BUILD_DIR%\CMakeCache.txt" > "%TEMP%\omega_qtdir.txt" 2>nul
    set "CACHED="
    for /f "tokens=2 delims==" %%V in ('type "%TEMP%\omega_qtdir.txt"') do set "CACHED=%%V"
    del "%TEMP%\omega_qtdir.txt" >nul 2>nul
    if not "!CACHED!"=="" (
        echo !CACHED! | findstr /i /c:"%QTPREFIX:\=/%" >nul
        if errorlevel 1 (
            echo ==^> %BUILD_DIR% was configured against a different Qt:
            echo         cached: !CACHED!
            echo         wanted: %QTPREFIX%
            echo     wiping it, or the build and windeployqt would disagree.
            rmdir /s /q "%BUILD_DIR%"
        )
    )
)

echo ==^> building
cmake -S . -B "%BUILD_DIR%" -DOMEGASSH_ANYTERMQT_DIR="%ANYTERMQT%" ^
      -DCMAKE_PREFIX_PATH="%QTPREFIX%" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 goto fail

cmake --build "%BUILD_DIR%" --config Release --target omega
if errorlevel 1 goto fail

REM Multi-config generators (MSBuild, the default here) put it under Release\;
REM single-config ones (Ninja) do not.
set "EXE=%BUILD_DIR%\app\Release\omega.exe"
if not exist "%EXE%" set "EXE=%BUILD_DIR%\app\omega.exe"
if not exist "%EXE%" (
    echo error: no omega.exe after the build 1>&2
    echo     Looked in %BUILD_DIR%\app\Release\ and %BUILD_DIR%\app\
    goto fail
)

REM --- stage -----------------------------------------------------------------

echo ==^> staging into %STAGE%
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%"
mkdir "%STAGE%\Resources"

copy /y "%EXE%" "%STAGE%\omega.exe" >nul
if errorlevel 1 goto fail
xcopy /e /i /q /y "theme\themes" "%STAGE%\Resources\themes" >nul
if errorlevel 1 goto fail

REM --- windeployqt -----------------------------------------------------------

echo ==^> running windeployqt from %QTPREFIX%\bin
"%QTPREFIX%\bin\windeployqt.exe" --release --no-translations ^
    --no-system-d3d-compiler --compiler-runtime "%STAGE%\omega.exe"
if errorlevel 1 goto fail

REM --- the MSVC runtime ------------------------------------------------------
REM
REM WITHOUT THIS THE PACKAGE DIES ON A CLEAN BOX and runs fine here, because
REM this machine has the VS 2022 redistributable installed and the loader finds
REM it system-wide. omega.exe is MSVC-built, so it needs VCRUNTIME140.dll,
REM VCRUNTIME140_1.dll and MSVCP140.dll beside it or on the target machine.
REM
REM --compiler-runtime above is asked for but not trusted. It locates the
REM redistributable through %VCINSTALLDIR%, which only a Developer Command
REM Prompt sets, and depending on the Qt version it may drop vc_redist.x64.exe
REM into the folder INSTEAD of the DLLs. An installer sitting in a directory is
REM not a deployed runtime, and the folder looks equally plausible either way.
REM So the DLLs are checked by name and copied if they are absent.
REM
REM ucrtbase.dll is deliberately not in the list. The UCRT ships with Windows
REM itself from 10 onward, which is below anything this targets.

set "CRTOK=1"
for %%F in (VCRUNTIME140.dll VCRUNTIME140_1.dll MSVCP140.dll) do (
    if not exist "%STAGE%\%%F" set "CRTOK=0"
)

if "!CRTOK!"=="0" (
    if "%VCToolsRedistDir%"=="" (
        echo error: MSVC runtime DLLs missing from %STAGE%, and 1>&2
        echo         %%VCToolsRedistDir%% is unset so they cannot be located.
        echo     Run this from an "x64 Native Tools Command Prompt for VS 2022";
        echo     that is what sets it. A plain cmd with cl.exe on PATH is not
        echo     enough -- the build works and the deployment silently does not.
        goto fail
    )
    REM Newest CRT directory wins. There is normally one, but a machine with
    REM several VS toolsets installed can have more.
    set "CRTDIR="
    for /f "delims=" %%D in ('dir /b /ad /o-n "%VCToolsRedistDir%x64\Microsoft.VC*.CRT" 2^>nul') do (
        if "!CRTDIR!"=="" set "CRTDIR=%VCToolsRedistDir%x64\%%D"
    )
    if "!CRTDIR!"=="" (
        REM !VAR! AND NOT %VAR%, and this is not style. Percent-expansion
        REM happens when cmd parses this whole if-block, before it runs a line
        REM of it -- so on a machine where VS lives under
        REM "C:\Program Files (x86)\..." the ")" in "(x86)" closes the block
        REM early and the rest of the path becomes a stray command:
        REM "\Microsoft was unexpected at this time." Delayed expansion happens
        REM at execution, after the parsing that would have broken.
        echo error: no Microsoft.VC*.CRT directory under 1>&2
        echo         !VCToolsRedistDir!x64
        echo     The VS installation has no redistributable component. Add
        echo     "MSVC v143 - VS 2022 C++ x64/x86 Redistributable MSMs" -- or
        echo     just the latest v143 build tools -- in the VS Installer.
        goto fail
    )
    echo ==^> copying the MSVC runtime from !CRTDIR!
    copy /y "!CRTDIR!\VCRUNTIME140.dll"   "%STAGE%\" >nul
    copy /y "!CRTDIR!\VCRUNTIME140_1.dll" "%STAGE%\" >nul
    copy /y "!CRTDIR!\MSVCP140.dll"       "%STAGE%\" >nul
)

REM If windeployqt left the installer behind, drop it. Shipping an .exe that
REM asks for elevation inside a folder that is meant to be unzip-and-run is the
REM wrong signal to whoever receives it, and the MSIX cannot execute it at all.
if exist "%STAGE%\vc_redist.x64.exe" del /q "%STAGE%\vc_redist.x64.exe"

REM --- verify ----------------------------------------------------------------
REM
REM The platform plugin is the one whose absence is fatal and whose error
REM message does not name it usefully: without platforms\qwindows.dll the
REM application exits with "could not find or load the Qt platform plugin".

if not exist "%STAGE%\platforms\qwindows.dll" (
    echo error: platforms\qwindows.dll missing; the package would not start 1>&2
    goto fail
)
if not exist "%STAGE%\Qt6Widgets.dll" (
    echo error: Qt6Widgets.dll missing; windeployqt did not do its job 1>&2
    goto fail
)
if not exist "%STAGE%\Resources\themes" (
    echo error: Resources\themes missing; the package would open unthemed 1>&2
    goto fail
)

REM The MSVC runtime again, as a gate rather than as a copy. The block above
REM either put these here or failed; if they are still absent something skipped
REM it, and this is the last point before the zip where that is cheap to catch.
for %%F in (VCRUNTIME140.dll VCRUNTIME140_1.dll MSVCP140.dll) do (
    if not exist "%STAGE%\%%F" (
        echo error: %%F missing; the package dies at launch on a machine 1>&2
        echo        without the VS 2022 redistributable, with a missing-DLL
        echo        dialog that says nothing about Qt or about Omega.
        goto fail
    )
)

REM LAST AND MOST IMPORTANT. Everything above proves files are present; this
REM proves they are the RIGHT files. A Qt6Core.dll from a different Qt than the
REM binary was linked against is present, correctly named, and wrong -- and the
REM package still runs on this machine, because the real Qt is on PATH here.
for /f "tokens=2 delims==" %%V in ('findstr /c:"Qt6_DIR:PATH=" "%BUILD_DIR%\CMakeCache.txt"') do set "USEDQT=%%V"
echo !USEDQT! | findstr /i /c:"%QTPREFIX:\=/%" >nul
if errorlevel 1 (
    echo error: the build and windeployqt used different Qt installations. 1>&2
    echo         linked against: !USEDQT!
    echo         deployed from:  %QTPREFIX%
    echo     The package would fail on a machine without Qt installed.
    echo     Delete %BUILD_DIR% and run this again.
    goto fail
)
echo ==^> platform plugin, Qt DLLs, MSVC runtime and themes present,
echo     one Qt throughout

REM --- zip -------------------------------------------------------------------

if "%MAKEZIP%"=="1" (
    echo ==^> writing dist\omega-windows-x64.zip
    if exist "dist\omega-windows-x64.zip" del /q "dist\omega-windows-x64.zip"
    powershell -NoProfile -Command ^
        "Compress-Archive -Path 'dist\omega' -DestinationPath 'dist\omega-windows-x64.zip'"
    if errorlevel 1 goto fail
)

echo.
echo run it with:
echo       %STAGE%\omega.exe
popd
exit /b 0

:usage
echo Packages Omega as a self-contained folder using windeployqt.
echo.
echo   scripts\bundle-windows.bat [--anytermqt ^<path^>] [--qt ^<prefix^>]
echo                              [--zip] [--build-dir ^<dir^>]
echo.
echo Environment: OMEGASSH_ANYTERMQT_DIR, CMAKE_PREFIX_PATH, Qt6_DIR
popd
exit /b 0

:fail
echo.
echo build failed
popd
exit /b 1