<#
.SYNOPSIS
    Builds every driver in this repository for Release|ARM64 with the EWDK.

.DESCRIPTION
    Checks the EWDK ISO (size and SHA-256), mounts it, sets up its build environment
    (BuildEnv\SetupBuildEnv.cmd amd64), runs its MSBuild and checks each INF with its
    infverif. WDK test signing and Inf2Cat are turned off in the projects; sign.ps1 does
    the signing.

    The driver's log names the build: the commit of this repository (12 hex digits),
    with "_dirty" appended when the working tree has changes. Without git it is left out.

    Output (everything under out\ is generated and deleted at the start of each build):
      out\ans\ans.sys, out\ans\ans.inf    the unsigned driver package
      out\build\                          intermediate files, ans.pdb and msbuild.log

.PARAMETER IsoPath
    The EWDK ISO. Default: EWDK_ge_release_svc_prod1_26100_250904-1728.iso in Downloads.
#>
[CmdletBinding()]
param(
    [string]$IsoPath = (Join-Path $env:USERPROFILE 'Downloads\EWDK_ge_release_svc_prod1_26100_250904-1728.iso')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'ewdk.ps1')

$repo  = $PSScriptRoot
$out   = Join-Path $repo 'out'
$build = Join-Path $out 'build'
$drivers = @('ans')

if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }
New-Item -ItemType Directory -Path $build | Out-Null

# The build id that goes into the driver's log (see above).
$buildId = ''
if (Get-Command git -ErrorAction SilentlyContinue) {
    $commit = (& git -C $repo rev-parse --short=12 HEAD 2>$null)
    if ($LASTEXITCODE -eq 0 -and $commit -match '^[0-9a-f]{12}$') {
        $buildId = $commit
        if (& git -C $repo status --porcelain 2>$null) { $buildId += '_dirty' }
    }
}
$buildIdArg = if ($buildId) { "`"/p:AnsBuildId=$buildId`"" } else { '' }
Write-Host "Build id: $(if ($buildId) { $buildId } else { '(none)' })"

Use-Ewdk $IsoPath {
    param($root)
    foreach ($name in $drivers) {
        $project = Join-Path $repo "$name\$name.vcxproj"
        $cmdFile = Join-Path $build "msbuild-$name.cmd"
        # A batch file, because SetupBuildEnv.cmd only sets up a cmd.exe environment.
        # Arguments that contain '=' or ';' are quoted for cmd.exe.
        @"
@echo off
call "${root}BuildEnv\SetupBuildEnv.cmd" amd64 >nul 2>&1
@echo off
where msbuild >nul 2>&1 || (echo msbuild.exe not found after SetupBuildEnv.cmd & exit /b 1)
msbuild "$project" /nologo /m:1 /v:minimal "/p:Configuration=Release" "/p:Platform=ARM64" "/p:OutDir=$build\bin\$name\\" "/p:IntDir=$build\obj\$name\\" "/p:PackageDir=$out\$name\\" "/flp:LogFile=$build\msbuild-$name.log;Verbosity=normal" $buildIdArg
exit /b %ERRORLEVEL%
"@ | Set-Content -LiteralPath $cmdFile -Encoding Ascii
        Invoke-Checked 'cmd.exe' @('/d', '/c', $cmdFile)

        # The WDK's own InfVerif step does not run for this project type; check the INF
        # here against the Windows Driver requirements (/w).
        $infverif = Join-Path $root "Program Files\Windows Kits\10\Tools\$EwdkKitVersion\x64\infverif.exe"
        Invoke-Checked $infverif @('/v', '/w', (Join-Path $out "$name\$name.inf"))
    }
}

foreach ($name in $drivers) {
    $package = Join-Path $out $name
    foreach ($file in @("$name.sys", "$name.inf")) {
        $path = Join-Path $package $file
        if (-not (Test-Path -LiteralPath $path)) { throw "The build did not produce $path" }
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        Write-Host "$hash  $path"
    }
}
Write-Host 'Build done. Next: sign.ps1'
