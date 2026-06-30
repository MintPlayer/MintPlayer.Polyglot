#!/usr/bin/env pwsh
# Python-backend differential conformance (P9 third-backend bring-up).
#
# The Python backend currently emits only the walking-skeleton subset, so — unlike run-diff.ps1, which runs
# all programs across C#/TS — this runs an ALLOWLIST of skeleton programs, emits Python (+ C# as the oracle),
# runs both, and asserts identical stdout. The allowlist grows as the Python emitter gains coverage; that it
# is a subset is the point (and is reported), not hidden.
#
# Usage:  pwsh tests/conformance/run-python.ps1   (build the solution first; needs `python3` + `dotnet`)

param(
    [string]$Cli = "$PSScriptRoot\..\..\x64\Debug\MintPlayer.Polyglot.Cli.exe"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Cli)) {
    Write-Host "polyglot CLI not found at $Cli — build the solution first (see CLAUDE.md)."
    exit 2
}

# Skeleton-subset programs the Python backend covers today. Grows as emit_python.cpp gains features.
$allowlist = @("arithmetic", "bool_print", "forrange", "casts", "records", "equality", "counter", "closures", "parse", "iterator", "vec2", "enums", "unions", "generic_union", "option", "optional_sugar",
    "float_print", "generics", "int64", "typeargs", "widening", "static_methods", "math", "collections", "empty_list", "extensions", "strings", "exceptions", "inheritance")

$progDir = Join-Path $PSScriptRoot "programs"
$work = Join-Path ([System.IO.Path]::GetTempPath()) "polyglot-conformance-python"

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

$py = (Get-Command python3 -ErrorAction SilentlyContinue) ?? (Get-Command python -ErrorAction SilentlyContinue)
if (-not $py) { Write-Host "python3 not found on PATH."; exit 2 }

$fail = 0
$count = 0
foreach ($name in $allowlist) {
    $count++
    $src = Join-Path $progDir "$name.pg"
    $dir = Join-Path $work $name
    Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $dir | Out-Null

    & $Cli build $src --target python --lib io --out $dir | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] $name (python transpile failed)"; $fail++; continue }
    & $Cli build $src --target csharp --lib io --out $dir | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] $name (csharp transpile failed)"; $fail++; continue }

    $csproj | Set-Content (Join-Path $dir "$name.csproj")
    & dotnet build (Join-Path $dir "$name.csproj") -c Release -v quiet --nologo *> $null
    $dll = Join-Path $dir "bin\Release\net10.0\$name.dll"
    $cs = (& dotnet $dll 2>$null | Out-String).TrimEnd("`r", "`n")
    $pyOut = (& $py.Source (Join-Path $dir "$name.py") 2>$null | Out-String).TrimEnd("`r", "`n")

    if ($cs -eq $pyOut) {
        Write-Host "[PASS] $name  ->  $($pyOut -replace "`r?`n", ' | ')"
    } else {
        Write-Host "[FAIL] $name : C# and Python diverged"
        Write-Host "        C#: $($cs -replace "`r?`n", ' | ')"
        Write-Host "        Py: $($pyOut -replace "`r?`n", ' | ')"
        $fail++
    }
}

Write-Host ""
$total = (Get-ChildItem $progDir -Filter *.pg).Count
if ($fail -eq 0) {
    Write-Host "All $count Python skeleton program(s) agree with C# ($count of $total conformance programs; the Python backend is a growing subset)."
    exit 0
}
Write-Host "$fail of $count Python program(s) diverged."
exit 1
