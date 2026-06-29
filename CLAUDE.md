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
proves it bites). **P6 ✅ done — faithfulness pass:** §3.B **refusal diagnostics** ("Polyglot refuses X");
**all integer widths faithful** (i8…u32 narrowing, i64/u64 → **BigInt**); **structural equality** for
records; **explicit casts `(T)x`** + **implicit lossless widening** (no conversion methods); **function
overloading** (C# native name / TS param-mangled). *Deferred tail:* strict-f32 `Math.fround`, `lock`/
`unsafe` statement refusals, null normalization. **P7 ✅ done — std core + the three plugin mechanisms as
first-party code:** **static methods** (`Type.method()`); **`i32.parse`/`f64.parse`** (string→number);
the **`Math`** namespace (replacement); **`expect`/`actual`** target-gated capabilities (each backend
emits only its `actual`); **`extern("…")`** raw-code FFI (binding); and the **portable-core guard**
(`extern` refused outside an `actual`). **P11 added to the roadmap** (PLAN/PRD): a NuGet package that
auto-transpiles `.pg`→`.cs` before `dotnet build` (Grpc.Tools-style, native CLI per-RID, non-transitive);
depends only on a stable CLI, so it can ship independently of P9/P10.
**P8 ✅ done — the FruitCake physics dogfood (★ north star).** `docs/lang/samples/fruitcake_sketch.pg`
(a full sequential-impulse circle solver, f64 so `+−×÷√` are bit-exact cross-target per §3.D) transpiles
to C# **and** TS with byte-identical output (`bodies=1 scored=1`); it's conformance program #27, all green.
Delivered: **`List<T>` as a first-party `.pg` std type** (not an intrinsic) via the new **binding
mechanism** — a method/property body of `actual(target) extern("…template…")` arms where `$this`
(receiver) + `$0…` (args) substitute at each call site, and a `$this = …` arm emits a receiver
*assignment* (so `list.clear()` → C# `xs.Clear()` / TS `xs = []`; `list.removeAll(p)` → TS
`xs = xs.filter(e => !((p)(e)))`). **`extern class`** marks a native-backed type (not emitted; maps to a
target type). **Embedded std module**: `import std.collections.{ List }` links an embedded `collections.pg`
(no FS resolver yet). List element typing (`[...]`, `lst[i]`, `for x in`/`for (a,b) in`) is compiler-level.
Also lowered+emitted for the first time (all previously hit a silent `0`): `null`, `x!` (→ non-null cast),
`??`, string interpolation, index, tuple literal, top-level globals, tuple destructuring. *Deferred tail
(unchanged):* strict-f32 `Math.fround`, `lock`/`unsafe` refusals, null normalization.
**P12 ✅ phase-1 done — modules/imports** (PRD §4.5): TS-style `import { a, b as c } from "spec"` (+ `* as ns`,
bare; `from`/`as` contextual, quoted specifier so `"std.io"`=logical, `"./x"`=relative); a **`ModuleResolver`**
seam (`compile(src, target, ModuleResolver*=nullptr)`, Core stays IO-free) with transitive cross-`.pg`
loading — dedup, cycle detection, deps-first merge; CLI `FileModuleResolver` (`--root`), in-memory test
resolver; **collision detection** closes the silent value/union-case/extension holes. *Phase-2 deferred*
(needs a per-file import-scope table): selective-import visibility restriction + `as` rebinding.
std.io ships File bindings (readText/writeText/…) via the capability mechanism; std is still embedded-source.
**Roadmap: P9** (declarative backend DSL), **P10** (plugin distribution), **P11** (build-integration NuGet,
independent).

## Sibling repo
The P8 dogfood target (FruitCake physics twins) lives in `C:\Repos\MintPlayer.AI` — see PRD §8 for paths.
That repo is a .NET + Angular app; this one is unrelated C++ tooling. Don't run its build/tests from here.
