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
overloading** (C# native name / TS param-mangled). *Deferred tail:* strict-f32 `Math.fround`, null
normalization (`lock`/`unsafe` statement refusals ✅ done — parseStmt emits a targeted §3.B message).
**P7 ✅ done — std core + the three plugin mechanisms as
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
(unchanged):* strict-f32 `Math.fround`, null normalization (`lock`/`unsafe` refusals now ✅ done).
**P12 ✅ phase-1 done — modules/imports** (PRD §4.5): TS-style `import { a, b as c } from "spec"` (+ `* as ns`,
bare; `from`/`as` contextual, quoted specifier so `"std.io"`=logical, `"./x"`=relative); a **`ModuleResolver`**
seam (`compile(src, target, ModuleResolver*=nullptr)`, Core stays IO-free) with transitive cross-`.pg`
loading — dedup, cycle detection, deps-first merge; CLI `FileModuleResolver` (`--root`), in-memory test
resolver; **collision detection** closes the silent value/union-case/extension holes. *Phase-2 deferred*
(needs a per-file import-scope table): selective-import visibility restriction + `as` rebinding.
std.io ships File bindings (readText/writeText/…) via the capability mechanism; std is still embedded-source.
**P9 ✅ done — declarative backend engine + DSL extracted (validated across three backends)** (design + slice
log: `docs/design/backend-spec.md`). A `{Spec data + Hooks}` split across byte-for-byte no-op slices:
**`EmitterBase`** (`emitter_base.hpp`/`.cpp`) owns the statement walk + buffer/indentation + block abstraction
and reads all per-target data through one `spec()` accessor; **`BackendSpec`** is the extracted declarative
DSL — **all** per-target data: scalar/suffix/operator/bracket tables + block style + statement terminator +
throw keyword + bool/null literal spellings (string escaping is the shared `renderString` primitive). Every
backend, incl. Python, is now a `{Spec + Hooks}` instance. The residual **hook surface** is genuine behavior:
`emitExpr`/`emitStmtTarget`/`localDecl`/`yieldStmt`/`rethrowStmt` + the per-target declaration emitters.
Extraction proved (and a third, non-sibling backend confirmed) the **expression walk + declaration shapes are
irreducibly per-target** — they can't be flattened to data without an embedded DSL the zero-dep core forbids.
**P9-V ✅ done — third backend (Python) validation spike** (`emit_python.cpp`, gate `tests/conformance/run-python.ps1`,
opt-in `--target python`). A non-sibling colon+indent target brought up to validate the engine generalizes,
now covering the **full §3.A surface — all 36 conformance programs (incl. the FruitCake north star) agree
byte-for-byte with the C# oracle**. It forced a real generalization (3-way `BlockStyle` + `stmtEnd` +
`throwKeyword` hook + block-style-agnostic `Use`, all verified C#/TS no-ops), after which the shared statement
layer served Python unchanged; declarations stay per-target (often cleaner — structural `==` via `__eq__`, no
`new`, exact bignum `int`, native dunders/generators, `except T as e`). The spike fixed **three latent bugs at
the root**, notably that **`break`/`continue` were silently dropped in lowering for all targets** (a §3.B
miscompile the C#/TS diff gate couldn't catch — both dropped them identically; now `ir::Break`/`Continue`
emit in the shared engine). `PythonBackend::supports` returns `true` for everything. The declarative DSL (P9
endpoint) can now be extracted from **three** backends instead of guessed. Slice log in PLAN §P9-V.
Follow-up ✅ (2026-07-01): a call to a portable fn (one with `actual`s) on a target lacking one now **refuses**
with a call-site diagnostic (`checkCapabilities`, call-site-keyed so unused portable fns are unaffected) —
closing that §3.B silent-broken-output gap.
**P13 ✅ done — std as real modules + the `lib` prelude** (PRD §4.6): `print` is now `std.io`'s generic
`expect/actual print<T>` (TS body wraps `console.log(String(x))` universally so bigint/number print like C#
`WriteLine`); `Math` is an `extern class Math` in `std.math` (bound static members + `PI`/`E`; `min/max/abs/round`
are call-site-inlined generic bindings — a generic C# `Math.Min<T>` wouldn't compile — constrained
`<T: INumber>`); `i32.parse` stays global, `Error`/`Iterable`/`INumber` stay core. **`INumber`** is a
compile-time-only numeric marker constraint (à la .NET's `System.Numerics.INumber<T>`, core `extern class`,
satisfied by `i8..u64`/`f32`/`f64`): sema rejects a non-numeric arg to any `<T: INumber>` generic ahead of
time (`checkNumericBounds`), and the bound is erased from emission (`csWhere`/`tsGenerics`). The **`lib` prelude** (`LibConfig` 4th arg to `compile()`, CLI `--lib io,math`)
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
**P15 ✅ done — single-threaded async/await** (design: **PRD §4.7**, from a 4-agent investigation). A colored
function like iterators: `isAsync` on `FunctionDecl`/`Member`/`ir::Function`/`ir::Method` (method `async`
promoted from `Member.modifiers` to a typed flag) + an `Await` expr node (AST + `ir::Await`) parsed at unary
precedence; author writes the unwrapped `T` and each backend synthesizes its wrapper (Option B — keeps `.pg`
portable): C# `async Task<T>` w/ `main().GetAwaiter().GetResult()`, TS `async … Promise<T>` w/ floating
`main();`, Python `async def` w/ `asyncio.run(main())`+`import asyncio`. Sema (`inAsync_`) validates `await`
only in an `async fn` + refuses `async`+`yield` (no async iterators v1); `Feature::Async` gates it (all 3
backends support it; bites only for a future PHP-like target). Conformance #38 `async_await.pg` agrees C#/TS
+ Python. **`await` typing is a real `Awaitable<T>` unwrap** (an async call types as the compile-time-only
`Awaitable<T>` via an `isAsync` bit on sema's `FnSig`/`MemberInfo`; `await` unwraps to `T`) — so sema catches
forgot-to-await (`return f()`/`print(f())` refuse) and awaited-non-async (`await plain()` refuses), mirroring
C#/TS. `Awaitable` is never author-written and never emitted. Shared engine unchanged, as designed.
**P16 ✅ (except the VS client) — editor tooling & the language server** (design: **PRD §4.8**, from a 4-agent
investigation; slice log: **PLAN §P16**). A shared TextMate grammar (`editors/vscode/syntaxes/`) + a zero-dep
**`polyglot lsp`** stdio JSON-RPC server built on a new front-end seam **`analyze()`** (returns the checked AST +
a position-indexed **`SemanticModel`** + a `SourceMap`, without lowering/emitting) and a sema-hook that records
defs/refs. Capabilities: diagnostics (live on-type), go-to-def (same-file + **cross-module** via `SourcePos.fileId`
+ `SourceMap` + `file://`, and **std** via `polyglot:<name>` virtual docs the server serves), hover, document
symbols, semantic tokens, formatting, references, rename (file-local), completion. The **VS Code extension** is a
thin `vscode-languageclient` client (plain JS, no bundler; F5 via repo-root `.vscode` → build CLI + `npm install`).
A minimal **`pgconfig.json`** (`{root,lib}`, CLI/LSP layer, core stays IO-free) drives module resolution. The CLI
now **statically links the CRT** (self-contained — see PRD §4.3). **P16d (Visual Studio client) 🚧 slices 1–4 built,
headless build green** — a VSIX at `editors/vs/` (`ILanguageClient` launching `polyglot lsp` + `polyglot` content
type + `.pg` association + the shared TextMate grammar bundled at build); builds with the VS 18 MSBuild
(`VSToolsPath` from `MSBuildExtensionsPath`; framework refs `System`+`System.ComponentModel.Composition`; manifest
`ProductArchitecture=amd64`; SDK 17.0 NuGet, `InstallationTarget [17.0,)`). **Interactive `devenv /rootsuffix Exp`
verification is the user's step** (not headless); Options page + custom-message std-docs/emit-preview deferred.
The P16 deferred tail is now **empty**. ✅ done:
live cross-file edits (a `BufferResolver` serves open unsaved imports; `didChange` re-analyzes all open docs);
semantic tokens/hover/def inside `polyglot:` std virtual docs (scheme added to the selector; std analyzed from
synced text, diagnostics suppressed); **member completion** (`obj.` — `SymbolDef.owner` on members; LSP analyzes a
repaired buffer to resolve the receiver type; `.` trigger char; `Math.` statics work; v1 skips inherited members +
`this.`); **in-scope local filtering** (parser records fn/method `bodyEnd`; sema stamps Local/Parameter defs with
`[scopeStart,scopeEnd]`; LSP offers a local only in-range); **non-ASCII UTF-16 position conversion** (guarded by a
`utf16_` flag so the utf-8 VS Code path is identity; `inCol`/`encRange`/semantic-token col+len convert per line).
**P17 ✅ done — live generated-output preview** (PRD §4.9, from a 2-agent investigation; slice plan PLAN §P17):
see a `.pg`'s emitted C#/TS/Python **live as you type**, rendered read-only into a `polyglot-gen:` virtual editor
opened beside the source (colored for free by the built-in target grammars). One new in-memory LSP request
`polyglot/emit` → `compile()` (no disk I/O, **zero Core change** — it's a CLI handler + client code over P16's
virtual-doc/custom-request plumbing); client-debounced request/response, one target per request, last-good-with-
stale-banner error UX (never a miscompile shown as valid). Delivered: `polyglot/emit` (spawn-tested), the
`polyglot-gen:` provider + follow-active-editor + 200 ms debounce. **"Show Generated Output" opens all three targets
at once** (one tab each, target-only gen URIs that follow the active `.pg`); an Explorer "Polyglot Outputs" tree
(`polyglot.openGenerated`, gated by a `polyglot.hasOutputs` context key) opens a single target on demand. Multi-root
is server-side (per-file `pgconfig.json` walk-up in `contextFor`). (The single-target status-bar switcher was built
then dropped on feedback.)
**Roadmap: P10** (plugin *distribution* — package/registry — still pending; needs P9), **P11**
(build-integration NuGet, independent), **P16d** (Visual Studio LSP client).

## Sibling repo
The P8 dogfood target (FruitCake physics twins) lives in `C:\Repos\MintPlayer.AI` — see PRD §8 for paths.
That repo is a .NET + Angular app; this one is unrelated C++ tooling. Don't run its build/tests from here.
