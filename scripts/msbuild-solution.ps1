#!/usr/bin/env pwsh
# Build the solution with the VS 2026 MSBuild (P35 slice 6: extracted from build-and-test.ps1 so the
# NX `build` target and the gate share one discovery path). Windows-only by design — POSIX builds go
# through CMake (see CLAUDE.md).

param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$sln = Join-Path $repo "MintPlayer.Polyglot.sln"

$msbuild = $null
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $msbuild = & $vswhere -latest -prerelease -requires Microsoft.Component.MSBuild `
        -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
}
if (-not $msbuild -or -not (Test-Path $msbuild)) {
    $msbuild = "C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\MSBuild.exe"
}
if (-not (Test-Path $msbuild)) {
    Write-Host "MSBuild (VS 2026) not found. Install VS 2026 with the C++ workload (toolset v145)."
    exit 2
}

& $msbuild $sln /p:Configuration=$Configuration /p:Platform=x64 /m /nologo /clp:Summary
exit $LASTEXITCODE
