<#
.SYNOPSIS
    Makes the test code-signing certificate that sign.ps1 uses. Run once per machine.

.DESCRIPTION
    A self-signed RSA-2048 certificate for code signing (EKU 1.3.6.1.5.5.7.3.3), valid
    for ten years. The key is made in memory and written only to the .pfx file; nothing
    is put into any certificate store or key container.

    Writes, in SigningDir:
      siliconnt-test.pfx   certificate and private key; no password; never commit or share it
      siliconnt-test.cer   certificate only; this is what a test machine has to trust

    Refuses to overwrite existing files: delete them yourself to make a new certificate
    (drivers signed with the old one then no longer match the new .cer).

.PARAMETER SigningDir
    Where the files go. Default: %USERPROFILE%\SiliconNT-build\signing. Keep it outside
    every repository.
#>
[CmdletBinding()]
param(
    [string]$SigningDir = (Join-Path $env:USERPROFILE 'SiliconNT-build\signing')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.Core   # CertificateRequest (.NET Framework 4.7.2 and later)
Add-Type -AssemblyName System.Security

$pfxPath = Join-Path $SigningDir 'siliconnt-test.pfx'
$cerPath = Join-Path $SigningDir 'siliconnt-test.cer'
foreach ($path in @($pfxPath, $cerPath)) {
    if (Test-Path -LiteralPath $path) { throw "$path already exists; not overwriting it" }
}
New-Item -ItemType Directory -Force -Path $SigningDir | Out-Null

$X509 = 'System.Security.Cryptography.X509Certificates'
$subject = 'CN=SiliconNT Test Driver Signing'

# RSACng with no key name: an ephemeral key, never stored in a key container.
$key = New-Object System.Security.Cryptography.RSACng 2048
try {
    $request = New-Object "$X509.CertificateRequest" @(
        $subject, $key,
        [System.Security.Cryptography.HashAlgorithmName]::SHA256,
        [System.Security.Cryptography.RSASignaturePadding]::Pkcs1)

    $usage = [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::DigitalSignature
    $request.CertificateExtensions.Add((New-Object "$X509.X509KeyUsageExtension" @($usage, $true)))

    $ekus = New-Object System.Security.Cryptography.OidCollection
    [void]$ekus.Add((New-Object System.Security.Cryptography.Oid '1.3.6.1.5.5.7.3.3'))  # code signing
    $request.CertificateExtensions.Add((New-Object "$X509.X509EnhancedKeyUsageExtension" @($ekus, $false)))

    $request.CertificateExtensions.Add((New-Object "$X509.X509SubjectKeyIdentifierExtension" @($request.PublicKey, $false)))

    $now = [DateTimeOffset]::UtcNow
    $cert = $request.CreateSelfSigned($now.AddDays(-1), $now.AddYears(10))
    try {
        $pfx = $cert.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Pfx)
        $cer = $cert.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert)
        [System.IO.File]::WriteAllBytes($pfxPath, $pfx)
        [System.IO.File]::WriteAllBytes($cerPath, $cer)
        Write-Host "Subject:    $($cert.Subject)"
        Write-Host "Thumbprint: $($cert.Thumbprint)"
        Write-Host "Valid:      $($cert.NotBefore.ToUniversalTime().ToString('u')) to $($cert.NotAfter.ToUniversalTime().ToString('u'))"
        Write-Host "Wrote $cerPath and the .pfx next to it."
    } finally {
        $cert.Dispose()
    }
} finally {
    $key.Dispose()
}
