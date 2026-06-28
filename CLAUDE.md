# MintPlayer.Polyglot — agent guide

A **cross-SDK transpiler**: one small source language → idiomatic, readable **C#/.NET** and
**TypeScript/JS**. A long-haul personal craft project, not on any delivery deadline.

**Read first:** `docs/prd/POLYGLOT_PRD.md` (vision, scope contract, architecture) and
`docs/prd/PLAN.md` (P0–P8 roadmap). This file is just the always-on rules.

## Prime directive — hold the scope line
The PRD §3 **support / refuse contract** is the law. Every multi-target transpiler that died (JSIL,
SharpKit, Bridge.NET) died of scope creep. Before adding any feature, check it against §3:
- **Supported (§3.A):** operators, properties/indexers, extension methods, exceptions, `using`,
  iterators, pattern matching/ADTs, enums, closures, overloading, strings (both targets are UTF-16).
- **Refused (§3.B), with a clear diagnostic — never a miscompile:** threads/locks, runtime reflection,
  finalizers/GC hooks, `decimal`, `unsafe`/pointers, `dynamic`/runtime code-gen, bit-exact cross-target
  floats.
- **Faithful-by-default with a *published* relaxation list (§3.C):** int overflow masking, int64→BigInt,
  opt-in `Math.fround` strict floats, structural equality. Never relax silently — document it.
- **Determinism honesty (§3.D):** only `+ − × ÷ √` are reproducible across .NET and JS; transcendentals
  are not. Don't promise bit-exact float parity; offer a fixed-point std type instead.

## Key decisions (don't relitigate without reason)
- **C++20**, single self-contained native CLI, zero runtime deps. Consequence: **no Roslyn / no ts-morph**
  — the C# and TS backends **hand-write** their pretty-printers over the IR (the Haxe path). The C#/Roslyn
  alternative is recorded in PRD §4.3 as the fork to revisit only if hand-emitters become painful.
- **One high-level, typed, tree-shaped IR. NOT SSA, NOT a common denominator.** Specialize per target.
- Targets: **C# and TS first.** More targets are post-P8 stretch.

## Build / run (Windows)
Open `MintPlayer.Polyglot.sln` in VS 2022 (*Desktop development with C++* workload), or from a Developer
Command Prompt:
```
msbuild MintPlayer.Polyglot.sln /p:Configuration=Debug /p:Platform=x64
x64\Debug\MintPlayer.Polyglot.Cli.exe --version      # -> 0.0.1
x64\Debug\MintPlayer.Polyglot.Tests.exe              # -> all tests pass
```
NOTE: the hand-authored `.sln`/`.vcxproj` have **not been compiled yet** (P0 gate still open). VS may
offer to re-save the project files on first load — that's expected.

## Layout
```
src/MintPlayer.Polyglot.Core/   # compiler library (lexer→parser→typed IR→backends); public headers in include/
src/MintPlayer.Polyglot.Cli/    # the `polyglot` CLI
tests/MintPlayer.Polyglot.Tests/# unit + (later) differential-conformance tests
docs/prd/                       # PRD + plan   docs/lang/  # SPEC.md lands in P1
```

## Status & next step
P0 (skeleton) committed but unbuilt. **Next: P1 — language design v0.1** (grammar + `docs/lang/SPEC.md` +
sample `.pg` programs, incl. a FruitCake-physics sketch to pressure-test the surface). See PLAN.md.

## Sibling repo
The P8 dogfood target (FruitCake physics twins) lives in `C:\Repos\MintPlayer.AI` — see PRD §8 for paths.
That repo is a .NET + Angular app; this one is unrelated C++ tooling. Don't run its build/tests from here.
