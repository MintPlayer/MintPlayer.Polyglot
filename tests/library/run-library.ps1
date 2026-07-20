#!/usr/bin/env pwsh
# Library-consumption gate — proves the emitted TypeScript is an importable, strict-clean ES module.
#
# The differential gate (run-diff.ps1) only ever *executes* a program (node runs the file, main() prints,
# stdout is compared). It cannot catch a module that runs fine as a script but can't be *consumed* as a
# library: if the emitter drops `export` off its top-level declarations, `node prog.ts` still prints the
# right thing, so run-diff stays green while the output is unusable from any real consumer (Angular, a
# sibling .ts, …) and trips `isolatedModules`. That exact bug shipped once. This gate closes the hole.
#
# For every conformance program it:
#   1. emits TS (`--lib io --target typescript`),
#   2. asserts every top-level declaration is `export`ed (regression guard for the dropped-export bug),
#   3. writes a sibling consumer that imports the emitted module, and
#   4. type-checks the whole set in ONE tsc pass under strict / isolatedModules (+ the flags a modern
#      bundler applies), so a type error in any emitted module — not just a missing export — fails the gate.
#
# The C# side of "can this be consumed?" is covered by run-nullable.ps1 (emitted C# compiles clean under
# <Nullable>enable</> + <TreatWarningsAsErrors>). Together they gate both targets as libraries, not scripts.
#
# Type-checking is hermetic (no @types/node / network): a tiny env.d.ts declares exactly the Node runtime
# surface the emitted std bindings touch (console, process). Needs `tsc` on PATH (TypeScript >= 5).
#
# -Staged <dir> (P35 slice 3): consume the PRISTINE .ts already emitted by run-conformance.ps1 (one subdir
# per program) instead of re-transpiling every program here — the conformance runner and this gate then share
# a single transpile pass. Standalone (no -Staged) behavior is unchanged: it self-emits each program's TS.
#
# Usage:  pwsh tests/library/run-library.ps1   (build the solution first; see CLAUDE.md)
#         pwsh tests/library/run-library.ps1 -Staged <library-staging-dir>

param(
    [string]$Cli = "$PSScriptRoot\..\..\x64\Debug\MintPlayer.Polyglot.Cli.exe",
    [string]$Staged = ""
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

if (-not $Staged -and -not (Test-Path $Cli)) {
    Write-Host "polyglot CLI not found at $Cli — build the solution first (see CLAUDE.md)."
    exit 2
}
if ($Staged -and -not (Test-Path $Staged)) {
    Write-Host "staging dir not found at $Staged — run run-conformance.ps1 -StagingOut <dir> first."
    exit 2
}
if (-not (Get-Command tsc -ErrorAction SilentlyContinue)) {
    Write-Host "tsc (TypeScript compiler) not found on PATH — install with 'npm i -g typescript'."
    exit 2
}

$progDir = Join-Path $PSScriptRoot "..\conformance\programs"
$work = Join-Path ([System.IO.Path]::GetTempPath()) "polyglot-library"
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $work | Out-Null

# Hermetic ambient env: exactly what the emitted std runtime surface references (see std overlays in the
# typescript plugin). Keeps the type-check offline and independent of an installed @types/node version.
@'
declare const console: { log(...args: any[]): void };
declare const process: { getBuiltinModule(id: string): any };
'@ | Set-Content (Join-Path $work "env.d.ts")

# The flags a strict, modern (bundler-resolved, ESM) consumer applies. `allowImportingTsExtensions` lets a
# consumer import "./prog.ts" by its real name without an emit step.
@'
{
  "compilerOptions": {
    "strict": true,
    "isolatedModules": true,
    "noImplicitReturns": true,
    "noImplicitOverride": true,
    "target": "ES2022",
    "module": "esnext",
    "moduleResolution": "bundler",
    "allowImportingTsExtensions": true,
    "noEmit": true,
    "skipLibCheck": true,
    "types": []
  },
  "include": ["**/*.ts"]
}
'@ | Set-Content (Join-Path $work "tsconfig.json")

$fail = 0
function Fail($msg) { Write-Host "[FAIL] $msg"; $script:fail++ }

# Assert the emitted entry .ts in $dir is a consumable, all-exported module, and drop a consumer next to it.
# Shared by both the self-emitting and the -Staged paths (the assertion is on the .ts, not on how it got there).
function Assert-Consumable($name, $stem, $dir) {
    $ts = Join-Path $dir "$stem.ts"
    if (-not (Test-Path $ts)) { Fail "$name (no $stem.ts emitted)"; return }

    # Regression guard: any top-level declaration (column 0) that is NOT `export`ed is a dropped export.
    $unexported = Select-String -Path $ts -Pattern '^(abstract class|class|function|async function|function\*|interface|type|const|let|enum|namespace) '
    if ($unexported) {
        Fail "$name : top-level declaration(s) not exported: $($unexported[0].Line.Trim())"
        return
    }
    if (-not (Select-String -Path $ts -Pattern '^export ' -Quiet)) {
        Fail "$name : emitted module has no exports (not importable)"
        return
    }

    # Consume it: a sibling module that imports the emitted one. If the module weren't importable this
    # would not type-check. `void m` references the namespace so it isn't an unused import.
    @"
import * as m from "./$stem.ts";
void m;
"@ | Set-Content (Join-Path $dir "_consumer.ts")

    Write-Host "[stage] $name"
}

# Self-emit one program's TS into $work, then assert it. $stem is the emitted entry basename.
function Stage-Program($name, $stem, $cliArgs) {
    $dir = Join-Path $work $name
    New-Item -ItemType Directory -Force $dir | Out-Null

    & $Cli @cliArgs --lib io --target typescript --out $dir *> $null
    if ($LASTEXITCODE -ne 0) { Fail "$name (transpile failed)"; return }

    Assert-Consumable $name $stem $dir
}

if ($Staged) {
    # -Staged: consume the pristine .ts run-conformance already emitted (one subdir per program). The entry
    # stem is the program name for a single-file program, "entry" for a multi-file one.
    Write-Host "==> Consuming staged TS from $Staged (no re-transpile)"
    foreach ($sub in Get-ChildItem $Staged -Directory | Sort-Object Name) {
        $name = $sub.Name
        $dir = Join-Path $work $name
        New-Item -ItemType Directory -Force $dir | Out-Null
        foreach ($f in Get-ChildItem $sub.FullName -Filter *.ts) { Copy-Item $f.FullName $dir }
        $stem = if (Test-Path (Join-Path $dir "$name.ts")) { $name } elseif (Test-Path (Join-Path $dir "entry.ts")) { "entry" } else { $null }
        if (-not $stem) { Fail "$name (no entry .ts in staging)"; continue }
        Assert-Consumable $name $stem $dir
    }
} else {
    foreach ($pg in Get-ChildItem $progDir -Filter *.pg | Sort-Object Name) {
        Stage-Program $pg.BaseName $pg.BaseName @("build", $pg.FullName)
    }
    foreach ($d in Get-ChildItem $progDir -Directory | Sort-Object Name) {
        $entry = Join-Path $d.FullName "entry.pg"
        if (-not (Test-Path $entry)) { continue }
        Stage-Program $d.Name "entry" @("build", $entry, "--root", $d.FullName)
    }
}

Write-Host ""
Write-Host "==> Type-checking every emitted module + consumer under strict / isolatedModules"
$tscOut = & tsc -p (Join-Path $work "tsconfig.json") 2>&1 | Out-String
if ($LASTEXITCODE -ne 0) {
    Write-Host $tscOut.Trim()
    Fail "tsc reported errors — an emitted module is not a strict-clean, consumable library"
} else {
    Write-Host "[PASS] all emitted modules type-check clean as importable ES modules"
}

Write-Host ""
if ($fail -eq 0) {
    Write-Host "Library-consumption gate green: every emitted TS module is an importable, strict-clean ES module."
    exit 0
}
Write-Host "$fail library-consumption check(s) failed."
exit 1
