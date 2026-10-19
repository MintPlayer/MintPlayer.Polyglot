# P11 gate (PLAN §P11): a fresh C# project + MintPlayer.Polyglot.MSBuild + a .pg file builds with
# `dotnet build` alone; a second build skips transpilation; `dotnet clean` removes the generated files;
# and a project referencing the first does NOT inherit the transpile behavior or the package dependency.
# Packs the package from the working tree first (Debug CLI), so this validates current sources.
# P30 slice 7: the package no longer passes `--target csharp` — consumers declare a pgconfig.json with
# at least "targets": ["csharp"] (fixtures 1-6), `include` rules route extra targets outside obj
# (fixture 7), and a missing config is a guided refusal (fixture 8).
#
# Gate-speedup slice 5: the fixtures split into a strictly-serial dependent chain and four independent
# one-shots. Serial chain: pack (fixture 0) -> App (fixtures 1-3: build -> incremental -> touch ->
# clean, order-dependent) -> Downstream (fixture 4, references App.csproj, so it rebuilds App and must
# not run concurrently with the App chain). Independent one-shots (each its own project dir + obj/bin,
# no cross-dependency): Shared (5), MultiPg (6), Twin (7), NoCfg (8), TwoLibs (9, P40 B1), Stale (10, P40 B6+D17). By default the App first build
# (the one real cross-fixture race: first-install of the freshly-packed nupkg into the global NuGet
# cache) warms the cache, then the four one-shots run as ThreadJobs OVERLAPPING the rest of the serial
# chain. Concurrent restores of an already-cached package are safe; per-fixture dirs never collide.
# Output is buffered and printed in a fixed fixture order so parallel runs never interleave and the
# exit code / every assertion is identical to serial. `-Sequential` restores the pure serial run.
#
# P40 (issue #73) adds fixture 10 (Stale): PolyglotTargets, the outputs record + depfile + inputs cache
# (rename/delete a .pg, pgconfig/lockfile edits, re-routing a twin, net8.0;net10.0 isolation).
# `-Work <dir>` runs in a private directory with a private NuGet package cache (NUGET_PACKAGES), so two
# concurrent runs on one machine cannot evict or overwrite each other's freshly-packed package.
# P41 (issue #68, D10/D11) adds the evaluation-time checks to Stale: `dotnet msbuild -getItem:Compile` lists the
# generated .cs without running a target; add/rename/delete a .pg builds with no CS2002 and no CS2001.
# `-Cli <path>` (P41 slice 7, D12): pack THAT binary instead of the Windows x64\Debug exe. The package is
# assets-only and the .targets resolves tools/<host RID>/polyglot, so on Linux CI the runner stages the given
# CLI + its sibling plugins/ as <work>/stage/<rid>/ and packs with -p:PolyglotStageRoot (the csproj's
# multi-RID hook release.yml uses). Without -Cli the historical single-RID win-x64 pack is unchanged.
param([switch]$Sequential, [string]$Work, [string]$Cli)

$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$work = if ($Work) { [IO.Path]::GetFullPath($Work) } else { Join-Path ([IO.Path]::GetTempPath()) "polyglot-nuget-gate" }
if ($Work) { $env:NUGET_PACKAGES = Join-Path $work "packages" }
$feed = Join-Path $work "feed"

# Buffered check results (never printed inline): each fixture accumulates {Name;Passed} objects that
# are emitted at the end in a fixed order, so parallel fixtures can't interleave their output.
function fck($list, [bool]$cond, [string]$name) { $list.Add([pscustomobject]@{ Name = $name; Passed = [bool]$cond }) }

# ---- Independent one-shot fixtures (5-8). Each is fully self-contained: it takes only absolute paths
#      ($work) + the packed version, creates its own project dir, and returns its {Name;Passed} list.
#      Run inline under -Sequential, or as ThreadJobs (via [scriptblock]::Create(...) so the block is
#      unaffiliated with the caller runspace) overlapping the serial chain. Each sets its own
#      $ErrorActionPreference + $PSNativeCommandUseErrorActionPreference=$false (native non-zero exit
#      must not throw — NoCfg deliberately expects a failing build). ----

# 5. Shared library (§4.5 module linking / issue #11 1.B): two .pg files where one imports the other.
$sharedFixture = {
    param($work, $pkgVer)
    $ErrorActionPreference = 'Stop'
    $PSNativeCommandUseErrorActionPreference = $false
    $r = [System.Collections.Generic.List[object]]::new()
    function ck([bool]$c, [string]$n) { $r.Add([pscustomobject]@{ Name = $n; Passed = [bool]$c }) }

    $shared = Join-Path $work "Shared"
    dotnet new console -o $shared --framework net8.0 | Out-Null
    Copy-Item (Join-Path $work "nuget.config") (Join-Path $shared "nuget.config")
@"
class PgThing {
  var v: i32
  constructor(v: i32) { this.v = v }
}
fn twice(x: i32): i32 { return x * 2 }
"@ | Set-Content (Join-Path $shared "nn.pg")
@"
import { twice, PgThing } from "./nn"
fn compute(): i32 {
  let t = PgThing(21)
  return twice(t.v)
}
"@ | Set-Content (Join-Path $shared "game.pg")
    '{ "targets": ["csharp"] }' | Set-Content (Join-Path $shared "pgconfig.json")
@"
System.Console.WriteLine(PolyglotProgram.compute());
"@ | Set-Content (Join-Path $shared "Program.cs")
    dotnet add $shared package MintPlayer.Polyglot.MSBuild --version $pkgVer | Out-Null
    $sharedBuild = dotnet build $shared --nologo -v q 2>&1 | Out-String
    ck ($LASTEXITCODE -eq 0 -and $sharedBuild -notmatch 'CS0101') "shared-library .pg imports compile into one assembly (no CS0101)"
    ck ((Test-Path (Join-Path $shared "obj/Debug/net8.0/polyglot/game.cs")) -and (Test-Path (Join-Path $shared "obj/Debug/net8.0/polyglot/nn.cs"))) "both modules emit one flat .cs each"
    $sharedRun = dotnet run --project $shared --no-build 2>&1
    ck ("$sharedRun".Trim() -eq "42") "the shared library is usable across .pg files (runs, prints 42)"
    $r
}

# 6. Independent multi-.pg (issue #14): two .pg files that do NOT import each other (the FruitCake +
# Snake shape). Each is a separate link root, so the auto-emitted runtime prelude (Option/Some/None +
# the PolyglotProgram wrapper) must be hoisted into ONE shared __polyglot_prelude.cs — not duplicated
# per file — or the assembly fails with CS0101 + CS8863. This is the multi-ROOT case fixture 5 does not
# exercise.
$multiFixture = {
    param($work, $pkgVer)
    $ErrorActionPreference = 'Stop'
    $PSNativeCommandUseErrorActionPreference = $false
    $r = [System.Collections.Generic.List[object]]::new()
    function ck([bool]$c, [string]$n) { $r.Add([pscustomobject]@{ Name = $n; Passed = [bool]$c }) }

    $multi = Join-Path $work "MultiPg"
    dotnet new console -o $multi --framework net8.0 | Out-Null
    Copy-Item (Join-Path $work "nuget.config") (Join-Path $multi "nuget.config")
    # P40 §0: the prelude is pruned to what is REACHED, so both roots use Option — otherwise there is no
    # prelude to hoist and no file to assert on.
    "fn pick(o: Option<i32>, d: i32): i32 => match o {`n  Some(x) => x,`n  None => d,`n}`nfn step(): i32 {`n  return pick(Some(1), 0)`n}`n" | Set-Content (Join-Path $multi "phys.pg")
    "fn pick2(o: Option<i32>, d: i32): i32 => match o {`n  Some(x) => x,`n  None => d,`n}`nfn tick(): i32 {`n  return pick2(None, 2)`n}`n" | Set-Content (Join-Path $multi "snake.pg")
    '{ "targets": ["csharp"] }' | Set-Content (Join-Path $multi "pgconfig.json")
@"
System.Console.WriteLine(PolyglotProgram.step() + PolyglotProgram.tick());
"@ | Set-Content (Join-Path $multi "Program.cs")
    dotnet add $multi package MintPlayer.Polyglot.MSBuild --version $pkgVer | Out-Null
    $multiBuild = dotnet build $multi --nologo -v q 2>&1 | Out-String
    ck ($LASTEXITCODE -eq 0 -and $multiBuild -notmatch 'CS0101' -and $multiBuild -notmatch 'CS8863') `
        "two independent .pg files compile into one assembly (no CS0101/CS8863 - issue #14)"
    $multiRun = dotnet run --project $multi --no-build 2>&1
    ck ("$multiRun".Trim() -eq "3") "independent multi-.pg project runs (prints 3)"
    ck (Test-Path (Join-Path $multi "obj/Debug/net8.0/polyglot/__polyglot_prelude.cs")) `
        "the shared runtime prelude is emitted once as __polyglot_prelude.cs"
    $r
}

# 7. P30 slice 7 — pgconfig-driven multi-target: `dotnet build` alone compiles the C# from obj AND
# routes the TypeScript twin outside the project via an `include` rule; the twin is a committed source
# artifact (write-if-changed, survives clean, never in FileWrites).
$twinFixture = {
    param($work, $pkgVer)
    $ErrorActionPreference = 'Stop'
    $PSNativeCommandUseErrorActionPreference = $false
    $r = [System.Collections.Generic.List[object]]::new()
    function ck([bool]$c, [string]$n) { $r.Add([pscustomobject]@{ Name = $n; Passed = [bool]$c }) }

    $twin = Join-Path $work "Twin"
    dotnet new console -o $twin --framework net8.0 | Out-Null
    Copy-Item (Join-Path $work "nuget.config") (Join-Path $twin "nuget.config")
    "fn answer(): i32 {`n  return 42`n}`n" | Set-Content (Join-Path $twin "calc.pg")
@"
{
  "targets": ["csharp", "typescript"],
  "include": [
    { "pattern": "*.pg", "target": "typescript", "output": "../web/gen/%(Filename)" }
  ]
}
"@ | Set-Content (Join-Path $twin "pgconfig.json")
    "System.Console.WriteLine(PolyglotProgram.answer());" | Set-Content (Join-Path $twin "Program.cs")
    dotnet add $twin package MintPlayer.Polyglot.MSBuild --version $pkgVer | Out-Null
    dotnet build $twin --nologo -v q 2>&1 | Out-Null
    $twinCs = Join-Path $twin "obj/Debug/net8.0/polyglot/calc.cs"
    $twinTs = Join-Path $work "web/gen/calc.ts"
    ck (Test-Path $twinCs) "multi-target: the C# leg still lands in obj and compiles"
    ck (Test-Path $twinTs) "multi-target: the include rule routes the .ts twin outside the project"
    $twinRun = dotnet run --project $twin --no-build 2>&1
    ck ("$twinRun".Trim() -eq "42") "multi-target project runs (prints 42)"
    $tsStamp = (Get-Item $twinTs).LastWriteTimeUtc
    Start-Sleep -Milliseconds 300
    dotnet build $twin --nologo -v q 2>&1 | Out-Null
    ck ((Get-Item $twinTs).LastWriteTimeUtc -eq $tsStamp) "incremental build leaves the twin untouched"
    (Get-Item (Join-Path $twin "calc.pg")).LastWriteTime = Get-Date
    dotnet build $twin --nologo -v q 2>&1 | Out-Null
    ck ((Get-Item $twinTs).LastWriteTimeUtc -eq $tsStamp) `
        "an unchanged re-transpile does not rewrite the twin (write-if-changed)"
    "fn answer(): i32 {`n  return 43`n}`n" | Set-Content (Join-Path $twin "calc.pg")
    dotnet build $twin --nologo -v q 2>&1 | Out-Null
    ck ((Get-Content $twinTs -Raw) -match '43') "a real .pg change refreshes the twin's content"
    dotnet clean $twin --nologo -v q 2>&1 | Out-Null
    ck ((-not (Test-Path $twinCs)) -and (Test-Path $twinTs)) `
        "dotnet clean removes the obj .cs but NEVER the external twin"
    $r
}

# 8. No pgconfig.json: the build refuses with the guided message (the config-sourced contract).
$noCfgFixture = {
    param($work, $pkgVer)
    $ErrorActionPreference = 'Stop'
    $PSNativeCommandUseErrorActionPreference = $false
    $r = [System.Collections.Generic.List[object]]::new()
    function ck([bool]$c, [string]$n) { $r.Add([pscustomobject]@{ Name = $n; Passed = [bool]$c }) }

    $nocfg = Join-Path $work "NoCfg"
    dotnet new console -o $nocfg --framework net8.0 | Out-Null
    Copy-Item (Join-Path $work "nuget.config") (Join-Path $nocfg "nuget.config")
    "fn x(): i32 {`n  return 1`n}`n" | Set-Content (Join-Path $nocfg "x.pg")
    dotnet add $nocfg package MintPlayer.Polyglot.MSBuild --version $pkgVer | Out-Null
    $nocfgBuild = dotnet build $nocfg --nologo 2>&1 | Out-String
    ck ($LASTEXITCODE -ne 0 -and $nocfgBuild -match 'no --target given and no pgconfig\.json') `
        "a consumer without a pgconfig.json gets the guided refusal"
    $r
}

# 10. P40 (issue #73, B6 + D17): PolyglotTargets, the outputs record and the inputs cache, against the
# real .props/.targets. Reproduces the two bugs the follow-up found — a deleted .pg's function still
# linked from a stale obj/ .cs, and a pgconfig.json edit did not regenerate — and the target filter.
# P41 (issue #68, D11): also the evaluation-time Compile items (-getItem:Compile) and the no-CS2001/CS2002 contract.
$staleFixture = {
    param($work, $pkgVer)
    $ErrorActionPreference = 'Stop'
    $PSNativeCommandUseErrorActionPreference = $false
    $r = [System.Collections.Generic.List[object]]::new()
    function ck([bool]$c, [string]$n) { $r.Add([pscustomobject]@{ Name = $n; Passed = [bool]$c }) }

    $p = Join-Path $work "Stale"
    $web = Join-Path $work "stale-web"
    dotnet new console -o $p --framework net8.0 | Out-Null
    Copy-Item (Join-Path $work "nuget.config") (Join-Path $p "nuget.config")
    "fn alpha(): i32 {`n  return 1`n}`n" | Set-Content (Join-Path $p "a.pg")
    "fn beta(): i32 {`n  return 2`n}`n" | Set-Content (Join-Path $p "b.pg")
    $cfg = { param($out, $extra) "{ `"targets`": [`"csharp`", `"typescript`"]$extra, `"include`": [ { `"pattern`": `"*.pg`", `"target`": `"typescript`", `"output`": `"$out/%(Filename)`" } ] }" }
    (& $cfg "../stale-web/gen" "") | Set-Content (Join-Path $p "pgconfig.json")
    "System.Console.WriteLine(PolyglotProgram.alpha() + PolyglotProgram.beta());" | Set-Content (Join-Path $p "Program.cs")
    dotnet add $p package MintPlayer.Polyglot.MSBuild --version $pkgVer | Out-Null
    $obj = Join-Path $p "obj/Debug/net8.0/polyglot"
    $stamp = Join-Path $p "obj/Debug/net8.0/polyglot.stamp"

    # "csharp;" — a trailing separator as a consumer writes it in the .csproj (review B7: the CLI now refuses an
    # empty --only entry, so the .targets must pass PolyglotTargets as an item list). Gated by a test-only switch.
    $sp = Join-Path $p "Stale.csproj"
    (Get-Content -Raw $sp).Replace("</Project>", "  <PropertyGroup Condition=`"'`$(PgTrailingSep)' == 'true'`"><PolyglotTargets>csharp;</PolyglotTargets></PropertyGroup>`n</Project>") | Set-Content $sp
    $b = dotnet build $p --nologo -v q -p:PgTrailingSep=true 2>&1 | Out-String
    ck ($LASTEXITCODE -eq 0 -and (Test-Path (Join-Path $obj "a.cs")) -and -not (Test-Path (Join-Path $web "gen"))) `
        "P40: PolyglotTargets=csharp (spelled 'csharp;') compiles the C# and writes no .ts twin"
    dotnet build $p --nologo -v q 2>&1 | Out-Null
    ck ((Test-Path (Join-Path $web "gen/a.ts")) -and (Test-Path (Join-Path $web "gen/b.ts"))) `
        "P40: flipping PolyglotTargets back to all targets re-transpiles and writes the twin"
    ck ((Test-Path (Join-Path $p "obj/Debug/net8.0/polyglot.outputs.json")) -and (Test-Path (Join-Path $p "obj/Debug/net8.0/polyglot.deps"))) `
        "P40: the record and depfile live in the intermediate dir, outside PolyglotOutDir"

    # P41 (issue #68, D10/D11): the generated .cs are Compile items at EVALUATION time (-getItem runs no target),
    # derived from the outputs list the last build wrote — what Solution Explorer / IntelliSense see from load.
    # Paths are matched case-insensitively (-match is; the list's drive-letter case can differ on Windows).
    $evalItems = dotnet msbuild $p -getItem:Compile -nologo 2>&1 | Out-String
    ck ($evalItems -match 'polyglot[\\/]a\.cs' -and $evalItems -match 'polyglot[\\/]b\.cs') `
        "P41 #68: after a build, -getItem:Compile lists the generated polyglot/a.cs + b.cs at evaluation (no target ran)"

    # The rename adds c.pg and removes b.pg in one build: the evaluation-time items (a.cs, b.cs) must be replaced by
    # the target's fresh set — no CS2002 (c.cs or a.cs added twice) and no CS2001 (b.cs kept as a concrete stale path).
    Move-Item (Join-Path $p "b.pg") (Join-Path $p "c.pg")
    $b = dotnet build $p --nologo -v q 2>&1 | Out-String
    ck ($LASTEXITCODE -eq 0 -and -not (Test-Path (Join-Path $obj "b.cs")) -and (Test-Path (Join-Path $obj "c.cs")) -and
        -not (Test-Path (Join-Path $web "gen/b.ts")) -and (Test-Path (Join-Path $web "gen/c.ts"))) `
        "P40: renaming a .pg re-runs the transpile and removes the stale .cs and .ts"
    ck ($b -notmatch 'CS2002' -and $b -notmatch 'CS2001') `
        "P41 #68: adding c.pg (and dropping b.pg) builds without CS2002 (duplicate) or CS2001 (stale evaluation-time path)"
    $evalItems = dotnet msbuild $p -getItem:Compile -nologo 2>&1 | Out-String
    ck ($evalItems -match 'polyglot[\\/]c\.cs' -and $evalItems -notmatch 'polyglot[\\/]b\.cs') `
        "P41 #68: ... and -getItem:Compile now lists c.cs and no longer b.cs"

    Remove-Item (Join-Path $p "a.pg")
    $b = dotnet build $p --nologo -v q 2>&1 | Out-String
    ck ($LASTEXITCODE -ne 0 -and $b -match 'CS0117' -and -not (Test-Path (Join-Path $obj "a.cs"))) `
        "P40: deleting a .pg removes its .cs, so calling its function fails (CS0117) instead of linking stale code"
    # P41 #68: the failure is the CALLER's (CS0117), never a stale evaluation-time a.cs path (CS2001); afterwards the
    # evaluation-time set (fresh list) no longer names a.cs either.
    $evalItems = dotnet msbuild $p -getItem:Compile -nologo 2>&1 | Out-String
    ck ($b -notmatch 'CS2001' -and $evalItems -notmatch 'polyglot[\\/]a\.cs' -and $evalItems -match 'polyglot[\\/]c\.cs') `
        "P41 #68: deleting a.pg gives no CS2001, and -getItem:Compile drops a.cs after the build"
    "System.Console.WriteLine(PolyglotProgram.beta());" | Set-Content (Join-Path $p "Program.cs")
    dotnet build $p --nologo -v q 2>&1 | Out-Null
    $run = dotnet run --project $p --no-build 2>&1
    ck ("$run".Trim() -eq "2") "P40: ... and the project builds and runs again without it (prints 2)"

    (& $cfg "../stale-web/gen" ", `"originInfo`": true") | Set-Content (Join-Path $p "pgconfig.json")
    dotnet build $p --nologo -v q 2>&1 | Out-Null
    ck ((Get-Content -Raw (Join-Path $obj "c.cs")) -match '(?m)^#line ') "P40: a pgconfig.json edit regenerates (it is a depfile input)"

    '{ "lockfileVersion": 1, "packages": {} }' | Set-Content (Join-Path $p "pgconfig.lock.json")
    (Get-Item (Join-Path $p "pgconfig.json")).LastWriteTime = Get-Date   # pick the new lockfile up
    dotnet build $p --nologo -v q 2>&1 | Out-Null
    $s0 = (Get-Item $stamp).LastWriteTimeUtc
    Start-Sleep -Milliseconds 300
    dotnet build $p --nologo -v q 2>&1 | Out-Null
    $s1 = (Get-Item $stamp).LastWriteTimeUtc
    (Get-Item (Join-Path $p "pgconfig.lock.json")).LastWriteTime = Get-Date
    dotnet build $p --nologo -v q 2>&1 | Out-Null
    $s2 = (Get-Item $stamp).LastWriteTimeUtc
    ck ($s1 -eq $s0 -and $s2 -ne $s1) "P40: an unchanged build skips the transpile, a lockfile edit re-runs it"

    (& $cfg "../stale-web/gen2" ", `"originInfo`": true") | Set-Content (Join-Path $p "pgconfig.json")
    dotnet build $p --nologo -v q 2>&1 | Out-Null
    ck (-not (Test-Path (Join-Path $web "gen/c.ts")) -and -not (Test-Path (Join-Path $web "gen/c.ts.map")) -and (Test-Path (Join-Path $web "gen2/c.ts"))) `
        "P40: re-routing the twin deletes the old one (and its .map)"

    # P40 review r4: PolyglotOutDir is part of the inputs cache — a changed out dir re-runs the transpile instead
    # of skipping it (CoreCompile then failed with CS0103 until a .pg changed).
    $b = dotnet build $p --nologo -v q "-p:PolyglotOutDir=obj\alt\" 2>&1 | Out-String
    ck ($LASTEXITCODE -eq 0 -and (Test-Path (Join-Path $p "obj/alt/c.cs"))) "P40 review r4: -p:PolyglotOutDir=obj\alt\ re-transpiles into it and builds (no CS0103)"
    dotnet build $p --nologo -v q 2>&1 | Out-Null

    # net8;net10 — one record per intermediate dir: a single-TFM build never deletes the other TFM's outputs.
    $m = Join-Path $work "StaleMulti"
    dotnet new classlib -o $m --framework net8.0 | Out-Null
    Copy-Item (Join-Path $work "nuget.config") (Join-Path $m "nuget.config")
    $csproj = Join-Path $m "StaleMulti.csproj"
    (Get-Content -Raw $csproj) -replace '<TargetFramework>net8.0</TargetFramework>', '<TargetFrameworks>net8.0;net10.0</TargetFrameworks>' | Set-Content $csproj
    "fn alpha(): i32 {`n  return 1`n}`n" | Set-Content (Join-Path $m "a.pg")
    '{ "targets": ["csharp"] }' | Set-Content (Join-Path $m "pgconfig.json")
    dotnet add $m package MintPlayer.Polyglot.MSBuild --version $pkgVer | Out-Null
    dotnet build $m --nologo -v q 2>&1 | Out-Null
    $o8 = Join-Path $m "obj/Debug/net8.0/polyglot"; $o10 = Join-Path $m "obj/Debug/net10.0/polyglot"
    $bothOk = (Test-Path (Join-Path $o8 "a.cs")) -and (Test-Path (Join-Path $o10 "a.cs"))
    Move-Item (Join-Path $m "a.pg") (Join-Path $m "z.pg")
    dotnet build $m --nologo -v q -f net8.0 2>&1 | Out-Null
    ck ($bothOk -and (Test-Path (Join-Path $o8 "z.cs")) -and -not (Test-Path (Join-Path $o8 "a.cs")) -and (Test-Path (Join-Path $o10 "a.cs"))) `
        "P40: net8.0;net10.0 — a net8.0-only build cleans its own obj and never touches net10.0's"

    # P40 review (#75, D17 under MSBuild): with 3+ .pg files the surviving .cs files are byte-identical after
    # a delete, so nothing they contain changes. The transpile + item injection must therefore run BEFORE the
    # SDK hashes @(Compile) (_GenerateCompileDependencyCache), or CoreCompile is skipped as up to date and the
    # stale DLL keeps the deleted function. (Before P41 the 2 -> 1 case above passed only because the survivor
    # flipped from `static partial class` to `static class`; the wrapper is now `partial` unconditionally, so a
    # module's text never depends on how many files share the class — issue #76, D15.) Program.cs is NOT
    # edited between builds here, so only a correct @(Compile) hash can make CoreCompile run.
    $flags = "System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.NonPublic"
    $three = Join-Path $work "StaleThree"
    dotnet new console -o $three --framework net8.0 | Out-Null
    Copy-Item (Join-Path $work "nuget.config") (Join-Path $three "nuget.config")
    foreach ($fn in @(@("a", "alpha", 1), @("b", "beta", 2), @("c", "gamma", 3), @("d", "delta", 4))) {
        "fn $($fn[1])(): i32 {`n  return $($fn[2])`n}`n" | Set-Content (Join-Path $three "$($fn[0]).pg")
    }
    '{ "targets": ["csharp"] }' | Set-Content (Join-Path $three "pgconfig.json")
    "System.Console.WriteLine(`$`"{PolyglotProgram.alpha() + PolyglotProgram.beta() + PolyglotProgram.gamma()} {typeof(PolyglotProgram).GetMethod(`"delta`", $flags) != null}`");" |
        Set-Content (Join-Path $three "Program.cs")
    dotnet add $three package MintPlayer.Polyglot.MSBuild --version $pkgVer | Out-Null
    dotnet build $three --nologo -v q 2>&1 | Out-Null
    $run0 = "$(dotnet run --project $three --no-build 2>&1)".Trim()
    Remove-Item (Join-Path $three "d.pg")
    $b = dotnet build $three --nologo -v q 2>&1 | Out-String
    $bRc = $LASTEXITCODE
    $run1 = "$(dotnet run --project $three --no-build 2>&1)".Trim()
    ck ($run0 -eq "6 True" -and $bRc -eq 0 -and $run1 -eq "6 False") `
        "P40 review: deleting 1 of 4 .pg (3 unchanged survivors) recompiles; run --no-build no longer sees the deleted fn ('$run0' -> '$run1')"
    Remove-Item (Join-Path $three "c.pg")
    $b = dotnet build $three --nologo -v q 2>&1 | Out-String
    ck ($LASTEXITCODE -ne 0 -and $b -match 'CS0117') `
        "P40 review: deleting a called .pg with unchanged survivors fails a plain incremental build with CS0117"
    $b = dotnet build $three --nologo -v q 2>&1 | Out-String
    ck ($LASTEXITCODE -ne 0 -and $b -match 'CS0117') "P40 review: ... and keeps failing on the next build (no stale success)"

    # The same under multi-targeting: BOTH TFMs must recompile (each has its own record and hash).
    $mt = Join-Path $work "StaleMultiDel"
    dotnet new console -o $mt --framework net8.0 | Out-Null
    Copy-Item (Join-Path $work "nuget.config") (Join-Path $mt "nuget.config")
    $mtProj = Join-Path $mt "StaleMultiDel.csproj"
    (Get-Content -Raw $mtProj) -replace '<TargetFramework>net8.0</TargetFramework>', '<TargetFrameworks>net8.0;net10.0</TargetFrameworks>' | Set-Content $mtProj
    foreach ($fn in @(@("a", "alpha", 1), @("b", "beta", 2), @("c", "gamma", 3), @("d", "delta", 4))) {
        "fn $($fn[1])(): i32 {`n  return $($fn[2])`n}`n" | Set-Content (Join-Path $mt "$($fn[0]).pg")
    }
    '{ "targets": ["csharp"] }' | Set-Content (Join-Path $mt "pgconfig.json")
    "System.Console.WriteLine(typeof(PolyglotProgram).GetMethod(`"delta`", $flags) != null);" | Set-Content (Join-Path $mt "Program.cs")
    dotnet add $mt package MintPlayer.Polyglot.MSBuild --version $pkgVer | Out-Null
    dotnet build $mt --nologo -v q 2>&1 | Out-Null
    $before = "$(dotnet run --project $mt --no-build -f net8.0 2>&1)".Trim() + "/" + "$(dotnet run --project $mt --no-build -f net10.0 2>&1)".Trim()
    Remove-Item (Join-Path $mt "d.pg")
    dotnet build $mt --nologo -v q 2>&1 | Out-Null
    $mtRc = $LASTEXITCODE
    $after = "$(dotnet run --project $mt --no-build -f net8.0 2>&1)".Trim() + "/" + "$(dotnet run --project $mt --no-build -f net10.0 2>&1)".Trim()
    ck ($before -eq "True/True" -and $mtRc -eq 0 -and $after -eq "False/False") `
        "P40 review: net8.0;net10.0 — a deleted .pg recompiles BOTH TFMs ('$before' -> '$after')"
    $r
}

# 11. P40 review (B8): Spark's REAL layout — a library .csproj with the package, pgconfig targets
# [csharp, typescript] and ONLY a typescript include rule (owned, into a sibling web tree), so the C# lands in
# obj/ via MSBuild and nowhere else. A git repository with a committed twin, then a real `git clone` (no record,
# no obj/), an importer (so an edit is `would update`), whole-tree snapshots, and Spark's exact commands:
# `polyglot build --check --only typescript` in CI and `polyglot build --only typescript` as the refresh.
# The refresh must leave the next `dotnet build` working. A BARE `polyglot build` in this layout also emits
# C# next to every .pg (no include rule routes it), which the SDK's default **/*.cs glob compiles alongside
# obj/'s copy -> CS0111; README "MSBuild" documents why the refresh command is `--only typescript`.
$sparkLayoutFixture = {
    param($work, $pkgVer, $cli)
    $ErrorActionPreference = 'Stop'
    $PSNativeCommandUseErrorActionPreference = $false
    $r = [System.Collections.Generic.List[object]]::new()
    function ck([bool]$c, [string]$n) { $r.Add([pscustomobject]@{ Name = $n; Passed = [bool]$c }) }
    function Snap([string]$dir) {
        (Get-ChildItem -LiteralPath $dir -Recurse -File -Force | Where-Object { $_.FullName -notmatch '[\\/]\.git[\\/]' } |
            Sort-Object FullName | ForEach-Object { "$($_.FullName)=$((Get-FileHash $_.FullName).Hash)" }) -join "`n"
    }
    function Pgc([string]$dir, [string[]]$a) {
        Push-Location $dir
        try { $o = (& $cli @a 2>&1 | Out-String); $rc = $LASTEXITCODE } finally { Pop-Location }
        [pscustomobject]@{ Rc = $rc; Out = $o }
    }
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { ck $false "spark-layout: git is required for the real-clone flow"; return $r }

    $origin = Join-Path $work "SparkOrigin"
    $lib = Join-Path $origin "libs/Gs1"
    dotnet new classlib -o $lib --framework net8.0 | Out-Null
    Remove-Item (Join-Path $lib "Class1.cs") -ErrorAction SilentlyContinue
    Copy-Item (Join-Path $work "nuget.config") (Join-Path $origin "nuget.config")
    "fn checkDigit(n: i32): i32 {`n  return n % 10`n}`n" | Set-Content (Join-Path $lib "digits.pg")
    "import { checkDigit } from `"./digits`"`nfn valid(n: i32): bool {`n  return checkDigit(n) == 7`n}`n" | Set-Content (Join-Path $lib "gtin.pg")
    "fn legacy(): i32 {`n  return 1`n}`n" | Set-Content (Join-Path $lib "legacy.pg")
    '{ "targets": ["csharp", "typescript"], "include": [ { "pattern": "*.pg", "target": "typescript", "output": "../../web/src/gen/%(Filename)", "owned": true } ] }' |
        Set-Content (Join-Path $lib "pgconfig.json")
    dotnet add $lib package MintPlayer.Polyglot.MSBuild --version $pkgVer | Out-Null
    $b = dotnet build $lib --nologo -v q 2>&1 | Out-String
    $gen = Join-Path $origin "web/src/gen"
    ck ($LASTEXITCODE -eq 0 -and (Test-Path (Join-Path $gen "gtin.ts")) -and (Test-Path (Join-Path $lib "obj/Debug/net8.0/polyglot/gtin.cs")) -and
        -not (Test-Path (Join-Path $lib "gtin.cs"))) "spark-layout: dotnet build puts the C# in obj/ only and the TS twin in the owned web folder"
    "bin/`nobj/`n" | Set-Content (Join-Path $origin ".gitignore")
    "* -text" | Set-Content (Join-Path $origin ".gitattributes")   # no EOL conversion on checkout (Spark pins eol=lf for its twin)
    # P40 review r3: the machine's commit signing and hooks must not decide this gate.
    $gq = @("-c", "user.name=gate", "-c", "user.email=gate@example.invalid", "-c", "commit.gpgsign=false", "-c", "core.hooksPath=")
    & git @gq -C $origin init -q 2>&1 | Out-Null
    & git @gq -C $origin add -A 2>&1 | Out-Null
    & git @gq -C $origin commit -q -m "lib + twin" 2>&1 | Out-Null

    $clone = Join-Path $work "SparkClone"
    & git @gq clone -q $origin $clone 2>&1 | Out-Null
    $clib = Join-Path $clone "libs/Gs1"
    $cgen = Join-Path $clone "web/src/gen"
    ck ((Test-Path (Join-Path $cgen "gtin.ts")) -and -not (Test-Path (Join-Path $clib "obj")) -and -not (Test-Path (Join-Path $clib ".polyglot"))) `
        "spark-layout: a real git clone has the committed twin, no obj/ and no record"
    $before = Snap $clone
    $c = Pgc $clib @("build", "--check", "--only", "typescript")
    ck ($c.Rc -eq 0 -and (Snap $clone) -ceq $before) "spark-layout: CI's check on the clean clone exits 0 and writes nothing (no .polyglot/)"

    # A rename, a deletion and an edit the importer reaches; then CI's check, Spark's refresh, dotnet build.
    & git @gq -C $clone mv libs/Gs1/legacy.pg libs/Gs1/legacy2.pg 2>&1 | Out-Null
    "fn checkDigit(n: i32): i32 {`n  return (n + 3) % 10`n}`n" | Set-Content (Join-Path $clib "digits.pg")
    $before = Snap $clone
    $c = Pgc $clib @("build", "--check", "--only", "typescript")
    ck ($c.Rc -eq 3 -and $c.Out -match 'would create .*legacy2\.ts' -and $c.Out -match 'would delete .*legacy\.ts' -and
        $c.Out -match 'would update .*digits\.ts' -and $c.Out -notmatch '\.cs\b') `
        "spark-layout: --check --only typescript: would create + would delete + would update, never a .cs (exit 3)"
    ck ((Snap $clone) -ceq $before -and -not (Test-Path (Join-Path $clib ".polyglot"))) "spark-layout: ... and the whole clone is byte-identical afterwards"
    $c = Pgc $clib @("build", "--only", "typescript")
    ck ($c.Rc -eq 0 -and -not (Test-Path (Join-Path $cgen "legacy.ts")) -and (Test-Path (Join-Path $cgen "legacy2.ts")) -and
        @(Get-ChildItem $clib -Filter *.cs).Count -eq 0) "spark-layout: the refresh (build --only typescript) fixes the twin and writes no C# next to the .pg"
    ck ((Pgc $clib @("build", "--check", "--only", "typescript")).Rc -eq 0) "spark-layout: the full twin check is clean after the refresh"
    $b = dotnet build $clib --nologo -v q 2>&1 | Out-String
    ck ($LASTEXITCODE -eq 0 -and $b -notmatch 'CS0111|CS0101') "spark-layout: dotnet build after the refresh succeeds (no duplicate C#)"
    $r
}

# 12. P40 review (B9): `access: "internal"` (the C# default) across assemblies. Two class libraries, each with
# INTERNAL generated code in its own namespace, both granting InternalsVisibleTo to one consumer, which uses both.
$internalLibsFixture = {
    param($work, $pkgVer)
    $ErrorActionPreference = 'Stop'
    $PSNativeCommandUseErrorActionPreference = $false
    $r = [System.Collections.Generic.List[object]]::new()
    function ck([bool]$c, [string]$n) { $r.Add([pscustomobject]@{ Name = $n; Passed = [bool]$c }) }

    $root = Join-Path $work "InternalLibs"
    $libA = Join-Path $root "IntA"; $libB = Join-Path $root "IntB"; $app = Join-Path $root "IntApp"
    dotnet new classlib -o $libA --framework net8.0 | Out-Null
    dotnet new classlib -o $libB --framework net8.0 | Out-Null
    dotnet new console -o $app --framework net8.0 | Out-Null
    Copy-Item (Join-Path $work "nuget.config") (Join-Path $root "nuget.config")
    Remove-Item (Join-Path $libA "Class1.cs"), (Join-Path $libB "Class1.cs") -ErrorAction SilentlyContinue
    "fn find(): Option<i32> => Some(20)`nfn name(): string => `"A`"`n" | Set-Content (Join-Path $libA "a.pg")
    "fn find(): Option<i32> => Some(22)`nfn name(): string => `"B`"`n" | Set-Content (Join-Path $libB "b.pg")
    '{ "targets": ["csharp"], "targetOptions": { "csharp": { "namespace": "IntA", "access": "internal" } } }' | Set-Content (Join-Path $libA "pgconfig.json")
    '{ "targets": ["csharp"], "targetOptions": { "csharp": { "namespace": "IntB" } } }' | Set-Content (Join-Path $libB "pgconfig.json")
    foreach ($l in @($libA, $libB)) {
        $proj = Join-Path $l "$(Split-Path $l -Leaf).csproj"
        (Get-Content $proj -Raw).Replace("</Project>", "  <ItemGroup><InternalsVisibleTo Include=`"IntApp`" /></ItemGroup>`n</Project>") | Set-Content $proj
        dotnet add $l package MintPlayer.Polyglot.MSBuild --version $pkgVer | Out-Null
    }
    dotnet add $app reference (Join-Path $libA "IntA.csproj") (Join-Path $libB "IntB.csproj") | Out-Null
@"
var a = IntA.PolyglotProgram.find();
var b = IntB.PolyglotProgram.find();
int va = a is IntA.Some<int> sa ? sa.value : 0;
int vb = b is IntB.Some<int> sb ? sb.value : 0;
System.Console.WriteLine(IntA.PolyglotProgram.name() + IntB.PolyglotProgram.name() + (va + vb));
"@ | Set-Content (Join-Path $app "Program.cs")
    $build = dotnet build $app --nologo -v q 2>&1 | Out-String
    $genA = Get-Content -Raw (Join-Path $libA "obj/Debug/net8.0/polyglot/a.cs") -ErrorAction SilentlyContinue
    $genB = Get-Content -Raw (Join-Path $libB "obj/Debug/net8.0/polyglot/b.cs") -ErrorAction SilentlyContinue
    ck ("$genA$genB" -notmatch '\bpublic\s+((static|partial|sealed|abstract|readonly|ref|unsafe|file)\s+)*(class|record|interface|enum|struct|delegate)\b' -and "$genA" -match 'static class PolyglotProgram') `
        "internal libraries: the generated C# carries no `public` type (access internal, explicit and default)"
    ck ($LASTEXITCODE -eq 0 -and $build -notmatch 'CS0433|CS0122') "internal libraries: an InternalsVisibleTo consumer links both (no CS0433/CS0122)"
    $run = dotnet run --project $app --no-build 2>&1
    ck ("$run".Trim() -eq "AB42") "internal libraries: the consumer uses both libraries' internal functions and prelude types (prints AB42)"
    $r
}

# 13. P40 review (#75 pre-push, 2a/2b + missing tests): upgrading a 0.10.x consumer through the REAL package.
#  * obj/…/polyglot/ holds 0.10.x leftovers: a headerless a.cs (overwritten via build-owned) and a headerless
#    STALE old.cs whose .pg is gone — nothing may delete it, and it must no longer compile (outputs list).
#  * a headerless 0.10.x TS twin routed outside obj/ blocks the build (exit 1, naming PolyglotAdopt), and
#    `-p:PolyglotAdopt=true` takes it over once.
#  * a no-change rebuild leaves the DLL untouched (CoreCompile skipped), and the build output is UTF-8.
$upgradeFixture = {
    param($work, $pkgVer)
    $ErrorActionPreference = 'Stop'
    $PSNativeCommandUseErrorActionPreference = $false
    # P40 review r3: this runs in a THREAD job, so it must not set the process-wide [Console]::OutputEncoding.
    # The one build whose output is decoded as UTF-8 runs through its own process with an explicit encoding.
    function BuildUtf8([string]$proj, [string[]]$extra) {
        $psi = [System.Diagnostics.ProcessStartInfo]::new("dotnet")
        foreach ($a in @("build", $proj, "--nologo") + $extra) { $psi.ArgumentList.Add($a) }
        $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true; $psi.UseShellExecute = $false
        $psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8; $psi.StandardErrorEncoding = [System.Text.Encoding]::UTF8
        $pr = [System.Diagnostics.Process]::Start($psi)
        $errTask = $pr.StandardError.ReadToEndAsync()
        $out = $pr.StandardOutput.ReadToEnd()
        $pr.WaitForExit()
        [pscustomobject]@{ Rc = $pr.ExitCode; Out = $out + $errTask.Result }
    }
    $r = [System.Collections.Generic.List[object]]::new()
    function ck([bool]$c, [string]$n) { $r.Add([pscustomobject]@{ Name = $n; Passed = [bool]$c }) }

    $p = Join-Path $work "Upgrade"
    dotnet new console -o $p --framework net8.0 | Out-Null
    Copy-Item (Join-Path $work "nuget.config") (Join-Path $p "nuget.config")
    "fn alpha(): i32 {`n  return 1`n}`n" | Set-Content (Join-Path $p "a.pg")
    '{ "targets": ["csharp", "typescript"], "include": [ { "pattern": "*.pg", "target": "typescript", "output": "../upgrade-web/gen/%(Filename)" } ] }' |
        Set-Content (Join-Path $p "pgconfig.json")
    $flags = "System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.NonPublic"
    "System.Console.WriteLine(`$`"{PolyglotProgram.alpha()} {typeof(PolyglotProgram).GetMethod(`"legacy`", $flags) != null}`");" | Set-Content (Join-Path $p "Program.cs")
    dotnet add $p package MintPlayer.Polyglot.MSBuild --version $pkgVer | Out-Null
    $obj = Join-Path $p "obj/Debug/net8.0/polyglot"
    New-Item -ItemType Directory -Force $obj, (Join-Path $work "upgrade-web/gen") | Out-Null
    # 0.10.x output: no header, partial wrapper like a multi-file emit; old.cs is the stale one (its .pg is gone).
    "public static partial class PolyglotProgram { public static int alpha() { return 1; } }" | Set-Content (Join-Path $obj "a.cs")
    "public static partial class PolyglotProgram { public static int legacy() { return 9; } }" | Set-Content (Join-Path $obj "old.cs")
    "export function alpha(): number { return 1; }" | Set-Content (Join-Path $work "upgrade-web/gen/a.ts")

    $b = dotnet build $p --nologo -v q 2>&1 | Out-String
    ck ($LASTEXITCODE -ne 0 -and $b -match 'PolyglotAdopt=true' -and (Get-Content -Raw (Join-Path $work "upgrade-web/gen/a.ts")) -notmatch 'Generated by Polyglot') `
        "upgrade: a headerless 0.10.x twin outside obj/ blocks the build, untouched, and the error names -p:PolyglotAdopt=true"
    $b = dotnet build $p --nologo -v q -p:PolyglotAdopt=true 2>&1 | Out-String
    ck ($LASTEXITCODE -eq 0 -and (Get-Content -Raw (Join-Path $work "upgrade-web/gen/a.ts")) -match 'Generated by Polyglot' -and
        (Get-Content -Raw (Join-Path $obj "a.cs")) -match 'Generated by Polyglot') `
        "upgrade: PolyglotAdopt=true takes the twin over once; the headerless obj/ a.cs is overwritten (build-owned)"
    $run = "$(dotnet run --project $p --no-build 2>&1)".Trim()
    ck ((Test-Path (Join-Path $obj "old.cs")) -and $run -eq "1 False") `
        "upgrade: the stale headerless obj/ old.cs is kept (never deletable) but no longer compiled — legacy() is gone ('$run')"
    $b = dotnet build $p --nologo -v q 2>&1 | Out-String
    ck ($LASTEXITCODE -eq 0) "upgrade: the next plain build (no PolyglotAdopt) is fine — the twin now carries the header"

    $dll = Join-Path $p "bin/Debug/net8.0/Upgrade.dll"
    $t0 = (Get-Item $dll).LastWriteTimeUtc
    Start-Sleep -Milliseconds 300
    dotnet build $p --nologo -v q 2>&1 | Out-Null
    $ncRc = $LASTEXITCODE
    ck ($ncRc -eq 0 -and (Get-Item $dll).LastWriteTimeUtc -eq $t0) "no-change rebuild: exits 0 and the DLL is not rewritten (CoreCompile skipped; exit $ncRc)"

    "fn café(): i32 {`n  return`n}`n" | Set-Content -Encoding utf8 (Join-Path $p "été.pg")
    # P40 review r3: `-match 'été\.pg'` also matched MSBuild's own MSB3073 command echo, and at -v q the CLI's
    # line is not shown at all — dropping StdOutEncoding stayed green. At -v n, look for what ONLY the CLI prints
    # (its `file(line,col): error` diagnostic, which MSBuild re-reports as an error, and the em dash of its
    # message) on lines that are not MSB3073's.
    $ub = BuildUtf8 $p @("-v", "n")
    $cliLines = @($ub.Out -split "`r?`n" | Where-Object { $_ -notmatch 'MSB3073' })
    ck ($ub.Rc -ne 0 -and @($cliLines | Where-Object { $_ -cmatch 'été\.pg\(\d+,\d+\): error .*—' }).Count -gt 0) `
        "build output is UTF-8: the CLI's own 'été.pg(<line>,<col>): error ... —' diagnostic arrives unmangled (not MSBuild's command echo)"
    Remove-Item (Join-Path $p "été.pg")

    # P40 review r3: PolyglotAdopt left set. The record exists now, so the CLI ignores the flag (and says so), the
    # targets warn while it is set, and a NEW hand-written twin at an output path is never overwritten.
    "fn beta(): i32 {`n  return 2`n}`n" | Set-Content (Join-Path $p "b.pg")
    "// hand-written`nexport const mine = 1;" | Set-Content (Join-Path $work "upgrade-web/gen/b.ts")
    $b = dotnet build $p --nologo -v n -p:PolyglotAdopt=true 2>&1 | Out-String
    ck ($LASTEXITCODE -ne 0 -and $b -match 'PolyglotAdopt=true is set' -and $b -match '--adopt is ignored' -and
        (Get-Content -Raw (Join-Path $work "upgrade-web/gen/b.ts")) -match 'hand-written') `
        "upgrade: PolyglotAdopt left set warns, is ignored once the record exists, and never overwrites a new hand-written twin"
    Remove-Item (Join-Path $p "b.pg"), (Join-Path $work "upgrade-web/gen/b.ts")
    $r
}

# 14. P40 review r3 (#75): the MSBuild half of round 3, through the real package.
#  * gating 1: a project under a non-ASCII path ('é', and '日' outside CP1252) compiles what it generates — the
#    outputs list and the depfile were ANSI while MSBuild reads UTF-8 (CS0103 under 'pé', "No mapping for the
#    Unicode character" under 'p日');
#  * gating 7: flipping PolyglotEntryPoint re-runs the transpile (it was missing from the inputs cache);
#  * --build-owned unmasked: a C#-only 0.10.x project whose PLAIN build must overwrite its headerless obj/ .cs;
#  * a listed .cs deleted by hand re-runs the transpile (was CS2001 until a .pg changed);
#  * a PolyglotFile the csproj adds is an UpToDateCheckInput (the item moved from the .props to the .targets);
#  * PolyglotTargets accepts ',' ("csharp," used to reach the CLI verbatim and exit 64).
$r3Fixture = {
    param($work, $pkgVer)
    $ErrorActionPreference = 'Stop'
    $PSNativeCommandUseErrorActionPreference = $false
    $r = [System.Collections.Generic.List[object]]::new()
    function ck([bool]$c, [string]$n) { $r.Add([pscustomobject]@{ Name = $n; Passed = [bool]$c }) }

    foreach ($ch in @([string][char]0xE9, [string][char]0x65E5)) {
        $u = Join-Path $work "r3-p$ch"
        dotnet new console -o $u -n Uni --framework net8.0 | Out-Null
        Copy-Item (Join-Path $work "nuget.config") (Join-Path $u "nuget.config")
        "fn twice(x: i32): i32 {`n  return x * 2`n}`n" | Set-Content (Join-Path $u "nn.pg")
        "import { twice } from `"./nn`"`nfn compute(): i32 {`n  return twice(21)`n}`n" | Set-Content (Join-Path $u "game.pg")
        '{ "targets": ["csharp"] }' | Set-Content (Join-Path $u "pgconfig.json")
        "System.Console.WriteLine(PolyglotProgram.compute());" | Set-Content (Join-Path $u "Program.cs")
        dotnet add $u package MintPlayer.Polyglot.MSBuild --version $pkgVer | Out-Null
        $b = dotnet build $u --nologo -v q 2>&1 | Out-String
        $rc = $LASTEXITCODE
        $run = "$(dotnet run --project $u --no-build 2>&1)".Trim()
        ck ($rc -eq 0 -and $run -eq "42") "r3 gating 1: a project under 'r3-p$ch' compiles its generated code (exit $rc, prints '$run')"
        $stamp = Join-Path $u "obj/Debug/net8.0/polyglot.stamp"
        $s0 = (Get-Item -LiteralPath $stamp).LastWriteTimeUtc
        Start-Sleep -Milliseconds 300
        $b = dotnet build $u --nologo -v q 2>&1 | Out-String
        $rc2 = $LASTEXITCODE
        ck ($rc2 -eq 0 -and (Get-Item -LiteralPath $stamp).LastWriteTimeUtc -eq $s0) "r3 gating 1: ... and its no-change rebuild skips the transpile (the depfile reads back; exit $rc2)"
        "fn twice(x: i32): i32 {`n  return x * 3`n}`n" | Set-Content (Join-Path $u "nn.pg")
        dotnet build $u --nologo -v q 2>&1 | Out-Null
        $run = "$(dotnet run --project $u --no-build 2>&1)".Trim()
        ck ($run -eq "63") "r3 gating 1: ... and an edit under 'r3-p$ch' is picked up (prints '$run')"
    }

    # Gating 7: PolyglotEntryPoint. An app with top-level statements AND a .pg `fn main`: the generated Main
    # collides (CS0017/CS7022-as-error) until the app passes PolyglotEntryPoint=false — which must re-transpile.
    $e = Join-Path $work "R3Entry"
    dotnet new console -o $e --framework net8.0 | Out-Null
    Copy-Item (Join-Path $work "nuget.config") (Join-Path $e "nuget.config")
    "fn main() {`n  print(`"pg main`")`n}`n" | Set-Content (Join-Path $e "app.pg")
    '{ "targets": ["csharp"], "lib": ["io"] }' | Set-Content (Join-Path $e "pgconfig.json")
    "PolyglotProgram.main();" | Set-Content (Join-Path $e "Program.cs")
    $ep = Join-Path $e "R3Entry.csproj"
    (Get-Content -Raw $ep).Replace("</Project>", "  <PropertyGroup><TreatWarningsAsErrors>true</TreatWarningsAsErrors></PropertyGroup>`n</Project>") | Set-Content $ep
    dotnet add $e package MintPlayer.Polyglot.MSBuild --version $pkgVer | Out-Null
    $b1 = dotnet build $e --nologo -v q 2>&1 | Out-String
    $rc1 = $LASTEXITCODE
    $b2 = dotnet build $e --nologo -v q -p:PolyglotEntryPoint=false 2>&1 | Out-String
    $rc2 = $LASTEXITCODE
    $gen = Get-Content -Raw (Join-Path $e "obj/Debug/net8.0/polyglot/app.cs") -ErrorAction SilentlyContinue
    $run = "$(dotnet run --project $e --no-build 2>&1)".Trim()
    ck ($rc1 -ne 0 -and $b1 -match 'CS7022|CS0017' -and $rc2 -eq 0 -and "$gen" -cnotmatch 'static void Main\(' -and $run -eq "pg main") `
        "r3 gating 7: -p:PolyglotEntryPoint=false re-transpiles without Main (exit $rc1 [$(([regex]::Matches($b1, '(CS|MSB|NETSDK|NU)\d+') | ForEach-Object Value | Select-Object -Unique) -join ',')] -> $rc2, prints '$run')"

    # --build-owned through the package, unmasked: C#-only, 0.10.x headerless obj/ a.cs, PLAIN build (no adopt).
    $o = Join-Path $work "R3Owned"
    dotnet new console -o $o --framework net8.0 | Out-Null
    Copy-Item (Join-Path $work "nuget.config") (Join-Path $o "nuget.config")
    "fn alpha(): i32 {`n  return 5`n}`n" | Set-Content (Join-Path $o "a.pg")
    '{ "targets": ["csharp"] }' | Set-Content (Join-Path $o "pgconfig.json")
    "System.Console.WriteLine(PolyglotProgram.alpha());" | Set-Content (Join-Path $o "Program.cs")
    dotnet add $o package MintPlayer.Polyglot.MSBuild --version $pkgVer | Out-Null
    $oobj = Join-Path $o "obj/Debug/net8.0/polyglot"
    New-Item -ItemType Directory -Force $oobj | Out-Null
    "static class PolyglotProgram { public static int alpha() { return 1; } }" | Set-Content (Join-Path $oobj "a.cs")
    $b = dotnet build $o --nologo -v q 2>&1 | Out-String
    $rc = $LASTEXITCODE
    $run = "$(dotnet run --project $o --no-build 2>&1)".Trim()
    ck ($rc -eq 0 -and (Get-Content -Raw (Join-Path $oobj "a.cs")) -match 'Generated by Polyglot' -and $run -eq "5") `
        "r3: a C#-only 0.10.x project's PLAIN build overwrites its headerless obj/ a.cs (build-owned; exit $rc, prints '$run')"

    # A listed .cs deleted by hand re-runs the transpile.
    Remove-Item (Join-Path $oobj "a.cs")
    $b = dotnet build $o --nologo -v q 2>&1 | Out-String
    ck ($LASTEXITCODE -eq 0 -and $b -notmatch 'CS2001' -and (Test-Path (Join-Path $oobj "a.cs"))) "r3: a generated .cs deleted by hand is regenerated by the next build (no CS2001)"

    # A PolyglotFile the csproj adds (outside the project folder) is an UpToDateCheckInput; PolyglotTargets takes ','.
    $shared = Join-Path $work "r3-shared"
    New-Item -ItemType Directory -Force $shared | Out-Null
    "fn shared(): i32 {`n  return 7`n}`n" | Set-Content (Join-Path $shared "s.pg")
    $op = Join-Path $o "R3Owned.csproj"
    $plain = Get-Content -Raw $op
    # Evaluation only (-getItem): the item lists are what Visual Studio's fast up-to-date check reads.
    $plain.Replace("</Project>", "  <ItemGroup><PolyglotFile Include=`"..\r3-shared\s.pg`" /></ItemGroup>`n</Project>") | Set-Content $op
    $items = dotnet msbuild $o -getItem:UpToDateCheckInput -nologo 2>&1 | Out-String
    ck ($items -match 's\.pg' -and $items -match 'a\.pg') "r3: a PolyglotFile added in the csproj is an UpToDateCheckInput (moved to the .targets)"
    $plain.Replace("</Project>", "  <PropertyGroup><PolyglotTargets>csharp,</PolyglotTargets></PropertyGroup>`n</Project>") | Set-Content $op
    "fn alpha(): i32 {`n  return 6`n}`n" | Set-Content (Join-Path $o "a.pg")
    $b = dotnet build $o --nologo -v q 2>&1 | Out-String
    $tRc = $LASTEXITCODE
    $run = "$(dotnet run --project $o --no-build 2>&1)".Trim()
    ck ($tRc -eq 0 -and $run -eq "6") "r3: PolyglotTargets 'csharp,' builds the C# (',' is a separator; exit $tRc, prints '$run')"

    # review r8 (gating, Spark pgb51_c\rootcase R2): a configuration typed in another case. `dotnet build` makes
    # obj/Debug/; `-c debug` then passes --out/--build-owned as obj\debug\<tfm>\polyglot\, a root spelled in another
    # case than the disk. r7 compared that root's components and refused obj/Debug (it holds the SDK's own
    # AssemblyAttributes.cs): MSB3073. All three builds must pass, and obj/ keeps the one spelling MSBuild made.
    $cc = Join-Path $work "r8-cfgcase"
    dotnet new console -o $cc -n CfgCase --framework net8.0 | Out-Null
    Copy-Item (Join-Path $work "nuget.config") (Join-Path $cc "nuget.config")
    "fn alpha(): i32 {`n  return 1`n}`n" | Set-Content (Join-Path $cc "a.pg")
    '{ "targets": ["csharp"] }' | Set-Content (Join-Path $cc "pgconfig.json")
    "System.Console.WriteLine(PolyglotProgram.alpha());" | Set-Content (Join-Path $cc "Program.cs")
    dotnet add $cc package MintPlayer.Polyglot.MSBuild --version $pkgVer | Out-Null
    # review r9 (part 2): -v q cannot show the transpiler ran, so each build gets a NEW alpha() body and must have
    # rewritten obj\<cfg as typed>\net8.0\polyglot\a.cs with it (an mtime from this build, the new `return N;`), and
    # the app must print N. A skipped Polyglot target leaves the previous body (and mtime) behind: the pin fails.
    $n = 0
    foreach ($cfgArgs in @(@(), @("-c", "debug"), @("-c", "Debug"))) {
        $n++
        "fn alpha(): i32 {`n  return $n`n}`n" | Set-Content (Join-Path $cc "a.pg")
        $cfgDir = if ($cfgArgs.Count) { $cfgArgs[1] } else { "Debug" }
        $emitted = Join-Path $cc "obj/$cfgDir/net8.0/polyglot/a.cs"
        $before = [DateTime]::UtcNow.AddSeconds(-2) # NTFS/FAT timestamp slack
        $b = dotnet build $cc --nologo -v q @cfgArgs 2>&1 | Out-String
        $bRc = $LASTEXITCODE
        $objNames = @(Get-ChildItem (Join-Path $cc "obj") -Directory -Name) -join ','
        $label = if ($cfgArgs.Count) { $cfgArgs -join ' ' } else { "(default)" }
        $wrote = (Test-Path -LiteralPath $emitted) -and (Get-Item -LiteralPath $emitted).LastWriteTimeUtc -ge $before -and
                 ((Get-Content -Raw -LiteralPath $emitted) -match "return $n;")
        $ran = "$(dotnet run --project $cc --no-build @cfgArgs 2>&1)".Trim()
        ck ($bRc -eq 0 -and $b -notmatch 'differs from' -and $objNames -ceq 'Debug' -and $wrote -and $ran -eq "$n") `
            "review r8 (gating) + r9: dotnet build $label after a plain build transpiles (wrote $emitted with 'return $n;': $wrote; app prints '$ran'), passes and renames nothing (exit $bRc; obj: $objNames)"
    }
    $r
}

$packResults = [System.Collections.Generic.List[object]]::new()
$appResults  = [System.Collections.Generic.List[object]]::new()
# 9. P40 B1 (issue #73): two class libraries, each with one .pg and public output, referenced by one app.
# Before target options both emitted a global `PolyglotProgram` + `Option<T>`/`Some<T>`, so the app failed
# with CS0433 (the same type in two assemblies). LibA sets its namespace through pgconfig
# `targetOptions.csharp`; LibB through the MSBuild properties (PolyglotNamespace / PolyglotClass /
# PolyglotAccess), so both configuration paths are exercised.
$twoLibsFixture = {
    param($work, $pkgVer)
    $ErrorActionPreference = 'Stop'
    $PSNativeCommandUseErrorActionPreference = $false
    $r = [System.Collections.Generic.List[object]]::new()
    function ck([bool]$c, [string]$n) { $r.Add([pscustomobject]@{ Name = $n; Passed = [bool]$c }) }

    $root = Join-Path $work "TwoLibs"
    $libA = Join-Path $root "LibA"; $libB = Join-Path $root "LibB"; $twoApp = Join-Path $root "App"
    dotnet new classlib -o $libA --framework net8.0 | Out-Null
    dotnet new classlib -o $libB --framework net8.0 | Out-Null
    dotnet new console -o $twoApp --framework net8.0 | Out-Null
    Copy-Item (Join-Path $work "nuget.config") (Join-Path $root "nuget.config")
    Remove-Item (Join-Path $libA "Class1.cs"), (Join-Path $libB "Class1.cs") -ErrorAction SilentlyContinue

    "fn find(): Option<i32> => Some(20)`nfn name(): string => `"A`"`n" | Set-Content (Join-Path $libA "a.pg")
    '{ "targets": ["csharp"], "targetOptions": { "csharp": { "namespace": "LibA", "access": "public" } } }' |
        Set-Content (Join-Path $libA "pgconfig.json")
    "fn find(): Option<i32> => Some(22)`nfn name(): string => `"B`"`n" | Set-Content (Join-Path $libB "b.pg")
    '{ "targets": ["csharp"] }' | Set-Content (Join-Path $libB "pgconfig.json")
    $csprojB = Join-Path $libB "LibB.csproj"
    (Get-Content $csprojB -Raw).Replace("</Project>",
        "  <PropertyGroup><PolyglotNamespace>LibB</PolyglotNamespace><PolyglotClass>BApi</PolyglotClass><PolyglotAccess>public</PolyglotAccess></PropertyGroup>`n</Project>") |
        Set-Content $csprojB
    dotnet add $libA package MintPlayer.Polyglot.MSBuild --version $pkgVer | Out-Null
    dotnet add $libB package MintPlayer.Polyglot.MSBuild --version $pkgVer | Out-Null
    dotnet add $twoApp reference (Join-Path $libA "LibA.csproj") (Join-Path $libB "LibB.csproj") | Out-Null
@"
var a = LibA.PolyglotProgram.find();
var b = LibB.BApi.find();
int va = a is LibA.Some<int> sa ? sa.value : 0;
int vb = b is LibB.Some<int> sb ? sb.value : 0;
System.Console.WriteLine(LibA.PolyglotProgram.name() + LibB.BApi.name() + (va + vb));
"@ | Set-Content (Join-Path $twoApp "Program.cs")
    $twoBuild = dotnet build $twoApp --nologo -v q 2>&1 | Out-String
    ck ($LASTEXITCODE -eq 0 -and $twoBuild -notmatch 'CS0433') "two .pg class libraries in distinct namespaces link into one app (no CS0433)"
    $genA = Get-Content -Raw (Join-Path $libA "obj/Debug/net8.0/polyglot/a.cs") -ErrorAction SilentlyContinue
    $genB = Get-Content -Raw (Join-Path $libB "obj/Debug/net8.0/polyglot/b.cs") -ErrorAction SilentlyContinue
    ck ("$genA" -match '(?m)^namespace LibA;' -and "$genB" -match '(?m)^namespace LibB;' -and "$genB" -match 'public static class BApi') `
        "pgconfig targetOptions and the PolyglotNamespace/PolyglotClass/PolyglotAccess properties both reach the emitter"
    $twoRun = dotnet run --project $twoApp --no-build 2>&1
    ck ("$twoRun".Trim() -eq "AB42") "the app uses both libraries' functions and prelude types (prints AB42)"
    $r
}

$downResults = [System.Collections.Generic.List[object]]::new()

if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Force $feed | Out-Null
# Same version every run: evict any cached extraction or restore serves a stale package.
$cached = if ($Work) { Join-Path $env:NUGET_PACKAGES "mintplayer.polyglot.msbuild" } else { Join-Path ([Environment]::GetFolderPath('UserProfile')) ".nuget/packages/mintplayer.polyglot.msbuild" }
if (Test-Path $cached) { Remove-Item -Recurse -Force $cached }

# 0. Pack from the working tree into a private feed. The version is read from the csproj so a bump
# can never desync this gate from what actually packs.
$pkgVer = ([xml](Get-Content (Join-Path $repo "src/MintPlayer.Polyglot.MSBuild/MintPlayer.Polyglot.MSBuild.csproj"))).Project.PropertyGroup.Version | Where-Object { $_ } | Select-Object -First 1
$packArgs = @()
if ($Cli) {
    # Stage the given CLI as tools/<portable host RID>/ — the same shape the .targets resolves at build time.
    $cliPath = (Resolve-Path $Cli).Path
    $os = if ($IsWindows) { 'win' } elseif ($IsMacOS) { 'osx' } else { 'linux' }
    $arch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLowerInvariant()
    $ridDir = Join-Path $work "stage" "$os-$arch"
    New-Item -ItemType Directory -Force $ridDir | Out-Null
    Copy-Item $cliPath (Join-Path $ridDir ($(if ($IsWindows) { 'polyglot.exe' } else { 'polyglot' })))
    Copy-Item -Recurse (Join-Path (Split-Path $cliPath -Parent) "plugins") (Join-Path $ridDir "plugins")
    $packArgs = @("-p:PolyglotStageRoot=$(Join-Path $work 'stage')")
} else {
    # The CLI the package ships (the local pack takes x64\Debug's — PolyglotCliDir), for the fixtures that also run
    # the CLI by hand the way a consumer's CI does (fixture 11).
    $cliPath = (Resolve-Path (Join-Path $repo "x64/Debug/MintPlayer.Polyglot.Cli.exe")).Path
}
dotnet pack (Join-Path $repo "src/MintPlayer.Polyglot.MSBuild") -c Release -o $feed --nologo -v q @packArgs | Out-Null
fck $packResults (Test-Path (Join-Path $feed "MintPlayer.Polyglot.MSBuild.$pkgVer.nupkg")) "package packs from the working tree ($pkgVer)"

# Consumer solution: App (uses the package + a .pg) and Downstream (references App, has its own .pg,
# must NOT transpile it).
$app = Join-Path $work "App"
$down = Join-Path $work "Downstream"
dotnet new console -o $app --framework net8.0 | Out-Null
dotnet new console -o $down --framework net8.0 | Out-Null

@"
<?xml version="1.0" encoding="utf-8"?>
<configuration>
  <packageSources>
    <clear />
    <add key="local" value="$feed" />
    <add key="nuget.org" value="https://api.nuget.org/v3/index.json" />
  </packageSources>
</configuration>
"@ | Set-Content (Join-Path $work "nuget.config")

@"
record Point(x: i32, y: i32)
fn makeOrigin(): Point {
  return Point(0, 0)
}
"@ | Set-Content (Join-Path $app "shapes.pg")
# P30: the plugin/target set is config-sourced — the consumer contract is a minimal pgconfig.json.
'{ "targets": ["csharp"] }' | Set-Content (Join-Path $app "pgconfig.json")

# Top-level statements deliberately: the generated wrapper is `static class PolyglotProgram`
# precisely so it can coexist with the implicit `Program` class every modern console app has
# (the collision was P11's sharpest edge until the wrapper rename).
@"
var p = PolyglotProgram.makeOrigin();
var q = new Point(3, 4);
System.Console.WriteLine(p.x + q.x + q.y);
"@ | Set-Content (Join-Path $app "Program.cs")

dotnet add $app package MintPlayer.Polyglot.MSBuild --version $pkgVer | Out-Null

# 1. dotnet build alone compiles the .pg types into the assembly. This first build restores + installs
# the freshly-packed package into the global NuGet cache — the one real cross-fixture race — so it is
# the designated warm before any parallel fixture starts.
dotnet build $app --nologo -v q 2>&1 | Out-Null
$gen = Join-Path $app "obj/Debug/net8.0/polyglot/shapes.cs"
fck $appResults (Test-Path $gen) "build generates obj/.../polyglot/shapes.cs"
$run = dotnet run --project $app --no-build 2>&1
fck $appResults ("$run".Trim() -eq "7") "the .pg record + fn are usable from C# (runs, prints 7)"

# Warm complete: the nupkg is now in the global cache. Launch the four independent one-shots to overlap
# the rest of the strictly-serial App/Downstream chain below. Recreate each block via
# [scriptblock]::Create so it is unaffiliated with this runspace; all args are absolute paths.
if (-not $Sequential) {
    $jobs = @($sharedFixture, $multiFixture, $twinFixture, $noCfgFixture, $twoLibsFixture, $staleFixture, $sparkLayoutFixture, $internalLibsFixture, $upgradeFixture, $r3Fixture) | ForEach-Object {
        Start-ThreadJob -ScriptBlock ([scriptblock]::Create($_.ToString())) -ArgumentList $work, $pkgVer, $cliPath
    }
}

# 2. A second build with no .pg change skips the transpile (generated file untouched).
$stamp = (Get-Item $gen).LastWriteTimeUtc
Start-Sleep -Milliseconds 300
dotnet build $app --nologo -v q 2>&1 | Out-Null
fck $appResults ((Get-Item $gen).LastWriteTimeUtc -eq $stamp) "incremental build skips transpilation"

# ...and touching the .pg re-runs the transpile. P40: the old RemoveDir is gone and every write is
# write-if-changed, so an identical .cs keeps its mtime (CoreCompile can skip); the transpile's own stamp
# is what proves it ran.
$appStamp = Join-Path $app "obj/Debug/net8.0/polyglot.stamp"
$tStamp = (Get-Item $appStamp).LastWriteTimeUtc
Start-Sleep -Milliseconds 300
(Get-Item (Join-Path $app "shapes.pg")).LastWriteTime = Get-Date
dotnet build $app --nologo -v q 2>&1 | Out-Null
fck $appResults ((Get-Item $appStamp).LastWriteTimeUtc -ne $tStamp) "touching the .pg re-transpiles"
fck $appResults ((Get-Item $gen).LastWriteTimeUtc -eq $stamp) "an identical re-transpile leaves the .cs untouched (write-if-changed)"

# 2a. P38 / issue #69: flipping PolyglotOriginInfo must re-transpile even though no .pg and no tool path
# changed. P40: the option is a line of the inputs cache ($(_PolyglotInputsCache), an Input rewritten only
# when different), which replaced the P38 stamp-name tag. Without that, a consumer sets the property,
# rebuilds, gets directive-free .cs left over in obj/, and concludes the feature is broken — and a package
# upgrade masks it on the first build, so it surfaces later as a toggle-off-then-on mystery.
$originStamp = (Get-Item $gen).LastWriteTimeUtc
Start-Sleep -Milliseconds 300
dotnet build $app --nologo -v q -p:PolyglotOriginInfo=true 2>&1 | Out-Null
$genText = Get-Content -LiteralPath $gen -Raw
fck $appResults ((Get-Item $gen).LastWriteTimeUtc -ne $originStamp) "flipping PolyglotOriginInfo re-transpiles (the inputs cache carries the option)"
fck $appResults ($genText -match '(?m)^#line ') "PolyglotOriginInfo=true actually emits #line directives"
# ...and flipping it back restores directive-free output rather than leaving the annotated file behind.
dotnet build $app --nologo -v q 2>&1 | Out-Null
fck $appResults ((Get-Content -LiteralPath $gen -Raw) -notmatch '(?m)^#line ') "turning PolyglotOriginInfo back off restores undecorated output"

# 2b. dotnet-watch integration (P21 slice 5): the package declares the .pg sources as `Watch` items —
# the item group dotnet watch honors beyond its Compile/EmbeddedResource defaults — so `dotnet watch
# build|run` on a consuming project re-transpiles on every .pg edit. -getItem evaluates the project the
# same way the watcher's GenerateWatchList does.
$watchItems = dotnet msbuild $app -getItem:Watch -nologo 2>&1 | Out-String
fck $appResults ($watchItems -match 'shapes\.pg') "the .pg sources are declared as dotnet-watch Watch items"

# 3. dotnet clean removes the generated file (FileWrites).
dotnet clean $app --nologo -v q 2>&1 | Out-Null
fck $appResults (-not (Test-Path $gen)) "dotnet clean removes the generated .cs"

# 4. Non-transitive: Downstream references App and carries its own .pg — which must be IGNORED (no
# inherited transpile, no package dependency), and the reference must not break its build. This
# rebuilds App (via the project reference), so it stays on the serial chain — never concurrent with
# the App steps above.
dotnet add $down reference (Join-Path $app "App.csproj") | Out-Null
"fn ignored(): i32 {`n  return 1`n}" | Set-Content (Join-Path $down "extra.pg")
"System.Console.WriteLine(42);" | Set-Content (Join-Path $down "Program.cs")
dotnet build $down --nologo -v q 2>&1 | Out-Null
fck $downResults (-not (Test-Path (Join-Path $down "obj/Debug/net8.0/polyglot"))) "a referencing project does not inherit the transpile"
$downRun = dotnet run --project $down --no-build 2>&1
fck $downResults ("$downRun".Trim() -eq "42") "downstream builds and runs untouched (prints 42)"

# ---- Independent fixtures (5-8): collect the parallel jobs, or run inline under -Sequential (which
#      reproduces the original serial order exactly). ----
if ($Sequential) {
    $sharedRes = @(& $sharedFixture $work $pkgVer)
    $multiRes  = @(& $multiFixture  $work $pkgVer)
    $twinRes   = @(& $twinFixture   $work $pkgVer)
    $noCfgRes  = @(& $noCfgFixture  $work $pkgVer)
    $twoLibsRes = @(& $twoLibsFixture $work $pkgVer)
    $staleRes  = @(& $staleFixture  $work $pkgVer)
    $sparkRes  = @(& $sparkLayoutFixture $work $pkgVer $cliPath)
    $internalRes = @(& $internalLibsFixture $work $pkgVer)
    $upgradeRes = @(& $upgradeFixture $work $pkgVer)
    $r3Res = @(& $r3Fixture $work $pkgVer)
} else {
    $sharedRes = @(Receive-Job -Job $jobs[0] -Wait -AutoRemoveJob)
    $multiRes  = @(Receive-Job -Job $jobs[1] -Wait -AutoRemoveJob)
    $twinRes   = @(Receive-Job -Job $jobs[2] -Wait -AutoRemoveJob)
    $noCfgRes  = @(Receive-Job -Job $jobs[3] -Wait -AutoRemoveJob)
    $twoLibsRes = @(Receive-Job -Job $jobs[4] -Wait -AutoRemoveJob)
    $staleRes  = @(Receive-Job -Job $jobs[5] -Wait -AutoRemoveJob)
    $sparkRes  = @(Receive-Job -Job $jobs[6] -Wait -AutoRemoveJob)
    $internalRes = @(Receive-Job -Job $jobs[7] -Wait -AutoRemoveJob)
    $upgradeRes = @(Receive-Job -Job $jobs[8] -Wait -AutoRemoveJob)
    $r3Res = @(Receive-Job -Job $jobs[9] -Wait -AutoRemoveJob)
}

# ---- Deterministic output: fixed fixture order (identical to the serial run), never interleaved. ----
$failures = 0
foreach ($section in @($packResults, $appResults, $downResults, $sharedRes, $multiRes, $twinRes, $noCfgRes, $twoLibsRes, $staleRes, $sparkRes, $internalRes, $upgradeRes, $r3Res)) {
    foreach ($res in $section) {
        if ($res.Passed) { Write-Host "[PASS] $($res.Name)" }
        else { Write-Host "[FAIL] $($res.Name)" -ForegroundColor Red; $failures++ }
    }
}

if ($failures -eq 0) { Write-Host "`nP11 gate: all checks passed."; exit 0 }
Write-Host "`nP11 gate: $failures check(s) FAILED." -ForegroundColor Red
exit 1
