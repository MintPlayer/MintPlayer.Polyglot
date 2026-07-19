#!/usr/bin/env pwsh
# G41 CLI-surface smoke + G39 four-target single-invocation check.
#
# Fast, dependency-free assertions on the CLI's front door — the contract editor tooling, CI, and the
# MSBuild/NuGet integration all lean on: version shape, help, exit codes for bad input, and that a single
# multi-target build emits byte-for-byte what four separate per-target builds do.
#
# Multi-target invocation (G39): repeated `--target` flags are LAST-WINS (not additive) and a comma list is
# rejected, so the real single-invocation multi-target path is a pgconfig.json `targets` array with no
# --target override — that is what this exercises.
#
# Usage:  pwsh tests/cli/run-cli-smoke.ps1   (build the solution first; see CLAUDE.md)

param(
    [string]$Cli = "$PSScriptRoot\..\..\x64\Debug\MintPlayer.Polyglot.Cli.exe"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Cli)) {
    Write-Host "polyglot CLI not found at $Cli — build the solution first (see CLAUDE.md)."
    exit 2
}

$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("polyglot-cli-smoke-" + [System.Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null

$failures = 0
function Check([bool]$cond, [string]$name) {
    if ($cond) { Write-Host "[PASS] $name" } else { Write-Host "[FAIL] $name"; $script:failures++ }
}

try {
    # 1. --version: exit 0 and a semver-shaped stdout.
    $verOut = (& $Cli --version 2>&1 | Out-String).Trim()
    Check ($LASTEXITCODE -eq 0 -and $verOut -match '^\d+\.\d+\.\d+') "--version exits 0 and prints a semver ($verOut)"

    # 2. -h: exit 0 and usage text.
    $helpOut = (& $Cli -h 2>&1 | Out-String)
    Check ($LASTEXITCODE -eq 0 -and $helpOut -match 'Usage:') "-h exits 0 and prints usage"

    # 3. Unknown command: exit 64 (EX_USAGE).
    & $Cli frobnicate *> $null
    Check ($LASTEXITCODE -eq 64) "unknown command exits 64"

    # 4. build of a nonexistent input: nonzero exit with a readable error (stdout or stderr).
    $missing = Join-Path $work "does-not-exist.pg"
    $missOut = (& $Cli build $missing --target csharp --out (Join-Path $work "o-missing") 2>&1 | Out-String)
    Check ($LASTEXITCODE -ne 0 -and $missOut -match 'cannot open|no such file|not found') `
        "build of a missing input fails with a readable error"

    # 5. build with no pgconfig.json and no --target: nonzero refusal (the config-sourced target contract).
    $noCfgDir = Join-Path $work "nocfg"
    New-Item -ItemType Directory -Force $noCfgDir | Out-Null
    Copy-Item (Join-Path $repo "tests/conformance/programs/counter.pg") (Join-Path $noCfgDir "counter.pg")
    $noCfgOut = (& $Cli build (Join-Path $noCfgDir "counter.pg") --lib io --out (Join-Path $work "o-nocfg") 2>&1 | Out-String)
    Check ($LASTEXITCODE -ne 0 -and $noCfgOut -match 'no --target|pgconfig') `
        "build with no config and no --target refuses"

    # 6. G39: a single config-sourced invocation emits byte-identical output to separate per-target
    # invocations. Guards against per-target codegen drifting based on which sibling targets are co-emitted in
    # the same run. Extended (P35 slice 3) from one program to three representative shapes, because the merged
    # conformance runner now relies on this single-vs-per-target equivalence for its whole corpus:
    #   - counter.pg       : the simple single-file, all four targets
    #   - modular/         : a multi-file program (co-emitted modules vs per-target runs, --root)
    #   - operators_full.pg: a pinned PHP refuser under its 3-target config (csharp/typescript/python; php
    #                        excluded because it refuses) vs per-target runs of those three
    $extOf = @{ csharp = "cs"; typescript = "ts"; python = "py"; php = "php" }

    # Compare every emitted file of each target's extension between the single (config-sourced) build dir and a
    # per-target build dir. Returns $false on any missing file or byte mismatch (multi-file: all modules).
    function Test-G39($label, $isDir, $srcName, $targetList) {
        $case = Join-Path $work "g39-$label"
        New-Item -ItemType Directory -Force $case | Out-Null
        $srcRoot = Join-Path $repo "tests/conformance/programs"
        if ($isDir) {
            Copy-Item -Recurse (Join-Path $srcRoot $srcName "*") $case
            $entry = Join-Path $case "entry.pg"
        } else {
            Copy-Item (Join-Path $srcRoot "$srcName.pg") $case
            $entry = Join-Path $case "$srcName.pg"
        }
        $json = '{ "targets": [' + (($targetList | ForEach-Object { "`"$_`"" }) -join ", ") + '] }'
        $json | Set-Content (Join-Path $case "pgconfig.json")

        $single = Join-Path $case "single"
        $singleArgs = @("build", $entry, "--lib", "io", "--out", $single)
        if ($isDir) { $singleArgs += @("--root", $case) }
        & $Cli @singleArgs *> $null
        if ($LASTEXITCODE -ne 0) { Write-Host "        ${label}: single config-sourced build failed"; return $false }

        $ok = $true
        foreach ($t in $targetList) {
            $ext = $extOf[$t]
            $sepDir = Join-Path $case "sep-$t"
            $sepArgs = @("build", $entry, "--target", $t, "--lib", "io", "--out", $sepDir)
            if ($isDir) { $sepArgs += @("--root", $case) }
            & $Cli @sepArgs *> $null
            if ($LASTEXITCODE -ne 0) { Write-Host "        ${label}/${t}: per-target build failed"; $ok = $false; continue }
            foreach ($f in Get-ChildItem $single -Filter "*.$ext") {
                $b = Join-Path $sepDir $f.Name
                if (-not (Test-Path $b)) { Write-Host "        ${label}/${t}: $($f.Name) missing from per-target run"; $ok = $false; continue }
                if ((Get-FileHash $f.FullName -Algorithm SHA256).Hash -ne (Get-FileHash $b -Algorithm SHA256).Hash) {
                    Write-Host "        ${label}/${t}: $($f.Name) differs single vs per-target"; $ok = $false
                }
            }
        }
        return $ok
    }

    $g39Ok = (Test-G39 "counter" $false "counter" @("csharp", "typescript", "python", "php"))
    $g39Ok = (Test-G39 "modular" $true "modular" @("csharp", "typescript", "python", "php")) -and $g39Ok
    $g39Ok = (Test-G39 "operators_full" $false "operators_full" @("csharp", "typescript", "python")) -and $g39Ok
    Check $g39Ok "single config-sourced invocation is byte-identical to per-target runs (counter, modular, operators_full)"
}
finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
    if (Test-Path $work) { Write-Host "WARNING: could not remove CLI-smoke work dir '$work' — clean it up manually." }
}

Write-Host ""
if ($failures -eq 0) { Write-Host "CLI smoke: all checks passed."; exit 0 }
Write-Host "CLI smoke: $failures check(s) FAILED."
exit 1
