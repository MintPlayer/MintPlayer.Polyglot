# OracleCompile.ps1 — the shared C#-oracle compiler for the test gate (P35 slice 2).
#
# Compiles emitted C# straight through Roslyn (`csc /shared`) instead of a per-program
# `dotnet build` (~0.2-0.6 s vs ~2 s: no MSBuild evaluation, no NuGet restore, and the /shared
# compile server amortizes Roslyn startup across the whole gate). Output verified byte-identical
# to the MSBuild-built oracle. The produced dll runs under `dotnet <dll>` via a minimal
# runtimeconfig (rollForward latestMinor).
#
# Escape hatch: POLYGLOT_ORACLE=msbuild routes Invoke-OracleCompile through the classic
# per-program `dotnet build` (the CI canary leg keeps csc-vs-SDK semantics honest).
#
# Dot-source this file, then:
#   $r = Invoke-OracleCompile -Sources @("prog.cs") -OutDll "bin\prog.dll" [-NullableEnable] [-WarnAsError]
#   if ($r.Ok) { dotnet $r.Dll }
#   ...
#   Stop-OracleBuildServer   # once, in the caller's finally

$script:OracleCtx = $null

function Get-OracleContext {
    if ($script:OracleCtx) { return $script:OracleCtx }

    $dotnet = (Get-Command dotnet -ErrorAction Stop).Source
    $dotnetRoot = Split-Path $dotnet -Parent

    # The ACTIVE SDK — the one `dotnet` itself resolves (respects global.json), NOT the newest listed.
    $sdkVersion = (& $dotnet --version).Trim()
    $sdkLine = (& $dotnet --list-sdks) | Where-Object { $_.StartsWith("$sdkVersion ") } | Select-Object -First 1
    if (-not $sdkLine) { throw "OracleCompile: active SDK $sdkVersion not found in 'dotnet --list-sdks'" }
    $sdkBase = $sdkLine.Substring($sdkLine.IndexOf('[') + 1).TrimEnd(']')
    $cscDll = Join-Path (Join-Path (Join-Path $sdkBase $sdkVersion) "Roslyn\bincore") "csc.dll"
    if (-not (Test-Path $cscDll)) { throw "OracleCompile: csc.dll not found at $cscDll" }

    # Reference assemblies: the ref pack matching the SDK's major (the runners' csprojs target
    # net<major>.0 with the same convention).
    $major = $sdkVersion.Split('.')[0]
    $tfm = "net$major.0"
    $refRoot = Join-Path $dotnetRoot "packs\Microsoft.NETCore.App.Ref"
    $refPack = Get-ChildItem $refRoot -Directory -ErrorAction Stop |
        Where-Object { $_.Name.StartsWith("$major.") } |
        Sort-Object { [version]($_.Name -replace '-.*$', '') } | Select-Object -Last 1
    if (-not $refPack) { throw "OracleCompile: no Microsoft.NETCore.App.Ref pack for major $major under $refRoot" }
    $refDir = Join-Path $refPack.FullName "ref\$tfm"
    if (-not (Test-Path $refDir)) { throw "OracleCompile: ref dir missing: $refDir" }

    # One rsp with every reference, reused for the whole gate run.
    $rsp = Join-Path ([System.IO.Path]::GetTempPath()) "polyglot-oracle-$sdkVersion.rsp"
    if (-not (Test-Path $rsp)) {
        $lines = Get-ChildItem $refDir -Filter *.dll | ForEach-Object { "/reference:`"$($_.FullName)`"" }
        Set-Content -Path $rsp -Value $lines -Encoding UTF8
    }

    $script:OracleCtx = @{
        Dotnet = $dotnet
        CscDll = $cscDll
        Rsp = $rsp
        Tfm = $tfm
        RuntimeVersion = "$major.0.0"
        SdkVersion = $sdkVersion
    }
    return $script:OracleCtx
}

# Compile C# source file(s) to an executable dll + runtimeconfig. Returns @{ Ok; ExitCode; Output; Dll }.
function Invoke-OracleCompile {
    param(
        [Parameter(Mandatory)] [string[]]$Sources,
        [Parameter(Mandatory)] [string]$OutDll,
        [switch]$NullableEnable,
        [switch]$WarnAsError
    )

    $outDir = Split-Path $OutDll -Parent
    if ($outDir -and -not (Test-Path $outDir)) { New-Item -ItemType Directory -Force $outDir | Out-Null }

    if ($env:POLYGLOT_ORACLE -eq "msbuild") {
        return Invoke-OracleCompileMsbuild -Sources $Sources -OutDll $OutDll -NullableEnable:$NullableEnable -WarnAsError:$WarnAsError
    }

    $ctx = Get-OracleContext
    $args = @("$($ctx.CscDll)", "/shared", "/nologo", "/target:exe", "/langversion:default",
              "/out:$OutDll", "@$($ctx.Rsp)")
    $args += if ($NullableEnable) { "/nullable:enable" } else { "/nullable:disable" }
    if ($WarnAsError) { $args += "/warnaserror" }
    $args += $Sources

    $output = & $ctx.Dotnet @args 2>&1
    $code = $LASTEXITCODE
    if ($code -eq 0) {
        $rc = [System.IO.Path]::ChangeExtension($OutDll, ".runtimeconfig.json")
        @"
{ "runtimeOptions": { "tfm": "$($ctx.Tfm)", "framework": { "name": "Microsoft.NETCore.App", "version": "$($ctx.RuntimeVersion)" }, "rollForward": "latestMinor" } }
"@ | Set-Content -Path $rc -Encoding UTF8
    }
    return @{ Ok = ($code -eq 0 -and (Test-Path $OutDll)); ExitCode = $code; Output = ($output | Out-String); Dll = $OutDll }
}

# The classic per-program `dotnet build` path (POLYGLOT_ORACLE=msbuild — the CI canary).
function Invoke-OracleCompileMsbuild {
    param(
        [Parameter(Mandatory)] [string[]]$Sources,
        [Parameter(Mandatory)] [string]$OutDll,
        [switch]$NullableEnable,
        [switch]$WarnAsError
    )
    $ctx = Get-OracleContext
    $stem = [System.IO.Path]::GetFileNameWithoutExtension($OutDll)
    $work = Join-Path ([System.IO.Path]::GetTempPath()) ("polyglot-oracle-msbuild-" + [System.Guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Force $work | Out-Null
    try {
        foreach ($s in $Sources) { Copy-Item $s $work }
        $nullable = if ($NullableEnable) { "enable" } else { "disable" }
        $warn = if ($WarnAsError) { "<TreatWarningsAsErrors>true</TreatWarningsAsErrors>" } else { "" }
        @"
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>$($ctx.Tfm)</TargetFramework>
    <Nullable>$nullable</Nullable>
    <ImplicitUsings>disable</ImplicitUsings>
    <AssemblyName>$stem</AssemblyName>
    $warn
  </PropertyGroup>
</Project>
"@ | Set-Content (Join-Path $work "$stem.csproj")
        $output = & $ctx.Dotnet build (Join-Path $work "$stem.csproj") -c Release -v quiet --nologo 2>&1
        $code = $LASTEXITCODE
        $built = Join-Path $work "bin\Release\$($ctx.Tfm)\$stem.dll"
        if ($code -eq 0 -and (Test-Path $built)) {
            Copy-Item $built $OutDll -Force
            Copy-Item ([System.IO.Path]::ChangeExtension($built, ".runtimeconfig.json")) ([System.IO.Path]::ChangeExtension($OutDll, ".runtimeconfig.json")) -Force
            return @{ Ok = $true; ExitCode = 0; Output = ($output | Out-String); Dll = $OutDll }
        }
        return @{ Ok = $false; ExitCode = $code; Output = ($output | Out-String); Dll = $OutDll }
    } finally {
        Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
    }
}

# Best-effort compile-server shutdown — call once from the gate's finally so no VBCSCompiler lingers.
function Stop-OracleBuildServer {
    try { & (Get-OracleContext).Dotnet build-server shutdown --vbcscompiler 2>&1 | Out-Null } catch {}
}
