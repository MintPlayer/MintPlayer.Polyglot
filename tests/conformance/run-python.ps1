#!/usr/bin/env pwsh
# Python-backend differential conformance (P9 third-backend validation).
#
# The Python backend now covers the FULL conformance suite (the P9-V spike grew it feature-by-feature from
# the walking skeleton). This emits every program to Python (+ C# as the oracle), runs both, and asserts
# identical stdout. C# is the oracle (not TS) because both are checked the same way and C# is the reference
# semantics; the C#/TS pair is covered by run-diff.ps1.
#
# Usage:  pwsh tests/conformance/run-python.ps1   (build the solution first; needs `python3` + `dotnet`)

param(
    [string]$Cli = "$PSScriptRoot\..\..\x64\Debug\MintPlayer.Polyglot.Cli.exe"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Cli)) {
    Write-Host "polyglot CLI not found at $Cli — build the solution first (see CLAUDE.md)."
    exit 2
}

$progDir = Join-Path $PSScriptRoot "programs"
# G31: per-run unique work root (fixed prefix + short GUID), removed loudly at end.
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("polyglot-conformance-python-" + [System.Guid]::NewGuid().ToString('N').Substring(0, 8))

# Every top-level conformance program (the Python backend now covers the full §3.A surface).
$programs = (Get-ChildItem $progDir -Filter *.pg | Sort-Object Name | ForEach-Object { $_.BaseName })

$csproj = @'
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net10.0</TargetFramework>
    <Nullable>disable</Nullable>
    <ImplicitUsings>disable</ImplicitUsings>
  </PropertyGroup>
</Project>
'@

$py = (Get-Command python3 -ErrorAction SilentlyContinue) ?? (Get-Command python -ErrorAction SilentlyContinue)
if (-not $py) { Write-Host "python3 not found on PATH."; exit 2 }
$dotnet = (Get-Command dotnet -ErrorAction SilentlyContinue)?.Source; if (-not $dotnet) { $dotnet = "dotnet" }

# G30 per-program timeout: run a generated program under a wall-clock cap. Reads stdout/stderr async BEFORE
# WaitForExit (avoids the pipe-deadlock when a child fills the pipe buffer), and on timeout kills the whole
# process tree. Returns raw stdout, exit code, and a TimedOut flag; stderr is discarded unless -MergeStdErr
# (preserving the `2>$null` property — build/runtime noise never pollutes program stdout). Duplicated per
# runner by design — the conformance scripts are self-contained.
function Invoke-WithTimeout {
    param(
        [string]$FilePath,
        [string[]]$Arguments,
        [int]$TimeoutSec = 60,
        [switch]$MergeStdErr
    )
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $FilePath
    foreach ($a in $Arguments) { $psi.ArgumentList.Add($a) }
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
        return [pscustomobject]@{ Stdout = ""; ExitCode = -1; TimedOut = $true }
    }
    $out = $outTask.GetAwaiter().GetResult()
    $err = $errTask.GetAwaiter().GetResult()
    if ($MergeStdErr) { $out = $out + $err }
    return [pscustomobject]@{ Stdout = $out; ExitCode = $p.ExitCode; TimedOut = $false }
}

# G32 golden normalization: CRLF -> LF, then strip trailing whitespace/newlines. Applied to both sides.
function Format-Golden([string]$s) {
    if ($null -eq $s) { return "" }
    return ($s -replace "`r`n", "`n").TrimEnd("`r", "`n", " ", "`t")
}

$fail = 0
$count = 0

# Transpile one program (single .pg or a multi-file entry) to Python + C#, build+run the C# oracle and run
# the Python, compare stdout. $stem is the emitted entry basename; $cliArgs is the `polyglot build …` argv.
# Multi-module (§4.5): the C# csproj globs every generated .cs (one assembly — proves no CS0101); Python
# runs the entry, which imports its siblings from the same directory.
function Test-Program($name, $stem, $cliArgs) {
    $dir = Join-Path $work $name
    Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $dir | Out-Null

    & $Cli @cliArgs --target python --lib io --out $dir | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] $name (python transpile failed)"; return $false }
    & $Cli @cliArgs --target csharp --lib io --out $dir | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] $name (csharp transpile failed)"; return $false }

    $csproj | Set-Content (Join-Path $dir "$stem.csproj")
    & dotnet build (Join-Path $dir "$stem.csproj") -c Release -v quiet --nologo *> $null
    $dll = Join-Path $dir "bin\Release\net10.0\$stem.dll"
    # Assert the generated C# compiled and both runtimes exited cleanly — a symmetric double-failure
    # (empty == empty) must not false-pass (issue #9 lesson; see run-diff.ps1).
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $dll)) { Write-Host "[FAIL] $name : generated C# did not compile"; return $false }
    $csr = Invoke-WithTimeout -FilePath $dotnet -Arguments @($dll)
    if ($csr.TimedOut) { Write-Host "[FAIL] $name : timed out after 60s"; return $false }
    # Fold CRLF->LF and trim the trailer so the compare is line-ending agnostic (the normalization
    # PowerShell's Out-String used to do implicitly before the timeout rewrite).
    $cs = Format-Golden $csr.Stdout
    if ($csr.ExitCode -ne 0) { Write-Host "[FAIL] $name : generated C# crashed at runtime (exit $($csr.ExitCode))"; return $false }
    $pyr = Invoke-WithTimeout -FilePath $py.Source -Arguments @((Join-Path $dir "$stem.py"))
    if ($pyr.TimedOut) { Write-Host "[FAIL] $name : timed out after 60s"; return $false }
    $pyOut = Format-Golden $pyr.Stdout
    if ($pyr.ExitCode -ne 0) { Write-Host "[FAIL] $name : generated Python crashed at runtime (exit $($pyr.ExitCode))"; return $false }

    if ($cs -ne $pyOut) {
        Write-Host "[FAIL] $name : C# and Python diverged"
        Write-Host "        C#: $($cs -replace "`r?`n", ' | ')"
        Write-Host "        Py: $($pyOut -replace "`r?`n", ' | ')"
        return $false
    }

    # G32: an optional committed golden pins the exact stdout. Both targets are asserted against it.
    $golden = Join-Path $progDir "$name.expected"
    if (Test-Path $golden) {
        $exp = Format-Golden (Get-Content $golden -Raw)
        if ((Format-Golden $cs) -ne $exp) { Write-Host "[FAIL] $name : csharp output differs from $name.expected"; return $false }
        if ((Format-Golden $pyOut) -ne $exp) { Write-Host "[FAIL] $name : python output differs from $name.expected"; return $false }
    }

    Write-Host "[PASS] $name  ->  $($pyOut -replace "`r?`n", ' | ')"
    return $true
}

try {
    # Single-file programs.
    foreach ($name in $programs) {
        $count++
        if (-not (Test-Program $name $name @("build", (Join-Path $progDir "$name.pg")))) { $fail++ }
    }
    # Multi-file programs (§4.5 module linking): each programs/<dir>/ with an entry.pg, built with --root <dir>.
    foreach ($d in Get-ChildItem $progDir -Directory | Sort-Object Name) {
        $entry = Join-Path $d.FullName "entry.pg"
        if (-not (Test-Path $entry)) { continue }
        $count++
        if (-not (Test-Program $d.Name "entry" @("build", $entry, "--root", $d.FullName))) { $fail++ }
    }
}
finally {
    # G31 loud cleanup.
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
    if (Test-Path $work) { Write-Host "WARNING: could not remove conformance work dir '$work' — clean it up manually." }
}

Write-Host ""
if ($fail -eq 0) {
    Write-Host "All $count conformance program(s) agree across Python and C#."
    exit 0
}
Write-Host "$fail of $count Python program(s) diverged."
exit 1
