<#
  Signs an MSIX with a self-signed development certificate and installs that
  certificate into LocalMachine\TrustedPeople so the package will sideload.

  The certificate Subject MUST match the Publisher attribute in the package's
  AppxManifest.xml byte-for-byte, or deployment fails with 0x800B0109
  ("A certificate chain processed, but terminated in a root certificate which
  is not trusted") or 0x80073CF3 (publisher mismatch). This script reads the
  Publisher straight out of the package so they cannot drift.

  Must be run from an elevated prompt (writing to LocalMachine\TrustedPeople).

  Usage:
    powershell -NoProfile -ExecutionPolicy Bypass -File scripts\sign-msix.ps1 dist\Omega-0.1.0-x64.msix
#>
param(
    [string]$Package = "dist\Omega-0.1.0-x64.msix",
    [string]$PfxPath = "build-win\omega-dev.pfx",
    [string]$PfxPassword = "omega-dev"
)

$ErrorActionPreference = "Stop"

# --- elevation check -------------------------------------------------------
$principal = New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this from an elevated PowerShell prompt; it writes to LocalMachine\TrustedPeople."
}

$pkg = (Resolve-Path $Package).Path

# --- pull Publisher out of the package -------------------------------------
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($pkg)
try {
    $entry = $zip.Entries | Where-Object { $_.FullName -eq "AppxManifest.xml" }
    if (-not $entry) { throw "AppxManifest.xml not found inside $pkg" }
    $reader = New-Object System.IO.StreamReader($entry.Open())
    $manifestXml = $reader.ReadToEnd()
    $reader.Dispose()
} finally {
    $zip.Dispose()
}

$xml = [xml]$manifestXml
$identity = $xml.DocumentElement.SelectSingleNode("*[local-name()='Identity']")
if (-not $identity) { throw "No <Identity> element in AppxManifest.xml" }
$publisher = $identity.GetAttribute("Publisher")
$pkgName   = $identity.GetAttribute("Name")
$pkgVer    = $identity.GetAttribute("Version")
if ([string]::IsNullOrWhiteSpace($publisher)) { throw "Identity/@Publisher is empty" }

"package : $pkgName $pkgVer"
"publisher: $publisher"

# --- find or create the signing certificate --------------------------------
$cert = Get-ChildItem Cert:\CurrentUser\My |
        Where-Object { $_.Subject -eq $publisher -and $_.HasPrivateKey } |
        Sort-Object NotAfter -Descending |
        Select-Object -First 1

if (-not $cert) {
    "creating self-signed certificate for $publisher"
    $cert = New-SelfSignedCertificate `
        -Type Custom `
        -Subject $publisher `
        -KeyUsage DigitalSignature `
        -FriendlyName "Omega development signing" `
        -CertStoreLocation "Cert:\CurrentUser\My" `
        -NotAfter (Get-Date).AddYears(3) `
        -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}")
} else {
    "reusing certificate $($cert.Thumbprint) (expires $($cert.NotAfter.ToString('yyyy-MM-dd')))"
}

# --- export pfx (for signtool) and cer (for the trust store) ---------------
$pfxDir = Split-Path -Parent $PfxPath
if ($pfxDir -and -not (Test-Path $pfxDir)) { New-Item -ItemType Directory -Path $pfxDir | Out-Null }
$cerPath = [System.IO.Path]::ChangeExtension($PfxPath, ".cer")

$securePw = ConvertTo-SecureString -String $PfxPassword -Force -AsPlainText
Export-PfxCertificate -Cert "Cert:\CurrentUser\My\$($cert.Thumbprint)" `
                      -FilePath $PfxPath -Password $securePw | Out-Null
Export-Certificate  -Cert "Cert:\CurrentUser\My\$($cert.Thumbprint)" `
                      -FilePath $cerPath | Out-Null

# --- locate signtool from the Windows SDK ----------------------------------
$signtool = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin\*\x64\signtool.exe" `
                -ErrorAction SilentlyContinue |
            Sort-Object { [version]($_.Directory.Parent.Name) } -Descending |
            Select-Object -First 1
if (-not $signtool) {
    throw "signtool.exe not found under the Windows 10/11 SDK. Install the 'Windows SDK Signing Tools' component from the Visual Studio Installer."
}
"signtool : $($signtool.FullName)"

# --- sign ------------------------------------------------------------------
& $signtool.FullName sign /fd SHA256 /a /f $PfxPath /p $PfxPassword $pkg
if ($LASTEXITCODE -ne 0) { throw "signtool failed with exit code $LASTEXITCODE" }

& $signtool.FullName verify /pa /v $pkg
if ($LASTEXITCODE -ne 0) { throw "signature verification failed with exit code $LASTEXITCODE" }

# --- trust the certificate so Add-AppxPackage will accept it ---------------
Import-Certificate -FilePath $cerPath -CertStoreLocation Cert:\LocalMachine\TrustedPeople | Out-Null
"trusted  : $cerPath -> LocalMachine\TrustedPeople"

""
"signed. install with:"
"  Add-AppxPackage -Path `"$pkg`""