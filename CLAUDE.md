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
Open `MintPlayer.Polyglot.sln` in a C++-capable VS (*Desktop development with C++* workload), or build
from MSBuild:
```
msbuild MintPlayer.Polyglot.sln /p:Configuration=Debug /p:Platform=x64
x64\Debug\MintPlayer.Polyglot.Cli.exe --version      # -> 0.0.1
x64\Debug\MintPlayer.Polyglot.Tests.exe              # -> all tests pass
```
**One-shot gate** (build → unit tests → differential C#/TS conformance): `pwsh scripts/build-and-test.ps1`
— or invoke the **`/build-and-test`** skill (`.claude/skills/build-and-test/`). Needs `dotnet` + `node`
for the conformance stage.
TOOLCHAIN: the projects target PlatformToolset **v145** / VCProjectVersion **18.0**, so they **require
VS 2026 (the "18" generation)** — by design; this is a VS-2026-only project. The build is **VS 18
"Insiders"** (v145 → MSVC 14.51):
- IDE:     `C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\IDE\devenv.exe`
- MSBuild: `C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\MSBuild.exe`

VS 2019 BuildTools (v142) and VS 2022 (v143) are both **insufficient** for v145 — don't build with them.

## Layout
```
src/MintPlayer.Polyglot.Core/   # compiler library (lexer→parser→typed IR→backends); public headers in include/
src/MintPlayer.Polyglot.Cli/    # the `polyglot` CLI
tests/MintPlayer.Polyglot.Tests/# unit + (later) differential-conformance tests
docs/prd/                       # PRD + plan
docs/lang/                      # SPEC.md + grammar.ebnf + samples/*.pg  (P1 design)
```

## Status & next step
P0 ✅ built. P1 ✅ language design v0.1 (`docs/lang/`: grammar.ebnf + SPEC.md + samples). **P2 ✅ done —
the walking-skeleton MVP:** the full pipeline (lexer→parser→typer→typed tree→**C# + TS** emitters) for a
minimal subset (`fn`, i32/f64 arithmetic, `let`/`var`, `if`/`while`, calls, `print`). `polyglot build
foo.pg` emits `foo.cs` + `foo.ts`; both compile/run with **identical stdout** — the differential
conformance gate (`tests/conformance/run-diff.ps1`) is green. 20 in-process tests pass.

To verify: build (VS 18 Insiders MSBuild), then run `x64\Debug\MintPlayer.Polyglot.Tests.exe` and
`pwsh tests/conformance/run-diff.ps1` (needs `dotnet` + `node` on PATH).

**P3 ✅** full P1 grammar parses (incl. interpolation); `polyglot fmt` round-trips all 10 samples
(`tests/fidelity/run-roundtrip.ps1`). **P4 ✅** semantics + a **separate typed IR**: resolution + nominal
typing + match exhaustiveness in `sema.cpp`; AST→IR `lower.cpp` into `ir.hpp`; backends emit from the IR
(pipeline = lexer→parser→sema→lower→IR→backend). Full sema runs on self-contained programs (std-using
samples checked end-to-end at P7). **P5 ✅ done — full §3.A surface lowers + emits to both targets:** the
**backend-interface seam** (`Backend` abstraction + registry), records, enums, unions + pattern matching,
operators/properties, **classes** (inheritance + `super`), **`for…in`** (ranges + iterables), **iterators
(`yield`)**, **exceptions**, **`use`/disposal**, **closures/lambdas** (incl. bare `x => …`), **extension
methods** (C# `this`-methods / TS free functions), and **generics** (`<T>` + bounds + construction type
args). `Iterable`/`Error` are core builtin types. **§3.E per-target capability gating is implemented +
active** (`backend.hpp` `Feature` enum + `Backend::supports`; `capability.cpp` + `compile()` refuse any
used feature a target can't emit — C#/TS declare the full set so nothing gates yet; a StubBackend test
proves it bites). 14 differential programs, all green. **Next: P6 — faithfulness pass** (§3.C relaxations
+ §3.B refusal diagnostics). See PLAN.md.

## Sibling repo
The P8 dogfood target (FruitCake physics twins) lives in `C:\Repos\MintPlayer.AI` — see PRD §8 for paths.
That repo is a .NET + Angular app; this one is unrelated C++ tooling. Don't run its build/tests from here.
