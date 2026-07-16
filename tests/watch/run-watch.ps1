#!/usr/bin/env pwsh
# Watch-mode golden gate (P21 slice 2, PRD §4.13): freezes the `--watch` console protocol.
#
# The regexes asserted here are THE SAME ONES the VS Code extension's `$polyglot-watch` background
# problemMatcher ships. Protocol drift must break this gate before it silently empties an editor's
# Problems panel:
#
#   begin sentinel:   [HH:MM:SS] polyglot watch: building|rebuilding <abs entry>
#   diagnostic line:  <ABSPATH>(<line>,<col>): error|warning|info: <message>     (absolute path!)
#   end sentinel:     [HH:MM:SS] polyglot watch: N error(s) - watching for changes   (ASCII hyphen)
#
# The scenario also locks the semantics: initial build on start; rebuild on an edit to an IMPORTED
# module (the transitive closure is watched, not just the entry); a broken edit keeps watching, prints
# the diagnostic, and NEVER touches the last-good outputs; fixing it recovers.
#
# Usage:  pwsh tests/watch/run-watch.ps1   (build the solution first; see CLAUDE.md)

param(
    [string]$Cli = "$PSScriptRoot\..\..\x64\Debug\MintPlayer.Polyglot.Cli.exe"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Cli)) {
    Write-Host "polyglot CLI not found at $Cli — build the solution first (see CLAUDE.md)."
    exit 2
}

# --- the frozen protocol (single source of truth for this gate) --------------------------------------
$reBegin = '^\[\d{2}:\d{2}:\d{2}\] polyglot watch: (building|rebuilding) '
$reEnd   = '^\[\d{2}:\d{2}:\d{2}\] polyglot watch: (\d+) error\(s\) - watching for changes$'
$reDiag  = '^(.+?)\((\d+),(\d+)\): (error|warning|info): (.+)$'

$bad = 0
function Assert([bool]$cond, [string]$name) {
    if ($cond) { Write-Host "[PASS] $name" } else { Write-Host "[FAIL] $name"; $script:bad++ }
}

# --- workspace ----------------------------------------------------------------------------------------
$work = Join-Path ([System.IO.Path]::GetTempPath()) "polyglot-watch-gate"
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $work | Out-Null
$entry = Join-Path $work "main.pg"
$util  = Join-Path $work "util.pg"
$log   = Join-Path $work "watch.log"

Set-Content -Path $util -Value "fn area(w: i32, h: i32): i32 {`n  return w * h`n}`n" -NoNewline
Set-Content -Path $entry -Value "import { area } from `"./util`"`nimport { print } from `"std.io`"`nfn main() {`n  print(area(3, 4))`n}`n" -NoNewline
# P30: the target set is config-sourced — no pgconfig + no --target refuses, so the workspace declares it.
Set-Content -Path (Join-Path $work "pgconfig.json") -Value "{ `"targets`": [`"csharp`", `"typescript`"] }" -NoNewline

# Wait until the watch log contains at least $n end sentinels (i.e. $n completed cycles).
function Wait-Cycles([int]$n, [int]$timeoutSec = 20) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        $lines = @(Get-Content $log -ErrorAction SilentlyContinue)
        if (@($lines | Where-Object { $_ -match $reEnd }).Count -ge $n) { return $lines }
        Start-Sleep -Milliseconds 200
    }
    return @(Get-Content $log -ErrorAction SilentlyContinue)
}

$proc = Start-Process -FilePath $Cli -ArgumentList "build", $entry, "--watch" `
    -RedirectStandardOutput $log -RedirectStandardError (Join-Path $work "watch.err") `
    -NoNewWindow -PassThru
try {
    # --- cycle 1: the initial build ---------------------------------------------------------------------
    $lines = Wait-Cycles 1
    Assert (@($lines).Count -gt 0 -and $lines[0] -match $reBegin -and $Matches[1] -eq 'building') `
        "initial cycle opens with the 'building' sentinel"
    $end1 = @($lines | Where-Object { $_ -match $reEnd }) | Select-Object -First 1
    Assert ($null -ne $end1 -and $end1 -match $reEnd -and $Matches[1] -eq '0') `
        "initial cycle closes with '0 error(s)'"
    Assert ((Test-Path (Join-Path $work "main.cs")) -and (Test-Path (Join-Path $work "main.ts"))) `
        "initial build wrote both default outputs"
    # §4.5 module linking: the imported module emits its OWN file (util.cs) rather than being inlined into main.
    Assert ((Test-Path (Join-Path $work "util.cs")) -and (Test-Path (Join-Path $work "util.ts"))) `
        "initial build wrote the imported module's own linked outputs"
    $goodUtil = Get-Content (Join-Path $work "util.cs") -Raw

    # --- cycle 2: edit the IMPORTED module (closure watching, not just the entry) -----------------------
    Set-Content -Path $util -Value "fn area(w: i32, h: i32): i32 {`n  return w * h * 2`n}`n" -NoNewline
    $lines = Wait-Cycles 2
    Assert (@($lines | Where-Object { $_ -match $reBegin -and $_ -match 'rebuilding' }).Count -ge 1) `
        "an edit to an imported .pg triggers a 'rebuilding' cycle"
    # Under linking `area`'s body lives in util.cs (main.cs only references it), so the imported module's
    # own output is what refreshes.
    $utilAfterEdit = Get-Content (Join-Path $work "util.cs") -Raw
    Assert ($utilAfterEdit -ne $goodUtil -and $utilAfterEdit.Contains("* 2")) `
        "the rebuild refreshed the imported module's emitted output"
    $csAfterEdit = Get-Content (Join-Path $work "main.cs") -Raw  # last-good entry output going into the broken cycle

    # --- cycle 3: a broken edit keeps watching and never touches last-good outputs ----------------------
    Set-Content -Path $entry -Value "import { area } from `"./util`"`nimport { print } from `"std.io`"`nfn main() {`n  print(area(3, oops))`n}`n" -NoNewline
    $lines = Wait-Cycles 3
    $diag = @($lines | Where-Object { $_ -match $reDiag }) | Select-Object -First 1
    Assert ($null -ne $diag) "a broken edit prints a matcher-parseable diagnostic line"
    if ($null -ne $diag) {
        $null = $diag -match $reDiag
        Assert ([System.IO.Path]::IsPathRooted($Matches[1])) "the diagnostic path is absolute"
        Assert ($Matches[4] -eq 'error') "the diagnostic severity is 'error'"
    }
    $end3 = @($lines | Where-Object { $_ -match $reEnd }) | Select-Object -Last 1
    Assert ($null -ne $end3 -and $end3 -match $reEnd -and [int]$Matches[1] -ge 1) `
        "the failing cycle's end sentinel reports a nonzero error count"
    Assert ((Get-Content (Join-Path $work "main.cs") -Raw) -eq $csAfterEdit) `
        "a failed rebuild leaves the last-good output untouched"
    Assert (-not $proc.HasExited) "the watcher keeps running after a failed rebuild"

    # --- cycle 4: fixing the error recovers -------------------------------------------------------------
    Set-Content -Path $entry -Value "import { area } from `"./util`"`nimport { print } from `"std.io`"`nfn main() {`n  print(area(3, 5))`n}`n" -NoNewline
    $lines = Wait-Cycles 4
    $end4 = @($lines | Where-Object { $_ -match $reEnd }) | Select-Object -Last 1
    Assert ($null -ne $end4 -and $end4 -match $reEnd -and $Matches[1] -eq '0') `
        "fixing the error recovers to a '0 error(s)' cycle"
    Assert ((Get-Content (Join-Path $work "main.cs") -Raw).Contains("area(3, 5)")) `
        "the recovery cycle refreshed the output"

    # --- cycles 5-6: an unresolved import's candidate path is polled (slice 3) --------------------------
    Set-Content -Path $entry -Value "import { area } from `"./util`"`nimport { boost } from `"./extra`"`nimport { print } from `"std.io`"`nfn main() {`n  print(area(3, 5))`n}`n" -NoNewline
    $lines = Wait-Cycles 5
    $end5 = @($lines | Where-Object { $_ -match $reEnd }) | Select-Object -Last 1
    Assert ($null -ne $end5 -and $end5 -match $reEnd -and [int]$Matches[1] -ge 1) `
        "an import of a not-yet-existing file fails the cycle"
    Set-Content -Path (Join-Path $work "extra.pg") -Value "fn boost(x: i32): i32 {`n  return x`n}`n" -NoNewline
    $lines = Wait-Cycles 6
    $end6 = @($lines | Where-Object { $_ -match $reEnd }) | Select-Object -Last 1
    Assert ($null -ne $end6 -and $end6 -match $reEnd -and $Matches[1] -eq '0') `
        "creating the missing import target triggers a green rebuild"

    # --- cycle 7: creating a pgconfig.json re-resolves the context (target set shrinks) -----------------
    Remove-Item (Join-Path $work "main.cs"), (Join-Path $work "main.ts") -Force
    Set-Content -Path (Join-Path $work "pgconfig.json") -Value "{ `"targets`": [`"csharp`"] }" -NoNewline
    $lines = Wait-Cycles 7
    Assert (Test-Path (Join-Path $work "main.cs")) "the pgconfig-triggered cycle emitted the configured target"
    Assert (-not (Test-Path (Join-Path $work "main.ts"))) `
        "a pgconfig 'targets' set replaces the default pair (no stray .ts)"

    # --- cycle 8: EDITING the active pgconfig.json re-resolves again (target set grows back) ------------
    Set-Content -Path (Join-Path $work "pgconfig.json") -Value "{ `"targets`": [`"csharp`", `"typescript`"] }" -NoNewline
    $lines = Wait-Cycles 8
    Assert (Test-Path (Join-Path $work "main.ts")) "editing the active pgconfig re-resolves the target set"

    # --- protocol sweep: every line in the log is one of the known shapes -------------------------------
    $known = @($lines | Where-Object {
        $_ -match $reBegin -or $_ -match $reEnd -or $_ -match $reDiag -or $_ -match '^  -> ' -or $_ -eq ''
    })
    Assert (@($known).Count -eq @($lines).Count) `
        "every watch-stream line is a sentinel, a diagnostic, or an output line (no strays)"
} finally {
    if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
}

$errLog = Get-Content (Join-Path $work "watch.err") -Raw -ErrorAction SilentlyContinue
Assert ([string]::IsNullOrWhiteSpace($errLog)) "the watch stream is stdout-only (stderr empty)"

Write-Host ""
if ($bad -eq 0) {
    Write-Host "Watch gate green: the --watch protocol and semantics hold."
    exit 0
}
Write-Host "$bad watch assertion(s) failed."
exit 1
