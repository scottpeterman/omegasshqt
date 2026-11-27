@echo off
REM scripts\build-ghostty-vt.bat
REM
REM Builds libghostty-vt -- the optional terminal core under anytermqt -- into an
REM install prefix that bundle-windows.bat takes with --ghostty <prefix>.
REM scripts/build-ghostty-vt.sh is the Linux and macOS equivalent; the pins and
REM the reasons for them are written down there.
REM
REM   scripts\build-ghostty-vt.bat --prefix C:\tools\ghostty-vt
REM   scripts\bundle-windows.bat --ghostty C:\tools\ghostty-vt --zip
REM
REM Options:
REM   --prefix <dir>   where to install (required)
REM   --ref <tag>      Ghostty tag, default below
REM   --src <dir>      checkout to use or create, default .ghostty-src
REM   --zig <path>     zig.exe, default zig on PATH
REM
REM THE TARGET IS x86_64-windows-gnu, ON PURPOSE. That is what Ghostty's own CI
REM builds and installs on Windows, and what its MSVC consumers link: the DLL
REM exports a plain C ABI and imports only kernel32, ntdll and the UCRT, so
REM which C++ ABI built its insides does not reach omega.exe. The msvc target
REM needs the Windows SDK visible to Zig and buys nothing.
REM
REM No path variable is expanded inside a ( ) block below -- "(x86)" in a path
REM closes the block early. Errors go through :fail instead.
setlocal

set "GHOSTTY_VT_REF=vt-1.0.2"
set "ZIG_VERSION=0.17.1"

set "ROOT=%~dp0.."
set "PREFIX="
set "REF=%GHOSTTY_VT_REF%"
set "SRC=%ROOT%\.ghostty-src"
set "ZIG=zig"

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="--prefix" (set "PREFIX=%~f2" & shift & shift & goto parse)
if /i "%~1"=="--ref" (set "REF=%~2" & shift & shift & goto parse)
if /i "%~1"=="--src" (set "SRC=%~f2" & shift & shift & goto parse)
if /i "%~1"=="--zig" (set "ZIG=%~f2" & shift & shift & goto parse)
set "MSG=unknown option: %~1"
goto fail
:parsed

set "MSG=--prefix <dir> is required"
if "%PREFIX%"=="" goto fail
set "MSG=no zig: install Zig %ZIG_VERSION% or pass --zig"
"%ZIG%" version >nul 2>&1
if errorlevel 1 goto fail
for /f "delims=" %%V in ('"%ZIG%" version') do set "HAVE_ZIG=%%V"
set "MSG=zig is %HAVE_ZIG%; this pin wants %ZIG_VERSION%"
if not "%HAVE_ZIG%"=="%ZIG_VERSION%" goto fail

set "AT="
if exist "%SRC%\.git" for /f "delims=" %%T in ('git -C "%SRC%" describe --tags --exact-match 2^>nul') do set "AT=%%T"
if not exist "%SRC%\.git" goto clone
if "%AT%"=="%REF%" goto have_src
echo ==^> %SRC% is not at %REF%; replacing it
rmdir /s /q "%SRC%"
:clone
echo ==^> cloning ghostty %REF%
git clone --quiet --depth 1 --branch %REF% https://github.com/ghostty-org/ghostty.git "%SRC%"
set "MSG=git clone failed"
if errorlevel 1 goto fail
:have_src

echo ==^> building libghostty-vt %REF% with zig %HAVE_ZIG% into %PREFIX%
pushd "%SRC%"
"%ZIG%" build -Demit-lib-vt -Demit-xcframework=false -Doptimize=ReleaseFast -Dtarget=x86_64-windows-gnu --prefix "%PREFIX%"
set "RC=%ERRORLEVEL%"
popd
set "MSG=zig build failed"
if not "%RC%"=="0" goto fail

set "MSG=no bin\ghostty-vt.dll under the prefix after the build"
if not exist "%PREFIX%\bin\ghostty-vt.dll" goto fail
set "MSG=no lib\cmake\ghostty-vt package config under the prefix"
if not exist "%PREFIX%\lib\cmake\ghostty-vt\ghostty-vt-config.cmake" goto fail
echo ==^> installed ghostty-vt.dll and its CMake package
exit /b 0

:fail
echo error: %MSG%
exit /b 1
