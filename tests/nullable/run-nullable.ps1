#!/usr/bin/env pwsh
# Nullable-annotation / NRT-cleanliness gate.
#
# The differential gate (run-diff.ps1) can't catch a dropped nullable annotation: `Box b` and `Box? b`
# run byte-identically, so stdout-equality stays green while the emitted C# rots. This gate asserts the
# emitted *text* keeps the `?` on nullable REFERENCE types (regression: the C# `type` rule dropped it,
# emitting `Box b`, which cascades CS8600/CS8602/CS8618/CS8625 under <Nullable>enable/>), and that a
# generated file compiles CLEAN under <Nullable>enable</Nullable> + <TreatWarningsAsErrors>true</> —
# i.e. every generated C# file now carries `#nullable enable` and needs 0 warnings to satisfy it.
#
# Usage:  pwsh tests/nullable/run-nullable.ps1   (build the solution first; see CLAUDE.md). Needs dotnet.

param(
    [string]$Cli = "$PSScriptRoot\..\..\x64\Debug\MintPlayer.Polyglot.Cli.exe"
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
if (-not (Test-Path $Cli)) { Write-Host "polyglot CLI not found at $Cli — build the solution first."; exit 2 }

$fixture = Join-Path $PSScriptRoot "..\conformance\programs\nullable_positions.pg"
$work = Join-Path ([System.IO.Path]::GetTempPath()) "polyglot-nullable"
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $work | Out-Null

$fail = 0
function Check($ok, $msg) {
    if ($ok) { Write-Host "[PASS] $msg" } else { Write-Host "[FAIL] $msg"; $script:fail++ }
}

& $Cli build $fixture --target csharp --out $work --lib io *> $null
if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] transpile to C# failed"; exit 1 }
$cs = Get-Content (Join-Path $work "nullable_positions.cs") -Raw

# 1. Every generated C# file self-declares its nullable context so the annotations are always valid.
Check ($cs.TrimStart() -match '^#nullable enable') "generated C# begins with '#nullable enable'"

# 2. The `?` survives on nullable REFERENCE types in every declaration position.
Check ($cs -match 'record Pair\(Box a, Box\? b\)') "record param keeps '?' (Box? b)"
Check ($cs -match 'Box\? maybe')                    "field keeps '?' (Box? maybe)"
Check ($cs -match 'Box\? get\(\)')                  "method return keeps '?' (Box? get())"
Check ($cs -match 'Box\? x')                        "method param keeps '?' (Box? x)"

# 3. A generated file compiles CLEAN under strict NRT (0 warnings) — proves annotations are self-consistent
#    and the always-shipped std helpers (print<T>, …) are null-clean too. P35 slice 2: through the shared
#    csc oracle (`/nullable:enable /warnaserror` — verified exit-0-equivalent to the csproj form).
. (Join-Path $PSScriptRoot "..\..\scripts\lib\OracleCompile.ps1")
$r = Invoke-OracleCompile -Sources (Join-Path $work "nullable_positions.cs") `
        -OutDll (Join-Path $work "nullable_positions.dll") -NullableEnable -WarnAsError
Check $r.Ok "emitted C# compiles clean under /nullable:enable /warnaserror (0 warnings)"
if (-not $r.Ok) { Write-Host ($r.Output.Trim()) }
Stop-OracleBuildServer

Write-Host ""
if ($fail -eq 0) { Write-Host "Nullable/NRT gate green."; exit 0 }
Write-Host "$fail nullable check(s) failed."; exit 1
