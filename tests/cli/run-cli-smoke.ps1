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

    # 6. G39: a single invocation driven by pgconfig `targets` emits byte-identical output to four separate
    # per-target invocations. Guards against per-target codegen drifting based on which sibling targets are
    # co-emitted in the same run.
    $g39 = Join-Path $work "g39"
    New-Item -ItemType Directory -Force $g39 | Out-Null
    $src = Join-Path $g39 "counter.pg"
    Copy-Item (Join-Path $repo "tests/conformance/programs/counter.pg") $src
    '{ "targets": ["csharp", "typescript", "python", "php"] }' | Set-Content (Join-Path $g39 "pgconfig.json")

    $single = Join-Path $g39 "single"
    & $Cli build $src --lib io --out $single *> $null
    $singleOk = ($LASTEXITCODE -eq 0)

    $targets = @{ csharp = "cs"; typescript = "ts"; python = "py"; php = "php" }
    $allMatch = $singleOk
    foreach ($t in $targets.Keys) {
        $ext = $targets[$t]
        $sepDir = Join-Path $g39 "sep-$t"
        & $Cli build $src --target $t --lib io --out $sepDir *> $null
        if ($LASTEXITCODE -ne 0) { $allMatch = $false; continue }
        $a = Join-Path $single "counter.$ext"
        $b = Join-Path $sepDir "counter.$ext"
        if (-not (Test-Path $a) -or -not (Test-Path $b)) { $allMatch = $false; continue }
        $ha = (Get-FileHash $a -Algorithm SHA256).Hash
        $hb = (Get-FileHash $b -Algorithm SHA256).Hash
        if ($ha -ne $hb) { Write-Host "        ${t}: single-invocation output differs from per-target run"; $allMatch = $false }
    }
    Check $allMatch "four-target single invocation is byte-identical to four per-target runs"
}
finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
    if (Test-Path $work) { Write-Host "WARNING: could not remove CLI-smoke work dir '$work' — clean it up manually." }
}

Write-Host ""
if ($failures -eq 0) { Write-Host "CLI smoke: all checks passed."; exit 0 }
Write-Host "CLI smoke: $failures check(s) FAILED."
exit 1
