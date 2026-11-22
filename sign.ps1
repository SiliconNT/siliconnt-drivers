<#
.SYNOPSIS
    Test-signs the driver packages that build.ps1 made.

.DESCRIPTION
    For each package in out\: signs the .sys (the signature is embedded in the file, as
    boot-start drivers need), makes the catalog with the EWDK's Inf2Cat, signs the catalog,
    and copies the certificate to out\siliconnt-test.cer next to the packages.

    The private key is read from the .pfx into memory only (EphemeralKeySet) and the files
    are signed with Set-AuthenticodeSignature: SHA-256, no timestamp server. signtool is not
    used for signing because "signtool sign /f <pfx>" copies the private key into the
    user's key store on every run. Nothing is put into any certificate or key store.

    The signing files come from new-test-cert.ps1 and stay outside the repository.

.PARAMETER IsoPath
    The EWDK ISO. Default: EWDK_ge_release_svc_prod1_26100_250904-1728.iso in Downloads.

.PARAMETER SigningDir
    Where siliconnt-test.pfx and siliconnt-test.cer are.
    Default: %USERPROFILE%\SiliconNT-build\signing.
#>
[CmdletBinding()]
param(
    [string]$IsoPath = (Join-Path $env:USERPROFILE 'Downloads\EWDK_ge_release_svc_prod1_26100_250904-1728.iso'),
    [string]$SigningDir = (Join-Path $env:USERPROFILE 'SiliconNT-build\signing')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'ewdk.ps1')

$out = Join-Path $PSScriptRoot 'out'
$drivers = @('ans')
# Inf2Cat's name for "Windows 10 and later, ARM64"; the INFs target NTarm64.10.0...16299.
$catalogOs = '10_GE_ARM64'

$pfxPath = Join-Path $SigningDir 'siliconnt-test.pfx'
$cerPath = Join-Path $SigningDir 'siliconnt-test.cer'
foreach ($path in @($pfxPath, $cerPath)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "$path not found. Make the test certificate once with new-test-cert.ps1."
    }
}
$X509 = 'System.Security.Cryptography.X509Certificates'
$publicCert = New-Object "$X509.X509Certificate2" -ArgumentList $cerPath
$thumbprint = $publicCert.Thumbprint

foreach ($name in $drivers) {
    $package = Join-Path $out $name
    foreach ($file in @("$name.sys", "$name.inf")) {
        if (-not (Test-Path -LiteralPath (Join-Path $package $file))) {
            throw "$package\$file not found. Run build.ps1 first."
        }
    }
    if ($null -ne (Get-AuthenticodeSignature -LiteralPath (Join-Path $package "$name.sys")).SignerCertificate) {
        throw "$package\$name.sys is already signed. Run build.ps1 again for a fresh package."
    }
    $cat = Join-Path $package "$name.cat"
    if (Test-Path -LiteralPath $cat) { Remove-Item -LiteralPath $cat -Force }
}

# The .pfx has no password (new-test-cert.ps1); its protection is where it is kept.
$signingCert = New-Object "$X509.X509Certificate2" -ArgumentList @(
    [System.IO.File]::ReadAllBytes($pfxPath), [string]'',
    [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet)
if ($signingCert.Thumbprint -ne $thumbprint -or -not $signingCert.HasPrivateKey) {
    throw "$pfxPath does not hold the private key of $cerPath"
}

function Set-TestSignature([string]$Path) {
    $result = Set-AuthenticodeSignature -LiteralPath $Path -Certificate $signingCert -HashAlgorithm SHA256
    # The certificate is not trusted on this machine, so the status is not "Valid";
    # what has to hold is that the file now carries our certificate's signature.
    $signer = (Get-AuthenticodeSignature -LiteralPath $Path).SignerCertificate
    if ($null -eq $signer -or $signer.Thumbprint -ne $thumbprint) {
        throw "Signing $Path failed: $($result.StatusMessage)"
    }
    Write-Host "Signed $Path"
}

try {
    Use-Ewdk $IsoPath {
        param($root)
        $inf2cat = Join-Path $root "Program Files\Windows Kits\10\bin\$EwdkKitVersion\x86\Inf2Cat.exe"
        foreach ($name in $drivers) {
            $package = Join-Path $out $name
            $cat = Join-Path $package "$name.cat"
            Set-TestSignature (Join-Path $package "$name.sys")
            Invoke-Checked $inf2cat @("/driver:$package", "/os:$catalogOs")
            if (-not (Test-Path -LiteralPath $cat)) { throw "Inf2Cat did not produce $cat" }
            Set-TestSignature $cat
        }
    }
} finally {
    $signingCert.Dispose()
}

Copy-Item -LiteralPath $cerPath -Destination (Join-Path $out 'siliconnt-test.cer') -Force

foreach ($name in $drivers) {
    foreach ($file in Get-ChildItem -LiteralPath (Join-Path $out $name) -File | Sort-Object Name) {
        $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
        Write-Host "$hash  $($file.FullName)"
    }
}
Write-Host "Signed with $thumbprint. Certificate: $out\siliconnt-test.cer"
