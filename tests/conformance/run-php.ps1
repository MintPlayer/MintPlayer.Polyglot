#!/usr/bin/env pwsh
# P35 slice 3 shim: run-php.ps1 merged into run-conformance.ps1. Kept so doc/muscle-memory invocations of the
# old C#-vs-PHP runner still work — forwards to the merged runner restricted to the csharp+php pair (the G28
# pinned-refuser handling lives in the merged runner). -Php is forwarded when given.
param(
    [string]$Cli = (Join-Path $PSScriptRoot ".." ".." "x64" "Debug" "MintPlayer.Polyglot.Cli.exe"),
    [string]$Php = ""
)
& (Join-Path $PSScriptRoot "run-conformance.ps1") -Cli $Cli -Targets csharp, php -Php $Php
exit $LASTEXITCODE
