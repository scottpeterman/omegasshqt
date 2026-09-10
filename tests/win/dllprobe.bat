@echo off
REM tests\win\dllprobe.bat
REM
REM Builds capi as a DLL with Go's own linker, then an MSVC probe that loads
REM it. Answers one question: does Go's runtime start inside a process it does
REM not own? See tests\win\dllprobe.cpp for what each outcome means.
REM
REM Run from the repository root, in an x64 Native Tools prompt:
REM
REM   tests\win\dllprobe.bat

setlocal
cd /d "%~dp0..\.."

where go >nul 2>&1 || (echo error: go is not on PATH 1>&2 & exit /b 1)
where cl >nul 2>&1 || (echo error: cl is not on PATH -- use the x64 Native Tools prompt 1>&2 & exit /b 1)

REM A 32-bit probe loading a 64-bit DLL fails at LoadLibrary with
REM 0xc000007b (STATUS_INVALID_IMAGE_FORMAT), which reads as "not designed to
REM run on Windows" and looks like a broken DLL rather than a wrong prompt.
REM vcvars sets this; a plain cmd has it empty.
if /i not "%VSCMD_ARG_TGT_ARCH%"=="x64" (
    echo error: this prompt targets "%VSCMD_ARG_TGT_ARCH%", not x64 1>&2
    echo   Open "x64 Native Tools Command Prompt for VS 2022" and run this again. 1>&2
    echo   cl there reports Hostx64/x64; the x86 prompt builds a 32-bit probe 1>&2
    echo   that cannot load the 64-bit DLL Go produces. 1>&2
    exit /b 1
)

if not exist build mkdir build

echo ==^> go build -buildmode=c-shared
REM Same flag the archive build uses. mingw-w64 defaults __USE_MINGW_ANSI_STDIO
REM on, which turns fprintf into __mingw_vfprintf; harmless here because Go
REM links the DLL itself, but kept identical so this differs from the real
REM build in exactly ONE way -- c-shared instead of c-archive.
set "CGO_ENABLED=1"
set "CGO_CFLAGS=-O2 -g -D__USE_MINGW_ANSI_STDIO=0"
go build -buildmode=c-shared -o build\omegassh.dll .\capi || exit /b 1

echo ==^> cl dllprobe.cpp
cl /nologo /EHsc /std:c++17 /Fe:build\dllprobe.exe /Fo:build\ ^
   tests\win\dllprobe.cpp || exit /b 1

echo ==^> architectures ^(these must match^)
for %%F in (build\omegassh.dll build\dllprobe.exe) do (
    for /f "tokens=1,2" %%A in ('dumpbin /nologo /headers %%F ^| findstr /i "machine ("') do (
        echo     %%F  %%A %%B
    )
)

echo.
echo ==^> running
build\dllprobe.exe build\omegassh.dll build\dllprobe-vault.json
echo.
echo exit code: %ERRORLEVEL%

endlocal