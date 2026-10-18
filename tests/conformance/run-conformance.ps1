#!/usr/bin/env pwsh
# P35 slices 3+4 — the MERGED, PARALLEL differential-conformance runner.
#
# Replaces run-diff.ps1 (C#/TS), run-python.ps1 (C#/Python) and run-php.ps1 (C#/PHP): those three each
# re-transpiled and re-compiled the SAME C# oracle per program (3x the CLI spawns, 3x the csc/dotnet work).
# This runner does it ONCE per program — a single config-sourced multi-target `polyglot build`, a single
# `csc /shared` oracle compile (via scripts/lib/OracleCompile.ps1), then runs every configured runtime and
# compares each against the C# oracle. C# is the oracle for every pairwise compare (it is the reference
# semantics; the C#<->TS pair the old run-diff owned is just one of the compares now).
#
# Slice 4: programs run through `ForEach-Object -Parallel` (default). Each program is fully self-contained
# (its own copied work dir + pgconfig + emitted files + oracle dll), so one program per runspace parallelizes
# cleanly — transpile, the shared `csc /shared` compile (contention-free, one VBCSCompiler for the pool) and
# the runtime executions all happen inside the worker. Worker output is buffered and printed sorted by program
# name so the log is deterministic regardless of completion order. `-Sequential` keeps the exact serial path
# (live, in-order output) for debugging.
#
# Every gate the three old runners carried has a home here (folded in verbatim):
#   - G30  per-program 60s wall-clock timeout (async stdout drain before WaitForExit; kill the tree on timeout)
#   - G31  per-run unique work root (fixed prefix + short GUID), removed loudly at script end
#   - G32  optional committed `<name>.expected` golden (CRLF->LF + trailer-trim on both sides)
#   - #9   the C# MUST actually compile (a non-compiling oracle + empty stdout must never false-pass a compare)
#   - G28  the PHP refuser set is PINNED ($expectedRefusers); an unpinned php refusal FAILS, a pinned one that
#          starts transpiling WARNs (stale pin) but does not fail
#   - anti-silent-drop: after the ONE build, the expected output file for EVERY configured target must exist
#
# Config discovery is ENTRY-relative (slice 0), so each program is COPIED into its own work dir with a
# per-program pgconfig.json declaring exactly the targets under test — the checked-in programs/pgconfig.json
# (csharp+typescript only) never leaks in. The 7 pinned PHP refusers get a php-less config plus a dedicated
# `--target php` refusal probe.
#
# Usage:  pwsh tests/conformance/run-conformance.ps1
#         [-Cli <path>] [-Targets csharp,typescript,python,php] [-Php <path>] [-StagingOut <dir>]
#         [-Sequential] [-ThrottleLimit <n>]
#   -Targets      : subset to test; the csharp oracle is ALWAYS implied. A requested-but-missing runtime = exit 2.
#   -StagingOut   : if set, pristine (pre-import-rewrite) .ts are staged there PER PROGRAM for the library gate,
#                   and that dir is NOT deleted (the caller owns its lifecycle). Absent -> staged inside the
#                   per-run work root and cleaned with it.
#   -Sequential   : run programs one at a time, printing live (the pre-slice-4 path); for debugging.
#   -ThrottleLimit: parallel degree (default 8 — each worker both csc-compiles and runs 1-4 runtimes).
#   -Cultures     : P40 A3 culture leg. Every compiled C# oracle is re-run once per culture with
#                   DOTNET_STARTUP_HOOKS=<culture/PgCultureHook.dll> + PG_CULTURE=<culture> set on THAT invocation
#                   only, and its stdout must equal the invariant oracle run. The hook asserts the culture took
#                   effect (exit 97 otherwise — a host without ICU fails loudly, never passes vacuously).
#                   Default tr-TR,nl-BE,sv-SE,fa-IR; `pg-hostile` is the synthetic culture (en-US clone, sign U+2212,
#                   decimal ',', group '.'); `-Cultures none` turns the leg off.
#   -Only         : restrict to these program names (comma list) — the -Tier fast synthetic-culture leg.
# PHP (when tested) is ALSO re-run under a hostile `-d precision=17 -d serialize_precision=17` and must still
# match the oracle (P40 A3: the float helper sets/restores serialize_precision itself).
# Grep gates on emitted code: no `__polyglot_unlowered_expr__` anywhere; no `CurrentCulture =` in any .cs.
# Needs `dotnet` always; `node`/`python`/`php` only for the corresponding requested targets.

param(
    [string]$Cli = (Join-Path $PSScriptRoot ".." ".." "x64" "Debug" "MintPlayer.Polyglot.Cli.exe"),
    [string[]]$Targets = @("csharp", "typescript", "python", "php"),
    [string]$Php = "",
    [string]$StagingOut = "",
    [string[]]$Cultures = @("tr-TR", "nl-BE", "sv-SE", "fa-IR"),
    [string[]]$Only = @(),
    [switch]$Sequential,
    [ValidateRange(1, 64)]
    [int]$ThrottleLimit = 8
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

if (-not (Test-Path $Cli)) {
    Write-Host "polyglot CLI not found at $Cli — build the solution first (see CLAUDE.md)."
    exit 2
}
# ProcessStartInfo + csc want a concrete, absolute CLI path (workers run with a per-program cwd).
$Cli = (Resolve-Path $Cli).Path

# Canonical target order (drives pgconfig ordering + the summary line) and the emitted-file extension per target.
$canonical = @("csharp", "typescript", "python", "php")
$extOf = @{ csharp = "cs"; typescript = "ts"; python = "py"; php = "php" }
$labelOf = @{ csharp = "C#"; typescript = "TS"; python = "Python"; php = "PHP" }

# G28 pinned expected-refusal set: exactly the programs that legitimately refuse on the PHP surface today —
# operatorOverloading:arithmetic/:conversion (vec2, operators_full, operator_unary/bitwise/compound/convert),
# async (async_await, async_compose), and a portable fn with no PHP `actual` (expect_actual, extern_ffi).
# P37 C6 widened PHP to :eq + :indexers, so operator_eq and indexer_grid now RUN on PHP. Add here ONLY when
# the PHP plugin gains a new deliberate gate.
$expectedRefusers = @(
    'async_await', 'async_compose', 'expect_actual', 'extern_ffi', 'operators_full', 'prop_accessors', 'vec2',
    'operator_unary', 'operator_bitwise', 'operator_compound', 'operator_convert',
    # Same gate as prop_accessors: PHP has no property setters (< 8.4).
    'prop_block_accessors',
    # Review round 6 — init-order/constructors half: a property setter + an arithmetic operator (both gates above).
    'init_order_setter_op'
)

# Normalize the requested target set; the csharp oracle is always present.
# Split on commas as well as array elements: a cross-process caller (`pwsh run-conformance.ps1 -Targets
# csharp,typescript`, or any `-File` invocation) hands the whole list over as ONE string — only an
# in-process `& script.ps1 -Targets csharp, typescript` binds a real array. Accept both spellings so a
# call site can't silently mis-bind (it blocked the 0.9.8 release: run 30071627886).
$targetSet = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
foreach ($t in (($Targets -split ',') | Where-Object { $_.Trim() })) {
    $lc = $t.Trim().ToLowerInvariant()
    if (-not $extOf.ContainsKey($lc)) { Write-Host "unknown target '$t' (want: $($canonical -join ', '))."; exit 2 }
    $null = $targetSet.Add($lc)
}
$null = $targetSet.Add("csharp")
$tested = @($canonical | Where-Object { $targetSet.Contains($_) })

# Runtime resolution + presence guards. ProcessStartInfo wants a concrete path, not a PATH lookup; a
# requested-but-missing runtime is a hard exit 2 (never a silent skip that shrinks coverage).
$dotnet = (Get-Command dotnet -ErrorAction SilentlyContinue)?.Source
if (-not $dotnet) { Write-Host "dotnet not found on PATH (required for the C# oracle)."; exit 2 }

$node = $null
if ($targetSet.Contains("typescript")) {
    $node = (Get-Command node -ErrorAction SilentlyContinue)?.Source
    if (-not $node) { Write-Host "typescript requested but node not found on PATH."; exit 2 }
}
$py = $null
if ($targetSet.Contains("python")) {
    $pyCmd = (Get-Command python3 -ErrorAction SilentlyContinue) ?? (Get-Command python -ErrorAction SilentlyContinue)
    if (-not $pyCmd) { Write-Host "python requested but neither python3 nor python found on PATH."; exit 2 }
    $py = $pyCmd.Source
}
$phpExe = $null
if ($targetSet.Contains("php")) {
    if ($Php -and (Test-Path $Php)) {
        $phpExe = (Resolve-Path $Php).Path
    } else {
        $phpExe = (Get-Command php -ErrorAction SilentlyContinue)?.Source
    }
    if (-not $phpExe) { Write-Host "php requested but php not found (pass -Php <path> or put it on PATH)."; exit 2 }
}

# The shared csc oracle (P35 slice 2). Dot-source in the parent for the pre-warm + Stop-OracleBuildServer;
# each parallel worker dot-sources it again into its own runspace (see the -Parallel block).
$oraclePath = (Join-Path $PSScriptRoot ".." ".." "scripts" "lib" "OracleCompile.ps1")
. $oraclePath

$progDir = Join-Path $PSScriptRoot "programs"
# G31: a per-run unique work root, removed loudly at script end.
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("polyglot-conformance-" + [System.Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null

# Library-staging: the caller-owned -StagingOut if given (survives this run), else inside $work.
$libStaging = if ($StagingOut) { $StagingOut } else { Join-Path $work "library-staging" }
if ($targetSet.Contains("typescript")) {
    Remove-Item -Recurse -Force $libStaging -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $libStaging | Out-Null
}

# G30 per-program timeout: run a generated program under a wall-clock cap. Reads stdout/stderr async BEFORE
# WaitForExit (the classic pipe-deadlock trap — a chatty child fills the pipe buffer and blocks forever if we
# wait first), and on timeout kills the whole process tree. stderr is discarded unless -MergeStdErr (so build/
# runtime noise never pollutes program stdout — the `2>$null` property this gate has always relied on).
# -WorkingDirectory sets the child's cwd (file_io* programs write relative paths into the per-program dir).
function Invoke-WithTimeout {
    param(
        [string]$FilePath,
        [string[]]$Arguments,
        [string]$WorkingDirectory,
        [int]$TimeoutSec = 60,
        [switch]$MergeStdErr,
        # Extra environment variables for THIS child only (the culture leg's DOTNET_STARTUP_HOOKS + PG_CULTURE);
        # never set on the runner process, so no other invocation (csc, node, the oracle run) can see them.
        [hashtable]$Environment = @{}
    )
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $FilePath
    foreach ($a in $Arguments) { $psi.ArgumentList.Add($a) }
    if ($WorkingDirectory) { $psi.WorkingDirectory = $WorkingDirectory }
    foreach ($k in $Environment.Keys) { $psi.Environment[$k] = [string]$Environment[$k] }
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $outTask = $p.StandardOutput.ReadToEndAsync()
    $errTask = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit($TimeoutSec * 1000)) {
        try { $p.Kill($true) } catch {}
        try { $p.WaitForExit(5000) | Out-Null } catch {}
        return [pscustomobject]@{ Stdout = ""; Stderr = ""; ExitCode = -1; TimedOut = $true }
    }
    $out = $outTask.GetAwaiter().GetResult()
    $err = $errTask.GetAwaiter().GetResult()
    if ($MergeStdErr) { $out = $out + $err }
    return [pscustomobject]@{ Stdout = $out; Stderr = $err; ExitCode = $p.ExitCode; TimedOut = $false }
}

# G32 golden normalization: CRLF -> LF, then strip trailing whitespace/newlines. Applied to every compared
# side so a committed golden is line-ending- and trailer-agnostic and C# (CRLF) matches node/python/php (LF).
function Format-Golden([string]$s) {
    if ($null -eq $s) { return "" }
    return ($s -replace "`r`n", "`n").TrimEnd("`r", "`n", " ", "`t")
}

# Process ONE program (single .pg or a multi-file dir). Self-contained so it runs identically in the parent
# (sequential) or a fresh runspace (parallel): it takes every path/exe/config it needs via $Ctx and returns
# a result object { Name; Ok; Refused; Lines } — Lines are the [PASS]/[FAIL]/[WARN] strings to print. It must
# NOT Write-Host (parallel output would interleave); the caller prints Lines.
#
# $Desc  = @{ Name; Stem; IsDir }.  $Ctx = the shared read-only config bag (see $ctx below).
# Requires Invoke-WithTimeout, Format-Golden, and Invoke-OracleCompile to be defined in the current runspace.
function Invoke-ProgramWorker {
    param([hashtable]$Desc, [hashtable]$Ctx)

    $name = $Desc.Name; $stem = $Desc.Stem; $isDir = $Desc.IsDir
    $work = $Ctx.Work; $progDir = $Ctx.ProgDir; $libStaging = $Ctx.LibStaging; $Cli = $Ctx.Cli
    $dotnet = $Ctx.Dotnet; $node = $Ctx.Node; $py = $Ctx.Py; $phpExe = $Ctx.PhpExe
    $tested = $Ctx.Tested; $expectedRefusers = $Ctx.ExpectedRefusers
    $canonical = $Ctx.Canonical; $extOf = $Ctx.ExtOf; $labelOf = $Ctx.LabelOf; $stageTs = $Ctx.StageTs

    $lines = [System.Collections.Generic.List[string]]::new()
    $refused = 0
    $result = { param($ok) [pscustomobject]@{ Name = $name; Ok = $ok; Refused = $refused; Lines = $lines } }

    # The pgconfig targets for this program: the tested set (canonical order), minus php for a pinned refuser.
    $progTargets = @($canonical | Where-Object { $tested -contains $_ })
    if (($expectedRefusers -contains $name) -and ($progTargets -contains 'php')) {
        $progTargets = @($progTargets | Where-Object { $_ -ne 'php' })
    }

    $dir = Join-Path $work $name
    try {
        # 1. Copy the source into the per-program dir and write its own pgconfig (entry-relative discovery).
        Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force $dir | Out-Null
        if ($isDir) {
            Copy-Item -Recurse (Join-Path $progDir $name "*") $dir
            $entry = Join-Path $dir "entry.pg"
        } else {
            Copy-Item (Join-Path $progDir "$name.pg") $dir
            $entry = Join-Path $dir "$name.pg"
        }
        $json = '{ "targets": [' + (($progTargets | ForEach-Object { "`"$_`"" }) -join ", ") + '] }'
        Set-Content -Path (Join-Path $dir "pgconfig.json") -Value $json -Encoding UTF8

        # 2. ONE config-sourced multi-target build (no --target; the pgconfig decides).
        $buildArgs = @("build", $entry, "--lib", "io", "--out", $dir)
        if ($isDir) { $buildArgs += @("--root", $dir) }
        $buildOut = & $Cli @buildArgs 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) {
            $lines.Add("[FAIL] $name (transpile failed): $($buildOut.Trim())")
            return (& $result $false)
        }
        # Anti-silent-drop: every configured target's output file must exist after the single build.
        foreach ($t in $progTargets) {
            if (-not (Test-Path (Join-Path $dir "$stem.$($extOf[$t])"))) {
                $lines.Add("[FAIL] $name : expected $stem.$($extOf[$t]) not emitted (silent drop of target '$t')")
                return (& $result $false)
            }
        }
        # P40 A2 grep gate: the lowering poison must never reach emitted code (compile() refuses with an
        # internal compiler error first; this catches a regression of that backstop).
        $poison = @(Get-ChildItem $dir -File | Where-Object { $_.Extension -in '.cs', '.ts', '.py', '.php' } |
                    Select-String -SimpleMatch '__polyglot_unlowered_expr__' -List)
        if ($poison.Count -gt 0) {
            $lines.Add("[FAIL] $name : emitted output contains __polyglot_unlowered_expr__ ($(($poison | ForEach-Object { Split-Path $_.Path -Leaf }) -join ', '))")
            return (& $result $false)
        }
        # P40 A3 grep gate: emitted C# is culture-free BY CONSTRUCTION (invariant formatting at each call site);
        # it must never mutate the process culture (the old `Main` guard is gone — library code can't own it).
        $cultureSet = @(Get-ChildItem $dir -File -Filter *.cs | Select-String -Pattern 'CurrentCulture\s*=' -List)
        if ($cultureSet.Count -gt 0) {
            $lines.Add("[FAIL] $name : emitted C# assigns CurrentCulture ($(($cultureSet | ForEach-Object { Split-Path $_.Path -Leaf }) -join ', '))")
            return (& $result $false)
        }

        # 3. Stage the PRISTINE .ts (before the node import rewrite) for the library gate. Race-free: each
        # worker writes only its own program's staging subdir.
        if ($stageTs) {
            $stageDir = Join-Path $libStaging $name
            New-Item -ItemType Directory -Force $stageDir | Out-Null
            foreach ($f in Get-ChildItem $dir -Filter *.ts) { Copy-Item $f.FullName $stageDir }
        }

        # 4. §4.5 module linking: a multi-module program emits several .ts with extensionless cross-imports
        # (`import { X } from "./dep"`); Node's ESM loader can't resolve an extensionless sibling when running a
        # .ts directly, so add the `.ts` extension to relative specifiers. NODE-ONLY + harness-local — the
        # staged (pristine) copy above keeps the extensionless library shape the library gate checks.
        $tsFiles = @(Get-ChildItem $dir -Filter *.ts)
        if ($tsFiles.Count -gt 1) {
            foreach ($f in $tsFiles) {
                $c = Get-Content $f.FullName -Raw
                $c = [regex]::Replace($c, 'from "(\./[^"]+)"', 'from "$1.ts"')
                Set-Content $f.FullName -Value $c -NoNewline
            }
        }

        # 5. ONE C# oracle compile (issue #9: it must actually compile, else empty stdout can false-pass a
        # compare). The multi-module C# is one assembly from every emitted .cs (also proves no CS0101).
        $csSources = @(Get-ChildItem $dir -Filter *.cs | ForEach-Object { $_.FullName })
        $dll = Join-Path $dir "$stem.dll"
        $r = Invoke-OracleCompile -Sources $csSources -OutDll $dll
        if (-not $r.Ok) {
            $lines.Add("[FAIL] $name : generated C# did not compile")
            return (& $result $false)
        }

        # 6. Run each configured runtime (WorkingDirectory = the program dir), oracle = C# output.
        # P40 B3: on Windows a .NET child writes redirected stdout in its (hidden) console's OEM code page, so
        # non-ASCII output came back as `?`. Switch that console to UTF-8 first — the C# twin of Python's `-X utf8`
        # below (a harness setting; the generated program is unchanged). Linux/macOS .NET already writes UTF-8.
        $runCs = {
            param([hashtable]$envVars = @{})
            if ($IsWindows) {
                Invoke-WithTimeout -FilePath "$env:ComSpec" -Arguments @('/d', '/c', 'chcp', '65001', '>nul', '&&', $dotnet, $dll) -WorkingDirectory $dir -Environment $envVars
            } else {
                Invoke-WithTimeout -FilePath $dotnet -Arguments @($dll) -WorkingDirectory $dir -Environment $envVars
            }
        }
        # Bounded first-difference report (a whole-output dump of a long program floods the log).
        $firstDiff = {
            param([string]$want, [string]$got)
            $w = $want -split "`n"; $g = $got -split "`n"
            $n = [Math]::Max($w.Count, $g.Count)
            for ($i = 0; $i -lt $n; $i++) {
                $wl = if ($i -lt $w.Count) { $w[$i] } else { '<eof>' }
                $gl = if ($i -lt $g.Count) { $g[$i] } else { '<eof>' }
                if (-not [string]::Equals($wl, $gl, [StringComparison]::Ordinal)) {
                    $clip = { param($s) if ($s.Length -gt 160) { $s.Substring(0, 160) + '…' } else { $s } }
                    return "line $($i + 1): want '$(& $clip $wl)' got '$(& $clip $gl)'"
                }
            }
            return "(no line difference)"
        }
        $csr = & $runCs
        if ($csr.TimedOut) { $lines.Add("[FAIL] $name : timed out after 60s (csharp)"); return (& $result $false) }
        if ($csr.ExitCode -ne 0) { $lines.Add("[FAIL] $name : generated C# crashed at runtime (exit $($csr.ExitCode))"); return (& $result $false) }
        $oracle = Format-Golden $csr.Stdout
        $outputs = @{ csharp = $oracle }

        foreach ($t in $progTargets) {
            if ($t -eq 'csharp') { continue }
            $file = Join-Path $dir "$stem.$($extOf[$t])"
            $label = $labelOf[$t]
            switch ($t) {
                'typescript' { $rr = Invoke-WithTimeout -FilePath $node   -Arguments @($file) -WorkingDirectory $dir }
                'python'     { $rr = Invoke-WithTimeout -FilePath $py     -Arguments @('-X', 'utf8', $file) -WorkingDirectory $dir } # S1: UTF-8 stdout on every OS
                'php'        { $rr = Invoke-WithTimeout -FilePath $phpExe  -Arguments @($file) -WorkingDirectory $dir -MergeStdErr }
            }
            if ($rr.TimedOut) { $lines.Add("[FAIL] $name : timed out after 60s ($t)"); return (& $result $false) }
            $out = Format-Golden $rr.Stdout
            if ($rr.ExitCode -ne 0) {
                if ($t -eq 'php') { $lines.Add("[FAIL] $name : generated PHP crashed at runtime (exit $($rr.ExitCode)): $out") }
                else { $lines.Add("[FAIL] $name : generated $label crashed at runtime (exit $($rr.ExitCode))") }
                return (& $result $false)
            }
            $outputs[$t] = $out
            # P40 review (B9): every output comparison in this runner is ORDINAL. PowerShell's -ne is
            # case-insensitive and culture-aware ("1E+21" -ne "1e+21" and "HELLO" -ne "hello" are both False,
            # and even -cne ignores culture-ignorable code points such as U+00AD), so real divergences passed.
            if (-not [string]::Equals($oracle, $out, [StringComparison]::Ordinal)) {
                $lines.Add("[FAIL] $name : C# and $label diverged")
                $lines.Add("        C#:  $($oracle -replace "`r?`n", ' | ')")
                $lines.Add("        $($label): $($out -replace "`r?`n", ' | ')")
                return (& $result $false)
            }
            # P40 A3: PHP again under a hostile float ini. The shipped float helper sets serialize_precision=-1
            # itself and restores it, so the output must not move.
            if ($t -eq 'php') {
                $hr = Invoke-WithTimeout -FilePath $phpExe -Arguments @('-d', 'precision=17', '-d', 'serialize_precision=17', $file) -WorkingDirectory $dir -MergeStdErr
                if ($hr.TimedOut) { $lines.Add("[FAIL] $name : timed out after 60s (php, hostile precision)"); return (& $result $false) }
                $hout = Format-Golden $hr.Stdout
                if ($hr.ExitCode -ne 0 -or -not [string]::Equals($hout, $oracle, [StringComparison]::Ordinal)) {
                    $lines.Add("[FAIL] $name : PHP under -d precision=17 -d serialize_precision=17 diverged (exit $($hr.ExitCode)): $(& $firstDiff $oracle $hout)")
                    return (& $result $false)
                }
            }
        }

        # P40 A3 culture leg: the SAME compiled oracle under each hostile culture must print byte-identical text.
        # The env vars go on this child only; exit 97 is the hook saying the culture did not take effect.
        foreach ($cul in $Ctx.Cultures) {
            $cr = & $runCs @{ DOTNET_STARTUP_HOOKS = $Ctx.CultureHook; PG_CULTURE = $cul }
            if ($cr.TimedOut) { $lines.Add("[FAIL] $name : timed out after 60s (csharp under culture $cul)"); return (& $result $false) }
            if ($cr.ExitCode -eq 97) {
                $lines.Add("[FAIL] $name : culture $cul did not take effect — $($cr.Stderr.Trim()) (no ICU? the leg must not pass vacuously)")
                return (& $result $false)
            }
            if ($cr.ExitCode -ne 0) {
                $lines.Add("[FAIL] $name : generated C# crashed under culture $cul (exit $($cr.ExitCode)): $(($cr.Stderr.Trim() -split "`n" | Select-Object -First 2) -join ' ')")
                return (& $result $false)
            }
            # P40 review (B9): the positive control. A hook that never loaded leaves the run invariant, so its
            # stdout matches the oracle and the compare below would pass vacuously; the hook's ok line proves
            # it ran and the culture took effect.
            if (-not $cr.Stderr.Contains("PgCultureHook: ok $cul", [StringComparison]::Ordinal)) {
                $lines.Add("[FAIL] $name : culture $cul — the startup hook never reported 'PgCultureHook: ok $cul' (it did not load; the leg must not pass vacuously)")
                return (& $result $false)
            }
            $cout = Format-Golden $cr.Stdout
            if (-not [string]::Equals($cout, $oracle, [StringComparison]::Ordinal)) {
                $lines.Add("[FAIL] $name : C# output changed under culture $cul — $(& $firstDiff $oracle $cout)")
                return (& $result $false)
            }
        }

        # G32: an optional committed golden pins the exact stdout. Every configured target (incl. the oracle)
        # is asserted against it (php is naturally skipped for pinned refusers — not in their target set).
        $golden = Join-Path $progDir "$name.expected"
        if (Test-Path $golden) {
            $exp = Format-Golden (Get-Content $golden -Raw)
            foreach ($t in $progTargets) {
                if (-not [string]::Equals((Format-Golden $outputs[$t]), $exp, [StringComparison]::Ordinal)) {
                    $lines.Add("[FAIL] $name : $t output differs from $name.expected")
                    return (& $result $false)
                }
            }
        }

        $lines.Add("[PASS] $name  ->  $($oracle -replace "`r?`n", ' | ')")

        # 7. Pinned PHP refuser (php requested): a dedicated `--target php` build must still refuse. PASS on
        # refusal; WARN (not fail) if it now transpiles — the pin is stale, narrow it.
        if (($expectedRefusers -contains $name) -and ($tested -contains 'php')) {
            $probeArgs = @("build", $entry, "--target", "php", "--lib", "io", "--out", (Join-Path $dir "_phpprobe"))
            if ($isDir) { $probeArgs += @("--root", $dir) }
            & $Cli @probeArgs *> $null
            if ($LASTEXITCODE -ne 0) {
                $lines.Add("[PASS] $name (php refused by design)")
                $refused++
            } else {
                $lines.Add("[WARN] $name was expected to refuse on php but now transpiles — update the pinned set")
            }
        }
        return (& $result $true)
    }
    catch {
        $lines.Add("[FAIL] $name : worker exception: $($_.Exception.Message)")
        return (& $result $false)
    }
}

# Build the program descriptor list: single-file programs (sorted) then multi-file dirs (sorted) — the serial
# order the old runners used, which -Sequential prints verbatim.
$descriptors = [System.Collections.Generic.List[hashtable]]::new()
foreach ($pg in Get-ChildItem $progDir -Filter *.pg | Sort-Object Name) {
    $descriptors.Add(@{ Name = $pg.BaseName; Stem = $pg.BaseName; IsDir = $false })
}
foreach ($d in Get-ChildItem $progDir -Directory | Sort-Object Name) {
    if (-not (Test-Path (Join-Path $d.FullName "entry.pg"))) { continue }
    $descriptors.Add(@{ Name = $d.Name; Stem = "entry"; IsDir = $true })
}
# -Only: same comma-or-array tolerance as -Targets. An unknown name is a hard error (never a silent shrink).
$onlySet = @(($Only -split ',') | ForEach-Object { $_.Trim() } | Where-Object { $_ })
if ($onlySet.Count -gt 0) {
    foreach ($o in $onlySet) {
        if (-not ($descriptors | Where-Object { $_.Name -eq $o })) { Write-Host "-Only: no conformance program named '$o'."; exit 2 }
    }
    $descriptors = [System.Collections.Generic.List[hashtable]]@($descriptors | Where-Object { $onlySet -contains $_.Name })
}

# P40 A3 culture leg: compile the startup hook ONCE (csc /shared, library, nullable + warnaserror).
$cultureList = @(($Cultures -split ',') | ForEach-Object { $_.Trim() } | Where-Object { $_ -and $_ -ne 'none' })
$cultureHook = ""
if ($cultureList.Count -gt 0) {
    $cultureHook = Join-Path $work "_culturehook" "PgCultureHook.dll"
    $hr = Invoke-OracleCompile -Sources @((Join-Path $PSScriptRoot "culture" "PgCultureHook.cs")) -OutDll $cultureHook -Library -NullableEnable -WarnAsError
    if (-not $hr.Ok) {
        Write-Host "culture leg: PgCultureHook.cs did not compile:`n$($hr.Output.Trim())"
        Stop-OracleBuildServer
        Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
        exit 1
    }
}

# The read-only config bag shared with every worker (by reference in-process; workers only read it).
$ctx = @{
    Work = $work; ProgDir = $progDir; LibStaging = $libStaging; Cli = $Cli
    Dotnet = $dotnet; Node = $node; Py = $py; PhpExe = $phpExe
    Tested = $tested; ExpectedRefusers = $expectedRefusers
    Canonical = $canonical; ExtOf = $extOf; LabelOf = $labelOf
    StageTs = $targetSet.Contains('typescript')
    Cultures = $cultureList; CultureHook = $cultureHook
}

# Pre-warm the shared csc oracle context so its reference .rsp exists on disk BEFORE parallel workers race to
# create it (each worker's Get-OracleContext then finds it present and skips the write — no file-lock race).
Get-OracleContext | Out-Null

$count = $descriptors.Count
$mode = if ($Sequential) { "sequential" } else { "parallel x$ThrottleLimit" }
Write-Host "==> Conformance: $count program(s) across $($tested -join ', ') ($mode)`n"

$results = $null
$sw = [System.Diagnostics.Stopwatch]::StartNew()
try {
    if ($Sequential) {
        # Exact serial path: one program at a time, live in-order output (for debugging).
        $acc = [System.Collections.Generic.List[object]]::new()
        foreach ($d in $descriptors) {
            $res = Invoke-ProgramWorker -Desc $d -Ctx $ctx
            foreach ($ln in $res.Lines) { Write-Host $ln }
            $acc.Add($res)
        }
        $results = $acc
    } else {
        # Parallel: each program in a fresh runspace. Re-establish the helper functions (a runspace does not
        # inherit the parent's functions) by passing their source text, and dot-source OracleCompile inside the
        # block. Output is buffered per worker and printed sorted below — deterministic despite completion order.
        $iwtText = ${function:Invoke-WithTimeout}.ToString()
        $fgText = ${function:Format-Golden}.ToString()
        $workerText = ${function:Invoke-ProgramWorker}.ToString()

        $results = $descriptors | ForEach-Object -ThrottleLimit $ThrottleLimit -Parallel {
            $ErrorActionPreference = 'Stop'
            ${function:Invoke-WithTimeout} = $using:iwtText
            ${function:Format-Golden} = $using:fgText
            ${function:Invoke-ProgramWorker} = $using:workerText
            . $using:oraclePath
            Invoke-ProgramWorker -Desc $_ -Ctx $using:ctx
        }
    }
}
finally {
    # Stop the shared compile server ONCE (all workers share one VBCSCompiler), then G31 loud cleanup of the
    # per-run root. A caller-owned -StagingOut lives outside $work and survives.
    Stop-OracleBuildServer
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
    if (Test-Path $work) { Write-Host "WARNING: could not remove conformance work dir '$work' — clean it up manually." }
}
$sw.Stop()

# Parallel results arrive out of order — print them sorted by program name for a deterministic log.
# (Sequential already printed live, in order.)
if (-not $Sequential) {
    foreach ($res in ($results | Sort-Object Name)) {
        foreach ($ln in $res.Lines) { Write-Host $ln }
    }
}

$fail = 0
$refused = 0
foreach ($res in $results) {
    if (-not $res.Ok) { $fail++ }
    $refused += $res.Refused
}

$testedLabels = ($tested | ForEach-Object { $labelOf[$_] }) -join ", "
Write-Host ""
Write-Host ("(conformance: {0} program(s) in {1:N1}s, {2})" -f $count, $sw.Elapsed.TotalSeconds, $mode)
$cultureNote = if ($cultureList.Count) { "C# oracle re-run under $($cultureList -join ', ')" } else { 'off' }
$phpNote = if ($targetSet.Contains('php')) { '; PHP also under precision=17/serialize_precision=17' } else { '' }
Write-Host "(culture leg: $cultureNote$phpNote)"
if ($fail -eq 0) {
    $refusedNote = if ($targetSet.Contains('php')) { " ($refused php refused by design)" } else { "" }
    Write-Host "All $count conformance program(s) agree across $testedLabels$refusedNote."
    exit 0
}
Write-Host "$fail of $count conformance program(s) diverged."
exit 1
