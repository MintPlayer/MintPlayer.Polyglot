#!/usr/bin/env pwsh
# Python-backend differential conformance (P9 third-backend validation).
#
# The Python backend now covers the FULL conformance suite (the P9-V spike grew it feature-by-feature from
# the walking skeleton). This emits every program to Python (+ C# as the oracle), runs both, and asserts
# identical stdout. C# is the oracle (not TS) because both are checked the same way and C# is the reference
# semantics; the C#/TS pair is covered by run-diff.ps1.
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

$progDir = Join-Path $PSScriptRoot "programs"
$work = Join-Path ([System.IO.Path]::GetTempPath()) "polyglot-conformance-python"

# Every top-level conformance program (the Python backend now covers the full §3.A surface).
$programs = (Get-ChildItem $progDir -Filter *.pg | Sort-Object Name | ForEach-Object { $_.BaseName })

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

# Transpile one program (single .pg or a multi-file entry) to Python + C#, build+run the C# oracle and run
# the Python, compare stdout. $stem is the emitted entry basename; $cliArgs is the `polyglot build …` argv.
# Multi-module (§4.5): the C# csproj globs every generated .cs (one assembly — proves no CS0101); Python
# runs the entry, which imports its siblings from the same directory.
function Test-Program($name, $stem, $cliArgs) {
    $dir = Join-Path $work $name
    Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $dir | Out-Null

    & $Cli @cliArgs --target python --lib io --out $dir | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] $name (python transpile failed)"; return $false }
    & $Cli @cliArgs --target csharp --lib io --out $dir | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] $name (csharp transpile failed)"; return $false }

    $csproj | Set-Content (Join-Path $dir "$stem.csproj")
    & dotnet build (Join-Path $dir "$stem.csproj") -c Release -v quiet --nologo *> $null
    $dll = Join-Path $dir "bin\Release\net10.0\$stem.dll"
    # Assert the generated C# compiled and both runtimes exited cleanly — a symmetric double-failure
    # (empty == empty) must not false-pass (issue #9 lesson; see run-diff.ps1).
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $dll)) { Write-Host "[FAIL] $name : generated C# did not compile"; return $false }
    $cs = (& dotnet $dll 2>$null | Out-String).TrimEnd("`r", "`n")
    if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] $name : generated C# crashed at runtime (exit $LASTEXITCODE)"; return $false }
    $pyOut = (& $py.Source (Join-Path $dir "$stem.py") 2>$null | Out-String).TrimEnd("`r", "`n")
    if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] $name : generated Python crashed at runtime (exit $LASTEXITCODE)"; return $false }

    if ($cs -eq $pyOut) {
        Write-Host "[PASS] $name  ->  $($pyOut -replace "`r?`n", ' | ')"
        return $true
    }
    Write-Host "[FAIL] $name : C# and Python diverged"
    Write-Host "        C#: $($cs -replace "`r?`n", ' | ')"
    Write-Host "        Py: $($pyOut -replace "`r?`n", ' | ')"
    return $false
}

# Single-file programs.
foreach ($name in $programs) {
    $count++
    if (-not (Test-Program $name $name @("build", (Join-Path $progDir "$name.pg")))) { $fail++ }
}
# Multi-file programs (§4.5 module linking): each programs/<dir>/ with an entry.pg, built with --root <dir>.
foreach ($d in Get-ChildItem $progDir -Directory | Sort-Object Name) {
    $entry = Join-Path $d.FullName "entry.pg"
    if (-not (Test-Path $entry)) { continue }
    $count++
    if (-not (Test-Program $d.Name "entry" @("build", $entry, "--root", $d.FullName))) { $fail++ }
}

Write-Host ""
if ($fail -eq 0) {
    Write-Host "All $count conformance program(s) agree across Python and C#."
    exit 0
}
Write-Host "$fail of $count Python program(s) diverged."
exit 1
