#!/usr/bin/env pwsh
# Local C++ line coverage (Windows / MSVC) — wave-2 slice 7.
#
# Runs OpenCppCoverage over the in-process unit/golden suite (and optionally a CLI transpile sweep of
# the conformance programs, so emitter paths count too) and writes an HTML report to x64\coverage\.
#
# NOTE what this measures: the C++ Core/CLI only. The four backends are JSON plugin templates — their
# coverage instrument is the differential conformance suite, not gcov/OpenCppCoverage.
#
# Usage:  pwsh scripts/coverage.ps1 [-Configuration Debug|Release] [-IncludeConformanceSweep]
# Needs:  choco install opencppcoverage   (and a built solution — see CLAUDE.md)

param(
    [string]$Configuration = "Debug",
    [switch]$IncludeConformanceSweep
)

$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent

$occ = Get-Command OpenCppCoverage -ErrorAction SilentlyContinue
if (-not $occ) {
    Write-Host "OpenCppCoverage not found on PATH — install it with:  choco install opencppcoverage"
    exit 2
}

$tests = Join-Path $repo "x64\$Configuration\MintPlayer.Polyglot.Tests.exe"
$cli = Join-Path $repo "x64\$Configuration\MintPlayer.Polyglot.Cli.exe"
if (-not (Test-Path $tests)) {
    Write-Host "test exe not found at $tests — build the solution first (see CLAUDE.md)."
    exit 2
}

$out = Join-Path $repo "x64\coverage"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null

$srcFilter = Join-Path $repo "src"

if ($IncludeConformanceSweep -and (Test-Path $cli)) {
    # Two runs merged via the binary intermediate format: unit suite + a four-target CLI sweep.
    $cov1 = Join-Path $out "units.cov"
    & $occ.Source --sources $srcFilter --export_type "binary:$cov1" --cover_children -- $tests | Out-Null

    $sweepOut = Join-Path $out "sweep-out"
    New-Item -ItemType Directory -Force $sweepOut | Out-Null
    $cov2 = Join-Path $out "sweep.cov"
    $programs = Get-ChildItem (Join-Path $repo "tests\conformance\programs") -Filter *.pg
    $driver = Join-Path $out "sweep.ps1"
    @"
foreach (`$p in Get-ChildItem "$(Join-Path $repo 'tests\conformance\programs')" -Filter *.pg) {
    foreach (`$t in 'csharp','typescript','python','php') {
        & "$cli" build `$p.FullName --target `$t --lib io --out "$sweepOut" *> `$null
    }
}
"@ | Set-Content $driver
    & $occ.Source --sources $srcFilter --export_type "binary:$cov2" --cover_children -- pwsh -NoProfile -File $driver | Out-Null

    & $occ.Source --sources $srcFilter --export_type "html:$out" --input_coverage $cov1 --input_coverage $cov2 | Out-Null
} else {
    & $occ.Source --sources $srcFilter --export_type "html:$out" --cover_children -- $tests | Out-Null
}

Write-Host ""
Write-Host "Coverage report: $out\index.html"
