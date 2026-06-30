#!/usr/bin/env pwsh
# Sample EMIT-correctness gate (P14a): every docs/lang/samples/*.pg must not just transpile but produce C#
# that COMPILES (dotnet) and TS that RUNS (node) without error. The transpile-only gate (run-compile.ps1)
# was green over a cluster of output-only miscompiles — this gate is the regression net for §3 "never
# miscompile" at the emitted-code level.
#
# It does NOT compare stdout across targets: most samples emit floats, which §3.D says are not bit-exact
# cross-target. (The differential gate, run-diff.ps1, owns stdout-equality for the integer/string programs.)
# Here the assertion is simply: each target's emitted program builds/runs with exit 0.
#
# Samples blocked on a documented missing capability are listed as expected-failures (xfail) with the gap.
# Green only when every non-xfail sample compiles+runs AND every xfail sample fails — so an "unexpected pass"
# flags a retired gap and an "unexpected fail" flags a regression. As P14b bugs are fixed, entries leave xfail.
#
# Usage:  pwsh tests/samples/run-emit.ps1   (build the solution first; see CLAUDE.md). Needs dotnet + node.

param(
    [string]$Cli = "$PSScriptRoot\..\..\x64\Debug\MintPlayer.Polyglot.Cli.exe"
)

$ErrorActionPreference = "Stop"
if (-not (Test-Path $Cli)) { Write-Host "polyglot CLI not found at $Cli — build the solution first."; exit 2 }

$sampleDir = Join-Path $PSScriptRoot "..\..\docs\lang\samples"
$work = Join-Path ([System.IO.Path]::GetTempPath()) "polyglot-emit"
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $work | Out-Null

$csproj = @'
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net10.0</TargetFramework>
    <Nullable>disable</Nullable>
    <ImplicitUsings>disable</ImplicitUsings>
  </PropertyGroup>
</Project>
'@

# Sample -> the P14b gap that keeps its emitted code from compiling/running. Remove an entry when fixed.
$xfail = @{
    "06_exceptions.pg"         = "aspirational string std methods (isEmpty / toI32)"
    "08_extensions.pg"         = "aspirational string std methods (toUpper / clamp)"
    "09_strings.pg"            = "aspirational string std methods (charAt / codePoints)"
}

$bad = 0
$count = 0
foreach ($f in Get-ChildItem $sampleDir -Filter *.pg | Sort-Object Name) {
    $count++
    $stem = $f.BaseName
    $dir = Join-Path $work $stem
    New-Item -ItemType Directory -Force $dir | Out-Null

    $ok = $true
    & $Cli build $f.FullName --out $dir *> $null
    if ($LASTEXITCODE -ne 0) {
        $ok = $false
    } else {
        $csproj | Set-Content (Join-Path $dir "$stem.csproj")
        & dotnet build (Join-Path $dir "$stem.csproj") -c Release -v quiet --nologo *> $null
        if ($LASTEXITCODE -ne 0) { $ok = $false }
        & node (Join-Path $dir "$stem.ts") *> $null
        if ($LASTEXITCODE -ne 0) { $ok = $false }
    }

    $expectFail = $xfail.ContainsKey($f.Name)
    if ($expectFail -and -not $ok) {
        Write-Host "[XFAIL] $($f.Name)  ($($xfail[$f.Name]))"
    } elseif ($expectFail -and $ok) {
        Write-Host "[UNEXPECTED PASS] $($f.Name) — the gap is fixed; remove it from the xfail list."
        $bad++
    } elseif (-not $expectFail -and $ok) {
        Write-Host "[PASS] $($f.Name)  (C# compiles, TS runs)"
    } else {
        Write-Host "[FAIL] $($f.Name) — emitted C# didn't compile or TS didn't run (regression)."
        $bad++
    }
}

Write-Host ""
if ($bad -eq 0) {
    Write-Host "All $count sample(s) accounted for ($($xfail.Count) expected-fail, $($count - $xfail.Count) compile+run)."
    exit 0
}
Write-Host "$bad sample(s) in an unexpected state."
exit 1
