#!/usr/bin/env pwsh
# One-shot build + test gate for MintPlayer.Polyglot (P35 tiered).
#
#   -Tier full (default): every stage — the ONLY pre-merge bar.
#   -Tier fast: parity -> build -> unit exe -> cli-smoke -> refusals -> lsp (~20 s + build) — the
#               mid-slice sanity ceiling CLAUDE.md prescribes, with the cheapest cross-cutting nets in.
#               fast is a subset, never a substitute: it runs NO conformance/fidelity/watch/registry/
#               samples/nullable/library/nuget coverage.
#
# Registry stage: fails the gate on failure UNLESS POLYGLOT_ALLOW_REGISTRY_SKIP=1 is set — the
# explicit local opt-out for machines whose loopback is broken (see MEMORY). CI never sets it.
#
# Exits nonzero if any stage fails. Usage:
#   pwsh scripts/build-and-test.ps1 [-Configuration Debug|Release] [-Tier fast|full]

param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",
    [ValidateSet("fast", "full")]
    [string]$Tier = "full"
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

Write-Host "`n==> CLI-surface smoke (--version / -h / bad input / four-target single build)"
& pwsh -NoProfile -File (Join-Path $repo "tests\cli\run-cli-smoke.ps1") -Cli (Join-Path $repo "x64\$Configuration\MintPlayer.Polyglot.Cli.exe")
if ($LASTEXITCODE -ne 0) { Write-Host "`nCLI SMOKE FAILED."; exit 1 }

Write-Host "`n==> Refusal gate (PRD paragraph 3.B: loud diagnostics, no output on refusal, check --json)"
& pwsh -NoProfile -File (Join-Path $repo "tests\refusals\run-refusals.ps1") -Cli (Join-Path $repo "x64\$Configuration\MintPlayer.Polyglot.Cli.exe")
if ($LASTEXITCODE -ne 0) { Write-Host "`nREFUSAL GATE FAILED."; exit 1 }

Write-Host "`n==> LSP protocol gate (framed JSON-RPC lifecycle over stdio)"
& pwsh -NoProfile -File (Join-Path $repo "tests\lsp\run-lsp.ps1") -Cli (Join-Path $repo "x64\$Configuration\MintPlayer.Polyglot.Cli.exe")
if ($LASTEXITCODE -ne 0) { Write-Host "`nLSP GATE FAILED."; exit 1 }

if ($Tier -eq "fast") {
    Write-Host "`nFast tier green: build + unit + cli-smoke + refusals + lsp."
    Write-Host "(fast is a SUBSET — run the full tier before merging.)"
    exit 0
}

Write-Host "`n==> Parser-fidelity round-trip (all samples)"
& pwsh -NoProfile -File (Join-Path $repo "tests\fidelity\run-roundtrip.ps1")
if ($LASTEXITCODE -ne 0) { Write-Host "`nROUND-TRIP FAILED."; exit 1 }

Write-Host "`n==> Watch-mode protocol gate (--watch)"
& pwsh -NoProfile -File (Join-Path $repo "tests\watch\run-watch.ps1") -Cli (Join-Path $repo "x64\$Configuration\MintPlayer.Polyglot.Cli.exe")
if ($LASTEXITCODE -ne 0) { Write-Host "`nWATCH GATE FAILED."; exit 1 }

Write-Host "`n==> Plugin auto-download gate (fake npm registry: download/lock/offline/tamper — P30)"
if ($env:POLYGLOT_ALLOW_REGISTRY_SKIP -eq "1") {
    Write-Host "   (SKIPPED: POLYGLOT_ALLOW_REGISTRY_SKIP=1 — this machine's loopback is broken; the leg runs in CI)"
} else {
    & pwsh -NoProfile -File (Join-Path $repo "tests\registry\run-registry.ps1") -Cli (Join-Path $repo "x64\$Configuration\MintPlayer.Polyglot.Cli.exe")
    if ($LASTEXITCODE -ne 0) { Write-Host "`nREGISTRY GATE FAILED. (set POLYGLOT_ALLOW_REGISTRY_SKIP=1 only if this machine's loopback is known-broken)"; exit 1 }
}

# P35 slice 3: ONE merged runner covers all four targets (C# oracle vs TS/Python/PHP) in a single transpile
# + single csc-oracle-compile per program, and stages the pristine .ts for the library gate below (so the two
# gates share one transpile pass). The staging dir outlives this stage; it is consumed by the library gate and
# removed at the end.
$confStaging = Join-Path ([System.IO.Path]::GetTempPath()) ("polyglot-libstage-" + [System.Guid]::NewGuid().ToString('N').Substring(0, 8))
Write-Host "`n==> Differential conformance (C# oracle vs TS / Python / PHP)"
& pwsh -NoProfile -File (Join-Path $repo "tests\conformance\run-conformance.ps1") -Cli (Join-Path $repo "x64\$Configuration\MintPlayer.Polyglot.Cli.exe") -StagingOut $confStaging
if ($LASTEXITCODE -ne 0) { Write-Host "`nCONFORMANCE FAILED."; Remove-Item -Recurse -Force $confStaging -ErrorAction SilentlyContinue; exit 1 }

Write-Host "`n==> Sample emit gate (each sample's C# compiles + TS runs)"
& pwsh -NoProfile -File (Join-Path $repo "tests\samples\run-emit.ps1") -Cli (Join-Path $repo "x64\$Configuration\MintPlayer.Polyglot.Cli.exe")
if ($LASTEXITCODE -ne 0) { Write-Host "`nSAMPLE EMIT GATE FAILED."; exit 1 }

Write-Host "`n==> Nullable / NRT gate (annotations preserved + clean under <Nullable>enable/>)"
& pwsh -NoProfile -File (Join-Path $repo "tests\nullable\run-nullable.ps1") -Cli (Join-Path $repo "x64\$Configuration\MintPlayer.Polyglot.Cli.exe")
if ($LASTEXITCODE -ne 0) { Write-Host "`nNULLABLE GATE FAILED."; exit 1 }

Write-Host "`n==> Library-consumption gate (emitted TS is an importable, strict-clean ES module)"
& pwsh -NoProfile -File (Join-Path $repo "tests\library\run-library.ps1") -Cli (Join-Path $repo "x64\$Configuration\MintPlayer.Polyglot.Cli.exe") -Staged $confStaging
$libExit = $LASTEXITCODE
Remove-Item -Recurse -Force $confStaging -ErrorAction SilentlyContinue
if ($libExit -ne 0) { Write-Host "`nLIBRARY GATE FAILED."; exit 1 }

# MSBuild/NuGet integration gate — the .pg-aware package auto-transpiling inside a consuming project (P11/P30).
# Needs the dotnet SDK (packs + restores + several fixture builds); guarded so a dotnet-less environment skips
# rather than fails, mirroring the conformance legs' dotnet dependency.
Write-Host "`n==> MSBuild / NuGet integration gate (.pg auto-transpile in a consuming project)"
if (Get-Command dotnet -ErrorAction SilentlyContinue) {
    & pwsh -NoProfile -File (Join-Path $repo "tests\msbuild\run-nuget.ps1")
    if ($LASTEXITCODE -ne 0) { Write-Host "`nNUGET GATE FAILED."; exit 1 }
} else {
    Write-Host "   (skipped: dotnet not found on PATH)"
}

Write-Host "`nAll green: the full tier (build + unit tests + all gates + conformance)."
exit 0
