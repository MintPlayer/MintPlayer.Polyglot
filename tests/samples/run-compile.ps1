#!/usr/bin/env pwsh
# Sample compile gate (P13): every docs/lang/samples/*.pg must transpile end-to-end with the polyglot CLI.
#
# The samples are the language's worked examples; before P13 they `import { print } from "std.io"` /
# `{ Math } from "std.math"` — imports that didn't resolve (print/Math were hardcoded builtins), so the
# samples were only *formatted* by the fidelity gate, never compiled. P13 made print/Math real std modules,
# so the samples now compile. This gate locks that in: it builds each sample (no `--lib`, so the samples
# must carry their own imports — they are meant to be self-contained) and asserts success.
#
# Two samples are known-blocked on capabilities that are NOT part of the std-module migration; they are
# listed as expected-failures (xfail) with the gap they need. The gate is green only when the xfail set
# fails for exactly that reason and everything else compiles — so an "unexpected pass" flags that an xfail
# can be retired, and an "unexpected fail" flags a regression.
#
# Usage:  pwsh tests/samples/run-compile.ps1   (build the solution first; see CLAUDE.md)

param(
    [string]$Cli = "$PSScriptRoot\..\..\x64\Debug\MintPlayer.Polyglot.Cli.exe"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Cli)) {
    Write-Host "polyglot CLI not found at $Cli — build the solution first (see CLAUDE.md)."
    exit 2
}

$sampleDir = Join-Path $PSScriptRoot "..\..\docs\lang\samples"
$work = Join-Path ([System.IO.Path]::GetTempPath()) "polyglot-samples"
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $work | Out-Null

# Known-blocked samples -> the missing capability (a follow-up, not the std-module migration). Keep the
# reason precise so it's obvious when the gap is closed and the entry should be removed.
$xfail = @{
    "06_exceptions.pg" = "Error.message: base-class member resolution + a per-target Error.message binding (C# .Message / JS .message)"
    "08_extensions.pg" = "extension on a generic receiver: 'extension fn List<T>.m()' does not scope the receiver type variable T"
}

$bad = 0
$count = 0
foreach ($f in Get-ChildItem $sampleDir -Filter *.pg | Sort-Object Name) {
    $count++
    & $Cli build $f.FullName --out $work *> $null
    $ok = ($LASTEXITCODE -eq 0)
    $expectFail = $xfail.ContainsKey($f.Name)

    if ($expectFail -and -not $ok) {
        Write-Host "[XFAIL] $($f.Name)  ($($xfail[$f.Name]))"
    } elseif ($expectFail -and $ok) {
        Write-Host "[UNEXPECTED PASS] $($f.Name) — the gap is closed; remove it from the xfail list."
        $bad++
    } elseif (-not $expectFail -and $ok) {
        Write-Host "[PASS] $($f.Name)"
    } else {
        Write-Host "[FAIL] $($f.Name) — transpile failed (regression)."
        $bad++
    }
}

Write-Host ""
if ($bad -eq 0) {
    Write-Host "All $count sample(s) accounted for ($($xfail.Count) expected-fail, $($count - $xfail.Count) compile)."
    exit 0
}
Write-Host "$bad sample(s) in an unexpected state."
exit 1
