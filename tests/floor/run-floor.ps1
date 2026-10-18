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
# Zero diagnostics, no allow list: when this leg landed (P40 review of #75) a temporary list named the 24
# diagnostics the review's sweep found; the emitter half of the same PR fixed every one at its root, and the
# list went with them. A new diagnostic is fixed in the compiler or the program, never allow-listed.
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

# The corpus' expected shape — asserted exactly, so a program silently dropping out of the leg is a failure:
#   * the programs whose C# uses a .NET 9 BCL type (std.collections Map<K, V> -> OrderedDictionary<K, V>);
#   * the programs the C# target refuses by design (no C# to hold to the floor). None today.
$expectedNet9 = @("coalesce_chain", "function_values", "map", "nullable_functions", "nullsafe_calls")   # coalesce_chain (r3 item 3) chains Map lookups; function_values / nullable_functions (r4) use Map; nullsafe_calls (r5 item 20) chains `?.` over Map.get
$expectedRefused = @()
# The canary compiled inside the net8.0 library (tests/floor/canary): exactly these diagnostics, or the leg is
# not observing what it claims (LangVersion 11 and TreatWarningsAsErrors live, csc actually run).
$canaryName = "__floor_canary"
# P40 review r4: with SEVERITY. CS0168 is a warning that only TreatWarningsAsErrors turns into an error, so a
# code-only comparison (the regex accepted `warning CS0168`) passed with TWAE switched off.
$canaryExpected = "error CS0168,error CS9058"
# P40 review r4: the corpus size is PINNED. Comparing the accounted count with the discovered count proved
# bookkeeping only — a corpus that lost programs still passed.
# P40 review r5: pinned EXACTLY (a `>=` floor let programs added since drop out silently). The ONE number to bump
# when a conformance program is added (or deliberately removed): every top-level tests/conformance/programs/*.pg
# plus every directory there holding an entry.pg. At c436aae: 157 = 152 files + 5 directories; after review round 5: 178 = 171 files + 7 directories (173 net8.0, 5 net9.0).
$expectedPrograms = 178
# Isolation from the machine: the work dir is under %TEMP%, so a Directory.Build.props/.targets (or a central
# package file) in any ancestor would be imported; and net8.0's end of support (2026-11-10) would add NETSDK1138,
# which can fail the build with no CS line. Global properties, because ImportDirectoryBuildProps is read by
# Sdk.props before the project body is evaluated.
$isolation = @("-p:ImportDirectoryBuildProps=false", "-p:ImportDirectoryBuildTargets=false",
               "-p:ImportDirectoryPackagesProps=false", "-p:CheckEolTargetFramework=false")

try {
    # 1. Transpile every program (single file, or a dir with entry.pg) into its own folder + namespace.
    $programs = @()
    foreach ($pg in Get-ChildItem $progDir -Filter *.pg | Sort-Object Name) { $programs += , @($pg.BaseName, $pg.FullName, $null) }
    foreach ($d in Get-ChildItem $progDir -Directory | Sort-Object Name) {
        $entry = Join-Path $d.FullName "entry.pg"
        if (Test-Path $entry) { $programs += , @($d.Name, $entry, $d.FullName) }
    }
    if ($programs.Count -ne $expectedPrograms) { Fail "found $($programs.Count) conformance program(s) under $progDir, expected exactly $expectedPrograms — a program was added or dropped out of the corpus (update `$expectedPrograms in tests/floor/run-floor.ps1 when that is deliberate)" }
    $byFloor = @{ "net8.0" = [System.Collections.Generic.List[string]]::new(); "net9.0" = [System.Collections.Generic.List[string]]::new() }
    $refused = [System.Collections.Generic.List[string]]::new()
    foreach ($p in $programs) {
        $name = $p[0]
        $out = Join-Path $work "src" $name
        New-Item -ItemType Directory -Force $out | Out-Null
        $ns = "Conf_" + ($name -replace '[^A-Za-z0-9_]', '_')
        $bargs = @("build", $p[1], "--target", "csharp", "--lib", "io", "--out", $out, "--target-option", "csharp.namespace=$ns")
        if ($p[2]) { $bargs += @("--root", $p[2]) }
        $log = & $Cli @bargs 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) {
            # Only a program LISTED as refused by the C# target may skip the floor, and only with an editor-shaped
            # error diagnostic. (A loose `-match 'refus'` used to skip any failure whose text said "refus" anywhere.)
            if ($expectedRefused -ccontains $name -and $log -cmatch ':\d+:\d+: error: ') { $refused.Add($name); Remove-Item -Recurse -Force $out; continue }
            Fail "$name : transpile failed: $(($log.Trim() -split "`n" | Select-Object -First 2) -join ' ')"
            continue
        }
        # The program's own C# must exist where the library's Compile glob looks (an empty folder would compile
        # nothing and pass): <name>.cs for a single file, entry.cs for a directory program.
        $own = Join-Path $out ($(if ($p[2]) { "entry" } else { $name }) + ".cs")
        if (-not (Test-Path -LiteralPath $own)) { Fail "$name : transpile exited 0 but wrote no $([System.IO.Path]::GetFileName($own))"; continue }
        $usesNet9 = @(Get-ChildItem $out -Filter *.cs | Select-String -SimpleMatch 'OrderedDictionary<' -List).Count -gt 0
        $byFloor[$(if ($usesNet9) { "net9.0" } else { "net8.0" })].Add($name)
    }
    Write-Host "==> D8 floor: $($byFloor['net8.0'].Count) program(s) at net8.0, $($byFloor['net9.0'].Count) at net9.0 (Map<K, V>), C# 11 + Nullable + TreatWarningsAsErrors$(if ($refused.Count) { "; $($refused.Count) refused by the C# target" })"
    # Exact bookkeeping, so the leg cannot pass without compiling the corpus:
    $accounted = $byFloor["net8.0"].Count + $byFloor["net9.0"].Count + $refused.Count
    if ($fail -eq 0 -and $accounted -ne $programs.Count) { Fail "accounted for $accounted of $($programs.Count) program(s)" }
    $net9 = @($byFloor["net9.0"] | Sort-Object) -join ","
    if ($net9 -cne (@($expectedNet9 | Sort-Object) -join ",")) { Fail "the net9.0 set is [$net9], expected [$(@($expectedNet9 | Sort-Object) -join ',')] — update `$expectedNet9 when a program starts or stops needing a .NET 9 type" }
    $ref = @($refused | Sort-Object) -join ","
    if ($ref -cne (@($expectedRefused | Sort-Object) -join ",")) { Fail "the refused set is [$ref], expected [$(@($expectedRefused | Sort-Object) -join ',')]" }
    if ($byFloor["net8.0"].Count -eq 0) { Fail "no program reached the net8.0 floor library — the leg would pass vacuously" }

    # 2. One library per floor. Every diagnostic is an error (TreatWarningsAsErrors), and csc reports them all.
    $diags = [System.Collections.Generic.List[object]]::new()
    foreach ($tfm in @("net8.0", "net9.0")) {
        $names = [System.Collections.Generic.List[string]]::new([string[]]@($byFloor[$tfm]))
        if ($names.Count -eq 0) { continue }
        if ($tfm -eq "net8.0") {
            # The canary joins the net8.0 library as one more "program" folder (see $canaryName above).
            $cdir = Join-Path $work "src" $canaryName
            New-Item -ItemType Directory -Force $cdir | Out-Null
            Copy-Item (Join-Path $PSScriptRoot "canary" "FloorCanary.cs") $cdir
            $names.Add($canaryName)
        }
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
    <CheckEolTargetFramework>false</CheckEolTargetFramework>
  </PropertyGroup>
  <ItemGroup>
$items
  </ItemGroup>
</Project>
"@ | Set-Content (Join-Path $proj "Floor.csproj")
        $buildLog = Join-Path $work "build-$tfm.log"
        # -nodeReuse:false: no MSBuild node outlives this build holding the work dir. The shared compiler server is
        # left alone (it used to be stopped with `dotnet build-server shutdown`, which also killed the server every
        # other build on the machine was using; VBCSCompiler holds no lock on the sources, so the cleanup below
        # does not need it gone).
        & dotnet build (Join-Path $proj "Floor.csproj") -nologo -v q -clp:NoSummary -nodeReuse:false @isolation > $buildLog 2>&1
        $rc = $LASTEXITCODE
        $seen = @{}
        foreach ($line in Get-Content $buildLog) {
            # <work>/src/<program>/<file>.cs(L,C): error CS1234: message [proj]
            if ($line -cmatch '[\\/]src[\\/]([^\\/]+)[\\/][^(]+\((\d+),(\d+)\): (error|warning) (CS\d{4}): ') {
                $key = "$($Matches[1])|$($Matches[2])|$($Matches[3])|$($Matches[5])"
                if ($seen.ContainsKey($key)) { continue }
                $seen[$key] = $true
                $diags.Add([pscustomobject]@{ Program = $Matches[1]; Code = $Matches[5]; Severity = $Matches[4]; Line = $line.Trim(); Tfm = $tfm })
            }
        }
        if ($rc -ne 0 -and $seen.Count -eq 0) {
            Fail "$tfm floor library failed without a C# diagnostic (restore/SDK problem?):"
            Get-Content $buildLog | Select-Object -First 20 | ForEach-Object { Write-Host "        $_" }
        }
    }

    # 3. Zero diagnostics. (The temporary allow list that let this leg land before the emitter fixes is gone:
    #    the floor is a contract, so a diagnostic is fixed at its root, never listed.)
    $canaryCodes = @($diags | Where-Object { $_.Program -ceq $canaryName } | ForEach-Object { "$($_.Severity) $($_.Code)" } | Sort-Object -Unique) -join ","
    if ($canaryCodes -cne $canaryExpected) {
        Fail "the canary produced [$canaryCodes], expected exactly [$canaryExpected] — the net8.0 library build did not observe LangVersion 11 + TreatWarningsAsErrors (or did not run csc)"
    } else { Write-Host "[PASS] canary: exactly $canaryExpected (C# 11 and TreatWarningsAsErrors are live)" }
    $byProgram = $diags | Where-Object { $_.Program -cne $canaryName } | Group-Object Program, Code | Sort-Object Name
    foreach ($g in $byProgram) {
        $first = $g.Group[0]
        Fail "$($first.Program) : $($g.Count) x $($first.Code) at the C# 11 / net8.0 / TreatWarningsAsErrors floor"
        $g.Group | Select-Object -First 3 | ForEach-Object { Write-Host "        $($_.Line)" }
    }
    if ($fail -eq 0) {
        Write-Host "[PASS] the conformance corpus' C# compiles at C# 11 + net8.0 (Map: net9.0) + Nullable + TreatWarningsAsErrors"
    }
}
finally {
    if ($Keep) { Write-Host "(work dir kept: $work)" }
    else {
        Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
    }
}

Write-Host ""
if ($fail -eq 0) { Write-Host "D8 floor gate green."; exit 0 }
Write-Host "$fail floor check(s) failed."
exit 1
