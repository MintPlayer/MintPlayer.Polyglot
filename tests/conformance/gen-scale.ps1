#!/usr/bin/env pwsh
# Regenerates programs/scale.pg (wave-2 G26) — a deterministic ~1000-line module that stresses
# emitter/plugin-interpreter throughput: 200 small functions, a 32-parameter function, a
# 256-element list literal, and a main() that runs the lot and prints three checksums.
# The program is COMMITTED; rerun this only to change its shape.

$out = Join-Path $PSScriptRoot "programs\scale.pg"
$sb = [System.Text.StringBuilder]::new()
[void]$sb.AppendLine("// Wave-2 G26: GENERATED scale program (~1000 lines) — do not hand-edit; regenerate")
[void]$sb.AppendLine("// with tests/conformance/gen-scale.ps1. Pins emitter throughput + big-module shapes")
[void]$sb.AppendLine("// (200 fns, 32-param fn, 256-element list literal) with deterministic checksums.")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("import { List } from `"std.collections`"")
[void]$sb.AppendLine("")

foreach ($i in 1..200) {
    [void]$sb.AppendLine("fn fx$i(x: i32): i32 {")
    [void]$sb.AppendLine("  let t = x * $(($i % 7) + 2) + $i")
    [void]$sb.AppendLine("  let u = t + (x % $(($i % 5) + 3))")
    [void]$sb.AppendLine("  return u % 9973")
    [void]$sb.AppendLine("}")
}

$params = (1..32 | ForEach-Object { "p${_}: i32" }) -join ", "
$sum = (1..32 | ForEach-Object { "p$_" }) -join " + "
[void]$sb.AppendLine("")
[void]$sb.AppendLine("fn wide($params): i32 {")
[void]$sb.AppendLine("  return $sum")
[void]$sb.AppendLine("}")
[void]$sb.AppendLine("")

$elems = (1..256 | ForEach-Object { ($_ * 13) % 977 }) -join ", "
[void]$sb.AppendLine("fn main() {")
[void]$sb.AppendLine("  var acc = 0")
foreach ($i in 1..200) {
    [void]$sb.AppendLine("  acc = (acc + fx$i($i)) % 1000003")
}
[void]$sb.AppendLine("  print(acc)")
[void]$sb.AppendLine("  print(wide($((1..32) -join ', ')))")
[void]$sb.AppendLine("  var xs: List<i32> = [$elems]")
[void]$sb.AppendLine("  var lsum = 0")
[void]$sb.AppendLine("  for v in xs { lsum = lsum + v }")
[void]$sb.AppendLine("  print(lsum)")
[void]$sb.AppendLine("}")

Set-Content -Path $out -Value $sb.ToString() -NoNewline
Write-Host "wrote $out ($((Get-Content $out | Measure-Object -Line).Lines) lines)"
