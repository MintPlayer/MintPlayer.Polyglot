#requires -Version 7
<#
.SYNOPSIS
  P39 / issue #71 — the `polyglot coverage remap` gate leg.

.DESCRIPTION
  Two halves, both hermetic:

   1. LIVE — every target that declares an `originMapping` emits its origin sink when built with
      --origin-info (sidecar + footer for sourceMapV3; directives for `directive`). This is the P39
      slice-1 acceptance: Python and PHP gained a sink from a MANIFEST ENTRY with no engine change, so a
      regression here means someone reintroduced a target-name comparison or broke the generic footer.

   2. GOLDEN — remap committed, REAL `coverage.py` reports (cobertura and lcov, captured from an actual
      `coverage run` over the generated Python) through committed generated output + sidecar, and compare
      against committed expected `.pg`-keyed output.

  The fixtures are checked in precisely so this leg needs NO Python, no PHP and no network — unlike the
  nuget leg it has no toolchain guard, because it has no toolchain.

  Exit codes: 0 pass, 1 assertion failed, 2 bad invocation / missing input.
#>
[CmdletBinding()]
param(
    [string]$Cli,
    [switch]$UpdateExpected   # re-record the expected outputs after an intentional change
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Resolve-Path (Join-Path $here '..' '..')

if (-not $Cli) {
    $Cli = Join-Path $repo 'x64/Debug/MintPlayer.Polyglot.Cli.exe'
    if (-not (Test-Path $Cli)) { $Cli = Join-Path $repo 'build/polyglot' }
}
if (-not (Test-Path $Cli)) {
    Write-Error "coverage: CLI not found (looked for x64/Debug and build/polyglot); pass -Cli"
    exit 2
}

$failures = 0
function Check([bool]$ok, [string]$name) {
    if ($ok) { Write-Host "[PASS] $name" }
    else { Write-Host "[FAIL] $name" -ForegroundColor Red; $script:failures++ }
}

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("pg-coverage-" + [guid]::NewGuid().ToString('n').Substring(0, 8))
New-Item -ItemType Directory -Path $work -Force | Out-Null

try {
    # ---------------------------------------------------------------------------------------------
    # 1. LIVE: each target's declared origin sink actually lands.
    $sample = Join-Path $repo 'docs/lang/samples/03_enums_unions_match.pg'
    foreach ($t in @(
            @{ name = 'python';     ext = '.py';  sink = 'sidecar' },
            @{ name = 'php';        ext = '.php'; sink = 'sidecar' },
            @{ name = 'typescript'; ext = '.ts';  sink = 'sidecar' },
            @{ name = 'csharp';     ext = '.cs';  sink = 'directive' })) {
        $out = Join-Path $work $t.name
        New-Item -ItemType Directory -Path $out -Force | Out-Null
        & $Cli build $sample --target $t.name --out $out --origin-info > $null 2>&1
        $emitted = Join-Path $out ("03_enums_unions_match" + $t.ext)
        if ($t.sink -eq 'sidecar') {
            $map = "$emitted.map"
            Check ((Test-Path $map) -and ((Get-Content $map -Raw) -match '"version"\s*:\s*3')) `
                "$($t.name): --origin-info writes a v3 sidecar"
            # The footer is manifest data with no comment-syntax knowledge in the CLI, so a target that
            # declares one must get it in its OWN comment syntax.
            Check ((Get-Content $emitted -Raw) -match 'sourceMappingURL=03_enums_unions_match') `
                "$($t.name): the emitted file points at its sidecar"
        } else {
            Check ((Get-Content $emitted -Raw) -match '#line\s+\d+') `
                "$($t.name): --origin-info writes per-line directives"
        }
    }

    # PHP never emits a closing `?>`, so the footer lands after the last statement and stays inside PHP
    # mode. If that ever changes, the footer moves into HTML output and silently stops being a comment.
    $php = Get-Content (Join-Path $work 'php/03_enums_unions_match.php') -Raw
    Check (-not ($php -match '\?>')) "php: output never leaves PHP mode, so the footer stays a comment"

    # ---------------------------------------------------------------------------------------------
    # 2. GOLDEN: real coverage.py reports projected onto the .pg.
    $gen = Join-Path $here 'fixtures/generated'
    foreach ($case in @(
            @{ report = 'py.cobertura.xml'; expected = 'pg.cobertura.xml'; fmt = 'cobertura' },
            @{ report = 'py.lcov';          expected = 'pg.lcov';          fmt = 'lcov' })) {
        $input = Join-Path $here "fixtures/reports/$($case.report)"
        $actual = Join-Path $work $case.expected
        & $Cli coverage remap $input --target python --generated-dir $gen --root $repo --out $actual > $null 2>&1
        if ($LASTEXITCODE -ne 0) {
            Check $false "coverage remap ($($case.fmt)) exits 0"
            continue
        }
        $expectedPath = Join-Path $here "expected/$($case.expected)"
        if ($UpdateExpected) {
            Copy-Item $actual $expectedPath -Force
            Write-Host "[UPDATED] $($case.expected)"
            continue
        }
        $a = (Get-Content $actual -Raw) -replace "`r`n", "`n"
        $e = (Get-Content $expectedPath -Raw) -replace "`r`n", "`n"
        Check ($a -eq $e) "coverage remap ($($case.fmt)) matches the expected .pg-keyed report"
        Check ($a -match 'docs/lang/samples/03_enums_unions_match\.pg') `
            "coverage remap ($($case.fmt)) keys the report on a repo-relative .pg path"
    }

    if (-not $UpdateExpected) {
        # Branch data is emitted COUNT-ONLY: arm keys from two targets are not comparable, so a union
        # would over-credit whenever both suites cover the same arm. lcov's BRDA is arm-keyed, so it must
        # not appear unless --branch-arms was asked for.
        $lcov = Get-Content (Join-Path $work 'pg.lcov') -Raw
        Check (-not ($lcov -match 'BRDA:')) "lcov output carries no arm records by default"

        # No origin data at all must fail loudly. Silently emitting an empty report is the failure mode
        # this whole design exists to avoid: it reads downstream as "nothing was covered".
        $empty = Join-Path $work 'empty'
        New-Item -ItemType Directory -Path $empty -Force | Out-Null
        & $Cli coverage remap (Join-Path $here 'fixtures/reports/py.lcov') --target python `
            --generated-dir $empty --root $repo --out (Join-Path $work 'nope.lcov') > $null 2>&1
        Check ($LASTEXITCODE -ne 0) "output with no origin data fails loudly rather than emitting nothing"
        Check (-not (Test-Path (Join-Path $work 'nope.lcov'))) "a failed remap writes no output file"
    }
} finally {
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failures -gt 0) {
    Write-Host "`ncoverage: $failures check(s) failed." -ForegroundColor Red
    exit 1
}
Write-Host "`ncoverage: all checks passed."
exit 0
