<#
.SYNOPSIS
    Folds a plugin arm-coverage trace into per-manifest lcov reports.

.DESCRIPTION
    The four backends ARE their JSON manifests (zero backends are compiled in), so gcov and
    OpenCppCoverage cannot see them. `polyglot --emit-arm-trace` records which manifest arms the
    compiler parsed (S = the denominator) and which ones actually fired (H = the numerator); this
    script turns that into lcov, one record per `plugins/<target>/polyglot-plugin.json`.

    The manifests are pretty-printed at ~27 bytes/line and a rule's nested `case` arms occupy
    distinct lines, so ordinary line coverage over the JSON expresses arm-level branch coverage —
    in a format coverage.mintplayer.com already ingests, against the file a plugin author edits.

    Both halves of the fraction come from the same parse inside the compiler, so an arm that never
    fires is reported UNCOVERED rather than quietly missing from the total. Nothing here regexes
    the JSON to guess at a denominator.

    Directly invocable, no NX (the standing runner contract).

.PARAMETER Trace
    The trace file written by one or more `--emit-arm-trace` runs (appended across a sweep).

.PARAMETER OutDir
    Directory for the generated .lcov files. Created if absent.

.PARAMETER RepoRoot
    Repository root; manifest paths in the report are written relative to it, because the coverage
    server resolves them by longest-suffix match against `git ls-files`.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Trace,
    [string]$OutDir = 'coverage/plugins',
    [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Trace)) {
    Write-Error "arm-trace file not found: $Trace"
    exit 2
}

# plugin -> line -> hit?  A sweep appends hundreds of invocations, so dedup and take the max: a line
# seen as S by one invocation and H by another is covered.
$plugins = @{}
$records = 0

foreach ($line in [System.IO.File]::ReadLines((Resolve-Path -LiteralPath $Trace))) {
    if ([string]::IsNullOrWhiteSpace($line)) { continue }
    $parts = $line.Split("`t")
    if ($parts.Count -ne 3) {
        Write-Error "malformed arm-trace record (expected 3 tab-separated fields): $line"
        exit 2
    }
    $name = $parts[0]
    $lineNo = [int]$parts[1]
    $hit = $parts[2] -eq 'H'

    if (-not $plugins.ContainsKey($name)) { $plugins[$name] = @{} }
    $table = $plugins[$name]
    if ($table.ContainsKey($lineNo)) {
        if ($hit) { $table[$lineNo] = $true }
    } else {
        $table[$lineNo] = $hit
    }
    $records++
}

if ($records -eq 0) {
    Write-Error "arm-trace file is empty: $Trace — the sweep produced no records"
    exit 2
}

if (-not (Test-Path -LiteralPath $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }

$written = 0
foreach ($name in ($plugins.Keys | Sort-Object)) {
    # Only in-repo manifests get a report: a plugin resolved from the npm cache has no path in this
    # repository, so the server could not match it against `git ls-files` anyway.
    $relative = "plugins/$name/polyglot-plugin.json"
    if (-not (Test-Path -LiteralPath (Join-Path $RepoRoot $relative))) {
        Write-Host "  skip '$name' — no in-repo manifest at $relative"
        continue
    }

    $table = $plugins[$name]
    $sorted = $table.Keys | Sort-Object
    $hitCount = 0

    $sb = [System.Text.StringBuilder]::new()
    [void]$sb.AppendLine('TN:')
    [void]$sb.AppendLine("SF:$relative")
    foreach ($lineNo in $sorted) {
        $covered = if ($table[$lineNo]) { 1 } else { 0 }
        if ($table[$lineNo]) { $hitCount++ }
        [void]$sb.AppendLine("DA:$lineNo,$covered")
    }
    [void]$sb.AppendLine("LF:$($sorted.Count)")
    [void]$sb.AppendLine("LH:$hitCount")
    [void]$sb.AppendLine('end_of_record')

    $outFile = Join-Path $OutDir "$name.lcov"
    [System.IO.File]::WriteAllText($outFile, $sb.ToString())

    $pct = if ($sorted.Count -gt 0) { [math]::Round(100.0 * $hitCount / $sorted.Count, 1) } else { 0 }
    Write-Host ("  {0,-12} {1,5}/{2,-5} arms ({3,5}%) -> {4}" -f $name, $hitCount, $sorted.Count, $pct, $outFile)
    $written++
}

if ($written -eq 0) {
    Write-Error 'no in-repo plugin manifests matched the trace — nothing written'
    exit 2
}

Write-Host "arm-trace: $records records -> $written lcov report(s) in $OutDir"
