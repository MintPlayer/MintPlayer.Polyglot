#!/usr/bin/env pwsh
# P35 slice 3 shim: run-python.ps1 merged into run-conformance.ps1. Kept so doc/muscle-memory invocations of
# the old C#-vs-Python runner still work — forwards to the merged runner restricted to the csharp+python pair.
param([string]$Cli = (Join-Path $PSScriptRoot ".." ".." "x64" "Debug" "MintPlayer.Polyglot.Cli.exe"))
& (Join-Path $PSScriptRoot "run-conformance.ps1") -Cli $Cli -Targets csharp, python
exit $LASTEXITCODE
