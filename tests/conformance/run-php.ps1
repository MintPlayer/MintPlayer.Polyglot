#!/usr/bin/env pwsh
# PHP-backend differential conformance (P26 slice 1 — the PHP fidelity uplift).
#
# The PHP backend now covers the full §3.A surface EXCEPT operator overloading and async (declared `false` in
# the plugin — a program using them refuses at transpile with a §3.E diagnostic). This emits every program to
# PHP (+ C# as the oracle), runs both, and asserts identical stdout. C# is the oracle (same reason as
# run-python.ps1: it's the reference semantics).
#
# G28: the set of programs allowed to refuse on PHP is PINNED (see $expectedRefusers). A refusal by any other
# program FAILS the gate (a silent capability regression must not hide behind the refusal branch); a pinned
# refuser that starts transpiling WARNS loudly (the pin is stale — narrow it) but does not fail.
#
# Usage:  pwsh tests/conformance/run-php.ps1   (build the solution first; needs `php` + `dotnet`)

param(
    [string]$Cli = "$PSScriptRoot\..\..\x64\Debug\MintPlayer.Polyglot.Cli.exe",
    [string]$Php = "$env:LOCALAPPDATA\polyglot-toolchains\php\php.exe"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Cli)) {
    Write-Host "polyglot CLI not found at $Cli — build the solution first (see CLAUDE.md)."
    exit 2
}
if (-not (Test-Path $Php)) {
    $onPath = Get-Command php -ErrorAction SilentlyContinue
    if ($onPath) { $Php = $onPath.Source } else { Write-Host "php not found at $Php nor on PATH."; exit 2 }
}

$progDir = Join-Path $PSScriptRoot "programs"
# G31: per-run unique work root (fixed prefix + short GUID), removed loudly at end.
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("polyglot-conformance-php-" + [System.Guid]::NewGuid().ToString('N').Substring(0, 8))

# G28 pinned expected-refusal set: exactly the programs that legitimately refuse on the PHP surface today —
# operatorOverloading (vec2, operators_full, indexer_grid's indexer), async (async_await, async_compose), and
# a portable fn with no PHP `actual` (expect_actual, extern_ffi). Established by transpiling every program to
# PHP and recording the refusers. Add here ONLY when the PHP plugin gains a new deliberate gate.
$expectedRefusers = @(
    'async_await', 'async_compose', 'expect_actual', 'extern_ffi', 'indexer_grid', 'operators_full', 'vec2'
)

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

# Every top-level conformance program.
$programs = (Get-ChildItem $progDir -Filter *.pg | Sort-Object Name | ForEach-Object { $_.BaseName })

$dotnet = (Get-Command dotnet -ErrorAction SilentlyContinue)?.Source; if (-not $dotnet) { $dotnet = "dotnet" }

# G30 per-program timeout: run a generated program under a wall-clock cap. Reads stdout/stderr async BEFORE
# WaitForExit (avoids the pipe-deadlock when a child fills the pipe buffer), and on timeout kills the whole
# process tree. Returns raw stdout, exit code, and a TimedOut flag; with -MergeStdErr the child's stderr is
# folded into stdout (PHP crash text lands in the output for the failure message). Duplicated per runner by
# design — the conformance scripts are self-contained.
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
$refused = 0

# Transpile one program to PHP + C#, build+run the C# oracle and run the PHP, compare stdout. A refusal is
# only allowed for a pinned program (G28); an unpinned refusal fails, a pinned program that starts
# transpiling warns.
function Test-Program($name, $stem, $cliArgs) {
    $dir = Join-Path $work $name
    Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $dir | Out-Null

    $phpErr = (& $Cli @cliArgs --target php --lib io --out $dir 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) {
        # A capability refusal (§3.B/§3.E) — distinguish it from a real transpile crash.
        if ($phpErr -match "does not support|has no 'actual'") {
            if ($expectedRefusers -contains $name) {
                Write-Host "[PASS] $name  ->  (refused by design: capability/portability gate)"
                $script:refused++
                return $true
            }
            Write-Host "[FAIL] $name : refused on php but is not in the pinned expected-refusal set (fix the program, or add it to `$expectedRefusers with a reason)"
            return $false
        }
        Write-Host "[FAIL] $name (php transpile failed): $($phpErr.Trim())"
        return $false
    }
    # Transpiled cleanly. If it was pinned as a refuser, the pin is stale — warn loudly, then still validate it.
    if ($expectedRefusers -contains $name) {
        Write-Host "[WARN] $name was expected to refuse on php but now transpiles — update the pinned set"
    }

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
    $phpr = Invoke-WithTimeout -FilePath $Php -Arguments @((Join-Path $dir "$stem.php")) -MergeStdErr
    if ($phpr.TimedOut) { Write-Host "[FAIL] $name : timed out after 60s"; return $false }
    $phpOut = Format-Golden $phpr.Stdout
    if ($phpr.ExitCode -ne 0) { Write-Host "[FAIL] $name : generated PHP crashed at runtime (exit $($phpr.ExitCode)): $phpOut"; return $false }

    if ($cs -ne $phpOut) {
        Write-Host "[FAIL] $name : C# and PHP diverged"
        Write-Host "        C#:  $($cs -replace "`r?`n", ' | ')"
        Write-Host "        PHP: $($phpOut -replace "`r?`n", ' | ')"
        return $false
    }

    # G32: an optional committed golden pins the exact stdout. Both targets are asserted against it.
    $golden = Join-Path $progDir "$name.expected"
    if (Test-Path $golden) {
        $exp = Format-Golden (Get-Content $golden -Raw)
        if ((Format-Golden $cs) -ne $exp) { Write-Host "[FAIL] $name : csharp output differs from $name.expected"; return $false }
        if ((Format-Golden $phpOut) -ne $exp) { Write-Host "[FAIL] $name : php output differs from $name.expected"; return $false }
    }

    Write-Host "[PASS] $name  ->  $($phpOut -replace "`r?`n", ' | ')"
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
    Write-Host "All $count conformance program(s) agree across PHP and C# ($refused refused by design)."
    exit 0
}
Write-Host "$fail of $count PHP program(s) diverged."
exit 1
