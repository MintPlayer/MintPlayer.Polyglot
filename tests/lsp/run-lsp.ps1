#!/usr/bin/env pwsh
# LSP protocol gate (E2E wave 2, G29): a scripted JSON-RPC/stdio session against `polyglot lsp`,
# freezing the editor contract the VS Code / Visual Studio clients depend on.
#
# Transport is Content-Length-framed JSON-RPC 2.0 over the process's raw stdin/stdout byte streams
# (the CLI puts both into binary mode on Windows, so there is no CRLF translation to fight). We drive
# a full lifecycle and assert the observable protocol:
#
#   initialize            -> a response carrying `capabilities` (+ the negotiated positionEncoding)
#   initialized           -> (notification)
#   didOpen (with error)  -> a publishDiagnostics notification with >= 1 diagnostic
#   didChange (fixed)     -> a follow-up publishDiagnostics with 0 diagnostics (Full-sync re-analysis)
#   hover  @ a symbol     -> a non-null markdown result naming the symbol
#   definition @ a use    -> a Location pointing back at the declaration
#   shutdown / exit       -> the process exits cleanly, promptly
#
# Every read is bounded (no test may hang): a per-read deadline plus a total wall-clock budget; a
# blown budget is a [FAIL], never an indefinite wait.
#
# Usage:  pwsh tests/lsp/run-lsp.ps1   (build the solution first; see CLAUDE.md)

param(
    [string]$Cli = "$PSScriptRoot\..\..\x64\Debug\MintPlayer.Polyglot.Cli.exe"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Cli)) {
    Write-Host "polyglot CLI not found at $Cli - build the solution first (see CLAUDE.md)."
    exit 2
}

$bad = 0
function Assert([bool]$cond, [string]$name) {
    if ($cond) { Write-Host "[PASS] $name" } else { Write-Host "[FAIL] $name"; $script:bad++ }
}

$DeadlineSec = 15                       # total wall-clock budget for the whole session
$sessionSw = [System.Diagnostics.Stopwatch]::StartNew()
function RemainingMs { return [int][math]::Max(0, ($DeadlineSec * 1000) - $sessionSw.ElapsedMilliseconds) }

# --- transport ---------------------------------------------------------------------------------------
# Read exactly $count bytes from $stream, or $null on timeout / EOF (never blocks past $timeoutMs).
function Read-Exactly($stream, [int]$count, [int]$timeoutMs) {
    if ($count -eq 0) { return @() }
    $buf = New-Object byte[] $count
    $got = 0
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($got -lt $count) {
        $remaining = $timeoutMs - $sw.ElapsedMilliseconds
        if ($remaining -le 0) { return $null }
        $task = $stream.ReadAsync($buf, $got, $count - $got)
        if (-not $task.Wait([int]$remaining)) { return $null }   # per-read timeout
        $n = $task.Result
        if ($n -le 0) { return $null }                           # EOF
        $got += $n
    }
    return $buf
}

# One ASCII header line (without the trailing CRLF), or $null on timeout.
function Read-HeaderLine($stream, [int]$timeoutMs) {
    $bytes = New-Object System.Collections.Generic.List[byte]
    while ($true) {
        $b = Read-Exactly $stream 1 $timeoutMs
        if ($null -eq $b) { return $null }
        if ($b[0] -eq 10) { break }              # \n ends the line
        if ($b[0] -ne 13) { $bytes.Add($b[0]) }  # drop \r
    }
    return [System.Text.Encoding]::ASCII.GetString($bytes.ToArray())
}

# Read one framed message and return it as a parsed object, or $null on timeout / EOF.
function Read-Message($stream, [int]$timeoutMs) {
    $len = -1
    while ($true) {
        $line = Read-HeaderLine $stream $timeoutMs
        if ($null -eq $line) { return $null }
        if ($line -eq "") { break }              # blank line ends the headers
        if ($line -match '^Content-Length:\s*(\d+)') { $len = [int]$Matches[1] }
    }
    if ($len -lt 0) { return $null }
    $body = Read-Exactly $stream $len $timeoutMs
    if ($null -eq $body) { return $null }
    $json = [System.Text.Encoding]::UTF8.GetString($body)
    return ($json | ConvertFrom-Json)
}

# Read messages until $predicate matches one (returned), or the session budget runs out ($null).
function Read-Until($stream, [scriptblock]$predicate) {
    while ($true) {
        $rem = RemainingMs
        if ($rem -le 0) { return $null }
        $msg = Read-Message $stream $rem
        if ($null -eq $msg) { return $null }
        if (& $predicate $msg) { return $msg }
    }
}

# Frame + write one message (a hashtable) to the server.
function Send-Message($stream, $obj) {
    $json = $obj | ConvertTo-Json -Depth 20 -Compress
    $body = [System.Text.Encoding]::UTF8.GetBytes($json)
    $header = [System.Text.Encoding]::ASCII.GetBytes("Content-Length: " + $body.Length + "`r`n`r`n")
    $stream.Write($header, 0, $header.Length)
    $stream.Write($body, 0, $body.Length)
    $stream.Flush()
}

# --- the source under test (0-based lines as the LSP sees them) --------------------------------------
#  0: import { print } from "std.io"
#  1: fn add(a: i32, b: i32): i32 => a + b
#  2: fn main() {
#  3:   let x: i32 = "hello"        <- the seeded type error (fixed to `3` by didChange)
#  4:   print(add(x, 2))
#  5: }
$nl = "`n"
$errSrc = 'import { print } from "std.io"' + $nl +
          'fn add(a: i32, b: i32): i32 => a + b' + $nl +
          'fn main() {' + $nl +
          '  let x: i32 = "hello"' + $nl +
          '  print(add(x, 2))' + $nl +
          '}' + $nl
$okSrc  = $errSrc -replace '"hello"', '3'
$uri = "file:///lsp-gate/main.pg"

# --- launch ------------------------------------------------------------------------------------------
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Cli
$psi.Arguments = "lsp"
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.CreateNoWindow = $true
$proc = [System.Diagnostics.Process]::Start($psi)
$stdin = $proc.StandardInput.BaseStream
$stdout = $proc.StandardOutput.BaseStream

try {
    # --- initialize -----------------------------------------------------------------------------------
    Send-Message $stdin @{
        jsonrpc = "2.0"; id = 1; method = "initialize"
        params = @{
            processId = $PID; rootUri = $null
            capabilities = @{ general = @{ positionEncodings = @("utf-8", "utf-16") } }
        }
    }
    $init = Read-Until $stdout { param($m) $m.id -eq 1 }
    Assert ($null -ne $init -and $null -ne $init.result.capabilities) "initialize returns server capabilities"
    if ($null -ne $init) {
        $enc = $init.result.capabilities.positionEncoding
        Write-Host "       negotiated positionEncoding: $enc"
        Assert ($enc -in @("utf-8", "utf-16")) "positionEncoding is negotiated to a valid value"
        Assert ($init.result.capabilities.hoverProvider -eq $true) "capabilities advertise hoverProvider"
        Assert ($init.result.capabilities.definitionProvider -eq $true) "capabilities advertise definitionProvider"
    }

    Send-Message $stdin @{ jsonrpc = "2.0"; method = "initialized"; params = @{} }

    # --- didOpen with the seeded error -> publishDiagnostics with >= 1 diagnostic ---------------------
    Send-Message $stdin @{
        jsonrpc = "2.0"; method = "textDocument/didOpen"
        params = @{ textDocument = @{ uri = $uri; languageId = "polyglot"; version = 1; text = $errSrc } }
    }
    $diag1 = Read-Until $stdout { param($m) $m.method -eq "textDocument/publishDiagnostics" -and $m.params.uri -eq $uri }
    Assert ($null -ne $diag1) "didOpen produces a publishDiagnostics notification"
    if ($null -ne $diag1) {
        Assert (@($diag1.params.diagnostics).Count -ge 1) "the seeded type error surfaces >= 1 diagnostic"
    }

    # --- didChange fixing the error -> a follow-up publishDiagnostics with 0 diagnostics --------------
    Send-Message $stdin @{
        jsonrpc = "2.0"; method = "textDocument/didChange"
        params = @{
            textDocument = @{ uri = $uri; version = 2 }
            contentChanges = @(@{ text = $okSrc })   # Full sync (server advertises textDocumentSync=1)
        }
    }
    $diag2 = Read-Until $stdout { param($m) $m.method -eq "textDocument/publishDiagnostics" -and $m.params.uri -eq $uri }
    Assert ($null -ne $diag2) "didChange produces a follow-up publishDiagnostics notification"
    if ($null -ne $diag2) {
        Assert (@($diag2.params.diagnostics).Count -eq 0) "fixing the error clears the diagnostics (0 remain)"
    }

    # --- hover at the `add` call (line 4, char 9) -> non-null markdown naming the symbol --------------
    Send-Message $stdin @{
        jsonrpc = "2.0"; id = 2; method = "textDocument/hover"
        params = @{ textDocument = @{ uri = $uri }; position = @{ line = 4; character = 9 } }
    }
    $hov = Read-Until $stdout { param($m) $m.id -eq 2 }
    Assert ($null -ne $hov -and $null -ne $hov.result) "hover at a known symbol returns a non-null result"
    if ($null -ne $hov -and $null -ne $hov.result) {
        Assert ([string]$hov.result.contents.value -like "*add*") "hover content names the hovered symbol ('add')"
    }

    # --- definition at the same `add` use -> a Location back at the declaration (line 1) --------------
    Send-Message $stdin @{
        jsonrpc = "2.0"; id = 3; method = "textDocument/definition"
        params = @{ textDocument = @{ uri = $uri }; position = @{ line = 4; character = 9 } }
    }
    $def = Read-Until $stdout { param($m) $m.id -eq 3 }
    Assert ($null -ne $def -and $null -ne $def.result) "definition at a use returns a non-null Location"
    if ($null -ne $def -and $null -ne $def.result) {
        Assert ($null -ne $def.result.uri -and $null -ne $def.result.range) "the Location carries a uri and a range"
        Assert ($def.result.range.start.line -eq 1) "definition points at the declaration line (line 1: 'fn add')"
    }

    # --- rename the file-local `add` -> a single-file WorkspaceEdit touching decl + use ---------------
    Send-Message $stdin @{
        jsonrpc = "2.0"; id = 5; method = "textDocument/rename"
        params = @{ textDocument = @{ uri = $uri }; position = @{ line = 4; character = 9 }; newName = "plus" }
    }
    $rn = Read-Until $stdout { param($m) $m.id -eq 5 }
    Assert ($null -ne $rn -and $null -ne $rn.result -and $null -ne $rn.result.changes) `
        "rename returns a WorkspaceEdit with changes"
    if ($null -ne $rn -and $null -ne $rn.result.changes) {
        $edits = @($rn.result.changes.PSObject.Properties | ForEach-Object { $_.Value })
        $editCount = $edits.Count
        Assert ($editCount -ge 2) "rename edits both the declaration and the use ($editCount edits)"
        Assert (($edits | ForEach-Object { $_.newText }) -contains "plus") "rename carries the new name"
    }

    # --- shutdown / exit: clean, prompt process termination -------------------------------------------
    Send-Message $stdin @{ jsonrpc = "2.0"; id = 4; method = "shutdown"; params = $null }
    $sd = Read-Until $stdout { param($m) $m.id -eq 4 }
    Assert ($null -ne $sd) "shutdown returns a response"
    Send-Message $stdin @{ jsonrpc = "2.0"; method = "exit"; params = $null }
    Assert ($proc.WaitForExit(10000)) "the server process exits within 10s of 'exit'"
    if ($proc.HasExited) {
        Assert ($proc.ExitCode -eq 0) "the server exits with code 0"
    }
} finally {
    if (-not $proc.HasExited) { $proc.Kill(); $proc.WaitForExit(2000) | Out-Null }
    $errText = $proc.StandardError.ReadToEnd()
    if (-not [string]::IsNullOrWhiteSpace($errText)) { Write-Host "  (stderr) $errText" }
}

Write-Host ""
if ($bad -eq 0) {
    Write-Host "LSP gate green: the JSON-RPC lifecycle and core requests hold."
    exit 0
}
Write-Host "$bad LSP-gate assertion(s) failed."
exit 1
