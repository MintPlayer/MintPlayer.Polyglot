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
# no cross-dependency): Shared (5), MultiPg (6), Twin (7), NoCfg (8). By default the App first build
# (the one real cross-fixture race: first-install of the freshly-packed nupkg into the global NuGet
# cache) warms the cache, then the four one-shots run as ThreadJobs OVERLAPPING the rest of the serial
# chain. Concurrent restores of an already-cached package are safe; per-fixture dirs never collide.
# Output is buffered and printed in a fixed fixture order so parallel runs never interleave and the
# exit code / every assertion is identical to serial. `-Sequential` restores the pure serial run.
param([switch]$Sequential)

$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$work = Join-Path ([IO.Path]::GetTempPath()) "polyglot-nuget-gate"
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
  init(v: i32) { this.v = v }
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
    "fn step(): i32 {`n  return 1`n}`n" | Set-Content (Join-Path $multi "phys.pg")
    "fn tick(): i32 {`n  return 2`n}`n" | Set-Content (Join-Path $multi "snake.pg")
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

$packResults = [System.Collections.Generic.List[object]]::new()
$appResults  = [System.Collections.Generic.List[object]]::new()
$downResults = [System.Collections.Generic.List[object]]::new()

if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Force $feed | Out-Null
# Same version every run: evict any cached extraction or restore serves a stale package.
$cached = Join-Path $env:USERPROFILE ".nuget/packages/mintplayer.polyglot.msbuild"
if (Test-Path $cached) { Remove-Item -Recurse -Force $cached }

# 0. Pack from the working tree into a private feed. The version is read from the csproj so a bump
# can never desync this gate from what actually packs.
$pkgVer = ([xml](Get-Content (Join-Path $repo "src/MintPlayer.Polyglot.MSBuild/MintPlayer.Polyglot.MSBuild.csproj"))).Project.PropertyGroup.Version | Where-Object { $_ } | Select-Object -First 1
dotnet pack (Join-Path $repo "src/MintPlayer.Polyglot.MSBuild") -c Release -o $feed --nologo -v q | Out-Null
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
    $jobs = @($sharedFixture, $multiFixture, $twinFixture, $noCfgFixture) | ForEach-Object {
        Start-ThreadJob -ScriptBlock ([scriptblock]::Create($_.ToString())) -ArgumentList $work, $pkgVer
    }
}

# 2. A second build with no .pg change skips the transpile (generated file untouched).
$stamp = (Get-Item $gen).LastWriteTimeUtc
Start-Sleep -Milliseconds 300
dotnet build $app --nologo -v q 2>&1 | Out-Null
fck $appResults ((Get-Item $gen).LastWriteTimeUtc -eq $stamp) "incremental build skips transpilation"

# ...and touching the .pg regenerates.
(Get-Item (Join-Path $app "shapes.pg")).LastWriteTime = Get-Date
dotnet build $app --nologo -v q 2>&1 | Out-Null
fck $appResults ((Get-Item $gen).LastWriteTimeUtc -ne $stamp) "touching the .pg re-transpiles"

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
} else {
    $sharedRes = @(Receive-Job -Job $jobs[0] -Wait -AutoRemoveJob)
    $multiRes  = @(Receive-Job -Job $jobs[1] -Wait -AutoRemoveJob)
    $twinRes   = @(Receive-Job -Job $jobs[2] -Wait -AutoRemoveJob)
    $noCfgRes  = @(Receive-Job -Job $jobs[3] -Wait -AutoRemoveJob)
}

# ---- Deterministic output: fixed fixture order (identical to the serial run), never interleaved. ----
$failures = 0
foreach ($section in @($packResults, $appResults, $downResults, $sharedRes, $multiRes, $twinRes, $noCfgRes)) {
    foreach ($res in $section) {
        if ($res.Passed) { Write-Host "[PASS] $($res.Name)" }
        else { Write-Host "[FAIL] $($res.Name)" -ForegroundColor Red; $failures++ }
    }
}

if ($failures -eq 0) { Write-Host "`nP11 gate: all checks passed."; exit 0 }
Write-Host "`nP11 gate: $failures check(s) FAILED." -ForegroundColor Red
exit 1
