#!/usr/bin/env pwsh
# One-shot build + full test gate for MintPlayer.Polyglot.
#
#   1. Build the solution (VS 2026 / toolset v145).
#   2. Run the in-process unit/golden tests (MintPlayer.Polyglot.Tests).
#   3. Run the differential C#/TS conformance suite (tests/conformance/run-diff.ps1).
#
# Exits nonzero if any stage fails. Usage:
#   pwsh scripts/build-and-test.ps1 [-Configuration Debug|Release] [-SkipConformance]

param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",
    [switch]$SkipConformance
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$sln = Join-Path $repo "MintPlayer.Polyglot.sln"

# Locate the VS 2026 MSBuild (prerelease/Insiders); fall back to the documented path (see CLAUDE.md).
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

Write-Host "==> Build-file parity (.vcxproj <-> CMake glob)"
& pwsh -NoProfile -File (Join-Path $repo "scripts\check-buildfile-parity.ps1")
if ($LASTEXITCODE -ne 0) { Write-Host "`nBUILD-FILE PARITY FAILED."; exit 1 }

Write-Host "`n==> Build ($Configuration|x64)"
& $msbuild $sln /p:Configuration=$Configuration /p:Platform=x64 /m /nologo /clp:Summary
if ($LASTEXITCODE -ne 0) { Write-Host "`nBUILD FAILED."; exit 1 }

$testsExe = Join-Path $repo "x64\$Configuration\MintPlayer.Polyglot.Tests.exe"
Write-Host "`n==> Unit / golden tests"
& $testsExe
if ($LASTEXITCODE -ne 0) { Write-Host "`nUNIT TESTS FAILED."; exit 1 }

Write-Host "`n==> Parser-fidelity round-trip (all samples)"
& pwsh -NoProfile -File (Join-Path $repo "tests\fidelity\run-roundtrip.ps1")
if ($LASTEXITCODE -ne 0) { Write-Host "`nROUND-TRIP FAILED."; exit 1 }

Write-Host "`n==> Watch-mode protocol gate (--watch)"
& pwsh -NoProfile -File (Join-Path $repo "tests\watch\run-watch.ps1") -Cli (Join-Path $repo "x64\$Configuration\MintPlayer.Polyglot.Cli.exe")
if ($LASTEXITCODE -ne 0) { Write-Host "`nWATCH GATE FAILED."; exit 1 }

if (-not $SkipConformance) {
    Write-Host "`n==> Differential conformance (C# vs TS)"
    & pwsh -NoProfile -File (Join-Path $repo "tests\conformance\run-diff.ps1")
    if ($LASTEXITCODE -ne 0) { Write-Host "`nCONFORMANCE FAILED."; exit 1 }

    Write-Host "`n==> Nullable / NRT gate (annotations preserved + clean under <Nullable>enable/>)"
    & pwsh -NoProfile -File (Join-Path $repo "tests\nullable\run-nullable.ps1") -Cli (Join-Path $repo "x64\$Configuration\MintPlayer.Polyglot.Cli.exe")
    if ($LASTEXITCODE -ne 0) { Write-Host "`nNULLABLE GATE FAILED."; exit 1 }
}

Write-Host "`nAll green: build + unit tests$(if (-not $SkipConformance) { ' + conformance' })."
exit 0
