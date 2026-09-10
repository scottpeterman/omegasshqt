@echo off
REM scripts\bundle-windows-msix.bat
REM
REM Wraps the folder that scripts\bundle-windows.bat produces into an MSIX.
REM
REM   scripts\bundle-windows-msix.bat
REM   scripts\bundle-windows-msix.bat --sign-self
REM   scripts\bundle-windows-msix.bat --cert mycert.pfx --password ****
REM
REM RUN bundle-windows.bat FIRST. This one packages dist\omega and does not
REM build anything, deliberately: the MSIX is a wrapper around exactly the
REM folder you have already run, so if the folder works and the MSIX does not,
REM the packaging is what changed and not the build.
REM
REM NEVER RUN. Written against the documented behaviour of makeappx and
REM signtool; no Windows was available to test it. Every step echoes what it is
REM doing. Treat the first run as the test.
REM
REM SIGNING IS NOT OPTIONAL FOR MSIX. Unlike an MSI or an Inno installer, which
REM will run unsigned with a SmartScreen warning, Windows refuses to INSTALL an
REM unsigned MSIX at all. The three routes:
REM
REM   --sign-self     generates a self-signed certificate and signs with it.
REM                   Everyone installing it must first import the .cer into
REM                   Local Machine \ Trusted People. Fine for testing, not
REM                   something to ask strangers to do.
REM   --cert <pfx>    a real code signing certificate. Installs anywhere.
REM   the Store       Microsoft re-signs; you upload unsigned. Skip signing
REM                   here entirely and submit the .msix from --no-sign.
REM
REM THE MSVC RUNTIME IS CHECKED HERE TOO, not just in bundle-windows.bat.
REM Packaging a folder that is missing VCRUNTIME140.dll produces an MSIX that
REM installs cleanly, appears in the Start menu, and dies on launch -- and
REM under MSIX the missing-DLL dialog may not appear at all, so the only
REM symptom is a tile that does nothing. Refusing to pack is cheaper than
REM debugging that. It is a check and not a copy: the fix belongs in
REM bundle-windows.bat so the zip and the MSIX carry the same files.
REM
REM WHAT TO TEST FIRST, BEFORE SPENDING ANY MONEY ON A CERTIFICATE. MSIX runs
REM packaged applications with filesystem and registry redirection. Omega's
REM whole premise is that ~\.nterm is SHARED with nterm-qt -- the same
REM config.json, the same sessions.db. If those writes get virtualized into the
REM package's private store, both applications keep working and silently stop
REM seeing each other's sessions, which is worse than a crash. %USERPROFILE%
REM is not %LOCALAPPDATA% and probably passes through untouched, but "probably"
REM is doing the work in that sentence. Install a self-signed build, save a
REM session, and check it shows up in nterm-qt. Check the vault too: the
REM keyring backend is wincred, and package identity can change what a process
REM sees in Credential Manager.

setlocal enabledelayedexpansion

set "ROOT=%~dp0.."
pushd "%ROOT%"

set "STAGE=dist\omega"
set "MSIXDIR=dist\msix"
set "MANIFEST=packaging\msix\AppxManifest.xml"
set "ASSETS=art\msix"
set "SIGNMODE=none"
set "CERTFILE="
set "CERTPASS="
set "SUBJECT=CN=Scott Peterman"

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="--sign-self" (set "SIGNMODE=self" & shift & goto parse)
if /i "%~1"=="--no-sign" (set "SIGNMODE=none" & shift & goto parse)
if /i "%~1"=="--cert" (
    if "%~2"=="" (echo error: --cert needs a .pfx path 1>&2 & goto fail)
    set "SIGNMODE=pfx" & set "CERTFILE=%~f2" & shift & shift & goto parse
)
if /i "%~1"=="--password" (
    set "CERTPASS=%~2" & shift & shift & goto parse
)
if /i "%~1"=="--subject" (
    if "%~2"=="" (echo error: --subject needs a value 1>&2 & goto fail)
    set "SUBJECT=%~2" & shift & shift & goto parse
)
if /i "%~1"=="--help" goto usage
if /i "%~1"=="-h" goto usage
echo error: unknown option: %~1 1>&2
goto fail
:parsed

REM --- what has to be there already ------------------------------------------

if not exist "%STAGE%\omega.exe" (
    echo error: no %STAGE%\omega.exe 1>&2
    echo     Run scripts\bundle-windows.bat first; this packages what that
    echo     produces rather than building its own copy.
    goto fail
)
if not exist "%MANIFEST%" (
    echo error: no %MANIFEST% 1>&2
    goto fail
)
if not exist "%ASSETS%\StoreLogo.png" (
    echo error: no %ASSETS%\StoreLogo.png 1>&2
    echo     Generate the tile assets:  python art\make-icons.py
    goto fail
)

REM --- the MSVC runtime ------------------------------------------------------
REM
REM windeployqt does not deploy these unless asked, and an older
REM bundle-windows.bat did not ask -- so a dist\omega staged by it looks
REM complete and is not. Checked by name because that is the only way to tell:
REM the folder has ~40 DLLs in it either way.
REM
REM ucrtbase.dll is deliberately absent from this list. The UCRT ships with
REM Windows from 10 onward, which is below the manifest's 10.0.19041 floor.

set "CRTMISSING="
for %%F in (VCRUNTIME140.dll VCRUNTIME140_1.dll MSVCP140.dll) do (
    if not exist "%STAGE%\%%F" set "CRTMISSING=!CRTMISSING! %%F"
)
if not "!CRTMISSING!"=="" (
    echo error: the MSVC runtime is missing from %STAGE%: !CRTMISSING! 1>&2
    echo     The package would install and then fail to start on any machine
    echo     without the VS 2022 redistributable -- including, under MSIX,
    echo     with no error dialog at all.
    echo.
    echo     Fix it in the staging step rather than here, so the zip and the
    echo     MSIX ship the same files:
    echo         scripts\bundle-windows.bat --zip
    echo     which now passes --compiler-runtime to windeployqt and copies the
    echo     three DLLs from %%VCToolsRedistDir%% if that leaves them out.
    goto fail
)

REM windeployqt sometimes drops the redist INSTALLER beside the exe instead of
REM the DLLs. Inside an MSIX that is worse than useless: the package cannot run
REM it, and an .exe demanding elevation inside a packaged application is the
REM kind of thing Store submission asks about.
if exist "%STAGE%\vc_redist.x64.exe" (
    echo ==^> dropping %STAGE%\vc_redist.x64.exe from the layout
)

REM --- the SDK tools ---------------------------------------------------------
REM
REM makeappx and signtool live in the Windows SDK, which is NOT on PATH in a
REM plain Developer Command Prompt. Newest SDK first, same descending-sort
REM trick the Qt lookup uses.

set "SDKBIN="
for /f "delims=" %%D in ('dir /b /ad /o-n "C:\Program Files (x86)\Windows Kits\10\bin\10.*" 2^>nul') do (
    if "!SDKBIN!"=="" (
        if exist "C:\Program Files (x86)\Windows Kits\10\bin\%%D\x64\makeappx.exe" (
            set "SDKBIN=C:\Program Files (x86)\Windows Kits\10\bin\%%D\x64"
        )
    )
)
if "%SDKBIN%"=="" (
    where makeappx.exe >nul 2>nul
    if not errorlevel 1 for /f "delims=" %%P in ('where makeappx.exe') do set "SDKBIN=%%~dpP"
)
if "%SDKBIN%"=="" (
    echo error: makeappx.exe not found. 1>&2
    echo     It ships in the Windows SDK, under
    echo     C:\Program Files ^(x86^)\Windows Kits\10\bin\^<version^>\x64.
    echo     Install "Windows 10/11 SDK" from the Visual Studio Installer.
    goto fail
)
echo ==^> SDK tools at %SDKBIN%

REM --- assemble --------------------------------------------------------------
REM
REM Into a copy rather than in place: dist\omega is the portable package and
REM stays shippable on its own. An MSIX layout is that folder plus a manifest
REM and an Assets directory, and leaving those behind in the zip would ship two
REM things that only one of them needs.

echo ==^> assembling %MSIXDIR%
if exist "%MSIXDIR%" rmdir /s /q "%MSIXDIR%"
mkdir "%MSIXDIR%"
xcopy /e /i /q /y "%STAGE%" "%MSIXDIR%" >nul
if errorlevel 1 goto fail
if exist "%MSIXDIR%\vc_redist.x64.exe" del /q "%MSIXDIR%\vc_redist.x64.exe"
mkdir "%MSIXDIR%\Assets"
xcopy /q /y "%ASSETS%\*.png" "%MSIXDIR%\Assets" >nul
if errorlevel 1 goto fail
copy /y "%MANIFEST%" "%MSIXDIR%\AppxManifest.xml" >nul
if errorlevel 1 goto fail

REM --- pack ------------------------------------------------------------------

set "PKG=dist\Omega-0.1.0-x64.msix"
echo ==^> packing %PKG%
if exist "%PKG%" del /q "%PKG%"
"%SDKBIN%\makeappx.exe" pack /d "%MSIXDIR%" /p "%PKG%" /o
if errorlevel 1 (
    echo.
    echo     makeappx names one missing or mismatched asset at a time. If it
    echo     complained about a file, check %ASSETS% has it and rerun
    echo     python art\make-icons.py if not.
    goto fail
)

REM --- sign ------------------------------------------------------------------

if "%SIGNMODE%"=="none" (
    echo ==^> not signed.
    echo     Windows will NOT install an unsigned MSIX. Either sign it
    echo     ^(--sign-self for testing, --cert for real^) or submit this file
    echo     to the Store, which signs it for you.
    goto done
)

REM REUSED IF IT IS ALREADY THERE. Generating a new certificate on every run
REM means a new key on every run, so the .cer already imported into
REM TrustedPeople no longer matches what signed the package -- and Windows
REM refuses the install with 0x800B0109, "terminated in a root certificate
REM which is not trusted", which reads like the import never worked rather
REM than like the certificate changed underneath it. Every run would also
REM leave another orphaned certificate in Cert:\CurrentUser\My.
REM
REM Delete dist\omega-test.pfx to force a fresh one.
if "%SIGNMODE%"=="self" if exist "dist\omega-test.pfx" (
    echo ==^> reusing dist\omega-test.pfx
    set "CERTFILE=dist\omega-test.pfx"
    set "CERTPASS=omega"
    set "SIGNMODE=pfx-self"
)

if "%SIGNMODE%"=="self" (
    echo ==^> generating a self-signed certificate for %SUBJECT%
    REM The subject MUST equal the manifest's Publisher exactly, or signtool
    REM refuses with a publisher-mismatch error. Both default to the same
    REM string here; change them together or not at all.
    powershell -NoProfile -Command ^
        "$c = New-SelfSignedCertificate -Type Custom -Subject '%SUBJECT%'" ^
        " -KeyUsage DigitalSignature -FriendlyName 'Omega test signing'" ^
        " -CertStoreLocation 'Cert:\CurrentUser\My'" ^
        " -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3','2.5.29.19={text}');" ^
        " Export-PfxCertificate -Cert $c -FilePath 'dist\omega-test.pfx'" ^
        " -Password (ConvertTo-SecureString -String 'omega' -Force -AsPlainText) | Out-Null;" ^
        " Export-Certificate -Cert $c -FilePath 'dist\omega-test.cer' | Out-Null"
    if errorlevel 1 goto fail
    set "CERTFILE=dist\omega-test.pfx"
    set "CERTPASS=omega"
)

echo ==^> signing
"%SDKBIN%\signtool.exe" sign /fd SHA256 /a /f "%CERTFILE%" /p "%CERTPASS%" "%PKG%"
if errorlevel 1 (
    echo.
    echo     If this said the publisher name does not match the certificate
    echo     subject: the Publisher in %MANIFEST% and the certificate subject
    echo     have to be identical, character for character.
    goto fail
)

if "%SIGNMODE%"=="self" goto selfnote
if "%SIGNMODE%"=="pfx-self" goto selfnote
goto done

:selfnote
echo.
echo Self-signed. In an ELEVATED POWERSHELL ^(not cmd^), once per machine:
echo       Import-Certificate -FilePath dist\omega-test.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople

:done
echo.
echo   %PKG%
echo.
echo install it with:
echo       Add-AppxPackage -Path %PKG%
echo.
echo CHECK IT ON A MACHINE THAT DID NOT BUILD IT. This box has the VS runtime
echo and Qt on PATH, so a package missing either one still runs here.
echo.
echo THEN CHECK THE THING THAT MATTERS: save a session, and confirm it appears
echo in nterm-qt. MSIX redirects some writes into a per-package store, and if
echo ~\.nterm is one of them the two applications stop sharing sessions without
echo either of them failing.
popd
exit /b 0

:usage
echo Wraps dist\omega into an MSIX. Run scripts\bundle-windows.bat first.
echo.
echo   scripts\bundle-windows-msix.bat [--sign-self ^| --cert ^<pfx^> [--password ^<pw^>] ^| --no-sign]
echo                                   [--subject "CN=Your Name"]
popd
exit /b 0

:fail
echo.
echo msix packaging failed
popd
exit /b 1