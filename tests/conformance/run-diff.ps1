#!/usr/bin/env pwsh
# P2 differential conformance runner (PLAN P2 gate).
#
# For every programs/*.pg: emit C# + TS with the polyglot CLI, compile & run the C# (dotnet) and run the
# TS (node's type-stripping), and assert the two stdouts are identical. This is the crown-jewel test in
# miniature — it grows to cover the full surface in P5 and the FruitCake physics in P8.
#
# Usage:  pwsh tests/conformance/run-diff.ps1   (build the solution first; see CLAUDE.md)

param(
    [string]$Cli = "$PSScriptRoot\..\..\x64\Debug\MintPlayer.Polyglot.Cli.exe"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Cli)) {
    Write-Host "polyglot CLI not found at $Cli — build the solution first (see CLAUDE.md)."
    exit 2
}

$progDir = Join-Path $PSScriptRoot "programs"
$work = Join-Path ([System.IO.Path]::GetTempPath()) "polyglot-conformance"

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

$fail = 0
$count = 0

foreach ($pg in Get-ChildItem $progDir -Filter *.pg | Sort-Object Name) {
    $count++
    $name = $pg.BaseName
    $dir = Join-Path $work $name
    Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $dir | Out-Null

    & $Cli build $pg.FullName --out $dir | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] $name (transpile failed)"; $fail++; continue }

    $csproj | Set-Content (Join-Path $dir "$name.csproj")
    # Build quietly and run the assembly directly, so build diagnostics never mix into program stdout.
    & dotnet build (Join-Path $dir "$name.csproj") -c Release -v quiet --nologo *> $null
    $dll = Join-Path $dir "bin\Release\net10.0\$name.dll"
    $cs = (& dotnet $dll 2>$null | Out-String).TrimEnd("`r","`n")
    $ts = (& node (Join-Path $dir "$name.ts") 2>$null | Out-String).TrimEnd("`r","`n")

    if ($cs -eq $ts) {
        $shown = ($cs -replace "`r?`n", " | ")
        Write-Host "[PASS] $name  ->  $shown"
    } else {
        Write-Host "[FAIL] $name : C# and TS diverged"
        Write-Host "        C#: $($cs -replace "`r?`n", ' | ')"
        Write-Host "        TS: $($ts -replace "`r?`n", ' | ')"
        $fail++
    }
}

Write-Host ""
if ($fail -eq 0) {
    Write-Host "All $count conformance program(s) agree across C# and TS."
    exit 0
}
Write-Host "$fail of $count conformance program(s) diverged."
exit 1
