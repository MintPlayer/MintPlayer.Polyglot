#!/usr/bin/env pwsh
# Stage a pinned polyglot CLI (from P22's GitHub Releases) into editors/vscode/bin/ so `vsce package --target`
# produces a platform-specific VSIX that bundles the language server (PRD §4.15 / PLAN §P23 slice 3).
#
# Layout produced: bin/polyglot(.exe) + bin/plugins/  — the plugins MUST sit next to the binary, because the
# CLI finds its plugins next to itself (P22 slice-1 exe_path.hpp), not in the cwd.
#
# We DOWNLOAD P22's already-built, provenance-attested release artifacts for a pinned version — we never
# rebuild the CLI here (extension cadence is decoupled from CLI cadence). This same script backs the CI
# matrix (publish-vscode.yml), one invocation per platform leg.
#
# Usage:
#   pwsh editors/vscode/scripts/stage-cli.ps1 -Target win32-x64 -Tag v0.5.0 [-Repo owner/name]
#   pwsh editors/vscode/scripts/stage-cli.ps1 -Target win32-x64 -Version 0.5.0     # -Tag = v$Version
#   pwsh editors/vscode/scripts/stage-cli.ps1 -Target universal -Tag v0.5.0        # empty bin/ (no-binary fallback vsix)
[CmdletBinding()]
param(
  [Parameter(Mandatory)][ValidateSet('win32-x64', 'linux-x64', 'linux-arm64', 'darwin-x64', 'darwin-arm64', 'universal')][string]$Target,
  # The CLI release to bundle. -Tag is the release tag (what Workflow C passes, from the release event); -Version
  # is the bare number for local convenience (mapped to v$Version). -Tag wins if both are given.
  [string]$Tag = '',
  [string]$Version = '',
  [string]$Repo = 'MintPlayer/MintPlayer.Polyglot'
)
$ErrorActionPreference = 'Stop'

# VS Code target -> release asset + the binary name inside it (echoing P22's RID set; VS Code's `darwin-*`
# maps to the CLI's `osx-*` archive). macOS binaries are ad-hoc signed at build; the extension additionally
# strips the com.apple.quarantine xattr on activation so Gatekeeper lets the bundled server run.
$map = @{
  'win32-x64'    = @{ asset = 'polyglot-win-x64.zip';        bin = 'polyglot.exe' }
  'linux-x64'    = @{ asset = 'polyglot-linux-x64.tar.gz';   bin = 'polyglot'     }
  'linux-arm64'  = @{ asset = 'polyglot-linux-arm64.tar.gz'; bin = 'polyglot'     }
  'darwin-x64'   = @{ asset = 'polyglot-osx-x64.tar.gz';     bin = 'polyglot'     }
  'darwin-arm64' = @{ asset = 'polyglot-osx-arm64.tar.gz';   bin = 'polyglot'     }
}

$extDir = Split-Path -Parent $PSScriptRoot   # editors/vscode
$binDir = Join-Path $extDir 'bin'

# Always start from a clean bin/ so each platform vsix carries exactly one RID's payload (no cross-leg bleed).
if (Test-Path $binDir) { Remove-Item -Recurse -Force $binDir }
New-Item -ItemType Directory -Force $binDir | Out-Null

# The universal fallback vsix ships no binary — highlighting + the resolveCli ladder's PATH/cliPath rungs.
if ($Target -eq 'universal') {
  Write-Host "staged universal (no-binary) fallback -> empty $binDir"
  exit 0
}

$m = $map[$Target]
if ($Tag) { $tag = $Tag } elseif ($Version) { $tag = "v$Version" } else { throw "stage-cli: pass -Tag <v-tag> (or -Version <number>)" }
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) "polyglot-stage-$([System.Guid]::NewGuid().ToString('n'))"
New-Item -ItemType Directory -Force $tmp | Out-Null
$archive = Join-Path $tmp $m.asset
try {
  # Prefer gh (release assets, auth + private-repo handled); fall back to the public download URL.
  $downloaded = $false
  if (Get-Command gh -ErrorAction SilentlyContinue) {
    gh release download $tag --repo $Repo --pattern $m.asset --dir $tmp --clobber
    if ($LASTEXITCODE -eq 0) { $downloaded = $true }
  }
  if (-not $downloaded) {
    $url = "https://github.com/$Repo/releases/download/$tag/$($m.asset)"
    Invoke-WebRequest -Uri $url -OutFile $archive
  }

  # Extract into the temp dir, then copy polyglot(.exe) + plugins/ into bin/. Both archives place them at
  # the archive root (release.yml: Compress-Archive stage\*  /  tar czf -C build polyglot plugins).
  # We extract the tarball with a FILENAME-only argument from inside $tmp — GNU tar (git's, first on PATH
  # on Windows) reads a `C:\...` path's colon as a remote host and fails; a cwd-relative name sidesteps it
  # (same gotcha the P19 install path hit). On Linux CI the paths have no colon, so this is equally correct.
  if ($m.asset.EndsWith('.zip')) {
    Expand-Archive -Path $archive -DestinationPath $tmp -Force
  }
  else {
    Push-Location $tmp
    try {
      tar -xzf $m.asset
      if ($LASTEXITCODE -ne 0) { throw "tar failed extracting $($m.asset)" }
    }
    finally { Pop-Location }
  }
  Copy-Item (Join-Path $tmp $m.bin) (Join-Path $binDir $m.bin)
  Copy-Item -Recurse (Join-Path $tmp 'plugins') (Join-Path $binDir 'plugins')
}
finally {
  Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

# Fail loudly if the payload is incomplete — a vsix with no server, or a server with zero targets, is worse
# than the universal fallback (it would win rung 2 and then not start).
$binPath = Join-Path $binDir $m.bin
if (-not (Test-Path $binPath)) { throw "staging produced no $($m.bin) in $binDir" }
$plugins = Join-Path $binDir 'plugins'
if (-not (Test-Path $plugins)) { throw "staging produced no plugins/ next to the binary" }
# tar preserves +x, but be explicit (belt-and-suspenders; the extension also chmods on activation).
if (-not $IsWindows) { & chmod 0755 $binPath }

$names = (Get-ChildItem $plugins -Directory).Name -join ','
if (-not $names) { throw "staged plugins/ is empty — the bundled CLI would have zero targets" }
Write-Host "staged $Target CLI $Version -> $binDir (binary: $($m.bin); plugins: $names)"
