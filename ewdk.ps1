# Shared by build.ps1 and sign.ps1: the one EWDK ISO this repository builds with.
#
# The ISO is pinned by size and SHA-256. Mounting an ISO needs no administrator rights.
# Use-Ewdk mounts it (unless it is already mounted), runs the script block with the
# drive's root, and dismounts it again if it mounted it.

$EwdkIsoName   = 'EWDK_ge_release_svc_prod1_26100_250904-1728.iso'
$EwdkIsoSize   = 20002537472
$EwdkIsoSha256 = '9F48251DD24AD31AAC206D8256E95BDA5F90A9783982C45A8AAFEB9054562379'
$EwdkKitVersion = '10.0.26100.0'

function Assert-EwdkIso([string]$IsoPath) {
    if (-not (Test-Path -LiteralPath $IsoPath -PathType Leaf)) {
        throw "EWDK ISO not found: $IsoPath (expected $EwdkIsoName, SHA-256 $EwdkIsoSha256)"
    }
    $size = (Get-Item -LiteralPath $IsoPath).Length
    if ($size -ne $EwdkIsoSize) {
        throw "EWDK ISO has $size bytes, expected ${EwdkIsoSize}: $IsoPath"
    }
    Write-Host "Checking the SHA-256 of $IsoPath ..."
    $hash = (Get-FileHash -LiteralPath $IsoPath -Algorithm SHA256).Hash
    if ($hash -ne $EwdkIsoSha256) {
        throw "EWDK ISO SHA-256 is $hash, expected ${EwdkIsoSha256}: $IsoPath"
    }
}

function Use-Ewdk([string]$IsoPath, [scriptblock]$Body) {
    Assert-EwdkIso $IsoPath
    $IsoPath = (Resolve-Path -LiteralPath $IsoPath).Path
    $mountedHere = $false
    if (-not (Get-DiskImage -ImagePath $IsoPath).Attached) {
        Mount-DiskImage -ImagePath $IsoPath -StorageType ISO | Out-Null
        $mountedHere = $true
    }
    try {
        $letter = (Get-DiskImage -ImagePath $IsoPath | Get-Volume).DriveLetter
        if (-not $letter) { throw "The EWDK ISO is mounted but has no drive letter: $IsoPath" }
        & $Body "${letter}:\"
    } finally {
        if ($mountedHere) {
            Dismount-DiskImage -ImagePath $IsoPath | Out-Null
        }
    }
}

# Runs a native program and stops when it fails.
function Invoke-Checked([string]$Exe, [string[]]$Arguments) {
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Exe exited with $LASTEXITCODE"
    }
}
