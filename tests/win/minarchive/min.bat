@echo off
REM tests\win\minarchive\min.bat
REM
REM Two c-archives, identical except for their imports, both linked by MSVC
REM and run. See min.go for what each outcome means.
REM
REM   stage 1  no imports at all      -- is the Go runtime OK as a library?
REM   stage 2  omegassh's init graph  -- does something block in init()?
REM
REM Run from the repository root, in an x64 Native Tools prompt:
REM
REM   tests\win\minarchive\min.bat

setlocal
cd /d "%~dp0..\..\.."

where go >nul 2>&1 || (echo error: go is not on PATH 1>&2 & exit /b 1)
where cl >nul 2>&1 || (echo error: cl is not on PATH 1>&2 & exit /b 1)
if /i not "%VSCMD_ARG_TGT_ARCH%"=="x64" (
    echo error: this prompt targets "%VSCMD_ARG_TGT_ARCH%", not x64 1>&2
    echo   Open "x64 Native Tools Command Prompt for VS 2022". 1>&2
    exit /b 1
)

REM The sources and the staging directories all live under _-prefixed names.
REM The go tool skips those when matching ./..., which is what keeps two files
REM that both export min_answer -- and the throwaway stage packages -- out of
REM `go vet ./...` and `go test ./...` in scripts\build.bat. An explicit path
REM still builds them, which is all this script needs.
set "SRCDIR=tests\win\minarchive"
set "SRC_IN=%SRCDIR%\_src"
set "OUT=build\minarchive"
if not exist "%OUT%" mkdir "%OUT%"
REM An earlier version of this script copied go.mod into build\, which made
REM build\ a module of its own and put the staging package outside the main
REM one. Clean it up rather than leave a landmine for the next run.
if exist build\go.mod del build\go.mod

set "CGO_ENABLED=1"
set "CGO_CFLAGS=-O2 -g -D__USE_MINGW_ANSI_STDIO=0"

REM Stage 1 runs twice, differing only in the C runtime model.
REM
REM Bare `cl` defaults to /MT, the STATIC UCRT. mingw-w64 compiles cgo's
REM objects against the DLL UCRT and references its symbols as dllimport, so
REM /MT puts two C runtimes in one process with separate stdio state and
REM separate locks. The linker says so and calls it a warning:
REM
REM   LNK4217: '__acrt_iob_func' defined in 'libucrt.lib' is imported by
REM            'min1.lib(...)' in function '_cgo_preinit_init'
REM
REM CMake defaults MSVC to /MD, so the real build does not have this. If /MT
REM hangs and /MD does not, this script was the problem and the real hang is
REM something else. If BOTH hang, this is a minimal reproducer of it.
call :stage 1 min.go  /MD "no imports, dynamic CRT (/MD, what CMake uses)"
call :stage 1mt min.go /MT "no imports, static CRT (/MT, bare cl default)"
call :stage 2 min2.go /MD "omegassh init graph, dynamic CRT"
exit /b 0

:stage
set "N=%~1"
set "SRC=%~2"
set "CRT=%~3"
set "WHAT=%~4"
echo.
echo ============================================================
echo stage %N%: %WHAT%
echo ============================================================

REM Staged inside the source tree, not under build\: `go build` takes a
REM package path within the module. Each stage needs its own directory --
REM two main packages cannot share one, and go build takes a package, not
REM a file.
set "PKG=%SRCDIR%\_stage%N%"
if "%N%"=="1mt" set "PKG=%SRCDIR%\_stage1"
if not exist "%PKG%" mkdir "%PKG%"
copy /y "%SRC_IN%\%SRC%" "%PKG%\main.go" >nul

echo ==^> go build -buildmode=c-archive
go build -buildmode=c-archive -o "%OUT%\min%N%.lib" ".\%PKG%" || (echo BUILD FAILED 1>&2 & exit /b 2)

echo ==^> pdatafix
go run .\scripts\pdatafix "%OUT%\min%N%.lib" || (echo PDATAFIX FAILED 1>&2 & exit /b 2)

echo ==^> cl + link
cl /nologo /EHsc %CRT% /Fe:"%OUT%\min%N%.exe" /Fo:"%OUT%\min%N%_" ^
   "%SRCDIR%\min.c" "%OUT%\min%N%.lib" ^
   ws2_32.lib winmm.lib ntdll.lib bcrypt.lib advapi32.lib ole32.lib oleaut32.lib ^
   legacy_stdio_definitions.lib || (echo LINK FAILED 1>&2 & exit /b 2)

echo ==^> running ^(ctrl-c if it hangs^)
"%OUT%\min%N%.exe"
echo     exit code: %ERRORLEVEL%
exit /b 0
