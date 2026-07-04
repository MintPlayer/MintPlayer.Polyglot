# P11 gate (PLAN §P11): a fresh C# project + MintPlayer.Polyglot.MSBuild + a .pg file builds with
# `dotnet build` alone; a second build skips transpilation; `dotnet clean` removes the generated files;
# and a project referencing the first does NOT inherit the transpile behavior or the package dependency.
# Packs the package from the working tree first (Debug CLI), so this validates current sources.
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$work = Join-Path ([IO.Path]::GetTempPath()) "polyglot-nuget-gate"
$feed = Join-Path $work "feed"
$failures = 0
function Check([bool]$cond, [string]$name) {
    if ($cond) { Write-Host "[PASS] $name" } else { Write-Host "[FAIL] $name" -ForegroundColor Red; $script:failures++ }
}

if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Force $feed | Out-Null
# Same version every run: evict any cached extraction or restore serves a stale package.
$cached = Join-Path $env:USERPROFILE ".nuget/packages/mintplayer.polyglot.msbuild"
if (Test-Path $cached) { Remove-Item -Recurse -Force $cached }

# 0. Pack from the working tree into a private feed.
dotnet pack (Join-Path $repo "src/MintPlayer.Polyglot.MSBuild") -c Release -o $feed --nologo -v q | Out-Null
Check (Test-Path (Join-Path $feed "MintPlayer.Polyglot.MSBuild.0.0.1.nupkg")) "package packs from the working tree"

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

# Top-level statements deliberately: the generated wrapper is `static class PolyglotProgram`
# precisely so it can coexist with the implicit `Program` class every modern console app has
# (the collision was P11's sharpest edge until the wrapper rename).
@"
var p = PolyglotProgram.makeOrigin();
var q = new Point(3, 4);
System.Console.WriteLine(p.x + q.x + q.y);
"@ | Set-Content (Join-Path $app "Program.cs")

dotnet add $app package MintPlayer.Polyglot.MSBuild --version 0.0.1 | Out-Null

# 1. dotnet build alone compiles the .pg types into the assembly.
dotnet build $app --nologo -v q 2>&1 | Out-Null
$gen = Join-Path $app "obj/Debug/net8.0/polyglot/shapes.cs"
Check (Test-Path $gen) "build generates obj/.../polyglot/shapes.cs"
$run = dotnet run --project $app --no-build 2>&1
Check ("$run".Trim() -eq "7") "the .pg record + fn are usable from C# (runs, prints 7)"

# 2. A second build with no .pg change skips the transpile (generated file untouched).
$stamp = (Get-Item $gen).LastWriteTimeUtc
Start-Sleep -Milliseconds 300
dotnet build $app --nologo -v q 2>&1 | Out-Null
Check ((Get-Item $gen).LastWriteTimeUtc -eq $stamp) "incremental build skips transpilation"

# ...and touching the .pg regenerates.
(Get-Item (Join-Path $app "shapes.pg")).LastWriteTime = Get-Date
dotnet build $app --nologo -v q 2>&1 | Out-Null
Check ((Get-Item $gen).LastWriteTimeUtc -ne $stamp) "touching the .pg re-transpiles"

# 3. dotnet clean removes the generated file (FileWrites).
dotnet clean $app --nologo -v q 2>&1 | Out-Null
Check (-not (Test-Path $gen)) "dotnet clean removes the generated .cs"

# 4. Non-transitive: Downstream references App and carries its own .pg — which must be IGNORED (no
# inherited transpile, no package dependency), and the reference must not break its build.
dotnet add $down reference (Join-Path $app "App.csproj") | Out-Null
"fn ignored(): i32 {`n  return 1`n}" | Set-Content (Join-Path $down "extra.pg")
"System.Console.WriteLine(42);" | Set-Content (Join-Path $down "Program.cs")
dotnet build $down --nologo -v q 2>&1 | Out-Null
Check (-not (Test-Path (Join-Path $down "obj/Debug/net8.0/polyglot"))) "a referencing project does not inherit the transpile"
$downRun = dotnet run --project $down --no-build 2>&1
Check ("$downRun".Trim() -eq "42") "downstream builds and runs untouched (prints 42)"

if ($failures -eq 0) { Write-Host "`nP11 gate: all checks passed."; exit 0 }
Write-Host "`nP11 gate: $failures check(s) FAILED." -ForegroundColor Red
exit 1
