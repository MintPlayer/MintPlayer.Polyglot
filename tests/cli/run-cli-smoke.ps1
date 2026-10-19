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
            Check ($r.Rc -eq 3 -and $r.Out -cmatch 'would rename .*Foo\.ts -> foo\.ts' -and $r.Out -cmatch 'git -C "[A-Za-z]:/[^"]*/rv-a3/gen" mv -f -- "Foo\.ts" "foo\.ts"' -and ($names -join ',') -ceq 'Foo.ts') "P40 review A3: --check reports the case-only rename (exit 3) with the git mv remedy, and renames nothing"
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
        # P40 review r4: the REASON, not only the shared "owned: true is refused" prefix every refusal carries.
        Check ($r.Rc -eq 64 -and $r.Out -match '"owned": true is refused because its output .* is an absolute \(UNC\) path') "P40 review: an owned UNC template (\\server\share\...) is refused (64) as an absolute (UNC) path"
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
        # 5. P40 review r3 item 5: `check` AND `check --watch` gate every configured target, not the reference
        #    target only — an `expect fn` with no typescript `actual` refuses in both, naming 'typescript'.
        $ex = "expect fn sep(): string`nactual(csharp) fn sep(): string => `"/`"`nfn main() { let s = sep() }"
        $g = New-Proj (Join-Path $work "rv-check-gate") '{"targets":["csharp","typescript"]}' @{ "a.pg" = $ex }
        $r = Pg $g @("check", "a.pg")
        Check ($r.Rc -eq 1 -and $r.Out -match "'typescript'") "P40 r3: check refuses an expect fn with no actual for a configured target"
        $wlog = Join-Path $work "rv-check-watch.log"
        $proc = Start-Process -FilePath $Cli -ArgumentList @("check", "a.pg", "--watch") -WorkingDirectory $g `
            -RedirectStandardOutput $wlog -RedirectStandardError (Join-Path $work "rv-check-watch.err") -PassThru -NoNewWindow
        $deadline = (Get-Date).AddSeconds(20)
        while ((Get-Date) -lt $deadline -and -not ((Get-Content $wlog -Raw -ErrorAction SilentlyContinue) -match 'watching for changes')) { Start-Sleep -Milliseconds 200 }
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        $wout = Get-Content $wlog -Raw -ErrorAction SilentlyContinue
        Check ($wout -match "'typescript'" -and $wout -match '[1-9]\d* error\(s\) - watching') `
            "P40 r3: check --watch uses the same configured-targets gate as check"
        Check (-not (Test-Path (Join-Path $g "a.ts")) -and -not (Test-Path (Join-Path $g "a.cs"))) "P40 r3: check --watch writes nothing"
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
        # P40 review r4: a non-ASCII SOURCE name too (`日.pg`), so the record (relative inputs/outputs) and that file's
        # source map (`file`, `sources`) hold bytes >= 0x80 — with ASCII-only content their strict decode could not fail.
        $ri = "$([char]0x65E5)"
        $u = New-Proj (Join-Path $work "rv3-utf8/p$([char]0xE9)$([char]0x65E5)") '{"targets":["csharp","typescript"],"originInfo":true,"include":[{"pattern":"*.pg","target":"typescript","output":"gen/%(Filename)"}]}' @{ "a.pg" = $fnA; "b.pg" = "import { alpha } from `"./a`"`nfn b(): i32 { return alpha() }"; "$ri.pg" = 'fn ri(): i32 { return 3 }' }
        $r = Pg $u @("build", "a.pg", "b.pg", "$ri.pg", "--out", "obj", "--record", "obj/rec.json", "--depfile", "obj/deps", "--outputs-list", "obj/list")
        $list = ReadUtf8Strict (Join-Path $u "obj/list")
        $deps = ReadUtf8Strict (Join-Path $u "obj/deps")
        $listOk = $null -ne $list -and $list.Contains((Join-Path $u "obj\a.cs")) -and $list.Contains((Join-Path $u "gen\a.ts")) -and
                  @($list.Trim() -split "`n" | Where-Object { -not (Test-Path -LiteralPath $_.Trim()) }).Count -eq 0
        $depsOk = $null -ne $deps -and $deps.Contains((Join-Path $u "pgconfig.json")) -and
                  @($deps.Trim() -split "`n" | Where-Object { -not (Test-Path -LiteralPath $_.Trim()) }).Count -eq 0
        Check ($r.Rc -eq 0 -and $listOk -and $depsOk) "P40 review r3 (gating 1): under 'p$([char]0xE9)$([char]0x65E5)' the outputs list and the depfile are UTF-8 and every line names an existing file"
        # A failed strict decode returns $null, and `$null | ConvertFrom-Json` does NOT throw — so test for it first.
        $recTxt = ReadUtf8Strict (Join-Path $u "obj/rec.json")
        $recOk = $false; try { $recOk = $null -ne $recTxt -and $recTxt.Contains("$ri.pg") -and $null -ne ($recTxt | ConvertFrom-Json) } catch {}
        $mapTxt = ReadUtf8Strict (Join-Path $u "gen/$ri.ts.map")
        $mapOk = $false; try { $m = $mapTxt | ConvertFrom-Json; $mapOk = $null -ne $mapTxt -and $m.file -ceq "$ri.ts" -and ($m.sources -join ',') -ceq "../$ri.pg" } catch {}
        Check ($recOk -and $mapOk) "P40 review r3 (gating 1): ... and the record and the source map (holding '$ri') read back as strict UTF-8 JSON"
        $r = Pg $u @("build", "a.pg", "b.pg", "$ri.pg", "--out", "obj", "--record", "obj/rec.json", "--depfile", "obj/deps", "--outputs-list", "obj/list", "--check")
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
            Check ($r.Rc -eq 0 -and ($names -join ',') -ceq 'foo.ts,foo.ts.map' -and $r.Out -cmatch 'git -C "[^"]*" mv -f --') `
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
        # P40 review r4 (gating 1): only an output's OWN temp (exact shape), older than 10 minutes, AND carrying the
        # header line is swept. A headerless one, a wrong shape, or another output's name is never touched.
        $old = Join-Path $v "gen/a.ts.pgtmp.1.2.3"; $fresh = Join-Path $v "gen/a.ts.pgtmp.4.5.6"
        $bare = Join-Path $v "gen/a.ts.pgtmp.7.8.9"; $shape = Join-Path $v "gen/a.ts.pgtmp.1.2"; $other = Join-Path $v "gen/zz.ts.pgtmp.1.2.3"
        $caseTmp = Join-Path $v "gen/a.ts.pgcase.tmp"
        foreach ($f in @($old, $fresh, $shape, $other, $caseTmp)) { Set-Content -LiteralPath $f -Value $hdr }
        Set-Content -LiteralPath $bare -Value "x"
        foreach ($f in @($old, $bare, $shape, $other, $caseTmp)) { (Get-Item -LiteralPath $f).LastWriteTime = (Get-Date).AddHours(-1) }
        Set-Content -LiteralPath (Join-Path $v "a.pg") -Value 'fn alpha(): i32 { return 7 }'
        Check ((Pg $v @("build")).Rc -eq 0 -and -not (Test-Path $old) -and -not (Test-Path $caseTmp) -and (Test-Path $fresh) -and (Test-Path $bare) -and (Test-Path $shape) -and (Test-Path $other)) `
            "P40 review r4: hour-old headed a.ts.pgtmp.<n>.<n>.<n> and a.ts.pgcase.tmp are swept; a fresh one, a headerless one, a wrong shape and another output's are kept"

        if ($IsWindows) {
            # Part 2: a reader holding the output without FILE_SHARE_DELETE — the write is retried, then FAILS (exit 1)
            # with the reader's bytes intact. It used to truncate and rewrite the file in place and exit 0.
            $h = New-Proj (Join-Path $work "rv3-held") '{"targets":["typescript"],"include":[{"pattern":"*.pg","output":"gen/%(Filename)"}]}' @{ "a.pg" = $fnA }
            Pg $h @("build") | Out-Null
            $twin = Join-Path $h "gen/a.ts"
            $before = Get-Content -Raw $twin
            Set-Content -LiteralPath (Join-Path $h "a.pg") -Value 'fn alpha(): i32 { return 99 }'
            # P40 review r4: held with FileShare.ReadWrite (what MSVC's ifstream shares) — FileShare.Read also blocked the
            # OLD truncate-and-write fallback, so it could not tell the two apart — and the HOLDER's bytes are asserted.
            function HeldRead($stream) { $stream.Position = 0; $buf = New-Object byte[] 65536; $n = $stream.Read($buf, 0, $buf.Length); [Text.Encoding]::UTF8.GetString($buf, 0, $n) }
            $fs = [IO.File]::Open($twin, 'Open', 'Read', 'ReadWrite')
            try { $r = Pg $h @("build"); $holderSaw = HeldRead $fs } finally { $fs.Dispose() }
            Check ($r.Rc -ne 0 -and $r.Out -match 'cannot write' -and (Get-Content -Raw $twin) -ceq $before -and $holderSaw -ceq $before -and
                   @(Get-ChildItem (Join-Path $h "gen") -Filter '*.pgtmp.*').Count -eq 0) `
                "P40 review r4: an output held with FileShare.ReadWrite is retried then refused (exit $($r.Rc)), the holder still reads the old bytes, no temp left"
            Check ((Pg $h @("build")).Rc -eq 0 -and (Get-Content -Raw $twin) -match '99') "P40 review r3: ... and the next build, unheld, writes it"
            # P40 review r4: a reader that shares DELETE (FileShare.ReadWrite|Delete) no longer blocks the write: the POSIX-
            # semantics rename replaces the file; the holder keeps reading the old bytes, new readers see the new ones.
            $before = Get-Content -Raw $twin
            Set-Content -LiteralPath (Join-Path $h "a.pg") -Value 'fn alpha(): i32 { return 123 }'
            $fs = [IO.File]::Open($twin, 'Open', 'Read', [IO.FileShare]'ReadWrite, Delete')
            $sw = [Diagnostics.Stopwatch]::StartNew()
            try { $r = Pg $h @("build"); $ms = $sw.ElapsedMilliseconds; $holderSaw = HeldRead $fs } finally { $fs.Dispose() }
            Check ($r.Rc -eq 0 -and (Get-Content -Raw $twin) -match '123' -and $holderSaw -ceq $before -and $ms -lt 3000) `
                "P40 review r4: an output held with FileShare.ReadWrite|Delete is replaced at once (exit $($r.Rc), $ms ms; the holder keeps the old bytes)"

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

    # -- P40 review r4 (#75): the CLI half of round 4 -----------------------------------------------------------
    & {
        $hdr = "// <auto-generated/>`n// Generated by Polyglot. Do not edit: regenerate with ``polyglot build``.`nexport const dummy = 1;"
        $caseInsensitive = $false
        $probe = New-Proj (Join-Path $work "rv4-ci-probe") $null @{ "Probe.txt" = "x" }
        if (Test-Path (Join-Path $probe "PROBE.TXT")) { $caseInsensitive = $true }

        # Gating 1: the stale-temp sweep never deletes a headerless file, a `.pgtmp.` source, or anything next to
        # a `--target`/default-sink output that is not that output's own headed temp (Spark: sweep.sh, r1\c6).
        $s = New-Proj (Join-Path $work "rv4-sweep") $null @{ "src/a.pg" = $fnA; "src/keep.pgtmp.txt" = "my notes, not generated"; "src/plan.pgcase.tmp" = "my plan, not generated"; "src/notes.pgtmp.pg" = $fnB }
        foreach ($f in @("src/keep.pgtmp.txt", "src/plan.pgcase.tmp", "src/notes.pgtmp.pg")) { (Get-Item -LiteralPath (Join-Path $s $f)).LastWriteTime = (Get-Date).AddHours(-2) }
        $r = Pg $s @("build", "src/a.pg", "--target", "csharp")
        Check ($r.Rc -eq 0 -and (Test-Path (Join-Path $s "src/keep.pgtmp.txt")) -and (Test-Path (Join-Path $s "src/plan.pgcase.tmp"))) `
            "P40 review r4 (gating 1): build --target keeps 2 h old headerless src/keep.pgtmp.txt and src/plan.pgcase.tmp"
        $r = Pg $s @("build", "src/notes.pgtmp.pg", "--target", "csharp")
        Check ($r.Rc -eq 0 -and (Test-Path (Join-Path $s "src/notes.pgtmp.pg")) -and (Test-Path (Join-Path $s "src/notes.pgtmp.cs"))) `
            "P40 review r4 (gating 1): building src/notes.pgtmp.pg never deletes the source itself"

        # Gating 2: a case-only rename of a DIRECTORY component the route creates (Spark: a3d.sh, r1\c2 G2/G3).
        if ($caseInsensitive) {
            $d = New-Proj (Join-Path $work "rv4-a3d") '{"targets":["typescript"],"include":[{"pattern":"src/**/*.pg","output":"gen/%(RecursiveDir)%(Filename)","owned":true}]}' @{ "src/Foo/a.pg" = $fnA }
            Check ((Pg $d @("build")).Rc -eq 0 -and (@(Get-ChildItem (Join-Path $d "gen") -Name) -join ',') -ceq 'Foo') "P40 review r4 A3d: build writes gen/Foo/a.ts"
            Move-Item (Join-Path $d "src/Foo") (Join-Path $d "src/tmpdir"); Move-Item (Join-Path $d "src/tmpdir") (Join-Path $d "src/foo")
            $before = Snapshot $d
            $r = Pg $d @("build", "--check")
            # (A directory's git remedy restages the index: `git mv -f gen/Foo gen/foo` exits 128 on Windows for a
            # case-only DIRECTORY rename, before or after the disk rename.)
            Check ($r.Rc -eq 3 -and $r.Out -cmatch 'would rename .*gen[\\/]Foo -> foo' -and $r.Out -cmatch 'git -C "[A-Za-z]:/[^"]*/rv4-a3d/gen" rm -r -q --cached -- "Foo" && git -C "[A-Za-z]:/[^"]*/rv4-a3d/gen" add -- "foo"' -and (Snapshot $d) -ceq $before) `
                "P40 review r4 A3d: --check reports the directory case rename with the git restage hint (exit $($r.Rc); was 0), touching nothing"
            $r = Pg $d @("build")
            Check ($r.Rc -eq 0 -and (@(Get-ChildItem (Join-Path $d "gen") -Name) -join ',') -ceq 'foo' -and (Test-Path (Join-Path $d "gen/foo/a.ts")) -and $r.Out -cmatch 'rm -r -q --cached -- "Foo"') `
                "P40 review r4 A3d: build renames gen/Foo to gen/foo and names the git remedy (exit $($r.Rc))"
            Check ((Pg $d @("build", "--check")).Rc -eq 0) "P40 review r4 A3d: ... after which --check is clean"
            # G3: the output template's own spelling changes (Gen/ -> gen/).
            $g = New-Proj (Join-Path $work "rv4-g3") '{"targets":["typescript"],"include":[{"pattern":"*.pg","output":"Gen/%(Filename)","owned":true}]}' @{ "a.pg" = $fnA }
            Pg $g @("build") | Out-Null
            Set-Content -LiteralPath (Join-Path $g "pgconfig.json") -Value '{"targets":["typescript"],"include":[{"pattern":"*.pg","output":"gen/%(Filename)","owned":true}]}'
            $r = Pg $g @("build", "--check")
            Check ($r.Rc -eq 3 -and $r.Out -cmatch 'would rename .*Gen -> gen') "P40 review r4 G3: --check reports Gen -> gen (exit $($r.Rc))"
            $r = Pg $g @("build")
            Check ($r.Rc -eq 0 -and (@(Get-ChildItem $g -Directory -Name | Where-Object { $_ -ieq 'gen' }) -join ',') -ceq 'gen') "P40 review r4 G3: build renames Gen/ to gen/ (exit $($r.Rc))"
            # A directory holding a file that is NOT Polyglot output is never renamed: refused, with the hint.
            $x = New-Proj (Join-Path $work "rv4-a3d-foreign") '{"targets":["typescript"],"include":[{"pattern":"*.pg","output":"Gen/%(Filename)"}]}' @{ "a.pg" = $fnA }
            Pg $x @("build") | Out-Null
            Set-Content -LiteralPath (Join-Path $x "Gen/notes.txt") -Value "mine"
            Set-Content -LiteralPath (Join-Path $x "pgconfig.json") -Value '{"targets":["typescript"],"include":[{"pattern":"*.pg","output":"gen/%(Filename)"}]}'
            $r = Pg $x @("build")
            Check ($r.Rc -eq 1 -and $r.Out -match 'not Polyglot output' -and $r.Out -match 'rm -r -q --cached -- "Gen"' -and (@(Get-ChildItem $x -Directory -Name | Where-Object { $_ -ieq 'gen' }) -join ',') -ceq 'Gen') `
                "P40 review r4 A3d: a directory with a foreign file is not renamed; the build refuses with the hint (exit $($r.Rc))"
            # The git remedy, run verbatim, fixes the commit (a directory rename on a core.ignorecase checkout).
            if (Get-Command git -ErrorAction SilentlyContinue) {
                $gq = @("-c", "user.name=gate", "-c", "user.email=gate@example.invalid", "-c", "commit.gpgsign=false", "-c", "core.hooksPath=", "-c", "core.autocrlf=false")
                $gd = New-Proj (Join-Path $work "rv4-a3d-git") '{"targets":["typescript"],"include":[{"pattern":"src/**/*.pg","output":"gen/%(RecursiveDir)%(Filename)","owned":true}]}' @{ "src/Models/x.pg" = $fnA }
                Pg $gd @("build") | Out-Null
                Set-Content -LiteralPath (Join-Path $gd ".gitignore") -Value ".polyglot/"
                & git @gq -C $gd init -q 2>&1 | Out-Null; & git @gq -C $gd add -A 2>&1 | Out-Null; & git @gq -C $gd commit -q -m one 2>&1 | Out-Null
                & git @gq -C $gd mv src/Models src/m_tmp 2>&1 | Out-Null; & git @gq -C $gd mv src/m_tmp src/models 2>&1 | Out-Null
                $r = Pg $gd @("build")
                $m = [regex]::Match($r.Out, 'git -C "[^"]*" rm -r -q --cached -- "[^"]*" && git -C "[^"]*" add -- "[^"]*"')
                $hint = $m.Value
                Push-Location $gd
                try {
                    if ($m.Success) { Invoke-Expression "& { $hint } 2>&1 | Out-Null" } # verbatim (review r7: it is cwd-independent)
                    & git @gq add -A 2>&1 | Out-Null; & git @gq commit -q -m two 2>&1 | Out-Null
                    $files = (& git @gq ls-files 2>&1 | Out-String)
                } finally { Pop-Location }
                Check ($hint -and $files -cmatch 'gen/models/x\.ts' -and $files -cnotmatch 'gen/Models/') "P40 review r4 A3d: the printed '$hint' fixes the committed directory case"
            }
        } else { Check $true "P40 review r4 A3d/G3 (skipped: case-sensitive filesystem)" }

        # Part 2: no git hint for a build-owned (obj/) output — those are never tracked.
        if ($caseInsensitive) {
            $o = New-Proj (Join-Path $work "rv4-obj-hint") '{"targets":["csharp"]}' @{ "Fold.pg" = $fnA }
            $bargs = @("--out", "obj/polyglot", "--record", "obj/rec.json", "--build-owned", "obj/polyglot")
            Pg $o (@("build", "Fold.pg") + $bargs) | Out-Null
            Move-Item (Join-Path $o "Fold.pg") (Join-Path $o "x.tmp"); Move-Item (Join-Path $o "x.tmp") (Join-Path $o "fold.pg")
            $r = Pg $o (@("build", "fold.pg") + $bargs)
            Check ($r.Rc -eq 0 -and $r.Out -match 'case-only rename' -and $r.Out -notmatch 'mv -f') "P40 review r4: a build-owned obj/ output is case-renamed without a git mv hint"
        }

        # Part 2: with a record present the refusal names the real remedy, and --adopt never takes over — not with
        # explicit inputs without --record, and not under --watch.
        $a = New-Proj (Join-Path $work "rv4-adopt") '{"targets":["typescript"],"include":[{"pattern":"*.pg","output":"gen/%(Filename)"}]}' @{ "a.pg" = $fnA }
        Pg $a @("build") | Out-Null
        Set-Content -LiteralPath (Join-Path $a "b.pg") -Value $fnB
        Set-Content -LiteralPath (Join-Path $a "gen/b.ts") -Value "export const handWritten = 1;"
        $r = Pg $a @("build")
        Check ($r.Rc -eq 1 -and $r.Out -match 'delete the outputs record' -and $r.Out -notmatch 'pass --adopt once') "P40 review r4: with a record, the overwrite refusal names deleting the record, not --adopt"
        $r = Pg $a @("build", "b.pg", "--adopt")
        Check ($r.Rc -eq 1 -and $r.Out -match '--adopt is ignored' -and (Get-Content -Raw (Join-Path $a "gen/b.ts")) -match 'handWritten') `
            "P40 review r4: explicit inputs without --record do not adopt over an existing record (exit $($r.Rc))"
        $wlog = Join-Path $work "rv4-adopt-watch.log"
        $proc = Start-Process -FilePath $Cli -ArgumentList @("build", "b.pg", "--watch", "--adopt") -WorkingDirectory $a `
            -RedirectStandardOutput $wlog -RedirectStandardError (Join-Path $work "rv4-adopt-watch.err") -PassThru -NoNewWindow
        $deadline = (Get-Date).AddSeconds(20)
        while ((Get-Date) -lt $deadline -and -not ((Get-Content $wlog -Raw -ErrorAction SilentlyContinue) -match 'watching for changes')) { Start-Sleep -Milliseconds 200 }
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        Check ((Get-Content -Raw (Join-Path $a "gen/b.ts")) -match 'handWritten' -and (Get-Content $wlog -Raw) -match '--adopt is ignored') "P40 review r4: --watch --adopt does not take over while a record exists"

        # Part 2: offline `check` degrades ONLY the targets an uncached registry dependency provides.
        $oldCache = $env:POLYGLOT_CACHE
        $env:POLYGLOT_CACHE = Join-Path $work "rv4-offline-cache"
        try {
            $q = New-Proj (Join-Path $work "rv4-offline-typo") '{"targets":["csharpp"],"dependencies":{"nosuch":"^1.0.0"}}' @{ "a.pg" = $fnA }
            $r1 = Pg $q @("check", "a.pg")
            $q2 = New-Proj (Join-Path $work "rv4-offline-ok") '{"targets":["csharp","nosuch"],"dependencies":{"nosuch":"^1.0.0"}}' @{ "a.pg" = $fnA }
            $r2 = Pg $q2 @("check", "a.pg")
            $q3 = New-Proj (Join-Path $work "rv4-offline-file") '{"targets":["csharp","broken"],"dependencies":{"nosuch":"^1.0.0","broken":"file:./nope"}}' @{ "a.pg" = $fnA }
            $r3 = Pg $q3 @("check", "a.pg")
        } finally { $env:POLYGLOT_CACHE = $oldCache }
        Check ($r1.Rc -eq 64 -and $r1.Out -match 'csharpp') "P40 review r4: offline check refuses a misspelled target beside an uncached registry dependency (exit $($r1.Rc); was 0)"
        Check ($r2.Rc -eq 0 -and $r2.Out -match "note: target\(s\) nosuch not verified") "P40 review r4: ... and degrades exactly the uncached dependency's target (exit $($r2.Rc))"
        Check ($r3.Rc -eq 64) "P40 review r4: ... and a broken file: plugin beside it is still an error (exit $($r3.Rc))"

        # Part 2: an include rule's "target" names a target; --target "" is a usage error.
        foreach ($t in @('" "', '""', '"typescrpt"')) {
            $p = New-Proj (Join-Path $work ("rv4-ruletarget-" + [guid]::NewGuid().ToString('N').Substring(0, 6))) ('{"targets":["typescript"],"include":[{"pattern":"*.pg","target":' + $t + ',"output":"gen/%(Filename)"}]}') @{ "a.pg" = $fnA }
            $r = Pg $p @("build")
            Check ($r.Rc -eq 64 -and -not (Test-Path (Join-Path $p "a.ts"))) "P40 review r4: an include rule with target $t is refused (exit $($r.Rc)), nothing written next to the .pg"
        }
        $p = New-Proj (Join-Path $work "rv4-target-empty") '{"targets":["csharp","typescript"]}' @{ "a.pg" = $fnA }
        $r = Pg $p @("build", "a.pg", "--target", "")
        Check ($r.Rc -eq 64 -and -not (Test-Path (Join-Path $p "a.cs"))) "P40 review r4: --target `"`" is exit 64, not every target (exit $($r.Rc))"

        # Part 2: a pgconfig.json that is not UTF-8 is refused by name (64); a record with a raw 0xE9 is unreadable.
        $p = New-Proj (Join-Path $work "rv4-cp1252") $null @{ "a.pg" = $fnA }
        [IO.File]::WriteAllBytes((Join-Path $p "pgconfig.json"), [byte[]](([Text.Encoding]::ASCII.GetBytes('{"targets":["typescript"],"include":[{"pattern":"*.pg","output":"g')) + 0xE9 + [Text.Encoding]::ASCII.GetBytes('n/%(Filename)"}]}')))
        $r = Pg $p @("build")
        Check ($r.Rc -eq 64 -and $r.Out -match 'pgconfig\.json: malformed JSON: invalid UTF-8') "P40 review r4: a CP1252 pgconfig.json exits 64 naming the file (exit $($r.Rc); was 1)"
        $p = New-Proj (Join-Path $work "rv4-rec-e9") '{"targets":["typescript"],"include":[{"pattern":"*.pg","output":"gen/%(Filename)"}]}' @{ "a.pg" = $fnA }
        Pg $p @("build") | Out-Null
        $recp = Join-Path $p ".polyglot/outputs.json"
        $bytes = [IO.File]::ReadAllBytes($recp); $i = [Array]::IndexOf($bytes, [byte][char]'a'); $bytes[$i] = 0xE9; [IO.File]::WriteAllBytes($recp, $bytes)
        $r = Pg $p @("build")
        Check ($r.Rc -eq 0 -and $r.Out -match 'outputs record .* is unreadable') "P40 review r4: a record holding a raw 0xE9 is reported unreadable by name, never a crash (exit $($r.Rc))"

        # Part 2: an owned folder INSIDE a nested project (src/sub/gen with src/sub/pgconfig.json) is that project's.
        $n = New-Proj (Join-Path $work "rv4-nested-owned") '{"targets":["typescript"],"include":[{"pattern":"src/*.pg","output":"src/sub/gen/%(Filename)","owned":true}]}' @{ "src/a.pg" = $fnA }
        New-Proj (Join-Path $n "src/sub") '{"targets":["typescript"],"include":[{"pattern":"*.pg","output":"gen/%(Filename)"}]}' @{ "x.pg" = $fnB } | Out-Null
        Pg (Join-Path $n "src/sub") @("build") | Out-Null
        $r = Pg $n @("build", "--check")
        Check ($r.Out -notmatch 'would delete' -and (Test-Path (Join-Path $n "src/sub/gen/x.ts"))) "P40 review r4: the outer owned scan never reports the nested project's src/sub/gen/x.ts (exit $($r.Rc))"

        # Part 2: pgconfig.lock.json is strict — a duplicate key is refused (64), never the first value.
        $l = New-Proj (Join-Path $work "rv4-lock") '{"targets":["csharp"],"dependencies":{"csharp":"*"}}' @{ "a.pg" = $fnA }
        Set-Content -LiteralPath (Join-Path $l "pgconfig.lock.json") -Value '{"lockfileVersion":1,"packages":{"@x/p":{"version":"1.0.0","resolved":"u","integrity":"sha512-a","integrity":"sha512-b"}}}'
        $r = Pg $l @("build", "a.pg")
        Check ($r.Rc -eq 64 -and $r.Out -match 'pgconfig\.lock\.json: malformed JSON: duplicate key') "P40 review r4: a lockfile with a duplicate key is refused by name (exit $($r.Rc))"

        # ---- Review round 5 — CLI half --------------------------------------------------------------------------
        # Gating 3 (Spark r1\rt.sh): an include rule's target that only an uncached registry dependency provides (and
        # `targets` omits) is "not verified" — offline check AND a build that cannot reach the registry exit 0.
        $oldCache = $env:POLYGLOT_CACHE; $oldReg = $env:POLYGLOT_REGISTRY
        $env:POLYGLOT_CACHE = Join-Path $work "rv5-rt-cache"; $env:POLYGLOT_REGISTRY = "http://127.0.0.1:9/"
        try {
            foreach ($cfg in @('{"targets":["csharp"],"dependencies":{"nosuch":"^1.0.0"},"include":[{"pattern":"*.pg","target":"nosuch","output":"gen/%(Filename)"}]}',
                               '{"targets":["csharp"],"dependencies":{"@acme/plugin":"^1.0.0"},"include":[{"pattern":"*.pg","target":"acme","output":"gen/%(Filename)"}]}')) {
                $q = New-Proj (Join-Path $work ("rv5-rt-" + [guid]::NewGuid().ToString('N').Substring(0, 6))) $cfg @{ "a.pg" = $fnA }
                $rc1 = Pg $q @("check", "a.pg")
                $rb = Pg $q @("build")
                Check ($rc1.Rc -eq 0 -and $rc1.Out -match 'include rule target\(s\) \w+ not verified' -and $rb.Rc -eq 0 -and (Test-Path (Join-Path $q "a.cs"))) `
                    "review r5 (gating 3): an uncached dependency's rule target is not verified; offline check $($rc1.Rc), offline build $($rb.Rc) (was 64, 64), a.cs written"
                # review r6: once per build — discovery validated the config, then the group validated it again.
                Check (([regex]::Matches($rb.Out, 'not verified')).Count -eq 1 -and ([regex]::Matches($rb.Out, '127\.0\.0\.1:9/')).Count -eq 1) `
                    "review r6: the build prints the 'not verified' note and the dependency error once ($([regex]::Matches($rb.Out, 'not verified').Count) note(s); was 2)"
            }
            # ... but a misspelled rule target beside the uncached dependency is still unknown.
            $q = New-Proj (Join-Path $work "rv5-rt-typo") '{"targets":["csharp"],"dependencies":{"nosuch":"^1.0.0"},"include":[{"pattern":"*.pg","target":"typo","output":"gen/%(Filename)"}]}' @{ "a.pg" = $fnA }
            $r = Pg $q @("check", "a.pg")
            Check ($r.Rc -eq 64 -and $r.Out -match "unknown target 'typo'") "review r5 (gating 3): a misspelled rule target beside an uncached first-party dependency is still refused (exit $($r.Rc))"
        } finally { $env:POLYGLOT_CACHE = $oldCache; $env:POLYGLOT_REGISTRY = $oldReg }

        # Part 2 (r5) + review r6 (Spark p2\rtv.sh R1): an include rule naming a bundled target `targets` omits applies
        # only to `--target python` runs — a WARNING saying so, never a refusal of the runs that do not build python.
        $p = New-Proj (Join-Path $work "rv5-rule-unconfigured") '{"targets":["typescript"],"include":[{"pattern":"*.pg","target":"python","output":"gen/%(Filename)"}]}' @{ "a.pg" = $fnA }
        $warnRx = "warning: pgconfig\.json include rule '\*\.pg' targets 'python', which no configured target or dependency provides; it applies only to runs with --target python"
        foreach ($av in @(@("build"), @("build", "--only", "typescript"), @("build", "a.pg", "--target", "csharp"), @("build", "--check"), @("check", "a.pg"))) {
            $r = Pg $p $av
            Check ($r.Rc -eq 0 -and $r.Out -match $warnRx -and ([regex]::Matches($r.Out, 'warning: pgconfig')).Count -eq 1) "review r6: '$($av -join ' ')' beside a rule naming the unconfigured 'python' warns once and runs (exit $($r.Rc); r5 refused it with 64)"
        }
        Check ((Test-Path (Join-Path $p "a.ts")) -and -not (Test-Path (Join-Path $p "gen"))) "review r6: ... the typescript build wrote a.ts (no rule applies to it), nothing under gen/"
        $r = Pg $p @("build", "a.pg", "--target", "python")
        Check ($r.Rc -eq 0 -and (Test-Path (Join-Path $p "gen/a.py")) -and $r.Out -notmatch 'warning: pgconfig') "review r5: ... while --target python builds it through the rule, without the warning (exit $($r.Rc))"
        # ... and `"target": []` is refused like `"targets": []` (it used to mean "every target").
        $p = New-Proj (Join-Path $work "rv5-rule-empty") '{"targets":["typescript"],"include":[{"pattern":"*.pg","target":[],"output":"gen/%(Filename)"}]}' @{ "a.pg" = $fnA }
        $r = Pg $p @("build")
        Check ($r.Rc -eq 64 -and $r.Out -match '"target" is an empty list' -and -not (Test-Path (Join-Path $p "gen"))) "review r5: an include rule with `"target`": [] is refused (exit $($r.Rc); was 0)"

        # Part 2: a failing build deletes nothing — not even an hour-old headed temp of an output it rewrote.
        $sw = New-Proj (Join-Path $work "rv5-sweep-fail") '{"targets":["csharp"],"include":[{"pattern":"*.pg","output":"gen/%(Filename)"}]}' @{ "a.pg" = $fnA }
        Pg $sw @("build") | Out-Null
        $tmp = Join-Path $sw "gen/a.cs.pgtmp.1.2.3"
        Copy-Item (Join-Path $sw "gen/a.cs") $tmp; (Get-Item -LiteralPath $tmp).LastWriteTime = (Get-Date).AddHours(-1)
        Set-Content -LiteralPath (Join-Path $sw "b.pg") -Value 'fn b(): i32 { return nope }'
        $r = Pg $sw @("build")
        Check ($r.Rc -eq 1 -and (Test-Path $tmp)) "review r5: an exit-1 build keeps the stale temp (exit $($r.Rc); it used to be swept)"
        Remove-Item -LiteralPath (Join-Path $sw "b.pg")
        $r = Pg $sw @("build")
        Check ($r.Rc -eq 0 -and -not (Test-Path $tmp)) "review r5: ... and the next clean build sweeps it (exit $($r.Rc))"

        # ---- Review round 6 — CLI half --------------------------------------------------------------------------
        # Gating 2 (Spark p2\surr.sh): a lone \u surrogate in an output template is refused by name (64) on EVERY
        # command form. Explicit-input builds used to compute the route roots from the partial document before the
        # config was validated, and died on the undecodable path (exit 1, a raw OS error 1113 naming no file).
        foreach ($tmplOut in @('g\ud800n/%(Filename)', '../w\ud800b/%(Filename)', 'gen/%(Filename)\udc00')) {
            $p = New-Proj (Join-Path $work ("rv6-surr-" + [guid]::NewGuid().ToString('N').Substring(0, 6))) ('{"targets":["csharp"],"include":[{"pattern":"*.pg","output":"' + $tmplOut + '"}]}') @{ "a.pg" = $fnA }
            foreach ($av in @(@("build"), @("build", "a.pg"), @("build", "a.pg", "--record", "obj/r.json"), @("build", "a.pg", "--target", "csharp", "--out", "o"), @("check", "a.pg"))) {
                $r = Pg $p $av
                Check ($r.Rc -eq 64 -and $r.Out -match 'pgconfig\.json: malformed JSON: lone \\u surrogate' -and @(Get-ChildItem $p -Name).Count -eq 2) `
                    "review r6 (gating 2): output '$tmplOut', '$($av -join ' ')' exits 64 naming the malformed pgconfig.json, nothing written (exit $($r.Rc); was 1)"
            }
        }

        # Part 2 (Spark p2\swf.sh F4/F5): the stale-temp sweep runs LAST — a build whose depfile or outputs list cannot
        # be written exits 1 and keeps the temp; the next fully successful build sweeps it.
        foreach ($case in @(@{ n = "depfile"; flag = "--depfile"; msg = 'cannot write the depfile' }, @{ n = "list"; flag = "--outputs-list"; msg = 'cannot write the outputs list' })) {
            $sw = New-Proj (Join-Path $work "rv6-sweep-$($case.n)") '{"targets":["csharp"],"include":[{"pattern":"*.pg","output":"gen/%(Filename)"}]}' @{ "a.pg" = $fnA }
            Pg $sw @("build", "a.pg", "--record", "obj/rec.json") | Out-Null
            $tmp = Join-Path $sw "gen/a.cs.pgtmp.1.2.3"
            Copy-Item (Join-Path $sw "gen/a.cs") $tmp; (Get-Item -LiteralPath $tmp).LastWriteTime = (Get-Date).AddHours(-1)
            New-Item -ItemType Directory -Force (Join-Path $sw "obj/blocked") | Out-Null # a directory: the write fails
            $r = Pg $sw @("build", "a.pg", "--record", "obj/rec.json", $case.flag, "obj/blocked")
            Check ($r.Rc -eq 1 -and $r.Out -match $case.msg -and (Test-Path $tmp)) "review r6: a failed $($case.flag) write exits 1 and keeps the stale temp (exit $($r.Rc); it was swept first)"
            $r = Pg $sw @("build", "a.pg", "--record", "obj/rec.json", $case.flag, "obj/ok.txt")
            Check ($r.Rc -eq 0 -and -not (Test-Path $tmp)) "review r6: ... and the next successful build sweeps it (exit $($r.Rc))"
        }

        # Gating 2 (Spark r1\h1.sh): two spellings of one directory in ONE run (gen/ and Gen/) — a conflict on the
        # FIRST run, naming both spellings, under --check too; one spelling twice is fine.
        $h = New-Proj (Join-Path $work "rv5-h1") '{"targets":["typescript"],"include":[{"pattern":"a.pg","output":"gen/%(Filename)"},{"pattern":"b.pg","output":"Gen/%(Filename)"}]}' @{ "a.pg" = $fnA; "b.pg" = $fnB }
        $rc = Pg $h @("build", "--check")
        $rb = Pg $h @("build")
        Check ($rc.Rc -eq 1 -and $rb.Rc -eq 1 -and $rb.Out -match "conflicting output spellings: .*'gen' and 'Gen' differ only by case" -and $rb.Out -notmatch 'different content') `
            "review r5 (gating 2): gen/ and Gen/ in one run conflict on the first run (check $($rc.Rc), build $($rb.Rc); was 3, 0), naming both spellings"
        $h = New-Proj (Join-Path $work "rv5-h1-file") '{"targets":["typescript"],"include":[{"pattern":"x/*.pg","output":"gen/%(Filename)"},{"pattern":"y/*.pg","output":"gen/%(Filename)"}]}' @{ "x/a.pg" = $fnA; "y/A.pg" = $fnB }
        $r = Pg $h @("build")
        Check ($r.Rc -eq 1 -and $r.Out -match "'a\.ts' and 'A\.ts' differ only by case") "review r5 (gating 2): gen/a.ts and gen/A.ts in one run conflict by spelling (exit $($r.Rc))"
        $h = New-Proj (Join-Path $work "rv5-h1-same") '{"targets":["typescript"],"include":[{"pattern":"a.pg","output":"gen/%(Filename)"},{"pattern":"b.pg","output":"gen/%(Filename)"}]}' @{ "a.pg" = $fnA; "b.pg" = $fnB }
        Check ((Pg $h @("build")).Rc -eq 0) "review r5 (gating 2): one directory spelling routed from two rules is accepted"

        # review r7 (Spark p2\mg2.sh): a multi-group build without --out puts each group's unrouted output next to
        # ITS OWN input — it used to land next to the run's first input (g2's c.cs in g1/, a false conflict with
        # g1/c.pg). Within one group the sink stays the group's first input's folder (one project, one C# prelude).
        $mg = Join-Path $work "rv7-mg"
        New-Proj $mg $null @{ "g1/pgconfig.json" = '{"targets":["csharp"],"include":[{"pattern":"*.pg","output":"gen/%(Filename)"}]}'; "g1/a.pg" = $fnA
                             "g2/pgconfig.json" = '{"targets":["csharp"]}'; "g2/c.pg" = $fnB } | Out-Null
        $r = Pg $mg @("build", "g1/a.pg", "g2/c.pg")
        Check ($r.Rc -eq 0 -and (Test-Path (Join-Path $mg "g1/gen/a.cs")) -and (Test-Path (Join-Path $mg "g2/c.cs")) -and -not (Test-Path (Join-Path $mg "g1/c.cs"))) `
            "review r7: build g1/a.pg g2/c.pg writes g2's unrouted c.cs next to g2/c.pg (exit $($r.Rc))"
        $mg = Join-Path $work "rv7-mg-clash"
        New-Proj $mg $null @{ "g1/pgconfig.json" = '{"targets":["csharp"]}'; "g1/a.pg" = $fnA; "g1/c.pg" = $fnA.Replace("alpha", "gamma")
                             "g2/pgconfig.json" = '{"targets":["csharp"]}'; "g2/c.pg" = $fnB } | Out-Null
        $r = Pg $mg @("build", "g1/a.pg", "g1/c.pg", "g2/c.pg")
        Check ($r.Rc -eq 0 -and $r.Out -notmatch 'conflicting output' -and (Test-Path (Join-Path $mg "g1/c.cs")) -and (Test-Path (Join-Path $mg "g2/c.cs"))) `
            "review r7: g1/c.pg and g2/c.pg in one multi-group build each emit beside their own source, no false conflict (exit $($r.Rc))"
        # ... and --out still collects every group's unrouted output in the one folder.
        $r = Pg (Join-Path $work "rv7-mg") @("build", "g1/a.pg", "g2/c.pg", "--out", "flat")
        Check ($r.Rc -eq 0 -and (Test-Path (Join-Path $work "rv7-mg/flat/c.cs"))) "review r7: with --out, the unrouted output goes to --out (exit $($r.Rc))"
        $mg = Join-Path $work "rv7-mg-one"
        $pr = "import { print } from `"std.io`"`nfn main() {`n  print(1)`n}"
        New-Proj $mg '{"targets":["csharp"]}' @{ "a.pg" = $pr; "sub/b.pg" = $pr.Replace("main", "helper") } | Out-Null
        $r = Pg $mg @("build", "a.pg", "sub/b.pg")
        Check ($r.Rc -eq 0 -and (Test-Path (Join-Path $mg "b.cs")) -and (Test-Path (Join-Path $mg "__polyglot_prelude.cs")) -and
               -not (Test-Path (Join-Path $mg "sub/__polyglot_prelude.cs"))) `
            "review r7: inputs of ONE group in two folders keep one sink and one shared C# prelude (exit $($r.Rc))"

        if ($caseInsensitive) {
            # review r6 (gating 1, Spark i1\a3h.sh H4): a route that climbs out of the config folder and re-enters it
            # under another case (repo/Proj, `../proj/gen/…`) is compared from its CLIMBED root, so `proj` meets the
            # disk's `Proj` — the user's project folder, never renamed: refused, under --check too, nothing written.
            # (The config folder, matched only up to case, used to win, so `proj` was never compared: exit 0 locally,
            # while a case-sensitive clone created a second `proj/` tree.)
            $x = New-Proj (Join-Path $work "rv6-a3h/Proj") '{"targets":["typescript"],"include":[{"pattern":"*.pg","output":"../proj/gen/%(Filename)"}]}' @{ "a.pg" = $fnA }
            $rc = Pg $x @("build", "--check")
            $rb = Pg $x @("build", "a.pg")
            Check ($rc.Rc -eq 1 -and $rb.Rc -eq 1 -and $rb.Out -cmatch "the directory '[^']*rv6-a3h[\\/]Proj' differs from 'proj'" -and $rb.Out -match 'not renamed' -and
                   -not (Test-Path (Join-Path $x "gen")) -and (@(Get-ChildItem (Join-Path $work "rv6-a3h") -Name) -join ',') -ceq 'Proj') `
                "review r6 (gating 1): ../proj/gen/ re-entering Proj/ is refused (check $($rc.Rc), build $($rb.Rc); was 0, 0), Proj/ untouched"
            # ... while re-entering under the folder's own spelling, beside a plain gen/ route into the same folder, builds.
            $x = New-Proj (Join-Path $work "rv6-a3h-same/Proj") '{"targets":["typescript"],"include":[{"pattern":"a.pg","output":"../Proj/gen/%(Filename)"},{"pattern":"b.pg","output":"gen/%(Filename)"}]}' @{ "a.pg" = $fnA; "b.pg" = $fnB }
            $r = Pg $x @("build")
            Check ($r.Rc -eq 0 -and (Test-Path (Join-Path $x "gen/a.ts")) -and (Test-Path (Join-Path $x "gen/b.ts")) -and (Pg $x @("build", "--check")).Rc -eq 0) `
                "review r6 (gating 1): ../Proj/gen/ (the folder's own spelling) beside gen/ builds and checks clean (exit $($r.Rc))"

            # review r7 (Spark i1\a3h2.log F): H4 with the input TYPED in another case from the parent — `build proj/a.pg`.
            # The config folder used to be spelled from the typed path, so `proj` was never compared: it wrote Proj/gen
            # and exited 0. The config root (and every route root) is now spelled as the disk spells it.
            $h4cfg = '{"targets":["typescript"],"include":[{"pattern":"*.pg","output":"../proj/gen/%(Filename)"}]}'
            $x = New-Proj (Join-Path $work "rv7-a3h-typed/Proj") $h4cfg @{ "a.pg" = $fnA }
            $parent = Join-Path $work "rv7-a3h-typed"
            $rc = Pg $parent @("build", "proj/a.pg", "--check")
            $rb = Pg $parent @("build", "proj/a.pg")
            $rB = Pg $parent @("build", "PROJ/a.pg")
            Check ($rc.Rc -eq 1 -and $rb.Rc -eq 1 -and $rB.Rc -eq 1 -and $rb.Out -cmatch "the directory '[^']*rv7-a3h-typed[\\/]Proj' differs from 'proj'" -and
                   -not (Test-Path (Join-Path $x "gen")) -and (@(Get-ChildItem $parent -Name) -join ',') -ceq 'Proj') `
                "review r7: H4 with the input typed 'proj/a.pg' / 'PROJ/a.pg' from the parent is refused like H4 (check $($rc.Rc), build $($rb.Rc), $($rB.Rc); was 3, 0), nothing written"
            # Accepted neighbours: a plain gen/ route and the folder's own spelling (../Proj/gen/), input typed in either case.
            $x = New-Proj (Join-Path $work "rv7-typed-plain/Proj") '{"targets":["typescript"],"include":[{"pattern":"*.pg","output":"gen/%(Filename)"}]}' @{ "a.pg" = $fnA }
            $r = Pg (Join-Path $work "rv7-typed-plain") @("build", "proj/a.pg")
            Check ($r.Rc -eq 0 -and $r.Out -cmatch '-> [^\r\n]*rv7-typed-plain[\\/]Proj[\\/]gen[\\/]a\.ts' -and (Pg (Join-Path $work "rv7-typed-plain") @("build", "proj/a.pg", "--check")).Rc -eq 0) `
                "review r7: gen/ routed from an input typed 'proj/a.pg' builds into the disk's Proj/gen and checks clean (exit $($r.Rc))"
            $x = New-Proj (Join-Path $work "rv7-typed-own/Proj") '{"targets":["typescript"],"include":[{"pattern":"*.pg","output":"../Proj/gen/%(Filename)"}]}' @{ "a.pg" = $fnA }
            $r = Pg (Join-Path $work "rv7-typed-own") @("build", "proj/a.pg")
            Check ($r.Rc -eq 0 -and (Test-Path (Join-Path $x "gen/a.ts")) -and (Pg $x @("build", "--check")).Rc -eq 0) `
                "review r7: ../Proj/gen/ (the folder's own spelling) from an input typed 'proj/a.pg' builds (exit $($r.Rc))"

            # review r7 (Spark i1\a3h3.sh): the H4 refusal's git hint, run VERBATIM after renaming Proj -> proj, from the
            # repo root and from an unrelated folder, restages exactly the renamed folder. It used to be cwd-relative —
            # `git rm -r -q --cached "." && …`: from the root it unstaged every tracked file, from elsewhere it failed.
            if (Get-Command git -ErrorAction SilentlyContinue) {
                $gq = @("-c", "user.name=gate", "-c", "user.email=gate@example.invalid", "-c", "commit.gpgsign=false", "-c", "core.hooksPath=", "-c", "core.autocrlf=false")
                $elsewhere = Join-Path $work "rv7-elsewhere"
                New-Item -ItemType Directory -Force $elsewhere | Out-Null
                foreach ($from in @("root", "elsewhere")) {
                    $repoDir = Join-Path $work "rv7-h4hint-$from"
                    $x = New-Proj (Join-Path $repoDir "Proj") $h4cfg @{ "a.pg" = $fnA }
                    New-Proj $repoDir $null @{ "README.md" = "readme"; "other/x.txt" = "x" } | Out-Null
                    & git @gq -C $repoDir init -q 2>&1 | Out-Null; & git @gq -C $repoDir add -A 2>&1 | Out-Null; & git @gq -C $repoDir commit -q -m one 2>&1 | Out-Null
                    $r = Pg $x @("build")
                    $m = [regex]::Match($r.Out, 'git -C "[^"]*" rm -r -q --cached -- "[^"]*" && git -C "[^"]*" add -- "[^"]*"')
                    Move-Item (Join-Path $repoDir "Proj") (Join-Path $repoDir "p_tmp"); Move-Item (Join-Path $repoDir "p_tmp") (Join-Path $repoDir "proj")
                    $hrc = -1
                    Push-Location $(if ($from -eq "root") { $repoDir } else { $elsewhere })
                    try { if ($m.Success) { Invoke-Expression "& { $($m.Value) } 2>&1 | Out-Null"; $hrc = $LASTEXITCODE } } finally { Pop-Location }
                    $status = @(& git @gq -C $repoDir status --porcelain 2>&1)
                    [string[]]$ls = @(& git @gq -C $repoDir ls-files 2>&1 | ForEach-Object { "$_" })
                    [Array]::Sort($ls, [StringComparer]::Ordinal)
                    $index = $ls -join ','
                    $outside = @($status | Where-Object { $_ -cnotmatch '^\S+\s+"?(Proj|proj)/' -or $_ -cmatch '-> "?(?!proj/)' })
                    Check ($r.Rc -eq 1 -and $m.Success -and $hrc -eq 0 -and $outside.Count -eq 0 -and $status.Count -gt 0 -and
                           $index -ceq 'README.md,other/x.txt,proj/a.pg,proj/pgconfig.json') `
                        "review r7: the H4 git hint run verbatim from $from (exit $hrc) restages only Proj -> proj (status: $($status -join ' | '); index: $index)"
                }
            }

            # review r8 (gating, Spark pgb51_c\rootcase R1): a route ROOT (--out / --build-owned) typed in another case
            # than the disk is never compared, refused or renamed — MSBuild's flag shape, `-c debug` after a build made
            # obj/Debug/. r7 respelled every root from disk, so the project-folder root swallowed the typed root and
            # `obj/Debug` (holding the SDK's own App.AssemblyInfo.cs) was refused: exit 1 on --check and build.
            $rk = New-Proj (Join-Path $work "rv8-rootcase") '{"targets":["csharp"]}' @{ "a.pg" = $fnA }
            $rkIn = Join-Path $rk "a.pg"
            $o1 = 'obj/Debug/net10.0/polyglot/.'
            $r = Pg $rk @("build", $rkIn, "--out", $o1, "--build-owned", $o1, "--record", "obj/Debug/net10.0/polyglot.json")
            Set-Content -LiteralPath (Join-Path $rk "obj/Debug/net10.0/App.AssemblyInfo.cs") -Value '// <auto-generated/> not Polyglot'
            $o2 = 'obj/debug/net10.0/polyglot/.'
            $rc = Pg $rk @("build", $rkIn, "--out", $o2, "--build-owned", $o2, "--record", "obj/debug/net10.0/polyglot.json", "--check")
            $rb = Pg $rk @("build", $rkIn, "--out", $o2, "--build-owned", $o2, "--record", "obj/debug/net10.0/polyglot.json")
            $objNames = @(Get-ChildItem (Join-Path $rk "obj") -Name) -join ','
            $netNames = @(Get-ChildItem (Join-Path $rk "obj/Debug/net10.0") -Name | Sort-Object) -join ','
            Check ($r.Rc -eq 0 -and $rc.Rc -eq 0 -and $rc.Out -match 'outputs are up to date' -and $rb.Rc -eq 0 -and
                   $rb.Out -notmatch '~>|differs from' -and $objNames -ceq 'Debug' -and $netNames -ceq 'App.AssemblyInfo.cs,polyglot,polyglot.json') `
                "review r8 (gating): --out/--build-owned typed obj/debug/... over the disk's obj/Debug/... is up to date and renames nothing (first $($r.Rc), check $($rc.Rc), build $($rb.Rc); was 1, 1; obj: $objNames; net10.0: $netNames)"
            # rr1\a3.log case 7: `--out GEN/SUB` over an existing gen/sub renames nothing (r7 renamed gen->GEN, sub->SUB).
            $r7 = New-Proj (Join-Path $work "rv8-outcase") $null @{ "a.pg" = $fnA }
            $r = Pg $r7 @("build", "a.pg", "--target", "typescript", "--out", "gen/sub")
            $rc = Pg $r7 @("build", "a.pg", "--target", "typescript", "--out", "GEN/SUB", "--check")
            $rb = Pg $r7 @("build", "a.pg", "--target", "typescript", "--out", "GEN/SUB")
            $dirs = (@(Get-ChildItem $r7 -Directory -Name) -join ',') + '/' + (@(Get-ChildItem (Join-Path $r7 "gen") -Directory -Name) -join ',')
            Check ($r.Rc -eq 0 -and $rc.Rc -eq 0 -and $rb.Rc -eq 0 -and $rb.Out -notmatch '~>' -and $dirs -ceq 'gen/sub') `
                "review r8: --out GEN/SUB over an existing gen/sub renames nothing (check $($rc.Rc), build $($rb.Rc); was 3, 0 with gen->GEN, sub->SUB; dirs: $dirs)"

            # review r9 (gating, Spark pg22b_c\h4x h4x.sh + h4b.sh): r8 exempted every ANCESTOR of a caller root, so a
            # caller root typed through the config folder in H4's case (`proj/out` over the disk's `Proj`) exempted
            # `proj` for H4's route `../proj/gen/a.ts` too — which LEAVES that root: --check 3, build 0 writing Proj/gen.
            # Only a route INTO a root skips the root's components now; each shape is refused like H4, nothing written.
            # (c) needs a child cwd typed `...\proj`: Push-Location respells it as on disk, ProcessStartInfo does not.
            function PgTypedCwd([string]$Dir, [string[]]$PgArgs) {
                $psi = [System.Diagnostics.ProcessStartInfo]::new((Resolve-Path $Cli).Path)
                foreach ($a in $PgArgs) { $psi.ArgumentList.Add($a) }
                $psi.WorkingDirectory = $Dir
                $psi.UseShellExecute = $false; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
                $p = [System.Diagnostics.Process]::Start($psi)
                $so = $p.StandardOutput.ReadToEndAsync(); $se = $p.StandardError.ReadToEndAsync()
                $p.WaitForExit()
                return [pscustomobject]@{ Rc = $p.ExitCode; Out = $so.Result + $se.Result }
            }
            foreach ($shape in @("out", "owned", "msbuild")) {
                $parent = Join-Path $work "rv9-h4-$shape"
                $x = New-Proj (Join-Path $parent "Proj") $h4cfg @{ "a.pg" = $fnA }
                if ($shape -eq "msbuild") {
                    $typed = Join-Path $parent "proj"
                    $flags = @("build", (Join-Path $typed "a.pg"), "--out", "obj/Debug/net10.0/polyglot/.", "--root", $typed,
                               "--record", "obj/Debug/net10.0/polyglot.outputs.json", "--build-owned", "obj/Debug/net10.0/polyglot/.")
                    $rc = PgTypedCwd $typed ($flags + @("--check"))
                    $rb = PgTypedCwd $typed $flags
                } else {
                    $flags = if ($shape -eq "out") { @("build", "proj/a.pg", "--out", "proj/out") } else { @("build", "proj/a.pg", "--build-owned", "proj/obj") }
                    $rc = Pg $parent ($flags + @("--check"))
                    $rb = Pg $parent $flags
                }
                $pat = "the directory '[^']*rv9-h4-$shape[\\/]Proj' differs from 'proj'"
                $inProj = @(Get-ChildItem -LiteralPath $x -Force -Name | Sort-Object) -join ','
                Check ($rc.Rc -eq 1 -and $rb.Rc -eq 1 -and $rc.Out -cmatch $pat -and $rb.Out -cmatch $pat -and $rb.Out -match 'not renamed' -and
                       $inProj -ceq 'a.pg,pgconfig.json' -and (@(Get-ChildItem $parent -Name) -join ',') -ceq 'Proj') `
                    "review r9 (gating): H4 beside a caller root typed through 'proj' ($shape) is refused like H4 (check $($rc.Rc), build $($rb.Rc); was 3, 0), nothing written (Proj: $inProj)"
            }

            # review r10 (item 1, Spark pg629_me\t4g t4g.sh + pg629_c\t4 t4.sh): rr1 case 7's tree (gen/sub from an
            # earlier `--out gen/sub` build) rebuilt with `--out GEN/SUB` plus an include rule `GEN/other/%(Filename)`.
            # When the route INTO the typed root was written first, the leaving route tripped r4's guard and main.cpp
            # printed "conflicting output ... 'a.pg' and 'a.pg' emit it" (two files: a non-existent second producer);
            # leaving first renamed gen -> GEN and built. Both orders now align gen -> GEN (sub stays) and build; with
            # a foreign file in gen, both refuse with A3's message. T-I/T-L: one a.pg, csharp+typescript, the rule on
            # typescript (a.cs into the root first) / on csharp (a.cs leaves first). F-I/F-L: a.pg + b.pg, typescript,
            # the rule on b.pg (a.pg into the root first) / on a.pg.
            foreach ($shape in @("T-I", "T-L", "F-I", "F-L", "T-I-foreign", "T-L-foreign")) {
                $p = Join-Path $work "rv10-order-$shape"
                if ($shape -like "T-*") {
                    $q = New-Proj $p '{"targets":["csharp","typescript"]}' @{ "a.pg" = $fnA }
                    $tg = if ($shape -like "T-I*") { "typescript" } else { "csharp" }
                    $rule = '{"targets":["csharp","typescript"],"include":[{"pattern":"*.pg","target":"' + $tg + '","output":"GEN/other/%(Filename)"}]}'
                    $ins = @("a.pg")
                } else {
                    $q = New-Proj $p '{"targets":["typescript"]}' @{ "a.pg" = $fnA; "b.pg" = $fnB }
                    $pt = if ($shape -eq "F-I") { "b.pg" } else { "a.pg" }
                    $rule = '{"targets":["typescript"],"include":[{"pattern":"' + $pt + '","output":"GEN/other/%(Filename)"}]}'
                    $ins = @("a.pg", "b.pg")
                }
                $r = Pg $q (@("build") + $ins + @("--out", "gen/sub"))
                if ($shape -like "*-foreign") { Set-Content -LiteralPath (Join-Path $q "gen/notes.txt") -Value 'hand-written' }
                Set-Content -LiteralPath (Join-Path $q "pgconfig.json") -Value $rule
                $rc = Pg $q (@("build") + $ins + @("--out", "GEN/SUB", "--check"))
                $rb = Pg $q (@("build") + $ins + @("--out", "GEN/SUB"))
                $dirs = (@(Get-ChildItem $q -Directory -Name) -join ',') + '/' + (@(Get-ChildItem (Join-Path $q "gen") -Directory -Name | Sort-Object) -join ',')
                $noConflict = $rc.Out -notmatch 'conflicting output' -and $rb.Out -notmatch 'conflicting output'
                if ($shape -like "*-foreign") {
                    $pat = "the directory '[^']*rv10-order-$shape[\\/]gen' differs from 'GEN'"
                    Check ($r.Rc -eq 0 -and $rc.Rc -eq 1 -and $rb.Rc -eq 1 -and $noConflict -and $rc.Out -cmatch $pat -and $rb.Out -cmatch $pat -and
                           $rb.Out -match 'not Polyglot output' -and $dirs -ceq 'gen/sub') `
                        "review r10: $shape refuses with A3's message in either route order, never a content conflict (check $($rc.Rc), build $($rb.Rc); dirs: $dirs)"
                } else {
                    Check ($r.Rc -eq 0 -and $rc.Rc -eq 3 -and $rb.Rc -eq 0 -and $noConflict -and $rc.Out -cmatch 'would rename [^\r\n]*[\\/]gen -> GEN' -and
                           $rb.Out -cmatch '~> [^\r\n]*[\\/]gen -> GEN' -and $dirs -ceq 'GEN/other,sub') `
                        "review r10: $shape aligns gen -> GEN and builds in either route order, never a content conflict (check $($rc.Rc), build $($rb.Rc); was 1, 1 into-root first; dirs: $dirs)"
                }
            }

            # review r10 (item 2, Spark pg629_c\t7 t7.sh): H4's config with the caller root typed AS the config folder
            # (`--out proj` over the disk's Proj, from the parent). Every route goes INTO the typed root, so r9 exempted
            # H4's `proj/gen/a.ts` and wrote Proj/gen/a.ts. A caller root respelling a config/input folder below another
            # route root (here the rule's climbed root, the parent) is refused before anything is written.
            $t7 = Join-Path $work "rv10-t7"
            $x = New-Proj (Join-Path $t7 "Proj") $h4cfg @{ "a.pg" = $fnA }
            $rc = Pg $t7 @("build", "proj/a.pg", "--out", "proj", "--check")
            $rb = Pg $t7 @("build", "proj/a.pg", "--out", "proj")
            $pat = "the directory '[^']*rv10-t7[\\/]Proj' differs from 'proj'"
            $inProj = @(Get-ChildItem -LiteralPath $x -Force -Name | Sort-Object) -join ','
            Check ($rc.Rc -eq 1 -and $rb.Rc -eq 1 -and $rc.Out -cmatch $pat -and $rb.Out -cmatch $pat -and $rb.Out -match 'not renamed' -and
                   $rb.Out -notmatch '(?m)^  -> ' -and $inProj -ceq 'a.pg,pgconfig.json' -and (@(Get-ChildItem $t7 -Name) -join ',') -ceq 'Proj') `
                "review r10: --out proj typed over H4's config folder Proj is refused like H4 (check $($rc.Rc), build $($rb.Rc); was 3, 0), nothing written (Proj: $inProj)"


            # Gating 1 (Spark h1\a3x.sh, a3x2.sh): a route that LEAVES the config folder compares its directories too.
            $x = New-Proj (Join-Path $work "rv5-a3x/proj") '{"targets":["typescript"],"include":[{"pattern":"src/**/*.pg","output":"../web/gen/%(RecursiveDir)%(Filename)"}]}' @{ "src/Foo/a.pg" = $fnA; "src/Foo/b.pg" = $fnB }
            Pg $x @("build") | Out-Null
            Move-Item (Join-Path $x "src/Foo") (Join-Path $x "src/tmpdir"); Move-Item (Join-Path $x "src/tmpdir") (Join-Path $x "src/foo")
            $web = Join-Path $work "rv5-a3x/web/gen"
            $r = Pg $x @("build", "--check")
            Check ($r.Rc -eq 3 -and $r.Out -cmatch 'would rename .*web[\\/]gen[\\/]Foo -> foo') "review r5 (gating 1): --check reports ../web/gen/Foo -> foo (exit $($r.Rc); was 0)"
            $r = Pg $x @("build")
            Check ($r.Rc -eq 0 -and (@(Get-ChildItem $web -Name) -join ',') -ceq 'foo' -and $r.Out -cmatch 'git -C "[^"]*/web/gen" rm -r -q --cached -- "Foo"') "review r5 (gating 1): build renames ../web/gen/Foo to foo with the git hint (exit $($r.Rc))"
            Check ((Pg $x @("build", "--check")).Rc -eq 0) "review r5 (gating 1): ... after which --check is clean"
            $x = New-Proj (Join-Path $work "rv5-a3x2/proj") '{"targets":["typescript"],"include":[{"pattern":"*.pg","output":"../web/Gen/%(Filename)","owned":true}]}' @{ "a.pg" = $fnA }
            Pg $x @("build") | Out-Null
            Set-Content -LiteralPath (Join-Path $x "pgconfig.json") -Value '{"targets":["typescript"],"include":[{"pattern":"*.pg","output":"../web/gen/%(Filename)","owned":true}]}'
            $r = Pg $x @("build", "--check")
            Check ($r.Rc -eq 3 -and $r.Out -cmatch 'would rename .*web[\\/]Gen -> gen') "review r5 (gating 1): a template change ../web/Gen/ -> ../web/gen/ is reported (exit $($r.Rc); was 0)"
            $r = Pg $x @("build")
            Check ($r.Rc -eq 0 -and (@(Get-ChildItem (Join-Path $work "rv5-a3x2/web") -Name) -join ',') -ceq 'gen') "review r5 (gating 1): ... and renamed (exit $($r.Rc))"
            # ... while an absolute route's own literal folder (a checkout path) is never compared.
            $abs = Join-Path $work "rv5-abs"
            New-Item -ItemType Directory -Force (Join-Path $abs "Out") | Out-Null
            $x = New-Proj (Join-Path $abs "proj") ('{"targets":["typescript"],"include":[{"pattern":"*.pg","output":"' + ((Join-Path $abs "out") -replace '\\', '/') + '/%(Filename)"}]}') @{ "a.pg" = $fnA }
            $r = Pg $x @("build")
            Check ($r.Rc -eq 0 -and (@(Get-ChildItem $abs -Directory -Name | Where-Object { $_ -ieq 'out' }) -join ',') -ceq 'Out') "review r5 (gating 1): an absolute route's literal folder Out/ (spelled out/) is left alone (exit $($r.Rc))"

            # Part 2 (Spark h1\c2g.sh G4/G5): a rename two levels deep prints ONE hint — the outermost directory's —
            # and --check names only that directory.
            foreach ($case in @(@{ n = "g4"; f = "src/Foo/Bar/x.pg"; to = @("src/Foo", "src/foo", "src/foo/Bar", "src/foo/bar") },
                                @{ n = "g5"; f = "src/Foo/Bar.pg"; to = @("src/Foo", "src/foo", "src/foo/Bar.pg", "src/foo/bar.pg") })) {
                $d = New-Proj (Join-Path $work "rv5-$($case.n)") '{"targets":["typescript"],"include":[{"pattern":"src/**/*.pg","output":"gen/%(RecursiveDir)%(Filename)","owned":true}]}' @{ $case.f = $fnA }
                Pg $d @("build") | Out-Null
                for ($i = 0; $i -lt 4; $i += 2) { Move-Item (Join-Path $d $case.to[$i]) (Join-Path $d "tmp.x"); Move-Item (Join-Path $d "tmp.x") (Join-Path $d $case.to[$i + 1]) }
                $rc = Pg $d @("build", "--check")
                $rb = Pg $d @("build")
                $hints = [regex]::Matches($rb.Out, 'run: git ').Count
                Check ($rc.Rc -eq 3 -and ([regex]::Matches($rc.Out, 'would rename')).Count -eq 1 -and $rc.Out -cmatch 'would rename .*gen[\\/]Foo -> foo' -and $rb.Rc -eq 0 -and $hints -eq 1 -and $rb.Out -cmatch 'run: git -C "[^"]*/gen" rm -r -q --cached -- "Foo" && git -C "[^"]*/gen" add -- "foo"') `
                    "review r5 ($($case.n)): a two-level rename reports one directory under --check and prints one hint (check $($rc.Rc), $hints hint(s); was 2)"
            }
        } else { Check $true "review r5 gating 1 / two-level hints (skipped: case-sensitive filesystem)" }
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
