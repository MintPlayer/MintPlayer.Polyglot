#!/usr/bin/env pwsh
# P30 auto-download gate (PLAN §P30 slice 5): the end-to-end proof that `polyglot build` sources
# its plugins from pgconfig.json — downloading from an (herein fake) npm registry, pinning
# pgconfig.lock.json, rebuilding fully OFFLINE from lock+cache, and refusing tampered artifacts —
# with no npm and no system tar anywhere on the path.
#
#   1. empty cache + pgconfig dependency  -> build downloads, locks, emits (byte-equal to the
#      in-box python emission of the same program: the P24 lockstep guarantee, fixture-sized)
#   2. registry killed                    -> a fresh process rebuilds OFFLINE (lock-first cache)
#   3. cached manifest tampered           -> offline build refuses it, names the tamper
#   4. registry back up                   -> the build heals (re-fetches the exact pinned artifact)
#   5. a packument lying about integrity  -> refused, nothing cached, nothing registered
#   6. --target override                  -> wins over the pgconfig target set
#   7. no config + no --target            -> refused with the loaded-target list
param(
    [string]$Cli = (Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) "x64\Debug\MintPlayer.Polyglot.Cli.exe")
)
$ErrorActionPreference = "Stop"
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not (Test-Path $Cli)) { Write-Host "polyglot CLI not found at $Cli — build the solution first."; exit 2 }
$node = Get-Command node -ErrorAction SilentlyContinue
if (-not $node) { Write-Host "[SKIP] node not found — the registry gate needs Node."; exit 0 }

$bad = 0
function Assert([bool]$cond, [string]$name) {
    if ($cond) { Write-Host "[PASS] $name" } else { Write-Host "[FAIL] $name"; $script:bad++ }
}

# --- workspace -----------------------------------------------------------------------------------
$work = Join-Path ([System.IO.Path]::GetTempPath()) "polyglot-registry-gate"
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
$proj = Join-Path $work "proj"
$cache = Join-Path $work "cache"
New-Item -ItemType Directory -Force $proj, $cache | Out-Null
Set-Content (Join-Path $proj "main.pg") "import { print } from `"std.io`"`nfn main() {`n  print(41 + 1)`n}`n" -NoNewline
Set-Content (Join-Path $proj "pgconfig.json") "{ `"targets`": [`"pyfixture`"], `"dependencies`": { `"pyfixture`": `"^1.2.0`" } }" -NoNewline
$env:POLYGLOT_CACHE = $cache

# --- the fake registry ---------------------------------------------------------------------------
$serverLog = Join-Path $work "server.log"
function Start-Registry {
    $p = Start-Process -FilePath "node" `
        -ArgumentList (Join-Path $PSScriptRoot "registry-server.js"), (Join-Path $repo "plugins/python/polyglot-plugin.json") `
        -RedirectStandardOutput $serverLog -RedirectStandardError (Join-Path $work "server.err") `
        -NoNewWindow -PassThru
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline) {
        $ready = @(@(Get-Content $serverLog -ErrorAction SilentlyContinue) -match '^READY (\d+)$')
        if ($ready.Count -gt 0) { return @{ Proc = $p; Port = [int]($ready[0] -replace 'READY ', '') } }
        Start-Sleep -Milliseconds 100
    }
    throw "registry server did not come up"
}
$reg = Start-Registry
$env:POLYGLOT_REGISTRY = "http://127.0.0.1:$($reg.Port)"

try {
    # --- 1. cold-cache build: download -> verify -> cache -> lock -> emit --------------------------
    $out1 = & $Cli build (Join-Path $proj "main.pg") --out $proj 2>&1 | Out-String
    Assert ($LASTEXITCODE -eq 0) "cold-cache build succeeds ($($out1.Trim() -replace '\s+',' '))"
    Assert (Test-Path (Join-Path $proj "main.py")) "the downloaded target emitted its output (.py from the manifest)"
    $lockPath = Join-Path $proj "pgconfig.lock.json"
    Assert (Test-Path $lockPath) "pgconfig.lock.json was written"
    $lock = Get-Content $lockPath -Raw | ConvertFrom-Json
    $pin = $lock.packages.'@mintplayer/polyglot-target-pyfixture'
    Assert ($lock.lockfileVersion -eq 1 -and $pin.version -eq '1.2.3' -and $pin.integrity -like 'sha512-*') `
        "the lock pins version 1.2.3 + SRI sha512 integrity"
    $entryDir = Join-Path $cache "@mintplayer/polyglot-target-pyfixture/1.2.3"
    Assert ((Test-Path (Join-Path $entryDir "polyglot-plugin.json")) -and (Test-Path (Join-Path $entryDir "meta.json"))) `
        "the versioned cache entry (<name>/<version>/manifest+meta) exists"

    # Lockstep byte-equality: the fixture is the real python plugin renamed, so the emission must
    # equal --target python's for the same program.
    $refDir = Join-Path $work "ref"
    New-Item -ItemType Directory -Force $refDir | Out-Null
    & $Cli build (Join-Path $proj "main.pg") --target python --out $refDir *> $null
    Assert ((Get-Content (Join-Path $proj "main.py") -Raw) -eq (Get-Content (Join-Path $refDir "main.py") -Raw)) `
        "the downloaded plugin's emission is byte-equal to the in-box python plugin's"

    # --- 2. offline rebuild: lock-first cache, zero network ---------------------------------------
    Stop-Process -Id $reg.Proc.Id -Force
    $env:POLYGLOT_REGISTRY = "http://127.0.0.1:1"  # actively unreachable, not just stale
    Remove-Item (Join-Path $proj "main.py")
    & $Cli build (Join-Path $proj "main.pg") --out $proj *> $null
    Assert ($LASTEXITCODE -eq 0 -and (Test-Path (Join-Path $proj "main.py"))) `
        "a fresh process rebuilds fully OFFLINE from lock + verified cache"

    # --- 3. tampered cache entry: refused offline, with the tamper named ---------------------------
    $cachedManifest = Join-Path $entryDir "polyglot-plugin.json"
    (Get-Content $cachedManifest -Raw).Replace('"pyfixture"', '"pyfixture" ') | Set-Content $cachedManifest -NoNewline
    $out3 = & $Cli build (Join-Path $proj "main.pg") --out $proj 2>&1 | Out-String
    Assert ($LASTEXITCODE -ne 0 -and $out3 -match 'hash mismatch|tampered|unavailable') `
        "a tampered cache entry is refused offline, naming the reason"

    # --- 4. registry back: the exact pinned artifact heals the cache -------------------------------
    $reg = Start-Registry
    $env:POLYGLOT_REGISTRY = "http://127.0.0.1:$($reg.Port)"
    & $Cli build (Join-Path $proj "main.pg") --out $proj *> $null
    Assert ($LASTEXITCODE -eq 0) "with the registry back, the pinned artifact re-fetches and heals the cache"
    $lock2 = Get-Content $lockPath -Raw | ConvertFrom-Json
    Assert ($lock2.packages.'@mintplayer/polyglot-target-pyfixture'.integrity -eq $pin.integrity) `
        "healing kept the SAME pinned integrity (no silent re-resolution)"

    # --- 5. a packument that lies about integrity: refused, nothing trusted ------------------------
    $proj2 = Join-Path $work "proj2"
    New-Item -ItemType Directory -Force $proj2 | Out-Null
    Set-Content (Join-Path $proj2 "main.pg") "import { print } from `"std.io`"`nfn main() {`n  print(1)`n}`n" -NoNewline
    Set-Content (Join-Path $proj2 "pgconfig.json") "{ `"targets`": [`"pyfixture2`"], `"dependencies`": { `"pyfixture2`": `"1.2.3`" } }" -NoNewline
    $out5 = & $Cli build (Join-Path $proj2 "main.pg") --out $proj2 2>&1 | Out-String
    Assert ($LASTEXITCODE -ne 0 -and $out5 -match 'integrity mismatch') `
        "a tarball failing the packument's SRI is refused with the mismatch named"
    Assert (-not (Test-Path (Join-Path $cache "@mintplayer/polyglot-target-pyfixture2"))) `
        "nothing from the refused tarball reaches the cache"
    Assert (-not (Test-Path (Join-Path $proj2 "pgconfig.lock.json"))) `
        "no lock entry is written for a refused dependency"

    # --- 6/7. --target override + the config-sourced refusal ---------------------------------------
    & $Cli build (Join-Path $proj "main.pg") --target csharp --out $proj *> $null
    Assert ($LASTEXITCODE -eq 0 -and (Test-Path (Join-Path $proj "main.cs"))) `
        "--target overrides the pgconfig target set"

    # --- include routing (P30 slice 7): rules route the twin; explicit flags keep flag semantics ---
    $proj3 = Join-Path $work "proj3"
    New-Item -ItemType Directory -Force $proj3 | Out-Null
    Set-Content (Join-Path $proj3 "main.pg") "import { print } from `"std.io`"`nfn main() {`n  print(7)`n}`n" -NoNewline
    Set-Content (Join-Path $proj3 "pgconfig.json") ("{ `"targets`": [`"pyfixture`"], " +
        "`"dependencies`": { `"pyfixture`": `"^1.2.0`" }, " +
        "`"include`": [ { `"pattern`": `"*.pg`", `"target`": `"pyfixture`", `"output`": `"twins/py/%(Filename)`" } ] }") -NoNewline
    & $Cli build (Join-Path $proj3 "main.pg") *> $null
    Assert ($LASTEXITCODE -eq 0 -and (Test-Path (Join-Path $proj3 "twins/py/main.py"))) `
        "an include rule routes the emitted twin (config-driven, extension auto-appended)"
    & $Cli build (Join-Path $proj3 "main.pg") --target pyfixture --out $proj3 *> $null
    Assert ($LASTEXITCODE -eq 0 -and (Test-Path (Join-Path $proj3 "main.py"))) `
        "explicit --target + --out bypasses the include rules (flag semantics)"

    # --- cross-dir specifiers (P30 slice 8): TS closures may split; others still refuse -----------
    $proj4 = Join-Path $work "proj4"
    New-Item -ItemType Directory -Force (Join-Path $proj4 "lib") | Out-Null
    Set-Content (Join-Path $proj4 "lib/util.pg") "fn three(): i32 {`n  return 3`n}" -NoNewline
    Set-Content (Join-Path $proj4 "main.pg") "import { three } from `"./lib/util`"`nimport { print } from `"std.io`"`nfn main() {`n  print(three())`n}" -NoNewline
    Set-Content (Join-Path $proj4 "pgconfig.json") ("{ `"targets`": [`"typescript`"], `"include`": [ " +
        "{ `"pattern`": `"main.pg`", `"target`": `"typescript`", `"output`": `"app/%(Filename)`" }, " +
        "{ `"pattern`": `"lib/*.pg`", `"target`": `"typescript`", `"output`": `"shared/%(Filename)`" } ] }") -NoNewline
    & $Cli build (Join-Path $proj4 "main.pg") *> $null
    Assert ($LASTEXITCODE -eq 0 -and (Test-Path (Join-Path $proj4 "app/main.ts")) -and (Test-Path (Join-Path $proj4 "shared/util.ts"))) `
        "a TS closure splits across routed dirs (crossDirImports)"
    Assert ((Get-Content (Join-Path $proj4 "app/main.ts") -Raw) -match 'from "\.\./shared/util"') `
        "the emitted TS import climbs with a real relative specifier"

    $proj5 = Join-Path $work "proj5"
    New-Item -ItemType Directory -Force (Join-Path $proj5 "lib") | Out-Null
    Set-Content (Join-Path $proj5 "lib/util.pg") "fn three(): i32 {`n  return 3`n}" -NoNewline
    Set-Content (Join-Path $proj5 "main.pg") "import { three } from `"./lib/util`"`nimport { print } from `"std.io`"`nfn main() {`n  print(three())`n}" -NoNewline
    Set-Content (Join-Path $proj5 "pgconfig.json") ("{ `"targets`": [`"pyfixture`"], " +
        "`"dependencies`": { `"pyfixture`": `"^1.2.0`" }, `"include`": [ " +
        "{ `"pattern`": `"main.pg`", `"target`": `"pyfixture`", `"output`": `"app/%(Filename)`" }, " +
        "{ `"pattern`": `"lib/*.pg`", `"target`": `"pyfixture`", `"output`": `"shared/%(Filename)`" } ] }") -NoNewline
    $out8 = & $Cli build (Join-Path $proj5 "main.pg") 2>&1 | Out-String
    Assert ($LASTEXITCODE -ne 0 -and $out8 -match 'split the import closure') `
        "a non-crossDirImports target still refuses a split closure (never a miscompile)"
    $noCfg = Join-Path $work "nocfg"
    New-Item -ItemType Directory -Force $noCfg | Out-Null
    Set-Content (Join-Path $noCfg "main.pg") "fn main() {`n}`n" -NoNewline
    $out7 = & $Cli build (Join-Path $noCfg "main.pg") --out $noCfg 2>&1 | Out-String
    Assert ($LASTEXITCODE -ne 0 -and $out7 -match 'no --target given and no pgconfig\.json' -and $out7 -match 'csharp') `
        "no config + no --target refuses, listing the loaded targets"

    # === wave-2 extensions (G34/G35/G40) =========================================================
    # These use in-box targets and `file:` dependencies, so (unlike stages 1-7) they need no live
    # registry — they hold even on a machine where the loopback registry is flaky (MEMORY.md).

    # --- G40a: bare `polyglot build` discovers inputs from include patterns (match: every file emits) ---
    # `%(RecursiveDir)` places a nested source at the mirrored output path.
    $disc = Join-Path $work "discover"
    New-Item -ItemType Directory -Force (Join-Path $disc "sub") | Out-Null
    Set-Content (Join-Path $disc "main.pg") "import { print } from `"std.io`"`nfn main() {`n  print(1)`n}`n" -NoNewline
    Set-Content (Join-Path $disc "sub/helper.pg") "fn helper(): i32 => 2`n" -NoNewline
    Set-Content (Join-Path $disc "pgconfig.json") ("{ `"targets`": [`"csharp`"], `"include`": [ " +
        "{ `"pattern`": `"**/*.pg`", `"target`": `"csharp`", `"output`": `"gen/%(RecursiveDir)%(Filename)`" } ] }") -NoNewline
    Push-Location $disc
    & $Cli build *> $null   # no input arg: discovery from the include patterns
    $discExit = $LASTEXITCODE
    Pop-Location
    Assert ($discExit -eq 0 -and (Test-Path (Join-Path $disc "gen/main.cs")) -and (Test-Path (Join-Path $disc "gen/sub/helper.cs"))) `
        "bare 'build' discovers every matched input and emits at the %(RecursiveDir) paths"

    # --- G40a: bare `polyglot build` where the patterns match NOTHING refuses with guidance -------------
    $empty = Join-Path $work "discover-empty"
    New-Item -ItemType Directory -Force $empty | Out-Null
    Set-Content (Join-Path $empty "main.pg") "fn main() {`n}`n" -NoNewline
    Set-Content (Join-Path $empty "pgconfig.json") ("{ `"targets`": [`"csharp`"], `"include`": [ " +
        "{ `"pattern`": `"nonesuch/*.pg`", `"target`": `"csharp`", `"output`": `"out/%(Filename)`" } ] }") -NoNewline
    Push-Location $empty
    $eout = & $Cli build 2>&1 | Out-String
    $eExit = $LASTEXITCODE
    Pop-Location
    Assert ($eExit -ne 0 -and $eout -match "needs an input file") `
        "bare 'build' with no matching inputs refuses with a guidance message"

    # --- G40b: `%(TargetLanguage)` + `%(RecursiveDir)` route a nested tree per target to disk -----------
    $tmpl = Join-Path $work "templated"
    New-Item -ItemType Directory -Force (Join-Path $tmpl "mod") | Out-Null
    Set-Content (Join-Path $tmpl "main.pg") "import { print } from `"std.io`"`nfn main() {`n  print(1)`n}`n" -NoNewline
    Set-Content (Join-Path $tmpl "mod/twain.pg") "fn twain(): i32 => 2`n" -NoNewline
    Set-Content (Join-Path $tmpl "pgconfig.json") ("{ `"targets`": [`"csharp`",`"typescript`"], `"include`": [ " +
        "{ `"pattern`": `"**/*.pg`", `"target`": `"csharp`", `"output`": `"dist/%(TargetLanguage)/%(RecursiveDir)%(Filename)`" }, " +
        "{ `"pattern`": `"**/*.pg`", `"target`": `"typescript`", `"output`": `"dist/%(TargetLanguage)/%(RecursiveDir)%(Filename)`" } ] }") -NoNewline
    Push-Location $tmpl
    & $Cli build *> $null
    $tmplExit = $LASTEXITCODE
    Pop-Location
    Assert ($tmplExit -eq 0 -and
            (Test-Path (Join-Path $tmpl "dist/csharp/main.cs")) -and (Test-Path (Join-Path $tmpl "dist/csharp/mod/twain.cs")) -and
            (Test-Path (Join-Path $tmpl "dist/typescript/main.ts")) -and (Test-Path (Join-Path $tmpl "dist/typescript/mod/twain.ts"))) `
        "%(TargetLanguage)/%(RecursiveDir) templates land each target's nested tree at distinct paths"

    # --- G35: `polyglot install <local dir>` validates a manifest; a corrupted one is refused ----------
    $goodPlugin = Join-Path $repo "plugins/python"
    $vout = & $Cli install $goodPlugin 2>&1 | Out-String
    Assert ($LASTEXITCODE -eq 0 -and $vout -match 'is valid' -and $vout -match 'file:') `
        "install <local dir> validates the manifest and points at the file: dependency form"
    $badPlugin = Join-Path $work "badplugin"
    New-Item -ItemType Directory -Force $badPlugin | Out-Null
    Set-Content (Join-Path $badPlugin "polyglot-plugin.json") '{ "name": "@x/broken", NOT VALID JSON' -NoNewline
    $bout = & $Cli install $badPlugin 2>&1 | Out-String
    Assert ($LASTEXITCODE -ne 0 -and $bout -match 'invalid plugin') `
        "install <local dir> refuses a corrupted manifest, naming it invalid"

    # --- G34: a `file:` dependency builds fully OFFLINE (registry pointed at a dead port) --------------
    # Local plugins resolve in place, so a build never touches the network — proven by unreachable URL.
    $fileDep = Join-Path $work "filedep"
    New-Item -ItemType Directory -Force $fileDep | Out-Null
    Set-Content (Join-Path $fileDep "main.pg") "import { print } from `"std.io`"`nfn main() {`n  print(7)`n}`n" -NoNewline
    $pyPluginFwd = ($goodPlugin -replace '\\','/')
    Set-Content (Join-Path $fileDep "pgconfig.json") ("{ `"targets`": [`"python`"], `"dependencies`": { " +
        "`"@mintplayer/polyglot-target-python`": `"file:$pyPluginFwd`" } }") -NoNewline
    $env:POLYGLOT_REGISTRY = "http://127.0.0.1:1"   # actively unreachable
    & $Cli build (Join-Path $fileDep "main.pg") --out $fileDep *> $null
    Assert ($LASTEXITCODE -eq 0 -and (Test-Path (Join-Path $fileDep "main.py"))) `
        "a file: dependency resolves in place and builds fully offline (registry unreachable)"

    # --- G34: `polyglot install <name@version>` warms a fresh cache from the registry -----------------
    # (Needs the live mock; on the flaky-loopback dev box this shares stage 1's outcome. The
    # cache-then-offline round-trip itself is already proven by stages 1-2.)
    if (-not $reg.Proc.HasExited) {
        $cache2 = Join-Path $work "cache2"
        New-Item -ItemType Directory -Force $cache2 | Out-Null
        $env:POLYGLOT_CACHE = $cache2
        $env:POLYGLOT_REGISTRY = "http://127.0.0.1:$($reg.Port)"
        & $Cli install "@mintplayer/polyglot-target-pyfixture@1.2.3" *> $null
        Assert ($LASTEXITCODE -eq 0 -and
                (Test-Path (Join-Path $cache2 "@mintplayer/polyglot-target-pyfixture/1.2.3/polyglot-plugin.json"))) `
            "polyglot install <name@version> warms the versioned cache entry from the registry"
    }
} finally {
    if ($reg -and -not $reg.Proc.HasExited) { Stop-Process -Id $reg.Proc.Id -Force -ErrorAction SilentlyContinue }
    Remove-Item Env:POLYGLOT_CACHE -ErrorAction SilentlyContinue
    Remove-Item Env:POLYGLOT_REGISTRY -ErrorAction SilentlyContinue
}

Write-Host ""
if ($bad -eq 0) { Write-Host "Registry gate: all assertions passed."; exit 0 }
Write-Host "$bad registry-gate assertion(s) failed."; exit 1
