#!/usr/bin/env pwsh
# Kotlin-backend differential conformance (P26 slice 2). Emits every program to Kotlin (+ C# as the oracle),
# compiles the Kotlin with kotlinc, runs both, and asserts identical stdout. Kotlin covers the full §3.A
# surface natively (real overloading, exceptions, ADTs) with disposal/async emulated; a program using a
# still-gated capability (a portable fn with no kotlin `actual`) refuses cleanly and counts as an expected
# PASS. kotlinc is slow (~30-60s/compile), so a full run takes a while.
#
# Usage:  pwsh tests/conformance/run-kotlin.ps1   (build the solution first; needs `kotlinc` + `java` + `dotnet`)

param(
    [string]$Cli = "$PSScriptRoot\..\..\x64\Debug\MintPlayer.Polyglot.Cli.exe",
    [string]$Kotlinc = "$env:LOCALAPPDATA\polyglot-toolchains\kotlin\kotlinc\bin\kotlinc.bat"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Cli)) { Write-Host "polyglot CLI not found at $Cli — build the solution first."; exit 2 }
if (-not (Test-Path $Kotlinc)) {
    $onPath = Get-Command kotlinc -ErrorAction SilentlyContinue
    if ($onPath) { $Kotlinc = $onPath.Source } else { Write-Host "kotlinc not found at $Kotlinc nor on PATH."; exit 2 }
}

$progDir = Join-Path $PSScriptRoot "programs"
$work = Join-Path ([System.IO.Path]::GetTempPath()) "polyglot-conformance-kotlin"
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

$fail = 0; $count = 0; $refused = 0; $skipped = 0
# Programs still awaiting a Kotlin rule family (incremental slice — see PLAN §P26 slice 2). Each is honestly
# skipped, not faked; the list shrinks to empty as the families land.
$skip = @()

function Test-Program($name, $stem, $cliArgs) {
    $dir = Join-Path $work $name
    Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $dir | Out-Null

    $ktErr = (& $Cli @cliArgs --target kotlin --lib io --out $dir 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) {
        if ($ktErr -match "does not support|has no 'actual'") {
            Write-Host "[PASS] $name  ->  (refused by design: capability/portability gate)"; $script:refused++; return $true
        }
        Write-Host "[FAIL] $name (kotlin transpile failed): $($ktErr.Trim())"; return $false
    }
    & $Cli @cliArgs --target csharp --lib io --out $dir | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] $name (csharp transpile failed)"; return $false }

    $csproj | Set-Content (Join-Path $dir "$stem.csproj")
    & dotnet build (Join-Path $dir "$stem.csproj") -c Release -v quiet --nologo *> $null
    $dll = Join-Path $dir "bin\Release\net10.0\$stem.dll"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $dll)) { Write-Host "[FAIL] $name : generated C# did not compile"; return $false }
    $cs = (& dotnet $dll 2>$null | Out-String).TrimEnd("`r", "`n")
    if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] $name : generated C# crashed at runtime (exit $LASTEXITCODE)"; return $false }

    # Compile every emitted .kt in the dir together (multi-module programs emit siblings), then run the jar.
    $kts = @(Get-ChildItem $dir -Filter *.kt | ForEach-Object { $_.FullName })
    $jar = Join-Path $dir "$stem.jar"
    $kErr = (& $Kotlinc @kts -include-runtime -d $jar 2>&1 | Where-Object { $_ -notmatch '^warning:' } | Out-String)
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $jar)) { Write-Host "[FAIL] $name : generated Kotlin did not compile: $($kErr.Trim())"; return $false }
    $ktOut = (& java -jar $jar 2>&1 | Out-String).TrimEnd("`r", "`n")
    if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] $name : generated Kotlin crashed at runtime (exit $LASTEXITCODE): $ktOut"; return $false }

    if ($cs -eq $ktOut) { Write-Host "[PASS] $name  ->  $($ktOut -replace "`r?`n", ' | ')"; return $true }
    Write-Host "[FAIL] $name : C# and Kotlin diverged"
    Write-Host "        C#:  $($cs -replace "`r?`n", ' | ')"
    Write-Host "        Kt:  $($ktOut -replace "`r?`n", ' | ')"
    return $false
}

foreach ($name in $programs) {
    if ($skip -contains $name) { Write-Host "[SKIP] $name (Kotlin rule-family follow-up)"; $skipped++; continue }
    $count++
    if (-not (Test-Program $name $name @("build", (Join-Path $progDir "$name.pg")))) { $fail++ }
}
foreach ($d in Get-ChildItem $progDir -Directory | Sort-Object Name) {
    $entry = Join-Path $d.FullName "entry.pg"
    if (-not (Test-Path $entry)) { continue }
    if ($skip -contains $d.Name) { Write-Host "[SKIP] $($d.Name) (Kotlin rule-family follow-up)"; $skipped++; continue }
    $count++
    if (-not (Test-Program $d.Name "entry" @("build", $entry, "--root", $d.FullName))) { $fail++ }
}

Write-Host ""
if ($fail -eq 0) { Write-Host "All $count conformance program(s) agree across Kotlin and C# ($refused refused by design, $skipped skipped)."; exit 0 }
Write-Host "$fail of $count Kotlin program(s) diverged."
exit 1
