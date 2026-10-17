#!/usr/bin/env pwsh
# G41 CLI-surface smoke + G39 four-target single-invocation check.
#
# Fast, dependency-free assertions on the CLI's front door — the contract editor tooling, CI, and the
# MSBuild/NuGet integration all lean on: version shape, help, exit codes for bad input, and that a single
# multi-target build emits byte-for-byte what four separate per-target builds do.
#
# Multi-target invocation (G39): `--target` names ONE override target (a repeat is a usage error since P40;
# `--only a,b` narrows the pgconfig set instead), so the real single-invocation multi-target path is a
# pgconfig.json `targets` array with no --target override — that is what this exercises.
#
# P40 (issue #73) adds: relative config discovery, every `build --check` case, `--only`, the outputs
# record (stale-output cleanup and the deletion invariant), the named `spark-ci-flow` test, and
# origin-info byte-identity across two checkout roots.
#
# Usage:  pwsh tests/cli/run-cli-smoke.ps1   (build the solution first; see CLAUDE.md)

param(
    [string]$Cli = "$PSScriptRoot\..\..\x64\Debug\MintPlayer.Polyglot.Cli.exe"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Cli)) {
    Write-Host "polyglot CLI not found at $Cli — build the solution first (see CLAUDE.md)."
    exit 2
}

$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("polyglot-cli-smoke-" + [System.Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null

$failures = 0
function Check([bool]$cond, [string]$name) {
    if ($cond) { Write-Host "[PASS] $name" } else { Write-Host "[FAIL] $name"; $script:failures++ }
}

try {
    # 1. --version: exit 0 and a semver-shaped stdout.
    $verOut = (& $Cli --version 2>&1 | Out-String).Trim()
    Check ($LASTEXITCODE -eq 0 -and $verOut -match '^\d+\.\d+\.\d+') "--version exits 0 and prints a semver ($verOut)"

    # 2. -h: exit 0 and usage text.
    $helpOut = (& $Cli -h 2>&1 | Out-String)
    Check ($LASTEXITCODE -eq 0 -and $helpOut -match 'Usage:') "-h exits 0 and prints usage"

    # 3. Unknown command: exit 64 (EX_USAGE).
    & $Cli frobnicate *> $null
    Check ($LASTEXITCODE -eq 64) "unknown command exits 64"

    # 4. build of a nonexistent input: nonzero exit with a readable error (stdout or stderr).
    $missing = Join-Path $work "does-not-exist.pg"
    $missOut = (& $Cli build $missing --target csharp --out (Join-Path $work "o-missing") 2>&1 | Out-String)
    Check ($LASTEXITCODE -ne 0 -and $missOut -match 'cannot open|no such file|not found') `
        "build of a missing input fails with a readable error"

    # 5. build with no pgconfig.json and no --target: nonzero refusal (the config-sourced target contract).
    $noCfgDir = Join-Path $work "nocfg"
    New-Item -ItemType Directory -Force $noCfgDir | Out-Null
    Copy-Item (Join-Path $repo "tests/conformance/programs/counter.pg") (Join-Path $noCfgDir "counter.pg")
    $noCfgOut = (& $Cli build (Join-Path $noCfgDir "counter.pg") --lib io --out (Join-Path $work "o-nocfg") 2>&1 | Out-String)
    Check ($LASTEXITCODE -ne 0 -and $noCfgOut -match 'no --target|pgconfig') `
        "build with no config and no --target refuses"

    # 6. G39: a single config-sourced invocation emits byte-identical output to separate per-target
    # invocations. Guards against per-target codegen drifting based on which sibling targets are co-emitted in
    # the same run. Extended (P35 slice 3) from one program to three representative shapes, because the merged
    # conformance runner now relies on this single-vs-per-target equivalence for its whole corpus:
    #   - counter.pg       : the simple single-file, all four targets
    #   - modular/         : a multi-file program (co-emitted modules vs per-target runs, --root)
    #   - operators_full.pg: a pinned PHP refuser under its 3-target config (csharp/typescript/python; php
    #                        excluded because it refuses) vs per-target runs of those three
    $extOf = @{ csharp = "cs"; typescript = "ts"; python = "py"; php = "php" }

    # Compare every emitted file of each target's extension between the single (config-sourced) build dir and a
    # per-target build dir. Returns $false on any missing file or byte mismatch (multi-file: all modules).
    function Test-G39($label, $isDir, $srcName, $targetList) {
        $case = Join-Path $work "g39-$label"
        New-Item -ItemType Directory -Force $case | Out-Null
        $srcRoot = Join-Path $repo "tests/conformance/programs"
        if ($isDir) {
            Copy-Item -Recurse (Join-Path $srcRoot $srcName "*") $case
            $entry = Join-Path $case "entry.pg"
        } else {
            Copy-Item (Join-Path $srcRoot "$srcName.pg") $case
            $entry = Join-Path $case "$srcName.pg"
        }
        $json = '{ "targets": [' + (($targetList | ForEach-Object { "`"$_`"" }) -join ", ") + '] }'
        $json | Set-Content (Join-Path $case "pgconfig.json")

        $single = Join-Path $case "single"
        $singleArgs = @("build", $entry, "--lib", "io", "--out", $single)
        if ($isDir) { $singleArgs += @("--root", $case) }
        & $Cli @singleArgs *> $null
        if ($LASTEXITCODE -ne 0) { Write-Host "        ${label}: single config-sourced build failed"; return $false }

        $ok = $true
        foreach ($t in $targetList) {
            $ext = $extOf[$t]
            $sepDir = Join-Path $case "sep-$t"
            $sepArgs = @("build", $entry, "--target", $t, "--lib", "io", "--out", $sepDir)
            if ($isDir) { $sepArgs += @("--root", $case) }
            & $Cli @sepArgs *> $null
            if ($LASTEXITCODE -ne 0) { Write-Host "        ${label}/${t}: per-target build failed"; $ok = $false; continue }
            foreach ($f in Get-ChildItem $single -Filter "*.$ext") {
                $b = Join-Path $sepDir $f.Name
                if (-not (Test-Path $b)) { Write-Host "        ${label}/${t}: $($f.Name) missing from per-target run"; $ok = $false; continue }
                if ((Get-FileHash $f.FullName -Algorithm SHA256).Hash -ne (Get-FileHash $b -Algorithm SHA256).Hash) {
                    Write-Host "        ${label}/${t}: $($f.Name) differs single vs per-target"; $ok = $false
                }
            }
        }
        return $ok
    }

    # ---- P38 / issue #69: --origin-info, the standing flag-on witness (PRD A10) -----------------
    # The feature is deliberately NOT an arm-trace arm (it is manifest data, not a rule), so without a
    # gate leg that turns it ON nothing would ever observe it running and the next refactor of the
    # emitter's line() chokepoint would break it silently.
    & {
        $ld = Join-Path $work "origininfo"
        New-Item -ItemType Directory -Force $ld | Out-Null
        $src = Join-Path $ld "origin.pg"
        # Deliberately covers the emitter's IRREGULAR paths, not a representative sample: a block lambda
        # (flattened by inlineBlock — a directive spliced in there would break compilation), a try/catch
        # and a for loop (rule-driven statement arms), and a class with a field initializer (the one
        # declaration kind whose emitted code actually executes).
        @(
            'class Counter {',
            '  var n: i32 = 7',
            '  fn bump(): i32 { return this.n }',
            '}',
            'fn main() {',
            '  var total = 0',
            '  let add = (v: i32) => { total += v }',
            '  add(3)',
            '  for i in 0..2 { total += i }',
            '  try { total += Counter().bump() } catch (e: Error) { total = -1 }',
            '  print(total)',
            '}'
        ) | Set-Content -LiteralPath $src -Encoding UTF8

        $offDir = Join-Path $ld "off"; $onDir = Join-Path $ld "on"
        & $Cli build $src --target csharp --lib io --out $offDir 2>&1 | Out-Null
        $offRc = $LASTEXITCODE
        & $Cli build $src --target csharp --lib io --origin-info --out $onDir 2>&1 | Out-Null
        $onRc = $LASTEXITCODE
        $offFile = Join-Path $offDir "origin.cs"
        $onFile  = Join-Path $onDir "origin.cs"
        Check ($offRc -eq 0 -and $onRc -eq 0 -and (Test-Path $offFile) -and (Test-Path $onFile)) `
            "P38: build succeeds with and without --origin-info"

        if ((Test-Path $offFile) -and (Test-Path $onFile)) {
            $onLines = Get-Content -LiteralPath $onFile
            # Every emitted line carries exactly one directive: strip the directives and what remains must
            # be the flag-OFF output, byte for byte. That proves placement AND no-drift in one comparison,
            # without pinning a golden file (PRD A2).
            $directives = @($onLines | Where-Object { $_ -match '^#line ' })
            $body = @($onLines | Where-Object { $_ -notmatch '^#line ' })
            $offLines = @(Get-Content -LiteralPath $offFile)
            Check ($directives.Count -eq $body.Count) `
                "P38: every emitted line carries exactly one #line directive ($($directives.Count) directives / $($body.Count) lines)"
            Check (($body -join "`n") -ceq ($offLines -join "`n")) `
                "P38: stripping the directives reproduces the flag-off output exactly"
            Check ($directives | Where-Object { $_ -match '\.pg"$' }).Count -gt 0 `
                "P38: at least one directive names the .pg source"
            Check (($onLines | Where-Object { $_ -match '^#line hidden$' }).Count -gt 0) `
                "P38: scaffolding is emitted as #line hidden (no phantom attribution)"
            # A directive must never be spliced INTO a line — the inlineBlock hazard.
            Check (($onLines | Where-Object { $_ -match '.\#line ' }).Count -eq 0) `
                "P38: no directive is spliced mid-line (inlineBlock stays suppressed)"
        }

        # P39/issue #71 INVERTED this check. Through P38 only C# and TypeScript declared an
        # `originMapping`, so asking only for Python refused with exit 64. All four targets now declare a
        # sink, so the same command must SUCCEED and write a sidecar — the refusal path survives for a
        # third-party plugin that declares no originMapping, but has no in-box trigger left to smoke-test.
        $pyOut = Join-Path $ld "py"
        & $Cli build $src --target python --lib io --origin-info --out $pyOut 2>&1 | Out-Null
        Check ($LASTEXITCODE -eq 0) "P39: --origin-info succeeds for python (it now declares a sink)"
        Check ((Get-ChildItem -Path $pyOut -Filter '*.py.map' -ErrorAction SilentlyContinue).Count -gt 0) `
            "P39: python --origin-info writes a v3 sidecar"

        # TypeScript answers the same flag with a v3 sidecar plus a footer pointing at it.
        $tsDir = Join-Path $ld "ts"
        & $Cli build $src --target typescript --lib io --origin-info --out $tsDir 2>&1 | Out-Null
        $tsMap = Join-Path $tsDir "origin.ts.map"
        $tsOk = (Test-Path $tsMap)
        if ($tsOk) {
            $map = Get-Content -LiteralPath $tsMap -Raw | ConvertFrom-Json
            $tsOk = ($map.version -eq 3) -and ($map.sources.Count -gt 0) -and
                    ($map.sourcesContent.Count -gt 0) -and ($map.mappings.Length -gt 0)
            $tsOk = $tsOk -and ((Get-Content -LiteralPath (Join-Path $tsDir "origin.ts") -Raw) -match 'sourceMappingURL=origin\.ts\.map')
        }
        Check $tsOk "P38: TypeScript emits a valid v3 sidecar with sourcesContent and a sourceMappingURL footer"
    }

    # 7. P40 A1: a SUCCESSFUL build prints its warnings with their real severity, once — not `error:`, not
    # only on failure, and not duplicated. (P40 B3 retired the non-ASCII literal caveat; Python's
    # `utf16Strings:index` "emulated" stance is the warning now: one target, one line.)
    $warnDir = Join-Path $work "warn"
    New-Item -ItemType Directory -Force $warnDir | Out-Null
    Set-Content -Path (Join-Path $warnDir "pgconfig.json") -Value '{ "targets": ["csharp", "typescript", "python"] }' -NoNewline
    Set-Content -Path (Join-Path $warnDir "w.pg") -Value "import `"std.strings`"`nfn main() {`n  print(`"caf\u{E9}`".len())`n}`n" -NoNewline
    $warnOut = (& $Cli build (Join-Path $warnDir "w.pg") --lib io --out (Join-Path $work "o-warn") 2>&1 | Out-String)
    $warnExit = $LASTEXITCODE
    $warnCount = ([regex]::Matches($warnOut, ":\d+:\d+: warning: target 'python' emulates utf16Strings:index")).Count
    Check ($warnExit -eq 0 -and $warnCount -eq 1 -and $warnOut -notmatch ': error: ') `
        "a successful 3-target build prints its warning once, as 'warning:' (exit $warnExit, $warnCount warning line(s))"
    # ---- P40 (issue #73): config discovery, --check, the outputs record, --only, relative origins ---------
    # One CLI call, run from a chosen working directory (relative inputs are the point of A5).
    function Pg([string]$Dir, [string[]]$PgArgs) {
        Push-Location $Dir
        try { $o = (& $Cli @PgArgs 2>&1 | Out-String); $rc = $LASTEXITCODE } finally { Pop-Location }
        return [pscustomobject]@{ Rc = $rc; Out = $o }
    }
    # A project: pgconfig.json text + { relative path = content }.
    function New-Proj([string]$Dir, [string]$Config, [hashtable]$Files) {
        New-Item -ItemType Directory -Force $Dir | Out-Null
        if ($Config) { Set-Content -LiteralPath (Join-Path $Dir "pgconfig.json") -Value $Config }
        foreach ($k in $Files.Keys) {
            $p = Join-Path $Dir $k
            New-Item -ItemType Directory -Force (Split-Path $p -Parent) | Out-Null
            Set-Content -LiteralPath $p -Value $Files[$k]
        }
        return $Dir
    }
    function Snapshot([string]$Dir) { # path -> hash, for "untouched" assertions
        $h = @{}
        if (Test-Path $Dir) {
            foreach ($f in Get-ChildItem -LiteralPath $Dir -Recurse -File) { $h[$f.FullName] = (Get-FileHash $f.FullName).Hash }
        }
        return ($h.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join "`n"
    }
    $fnA = 'fn alpha(): i32 { return 1 }'
    $fnB = 'fn beta(): i32 { return 2 }'

    # -- A5: relative inputs find their config (3 forms + ./a.pg), and mixed rel/abs is ONE project ---------
    & {
        $root = Join-Path $work "p40-a5"
        $lib = New-Proj (Join-Path $root "LibA") `
            '{ "targets": ["csharp"], "forbiddenIdentifiers": ["temp"], "include": [ { "pattern": "*.pg", "output": "out/%(Filename)" } ] }' `
            @{ "a.pg" = $fnA; "b.pg" = $fnB; "bad.pg" = "fn f(): i32 { let temp = 1`n return temp }" }
        New-Item -ItemType Directory -Force (Join-Path $root "other") | Out-Null
        $routed = Join-Path $lib "out/a.cs"
        foreach ($form in @(@($lib, "a.pg"), @($lib, "./a.pg"), @($root, "LibA/a.pg"), @((Join-Path $root "other"), "../LibA/a.pg"))) {
            Remove-Item -Recurse -Force (Join-Path $lib "out") -ErrorAction SilentlyContinue
            $r = Pg $form[0] @("build", $form[1])
            Check ($r.Rc -eq 0 -and (Test-Path $routed)) "P40 A5: 'build $($form[1])' finds LibA/pgconfig.json (include routing applied)"
        }
        $r = Pg $lib @("check", "bad.pg")
        Check ($r.Rc -eq 1) "P40 A5: 'check bad.pg' (relative) applies the config's forbiddenIdentifiers (exit $($r.Rc))"
        Remove-Item -Recurse -Force (Join-Path $lib "out") -ErrorAction SilentlyContinue
        $r = Pg (Join-Path $root "other") @("build", "../LibA/a.pg", (Join-Path $lib "b.pg"))
        $aCs = Join-Path $lib "out/a.cs"
        Check ($r.Rc -eq 0 -and (Test-Path $aCs) -and ((Get-Content -Raw $aCs) -match 'partial')) `
            "P40 A5: mixed relative + absolute inputs of one config build as ONE project (partial wrapper)"
    }

    # -- usage errors ---------------------------------------------------------------------------------------
    & {
        $d = New-Proj (Join-Path $work "p40-usage") '{ "targets": ["csharp", "typescript"] }' @{ "a.pg" = $fnA }
        Check ((Pg $d @("build", "a.pg", "--check", "--watch")).Rc -eq 64) "P40 B5: --check --watch is a usage error (64)"
        Check ((Pg $d @("build", "a.pg", "--target", "csharp", "--target", "typescript")).Rc -eq 64) "P40 B6: a repeated --target is a usage error (64)"
        Check ((Pg $d @("build", "a.pg", "--only", "csharp", "--target", "csharp")).Rc -eq 64) "P40 B6: --only with --target is a usage error (64)"
        $r = Pg $d @("build", "a.pg", "--only", "python")
        Check ($r.Rc -eq 64 -and $r.Out -match 'csharp, typescript' -and $r.Out -match 'python') "P40 B6: --only outside the pgconfig targets is 64, naming both lists"
        $r = Pg $d @("build", "a.pg", "--only", "typescript")
        Check ($r.Rc -eq 0 -and (Test-Path (Join-Path $d "a.ts")) -and -not (Test-Path (Join-Path $d "a.cs"))) "P40 B6: --only typescript builds only the TS twin"
        $r = Pg $d @("build", "a.pg", "--only", "csharp", "--only", "typescript")
        Check ($r.Rc -eq 0 -and (Test-Path (Join-Path $d "a.cs"))) "P40 B6: --only is repeatable"
        # P40 review (B7): an empty entry used to vanish, and an all-empty list silently meant ALL targets.
        foreach ($bad in @("", ",", " ", "csharp,", ", typescript")) {
            Remove-Item (Join-Path $d "a.cs"), (Join-Path $d "a.ts") -ErrorAction SilentlyContinue
            $r = Pg $d @("build", "a.pg", "--only", $bad)
            Check ($r.Rc -eq 64 -and $r.Out -match 'empty target name' -and -not (Test-Path (Join-Path $d "a.cs")) -and -not (Test-Path (Join-Path $d "a.ts"))) `
                "P40 review B7: --only '$bad' is a usage error (64) that builds nothing"
        }
    }

    # -- P40 review (A5): a pgconfig that fails to parse or validate refuses on EVERY command, --target included -
    & {
        $d = New-Proj (Join-Path $work "rv-a5") '{' @{ "a.pg" = $fnA }
        $cases = @(
            @('{', 'malformed JSON'),
            @('{"targets":["csharp"],"access":"public"}', 'access" was removed'),
            @('{"targets":["nosuchtarget"]}', "unknown target 'nosuchtarget'"),
            @('{"target":["csharp"]}', 'unknown key "target"'),
            @('{"targets":"csharp"}', 'must be an array'),
            # P40 review r3: an empty target set, a blank pattern and a duplicate key refuse everywhere too.
            @('{"targets":[]}', 'must name at least one target'),
            @('{"targets":["csharp"],"include":[{"pattern":" ","output":"x/%(Filename)"}]}', 'is blank'),
            @('{"targets":["csharp"],"targets":["python"]}', 'duplicate key')
        )
        foreach ($c in $cases) {
            Set-Content -LiteralPath (Join-Path $d "pgconfig.json") -Value $c[0]
            Remove-Item (Join-Path $d "a.cs") -ErrorAction SilentlyContinue
            $ok = $true
            foreach ($cmd in @(@("build", "a.pg", "--target", "csharp"), @("build", "a.pg"), @("build"), @("check", "a.pg"), @("build", "a.pg", "--check"))) {
                $r = Pg $d $cmd
                if ($r.Rc -ne 64 -or $r.Out -notmatch [regex]::Escape($c[1])) { $ok = $false; Write-Host "        '$($cmd -join ' ')' on $($c[0]): exit $($r.Rc)" }
            }
            Check ($ok -and -not (Test-Path (Join-Path $d "a.cs"))) "P40 review A5: pgconfig $($c[0]) -> 64 on build (--target too), bare build, check and --check; nothing written"
        }
    }

    # -- P40 review (A3): a case-only rename of a .pg renames its output on a case-insensitive filesystem -----
    & {
        $d = New-Proj (Join-Path $work "rv-a3") '{"targets":["typescript"],"include":[{"pattern":"src/*.pg","output":"gen/%(Filename)","owned":true}]}' @{ "src/Foo.pg" = $fnA }
        Check ((Pg $d @("build")).Rc -eq 0) "P40 review A3: build src/Foo.pg"
        Move-Item (Join-Path $d "src/Foo.pg") (Join-Path $d "src/foo.tmp"); Move-Item (Join-Path $d "src/foo.tmp") (Join-Path $d "src/foo.pg")
        $caseInsensitive = Test-Path (Join-Path $d "GEN/FOO.TS")
        $r = Pg $d @("build", "--check")
        $names = @(Get-ChildItem (Join-Path $d "gen") | ForEach-Object Name)
        if ($caseInsensitive) {
            Check ($r.Rc -eq 3 -and $r.Out -cmatch 'would rename .*Foo\.ts -> foo\.ts' -and $r.Out -cmatch 'git mv -f "gen/Foo\.ts" "gen/foo\.ts"' -and ($names -join ',') -ceq 'Foo.ts') "P40 review A3: --check reports the case-only rename (exit 3) with the git mv remedy, and renames nothing"
        } else {
            Check ($r.Rc -eq 3 -and $r.Out -match 'would create .*foo\.ts' -and $r.Out -match 'would delete .*Foo\.ts') "P40 review A3: --check (case-sensitive fs) reports create + delete (exit 3)"
        }
        $r = Pg $d @("build")
        $names = @(Get-ChildItem (Join-Path $d "gen") | ForEach-Object Name)
        Check ($r.Rc -eq 0 -and ($names -join ',') -ceq 'foo.ts') "P40 review A3: build leaves exactly gen/foo.ts, in the exact case (was: Foo.ts forever on Windows)"
        Check ((Pg $d @("build", "--check")).Rc -eq 0) "P40 review A3: ... and --check is then clean"
    }

    # -- P40 review (A4): a partly corrupt record is EMPTY, whichever position the bad part takes -------------
    & {
        foreach ($order in @("after", "before")) {
            $d = New-Proj (Join-Path $work "rv-a4-$order") '{"targets":["typescript"],"include":[{"pattern":"*.pg","output":"gen/%(Filename)"}]}' @{ "a.pg" = $fnA; "b.pg" = $fnB }
            Pg $d @("build") | Out-Null
            $recp = Join-Path $d ".polyglot/outputs.json"
            $bad = '"zz|x": {"inputs": [], "outputs": [{"path": 5}]}'
            $txt = Get-Content -Raw $recp
            $txt = if ($order -eq "after") { $txt -replace '\]\s*\}\s*\}\s*\}\s*$', "]}, $bad}}" } else { $txt -replace '"sections": \{', "`"sections`": { $bad," }
            Set-Content -LiteralPath $recp -Value $txt -NoNewline
            $ok = $true; try { $txt | ConvertFrom-Json | Out-Null } catch { $ok = $false }
            Move-Item (Join-Path $d "b.pg") (Join-Path $d "c.pg")
            $r = Pg $d @("build", "--check")
            Check ($ok -and $r.Rc -eq 3 -and $r.Out -match 'unreadable' -and $r.Out -notmatch 'would delete') "P40 review A4: a record with a bad section $order the good one: --check warns and reports NO deletion"
            $r = Pg $d @("build")
            Check ($r.Rc -eq 0 -and (Test-Path (Join-Path $d "gen/b.ts"))) "P40 review A4: ... and build deletes nothing it was told about by that record (b.ts kept, bad section $order)"
        }
    }

    # -- B5: every --check case (explicit inputs) -------------------------------------------------------------
    & {
        $d = New-Proj (Join-Path $work "p40-check") '{ "targets": ["csharp"], "include": [ { "pattern": "*.pg", "output": "out/%(Filename)" } ] }' @{ "a.pg" = $fnA }
        $r = Pg $d @("build", "a.pg", "--check")
        Check ($r.Rc -eq 3 -and $r.Out -match 'would create .*a\.cs' -and -not (Test-Path (Join-Path $d "out"))) "P40 B5: --check on a fresh project: would create, exit 3, writes nothing (no directory)"
        Check ((Pg $d @("build", "a.pg")).Rc -eq 0) "P40 B5: build"
        Check ((Pg $d @("build", "a.pg", "--check")).Rc -eq 0) "P40 B5: --check unchanged -> 0"
        Add-Content -LiteralPath (Join-Path $d "out/a.cs") -Value "// hand edit"
        $r = Pg $d @("build", "a.pg", "--check")
        Check ($r.Rc -eq 3 -and $r.Out -match 'would update .*a\.cs') "P40 B5: --check modified -> would update, 3"
        Check ((Get-Content -Raw (Join-Path $d "out/a.cs")) -match 'hand edit') "P40 B5: --check left the modified file alone"
        Pg $d @("build", "a.pg") | Out-Null
        Set-Content -LiteralPath (Join-Path $d "b.pg") -Value $fnB
        $r = Pg $d @("build", "a.pg", "b.pg", "--check")
        Check ($r.Rc -eq 3 -and $r.Out -match 'would create .*b\.cs') "P40 B5: --check new input -> would create, 3"
        Set-Content -LiteralPath (Join-Path $d "c.pg") -Value "fn broken(: i32 {"
        $r = Pg $d @("build", "a.pg", "b.pg", "c.pg", "--check")
        Check ($r.Rc -eq 1) "P40 B5: --check compile error + drift -> 1 (exit codes rank, 1 > 3)"
        Remove-Item (Join-Path $d "c.pg")

        # owned extras need discovery mode
        $o = New-Proj (Join-Path $work "p40-owned") '{ "targets": ["typescript"], "include": [ { "pattern": "src/*.pg", "output": "gen/%(Filename)", "owned": true } ] }' @{ "src/a.pg" = $fnA }
        Check ((Pg $o @("build")).Rc -eq 0) "P40 B5: owned discovery build"
        Copy-Item (Join-Path $o "gen/a.ts") (Join-Path $o "gen/stale.ts")
        $r = Pg $o @("build", "--check")
        Check ($r.Rc -eq 3 -and $r.Out -match 'would delete .*stale\.ts') "P40 B5: --check extra marked file in an owned folder -> would delete, 3"
        Set-Content -LiteralPath (Join-Path $o "gen/hand.ts") -Value "export const x = 1;"
        $r = Pg $o @("build", "--check")
        Check ($r.Rc -eq 1 -and $r.Out -notmatch 'would delete .*hand\.ts') "P40 B5: --check extra UNMARKED file in an owned folder -> 1, never deletable"
        $r = Pg $o @("build")
        Check ($r.Rc -eq 1 -and (Test-Path (Join-Path $o "gen/hand.ts")) -and -not (Test-Path (Join-Path $o "gen/stale.ts"))) `
            "P40 B5: build deletes the marked stale file, keeps the unmarked one, exits 1"
        Remove-Item (Join-Path $o "gen/hand.ts")
        Check ((Pg $o @("build", "--check")).Rc -eq 0) "P40 B5: owned folder clean -> --check 0"
    }

    # -- P40 review (#75): the deletion invariant's holes and the overwrite guard (Spark's repros) ------------
    & {
        $hdr = "// <auto-generated/>`n// Generated by Polyglot. Do not edit: regenerate with ``polyglot build``.`nexport const dummy = 1;"
        # 2a: the marker is the header LINE, not the phrase anywhere in lines 1-5.
        $m = New-Proj (Join-Path $work "rv-2a") '{"targets":["typescript"],"include":[{"pattern":"*.pg","output":"gen/%(Filename)","owned":true}]}' `
            @{ "a.pg" = 'fn a(): i32 { return 1 }'; "gen/notes.ts" = "// hand-written`n// this file is not Generated by Polyglot.`nexport const x = 1;" }
        $r = Pg $m @("build")
        Check ($r.Rc -eq 1 -and (Test-Path (Join-Path $m "gen/notes.ts"))) "P40 review 2a: a file that merely mentions the phrase is not deleted from an owned folder (exit 1)"

        # 2b: the record's `map` field is never trusted; the sidecar is derived and must name its owner.
        $b = New-Proj (Join-Path $work "rv-2b/proj") '{"targets":["typescript"],"include":[{"pattern":"*.pg","output":"%(Filename)"}]}' @{ "g.pg" = 'fn g(): i32 { return 1 }' }
        Pg $b @("build") | Out-Null
        New-Proj (Join-Path $work "rv-2b/victim") $null @{ "notes.txt" = "precious" } | Out-Null
        Set-Content -LiteralPath (Join-Path $work "rv-2b/ghost.ts") -Value $hdr
        $recp = Join-Path $b ".polyglot/outputs.json"
        $rec = Get-Content -Raw $recp | ConvertFrom-Json
        ($rec.sections.PSObject.Properties | Select-Object -First 1).Value.outputs = @([pscustomobject]@{ path = "../../ghost.ts"; map = "../../victim/notes.txt" })
        $rec | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $recp
        $r = Pg $b @("build")
        Check ($r.Rc -eq 0 -and (Test-Path (Join-Path $work "rv-2b/victim/notes.txt")) -and -not (Test-Path (Join-Path $work "rv-2b/ghost.ts"))) `
            "P40 review 2b: a forged record `map` never deletes a foreign file (the headed owner still goes)"

        # 2c: unbounded owned scopes are refused at config load; a sibling subtree is allowed.
        foreach ($t in @('./%(Filename)', '%(RecursiveDir)%(Filename)', '../%(Filename)', '/gen/%(Filename)', 'gen-%(TargetLanguage)/%(Filename)')) {
            $c = New-Proj (Join-Path $work ("rv-2c-" + [guid]::NewGuid().ToString('N').Substring(0, 6) + "/proj")) `
                ('{"targets":["typescript"],"include":[{"pattern":"*.pg","output":"' + $t + '","owned":true}]}') @{ "a.pg" = 'fn a(): i32 { return 1 }'; "dummy.ts" = $hdr }
            $r = Pg $c @("build")
            Check ($r.Rc -eq 64 -and $r.Out -match '"owned": true is refused' -and (Test-Path (Join-Path $c "dummy.ts"))) "P40 review 2c: owned '$t' is refused (64), nothing deleted"
        }
        $sib = New-Proj (Join-Path $work "rv-2c-sib/app/proj") '{"targets":["typescript"],"include":[{"pattern":"*.pg","output":"../../web/src/gen/%(Filename)","owned":true}]}' @{ "a.pg" = 'fn a(): i32 { return 1 }' }
        New-Proj (Join-Path $work "rv-2c-sib/web/src/gen") $null @{ "stale.ts" = $hdr } | Out-Null
        $r = Pg $sib @("build")
        Check ($r.Rc -eq 0 -and (Test-Path (Join-Path $work "rv-2c-sib/web/src/gen/a.ts")) -and -not (Test-Path (Join-Path $work "rv-2c-sib/web/src/gen/stale.ts"))) `
            "P40 review 2c: an owned route through `..` into a sibling subtree still works (and cleans it)"

        # 3a: a hand-written file at an output path is never overwritten; --adopt is the upgrade path.
        $h = New-Proj (Join-Path $work "rv-3a") '{"targets":["typescript"],"include":[{"pattern":"*.pg","output":"gen/%(Filename)"}]}' `
            @{ "n.pg" = 'fn n(): i32 { return 1 }'; "gen/n.ts" = "// hand-written`nexport const keep = 1;" }
        $r = Pg $h @("build", "--check")
        Check ($r.Rc -eq 1 -and $r.Out -notmatch 'would update' -and $r.Out -match 'never overwritten') "P40 review 3a: --check reports an unmarked output as an error (1), not would-update"
        $r = Pg $h @("build")
        Check ($r.Rc -eq 1 -and (Get-Content -Raw (Join-Path $h "gen/n.ts")) -match 'hand-written' -and $r.Out -match '--adopt') `
            "P40 review 3a: build refuses to overwrite a hand-written file (exit 1, kept, --adopt named)"
        $r = Pg $h @("build", "--adopt")
        Check ($r.Rc -eq 0 -and (Get-Content -Raw (Join-Path $h "gen/n.ts")) -match 'Generated by Polyglot') "P40 review 3a: --adopt takes it over once"
        Check ((Pg $h @("build")).Rc -eq 0) "P40 review 3a: ... after which a plain build is fine (it carries the header)"
        $x = New-Proj (Join-Path $work "rv-3a-x") '{"targets":["csharp"]}' @{ "a.pg" = 'fn a(): i32 { return 1 }'; "obj/polyglot/a.cs" = "// 0.10.x output: no header" }
        $r = Pg $x @("build", "a.pg", "--out", "obj/polyglot", "--record", "obj/rec.json", "--build-owned", "obj/polyglot")
        Check ($r.Rc -eq 0 -and (Get-Content -Raw (Join-Path $x "obj/polyglot/a.cs")) -match 'Generated by Polyglot') "P40 review 3a: --build-owned (MSBuild's obj dir) overwrites a headerless 0.10.x output"

        # 3b: origin info + a module imported by two roots — one map, determined by the module alone.
        $s = New-Proj (Join-Path $work "rv-3b") '{"targets":["typescript"],"originInfo":true,"include":[{"pattern":"*.pg","output":"gen/%(Filename)"}]}' `
            @{ "a.pg" = 'fn a(): i32 { return 1 }'; "b.pg" = "import { a } from `"./a`"`nfn b(): i32 { return a() }"; "c.pg" = "import { a } from `"./a`"`nfn c(): i32 { return a() + 1 }" }
        $r = Pg $s @("build")
        $map = Join-Path $s "gen/a.ts.map"
        Check ($r.Rc -eq 0 -and (Test-Path $map) -and ((Get-Content -Raw $map | ConvertFrom-Json).sources -join ',') -ceq '../a.pg') `
            "P40 review 3b: a shared module's map lists only its own .pg (no conflict across importers)"
        $q = New-Proj (Join-Path $work "rv-3b-conflict") '{"targets":["typescript"],"include":[{"pattern":"*.pg","output":"gen/same"}]}' @{ "a.pg" = 'fn a(): i32 { return 1 }'; "b.pg" = 'fn b(): i32 { return 2 }' }
        $r = Pg $q @("build")
        Check ($r.Rc -eq 1 -and $r.Out -match "conflicting output .*a\.pg'.* and .*b\.pg'") "P40 review 3b: a conflict names both producers"
    }

    # -- P40 review (#75 pre-push): owned-scope aliases, refusal edges, marker encodings, header-removed warning -
    & {
        $hdr = "// <auto-generated/>`n// Generated by Polyglot. Do not edit: regenerate with ``polyglot build``.`nexport const dummy = 1;"
        # 1. a case alias of the config folder (or an ancestor) is NOT a dedicated subfolder.
        $proj = New-Proj (Join-Path $work "rv-alias/t/Proj") '{"targets":["typescript"],"include":[{"pattern":"src/*.pg","output":"../PROJ/%(RecursiveDir)%(Filename)","owned":true}]}' `
            @{ "src/a.pg" = $fnA; "keep/other.ts" = $hdr }
        if (Test-Path (Join-Path $work "rv-alias/T/PROJ")) {
            $r = Pg $proj @("build")
            Check ($r.Rc -eq 64 -and $r.Out -match '"owned": true is refused' -and (Test-Path (Join-Path $proj "keep/other.ts"))) `
                "P40 review: owned '../PROJ/...' from 'Proj' (a case alias of the config folder) is refused (64), nothing deleted"
            Set-Content -LiteralPath (Join-Path $proj "pgconfig.json") -Value '{"targets":["typescript"],"include":[{"pattern":"src/*.pg","output":"../../T/%(RecursiveDir)%(Filename)","owned":true}]}'
            $r = Pg $proj @("build")
            Check ($r.Rc -eq 64 -and (Test-Path (Join-Path $proj "keep/other.ts"))) "P40 review: owned '../../T/...' (a case alias of the grandparent) is refused (64)"
        } else { Check $true "P40 review: owned case aliases (skipped: case-sensitive filesystem, where they are distinct folders)" }
        # UNC and a nested config's own refusal.
        $u = New-Proj (Join-Path $work "rv-unc") '{"targets":["typescript"],"include":[{"pattern":"*.pg","output":"\\\\server\\share\\gen\\%(Filename)","owned":true}]}' @{ "a.pg" = $fnA }
        $r = Pg $u @("build")
        Check ($r.Rc -eq 64 -and $r.Out -match '"owned": true is refused') "P40 review: an owned UNC template (\\server\share\...) is refused (64)"
        $n = New-Proj (Join-Path $work "rv-nested") '{"targets":["typescript"],"include":[{"pattern":"**/*.pg","output":"gen/%(RecursiveDir)%(Filename)"}]}' @{ "top.pg" = $fnA }
        New-Proj (Join-Path $n "inner") '{"targets":["typescript"],"include":[{"pattern":"*.pg","output":"./%(Filename)","owned":true}]}' @{ "x.pg" = $fnB; "keep.ts" = $hdr } | Out-Null
        $r = Pg $n @("build", "inner/x.pg")
        Check ($r.Rc -eq 64 -and $r.Out -match '"owned": true is refused' -and (Test-Path (Join-Path $n "inner/keep.ts"))) "P40 review: an unbounded owned rule in a NESTED config is refused (64)"
        # 2. the marker survives a BOM and CRLF, and is found after `<?php` on PHP.
        $m = New-Proj (Join-Path $work "rv-marker") '{"targets":["typescript","php"],"include":[{"pattern":"*.pg","output":"gen/%(TargetLanguage)/%(Filename)","owned":true}]}' @{ "a.pg" = $fnA }
        Check ((Pg $m @("build")).Rc -eq 0) "P40 review: marker project builds (TS + PHP)"
        # P40 review r3: the header must be LINE 1 for the BOM to matter (it was line 2, after <auto-generated/>,
        # so a reader that never stripped the BOM still passed). The BOM sits right before the header line.
        $ts = ([IO.File]::ReadAllText((Join-Path $m "gen/typescript/a.ts")) -replace "`r?`n", "`r`n") -replace '^// <auto-generated/>\r\n', ''
        Check ($ts.StartsWith('// Generated by Polyglot.')) "P40 review r3: the BOM case puts the header on line 1"
        [IO.File]::WriteAllText((Join-Path $m "gen/typescript/stale.ts"), $ts, [Text.UTF8Encoding]::new($true))   # BOM + CRLF
        Copy-Item (Join-Path $m "gen/php/a.php") (Join-Path $m "gen/php/stale.php")
        $r = Pg $m @("build")
        Check ($r.Rc -eq 0 -and -not (Test-Path (Join-Path $m "gen/typescript/stale.ts")) -and -not (Test-Path (Join-Path $m "gen/php/stale.php"))) `
            "P40 review: a stale twin with a BOM + CRLF, and a PHP twin (header after <?php), are recognised and deleted"
        # 3. a recorded output whose header a human removed: overwritten (the record vouches), with a warning.
        $w = New-Proj (Join-Path $work "rv-hdr") '{"targets":["typescript"],"include":[{"pattern":"*.pg","output":"gen/%(Filename)"}]}' @{ "a.pg" = $fnA }
        Pg $w @("build") | Out-Null
        $f = Join-Path $w "gen/a.ts"
        (Get-Content $f | Where-Object { $_ -notmatch 'Generated by Polyglot' }) + "// hand edit" | Set-Content $f
        $r = Pg $w @("build")
        Check ($r.Rc -eq 0 -and $r.Out -match 'warning: overwriting .*header line was removed' -and (Get-Content -Raw $f) -match 'Generated by Polyglot') `
            "P40 review: overwriting a recorded output whose header was removed warns"
        # 4. --adopt in watch mode takes over an unmarked output on the first cycle.
        $a = New-Proj (Join-Path $work "rv-watch-adopt") '{"targets":["typescript"],"include":[{"pattern":"*.pg","output":"gen/%(Filename)"}]}' `
            @{ "a.pg" = $fnA; "gen/a.ts" = "// 0.10.x output, no header`nexport function alpha(): number { return 1; }" }
        $wlog = Join-Path $work "rv-watch-adopt.log"
        $proc = Start-Process -FilePath $Cli -ArgumentList @("build", "a.pg", "--watch", "--adopt") -WorkingDirectory $a `
            -RedirectStandardOutput $wlog -RedirectStandardError (Join-Path $work "rv-watch-adopt.err") -PassThru -NoNewWindow
        $deadline = (Get-Date).AddSeconds(20)
        while ((Get-Date) -lt $deadline -and -not ((Get-Content $wlog -Raw -ErrorAction SilentlyContinue) -match 'watching for changes')) { Start-Sleep -Milliseconds 200 }
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        Check ((Get-Content -Raw (Join-Path $a "gen/a.ts")) -match 'Generated by Polyglot') "P40 review: --watch --adopt takes over an unmarked output on its first cycle"
    }

    # -- spark-ci-flow (PLAN slice 13: Spark's review scenario, named so it can be pointed at) --------------
    # Fresh clone, NO record, an owned TS route, C# routed to obj/; rename a .pg; then CI's exact command.
    & {
        $cfg = '{ "targets": ["csharp", "typescript"], "include": [ ' +
               '{ "pattern": "src/*.pg", "target": "csharp", "output": "obj/polyglot/%(Filename)" }, ' +
               '{ "pattern": "src/*.pg", "target": "typescript", "output": "web/gen/%(Filename)", "owned": true } ] }'
        # P40 review (B8/B9): an IMPORTER (main.pg imports a.pg), so an edit to a.pg's signature is `would update`
        # of a twin whose own .pg did not change.
        $fnMain = "import { alpha } from `"./a`"`nfn main2(): i32 { return alpha() + 1 }"
        $origin = New-Proj (Join-Path $work "spark-ci-flow-origin") $cfg @{ "src/a.pg" = $fnA; "src/b.pg" = $fnB; "src/main.pg" = $fnMain }
        Pg $origin @("build") | Out-Null
        # 1. A REAL fresh clone when git is available (no record, no obj/, nothing untracked); else a copy minus
        #    the untracked state (.polyglot/ and obj/ — what a clone never has).
        $s = Join-Path $work "spark-ci-flow"
        $haveGit = [bool](Get-Command git -ErrorAction SilentlyContinue)
        if ($haveGit) {
            Set-Content -LiteralPath (Join-Path $origin ".gitignore") -Value "obj/`n"
            Set-Content -LiteralPath (Join-Path $origin ".gitattributes") -Value "* -text"   # no EOL conversion on checkout (Spark pins eol=lf for its twin)
            # P40 review r3: the machine's commit signing and hooks must not decide this gate.
            $gq = @("-c", "user.name=gate", "-c", "user.email=gate@example.invalid", "-c", "commit.gpgsign=false", "-c", "core.hooksPath=", "-c", "core.autocrlf=false")
            & git @gq -C $origin init -q 2>&1 | Out-Null
            & git @gq -C $origin add -A 2>&1 | Out-Null
            & git @gq -C $origin commit -q -m "twin" 2>&1 | Out-Null
            & git @gq clone -q $origin $s 2>&1 | Out-Null
            # P40 review r3: git is installed, so the clone MUST have happened. It used to fall back to a copy silently.
            $cloned = (Test-Path (Join-Path $s "web/gen/a.ts")) -and (Test-Path (Join-Path $s ".git")) -and -not (Test-Path (Join-Path $s ".polyglot")) -and -not (Test-Path (Join-Path $s "obj"))
            Check $cloned "spark-ci-flow: git is installed, so the fixture is a real clone (no fallback to a copy)"
        }
        if (-not $haveGit) {
            Remove-Item -Recurse -Force $s -ErrorAction SilentlyContinue
            Copy-Item -Recurse $origin $s
            Remove-Item -Recurse -Force (Join-Path $s ".polyglot"), (Join-Path $s "obj"), (Join-Path $s ".git") -ErrorAction SilentlyContinue
        }
        Check (-not (Test-Path (Join-Path $s ".polyglot")) -and -not (Test-Path (Join-Path $s "obj"))) "spark-ci-flow: a fresh clone (git: $haveGit) has no record and no obj/"
        $r = Pg $s @("build", "--check", "--only", "typescript")
        if ($r.Rc -ne 0) { Write-Host "        $($r.Out.Trim() -replace "`r?`n", ' | ')" }
        Check ($r.Rc -eq 0 -and -not (Test-Path (Join-Path $s ".polyglot"))) "spark-ci-flow: on the clean clone CI's check exits 0 and creates no .polyglot/"
        Move-Item (Join-Path $s "src/b.pg") (Join-Path $s "src/c.pg")     #    ... a rename
        Set-Content -LiteralPath (Join-Path $s "src/a.pg") -Value 'fn alpha(): i32 { return 10 }'   # ... and an edit the importer sees
        $before = Snapshot $s
        $r = Pg $s @("build", "--check", "--only", "typescript")          # 2.
        Check ($r.Rc -eq 3 -and $r.Out -match 'would create .*c\.ts' -and $r.Out -match 'would delete .*b\.ts' -and $r.Out -match 'would update .*a\.ts') `
            "spark-ci-flow: --check --only typescript reports would create <new> + would delete <old> + would update <edited>, exit 3"
        Check ((Snapshot $s) -ceq $before -and $r.Out -notmatch 'obj' -and -not (Test-Path (Join-Path $s ".polyglot"))) `
            "spark-ci-flow: --check left the WHOLE tree byte-identical (no .polyglot/, no obj/, nothing written or deleted)"
        $r = Pg $s @("build")                                             # 3.
        Check ($r.Rc -eq 0 -and -not (Test-Path (Join-Path $s "web/gen/b.ts")) -and (Test-Path (Join-Path $s "web/gen/c.ts"))) `
            "spark-ci-flow: a plain build deletes <old> and writes <new> (no record needed)"
        Check ((Pg $s @("build", "--check", "--only", "typescript")).Rc -eq 0) "spark-ci-flow: --check then exits 0"   # 4.
        Check ((Pg $s @("build", "--check")).Rc -eq 0) "spark-ci-flow: ... and so does a FULL --check (every target)"

        # The `--check --only typescript` matrix under a PolyglotTargets=csharp-style setup.
        Add-Content -LiteralPath (Join-Path $s "obj/polyglot/a.cs") -Value "// drift the C# side"
        $r = Pg $s @("build", "--check", "--only", "typescript")
        Check ($r.Rc -eq 0 -and $r.Out -notmatch 'obj') "check --only typescript: obj/ C# drift is never read or reported (0)"
        Add-Content -LiteralPath (Join-Path $s "web/gen/a.ts") -Value "// hand edit"
        Check ((Pg $s @("build", "--check", "--only", "typescript")).Rc -eq 3) "check --only typescript: modified twin -> 3"
        Pg $s @("build", "--only", "typescript") | Out-Null
        Remove-Item (Join-Path $s "web/gen/a.ts")
        Check ((Pg $s @("build", "--check", "--only", "typescript")).Rc -eq 3) "check --only typescript: missing twin -> 3"
        Pg $s @("build", "--only", "typescript") | Out-Null
        Set-Content -LiteralPath (Join-Path $s "web/gen/contracts.ts") -Value "// <auto-generated/>`nexport interface C {}"
        $r = Pg $s @("build", "--check", "--only", "typescript")
        Check ($r.Out -notmatch 'would delete .*contracts' -and $r.Rc -eq 1) "check --only typescript: an NSwag-style <auto-generated/> file is never reported deletable"
        Pg $s @("build", "--only", "typescript") | Out-Null
        Check (Test-Path (Join-Path $s "web/gen/contracts.ts")) "check --only typescript: ... and a build never deletes it"
    }

    # -- the outputs record (D17-D20) ----------------------------------------------------------------------------
    & {
        $cfg = '{ "targets": ["csharp", "typescript"], "originInfo": true, "include": [ ' +
               '{ "pattern": "src/*.pg", "target": "csharp", "output": "out/cs/%(Filename)" }, ' +
               '{ "pattern": "src/*.pg", "target": "typescript", "output": "out/ts/%(Filename)" } ] }'
        $p = New-Proj (Join-Path $work "p40-record") $cfg @{ "src/a.pg" = $fnA; "src/b.pg" = $fnB }
        $r = Pg $p @("build")
        $rec = Join-Path $p ".polyglot/outputs.json"
        Check ($r.Rc -eq 0 -and (Test-Path $rec) -and ((Get-Content -Raw (Join-Path $p ".polyglot/.gitignore")).Trim() -eq '*')) `
            "record: discovery writes .polyglot/outputs.json with a self-ignoring .gitignore"
        Check ($r.Out -match 'no outputs record') "record: discovery's first build says it starts a record"
        $xr = Join-Path $work "p40-explicit.outputs.json"
        $r2 = Pg $p @("build", "src/a.pg", "src/b.pg", "--record", $xr)
        Check ($r2.Rc -eq 0 -and (Test-Path $xr) -and $r2.Out -notmatch 'no outputs record') `
            "record: an explicit --record (MSBuild) starts silently on a fresh clone"
        Remove-Item $xr
        $json = Get-Content -Raw $rec | ConvertFrom-Json
        Check ($json.version -eq 1 -and ($json.sections.PSObject.Properties.Name -join ',') -match '\|csharp' -and (Get-Content -Raw $rec) -notmatch [regex]::Escape($p)) `
            "record: version 1, per-target sections, paths relative to the record"

        Move-Item (Join-Path $p "src/a.pg") (Join-Path $p "src/z.pg")
        $r = Pg $p @("build")
        Check ($r.Rc -eq 0 -and -not (Test-Path (Join-Path $p "out/ts/a.ts")) -and -not (Test-Path (Join-Path $p "out/ts/a.ts.map")) -and
               -not (Test-Path (Join-Path $p "out/cs/a.cs")) -and (Test-Path (Join-Path $p "out/ts/z.ts"))) `
            "record: a rename deletes the old twin AND its .map (and the old .cs)"

        $bTs = Join-Path $p "out/ts/b.ts"
        (Get-Content $bTs | Where-Object { $_ -notmatch 'Generated by Polyglot' }) | Set-Content $bTs
        Move-Item (Join-Path $p "src/b.pg") (Join-Path $p "src/y.pg")
        $r = Pg $p @("build")
        Check ($r.Rc -eq 0 -and (Test-Path $bTs) -and $r.Out -match 'kept') "record: a recorded file whose header was stripped is kept (with a notice)"
        Remove-Item $bTs

        Pg $p @("build", "--only", "csharp") | Out-Null
        Pg $p @("build", "--only", "typescript") | Out-Null
        $r = Pg $p @("build", "--only", "csharp")
        $secs = (Get-Content -Raw $rec | ConvertFrom-Json).sections.PSObject.Properties.Name
        Check ($r.Rc -eq 0 -and (Test-Path (Join-Path $p "out/ts/z.ts")) -and ($secs -match 'typescript') -and ($secs -match 'csharp')) `
            "record: per-target sections survive alternating --only (a csharp-only build never touches the TS twins)"

        Move-Item (Join-Path $p "src/z.pg") (Join-Path $p "src/x.pg")
        Set-Content -LiteralPath (Join-Path $p "src/broken.pg") -Value "fn broken(: i32 {"
        $r = Pg $p @("build")
        Check ($r.Rc -eq 1 -and (Test-Path (Join-Path $p "out/ts/z.ts")) -and (Test-Path (Join-Path $p "out/cs/z.cs"))) "record: a compile error deletes nothing"
        Remove-Item (Join-Path $p "src/broken.pg")
        Pg $p @("build") | Out-Null

        Set-Content -LiteralPath $rec -Value "{ this is not json"
        $r = Pg $p @("build")
        $ok = $true; try { Get-Content -Raw $rec | ConvertFrom-Json | Out-Null } catch { $ok = $false }
        Check ($r.Rc -eq 0 -and $r.Out -match 'warning' -and $ok) "record: a corrupt record warns, the build succeeds, and the record is rewritten"

        $before = (Get-FileHash $rec).Hash
        Move-Item (Join-Path $p "src/x.pg") (Join-Path $p "src/w.pg")
        $r = Pg $p @("build", "--target", "csharp", "--out", (Join-Path $work "p40-record-tmp"))
        Check ($r.Rc -eq 0 -and (Get-FileHash $rec).Hash -eq $before -and (Test-Path (Join-Path $p "out/cs/x.cs"))) "record: --target + --out leaves the record alone and deletes nothing"
        Move-Item (Join-Path $p "src/w.pg") (Join-Path $p "src/x.pg")

        # a copied repo never deletes outside itself
        $copy = Join-Path $work "p40-record-copy"
        Copy-Item -Recurse $p $copy
        Move-Item (Join-Path $copy "src/x.pg") (Join-Path $copy "src/v.pg")
        $r = Pg $copy @("build")
        Check ($r.Rc -eq 0 -and -not (Test-Path (Join-Path $copy "out/ts/x.ts")) -and (Test-Path (Join-Path $p "out/ts/x.ts"))) `
            "record: a copied repository deletes its own stale twin, never the original's"

        # watch never deletes (and never writes the record)
        Move-Item (Join-Path $p "src/x.pg") (Join-Path $p "src/u.pg")
        $recBefore = (Get-FileHash $rec).Hash
        $wlog = Join-Path $work "p40-watch.log"
        $proc = Start-Process -FilePath $Cli -ArgumentList @("build", "src/u.pg", "--watch") -WorkingDirectory $p `
            -RedirectStandardOutput $wlog -RedirectStandardError (Join-Path $work "p40-watch.err") -PassThru -NoNewWindow
        $deadline = (Get-Date).AddSeconds(20)
        while ((Get-Date) -lt $deadline -and -not ((Get-Content $wlog -Raw -ErrorAction SilentlyContinue) -match 'watching for changes')) { Start-Sleep -Milliseconds 200 }
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        Check ((Test-Path (Join-Path $p "out/ts/u.ts")) -and (Test-Path (Join-Path $p "out/ts/u.ts.map")) -and (Test-Path (Join-Path $p "out/ts/x.ts")) -and
               (Get-FileHash $rec).Hash -eq $recBefore) "record: watch writes the twin + its source map, deletes nothing, leaves the record alone"

        # a cross-group collision is an error
        $g = Join-Path $work "p40-collide"
        New-Proj (Join-Path $g "g1") '{ "targets": ["csharp"] }' @{ "a.pg" = $fnA } | Out-Null
        New-Proj (Join-Path $g "g2") '{ "targets": ["csharp"] }' @{ "a.pg" = $fnB } | Out-Null
        $r = Pg $g @("build", "g1/a.pg", "g2/a.pg", "--out", "shared")
        Check ($r.Rc -eq 1 -and $r.Out -match 'conflicting output') "record: two config groups routing to one path is an error, not a silent overwrite"
    }

    # -- B7: origin-info output is byte-identical across two checkout roots ---------------------------------
    & {
        $cfg = '{ "targets": ["csharp", "typescript"], "originInfo": true, "include": [ { "pattern": "src/*.pg", "output": "gen/%(TargetLanguage)/%(Filename)" } ] }'
        $files = @{ "src/a.pg" = "import { beta } from `"./b`"`nfn alpha(): i32 { return beta() + 1 }"; "src/b.pg" = $fnB }
        $r1 = New-Proj (Join-Path $work "p40-root1/deeper/proj") $cfg $files
        $r2 = New-Proj (Join-Path $work "p40-root2") $cfg $files
        $ok = ((Pg $r1 @("build")).Rc -eq 0) -and ((Pg $r2 @("build")).Rc -eq 0)
        $g1 = @(Get-ChildItem -Recurse -File (Join-Path $r1 "gen") | Sort-Object FullName)
        $ok = $ok -and $g1.Count -ge 4
        foreach ($f in $g1) {
            $twin = Join-Path $r2 ($f.FullName.Substring($r1.Length))
            if (-not (Test-Path $twin) -or (Get-FileHash $f.FullName).Hash -ne (Get-FileHash $twin).Hash) { Write-Host "        differs: $($f.Name)"; $ok = $false }
        }
        Check $ok "P40 B7: origin-info output (.cs #line, .ts + .map) is byte-identical across two checkout roots"
        $cs = Get-Content -Raw (Join-Path $r1 "gen/csharp/a.cs")
        Check ($cs -match '#line \d+ "\.\./\.\./src/a\.pg"') "P40 B7: C# #line paths are relative to the generated file"
    }

    # -- P40 review r3 (#75): the CLI half of round 3 -----------------------------------------------------------
    & {
        $hdr = "// <auto-generated/>`n// Generated by Polyglot. Do not edit: regenerate with ``polyglot build``.`nexport const dummy = 1;"
        $utf8Strict = [Text.UTF8Encoding]::new($false, $true)
        function ReadUtf8Strict([string]$p) { try { return $utf8Strict.GetString([IO.File]::ReadAllBytes($p)) } catch { return $null } }

        # Gating 1: a non-ASCII project path. The outputs list and the depfile were written in the ANSI code page
        # (a lone 0xE9 for 'é'; '日' is outside CP1252 entirely) while MSBuild reads them as UTF-8.
        $u = New-Proj (Join-Path $work "rv3-utf8/p$([char]0xE9)$([char]0x65E5)") '{"targets":["csharp","typescript"],"originInfo":true,"include":[{"pattern":"*.pg","target":"typescript","output":"gen/%(Filename)"}]}' @{ "a.pg" = $fnA; "b.pg" = "import { alpha } from `"./a`"`nfn b(): i32 { return alpha() }" }
        $r = Pg $u @("build", "a.pg", "b.pg", "--out", "obj", "--record", "obj/rec.json", "--depfile", "obj/deps", "--outputs-list", "obj/list")
        $list = ReadUtf8Strict (Join-Path $u "obj/list")
        $deps = ReadUtf8Strict (Join-Path $u "obj/deps")
        $listOk = $null -ne $list -and $list.Contains((Join-Path $u "obj\a.cs")) -and $list.Contains((Join-Path $u "gen\a.ts")) -and
                  @($list.Trim() -split "`n" | Where-Object { -not (Test-Path -LiteralPath $_.Trim()) }).Count -eq 0
        $depsOk = $null -ne $deps -and $deps.Contains((Join-Path $u "pgconfig.json")) -and
                  @($deps.Trim() -split "`n" | Where-Object { -not (Test-Path -LiteralPath $_.Trim()) }).Count -eq 0
        Check ($r.Rc -eq 0 -and $listOk -and $depsOk) "P40 review r3 (gating 1): under 'p$([char]0xE9)$([char]0x65E5)' the outputs list and the depfile are UTF-8 and every line names an existing file"
        $recOk = $false; try { $null = (ReadUtf8Strict (Join-Path $u "obj/rec.json")) | ConvertFrom-Json; $recOk = $true } catch {}
        $mapOk = $false; try { $mapOk = (((ReadUtf8Strict (Join-Path $u "gen/a.ts.map")) | ConvertFrom-Json).file -ceq 'a.ts') } catch {}
        Check ($recOk -and $mapOk) "P40 review r3 (gating 1): ... and the record and the source map read back as UTF-8 JSON"
        $r = Pg $u @("build", "a.pg", "b.pg", "--out", "obj", "--record", "obj/rec.json", "--depfile", "obj/deps", "--outputs-list", "obj/list", "--check")
        Check ($r.Rc -eq 0) "P40 review r3 (gating 1): ... and --check over that path is clean (exit $($r.Rc))"

        # Gating 4a: with originInfo, a case-only rename also renames the sidecar (its `file` names the OLD owner).
        $c = New-Proj (Join-Path $work "rv3-a3a") '{"targets":["typescript"],"originInfo":true,"include":[{"pattern":"src/*.pg","output":"gen/%(Filename)","owned":true}]}' @{ "src/Foo.pg" = $fnA }
        Check ((Pg $c @("build")).Rc -eq 0) "P40 review r3 A3a: build src/Foo.pg with originInfo"
        Move-Item (Join-Path $c "src/Foo.pg") (Join-Path $c "src/foo.tmp"); Move-Item (Join-Path $c "src/foo.tmp") (Join-Path $c "src/foo.pg")
        if (Test-Path (Join-Path $c "GEN/FOO.TS")) {
            $r = Pg $c @("build", "--check")
            Check ($r.Rc -eq 3 -and $r.Out -cmatch 'would rename .*Foo\.ts -> foo\.ts' -and $r.Out -cmatch 'would rename .*Foo\.ts\.map -> foo\.ts\.map') `
                "P40 review r3 A3a: --check reports both case-only renames (exit $($r.Rc); was 1)"
            $r = Pg $c @("build")
            $names = @(Get-ChildItem (Join-Path $c "gen") | ForEach-Object Name | Sort-Object)
            Check ($r.Rc -eq 0 -and ($names -join ',') -ceq 'foo.ts,foo.ts.map' -and $r.Out -cmatch 'git mv -f') `
                "P40 review r3 A3a: build renames the twin AND its map, and names the git mv remedy (exit $($r.Rc), gen: $($names -join ','))"
            Check ((Pg $c @("build")).Rc -eq 0 -and (Pg $c @("build", "--check")).Rc -eq 0) "P40 review r3 A3a: ... and later builds and --check stay green"
            # Gating 4c: a non-ASCII case change (towlower folded ASCII only).
            $n = New-Proj (Join-Path $work "rv3-a3c") '{"targets":["typescript"],"include":[{"pattern":"src/*.pg","output":"gen/%(Filename)","owned":true}]}' @{ "src/$([char]0xC4)rger.pg" = $fnA }
            Check ((Pg $n @("build")).Rc -eq 0) "P40 review r3 A3c: build src/$([char]0xC4)rger.pg"
            Move-Item -LiteralPath (Join-Path $n "src/$([char]0xC4)rger.pg") (Join-Path $n "src/x.tmp"); Move-Item -LiteralPath (Join-Path $n "src/x.tmp") (Join-Path $n "src/$([char]0xE4)rger.pg")
            $r = Pg $n @("build", "--check")
            Check ($r.Rc -eq 3) "P40 review r3 A3c: --check sees the non-ASCII case-only rename (exit $($r.Rc); was 0)"
            $r = Pg $n @("build")
            $names = @(Get-ChildItem (Join-Path $n "gen") | ForEach-Object Name)
            Check ($r.Rc -eq 0 -and ($names -join ',') -ceq "$([char]0xE4)rger.ts") "P40 review r3 A3c: build renames it to the exact case"
        } else { Check $true "P40 review r3 A3a/A3c (skipped: case-sensitive filesystem)" }

        # Part 2: --adopt is honoured only while the record is missing; each adopted file is named.
        $a = New-Proj (Join-Path $work "rv3-adopt") '{"targets":["typescript"],"include":[{"pattern":"*.pg","output":"gen/%(Filename)"}]}' `
            @{ "n.pg" = 'fn n(): i32 { return 1 }'; "gen/n.ts" = "// 0.10.x output`nexport const old = 1;" }
        $r = Pg $a @("build", "--adopt")
        Check ($r.Rc -eq 0 -and $r.Out -match 'adopt: taking over .*n\.ts') "P40 review r3: --adopt names each file it takes over"
        Set-Content -LiteralPath (Join-Path $a "b.pg") -Value 'fn b(): i32 { return 2 }'
        Set-Content -LiteralPath (Join-Path $a "gen/b.ts") -Value "// hand-written`nexport const mine = 1;"
        $r = Pg $a @("build", "--adopt")
        Check ($r.Rc -eq 1 -and $r.Out -match '--adopt is ignored' -and (Get-Content -Raw (Join-Path $a "gen/b.ts")) -match 'hand-written') `
            "P40 review r3: with a record present, --adopt is ignored (warned) and a new hand-written gen/b.ts is NOT overwritten (exit $($r.Rc))"

        # Part 2 nit: --check says "would overwrite" for a recorded output whose header was removed.
        $w = New-Proj (Join-Path $work "rv3-hdr") '{"targets":["typescript"],"include":[{"pattern":"*.pg","output":"gen/%(Filename)"}]}' @{ "a.pg" = $fnA }
        Pg $w @("build") | Out-Null
        $f = Join-Path $w "gen/a.ts"
        (Get-Content $f | Where-Object { $_ -notmatch 'Generated by Polyglot' }) + "// hand edit" | Set-Content $f
        $r = Pg $w @("build", "--check")
        Check ($r.Rc -eq 3 -and $r.Out -match 'warning: would overwrite .*header line was removed' -and $r.Out -notmatch 'warning: overwriting') "P40 review r3: --check says 'would overwrite', not 'overwriting'"

        # Part 2: the owned-extras scan skips a nested project's subtree (as discovery does).
        $o = New-Proj (Join-Path $work "rv3-nested-owned") '{"targets":["typescript"],"include":[{"pattern":"src/*.pg","output":"src/%(RecursiveDir)gen/%(Filename)","owned":true}]}' @{ "src/a.pg" = $fnA }
        New-Proj (Join-Path $o "src/sub") '{"targets":["typescript"],"include":[{"pattern":"*.pg","output":"gen/%(Filename)"}]}' @{ "x.pg" = $fnB } | Out-Null
        Check ((Pg (Join-Path $o "src/sub") @("build")).Rc -eq 0 -and (Test-Path (Join-Path $o "src/sub/gen/x.ts"))) "P40 review r3: the nested project builds its own gen/x.ts"
        Check ((Pg $o @("build")).Rc -eq 0) "P40 review r3: the outer owned build"
        $r = Pg $o @("build", "--check")
        Check ($r.Rc -eq 0 -and $r.Out -notmatch 'would delete' -and (Test-Path (Join-Path $o "src/sub/gen/x.ts"))) "P40 review r3: the outer owned scan leaves the nested project's output alone (exit $($r.Rc))"

        # Part 2: `check` works offline — an uncached registry dependency is a note, not exit 64.
        $q = New-Proj (Join-Path $work "rv3-offline") '{"targets":["csharp"],"dependencies":{"polyglot-target-r3-uncached":"^1.0.0"}}' @{ "a.pg" = $fnA }
        $oldCache = $env:POLYGLOT_CACHE
        $env:POLYGLOT_CACHE = Join-Path $work "rv3-offline-cache"
        try { $r = Pg $q @("check", "a.pg") } finally { $env:POLYGLOT_CACHE = $oldCache }
        Check ($r.Rc -eq 0 -and $r.Out -match 'does not use the network') "P40 review r3: check with an uncached registry plugin works offline (exit $($r.Rc); was 64)"

        # Gates: install with a broken config refuses before anything else.
        $b = New-Proj (Join-Path $work "rv3-install") '{' @{}
        $r = Pg $b @("install", "polyglot-target-r3-anything")
        Check ($r.Rc -eq 64 -and $r.Out -match 'malformed JSON') "P40 review r3: install with a malformed pgconfig.json exits 64"

        # Part 2 nits: a version-1.5 record is unreadable; a crashed run's old temp file is swept, a fresh one kept.
        $v = New-Proj (Join-Path $work "rv3-rec15") '{"targets":["typescript"],"include":[{"pattern":"*.pg","output":"gen/%(Filename)"}]}' @{ "a.pg" = $fnA }
        Pg $v @("build") | Out-Null
        $recp = Join-Path $v ".polyglot/outputs.json"
        (Get-Content -Raw $recp) -replace '"version": 1,', '"version": 1.5,' | Set-Content -LiteralPath $recp -NoNewline
        $r = Pg $v @("build")
        Check ($r.Rc -eq 0 -and $r.Out -match 'unreadable') "P40 review r3: a record with version 1.5 is unreadable, not version 1"
        $old = Join-Path $v "gen/a.ts.pgtmp.1.2.3"; $fresh = Join-Path $v "gen/a.ts.pgtmp.4.5.6"
        Set-Content -LiteralPath $old -Value "x"; Set-Content -LiteralPath $fresh -Value "y"
        (Get-Item $old).LastWriteTime = (Get-Date).AddHours(-1)
        Set-Content -LiteralPath (Join-Path $v "a.pg") -Value 'fn alpha(): i32 { return 7 }'
        Check ((Pg $v @("build")).Rc -eq 0 -and -not (Test-Path $old) -and (Test-Path $fresh)) "P40 review r3: an hour-old *.pgtmp.* is swept on the next write; a fresh one (a parallel build's) is kept"

        if ($IsWindows) {
            # Part 2: a reader holding the output without FILE_SHARE_DELETE — the write is retried, then FAILS (exit 1)
            # with the reader's bytes intact. It used to truncate and rewrite the file in place and exit 0.
            $h = New-Proj (Join-Path $work "rv3-held") '{"targets":["typescript"],"include":[{"pattern":"*.pg","output":"gen/%(Filename)"}]}' @{ "a.pg" = $fnA }
            Pg $h @("build") | Out-Null
            $twin = Join-Path $h "gen/a.ts"
            $before = Get-Content -Raw $twin
            Set-Content -LiteralPath (Join-Path $h "a.pg") -Value 'fn alpha(): i32 { return 99 }'
            $fs = [IO.File]::Open($twin, 'Open', 'Read', 'Read')
            try { $r = Pg $h @("build") } finally { $fs.Dispose() }
            Check ($r.Rc -ne 0 -and $r.Out -match 'cannot write' -and (Get-Content -Raw $twin) -ceq $before -and
                   @(Get-ChildItem (Join-Path $h "gen") -Filter '*.pgtmp.*').Count -eq 0) `
                "P40 review r3: a held output is retried then refused (exit $($r.Rc)), the reader's bytes intact, no temp left"
            Check ((Pg $h @("build")).Rc -eq 0 -and (Get-Content -Raw $twin) -match '99') "P40 review r3: ... and the next build, unheld, writes it"

            # Gates: owned-scope aliases through an 8.3 short name and a junction are refused (Windows-only shapes).
            $jroot = Join-Path $work "rv3-alias"
            $real = New-Proj (Join-Path $jroot "real/LongProjectFolder") '{"targets":["typescript"],"include":[{"pattern":"src/*.pg","output":"../../link/%(RecursiveDir)%(Filename)","owned":true}]}' @{ "src/a.pg" = $fnA; "keep/other.ts" = $hdr }
            New-Item -ItemType Junction -Path (Join-Path $jroot "link") -Target $real | Out-Null
            $r = Pg $real @("build")
            Check ($r.Rc -eq 64 -and $r.Out -match '"owned": true is refused' -and (Test-Path (Join-Path $real "keep/other.ts"))) "P40 review r3: an owned scope through a junction to the config folder is refused (64)"
            $short = (New-Object -ComObject Scripting.FileSystemObject).GetFolder($real).ShortName
            if ($short -and $short -cne 'LongProjectFolder') {
                Set-Content -LiteralPath (Join-Path $real "pgconfig.json") -Value ('{"targets":["typescript"],"include":[{"pattern":"src/*.pg","output":"../' + $short + '/%(RecursiveDir)%(Filename)","owned":true}]}')
                $r = Pg $real @("build")
                Check ($r.Rc -eq 64 -and $r.Out -match '"owned": true is refused' -and (Test-Path (Join-Path $real "keep/other.ts"))) "P40 review r3: an owned scope through the 8.3 name '$short' of the config folder is refused (64)"
            } else { Check $true "P40 review r3: 8.3 alias (skipped: this volume generates no short names)" }
        }
    }

    $g39Ok = (Test-G39 "counter" $false "counter" @("csharp", "typescript", "python", "php"))
    $g39Ok = (Test-G39 "modular" $true "modular" @("csharp", "typescript", "python", "php")) -and $g39Ok
    $g39Ok = (Test-G39 "operators_full" $false "operators_full" @("csharp", "typescript", "python")) -and $g39Ok
    Check $g39Ok "single config-sourced invocation is byte-identical to per-target runs (counter, modular, operators_full)"
}
finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
    if (Test-Path $work) { Write-Host "WARNING: could not remove CLI-smoke work dir '$work' — clean it up manually." }
}

Write-Host ""
if ($failures -eq 0) { Write-Host "CLI smoke: all checks passed."; exit 0 }
Write-Host "CLI smoke: $failures check(s) FAILED."
exit 1
