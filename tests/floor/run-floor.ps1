#!/usr/bin/env pwsh
# D8 floor gate (P40 review of #75, A9) — the emitted C# of the WHOLE conformance corpus compiles at the
# published floor: C# 11, the .NET 8 BCL, <Nullable>enable</Nullable> and <TreatWarningsAsErrors>true</>.
#
# Why a separate leg: the conformance oracle compiles each program with `/langversion:default` (the newest
# language) and no `/warnaserror`, and discards csc's output — so a construct that needs C# 12, or a warning a
# consumer with TreatWarningsAsErrors gets, passed every gate. The floor is a documented contract (README, the
# MSBuild .targets header, the nupkg description); this leg is what pins it.
#
# How: every conformance program is transpiled to C# with its own namespace (`csharp.namespace=Conf_<name>`,
# so 100+ `PolyglotProgram`s and preludes coexist), and the lot is compiled as ONE class library per BCL floor:
#   * net8.0 — every program whose C# needs nothing newer;
#   * net9.0 — only the programs whose C# uses a .NET 9 BCL type (std.collections Map<K, V> emits
#              OrderedDictionary<K, V>, the documented exception to the floor).
# Both at LangVersion 11, Nullable enable, TreatWarningsAsErrors. A real SDK build (not csc) so the targeting
# packs come from restore exactly as a consumer's would.
#
# THE ALLOW LIST (tests/floor/allowlist.txt) — TEMPORARY. When this leg landed, the emitter still produced the
# warnings the review's floor sweep found (custom-eq records without GetHashCode, definite assignment, unused
# catch variables, non-nullable user operator parameters, an unreachable trailing return, self-comparisons in
# the corpus). Those are fixed on the language/emitter side of the same PR; until that half merges, the list
# names EXACTLY those diagnostics (program, code, count) so this leg is correct and green on its own. The match
# is exact in both directions: a diagnostic not on the list fails, and a listed one that no longer fires fails
# too ("stale: remove it"), so the list can only shrink. AFTER THE MERGE THE LIST MUST BE EMPTY.
#
# Usage:  pwsh tests/floor/run-floor.ps1 [-Cli <path>] [-Keep]     (build the solution first). Needs dotnet.

param(
    [string]$Cli = (Join-Path $PSScriptRoot ".." ".." "x64" "Debug" "MintPlayer.Polyglot.Cli.exe"),
    [switch]$Keep   # keep the work dir (for inspecting the generated projects)
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
if (-not (Test-Path $Cli)) { Write-Host "polyglot CLI not found at $Cli — build the solution first (see CLAUDE.md)."; exit 2 }
if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) { Write-Host "dotnet not found on PATH."; exit 2 }
$Cli = (Resolve-Path $Cli).Path

$progDir = Join-Path $PSScriptRoot ".." "conformance" "programs"
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("polyglot-floor-" + [System.Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null

$fail = 0
function Fail($msg) { Write-Host "[FAIL] $msg"; $script:fail++ }

try {
    # 1. Transpile every program (single file, or a dir with entry.pg) into its own folder + namespace.
    $programs = @()
    foreach ($pg in Get-ChildItem $progDir -Filter *.pg | Sort-Object Name) { $programs += , @($pg.BaseName, $pg.FullName, $null) }
    foreach ($d in Get-ChildItem $progDir -Directory | Sort-Object Name) {
        $entry = Join-Path $d.FullName "entry.pg"
        if (Test-Path $entry) { $programs += , @($d.Name, $entry, $d.FullName) }
    }
    $byFloor = @{ "net8.0" = [System.Collections.Generic.List[string]]::new(); "net9.0" = [System.Collections.Generic.List[string]]::new() }
    $refused = 0
    foreach ($p in $programs) {
        $name = $p[0]
        $out = Join-Path $work "src" $name
        New-Item -ItemType Directory -Force $out | Out-Null
        $ns = "Conf_" + ($name -replace '[^A-Za-z0-9_]', '_')
        $bargs = @("build", $p[1], "--target", "csharp", "--lib", "io", "--out", $out, "--target-option", "csharp.namespace=$ns")
        if ($p[2]) { $bargs += @("--root", $p[2]) }
        $log = & $Cli @bargs 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) {
            # A program the C# target refuses by design has no C# to hold to the floor; anything else is a failure.
            if ($log -match 'refus') { $refused++; Remove-Item -Recurse -Force $out; continue }
            Fail "$name : transpile failed: $(($log.Trim() -split "`n" | Select-Object -First 2) -join ' ')"
            continue
        }
        $usesNet9 = @(Get-ChildItem $out -Filter *.cs | Select-String -SimpleMatch 'OrderedDictionary<' -List).Count -gt 0
        $byFloor[$(if ($usesNet9) { "net9.0" } else { "net8.0" })].Add($name)
    }
    Write-Host "==> D8 floor: $($byFloor['net8.0'].Count) program(s) at net8.0, $($byFloor['net9.0'].Count) at net9.0 (Map<K, V>), C# 11 + Nullable + TreatWarningsAsErrors$(if ($refused) { "; $refused refused by the C# target" })"
    if ($byFloor["net8.0"].Count -lt 50) { Fail "only $($byFloor['net8.0'].Count) program(s) reached the net8.0 floor library — the leg would pass vacuously" }

    # 2. One library per floor. Every diagnostic is an error (TreatWarningsAsErrors), and csc reports them all.
    $diags = [System.Collections.Generic.List[object]]::new()
    foreach ($tfm in @("net8.0", "net9.0")) {
        $names = $byFloor[$tfm]
        if ($names.Count -eq 0) { continue }
        $proj = Join-Path $work "Floor_$($tfm -replace '\.', '')"
        New-Item -ItemType Directory -Force $proj | Out-Null
        $items = ($names | ForEach-Object { "    <Compile Include=`"$(Join-Path $work 'src' $_)\**\*.cs`" />" }) -join "`n"
        @"
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>$tfm</TargetFramework>
    <OutputType>Library</OutputType>
    <LangVersion>11</LangVersion>
    <Nullable>enable</Nullable>
    <TreatWarningsAsErrors>true</TreatWarningsAsErrors>
    <ImplicitUsings>disable</ImplicitUsings>
    <EnableDefaultCompileItems>false</EnableDefaultCompileItems>
    <GenerateDocumentationFile>false</GenerateDocumentationFile>
  </PropertyGroup>
  <ItemGroup>
$items
  </ItemGroup>
</Project>
"@ | Set-Content (Join-Path $proj "Floor.csproj")
        $buildLog = Join-Path $work "build-$tfm.log"
        & dotnet build (Join-Path $proj "Floor.csproj") -nologo -v q -clp:NoSummary > $buildLog 2>&1
        $rc = $LASTEXITCODE
        $seen = @{}
        foreach ($line in Get-Content $buildLog) {
            # <work>/src/<program>/<file>.cs(L,C): error CS1234: message [proj]
            if ($line -match '[\\/]src[\\/]([^\\/]+)[\\/][^(]+\((\d+),(\d+)\): (?:error|warning) (CS\d{4}): ') {
                $key = "$($Matches[1])|$($Matches[2])|$($Matches[3])|$($Matches[4])"
                if ($seen.ContainsKey($key)) { continue }
                $seen[$key] = $true
                $diags.Add([pscustomobject]@{ Program = $Matches[1]; Code = $Matches[4]; Line = $line.Trim(); Tfm = $tfm })
            }
        }
        if ($rc -ne 0 -and $seen.Count -eq 0) {
            Fail "$tfm floor library failed without a C# diagnostic (restore/SDK problem?):"
            Get-Content $buildLog | Select-Object -First 20 | ForEach-Object { Write-Host "        $_" }
        }
    }

    # 3. Exact match against the (temporary) allow list: program code count.
    $allowFile = Join-Path $PSScriptRoot "allowlist.txt"
    $allowed = @{}
    if (Test-Path $allowFile) {
        foreach ($raw in Get-Content $allowFile) {
            $l = ($raw -replace '#.*$', '').Trim()
            if (-not $l) { continue }
            $f = $l -split '\s+'
            if ($f.Count -ne 3) { Fail "allowlist.txt: malformed line '$raw' (want: <program> <CSxxxx> <count>)"; continue }
            $allowed["$($f[0])|$($f[1])"] = [int]$f[2]
        }
    }
    $actual = @{}
    foreach ($d in $diags) { $k = "$($d.Program)|$($d.Code)"; $actual[$k] = 1 + ($actual[$k] ?? 0) }
    foreach ($k in ($actual.Keys | Sort-Object)) {
        $want = $allowed[$k] ?? 0
        if ($actual[$k] -ne $want) {
            $parts = $k -split '\|'
            Fail "$($parts[0]) : $($actual[$k]) x $($parts[1]) at the C# 11 / net8.0 / TreatWarningsAsErrors floor (allow-listed: $want)"
            $diags | Where-Object { "$($_.Program)|$($_.Code)" -eq $k } | Select-Object -First 3 | ForEach-Object { Write-Host "        $($_.Line)" }
        }
    }
    foreach ($k in ($allowed.Keys | Sort-Object)) {
        if (-not $actual.ContainsKey($k)) {
            $parts = $k -split '\|'
            Fail "allowlist.txt: '$($parts[0]) $($parts[1])' no longer fires — stale, remove it (the list must only shrink, to empty)"
        }
    }
    $allowTotal = ($allowed.Values | Measure-Object -Sum).Sum
    if ($fail -eq 0) {
        $note = if ($allowTotal) { " ($allowTotal allow-listed diagnostic(s) pending the emitter fixes — the list must be empty after the merge)" } else { "" }
        Write-Host "[PASS] the conformance corpus' C# compiles at C# 11 + net8.0 (Map: net9.0) + Nullable + TreatWarningsAsErrors$note"
    }
}
finally {
    if ($Keep) { Write-Host "(work dir kept: $work)" }
    else {
        & dotnet build-server shutdown --vbcscompiler *> $null
        Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
    }
}

Write-Host ""
if ($fail -eq 0) { Write-Host "D8 floor gate green."; exit 0 }
Write-Host "$fail floor check(s) failed."
exit 1
