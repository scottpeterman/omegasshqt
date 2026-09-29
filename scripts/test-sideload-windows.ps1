<#
  scripts\test-sideload-windows.ps1

  Sideload-test an MSIX without touching the Store upload. It copies the
  package into dist\sideload\, signs the COPY with the dev certificate (via
  scripts\sign-msix.ps1, which also trusts that cert in
  LocalMachine\TrustedPeople), and installs it. dist\Omega-<ver>-x64.msix is
  left unsigned, which is what goes to Partner Center.

  Usage (Windows PowerShell 5.1, elevated for install and -RemoveCert):

    # newest dist\Omega-*-x64.msix
    powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-sideload-windows.ps1

    # a specific package
    powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-sideload-windows.ps1 -Package dist\Omega-0.1.2-x64.msix

    # what is installed, and anything MSIX redirected into the package store
    powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-sideload-windows.ps1 -Status

    # uninstall
    powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-sideload-windows.ps1 -Remove

    # uninstall and drop the dev cert from LocalMachine\TrustedPeople
    powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-sideload-windows.ps1 -Remove -RemoveCert

  Remove-AppxPackage deletes the package's private store
  (%LOCALAPPDATA%\Packages\<family>). It does NOT touch ~\.nterm -- if that
  was written through rather than redirected, sessions survive uninstall,
  which is the point.
#>
param(
    [string]$Package,
    [switch]$Status,
    [switch]$Remove,
    [switch]$RemoveCert,
    [switch]$Launch
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if ($PSVersionTable.PSEdition -eq "Core") {
    throw "Run under Windows PowerShell 5.1 (powershell.exe). The Appx cmdlets are unreliable under pwsh."
}

function Test-Elevated {
    $p = New-Object Security.Principal.WindowsPrincipal(
        [Security.Principal.WindowsIdentity]::GetCurrent())
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Identity is read from a package when there is one, else from the source
# manifest, so nothing here hardcodes the package name or publisher.
function Get-ManifestIdentity([string]$msixPath) {
    if ($msixPath) {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $zip = [System.IO.Compression.ZipFile]::OpenRead((Resolve-Path $msixPath).Path)
        try {
            $entry = $zip.Entries | Where-Object { $_.FullName -eq "AppxManifest.xml" }
            if (-not $entry) { throw "AppxManifest.xml not found inside $msixPath" }
            $reader = New-Object System.IO.StreamReader($entry.Open())
            $text = $reader.ReadToEnd()
            $reader.Dispose()
        } finally {
            $zip.Dispose()
        }
    } else {
        $text = Get-Content -Raw "packaging\msix\AppxManifest.xml"
    }
    $xml = [xml]$text
    $id  = $xml.DocumentElement.SelectSingleNode("*[local-name()='Identity']")
    $app = $xml.DocumentElement.SelectSingleNode("*[local-name()='Applications']/*[local-name()='Application']")
    [pscustomobject]@{
        Name      = $id.GetAttribute("Name")
        Publisher = $id.GetAttribute("Publisher")
        Version   = $id.GetAttribute("Version")
        AppId     = if ($app) { $app.GetAttribute("Id") } else { $null }
    }
}

function Show-Installed($ident) {
    $installed = Get-AppxPackage -Name $ident.Name
    if (-not $installed) {
        "not installed: $($ident.Name)"
        return
    }
    foreach ($p in $installed) {
        "installed : $($p.Name) $($p.Version)"
        "family    : $($p.PackageFamilyName)"
        "location  : $($p.InstallLocation)"
        "signature : $($p.SignatureKind)"

        # MSIX redirects AppData writes into LocalCache. Anything here is a
        # file the app THINKS it wrote to the real profile. ~\.nterm is not
        # under AppData, so it should never show up -- if it does, sessions
        # are no longer shared with nterm-qt.
        $cache = Join-Path $env:LOCALAPPDATA "Packages\$($p.PackageFamilyName)\LocalCache"
        if (Test-Path $cache) {
            $files = Get-ChildItem -Recurse -File $cache -ErrorAction SilentlyContinue
            if ($files) {
                ""
                "redirected writes under $cache :"
                $files | ForEach-Object { "  " + $_.FullName.Substring($cache.Length + 1) }
            } else {
                "redirected: none"
            }
        } else {
            "redirected: none (no LocalCache yet -- run the app first)"
        }
        $nterm = Join-Path $env:USERPROFILE ".nterm"
        if (Test-Path $nterm) {
            $newest = Get-ChildItem -File $nterm | Sort-Object LastWriteTime -Descending | Select-Object -First 1
            if ($newest) { "~\.nterm  : newest $($newest.Name) at $($newest.LastWriteTime)" }
        }
    }
}

function Remove-Installed($ident) {
    $installed = Get-AppxPackage -Name $ident.Name
    if (-not $installed) {
        "nothing to remove: $($ident.Name) is not installed"
    } else {
        foreach ($p in $installed) {
            "removing  : $($p.PackageFullName)"
            Remove-AppxPackage -Package $p.PackageFullName
        }
    }
    if ($RemoveCert) {
        if (-not (Test-Elevated)) { throw "-RemoveCert needs an elevated prompt (LocalMachine\TrustedPeople)." }
        $certs = Get-ChildItem Cert:\LocalMachine\TrustedPeople | Where-Object { $_.Subject -eq $ident.Publisher }
        if (-not $certs) {
            "no '$($ident.Publisher)' cert in LocalMachine\TrustedPeople"
        }
        foreach ($c in $certs) {
            "untrusting: $($c.Thumbprint) $($c.Subject)"
            Remove-Item -Path "Cert:\LocalMachine\TrustedPeople\$($c.Thumbprint)"
        }
    }
}

# --- -Status / -Remove: no package file needed ------------------------------

if ($Status -or $Remove) {
    $ident = Get-ManifestIdentity $Package
    if ($Remove) { Remove-Installed $ident; "" }
    Show-Installed $ident
    exit 0
}

# --- install ----------------------------------------------------------------

if (-not (Test-Elevated)) {
    throw "Run from an elevated PowerShell: signing trusts the dev cert in LocalMachine\TrustedPeople."
}

if (-not $Package) {
    $pick = Get-ChildItem "dist\Omega-*-x64.msix" -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $pick) { throw "No dist\Omega-*-x64.msix. Run scripts\bundle-windows-msix.bat first." }
    $Package = $pick.FullName
}
$src = (Resolve-Path $Package).Path
$ident = Get-ManifestIdentity $src
"source    : $src"
"package   : $($ident.Name) $($ident.Version)"

$sideDir = Join-Path $root "dist\sideload"
if (-not (Test-Path $sideDir)) { New-Item -ItemType Directory -Path $sideDir | Out-Null }
$copy = Join-Path $sideDir (Split-Path -Leaf $src)
Copy-Item -Force $src $copy
"copy      : $copy"

""
"==> signing the copy"
& (Join-Path $PSScriptRoot "sign-msix.ps1") -Package $copy
if (-not $?) { throw "sign-msix.ps1 failed" }

""
"==> installing"
# -ForceUpdateFromAnyVersion lets the same or an older version reinstall over
# a test build, which a plain Add-AppxPackage refuses. -ForceApplicationShutdown
# closes a running Omega instead of failing on a locked file.
try {
    Add-AppxPackage -Path $copy -ForceUpdateFromAnyVersion -ForceApplicationShutdown
} catch {
    ""
    "install failed: $($_.Exception.Message)"
    ""
    "If an earlier build signed with a different cert (or the Store build) is"
    "installed, remove it and retry:"
    "  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-sideload-windows.ps1 -Remove"
    exit 1
}

""
Show-Installed $ident

$pkg = Get-AppxPackage -Name $ident.Name | Select-Object -First 1
if ($pkg -and $ident.AppId) {
    $aumid = "$($pkg.PackageFamilyName)!$($ident.AppId)"
    ""
    "launch    : explorer.exe shell:AppsFolder\$aumid"
    if ($Launch) { Start-Process "explorer.exe" "shell:AppsFolder\$aumid" }
}

""
"test: Help > About Qt, save a session, confirm it appears in nterm-qt, then:"
"  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-sideload-windows.ps1 -Status"
"remove:"
"  powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-sideload-windows.ps1 -Remove"
"  (add -RemoveCert to also untrust the dev cert)"