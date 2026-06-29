# MintPlayer.Polyglot — Milestone Plan

Companion to [POLYGLOT_PRD.md](POLYGLOT_PRD.md). Each milestone names a concrete deliverable and a **gate**
(the observable thing that says it's done). This is a long-haul craft project — milestones are ordered but
unhurried, and the §3 support/refuse contract in the PRD is the law every milestone is checked against.

---

## P0 — Solution skeleton ✅ done (built green 2026-06-28)
The Visual Studio solution and sources are committed: `MintPlayer.Polyglot.Core` (static lib),
`MintPlayer.Polyglot.Cli` (the `polyglot` exe answering `--version`/`--help`, `build` stubbed), and
`MintPlayer.Polyglot.Tests` (a tiny zero-dependency assert harness). C++20, x64 Debug/Release.
*Gate (closed):* the hand-authored `.sln`/`.vcxproj` build with **0 warnings / 0 errors** via the
VS 18 "Insiders" MSBuild. The projects target toolset **v145** (→ MSVC 14.51), so they require VS 2026 —
a deliberate VS-2026-only pin (see CLAUDE.md for paths). `MintPlayer.Polyglot.Cli.exe --version` prints
`0.0.1`; `...Tests.exe` reports all-pass.

## P1 — Language design v0.1 ✅ locked (2026-06-28)
Design the source language on paper before any compiler code. Write a grammar (EBNF) + a short language
spec + 5–10 sample `.pg` programs spanning the §3.A supported surface (a function, a struct/record, an
enum + pattern match, a generic, an iterator, exception handling, a `using`). Deliberately exclude the
§3.B refused features from the grammar entirely.
*Delivered:* `docs/lang/grammar.ebnf` (admits only §3.A; §3.B unspeakable), `docs/lang/SPEC.md` (spec with
per-feature C#/TS lowering tables + the §3.C relaxation list + §3.B refusals), and `docs/lang/samples/` —
9 focused samples (`01_functions` … `09_strings`) covering every §3.A feature + `fruitcake_sketch.pg`,
the surface test modeled 1:1 on `MintPlayer.AI`'s `FruitCakeWorld.cs`. Key design call: only `class`
(mutable, reference identity) and `record` (immutable, structural equality) — **mutable value types
(`struct`) are refused for v0.1** because they are the one construct whose value/reference identity
diverges between C# and TS (SPEC §4.2).
*Gate (closed):* samples reviewed across the design sessions; v0.1 locked and used as the basis for the
P2 MVP. (Semicolons clarified as an optional separator during P2 — grammar/SPEC updated to match.)

## P2 — Walking skeleton (MVP) ✅ done (2026-06-28) ★ thinnest end-to-end slice
Took a *minimal* language subset — `fn`, `i32`/`f64` + arithmetic, `let`/`var`, `if`/`while`, function
calls, `print` — **all the way through** the pipeline: lexer → recursive-descent parser → typer (name
resolution + types, annotated tree = the typed IR) → **hand-written C# *and* TS pretty-printers** (no
Roslyn/ts-morph — §4.3). `polyglot build foo.pg` emits `foo.cs` + `foo.ts`. This front-loaded the
project's biggest architectural bet — that **one high-level IR serves both targets** — and stood up the
crown-jewel **differential conformance test** at P2 instead of P5. Later milestones *widen* each pass.
*Delivered:* `src/MintPlayer.Polyglot.Core` (diagnostics/token/ast/lexer/parser/sema/emit_csharp/
emit_typescript/compiler); `polyglot build` in the CLI; 20 in-process unit/golden tests; and
`tests/conformance/` (`run-diff.ps1` + `programs/arithmetic.pg`).
*Gate (closed):* `arithmetic.pg` → emitted C# compiles+runs under `dotnet` and TS runs under `node`
(type-stripping), with **identical stdout** (`128 / 28 / 50`); `run-diff.ps1` is green. Semicolons are an
optional separator (statements are newline-terminated) — grammar/SPEC aligned to the samples.

## P3 — Full front-end (lexer + parser) ✅ done (2026-06-29)
Widened the MVP front-end to the entire P1 grammar: full token set + a recursive-descent parser over the
whole surface — expressions (member/index/call, `?.`/`??`/`!`, ranges, lambdas, list/tuple/`with`, if-expr,
`match` + patterns), the `TypeRef` type grammar (named/generic/tuple/function/nullable), all statements
(`for`/`while`/`do`-less, `try`/`catch`/`when`/`finally`, `throw`, `use`, `yield`, break/continue,
compound/lvalue assignment), declarations (`fn`/`record`/`class`/`interface`/`extension`/`enum`/`union`,
members, generic params/bounds, default params), and `import`. A canonical `.pg` pretty-printer
(`pg_printer`, exposed as `polyglot fmt`) is the fidelity surface.
*Gate (closed):* all 10 `docs/lang/samples/*.pg` (incl. `fruitcake_sketch`) round-trip source → AST →
source idempotently — `tests/fidelity/run-roundtrip.ps1`, wired into `/build-and-test`. Malformed input
yields `file:line:col` diagnostics. Built incrementally (P3 1 → 3e-3), green at every step.
String interpolation is fully parsed (chunks + hole expressions, re-entrant lexer). *Deferred (small, not
the gate):* the `{ get/set }` property-accessor form (samples use read-only `=> expr` properties only),
nested strings inside an interpolation hole, and nested-generic edge cases beyond the samples. The
trivia-bearing lexer keeps comments/whitespace for *later* readable output; the P3 printer is canonical
(re-formats), not trivia-preserving.

## P4 — Full semantics + typed IR ✅ done (2026-06-29)
Built the static type system + the **separate typed IR** (per the design decision to make the IR its own
tree, not just the typed AST): name/type resolution across the whole declaration surface (unknown-type,
duplicate, missing-member, wrong-arity diagnostics); nominal expression typing (members, construction,
method calls, operator-overload lookup, `this`/`super`, `EnumName.Case`, nullability) — lenient on
unknown/generic/std types so there are no false positives; pattern-match exhaustiveness (union/enum/bool +
catch-all required for non-enumerable scalars); a dedicated IR (`ir.hpp`: typed tagged hierarchy carrying
resolved decisions — the `print` intrinsic, the `main` entry) produced by a lowering pass (`lower.hpp`,
AST→IR after sema); and the **backends rerouted to emit from the IR**, so the pipeline is now
lexer→parser→sema→lower→IR→backend (PRD §4.1/§4.2).
*Gate (closed):* type errors reported with `file:line:col`; a deterministic **typed IR dump** (`<expr>:<type>`)
verified by unit tests; resolution/typing/exhaustiveness covered by ~25 sema/IR unit tests. Conformance
unchanged (arithmetic.pg → IR → identical C#/TS).
*Notes:* full semantic checking runs on self-contained programs; the std-using P1 samples get end-to-end
type-checking at P7 (when `List`/`Error`/`sqrt`/… exist). The IR/lowering cover the MVP subset today and
widen to the full §3.A surface in P5. Expr nodes now carry a resolved `TypeRef`. Generic *instantiation*
substitution and full overload *mangling* are best-effort/lenient for now (refined as P5/P6 need them).

## P5 — Backends to the full §3.A surface ✅ done (2026-06-29)
Widen both hand-written pretty-printers from the MVP subset to the **entire supported surface**: records,
enums, unions + pattern matching, iterators, exceptions, `using`/disposal, extension methods, operators,
properties/indexers, closures — idiomatic in each target. Golden-output baselines checked in for **both**
targets; the differential conformance suite (stood up at P2) grows to cover the surface.
Also introduce the **backend-interface seam**: a small `Backend` abstraction (`name()` + `emit(unit)`)
selected via a registry, replacing the `if/else` on `Target` in `compile()`. Backends stay compiled-in,
but this is the shape the P9 declarative-plugin API grows from — the natural moment, with two *complete*
native backends to generalize across (the design note's "extracted, not guessed").
*Gate:* P1 samples emit C# that compiles under `dotnet build` and TS that type-checks under `tsc` + runs
under Node, both with expected output; golden baselines green; the differential suite passes.
*Delivered (2026-06-29):* the **backend-interface seam** is in (a `Backend` abstraction +
registry; `compile()` selects via `findBackend`). The IR/lowering/both backends now cover **records**
(fields, methods, operators, properties), **enums**, **unions + pattern matching** (exhaustiveness, ctor
patterns/binders), **operators & properties** (C# `operator +` / expr-bodied property vs TS `.plus()` /
getter), **classes** (mutable reference types with `init`, **inheritance + `super(...)`**), **`for…in`**
over ranges and iterables, **iterators (`yield`** → C# `IEnumerable`+`yield return` / TS `function*`),
**exceptions** (`throw` + `try`/`catch`/`when`/`finally`; TS gets an `instanceof`/guard dispatch chain),
and **`use`/disposal** (→ `try/finally` + `.dispose()`). `Iterable` and `Error` are registered as core
builtin types. The differential suite grew from 1 → 11 self-contained programs (arithmetic, records,
vec2, enums, unions, counter, forrange, iterator, exceptions, inheritance, disposal), all agreeing.
*Lambda syntax* also admits the bare single-parameter form `x => …`. **Closures/lambdas** emit (native
arrow functions both sides; function types → `Func`/`Action` / `(a:T)=>R`). **Extension methods** emit:
C# `static class Extensions` of `this`-methods (call site `x.m()` survives), TS free functions (call site
becomes `m(x)`) — the §3.E call-site divergence, exercised by `extensions.pg`.
**§3.E per-target capability gating is implemented early and active**, not just designed: `backend.hpp`
has a closed `Feature` enum + `Backend::supports(Feature)`; `capability.cpp` walks the AST for used
features and `compile()` refuses (per target, intersection-wise) any a target can't emit — a clear error,
never a miscompile. C#/TS declare the full set, so nothing is gated yet; a `StubBackend` unit test proves
the gate bites and names the feature + target. Finally **generics** emit: `<T>` parameter lists + bounds
(C# `where T : …`, TS `<T extends …>`) on functions/records/classes/methods/extensions, and explicit
construction type args (`Box<i32>(7)` → `new Box<int>(7)` / `new Box<number>(7)`).
*Gate (closed):* full §3.A surface lowers + emits; **14 differential programs** agree across C#/TS; 10/10
round-trip; capability gate proven by unit test. Golden `tsc`/`dotnet build` baselines deferred to P7
(std-using samples) — the differential run already compiles+runs both targets per program.

## P6 — Faithfulness pass ✅ done (2026-06-29)
Implement the §3.C relaxations *as documented behaviour*: int32/uint masking (`|0`/`>>>0`/`Math.imul`),
opt-in `Math.fround` strict floats, `BigInt` for int64, structural equality/hashing, null/undefined
normalization. Enforce the §3.B **refusals** with clear, actionable compiler errors (not miscompiles).
*Gate:* a numeric conformance suite passes within tolerance across both targets; every refused feature has
a refusal test asserting the diagnostic; the relaxation list is written up in the spec.
*Delivered:* §3.B **refusal diagnostics** (threads/locks, `decimal`, pointers/unsafe, `dynamic`, expression
trees, reflection → "Polyglot refuses X" with a unit test each); **all integer widths faithful** — i8/i16/
u8/u16/u32 narrowing (TS bit-ops; C# casts), i32 `|0`/`Math.imul`, **i64/u64 → BigInt** with 64-bit wrap;
**structural value equality** for records (C# native, TS generated `equals`); **explicit casts `(T)x`** +
**implicit lossless widening** (replacing conversion methods); and **function overloading** (compile-time
resolution; C# native name / TS param-mangled). 8 new differential programs (int_overflow, int64,
int_widths, equality, casts, widening, overloading) + refusal/overload unit tests. *Deferred to its own
step (low value):* opt-in strict-f32 `Math.fround`, statement-level `lock`/`unsafe` refusals, null/undefined
normalization (better with std). Numeric conversion design recorded in PRD §3.A / SPEC §3.

## P7 — Std core + expect/actual + FFI (the three plugin mechanisms, as first-party code) ✅ done (2026-06-29)
A minimal portable standard library in `.pg` (math, basic collections, iterators) compiled to both
targets. The **target-gated `expect`/`actual`** mechanism (portable core forbidden from touching platform
APIs; per-target `actual` impls) and an `extern`/inline-target **FFI hatch**. This is where the three
mechanisms of the plugin architecture — *binding*, *replacement*, *capability* — are proven **as
first-party code** behind a backend interface designed to become the plugin API (see the plugin design
note, below). This milestone also introduces **static methods on types** and the first use of them:
**string↔number parsing as static type methods** — `i32.parse(s)`/`i64.parse(s)`/`f64.parse(s)` (throwing)
and `tryParse(s): T?` (nullable) — realized per target (C# `int.Parse`/`TryParse`, JS `Number`/parse +
range checks). Parsing is *not* a cast (PRD §3.A/§4.4): a cast `(T)x` converts between numeric types, parse
reads text. (This unblocks the `.pg` samples that currently sketch `s.toI32()` — now `i32.parse(s)`.)
*Gate:* a program using a portable std API + one `expect`/`actual` capability (e.g. current time) builds
and runs identically on both targets; `i32.parse("42")` round-trips on both; the portable core cannot
reference `document`/`System.*` (compiler-enforced, with a test proving the rejection).
*Delivered:* **static methods** on types (`Type.method()`, native on both); **`i32.parse`/`i64.parse`/
`f64.parse`** (string→number, per-target idiom); the **`Math`** namespace (`sqrt`/`ln`/`abs`/`min`/`max`/
`floor`/`ceil` → `System.Math.*` / `Math.*`) — the **replacement** mechanism; **`expect`/`actual`** target-
gated capabilities (each backend emits only its matching `actual`) — the **capability** mechanism;
**`extern("…")`** raw-code FFI — the **binding** mechanism; and the **portable-core guard** (`extern`
refused outside a target-gated `actual`). 6 new differential programs (static_methods, parse, math,
expect_actual, extern_ffi) + extern-guard unit tests. *Deviation from the sketch:* the std is currently
**compiler-builtin intrinsics** (Math/parse), not yet a portable std *written in `.pg`* with an import
mechanism — that (and `tryParse`, collections) is a further step; the three plugin mechanisms are proven,
which is the milestone's point. At P9 these intrinsics become declarative plugin data.

## P8 — Dogfood: the FruitCake physics ★ north star — ✅ DONE
Expressed the MintPlayer.AI FruitCake circle-physics solver in `.pg`
(`docs/lang/samples/fruitcake_sketch.pg`): the full sequential-impulse circle solver with rigid-body
rotation, walls, deferred same-tier merges, settle-to-rest. Generates `FruitCakeWorld`-equivalent C# and
`fruit-cake-physics`-equivalent TS from one source.

**Delivered (beyond the original plan):**
- Written in **`f64`** so the solver's `+ − × ÷ √` are **bit-exact across .NET and JS** (§3.D). The two
  emitted twins therefore agree *to the bit* — a stronger result than the hand-ported `float`/`number`
  twins, which diverge from each other. So the conformance gate asserts **byte-identical** stdout
  (`bodies=1 scored=1` on a scripted two-fruit drop+merge), not just behavioural tolerance.
- **`List<T>` as a first-party `.pg` std type** (the principled path, not a compiler intrinsic) via a new
  **binding mechanism**: a method/property body of `actual(target) extern("…template…")` arms with
  `$this` (receiver) / `$0…` (args) substituted at each call site; a `$this = …` arm emits a receiver
  *assignment* (`list.clear()` → C# `xs.Clear()` / TS `xs = []`; `list.removeAll(p)` → C# `xs.RemoveAll(p)`
  / TS `xs = xs.filter(e => !((p)(e)))`). `extern class` = native-backed type (not emitted). Embedded std
  module linked on `import std.collections.{ List }` (no filesystem resolver yet). List literal / `lst[i]` /
  `for x in` / `for (a,b) in` element typing is compiler-level (syntax the bindings can't express).
- Lowered + emitted for the first time (all previously fell through to a silent `0`): `null` literal,
  `x!` null-assert (→ cast to the non-null type, unwrapping C# `Nullable<T>`), `??`, string interpolation,
  index, tuple literal, top-level `const`/`let` globals, tuple destructuring in `for`.

*Gate (met):* `fruitcake` is conformance program #27; generated C# and TS produce identical stdout. The
27-program differential suite + 10-sample round-trip + in-process unit tests are all green. Polyglot has
earned the right to own that physics; the hand-ports can retire while the conformance test guards the
generator. *Not yet done here:* wiring the generated output back into the live MintPlayer.AI repo (out of
scope — that repo's build is not run from here) and a real module-resolution system (std is embedded source).

## P9 — Declarative backend engine + DSL — 🚧 in progress
**Concrete design + extraction map: [`../design/backend-spec.md`](../design/backend-spec.md).** A structural
catalog of both emitters found the split is ~70% tabular / ~30% imperative, so a backend = **Spec (declarative
data)** + **Hooks (C++ for the imperative 30%: `tsConvert`, TS `try` lowering, numeric narrowing, operator-
method dispatch, record equality, `Match`, interpolation)** + capability set, over **one shared emit engine**.
Migration is incremental, each slice a byte-for-byte no-op enforced by the golden/differential/round-trip
gates. **Slice 1 ✅:** the scalar type-leaf table extracted into `BackendSpec` (`backend_spec.hpp`); both
emitters consult their spec instead of hardcoded type ladders. *Next slices:* operator/precedence + literal-
suffix tables → spec; per-node templates + the `SpecEmitter` engine; declaration scaffolds; formalize Hooks.

The backends, generalized. *Extract* a declarative backend format from the two **native** C#/TS backends
(P4/P5) — a rule/template per IR node (context-aware: precedence, expr-vs-stmt position), the std-type
mapping, operator/keyword tables, naming/import rules, and the build-project scaffold. Build the core's
**declarative emit engine** that interprets a spec, and **re-express C# and TS as declarative specs**.
Critically, the DSL is extracted from working backends, never guessed (the §4.3 "design it twice"
discipline applied to the format itself); a **local full-power plugin tier** covers what the DSL can't yet
express. See **[`../design/plugins-and-targets.md`](../design/plugins-and-targets.md)** §4/§7.
Each backend spec also declares its **capability set** — the named §3.E flags (`extensionMethods`,
`operatorOverloading`, `properties`, `iterators`, `patternMatching`, …) it can emit. C# and TS both declare
the full §3.A set, so the format carries capabilities from the start even though nothing is gated yet.
*Gate:* C#/TS emitted via the declarative specs match the native backends' golden output byte-for-byte;
each spec carries a capability set (both = full §3.A).

## P10 — Plugin distribution + ecosystem (the endpoint of §4.4)
The downloadable, declarative plugin system: a **workspace config (`pgconfig.json`)** declaring target
*environments* (desktop/web/mobile/…) + plugins+versions; **download → shared cache → verify → lockfile**
(declarative data only — no executable code fetched; integrity-pinned, zip-slip-safe); **availability
resolution** by target + environment (off-target use is a compile error, never a miscompile); and
**build-dependency threading** (a plugin declares the NuGet `PackageReference`s/SDK or npm deps its output
needs; the core emits a buildable project including them). Trust model + open decisions in the design note.
This is also where **§3.E capability-gating** first *bites*: the usable §3.A surface for a project becomes
the **intersection** of the configured backends' declared capability sets, and using a feature outside it
is refused at compile time naming the capability + the lacking target (distinct from a §3.B global refusal).
A natural exercise: the downloaded Python backend declares a capability set that omits, say, call-site-
preserving `extensionMethods`, and a program using extension methods while targeting Python is refused.
*Gate:* adding a **downloaded declarative Python backend** *and* a target-scoped binding plugin (e.g.
WinForms, with its PackageReferences) requires **no core change** — only `pgconfig.json` + downloads; a program
using them emits a buildable project, and wrong-target/-environment use **and use of a feature outside the
target intersection** are each rejected with a clear, distinct diagnostic.

**Prerequisite mechanism gap to close here (noted P13, 2026-06-29):** a plugin/`extern class` can currently
bind **member/property access** (`$this.method($0)`) but **not its own type-name → target type, nor its
construction**. Only the std-blessed types are hardcoded (`List`→`System.Collections.Generic.List`/`T[]`,
`Iterable`, `Error`; `List<T>()`→`new List`/`[]` in `csType`/`tsType`/emit). So a user plugin class's type
name and `new T(…)` emit literally today. P10 (or a small precursor) must let a binding plugin declare its
**target type spelling** (feeding the P9 `BackendSpec` type table) and a **constructor template**, so an
`extern class` is fully usable — instances constructed, not just methods called on params/FFI results. This
is the "Binding" mechanism (plugins-and-targets.md §2) reaching its complete form. See `design/backend-spec.md` §4a.

## P11 — Build integration: the `.pg`-aware NuGet (and npm) on-ramp
Make adoption frictionless: a developer adds a package to an ordinary C# project, drops in `.pg` files,
runs `dotnet build`, and the `.pg` is transpiled to C# and compiled into the assembly with **no manual
step and no extra SDK/runtime** — the payoff of the §4.3 self-contained native CLI (the consuming dev
already has the .NET SDK; the transpiler ships in the package). This milestone depends only on a stable
CLI (the `.pg → .cs` path works from P5), **not** on the P9/P10 plugin engine — it can ship independently;
it sits here because it's an ecosystem/distribution concern. A sibling **npm + build-script** story does
the same for TypeScript projects.

**Design (modeled on `Grpc.Tools` — the closest prior art: a *native, non-.NET* compiler invoked at build
to turn a custom source into `.cs` that joins `@(Compile)`; a Roslyn source generator can't be used because
the input is a foreign language and the transpiler is a native binary):**
- **Package** `MintPlayer.Polyglot.MSBuild` ships the native `polyglot` CLI **per host RID** (win/linux/osx
  × x64/arm64) plus `build/<id>.props` + `build/<id>.targets` (auto-imported: props near the top, targets
  near the bottom of the consuming project). `.props` resolves the host RID and tool path from
  `$(MSBuildThisFileDirectory)` (via `$(NETCoreSdkPortableRuntimeIdentifier)`), with an env-var/property
  **override hook** for CI; on unix it `chmod +x`'s the binary (NuGet drops the exec bit).
- **Non-transitive by construction:** assets go in **`build/`** (not `buildTransitive/`), and the package is
  marked **`DevelopmentDependency=true`** so it never flows to downstream consumers or into published
  output; consumers may reinforce with `PrivateAssets="all"`. Installing it in project A does **not** impose
  it on a project that references A.
- **Hook + incrementality:** a target `BeforeTargets="CoreCompile"` transpiles each `.pg` → `*.g.cs` into
  `$(IntermediateOutputPath)` (`obj/`, so no source-tree noise and no double-compile against the SDK's
  default `**/*.cs` glob) and adds them to `@(Compile)` + `@(FileWrites)` (for clean/rebuild). `Inputs`/
  `Outputs` (including the tool path, so a CLI upgrade forces regen) skip unchanged files; a companion
  always-run target re-adds the existing generated files when the transpile target is skipped (else the
  compiler loses them). Uses the `Exec` task (no managed MSBuild task — keeps the zero-dep ethos).
- **Design-time builds run the transpile** (do not skip on `$(DesignTimeBuild)`) — Grpc.Tools' hard-won
  lesson: skipping makes generated types vanish from IntelliSense.
- **Fail loudly** if no native binary matches the host RID (name the RID + the override property) — never
  silently skip, which would cascade into misleading "type not found" errors. Don't fetch a runtime at
  build (the Antlr-JRE / TypeScript-needs-node trap) — the self-contained binary is exactly the advantage.
*Gate:* a fresh C# project + the package + a `.pg` file builds with **`dotnet build` alone** (the `.pg`
types are usable from C# in the same project, incl. the IDE); a second build with no `.pg` change skips
transpilation; `dotnet clean` removes the generated files; and a project referencing the first project's
output does **not** inherit the transpile behavior or the package dependency.

## P12 — Modules, imports & name resolution — ✅ DONE (phase-1)
Turn the P8 embedded-std foothold into a real module system (full design in PRD §4.5). Independently
sequenceable — frontend-only, depends just on the current pipeline — and a natural companion to P10's
`pgconfig.json` workspace. **Delivered:** the import syntax, the `ModuleResolver` seam + transitive
cross-`.pg` loading, and collision *detection*. **Phase-2 (deferred):** selective-import visibility
restriction and `as` rebinding — both need a per-file import-scope table (the current merge-into-one-unit
model has no place to hang per-file visibility); the *safety* property (never silently shadow) is already
met. Three tracks:

- **New import syntax:** `import { a, b as c } from "spec"`, `import * as ns from "spec"`, bare
  `import "spec"`. `from` is a contextual identifier (no new keyword); the specifier is a quoted `StringLit`
  (a bare `"std.io"` = logical name, a `"./x"` = importer-relative). Rewrite `ImportDecl` (per-name aliases
  + namespace), `parseImport`, and `importStr`; migrate the ~14 existing `.pg` import lines + grammar.ebnf
  + SPEC §11 in the same change (round-trip fidelity is atomic). No lexer/token change.
- **`ModuleResolver` seam for cross-`.pg` imports:** add `compile(source, target, ModuleResolver* = nullptr)`
  (default = std-only, so all existing callers/tests are unchanged). Core generalizes `linkStdModules` into
  a transitive load + `visited` dedup + `inProgress` cycle-detection (clear `a → b → a` error) + post-order
  merge; the resolver only maps specifier→source. CLI ships a `FileModuleResolver` (std registry first, then
  `<root>/a/b/c.pg` for logical names, importer-relative for `./`), with `--root` (later `pgconfig.json`);
  tests ship an in-memory `MapModuleResolver` (no disk). Core stays IO-free.
- **Collision policy — refuse loudly:** ✅ any name collision is a hard error; builtins can't be shadowed;
  functions merge as overloads unless the signature is identical; the silent last-wins holes for top-level
  values / union ctors / extensions are closed. Exports = all top-level public for now (`private` marker
  deferred). ⏳ *phase-2:* selective imports *restricting* visibility (today they validate names but still
  merge the module's whole public surface) and `as` *rebinding* (parsed + recorded, not yet applied) —
  both await the per-file import-scope table.

*Gate (met):* a two-file program (`entry.pg` importing `"./util"` + a logical module) builds via `--root` to
identical C#/TS stdout; a transitive chain `a → b → c` resolves (with shared-dependency dedup); an import
cycle, an unknown module, an unknown imported name, and a cross-module collision (type / value / union case /
extension) are each a clear diagnostic; the in-process tests cover all of this with the in-memory resolver
(no filesystem); existing single-file programs are unaffected. (Phase-2 visibility/`as` is the remaining
gate.)

## P13 — Std as real modules + the `lib` prelude
Close the "builtins that bypass imports" gap (full design in PRD §4.6). Today `print` and the `Math`
namespace are hardcoded builtins; the samples that `import` them are actually **broken** (the import
validation rejects them — `std.io` has no `print`, `std.math` doesn't exist) and only survive because the
fidelity gate `fmt`s rather than compiles. Make the std honestly import-based; keep `i32.parse` global and
`Error`/`Iterable` core. Three tracks, sequenced (the `lib` track is a prerequisite for low-churn rollout):

- **`lib` prelude (do first):** add a `LibConfig` (list of names) param to `compile()`; synthesize one
  whole-module `ImportDecl` per entry (`"io"` → `std.io`), tagged lib-origin; reuse `linkModules`. Add a
  `dropShadowedLibDecls` pre-link pass so a lib decl **loses silently** to any user/explicit decl of the same
  name (explicit-vs-explicit and lib-vs-lib still hard-error). Provenance via an `isLibAuto` flag on
  `ImportDecl` + an `originLib` tag threaded through `mergeDecls`. CLI `--lib io,math`; `pgconfig.json`
  `"lib"` deferred to P10. Core stays IO-free; the defaulted param keeps every existing `compile`/test call
  working.
- **`Math` → `std.math`:** an `extern class Math` with bound static members + `PI`/`E` constants (call
  surface `Math.sqrt(x)` unchanged). Generic `min<T>/max<T>/abs<T>` get type-preservation from normal
  generic typing; IIFE-form operator-ternary templates are `number`+`bigint`-safe and evaluate-once. Delete
  `mathArity`, the `Math` sema/lower/emit special-casing, `mathRename`. *Minor* parser work: accept binding
  arms on a `const` member (or model `PI`/`E` as bound static properties).
- **`print` → `std.io`:** generic `expect fn print<T>(x: T)` + per-target `actual` `extern`; i64/u64 TS
  `actual` overloads carry the `String(…)` wrap (pure data); retain a one-line sema guard rejecting
  non-printable args (no `Printable` bound needed yet). Delete the `isPrint` flag + `printFn`.

*Gate:* `print`/`Math` are unresolved without an import or a matching `lib` entry; `--lib io math` (or
`import`s) makes them available; a user `print`/`Math` or explicit import **silently shadows** the lib one
(no error) while two explicit imports of a name still error; the (now-fixed) `docs/lang/samples/*.pg` join a
**compile** check, not just `fmt`; and the `math` + all `print`-using conformance programs stay byte-identical
across C#/TS (goldens regenerated where the Math min/max template changes). A full `Printable` bound and
per-name `lib` config are explicitly out of scope.

## Stretch (unordered, post-P10)
- **Further targets** as downloadable declarative backends (the IR is target-neutral by design).
- **Source maps:** thread positions through every pass for debuggable JS output; decide the C# debug story.
- **LSP:** build the frontend as a reusable library; make the CLI and a future language server thin clients.
- **Binding auto-generation:** shape-only bindings from `.d.ts` / .NET metadata / WebIDL + hand overrides
  (feeds the plugin ecosystem; produces declarative data at authoring time).
- **Plugin registry & signing:** distribution/versioning infrastructure + signature trust for downloads.
