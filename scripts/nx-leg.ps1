#!/usr/bin/env pwsh
# NX gate-leg dispatcher (P35 slice 6). NX runs each target's command through the platform shell
# (bash on Linux, cmd on Windows), so a `$IsWindows` written INTO an nx command string is expanded by
# that shell before pwsh sees it — on Linux bash turned `if ($IsWindows)` into `if ()`. The fix: keep
# every nx command `$`-free (`pwsh -NoProfile -File scripts/nx-leg.ps1 <leg>`) and do ALL OS logic
# here, where only pwsh evaluates it. The underlying runners are untouched and still directly
# invocable without NX (release.yml calls them straight) — this is orchestration glue only.

param([Parameter(Mandatory)][string]$Leg)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

# OS-aware artifact paths: Windows = the MSBuild x64\Debug tree; POSIX = the CMake build/ tree.
$cli = if ($IsWindows) { "x64/Debug/MintPlayer.Polyglot.Cli.exe" } else { "build/polyglot" }
$tests = if ($IsWindows) { "x64/Debug/MintPlayer.Polyglot.Tests.exe" } else { "build/polyglot-tests" }

function Invoke-Leg([scriptblock]$Body) { & $Body; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE } }

switch ($Leg) {
    "build" {
        if ($IsWindows) {
            Invoke-Leg { & pwsh -NoProfile -File scripts/msbuild-solution.ps1 }
        } else {
            # Shallow PR checkout has no tags — pin the dev version (matches release.yml's linux floor).
            Invoke-Leg { cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPOLYGLOT_VERSION=0.0.0-dev }
            Invoke-Leg { cmake --build build -j ([int](& nproc)) }
        }
    }
    "unit"      { Invoke-Leg { & $tests } }
    "parity"    { Invoke-Leg { & pwsh -NoProfile -File scripts/check-buildfile-parity.ps1 } }
    "fidelity"  { Invoke-Leg { & pwsh -NoProfile -File tests/fidelity/run-roundtrip.ps1 } }
    "nuget"     { Invoke-Leg { & pwsh -NoProfile -File tests/msbuild/run-nuget.ps1 } }
    "cli-smoke" { Invoke-Leg { & pwsh -NoProfile -File tests/cli/run-cli-smoke.ps1 -Cli $cli } }
    "refusals"  { Invoke-Leg { & pwsh -NoProfile -File tests/refusals/run-refusals.ps1 -Cli $cli } }
    "lsp"       { Invoke-Leg { & pwsh -NoProfile -File tests/lsp/run-lsp.ps1 -Cli $cli } }
    "watch"     { Invoke-Leg { & pwsh -NoProfile -File tests/watch/run-watch.ps1 -Cli $cli } }
    "registry"  { Invoke-Leg { & pwsh -NoProfile -File tests/registry/run-registry.ps1 -Cli $cli } }
    "samples"   { Invoke-Leg { & pwsh -NoProfile -File tests/samples/run-emit.ps1 -Cli $cli } }
    "nullable"  { Invoke-Leg { & pwsh -NoProfile -File tests/nullable/run-nullable.ps1 -Cli $cli } }
    "conformance" {
        # One merged run: the C# oracle vs TS/Python/PHP, staging the pristine TS for the library check.
        Invoke-Leg { & pwsh -NoProfile -File tests/conformance/run-conformance.ps1 -Cli $cli -StagingOut .nx-libstage }
        Invoke-Leg { & pwsh -NoProfile -File tests/library/run-library.ps1 -Cli $cli -Staged .nx-libstage }
        Remove-Item -Recurse -Force .nx-libstage -ErrorAction SilentlyContinue
    }
    default { Write-Host "nx-leg: unknown leg '$Leg'"; exit 2 }
}
