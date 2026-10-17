#!/usr/bin/env pwsh
# P40 A3 — the number-text corpus gate. The one stringification rule (SPEC §9: f64 prints as ECMAScript
# Number::toString) is checked over ~247k doubles against Node's own `String(x)`:
#   * C#     — the csharp plugin's shipped `__PgRt.Str(double)` (exact Burger–Dybvig over BigInteger; never
#              "R", which is wrong on .NET Framework and on .NET 10.0.13), under a hostile current culture;
#   * Python — the python plugin's shipped `_pg_str` (repr digits + the ECMAScript layout);
#   * PHP    — the php plugin's shipped `__pg_str`, run with a hostile `precision=17 serialize_precision=17`.
# The helpers are read straight out of the plugin manifests, so the gate tests what the transpiler emits.
# The corpus is generated per run by a SEEDED generator (gen-corpus.js), so nothing large is committed.
#
# Usage: pwsh tests/numfmt/run-numfmt.ps1 [-Php <path>]
# Needs node + dotnet; python and php legs are skipped (with a notice) when the runtime is missing.

param([string]$Php = "")

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $repo "scripts\lib\OracleCompile.ps1")

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("polyglot-numfmt-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null
$fail = 0
try {
    $corpus = Join-Path $work "corpus.txt"
    & node (Join-Path $PSScriptRoot "gen-corpus.js") $corpus
    if ($LASTEXITCODE -ne 0) { throw "corpus generation failed" }

    # C#: the runner + the shipped prelude members, in ONE file (the class is `file`-local).
    $cs = Get-Content -Raw (Join-Path $repo "plugins\csharp\polyglot-plugin.json") | ConvertFrom-Json
    $src = (Get-Content -Raw (Join-Path $PSScriptRoot "Runner.cs.in")) +
           "`nfile static class __PgRt`n{`n" + $cs.spec.preludes.pgStrFloat + "}`n"
    $runnerCs = Join-Path $work "Runner.cs"
    Set-Content -Path $runnerCs -Value $src -NoNewline
    $r = Invoke-OracleCompile -Sources @($runnerCs) -OutDll (Join-Path $work "bin\numfmt.dll") -NullableEnable -WarnAsError
    if (-not $r.Ok) { Write-Host "[FAIL] C# runner did not compile:"; Write-Host $r.Output; $fail++ }
    else {
        & dotnet $r.Dll $corpus
        if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] C# __PgRt.Str(double)"; $fail++ } else { Write-Host "[PASS] C# __PgRt.Str(double)" }
    }

    $py = (Get-Command python -ErrorAction SilentlyContinue)?.Source
    if (-not $py) { $py = (Get-Command python3 -ErrorAction SilentlyContinue)?.Source }
    if ($py) {
        & $py -I (Join-Path $PSScriptRoot "runner.py") (Join-Path $repo "plugins\python\polyglot-plugin.json") $corpus
        if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] Python _pg_str"; $fail++ } else { Write-Host "[PASS] Python _pg_str" }
    } else { Write-Host "   (Python leg skipped: python not found on PATH)" }

    if (-not $Php) { $Php = (Get-Command php -ErrorAction SilentlyContinue)?.Source }
    if ($Php) {
        & $Php -d precision=17 -d serialize_precision=17 (Join-Path $PSScriptRoot "runner.php") (Join-Path $repo "plugins\php\polyglot-plugin.json") $corpus
        if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] PHP __pg_str"; $fail++ } else { Write-Host "[PASS] PHP __pg_str" }
    } else { Write-Host "   (PHP leg skipped: php not found on PATH)" }
} finally {
    Stop-OracleBuildServer
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
if ($fail -ne 0) { Write-Host "`nNUMBER-TEXT CORPUS: $fail leg(s) failed."; exit 1 }
Write-Host "`nNumber-text corpus green."
exit 0
