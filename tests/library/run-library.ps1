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

#
# P40 B1 (issue #73): a multi-library case — two PHP libraries defining the SAME function names, each in its
# own namespace (set per pgconfig `include` rule via `targetOptions.php.namespace`), loaded by one consumer.
# Without namespaces PHP fails with "Cannot redeclare". -MultiLibraryOnly runs just that case. (The C#
# two-assembly case lives in tests/msbuild/run-nuget.ps1, fixture 9, because it needs the MSBuild package.)

param(
    [string]$Cli = "$PSScriptRoot\..\..\x64\Debug\MintPlayer.Polyglot.Cli.exe",
    [string]$Staged = "",
    [switch]$MultiLibraryOnly
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
if (-not $MultiLibraryOnly -and -not (Get-Command tsc -ErrorAction SilentlyContinue)) {
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

    # P40 A2 grep gate: the lowering poison must never reach emitted code.
    $poison = @(Get-ChildItem $dir -Filter *.ts | Select-String -SimpleMatch '__polyglot_unlowered_expr__' -List)
    if ($poison.Count -gt 0) {
        Fail "$name : emitted output contains __polyglot_unlowered_expr__ ($(($poison | ForEach-Object { Split-Path $_.Path -Leaf }) -join ', '))"
        return
    }

    # Regression guard: a USER top-level declaration (column 0) that is NOT `export`ed is a dropped export.
    # P40 §0/D10: prelude declarations (Option, print, the `__pgNullAssert` helper, …) are deliberately
    # module-local, so only names the program's own .pg sources declare are held to the rule.
    $pgSources = @()
    $single = Join-Path $progDir "$name.pg"
    if (Test-Path $single) { $pgSources = @($single) }
    elseif (Test-Path (Join-Path $progDir $name)) { $pgSources = @(Get-ChildItem (Join-Path $progDir $name) -Filter *.pg -Recurse | ForEach-Object FullName) }
    $userNames = @{}
    foreach ($src in $pgSources) {
        foreach ($m in Select-String -Path $src -Pattern '^(?:export\s+)?(?:(?:open|abstract|sealed|async|extern)\s+)*(?:fn|class|record|enum|union|interface|let|const|var)\s+([A-Za-z_]\w*)') {
            $userNames[$m.Matches[0].Groups[1].Value] = $true
        }
    }
    $unexported = @(Select-String -Path $ts -Pattern '^(?:abstract class|class|function\*?|async function|interface|type|const|let|enum|namespace)\s+([A-Za-z_$][\w$]*)' |
        Where-Object { $userNames.ContainsKey($_.Matches[0].Groups[1].Value) })
    if ($unexported.Count -gt 0) {
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

# P40 B1: two PHP libraries with the same function names, namespaced per include rule, in one process.
function Test-PhpMultiLibrary {
    if (-not (Get-Command php -ErrorAction SilentlyContinue)) {
        Write-Host "[SKIP] PHP multi-library case: php not on PATH"
        return
    }
    $proj = Join-Path $work "_php_multilib"
    New-Item -ItemType Directory -Force $proj | Out-Null
    # Same names in both: a function, a record, and (through `print`) the guarded io prelude.
    foreach ($lib in "alpha", "beta") {
        @"
record Tag(name: string)
fn greet(): string => "$lib"
fn tag(): Tag => Tag("$lib")
fn shout() { print(greet()) }
"@ | Set-Content (Join-Path $proj "$lib.pg")
    }
    @'
{
  "targets": ["php"],
  "lib": ["io"],
  "include": [
    { "pattern": "alpha.pg", "output": "out/%(Filename)", "targetOptions": { "php": { "namespace": "Libs\\Alpha" } } },
    { "pattern": "beta.pg",  "output": "out/%(Filename)", "targetOptions": { "php": { "namespace": "Libs\\Beta" } } }
  ]
}
'@ | Set-Content (Join-Path $proj "pgconfig.json")
    Push-Location $proj
    try { & $Cli build *> (Join-Path $proj "build.log") } finally { Pop-Location }
    if ($LASTEXITCODE -ne 0) { Fail "PHP multi-library: polyglot build failed (see $proj\build.log)"; return }
    @'
<?php
require __DIR__ . '/out/alpha.php';
require __DIR__ . '/out/beta.php';
echo \Libs\Alpha\greet() . "+" . \Libs\Beta\greet() . "+" . \Libs\Alpha\tag()->name . "+" . \Libs\Beta\tag()->name . "\n";
\Libs\Alpha\shout();
\Libs\Beta\shout();
'@ | Set-Content (Join-Path $proj "consumer.php")
    $out = (& php (Join-Path $proj "consumer.php") 2>&1 | Out-String).Trim() -replace "`r", ""
    if ($out -eq "alpha+beta+alpha+beta`nalpha`nbeta") {
        Write-Host "[PASS] two PHP libraries with the same names load side by side in distinct namespaces"
    } else {
        Fail "PHP multi-library: unexpected output:`n$out"
    }
}

if ($MultiLibraryOnly) {
    Test-PhpMultiLibrary
    if ($fail -eq 0) { Write-Host "Multi-library case green."; exit 0 }
    Write-Host "$fail multi-library check(s) failed."
    exit 1
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

# P40 A4: the `!` case — null_assert.pg emits the checked `x ?? __pgNullAssert("m.pg:L:C")` form plus a
# module-local (non-exported) `never` helper; the strict tsc pass below must accept both.
$naTs = Join-Path (Join-Path $work "null_assert") "null_assert.ts"
if (Test-Path $naTs) {
    $naText = Get-Content $naTs -Raw
    if (($naText -match '\?\? __pgNullAssert\("null_assert\.pg:\d+:\d+"\)') -and
        ($naText -match '(?m)^function __pgNullAssert\(at: string\): never')) {
        Write-Host "[PASS] null_assert : '!' emits the checked form with a module-local never-helper"
    } else {
        Fail "null_assert : '!' did not emit '?? __pgNullAssert(`"m.pg:L:C`")' with a non-exported helper"
    }
} else {
    Fail "null_assert : not staged (expected $naTs)"
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
Write-Host "==> Multi-library consumption (P40 B1)"
Test-PhpMultiLibrary

Write-Host ""
if ($fail -eq 0) {
    Write-Host "Library-consumption gate green: every emitted TS module is an importable, strict-clean ES module."
    exit 0
}
Write-Host "$fail library-consumption check(s) failed."
exit 1
