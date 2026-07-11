#!/usr/bin/env pwsh
# PHP-backend differential conformance (P26 slice 1 — the PHP fidelity uplift).
#
# The PHP backend now covers the full §3.A surface EXCEPT operator overloading and async (declared `false` in
# the plugin — a program using them refuses at transpile with a §3.E diagnostic, which this runner treats as an
# expected PASS, not a failure). This emits every program to PHP (+ C# as the oracle), runs both, and asserts
# identical stdout. C# is the oracle (same reason as run-python.ps1: it's the reference semantics).
#
# Usage:  pwsh tests/conformance/run-php.ps1   (build the solution first; needs `php` + `dotnet`)

param(
    [string]$Cli = "$PSScriptRoot\..\..\x64\Debug\MintPlayer.Polyglot.Cli.exe",
    [string]$Php = "$env:LOCALAPPDATA\polyglot-toolchains\php\php.exe"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Cli)) {
    Write-Host "polyglot CLI not found at $Cli — build the solution first (see CLAUDE.md)."
    exit 2
}
if (-not (Test-Path $Php)) {
    $onPath = Get-Command php -ErrorAction SilentlyContinue
    if ($onPath) { $Php = $onPath.Source } else { Write-Host "php not found at $Php nor on PATH."; exit 2 }
}

$progDir = Join-Path $PSScriptRoot "programs"
$work = Join-Path ([System.IO.Path]::GetTempPath()) "polyglot-conformance-php"

# Every top-level conformance program. The PHP backend covers the §3.A surface minus operatorOverloading/async;
# a program using a gated feature refuses cleanly (a §3.E diagnostic) and is counted as an expected PASS.
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

$fail = 0
$count = 0
$refused = 0
$skipped = 0

# Nothing is skipped: PHP covers the full conformance surface. Programs using a still-gated capability
# (operatorOverloading → vec2; async → async_await; a portable fn with no php `actual` → expect_actual/
# extern_ffi) refuse cleanly at transpile and are counted as expected passes (see the refusal branch below).
$skip = @()

# Transpile one program to PHP + C#, build+run the C# oracle and run the PHP, compare stdout. A clean PHP
# refusal (nonzero exit with a §3.E "does not support" diagnostic, no output file) is an expected PASS.
function Test-Program($name, $stem, $cliArgs) {
    $dir = Join-Path $work $name
    Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $dir | Out-Null

    $phpErr = (& $Cli @cliArgs --target php --lib io --out $dir 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) {
        # A capability refusal (§3.B/§3.E) is by design — the PHP surface deliberately gates operator
        # overloading and async. Distinguish it from a real transpile crash.
        if ($phpErr -match "does not support|has no 'actual'") {
            Write-Host "[PASS] $name  ->  (refused by design: capability/portability gate)"
            $script:refused++
            return $true
        }
        Write-Host "[FAIL] $name (php transpile failed): $($phpErr.Trim())"
        return $false
    }
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
    $phpOut = (& $Php (Join-Path $dir "$stem.php") 2>&1 | Out-String).TrimEnd("`r", "`n")
    if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] $name : generated PHP crashed at runtime (exit $LASTEXITCODE): $phpOut"; return $false }

    if ($cs -eq $phpOut) {
        Write-Host "[PASS] $name  ->  $($phpOut -replace "`r?`n", ' | ')"
        return $true
    }
    Write-Host "[FAIL] $name : C# and PHP diverged"
    Write-Host "        C#:  $($cs -replace "`r?`n", ' | ')"
    Write-Host "        PHP: $($phpOut -replace "`r?`n", ' | ')"
    return $false
}

# Single-file programs.
foreach ($name in $programs) {
    if ($skip -contains $name) { Write-Host "[SKIP] $name (PHP module-scope follow-up)"; $skipped++; continue }
    $count++
    if (-not (Test-Program $name $name @("build", (Join-Path $progDir "$name.pg")))) { $fail++ }
}
# Multi-file programs (§4.5 module linking): each programs/<dir>/ with an entry.pg, built with --root <dir>.
foreach ($d in Get-ChildItem $progDir -Directory | Sort-Object Name) {
    $entry = Join-Path $d.FullName "entry.pg"
    if (-not (Test-Path $entry)) { continue }
    if ($skip -contains $d.Name) { Write-Host "[SKIP] $($d.Name) (PHP module-scope follow-up)"; $skipped++; continue }
    $count++
    if (-not (Test-Program $d.Name "entry" @("build", $entry, "--root", $d.FullName))) { $fail++ }
}

Write-Host ""
if ($fail -eq 0) {
    Write-Host "All $count conformance program(s) agree across PHP and C# ($refused refused by design, $skipped skipped: PHP module-scope follow-up)."
    exit 0
}
Write-Host "$fail of $count PHP program(s) diverged."
exit 1
