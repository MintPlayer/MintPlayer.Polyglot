#!/usr/bin/env pwsh
# Local C++ line coverage (Windows / MSVC) — wave-2 slice 7.
#
# Runs OpenCppCoverage over the in-process unit/golden suite (and optionally a CLI transpile sweep of
# the conformance programs, so emitter paths count too) and writes an HTML report to x64\coverage\.
#
# NOTE what the OpenCppCoverage half measures: the C++ Core/CLI only. The four backends are JSON
# plugin templates and gcov/OpenCppCoverage cannot see them at all — their instrument is the arm
# tracer below (-IncludeConformanceSweep), which reports per-manifest lcov. Neither substitutes for
# the other, and neither substitutes for the differential conformance suite proving the four targets
# agree at runtime.
#
# Produces the same artifact shapes CI uploads (Cobertura + plugin lcov), so a local run can be
# checked against the dashboard's numbers. This script never uploads — the server's history stays
# CI-authored.
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

# OpenCppCoverage writes DRIVE-relative backslash paths (`<source>C:</source>` plus
# `filename="Repos\MintPlayer.Polyglot\src\x.cpp"`), where gcovr on CI writes repo-relative
# forward-slash ones. The coverage server resolves paths by longest-suffix match against
# `git ls-files` and drops a non-matching path SILENTLY, so without this the local report would only
# LOOK like the one CI uploads — a wrong number rather than an error, the §4.E trap.
# `scripts/verify-coverage-paths.ps1` is the guard that catches it if this ever regresses.
function Repair-CoberturaPaths {
    param([string]$XmlPath, [string]$RepoRoot)
    if (-not (Test-Path $XmlPath)) { return }
    $drive = $RepoRoot.Substring(0, 2)                                    # "C:"
    $prefix = (($RepoRoot -replace '^[A-Za-z]:[\\/]', '') -replace '/', '\').TrimEnd('\') + '\'
    $text = [System.IO.File]::ReadAllText($XmlPath)
    $text = $text.Replace("<source>$drive</source>", '<source>.</source>')
    $text = [regex]::Replace($text, 'filename="([^"]*)"', {
        param($m)
        $p = $m.Groups[1].Value
        if ($p.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            $p = $p.Substring($prefix.Length)
        }
        'filename="' + ($p -replace '\\', '/') + '"'
    })
    [System.IO.File]::WriteAllText($XmlPath, $text)
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
    # The arm trace accumulates across every invocation below (it appends), giving the plugin-manifest
    # half of the picture the C++ instrument is blind to.
    $armTrace = Join-Path $out "arm-trace.tsv"
    $lspLeg = Join-Path $repo "tests\lsp\run-lsp.ps1"
    $driver = Join-Path $out "sweep.ps1"
    @"
foreach (`$p in Get-ChildItem "$(Join-Path $repo 'tests\conformance\programs')" -Filter *.pg) {
    foreach (`$t in 'csharp','typescript','python','php') {
        & "$cli" --emit-arm-trace "$armTrace" build `$p.FullName --target `$t --lib io --out "$sweepOut" *> `$null
    }
}
# Multi-file programs build the way the conformance runner builds them — entry.pg + --root, with a
# pgconfig naming every target instead of a single --target. Sweeping them as loose files would miss
# it entirely: the linked-module arms (`module.linked` -> `partial `, attribute imports, module
# globals) only fire on a config-sourced multi-module build.
foreach (`$d in Get-ChildItem "$(Join-Path $repo 'tests\conformance\programs')" -Directory) {
    `$entry = Join-Path `$d.FullName 'entry.pg'
    if (-not (Test-Path `$entry)) { continue }
    `$w = Join-Path "$sweepOut" `$d.Name
    New-Item -ItemType Directory -Force `$w | Out-Null
    Copy-Item -Recurse "`$(`$d.FullName)\*" `$w -Force
    '{ "targets": ["csharp","typescript","python","php"] }' | Set-Content (Join-Path `$w 'pgconfig.json')
    & "$cli" --emit-arm-trace "$armTrace" build (Join-Path `$w 'entry.pg') --root `$w --lib io --out `$w *> `$null
}
# Past `build`, or the diagnostic and front-end-only paths read as dead code: refusals, check, and
# the LSP protocol leg (reused rather than reinvented — it already drives a scripted stdio session).
foreach (`$p in Get-ChildItem "$(Join-Path $repo 'tests\refusals\fixtures')" -Filter *.pg) {
    & "$cli" --emit-arm-trace "$armTrace" build `$p.FullName --target csharp --lib io --out "$sweepOut" *> `$null
    & "$cli" --emit-arm-trace "$armTrace" check `$p.FullName --lib io *> `$null
}
if (Test-Path "$lspLeg") { & "$lspLeg" -Cli "$cli" *> `$null }
"@ | Set-Content $driver
    & $occ.Source --sources $srcFilter --export_type "binary:$cov2" --cover_children -- pwsh -NoProfile -File $driver | Out-Null

    & $occ.Source --sources $srcFilter --export_type "html:$out" --export_type "cobertura:$out\cobertura.xml" `
        --input_coverage $cov1 --input_coverage $cov2 | Out-Null

    if (Test-Path $armTrace) {
        Write-Host ""
        Write-Host "Plugin arm coverage (the JSON manifests — invisible to OpenCppCoverage):"
        & (Join-Path $PSScriptRoot "arm-trace-to-lcov.ps1") -Trace $armTrace -OutDir (Join-Path $repo "coverage\plugins")
    }
} else {
    & $occ.Source --sources $srcFilter --export_type "html:$out" --export_type "cobertura:$out\cobertura.xml" `
        --cover_children -- $tests | Out-Null
}

Repair-CoberturaPaths -XmlPath "$out\cobertura.xml" -RepoRoot $repo

Write-Host ""
Write-Host "C++ coverage (Core/CLI): $out\index.html"
Write-Host "C++ Cobertura:           $out\cobertura.xml"
if (-not $IncludeConformanceSweep) {
    Write-Host "(plugin manifests NOT measured — re-run with -IncludeConformanceSweep for arm coverage)"
}
