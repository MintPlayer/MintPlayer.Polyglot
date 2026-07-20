#!/usr/bin/env pwsh
# §3.B refusal gate (E2E wave 2, G33): every refused construct must FAIL LOUDLY and NEVER miscompile.
#
# For each fixture we assert the PRD §3.B contract end to end:
#   * `polyglot build` exits nonzero,
#   * it prints a `<path>:<line>:<col>: error:` shaped diagnostic (the editor-parseable shape the
#     watch gate and the VS Code problemMatcher also freeze),
#   * the message NAMES the refusal (not a generic "unknown type"), and
#   * NO output file is written — a stale pre-seeded twin keeps its sentinel byte-for-byte and the
#     out dir gains nothing (a refused compile is all-or-nothing, never a partial emit).
#
# Plus the `polyglot check` surface: good -> exit 0, broken -> nonzero, `--json` field shape, and the
# `--json --watch` incompatibility (exit 64).
#
# Usage:  pwsh tests/refusals/run-refusals.ps1   (build the solution first; see CLAUDE.md)

param(
    [string]$Cli = "$PSScriptRoot\..\..\x64\Debug\MintPlayer.Polyglot.Cli.exe"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Cli)) {
    Write-Host "polyglot CLI not found at $Cli - build the solution first (see CLAUDE.md)."
    exit 2
}

$fixtures = Join-Path $PSScriptRoot "fixtures"
# A diagnostic line, editor-parseable: `<path>:<line>:<col>: <severity>: <message>`. Searched as a
# substring so a leading Windows drive letter (`C:\`) never trips it (a drive colon is followed by `\`).
$reDiag = ':\d+:\d+: (error|warning|info): '

$bad = 0
function Assert([bool]$cond, [string]$name) {
    if ($cond) { Write-Host "[PASS] $name" } else { Write-Host "[FAIL] $name"; $script:bad++ }
}

# Per-run unique temp root so a crashed previous run can't poison this one.
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("polyglot-refusals-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null

# Run `build <fixture> --target csharp --out <fresh dir>` with a stale sentinel twin pre-seeded, and
# return the merged output + whether the sentinel survived + the post-build file count.
function Invoke-RefusedBuild([string]$fixtureName) {
    $src = Join-Path $fixtures $fixtureName
    $stem = [System.IO.Path]::GetFileNameWithoutExtension($fixtureName)
    $out = Join-Path $work ($stem + "-out")
    Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $out | Out-Null
    $sentinel = "STALE-SENTINEL-$stem-DO-NOT-TOUCH"
    $twin = Join-Path $out ($stem + ".cs")
    Set-Content -Path $twin -Value $sentinel -NoNewline
    $output = & $Cli build $src --target csharp --out $out 2>&1 | Out-String
    $code = $LASTEXITCODE
    $twinAfter = Get-Content $twin -Raw -ErrorAction SilentlyContinue
    $fileCount = @(Get-ChildItem $out -File).Count
    return [pscustomobject]@{
        Output = $output; Exit = $code
        SentinelKept = ($twinAfter -eq $sentinel); FileCount = $fileCount
    }
}

# fixture -> ASCII message fragments the diagnostic must contain. ($null would mean a raw-parse-error
# fixture asserting only nonzero exit + no output — none remain: #54 gave the finalizer a targeted message.)
$cases = [ordered]@{
    "lock_stmt.pg"              = @("refuses threads and locks")
    "unsafe_block.pg"           = @("no unsafe blocks")
    "thread_type.pg"            = @("refuses threads and locks")
    "decimal_type.pg"           = @("refuses 'decimal'")
    "activator_type.pg"         = @("refuses runtime reflection")
    "dynamic_type.pg"           = @("refuses 'dynamic'")
    "await_outside_async.pg"    = @("'await' is only allowed inside an 'async fn'")
    "empty_list_uninferable.pg" = @("cannot infer the type of 'xs'")
    "null_var_uninferable.pg"   = @("cannot infer the type of 'x'")
    "finalizer.pg"              = @("refuses finalizers")
}

foreach ($fx in $cases.Keys) {
    $r = Invoke-RefusedBuild $fx
    Assert ($r.Exit -ne 0) "$fx : build exits nonzero"
    Assert ($r.Output -match $reDiag) "$fx : prints a path:line:col: error: diagnostic"
    $frags = $cases[$fx]
    if ($null -ne $frags) {
        foreach ($frag in $frags) {
            Assert ($r.Output -like "*$frag*") "$fx : diagnostic names the refusal ($frag)"
        }
    }
    Assert $r.SentinelKept "$fx : the stale pre-seeded twin is untouched (no overwrite)"
    Assert ($r.FileCount -eq 1) "$fx : no output file written (out dir holds only the pre-seed)"
}

# --- the 3-error fixture: every position is reported, not just the first ------------------------------
$three = Invoke-RefusedBuild "three_errors.pg"
$diagLines = @($three.Output -split "`r?`n" | Where-Object { $_ -match $reDiag })
Assert ($diagLines.Count -ge 3) "three_errors.pg : all three diagnostics reported (got $($diagLines.Count))"
Assert (($three.Output -like "*refuses threads and locks*") -and
        ($three.Output -like "*refuses runtime reflection*") -and
        ($three.Output -like "*refuses 'decimal'*")) `
    "three_errors.pg : each of the three distinct refusals is named"
# The three diagnostics sit on lines 3, 4, 5 of the fixture (after the 2-line header comment).
$linesReported = @($diagLines | ForEach-Object { if ($_ -match ':(\d+):\d+: error:') { [int]$Matches[1] } } | Sort-Object -Unique)
Assert (($linesReported -contains 3) -and ($linesReported -contains 4) -and ($linesReported -contains 5)) `
    "three_errors.pg : the three error positions are on distinct lines 3/4/5"

# --- polyglot check: good exits 0, broken exits nonzero -----------------------------------------------
$good = Join-Path $fixtures "good.pg"
& $Cli check $good *> $null
Assert ($LASTEXITCODE -eq 0) "check <good.pg> exits 0"
$broken = Join-Path $fixtures "three_errors.pg"
& $Cli check $broken *> $null
Assert ($LASTEXITCODE -ne 0) "check <broken.pg> exits nonzero"

# --- check --json: a machine-readable array with line/col/message per diagnostic ----------------------
# (Probed: `check --help`/`build --help` document --json as a line/col/severity/message array. There is
# NO `file` field — check is single-file, so the path is implicit; asserted below and noted in the summary.)
$jsonRaw = & $Cli check $broken --json 2>&1 | Out-String
Assert ($LASTEXITCODE -ne 0) "check --json on a broken file still exits nonzero"
$parsed = $null
try { $parsed = $jsonRaw | ConvertFrom-Json } catch { }
Assert ($null -ne $parsed) "check --json emits parseable JSON"
if ($null -ne $parsed) {
    $arr = @($parsed)
    Assert ($arr.Count -ge 3) "check --json is an array with all diagnostics ($($arr.Count))"
    $d0 = $arr[0]
    $hasLine = $null -ne $d0.PSObject.Properties['line']
    $hasCol = $null -ne $d0.PSObject.Properties['col']
    $hasMsg = $null -ne $d0.PSObject.Properties['message']
    Assert ($hasLine -and $hasCol -and $hasMsg) "check --json elements carry line/col/message fields"
    Assert (($d0.line -is [int]) -or ($d0.line -is [long])) "check --json 'line' is numeric"
    Assert (-not [string]::IsNullOrEmpty([string]$d0.message)) "check --json 'message' is non-empty"
}
# check --json on the good file: an empty array, exit 0.
$goodJson = & $Cli check $good --json 2>&1 | Out-String
Assert ($LASTEXITCODE -eq 0 -and $goodJson.Trim() -eq "[]") "check --json on a clean file is '[]' with exit 0"

# --- --json + --watch are mutually exclusive (exit 64, the CLI-usage code) ----------------------------
$jw = & $Cli check $broken --json --watch 2>&1 | Out-String
Assert ($LASTEXITCODE -eq 64 -and $jw -match 'cannot be combined') "check --json --watch is refused with exit 64"

Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue

Write-Host ""
if ($bad -eq 0) {
    Write-Host "Refusal gate green: every refused construct fails loudly and writes no output."
    exit 0
}
Write-Host "$bad refusal-gate assertion(s) failed."
exit 1
