<#
.SYNOPSIS
    Tests the disk controller core (ans\core) and the miniport's pure parts with the
    EWDK: runs the model test and the host test, and builds the core as ARM64 Windows
    kernel code.

.DESCRIPTION
    Checks the EWDK ISO, mounts it and sets up its build environment, as build.ps1
    does (both use ewdk.ps1). Then:
      1. Builds the model test (ans\core\test) as an x64 console program with the
         EWDK's MSVC, warnings as errors and AddressSanitizer, and runs it.
      2. Builds the host test (ans\test) the same way and runs it: the miniport's SCSI
         layer, _DSD parser and log formatter (ans\scsi.c, dsd.c, format.c).
      3. Builds ans\core\ans_core.vcxproj: the core as a static library with the
         miniport's toolset and options (Release|ARM64, kernel mode, /W4 /WX).
    Stops at the first failure. Nothing else is installed or used.

    Output (everything under out\test\ is generated and deleted at the start of each run):
      out\test\model_test.exe                  the model test (model_test.exe -v <name>
                                               runs the tests whose name contains <name>
                                               and prints the core's log)
      out\test\host_test.exe                   the host test (host_test.exe <name> runs
                                               the tests whose name contains <name>)
      out\test\core-arm64\bin\ans_core.lib     the core as ARM64 kernel code
      out\test\core-arm64\msbuild.log

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

$repo = $PSScriptRoot
$core = Join-Path $repo 'ans\core'
$out  = Join-Path $repo 'out\test'
$obj  = Join-Path $out 'obj'
$exe  = Join-Path $out 'model_test.exe'
$ans  = Join-Path $repo 'ans'
$hobj = Join-Path $out 'host-obj'
$hexe = Join-Path $out 'host_test.exe'
$km   = Join-Path $out 'core-arm64'

if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }
New-Item -ItemType Directory -Path $obj | Out-Null
New-Item -ItemType Directory -Path $hobj | Out-Null

Use-Ewdk $IsoPath {
    param($root)
    $cmdFile = Join-Path $out 'test.cmd'
    # A batch file, because SetupBuildEnv.cmd only sets up a cmd.exe environment.
    # The model test is an ordinary x64 program: it needs the SDK's user-mode headers
    # and libraries, which SetupBuildEnv.cmd does not put on INCLUDE and LIB.
    # Arguments that contain '=' or ';' are quoted for cmd.exe.
    @"
@echo off
call "${root}BuildEnv\SetupBuildEnv.cmd" amd64 >nul 2>&1
@echo off
where cl >nul 2>&1 || (echo cl.exe not found after SetupBuildEnv.cmd & exit /b 1)
set "INCLUDE=%VCToolsInstallDir%include;%WindowsSdkDir%Include\%Version_Number%\ucrt;%WindowsSdkDir%Include\%Version_Number%\um;%WindowsSdkDir%Include\%Version_Number%\shared"
set "LIB=%VCToolsInstallDir%lib\x64;%WindowsSdkDir%Lib\%Version_Number%\ucrt\x64;%WindowsSdkDir%Lib\%Version_Number%\um\x64"
echo === model test (x64, AddressSanitizer)
cl /nologo /std:c11 /W4 /WX /O1 /Zi /fsanitize=address /I"$core" /Fo"$obj\\" /Fd"$obj\\" /Fe"$exe" "$core\*.c" "$core\test\*.c" >"$obj\cl.log" || (type "$obj\cl.log" & exit /b 1)
"$exe" || exit /b 1
echo === host test of the miniport's SCSI layer, _DSD parser and formatter (x64, AddressSanitizer)
cl /nologo /std:c11 /W4 /WX /O1 /Zi /fsanitize=address /I"$ans" /Fo"$hobj\\" /Fd"$hobj\\" /Fe"$hexe" "$ans\scsi.c" "$ans\dsd.c" "$ans\format.c" "$ans\test\host_test.c" >"$hobj\cl.log" || (type "$hobj\cl.log" & exit /b 1)
"$hexe" || exit /b 1
echo === the core as ARM64 kernel code
msbuild "$core\ans_core.vcxproj" /nologo /m:1 /v:minimal "/p:Configuration=Release" "/p:Platform=ARM64" "/p:OutDir=$km\bin\\" "/p:IntDir=$km\obj\\" "/flp:LogFile=$km\msbuild.log;Verbosity=normal"
exit /b %ERRORLEVEL%
"@ | Set-Content -LiteralPath $cmdFile -Encoding Ascii
    Invoke-Checked 'cmd.exe' @('/d', '/c', $cmdFile)
}

$lib = Join-Path $km 'bin\ans_core.lib'
if (-not (Test-Path -LiteralPath $lib)) { throw "The build did not produce $lib" }
Write-Host 'Model test and host test passed; the core builds as ARM64 kernel code.'
