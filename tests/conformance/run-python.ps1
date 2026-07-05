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
foreach ($name in $programs) {
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
    # Assert the generated C# compiled and both runtimes exited cleanly — a symmetric double-failure
    # (empty == empty) must not false-pass (issue #9 lesson; see run-diff.ps1).
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $dll)) { Write-Host "[FAIL] $name : generated C# did not compile"; $fail++; continue }
    $cs = (& dotnet $dll 2>$null | Out-String).TrimEnd("`r", "`n")
    if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] $name : generated C# crashed at runtime (exit $LASTEXITCODE)"; $fail++; continue }
    $pyOut = (& $py.Source (Join-Path $dir "$name.py") 2>$null | Out-String).TrimEnd("`r", "`n")
    if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] $name : generated Python crashed at runtime (exit $LASTEXITCODE)"; $fail++; continue }

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
if ($fail -eq 0) {
    Write-Host "All $count conformance program(s) agree across Python and C#."
    exit 0
}
Write-Host "$fail of $count Python program(s) diverged."
exit 1
