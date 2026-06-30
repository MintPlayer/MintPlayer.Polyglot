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

## Principled fix over workaround
This is a long-haul craft project — prefer the **root-cause fix** over an expedient patch, even when the
patch is smaller. When a problem traces to a missing language/compiler capability, build (or plan) that
capability rather than papering over the symptom. Example (2026-06-29): a generic call's return type wasn't
substituted, so `Math.max(i64,i64)` would print `20n` vs `20`; the workaround was to wrap every `print` arg
in `String()`, but the principled fix was **real TypeArg inference** (bind type params from args, substitute
the return) — which fixes the whole class of generic-call bugs, not just `print`. Take that path. If a
workaround is genuinely warranted (time-boxed, the real fix is out of scope), say so explicitly and leave a
note pointing at the principled follow-up — never let a silent shortcut masquerade as the design.

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
**P9 🚧 in progress — declarative backend DSL** (design + extraction map: `docs/design/backend-spec.md`).
Catalog found backends are ~70% tabular / ~30% imperative → a backend = **Spec (data)** + **Hooks (C++ for
the imperative 30%)** over one shared emit engine; extraction is incremental, each slice a byte-for-byte
no-op. Slice 1 ✅: scalar type-leaf table → `BackendSpec` (`backend_spec.hpp`), both emitters consult it.
**P13 ✅ done — std as real modules + the `lib` prelude** (PRD §4.6): `print` is now `std.io`'s generic
`expect/actual print<T>` (TS body wraps `console.log(String(x))` universally so bigint/number print like C#
`WriteLine`); `Math` is an `extern class Math` in `std.math` (bound static members + `PI`/`E`; `min/max/abs`
are call-site-inlined bindings since a generic C# `Math.Min<T>` wouldn't compile); `i32.parse` stays global,
`Error`/`Iterable` stay core. The **`lib` prelude** (`LibConfig` 4th arg to `compile()`, CLI `--lib io,math`)
auto-imports std modules ambiently and **silently loses to** any user/explicit decl of the same name; a bare
entry (`"io"`) means `std.io`, a qualified one (`"acme.physics"`) is a full specifier so third-party plugins
auto-import by their own namespace. Also delivered: **TypeArg inference** (bind generic params from arg types,
substitute the return) as the principled fix for generic-call return types; `List.removeAt`. The
`docs/lang/samples/*.pg` now **compile** (gate: `tests/samples/run-compile.ps1`), not just `fmt` — all 10
green. Two follow-up gaps the gate surfaced are now also fixed: **base-class member resolution**
(`findMember` walks `TypeInfo.bases`; lower's binding lookup walks `bases_`) with **`Error.message`** as a
per-target bound property (C# `$this.Message` / JS `$this.message`); and **extensions on a generic receiver**
(`liftExtensionGenerics` lifts free receiver type-vars like `List<T>`'s `T` into the extension's generics).
**P10 type-mapping/construction gap ✅ closed (2026-06-30) + full std/core dogfood.** An `extern class`
declares its per-target **type spelling** (`type { actual(target) extern("…$0…") }`; `$0,$1`=rendered type
args) and **construction** (binding arms on `init`; `$T`=mapped type, `$0,…`=ctor args; `Type(args)`→`ir::Bound`).
Carried as an `ir::ExternType` registry on the IR module that `csType`/`tsType` consult per-emit. **Dogfooded:**
`List` (in std.collections) + an always-linked **core prelude** (`compiler.cpp` `STD_CORE` via `linkCoreModule`)
declaring `extern class Error` (→`System.Exception`/`Error`, ctor, `message` property) and `Iterable`
(→`IEnumerable<$0>`/`Iterable<$0>`). The emitters now have **zero** hardcoded type mappings. Backlog: idiomatic
per-target member casing (PLAN P13).
**Roadmap: P10** (plugin *distribution* — package/registry — still pending; needs P9), **P11**
(build-integration NuGet, independent).

## Sibling repo
The P8 dogfood target (FruitCake physics twins) lives in `C:\Repos\MintPlayer.AI` — see PRD §8 for paths.
That repo is a .NET + Angular app; this one is unrelated C++ tooling. Don't run its build/tests from here.
