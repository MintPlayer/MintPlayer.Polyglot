#!/usr/bin/env pwsh
# P3 parser-fidelity gate: every docs/lang/samples/*.pg must round-trip
# source -> AST -> source idempotently through `polyglot fmt`.
#
# Usage:  pwsh tests/fidelity/run-roundtrip.ps1   (build the solution first)

param(
    [string]$Cli = "$PSScriptRoot\..\..\x64\Debug\MintPlayer.Polyglot.Cli.exe"
)

$ErrorActionPreference = "Stop"
# The CLI writes UTF-8; decode its stdout as UTF-8 regardless of the console's OEM codepage, or non-ASCII
# samples (curly quotes, emoji) come back mojibake and the round-trip compare spuriously fails.
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
if (-not (Test-Path $Cli)) {
    Write-Host "polyglot CLI not found at $Cli — build the solution first (see CLAUDE.md)."
    exit 2
}

$samples = Join-Path $PSScriptRoot "..\..\docs\lang\samples"
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) "polyglot-roundtrip"
New-Item -ItemType Directory -Force $tmp | Out-Null

$fail = 0
$count = 0
foreach ($pg in Get-ChildItem $samples -Filter *.pg | Sort-Object Name) {
    $count++
    $a = & $Cli fmt $pg.FullName 2>$null
    if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] $($pg.Name) (parse error)"; $fail++; continue }
    $aPath = Join-Path $tmp "$($pg.BaseName).pg"
    ($a -join "`n") | Set-Content $aPath
    $b = & $Cli fmt $aPath 2>$null
    if ((($a -join "`n") -eq ($b -join "`n")) -and $a.Count -gt 0) {
        Write-Host "[PASS] $($pg.Name)"
    } else {
        Write-Host "[FAIL] $($pg.Name) (not idempotent)"
        $fail++
    }
}

Write-Host ""
if ($fail -eq 0) {
    Write-Host "All $count sample(s) round-trip (source -> AST -> source idempotent)."
    exit 0
}
Write-Host "$fail of $count sample(s) failed the round-trip."
exit 1
