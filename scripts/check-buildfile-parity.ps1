#!/usr/bin/env pwsh
# Build-file drift guard (PLAN §P22 slice 2). The POSIX CMakeLists.txt globs source files; the three
# Windows .vcxproj list them explicitly. If the two disagree, one build system silently compiles a
# different set than the other. This asserts, per project, that the .vcxproj <ClCompile> set exactly
# equals the .cpp files on disk (which is what CMake's file(GLOB) picks up). Run in CI on every platform.

$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent

$projects = @(
    @{ Name = "Core";  Vcxproj = "src/MintPlayer.Polyglot.Core/MintPlayer.Polyglot.Core.vcxproj";   Src = "src/MintPlayer.Polyglot.Core/src" }
    @{ Name = "Cli";   Vcxproj = "src/MintPlayer.Polyglot.Cli/MintPlayer.Polyglot.Cli.vcxproj";     Src = "src/MintPlayer.Polyglot.Cli/src" }
    @{ Name = "Tests"; Vcxproj = "tests/MintPlayer.Polyglot.Tests/MintPlayer.Polyglot.Tests.vcxproj"; Src = "tests/MintPlayer.Polyglot.Tests/src" }
)

$fail = 0
foreach ($p in $projects) {
    $vcxprojPath = Join-Path $root $p.Vcxproj
    $srcDir = Join-Path $root $p.Src

    # <ClCompile Include="src\foo.cpp" /> — take the basename so the path separator is irrelevant.
    $xml = [xml](Get-Content $vcxprojPath)
    $ns = New-Object System.Xml.XmlNamespaceManager($xml.NameTable)
    $ns.AddNamespace("m", "http://schemas.microsoft.com/developer/msbuild/2003")
    $declared = @($xml.SelectNodes("//m:ClCompile[@Include]", $ns) |
        ForEach-Object { Split-Path $_.Include -Leaf } | Sort-Object -Unique)

    $onDisk = @(Get-ChildItem -Path $srcDir -Filter *.cpp -File |
        ForEach-Object { $_.Name } | Sort-Object -Unique)

    $missingFromVcxproj = @($onDisk | Where-Object { $_ -notin $declared })
    $missingFromDisk = @($declared | Where-Object { $_ -notin $onDisk })

    if ($missingFromVcxproj.Count -eq 0 -and $missingFromDisk.Count -eq 0) {
        Write-Host "[PASS] $($p.Name): $($onDisk.Count) source(s) agree (.vcxproj <-> disk/CMake glob)"
    } else {
        $fail++
        Write-Host "[FAIL] $($p.Name): .vcxproj and disk disagree"
        if ($missingFromVcxproj.Count) { Write-Host "   on disk but NOT in .vcxproj (Windows build would miss): $($missingFromVcxproj -join ', ')" }
        if ($missingFromDisk.Count)    { Write-Host "   in .vcxproj but NOT on disk (stale entry): $($missingFromDisk -join ', ')" }
    }
}

# Version-injection parity (PRD §4.16 / PLAN §P24): the POLYGLOT_VERSION define must exist on the Core project
# in BOTH build systems, or one build silently ships the dev-fallback version. The source-list check above
# can't see a preprocessor define, so assert it explicitly here.
$coreVcxproj = Get-Content (Join-Path $root "src/MintPlayer.Polyglot.Core/MintPlayer.Polyglot.Core.vcxproj") -Raw
$cmake = Get-Content (Join-Path $root "CMakeLists.txt") -Raw
if ($coreVcxproj -match 'POLYGLOT_VERSION=\$\(PolyglotVersion\)') {
    Write-Host "[PASS] Version define: Core .vcxproj carries POLYGLOT_VERSION=`$(PolyglotVersion)"
} else {
    $fail++; Write-Host "[FAIL] Version define: Core .vcxproj is missing POLYGLOT_VERSION=`$(PolyglotVersion)"
}
if ($cmake -match 'target_compile_definitions\(polyglot-core[^)]*POLYGLOT_VERSION=') {
    Write-Host "[PASS] Version define: CMakeLists.txt sets POLYGLOT_VERSION on polyglot-core"
} else {
    $fail++; Write-Host "[FAIL] Version define: CMakeLists.txt is missing POLYGLOT_VERSION on polyglot-core"
}

Write-Host ""
if ($fail -eq 0) {
    Write-Host "Build-file parity holds: CMake glob and the .vcxproj compile the same sources + agree on the version define."
    exit 0
}
Write-Host "$fail project(s) drifted — reconcile the .vcxproj <ClCompile> list with the .cpp files on disk."
exit 1
