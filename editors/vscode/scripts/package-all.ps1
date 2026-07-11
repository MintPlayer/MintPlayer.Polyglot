#!/usr/bin/env pwsh
# Local proof for PRD §4.15 / PLAN §P23 slice 3: build every P23 vsix — the three platform-specific vsixes
# (each bundling one RID's CLI) plus the universal no-binary fallback — into editors/vscode/dist/.
#
# This mirrors what CI (publish-vscode.yml, slice 4) does per matrix leg: stage bin/ for the target, then
# `vsce package --target`. Run it to verify the whole stage->package flow locally before pushing. The vsixes
# and bin/ are gitignored (payloads are downloaded release artifacts, never committed).
#
# Usage: pwsh editors/vscode/scripts/package-all.ps1 [-Version 0.3.1]
[CmdletBinding()]
param([string]$Version = '0.3.1')
$ErrorActionPreference = 'Stop'

$extDir = Split-Path -Parent $PSScriptRoot
$stage = Join-Path $PSScriptRoot 'stage-cli.ps1'
$distDir = Join-Path $extDir 'dist'
if (Test-Path $distDir) { Remove-Item -Recurse -Force $distDir }
New-Item -ItemType Directory -Force $distDir | Out-Null

# Platform vsixes carry --target; the universal one omits it (and stages an empty bin/).
$targets = 'win32-x64', 'linux-x64', 'linux-arm64', 'universal'
foreach ($t in $targets) {
  Write-Host "== $t ==" -ForegroundColor Cyan
  & pwsh $stage -Target $t -Version $Version
  if ($LASTEXITCODE -ne 0) { throw "staging failed for $t" }
  $out = Join-Path $distDir "polyglot-lang-$t.vsix"
  Push-Location $extDir
  try {
    $targetArg = if ($t -eq 'universal') { @() } else { @('--target', $t) }
    & npx --no-install @vscode/vsce package @targetArg --out $out
    if ($LASTEXITCODE -ne 0) { throw "vsce package failed for $t" }
  }
  finally { Pop-Location }
}

# Leave bin/ empty so a stray committed payload can't sneak into a later plain `vsce package`.
$binDir = Join-Path $extDir 'bin'
if (Test-Path $binDir) { Remove-Item -Recurse -Force $binDir }

Write-Host "`nPackaged vsixes:" -ForegroundColor Green
Get-ChildItem $distDir -Filter *.vsix | ForEach-Object { Write-Host "  $($_.Name) ($([math]::Round($_.Length/1KB)) KB)" }
