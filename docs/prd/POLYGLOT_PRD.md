# MintPlayer.Polyglot — Cross-SDK Transpiler (PRD & Plan)

> Write logic **once** in a small, purpose-built source language; emit **idiomatic, readable** code for
> multiple target SDKs — **C#/.NET** and **TypeScript/JS** first. Polyglot is deliberately *not* a
> general-purpose "any language → any language" compiler (a multi-decade trap); it is **faithful-by-
> default with a published relaxation list**, and it **refuses** the features that sink transpilers.

- **Status:** Draft v1.0 · 2026-06-29 · P0 built; P1 locked; P2 MVP; P3 (full grammar + round-trip);
  **P4 done** — semantics + a separate typed IR; pipeline is lexer→parser→sema→lower→IR→backend. Next: P5
  (widen IR/lowering/backends to the full §3.A surface).
- **Author:** Pieterjan (with Claude Code).
- **Provenance:** distilled from a four-agent investigation into multi-target transpilers (Haxe, Kotlin
  Multiplatform, Fable, Scala.js, TypeScript, J2CL/GWT, JSIL, Bridge.NET). The investigation's
  "what to support / what to refuse / how others die" findings *are* this document's spec (see §9 Sources).
- **Motivating dogfood:** the MintPlayer.AI **FruitCake physics solver**, hand-maintained today as a C#
  twin and a TS twin. Generating both from one Polyglot source — with a differential conformance test
  proving they agree — is the v1.0 north star (§6 P8). The twins live in the **sibling repo**
  `C:\Repos\MintPlayer.AI`: C# at `src/MintPlayer.AI.ReinforcementLearning.Environments/FruitCake/FruitCakeWorld.cs`,
  TS at `src/RLDemo.Web/ClientApp/src/app/fruit-cake/fruit-cake-physics.ts` (the two files declare each
  other canonical mirrors). "M30" referenced below is that repo's milestone for shipping the hand-ported
  client-side physics + conformance test (see its `docs/prd/FRUITCAKE_CLIENT_AI_PRD.md`).

---

## 1. Vision & the problem it solves

Cross-platform projects routinely need the *same logic* in more than one language: a physics solver in
C# (for native/training) **and** TypeScript (for the browser); validation rules on a .NET backend **and**
an Angular frontend; a state machine in both. Hand-porting works but rots — the two copies drift, and
parity becomes a manual, error-prone chore.

The investigation's blunt lesson: a **general** transpiler that supports "as many features and platform
APIs as possible" is a person-decade effort that only survives with a corporate sponsor or a founder's
life-long stewardship, because it drowns in (a) the runtime-pushed language features and (b) the
unbounded standard-library/platform-API surface. **Polyglot wins by refusing that scope.** It targets
the *portable-logic* sweet spot — pure computation, data, and control flow that is identical everywhere —
and explicitly hands platform APIs back to the target via thin bindings.

The decision rule we build around: **the more the shared surface is structure/logic, the better a
transpiler fits; the more it is platform glue, the less.** Polyglot owns the first half.

---

## 2. Goals & Non-Goals

### Goals
- **One source language → idiomatic, readable C# *and* TypeScript** for the supported feature set.
- **Faithful-by-default semantics with a documented relaxation list** (the Scala.js model) — never silent
  surprises.
- **A self-contained native toolchain** (single `polyglot` CLI, zero runtime dependency) — fitting the
  from-scratch ethos of the surrounding repos.
- **Differential conformance built in**: the same source → both targets → a generated test asserting they
  produce identical results. Parity is a *test artifact*, not a hope.
- **Dogfood the FruitCake physics** as the first real program, retiring its hand-ported twin.

### Non-Goals
- A general "any-language-in, any-language-out" compiler.
- Broad platform-API coverage (all of the DOM *and* all of the .NET BCL) — incoherent by deployment
  target; handled instead by **target-gated bindings** (§4.4).
- The 🔴 runtime-heavy features (see §3.B): threads/locks, runtime reflection, finalizers/GC-timing,
  `decimal`, `unsafe`/pointers, `dynamic`/runtime code-gen, **bit-exact cross-target floats**.
- Beating Haxe/Kotlin/Fable at their game. If "many mature targets" is ever the goal, adopt one of those.
  Polyglot is a focused, owned tool for *this* author's portable-logic needs (and a long-loved craft).

---

## 3. Language-feature spec (the support/refuse contract)

Derived directly from the investigation's per-feature analysis. **This is the scope contract** — the
single most important thing to hold the line on, because every dead transpiler died by not holding it.

### A. Supported — lowers cleanly to **both** C# and JS (the 🟢 core)
*(This is the maximal surface, calibrated to the first two targets. With more targets the usable surface
is the per-project intersection of backend capabilities — see §3.E.)*
Operators (overloading → static calls), properties/indexers (→ get/set / `get_Item`), extension methods
(→ static calls), exceptions (`try/catch/finally`, typed catch → `instanceof` dispatch + rethrow, `when`
→ guard), `using`/disposal (→ `try/finally`), iterators/`yield` (→ `function*` / `IEnumerable`),
pattern matching / discriminated unions (→ tagged objects + switch, compile-time exhaustiveness), enums,
closures / lambdas (`x => …` or `(a, b) => …`, both → native arrow functions; capture-by-ref agrees on
both sides), function overloading (→ compile-time name-mangling; C# keeps the source name, TS mangles by
parameter type), **numeric conversions via explicit casts `(T)x`** (not a `toI64()`/`toU32()` method swamp)
with **implicit lossless widening** (a narrower value flows into a wider slot automatically — assignment,
argument, return, mixed-width arithmetic; narrowing, lossy `i64→f64`, and sign changes require the cast —
see SPEC §3), strings/char (**both targets are UTF-16** — near 1:1, with surrogate-pair care), and
**single-threaded `async`/`await`** (a "colored function" → C# `async Task<T>`, TS `async … Promise<T>`,
Python `async def`; the author writes the unwrapped `T` — *designed §4.7, not yet implemented; it is the
first supported feature a real target (PHP) can't express, so it's capability-gated per §3.E*).

### B. Refused — out loud, with clear compiler diagnostics (the 🔴 list)
Threads / `lock` / `Interlocked` / `Parallel.*` (single-threaded async only); runtime reflection /
`Activator` / open-world `Type.GetType(string)` (compile-time metadata only — reflection defeats tree-
shaking and is the #1 bloat source); finalizers / non-deterministic GC hooks (keep `using`, drop `~T()`);
`decimal` (unless a big-decimal lib is later opted in); `unsafe` / pointers / `stackalloc` / raw `Span`;
`dynamic` / `Reflection.Emit` / `Expression.Compile`; **LINQ expression trees** (`Expression<Func<…>>` —
code-as-inspectable-data has no portable JS counterpart; a lambda is *always* an executable closure, never
a queryable tree); **bit-exact cross-target floating point** (see §3.D).

### C. Faithful-by-default, with a *published relaxation list* (the 🟡 corners)
- **int32 / uint / sub-32 overflow:** mask at operation boundaries (`|0`, `>>>0`, `Math.imul`) so JS
  wraps like .NET. Documented relaxation: intermediate overflow below 2⁵³ that .NET would wrap is *not*
  caught unless an explicit `Int32` type is used (the Fable/Haxe leak — made explicit, not silent).
- **int64 / long:** emit JS `BigInt` (ES2020) — correct, at a perf cost; the alternative two-word `Long`
  class is a later option if BigInt proves too slow.
- **float (32-bit) vs double:** default lets `float` ride a JS `double`; `Math.fround`-per-op strictness is
  **opt-in** (the Scala.js strict-floats tax) for code that needs single-precision rounding parity.
- **nullability:** normalize `null`/`undefined`; pick one and stick to it. **Nullable generics use a real
  `Option<T>` (✅ done 2026-06-30 — see PLAN P14c):** `T?` over an *unconstrained* type
  parameter has no faithful native emission — C# `null` is CS0403, `T?`+`default(T)` returns `0` for value
  types (silent divergence from TS `null`), and `Nullable<T>` is value-types-only (CS0453). So `T?` whose
  base is a bare generic parameter **desugars to a real `Option<T>` generic union** (`Some(x)`/`None`),
  emitted via the existing union machinery (tagged on both targets — distinguishes `Some(null)` from `None`).
  Concrete/reference `T?` keeps the idiomatic native nullable (C# `int?`/ref, TS `T | null`).
- **equality / hashing:** generate structural `Equals`/`GetHashCode`; identity hash via a side `WeakMap`.

### D. The determinism honesty clause
Bit-exact cross-runtime IEEE-754 is **not a promise Polyglot makes.** Only `+ − × ÷ √` are correctly-
rounded and reproducible (and only at matched width); **transcendentals** (`sin`/`cos`/`exp`/`pow`),
FMA contraction, and JIT reassociation diverge between the .NET JIT and a JS engine. Code that needs
identical results across targets must use the std's **fixed-point / soft-float** numeric type (a planned
std module), *not* `float`/`double`. (The FruitCake solver uses only `+ − × ÷ √`, so its differential
test gates on **tolerance + behavioural equality**, never bit-equality — see the M30 plan in MintPlayer.AI.)

### E. Per-target capability negotiation (the multi-target generalization)
§3.A is written against C# and TS, which both happen to support the **entire** supported surface. That
two-target coincidence hides a question every *additional* target raises: not every SDK can express every
§3.A feature with an idiomatic, call-site-preserving mapping. Concretely (from the cross-SDK survey):
**extension methods** keep `x.method()` on C#, Kotlin, Swift, Dart, Rust (extension-trait + `use`), Ruby,
but **cannot** on Java, Go, C++, PHP — there is no language mechanism to attach instance-call syntax to a
type you don't own; the only faithful emission is a free function `method(x)`, which *changes the call
site*. Several targets also lack operator overloading, properties, etc.

**Decision — capabilities are declared, intersected, and gated at compile time:**
- Each **backend declares a finite, named set of capability flags** — one per §3.A feature
  (`extensionMethods`, `operatorOverloading`, `properties`, `iterators`, `patternMatching`, …). This is the
  §3.A list turned machine-checkable; the set is **closed** (resist growing it per feature request — that
  is the scope-creep failure mode).
- The **usable surface for a project is the *intersection*** of the declared capabilities across **all**
  targets configured in `pgconfig.json`. Using a feature outside the intersection is a **compile-time
  refusal that names the capability and the lacking target(s)** — never a silent miscompile and never a
  silent per-target call-site change.
- **Two refusal classes, kept distinct.** §3.B is a **global** hard-refusal (no faithful emission *anywhere*
  — target-independent). §3.E capability-gating is **target-set-dependent** (the feature is fine on some
  targets, withheld only when a configured target lacks it). The diagnostics must read differently:
  "Polyglot refuses X" (§3.B) vs "target *T* does not support X; remove it or drop *T*" (§3.E).
- **For C#/TS today the intersection is the full §3.A surface, so nothing is gated yet.** The mechanism is
  nonetheless **already implemented and active** (P5): each `compile()` walks the program for used features
  and refuses any the target can't emit — it simply never fires while both backends declare the full set.
  This means the gate is in place *before* a target that can't do a feature ever ships (no retrofit — the
  lesson from Haxe's late threading-capability retrofit); a third backend just declares a smaller set and
  the existing check starts biting. (A `StubBackend` test exercises the refusal today.)
- **`async`/`await` is the first supported feature with a plausible target that can't express it** (design
  §4.7): C#/TS/JS/Python all have it, but **PHP has no native `async`/`await`** (Fibers/amphp are
  library-level, not call-site-preserving), so a PHP backend would declare `Feature::Async = false` and the
  gate would refuse async when PHP is a configured target — exactly this clause's purpose. Note the
  intersection is currently *emergent*: the CLI compiles each target in a loop and fails if any refuses, so
  "usable only if ALL targets support it" already holds without a `pgconfig.json` intersection pass (P10).

This is the **survivor pattern**: Kotlin Multiplatform makes the common surface literally the intersection;
Haxe (`target.threaded`…), Rust (`cfg`/`target_feature`), LLVM target features, and Protobuf editions all
use named, declared, compile-time-enforced capability flags. The dead .NET→JS transpilers (JSIL, SharpKit,
Bridge.NET) had **no published, enforced capability contract** and so miscompiled in the corners — exactly
what this clause prevents. (One nuance: extension methods *already* differ in call-site between C# and TS —
method-call vs free function, SPEC §6.3 — so the capability is precisely "call-site-preserving extension
methods"; a backend may instead opt into a documented free-function lowering rather than gate the feature
out entirely. Tiers: native / idiomatic-with-import / free-function fallback.)

---

## 4. Architecture

Modeled on **Fable** (the closest analogue: existing-frontend-style → one shared typed IR → per-target
backends with a "Replacements" library + a pretty-printer).

### 4.1 Pipeline
```
source (.pg) → lexer (trivia-bearing) → parser → AST
            → name resolution + static type-check (diagnostics emitted HERE, on a near-source form)
            → lower to ONE high-level, typed, tree-shaped IR (desugaring folded in)
            → per-target backend: stdlib "replacements" + construct lowering
            → hand-written pretty-printer per target  → .cs / .ts  (+ source map where applicable)
```

### 4.2 The IR decision
**A single high-level, typed, tree-shaped IR — NOT SSA, NOT lowered to a common denominator.** SSA
destroys the loop/name structure needed for readable output; the common-denominator approach yields ugly
or impossible output. Keep the IR high-level and **specialize per target** (C# keeps `foreach`/`async`
natively; a weaker construct is lowered only where a target lacks it). C# and TS overlap massively (both
typed, OO, closures, generics, async), so lowering is needed only at the divergences (C# value
types/nominal typing vs TS structural typing/`Symbol.dispose`).

### 4.3 Implementation language — **C++** (decision, with the tradeoff logged)
**Chosen: C++20**, single self-contained native CLI, zero runtime dependency (consistent with the
from-scratch, zero-native-deps spirit of the sibling repos; the same path Haxe/Nim took with OCaml).
- **Consequence:** a C++ host **cannot** emit via Roslyn `SyntaxFactory` (C#) or `ts-morph` (TS). The
  backends therefore **hand-write the C# and TS pretty-printers** over the IR. This is normal for native
  compilers (Haxe hand-emits every target) and keeps the toolchain dependency-free, at the cost of
  writing/maintaining the emitters ourselves.
- **Alternative considered — C# + Roslyn host:** would let the C# backend reuse Roslyn's emitter and
  matches the house .NET stack, but adds a .NET runtime dependency to the tool and doesn't help the TS
  side. *Recorded as the main fork in the road; revisit only if hand-written emitters prove painful.*
- **Reinforced by the plugin model (2026-06-28):** a self-contained native CLI means **plugin *users* need
  no SDK/runtime** — a Roslyn host would force every consumer to install the .NET SDK merely to transpile.
  This holds *because* downloaded plugins are **declarative data, not loadable code** (so the host needs no
  managed plugin runtime); backends themselves are declarative specs the core interprets. See
  [`../design/plugins-and-targets.md`](../design/plugins-and-targets.md).
- **The build-integration payoff (P11):** because the CLI is a self-contained native binary, it can be
  shipped inside a NuGet package's `build/` targets and run during `dotnet build` to transpile `.pg` → `.cs`
  with **no extra SDK/runtime** for the consumer — the `Grpc.Tools` pattern, where a native compiler runs at
  build to feed `@(Compile)`. A managed/Roslyn host couldn't be embedded as cleanly. See PLAN P11.
- **Static CRT (2026-07-01):** the projects link the runtime statically (`/MTd`,`/MT`), so the CLI depends only
  on `KERNEL32.dll` — no MSVC/UCRT DLLs. This is what "self-contained" means in practice: it spawns under
  VS Code's extension host (which lacks the debug CRT on PATH — the bug that surfaced this) and will run on a
  P11 consumer machine with no prerequisites.

### 4.4 Standard library & platform APIs — the bounded strategy
The investigation's headline cost. Polyglot bounds it deliberately:
- **A small portable std** written in the source language (collections, strings, math, iterators) compiled
  to every target — the only thing guaranteed identical everywhere.
- **Target-gated native access via an `expect`/`actual` split** (Kotlin's model): portable code may name a
  capability (time, env, IO) via an `expect` declaration; each target supplies an `actual`. Platform APIs
  (`document`/`window` on JS, `System.*` on .NET) live **only** in target-gated regions — the portable
  core is compiler-forbidden from touching them (kills the #1 portability bug class).
- **String↔number parsing is a static method on the target type** — `i32.parse(s)`, `i64.parse(s)`,
  `f64.parse(s)` (throw on invalid input) plus `i32.tryParse(s): i32?` (nullable, non-throwing). It is
  deliberately **not a cast**, for two reasons: (1) a cast `(T)x` is a *total* numeric→numeric conversion
  that can never fail, whereas parsing text *can* fail — folding parse into cast syntax would hide that a
  `(i32)s` might throw; and (2) **C# cannot cast `string`→`int`** at all, so `(i32)stringExpr` couldn't even
  lower to a C# cast — it would have to secretly mean `int.Parse`, giving one syntax two semantics. It is
  also **not a free `parseI32` function**: parse is exactly *one* method per numeric type (you must name the
  target), so it's irreducible, not the per-type free-function swamp that motivated casts-over-`toI64()`.
  Lives in the portable std, realized per target (C# `int.Parse`/`int.TryParse`, JS `Number`/parse + range
  checks). This implies the language supports **static methods on types** (built-in on the primitives;
  `static fn` members on user types follow the same shape).
- **An FFI escape hatch** (`extern` / inline-target blocks) as the pressure valve past any unbound API.
- **No promise of broad coverage.** Bind what's used; bindings are auto-generatable in *shape* (from
  `.d.ts` / .NET metadata / WebIDL) but always need hand-written semantic overrides — so coverage grows
  on demand, never "complete."

> **Refinement (2026-06-28) — the plugin architecture.** The above is sharpened to its endpoint: the
> **core is a pure translator + a declarative emit engine** with *zero* hardcoded target/platform/SDK
> knowledge. **All** of it — bindings, replacements, capability `actual`s, and even the **target backends
> themselves** — lives in **declarative plugins** the engine interprets (C#/TS bundled; Python and others
> downloadable). Two tiers: **downloaded plugins are declarative data only** (safe to fetch, no host
> runtime, versioned + integrity-verified), while **local plugins may be full-power** for what the
> declarative DSL can't express. A **workspace config (`pgconfig.json`)** declares the target *environments*
> (desktop/web/mobile/…) and the plugins+versions in use; `pg` downloads them to a shared cache, and
> off-target/-environment use is a compile error, never a miscompile. Faithfulness (§3.C) and determinism
> (§3.D) apply to **core translation + portable std + bundled backend specs**; plugin output is the plugin
> author's contract. Full design, sequencing, trust model, and open decisions:
> **[`../design/plugins-and-targets.md`](../design/plugins-and-targets.md)**.

### 4.5 Modules, imports & name resolution (design — 2026-06-29)
P8 shipped the std as real `.pg` modules (`std.collections`, `std.io`) resolved from an embedded registry,
but with three rough edges: a dotted import syntax, no cross-file user modules, and a "merge everything,
collisions are accidental" model. The decided design (from a three-track investigation):

- **Import syntax — TypeScript-style, quoted specifier.**
  `import { readText, writeText as wt } from "std.io"`, plus `import * as io from "std.io"` (namespace) and
  bare `import "std.io"` (link only). `from` is a **contextual identifier** (not reserved — `from` is too
  common a name to burn), matching how `as` already works. The module specifier is a **quoted string**
  (`StringLit`), which is the load-bearing call: a bare specifier (`"std.io"`, `"app.physics"`) is a logical
  module name, while a `./`-prefixed one (`"./physics"`, `"../shared/vec"`) is importer-relative — the
  Node/TS/Deno convention, and impossible to express with a bare dotted path. Cost is localized: `ImportDecl`
  gains per-name aliases + a namespace field, `parseImport`/`importStr` are rewritten, **no lexer/token
  change**, and ~14 existing `.pg` import lines migrate. (`InterpString` specifiers — a `"…${}…"` — are
  refused with a clear diagnostic.)

- **User-module resolution — a pluggable `ModuleResolver`, Core stays IO-free.** `compile()` gains one
  optional `ModuleResolver*` parameter (default `nullptr` = std-only, so every existing caller and the
  in-process tests are unchanged). Core owns the **transitive load + dedup + cycle-detection + post-order
  merge** loop (generalizing today's `linkStdModules`); the resolver only answers "specifier (+ importer)
  → source text." The **CLI** implements it with `std::filesystem` (a bare dotted specifier `a.b.c` →
  `<root>/a/b/c.pg`; a relative `./x` → relative to the importer; `std.*` → the embedded registry first);
  the **tests** implement it with an in-memory map (no disk). Import cycles are a clear diagnostic
  (`a → b → a`), never a hang or miscompile. The source root comes from a `--root` flag now, from
  `pgconfig.json` later (ties into §4.4/P10).

- **Name collisions — refuse loudly, resolve with `as`.** Aligning with the §3 "never miscompile" law:
  **selective imports actually restrict visibility** (only the named symbols enter the importing file's
  scope — today's merge-everything is the bug to fix), and any collision is a **hard error naming both
  sources**, fixable only by aliasing one with `as`. Cases: two modules exporting one name → error;
  import vs local decl → error; import vs a **builtin** (`i32`, `Error`, `Iterable`) → error (builtins can't
  be shadowed); same module imported twice → already deduped. **Functions are special:** a same-name import
  with a *different signature* merges into the existing overload set (legal, per the native-overloading
  model); only a *same-signature* clash is a duplicate error. This also closes today's silent last-wins
  overwrite holes for top-level values, union constructors, and extensions.

- **Exports — all top-level declarations are module-public for now.** A `private` opt-out marker
  (SPEC §11's stated intent) is **deferred**; the loader already computes the per-module export set the
  collision/visibility rules need. Output stays single-file per target: multi-file `.pg` input merges into
  one root unit, so the emitted `.cs`/`.ts` is still one file (no conformance-harness redesign — just a
  directory-input + `--root` convention).

> **Status (P12 implemented, 2026-06-29).** Shipped: the import **syntax** (`import { a, b as c } from
> "spec"`, `* as ns`, bare); the **`ModuleResolver`** seam + transitive cross-`.pg` loading (dedup, cycle
> detection, dependencies-first merge; CLI filesystem resolver with `--root`, in-memory test resolver); and
> **collision detection** — every top-level name category now refuses a duplicate (types via `declareType`,
> functions by signature with overloads allowed, and the previously-silent value / union-case / extension
> holes now closed). **Deferred to a P12 phase-2** (needs a per-file import-scope table, which the current
> merge-into-one-unit model doesn't have): *selective-import visibility restriction* (today a selective
> import validates its names but still merges the whole module's public decls) and **`as` rebinding**
> (parsed, recorded, but not yet semantically applied). The safety property — never silently shadow/
> miscompile — holds today; the encapsulation niceties are the follow-up.

### 4.6 Std-as-real-modules + the `lib` prelude (design — 2026-06-29; ✅ shipped — 2026-06-30)
The module system exposed a contradiction: `print` and the `Math` namespace *were* **hardcoded builtins**, yet
the samples `import { print } from "std.io"` / `import { sqrt } from "std.math"` — which the P12 import
validation actively **rejected** (`std.io` had no `print`; `std.math` didn't exist). The samples only
"passed" because the fidelity gate `fmt`s them, never compiles. This is now resolved (P13): `print`/`Math` are
real std-module exports and the samples compile via `tests/samples/run-compile.ps1`. Decision (the language
designer's call):
**`print` and `Math` become real std-module exports — usable only via import — while `i32.parse` & friends
stay global primitive static methods.** Ergonomics are restored by a **`lib` prelude**, not by keeping
builtins. A two-track investigation found the design clean:

- **`std.math` — an `extern class Math`** of bound static members (`sqrt`/`ln`/`floor`/`ceil` → f64; generic
  `min<T>`/`max<T>`/`abs<T>` → `T`) plus bound `PI`/`E` constants, each an `actual(target) extern("…")` arm.
  The `Math.sqrt(x)` / `Math.PI` **call surface is unchanged** (still a static member of a type named `Math`).
  Type-preservation for `min`/`max`/`abs` falls out of ordinary generic-return typing (no special sema rule),
  and the TS-BigInt problem dissolves: an operator-ternary template (IIFE-form, to keep evaluate-once) works
  for both `number` and `bigint`. This **deletes all `Math` special-casing** across sema/lower/emit
  (`mathArity`, the `Math` namespace branch, `Math.PI/E`, the C# `mathRename`, the TS BigInt IIFE).

- **`print` — a generic `std.io` export** `expect fn print<T>(x: T)` + per-target `actual` `extern`
  (`Console.WriteLine($0)` / `console.log($0)`). Two behaviors carry over without keeping print a builtin:
  (1) the i64/u64 → `String(…)` wrap (so TS doesn't print a trailing `n`) becomes **i64/u64 TS `actual`
  overloads** — pure data, picked by the existing overload scoring; (2) the "this type isn't printable"
  diagnostic is kept as a **one-line sema guard on calls to `std.io.print`** — substituting for a `Printable`
  bound the language needn't grow yet. This deletes the `isPrint` flag and `printFn` machinery.

- **The `lib` prelude — auto-import without losing the "everything is a real import" model.** A workspace
  `lib: ["io", "math"]` auto-imports those std modules into every file, so `print(…)`/`Math.sqrt(…)` need no
  explicit import. Mechanism: a `LibConfig` (just specifiers) is passed to `compile()`; it **synthesizes one
  whole-module `ImportDecl` per entry**, tagged lib-origin, and the existing `linkModules` merges them.
  **A `lib` entry is a module specifier resolved through the same chain as `import`:** a *bare word* (`"io"`)
  is sugar for the std module `std.io`, while a *qualified* name (`"acme.physics"`) is used as-is and resolved
  via the resolver / (future) plugin registry — so a **third-party plugin auto-imports by its own namespace**,
  with no per-publisher special-casing. **Precedence (Rust-prelude / Python-builtins / TS-`lib` semantics):** a
  lib-imported decl is *ambient and lowest-priority* — it **loses silently** to any user declaration or
  explicit import of the same name (a pre-link `dropShadowedLibDecls` pass drops it before sema's collision
  tables build); explicit-vs-explicit collisions still hard-error (P12 unchanged), and lib-vs-lib collisions
  error (a prelude must be internally consistent). Config source: a `--lib` CLI flag now, `pgconfig.json`
  `"lib"` later (P10) — the Core stays IO-free (receives names, not files). **`lib` is the hard prerequisite**
  for the print/Math migration: without it, every `.pg` would need explicit `import … from "std.io"/"std.math"`.

Net effect: the std becomes honestly import-based (no magic builtins except the primitive `i32.parse`
static methods and the core `Error`/`Iterable` types), the broken samples become *compilable* (and join a
compile check, not just `fmt`), and hello-world stays `print(…)` via `lib: ["io"]`. See **PLAN P13**.

> **Known mechanism gap (note 2026-06-29, to close in P10).** The "Binding" plugin mechanism (`extern class`
> + `actual(target) extern("…")`) currently binds **member/property access** for *any* type, but **cannot
> yet bind a plugin class's own type-name → target type, nor its construction** — only the std-blessed
> `List`/`Iterable`/`Error` (and `List<T>()`) are hardcoded. So a user plugin class's type name and `new T(…)`
> emit literally; it's usable today only for methods on params/FFI results. Letting a binding declare its
> target type spelling (feeding the P9 backend type table) + a constructor template is **P10** work — the
> Binding mechanism's complete form. See PLAN P10 and `design/backend-spec.md` §4a.

### 4.7 Async/await (design — 2026-07-01; investigated by a 4-agent team; **implemented 2026-07-01**, PLAN §P15)

Single-threaded `async`/`await` is the **sanctioned concurrency model** (§3.B refuses threads/locks; async is
what's left). It is a **"colored function"** exactly like iterators (`yield`) — the iterator machinery is the
proven precedent this design follows, with two deliberate divergences noted below. **Status: ✅ implemented**
(built exactly to this design; conformance #38 `async_await.pg` agrees across C#/TS/Python; the description
below is retained as the as-built spec). *Historical note — before P15,* `async`/`await` were lexed (`KwAsync`/`KwAwait`) and reserved in the grammar, but: a top-level
`async fn` fails to parse, `async` on a *method* parses into `Member.modifiers` and is then **silently
dropped** (a latent no-op — emits a sync method), and `await` has no parser production (errors). Building it
closes that silent hole.

**The model.** `async fn foo(): T` is a function whose body may `await`; callers `await foo()` to get a `T`.
The author writes the **unwrapped** return type `T` (idiomatic + portable — see the decision below); each
backend synthesizes its own async wrapper. `await e` is a **prefix expression** at unary precedence (so
postfix `.`/`()`/`[]` binds tighter: `await a.b()` = `await (a.b())`), matching C#/TS.

**Return-type mapping — DECISION: backend-synthesized wrapper (not a user-written `Task<T>`).**
Two options were weighed (design-it-twice):
- *Option A (rejected):* mirror iterators — the user writes `Task<T>` and it's an `extern class` in `STD_CORE`
  (like `Iterable<T>`), so emitters stay mapping-free. Rejected because it's **un-idiomatic** (`return x` of
  type `T` under a `Task<T>` signature) and **non-portable** — the wrapper name differs per target (`Task` vs
  `Promise`), so there's no single spelling for `.pg` source.
- *Option B (chosen):* the author writes the **unwrapped `T`**; the `Task<T>`/`Promise<T>`/coroutine wrapper is
  **implied by the `async` coloring and synthesized at backend emission**. Sema's `return`-checking is
  unchanged (`currentReturn_` stays `T`); the IR stays a faithful high-level tree; the per-target wrappers are
  exactly the backend tier's job (§4.2 "specialize per target"); `.pg` source stays portable (one spelling).
  The `extern class` type registry is for *named types the user writes* — the async wrapper isn't one.

**Per-pass implementation map** (each dimension investigated; file refs current as of 2026-07-01):
- **Surface (lexer done).** AST: add `bool isAsync` to `FunctionDecl`; promote method-`async` from the
  `modifiers` string to a typed `isAsync`; add `ExprKind::Await` (operand in `lhs`, a *distinct* kind — do not
  reuse `Unary` with `text="await"`). Parser: consume a leading `async` in `parseFunction` + route top-level
  `async fn` from the unit dispatcher; set `Member.isAsync` and strip `"async"` from `modifiers`; parse
  `await` in `parseUnary` (not `parsePrimary`); add `KwAwait` to `beginsExpr` (fixes the `(i64)await x`
  cast-lookahead). Printer (`pg_printer.cpp`): emit `async ` on `printFunction`/`printMember` and `await ` in
  `expr()` — *strip-from-modifiers + re-emit-from-flag must ship together or the round-trip gate regresses.*
- **IR + sema + lowering.** IR: `bool isAsync` on `ir::Function` + `ir::Method`; an `Await` expr node (mirrors
  `ir::Unary`, one operand). Lowering: carry `f.isAsync = fn.isAsync` (async is **declared**, so — unlike
  iterators' `sawYield_` inference — no body scan is needed for correctness); lower `Await` straight through.
  Sema: track an `inAsync_` flag (alongside `currentReturn_`/`inActual_`); **validate `await` only inside an
  `async fn`** (new rigor — a stray `await` would be a native compile error on all targets = a miscompile the
  PRD forbids); **refuse `async` + `yield`** (async iterators / `IAsyncEnumerable` are a genuine third color,
  out of scope for v1); allow async-without-`await` (optional soft warning later). `await e` **typing (as
  built, 2026-07-01): a real `Awaitable<T>` unwrap.** A call to an async fn/method types as the compile-time-only
  `Awaitable<T>` (an `isAsync` bit on `FnSig`/`MemberInfo` wraps the inferred result); `await` unwraps
  `Awaitable<T>` → `T`, symmetric to `List/Iterable<T>` element unwrapping. This catches **forgot-to-await**
  (`return f()`/`let x: i32 = f()`/`print(f())` refuse — `checkConvert` + the print guard name the fix) and
  **awaited-a-non-async-value** (`await plain()` refuses), and mirrors C#/TS where `return f()` from an async fn
  requires `return await f()`. `Awaitable` is never author-written and never reaches emission (locals infer via
  `var`/`const`; backends synthesize the real `Task`/`Promise` from `isAsync`), so all conformance stays
  byte-identical. *(The v0 plan shipped identity typing as a stopgap; this replaced it the same day.)*
- **Backends (shared engine needs ZERO changes — async is signature-level, `await` is an expression).**
  - **C#**: signature gets an `async ` prefix and the return wraps to `Task<T>` (bare `Task` for `unit`);
    `await e` → `await <atom(e)>`; entry — keep the InvariantCulture pin first (load-bearing §3.D), then
    `main().GetAwaiter().GetResult();` (**never `async void Main`**).
  - **TypeScript**: extend the `function*`/`function` signature choice to `async function`; return wraps to
    `Promise<T>` (`Promise<void>` for `unit`); `await e`; entry keeps a **floating `main();`** — NOT top-level
    `await` (the conformance runner executes the `.ts` as a script; top-level await needs ESM and fails).
  - **Python**: `async def`; no return annotation; `await e`; entry becomes `asyncio.run(main())` with a
    prepended `import asyncio`, emitted only when needed via a `needsAsyncio_` flag mirroring `needsIdiv_`.
  - Use each backend's existing `atom()` helper for the awaited operand (parenthesization).
- **Capability gating (§3.E) — a ~4-line change following the `Iterators` precedent.** Add `Feature::Async`
  to the `Feature` enum + `kAllFeatures[]`; add a `featureName()` case (`"async"`; the switch has no default,
  so omitting it silently yields `"?"`); the three current backends already `return true` from `supports()`,
  so **nothing gates today** — a future PHP backend (no native `async`/`await`) returns false and the existing
  `checkCapabilities` refuses cleanly. The Collector (`capability.cpp`) marks async when it sees an `async`
  fn/method decl **or** an `Await` expr (mark on both). **Multi-target "only if ALL targets support it" needs
  no new code:** it already emerges from the CLI compiling each target in a loop (`ok &= emitOne(...)`), so a
  build fails if any configured target refuses. A real `pgconfig.json`-driven intersection pass is P10, out of
  scope here. Add a `StubBackend(Feature::Async)` test (mirrors the P5.E gate proof).

**Scope for v1:** `async fn`/`async` methods, `await` expressions, the three entry wrappers, `Feature::Async`
gating, the `async`+`yield` refusal, and the `Awaitable<T>` unwrap (an async call is `Awaitable<T>`; `await`
unwraps it, so forgot-await / awaited-non-async are caught). **Out of scope:** async iterators (`for await`),
a user-nameable awaitable type / `Task.WhenAll`-style combinators (bind via std when needed), cancellation
tokens, and the multi-target `pgconfig.json` intersection (P10).

### 4.8 Editor tooling & the language server (design — 2026-07-01; investigated by a 4-agent team)

**Two layers, deliberately separated.** (1) A declarative **TextMate grammar** (`editors/grammars/polyglot.tmLanguage.json`)
is the coloring floor — instant, offline, parse-error-tolerant, consumed *natively* by both VS Code and Visual
Studio, zero compiler dependency. (2) A **`polyglot lsp`** Language Server is the intelligence layer:
go-to-definition, hover, completion, document symbols, find-references, and **semantic tokens** (which *refine*
the grammar's coloring — the accurate way to distinguish a function call from a variable from a type, which regex
cannot). The grammar stays; semantic tokens layer on top. **Tier 1** (grammar + `fmt`/`check`-shell-out
formatting/diagnostics in a build-free VS Code extension) is ✅ shipped; **Tier 2** is the LSP, designed here.

**Core principle — write the intelligence once.** The server is a thin JSON-RPC loop over the existing C++
frontend; VS Code and Visual Studio are both standard LSP clients over it. No language analysis is reimplemented
per editor. Zero external deps holds (CLAUDE.md): we hand-write a small JSON reader to match the JSON we already
hand-emit.

**The four load-bearing changes** (each independently identified by the investigation):
1. **`SourcePos` gains `int fileId = 0`** (`diagnostics.hpp`). Defaulted → all ~90 use-sites copy by value and keep
   compiling unchanged; the *only* literal construction is in the lexer (`lexer.cpp:45`). Thread a `fileId` into
   `lex(source, diags, fileId=0)` and every token/AST/IR position inherits it by copy. This is the foundation for
   multi-file positions (cross-module go-to-def and honest multi-file diagnostics) and is near-zero-risk.
2. **The parser captures the *name-token* position.** Today decl `pos` points at the *keyword* (`fn`/`class`/…) and
   `Member`-expr `pos` at the *dot* — `expect(Identifier)` consumes and discards the name token. `Name` expressions,
   `Param`, and `Pattern` positions are already precise (so *references* are free). A mechanical per-site change —
   capture `expect(...).pos` into a `namePos`/`nameSpan` field — gives precise go-to-def *targets* and
   document-symbol selection ranges.
3. **`analyze(source, resolver, lib) → { CompilationUnit, diagnostics }`** — split the front half out of `compile()`
   (lex→parse→link→sema, stopping *before* lower/emit). `compile()` then calls `analyze` and lowers. Today the
   checked AST is discarded; the server needs it. This one refactor enables the whole server.
4. **The `SymbolIndex` (a sema by-product).** Resolution is already centralized and correct in
   `checkName`/`checkCall`/`checkMember`/`findMember`/`resolveOverload`. Add an optional `SemanticModel* out = nullptr`
   to `check()` (matching the `ModuleResolver*` optional-seam convention; `compile()` passes `nullptr` and pays
   nothing). At each resolution site record a **`SymbolRef` (occurrence span → resolved `SymbolDef` id)**; at each
   `declare`/`buildTables` site record a **`SymbolDef`** (kind, name, `nameSpan`, type, id, `external` flag). This
   is a **sema hook, not a standalone pass**: a separate walker would re-implement ~200 lines of scope/overload/
   inheritance logic and *still* couldn't do members (which need the receiver's type, only known inside sema).
   Shadowing is naturally correct because refs are recorded mid-walk when the scope stack is exactly right; overloads
   link to the chosen `FnSig`; unresolved refs get a sentinel id (highlight still works). **Hover needs no sema
   change** — a post-check read-only walk reads the `Expr.type` sema already annotates in place.

**Query API** (over the `SemanticModel`): `definitionAt(line,col)` (binary-search the ref spans → follow to the def),
`documentSymbols()` (the `!external` defs, grouped by kind), `hoverAt()` (post-check `Expr.type`), later
`referencesTo(id)` / `completionAt(...)`.

**The server** (lives in the CLI as a new `polyglot lsp` subcommand — one more `if` branch; the JSON reader lives in
Core as a testable IO-free primitive):
- **Transport:** `Content-Length`-framed JSON-RPC 2.0 over stdio. `_setmode(_O_BINARY)` on stdin/stdout is mandatory
  on Windows or `\r\n` translation corrupts the byte framing. Lifecycle `initialize`/`initialized`/`shutdown`/`exit`;
  advertise only implemented capabilities.
- **Position encoding:** LSP columns are UTF-16 by default but our columns are UTF-8 *bytes*. **Negotiate
  `positionEncoding: "utf-8"` at `initialize`** (VS Code supports it) so only a ±1 line/col shift remains — the
  UTF-16 conversion walk is deferred as a fallback for clients that refuse utf-8. The lexer never changes.
- **Doc sync:** **Full** (`change=1`), not incremental — files are small and the whole-program compile is fast; keep
  it boring. An open-document store (`uri → {text, version}`) plus a **buffer-aware `ModuleResolver`** that wraps
  `FileModuleResolver` and serves open-buffer text for open docs → unsaved-edit reparsing reuses the entire import/
  link machinery with **zero Core change**. Cache the `CompilationUnit` + `SymbolIndex` by `(uri, version)`.
- **Diagnostics need real ranges + severity.** `Diagnostic` today is a single point + a hardcoded "error"; extend it
  with an end position and a severity so `publishDiagnostics` is a genuine upgrade over the client's word-range guess.
- **Semantic tokens** are delta-encoded (`[Δline, Δstart, length, type, modifiers]` per token, document-ordered) with a
  declared legend; classify from the lexer first (keyword/string/number/operator), upgrade identifiers via the
  `SymbolIndex`.

**Capabilities by cost:** `publishDiagnostics` (~free — reuses `EmitResult.diagnostics`) and `documentSymbol` (cheap —
walk decl vectors) come first; `definition`/`hover`/`semanticTokens`/`completion` all hinge on the `SymbolIndex`.

**Cross-module** (the harder milestone): stamp each module's `fileId`/URI at its lex boundary in `loadImports`/
`linkCoreModule` (keyed on the `canon` path/name those already compute); embedded std modules get a virtual URI
(`polyglot:std.collections`) the server serves from the same `STD_MODULES` registry. The merge (`mergeDecls`) is
unchanged — origin rides inside each node's now-file-stamped `pos`. Until then, same-file queries mark merged
std/prelude/import decls `external` and honestly answer "definition not in this file."

**`pgconfig.json`** (your project manifest): minimal `{ root, lib }`, parsed in the **CLI/LSP layer** with the Core
JSON reader (core stays IO-free) into the same `FileModuleResolver` root + `LibConfig` the core already consumes —
so the LSP (and `check`/`build`) resolve modules with no per-keystroke flags. **✅ implemented 2026-07-01:** found by
walking up from the file; the LSP re-reads it each analysis and it wins over the client's `initializationOptions`;
explicit CLI flags still win over it. A strict **subset/precursor of P10's manifest** (P10 later *adds*
`environments`/`plugins`/lockfile + a `paths` search-map to the same file), independent of P11.

**Editor clients.** VS Code: add `vscode-languageclient` (the extension's first npm dep) with a light **esbuild** bundle
(source stays JS); reuse the existing `cliPath()` resolver as the server command (`args: ["lsp"]`); pass `{root, lib}`
as `initializationOptions`. The LSP's `publishDiagnostics`/`textDocument/formatting`/`semanticTokens` **supersede and
replace** the current `check`/`fmt` shell-out providers (gaining on-type + unsaved-buffer diagnostics with real
ranges); the grammar and `language-configuration.json` stay; the CLI `check`/`fmt` subcommands stay for headless/CI.
Visual Studio: a coloring-only VSIX (bundling the shared grammar) can land anytime; an `ILanguageClient` VSIX pointing
at `polyglot lsp` follows once the VS Code client proves the server.

**Scope for v1 (same-file):** definition/hover/document-symbols/semantic-tokens/diagnostics for symbols in the file
being edited (locals, params, functions, types, members). **Deferred:** cross-module go-to-def (needs the `fileId`
stamping + virtual URIs), find-references/rename, and member *completion* (needs receiver-type resolution).

### 4.9 Live generated-output preview (design — 2026-07-01; investigated by a 2-agent team)

**The want.** While editing a `.pg` file, *see the code it becomes* — the emitted C#/TS/Python — **live, as you
type**, and be able to **browse the std/plugin sources** the program pulls in (the `std.math`-style modules already
openable read-only by Ctrl+clicking a std symbol). Today `polyglot build` writes those files to disk; the preview
must produce them **on demand, in memory, never touching the filesystem**, so it's a pure view of the current
(possibly unsaved) buffer.

**Where it renders — a real editor beside the source, not the file tree.** The crux question ("explorer? a tree
that expands a `.pg` into its outputs?") resolves to: **the file tree is the wrong place to *render* code** — a
`TreeView` item is only label+icon, it can't show colored code. Render into a **read-only virtual document**
(`TextDocumentContentProvider`, a new `polyglot-gen:` scheme) opened **`ViewColumn.Beside`** the active `.pg`
editor — the *exact* mechanism the `polyglot:` std docs already use, and the same platform pattern VS Code itself
uses for "go to definition into generated/decompiled sources." This buys three things for free: **coloring** (set
the virtual doc's `languageId` to the built-in `csharp`/`typescript`/`python` — reuse their grammars, zero grammar
work; the URL carries a `.cs`/`.ts`/`.py` extension so detection is automatic, with an explicit
`setTextDocumentLanguage` fallback), **read-only correctness** (content-provider docs are non-editable/non-savable
by construction — no "don't save the generated file" guard), and **liveness** (a `vscode.EventEmitter<Uri>` backing
the provider's `onDidChange`; fire it on a debounced source edit → VS Code re-pulls and *diff-patches the visible
editor in place*, preserving scroll/cursor). This is strictly better than the Markdown-preview analogue, which must
use a Webview because Markdown renders to HTML — our output is *code*, so a real editor beats a webview on every
axis (find, select, copy, minimap, folding, go-to). **A Webview is explicitly rejected** for the rendering (it
throws away free grammar coloring and native editor affordances for a large re-implementation cost, against the
project's reuse-the-platform grain). The one legitimate future Webview use — gutter source-map lines linking a `.pg`
line to its output line — is a post-P17 stretch, not this feature.

**One preview, following focus, with a target switch** (the lean configuration): keep a single generated tab that
shows *the selected target* for *the focused `.pg`*, retargeted via `window.onDidChangeActiveTextEditor`; switch
target through a **StatusBarItem** (`Output: C#` → a `QuickPick` of the three) persisted in workspace state. Open
with `preview:true`+`preserveFocus:true` so it reuses one tab and never steals the cursor. An **optional `TreeView`**
(activity-bar "Polyglot Outputs": each open `.pg` → C#/TypeScript/Python leaves whose command opens the same virtual
doc) is *discovery only* — a thin navigator over the identical provider, added as polish, not required for v1.

**Server side — one new request, zero Core change.** `compile(source, target, resolver, lib)` is already a pure
in-memory function returning `EmitResult { ok, code, diagnostics }` — `code` is the emitted target text, and it
never writes to disk (the CLI's `build` verb is what writes; `compile()` does not). So a new custom LSP request
**`polyglot/emit`** (params `{ uri, target }` → `{ target, code, ok, diagnostics }`) lives entirely in the CLI's
`LspServer` beside `polyglot/moduleSource`: look up the open buffer in `text_`, build the **same** resolver/lib from
`pgconfig.json` that `analyzeDoc` already computes (factor that block into a shared `contextFor(uri)` helper so
preview output is *always consistent with the squiggles*), call `compile()`, serialize the result. It does **not**
reuse `analyze()`'s cached `SemanticModel`/`SourceMap` (a preview needs neither) — `analyze()` deliberately stops
before lower/emit, so there is no mid-pipeline state to reuse; the preview re-runs the full pipeline, which is
negligible for an editor-sized file and only fires on the debounced request. Single-threaded stdio ⇒ no races; the
only I/O is the resolver reading imported `.pg` files, exactly as diagnostics already do.

**Contract-honest error behavior** (this is a §3.B-adjacent concern — *never present a miscompile as valid*).
`compile()` stops at the first failing pass and returns `{ ok:false, code:"", diagnostics:[…] }` — never partial or
garbage output. The **server stays dumb** (returns that verbatim); the **client owns the policy**: keep the
**last-good** output visible with a one-line stale banner (`// Polyglot: N errors — showing last successful output`)
rather than blanking the pane on every half-typed keystroke, and never render half-emitted code as if valid. The
real errors already surface as squiggles in the `.pg` editor. Note **per-target honesty**: `compile()` runs
`checkCapabilities` for the *previewed* target, so a Python preview may legitimately fail where C#/TS succeed
(Python is still the walking-skeleton subset) — surface that in the banner, don't paper over it.

**Liveness model — request/response, client-debounced; no server push.** The client already knows when the buffer
changed (it sent `didChange`) and which target the pane shows, so it fires a debounced (~150–250 ms) `polyglot/emit`
and re-requests on target switch. A server→`client` push notification would be the first of its kind, would need
the server to track "which target is each pane showing," and buys nothing — rejected. **One target per request**
(not a bundled three-target map): the pane shows one target; bundling triples payload and emits two targets nobody's
looking at. Guard stale responses with a per-URI request sequence so a slow emit can't overwrite a newer one; emit
from the **in-memory buffer** (unsaved text), not disk; on `didClose` drop the doc's last-good cache.
**Multi-root correctness:** resolve the emitted file's owning folder via `getWorkspaceFolder(sourceUri)` and pass
*that* folder's `{root,lib}` (its `pgconfig.json`), not `workspaceFolders[0]`, so imports in a second root resolve
right.

**Touch list.** Server: `main.cpp` only — a `contextFor(uri)` helper (extracted from `analyzeDoc`), a
`targetFromString` helper (also tidies `runBuild`), a `diagnosticsToJson` helper (shared with `check --json`), a
`generatedSource`/`emit` handler, and one dispatch arm. Client: `extension.js` (a `polyglot-gen:` provider +
emitter + debounce + follow-active-editor + status-bar switcher; optional tree) and `package.json` (a
`polyglot.showOutput` command, an editor-title menu icon, a `polyglot.preview.defaultTarget` setting, and — if the
tree ships — `viewsContainers`/`views`). No Core change. Full slice log: PLAN §P17.

---

## 5. Testing strategy
- **Unit tests** per pass (lexer, parser, type-checker, each backend) — the `MintPlayer.Polyglot.Tests`
  project (a tiny zero-dependency assert harness to start).
- **Golden-output tests:** source → emitted C#/TS compared against checked-in baselines (the TypeScript-
  team model). Catches emitter regressions.
- **Differential conformance (the crown jewel):** for a program emitted to *both* targets, compile & run
  both and assert identical results on a shared input suite. This is exactly the FruitCake parity test —
  Polyglot turns "keep two ports in sync" into a CI gate. Per §3.D, numeric programs gate on tolerance +
  behavioural equality, not bit-equality.
- **Refusal tests:** every 🔴 feature must produce a clear, actionable diagnostic — not a miscompile.

---

## 6. Milestone roadmap
Full detail in [PLAN.md](PLAN.md). Summary:
- **P0 — Solution skeleton.** ✅ `.sln` + Core lib + CLI (`--version`/`--help`) + test harness.
- **P1 — Language design v0.1.** ✅ Locked (`docs/lang/`): grammar + spec doc + sample programs; the
  deliberately-small surface checked against the §3 contract.
- **P2 — Walking skeleton (MVP).** ✅ Thinnest end-to-end slice: a minimal subset through lexer→parser→
  typer→IR→**both** hand-written backends; `polyglot build` emits running C# + TS with identical stdout
  (the **differential conformance test** stands up here, not at P5). Proved the "one IR serves both
  targets" bet.
- **P3 — Full front-end.** ✅ Full P1 grammar parses (incl. real string interpolation); `.pg` pretty-printer
  (`polyglot fmt`) round-trips all 10 samples idempotently (fidelity gate in `/build-and-test`).
- **P4 — Full semantics + IR.** ✅ Resolution + nominal type system + match exhaustiveness; a separate
  typed IR (`ir.hpp`) produced by a lowering pass; backends emit from the IR.
- **P5 — Backends to full §3.A.** Widen both C#/TS pretty-printers to the entire surface; golden baselines
  both targets; the differential suite grows.
- **P6 — Faithfulness pass.** int32 masking, the §3.C relaxations (documented), the §3.B refusals (with
  diagnostics).
- **P7 — Std core + expect/actual.** Minimal portable std (math, basic collections) + the target-gated
  binding mechanism + an FFI hatch.
- **P8 — Dogfood FruitCake physics.** Express the circle-physics solver in `.pg`; generate `.cs` + `.ts`;
  wire the differential conformance test against the existing MintPlayer.AI twins. *North star.*
- **P9 — Declarative backend engine + DSL — ✅ done (extracted, validated across three backends).** Extracted
  the shared engine (`EmitterBase` owns the statement walk + buffer/indent + render primitives, reading data
  via one `spec()` accessor) and the **`BackendSpec` declarative DSL — all per-target data**: scalar/suffix/
  operator/bracket tables + block style + statement terminator + throw keyword + bool/null literal spellings
  (string escaping is the shared `renderString`). Every backend — including the non-sibling Python — is a
  `{Spec + Hooks}` instance; the hook surface (`emitExpr`/`emitStmtTarget`/`localDecl`/`yieldStmt`/
  `rethrowStmt` + declaration emitters) is the residual imperative tier. All byte-for-byte no-op slices (gate
  held continuously). Extraction proved — and the third backend confirmed — declaration *shapes* and the
  expression walk are irreducibly per-target (they can't flatten to data without an embedded DSL the zero-dep
  core forbids — the design's "full-power local tier"). See `design/backend-spec.md` §3.
- **P9-V — Third backend (Python): engine-validation spike — ✅ done (36/36 conformance programs).**
  A native Python backend (a non-sibling, colon+indent target) brought up to validate that the P9 engine
  generalizes — and to be the artifact the declarative DSL is later extracted from. It now covers the **full
  §3.A surface**: all 36 conformance programs (incl. the FruitCake north star) transpile to Python with
  output byte-identical to the C# oracle. Findings: the engine was brace-family-specific, so Python forced a
  real generalization (3-way `BlockStyle` + statement terminator + a `throwKeyword` hook + a block-style-
  agnostic `Use`, each a verified C#/TS no-op); after that the shared statement layer served Python unchanged,
  and declarations stayed per-target as predicted. The spike also surfaced **three latent bugs fixed at the
  root** — chiefly that `break`/`continue` were silently dropped in lowering for *all* targets (a §3.B
  miscompile the C#/TS diff gate couldn't catch). The declarative DSL can now be extracted from **three**
  backends instead of guessed. Details + full slice log in `PLAN.md` §P9-V.
- **P10 — Plugin distribution + ecosystem.** `pgconfig.json` + download/cache/verify/version; availability by
  target+environment; build-dependency threading; the local full-power tier; proof = a **downloaded
  declarative Python backend** + a binding plugin, with **no core change**. The endpoint of §4.4 — see
  [`../design/plugins-and-targets.md`](../design/plugins-and-targets.md).
- **P11 — Build integration (the `.pg`-aware NuGet / npm on-ramp).** A NuGet package that auto-transpiles
  `.pg` → `.cs` **before `dotnet build`** with no manual step — modeled on `Grpc.Tools` (native CLI shipped
  per-RID in the package; `build/` props+targets hooking `BeforeTargets="CoreCompile"`, generating into
  `obj/` and joining `@(Compile)`; incremental; runs in design-time builds). **Non-transitive**
  (`DevelopmentDependency`/`PrivateAssets`). The payoff of the §4.3 zero-runtime-dep native CLI: the
  consuming dev needs no extra SDK/runtime. Depends only on a stable CLI, so it can ship independently of
  P9/P10. A sibling npm/build-script story does the same for TS.
- **P13 — Std as real modules + the `lib` prelude.** ✅ `print`/`Math` are real `std.io`/`std.math` exports
  (not builtins); a `lib` prelude auto-imports them ambiently. Also delivered the §4.6 type-mapping/
  construction binding (P10 precursor) — `extern class`es declare their per-target type + ctor, and
  `List`/`Error`/`Iterable` are fully dogfooded onto it (zero hardcoded type mappings in the emitters). See
  §4.6 and PLAN P13.
- **P14 — Emitted-output correctness + `Option<T>`.** ✅ A compile-run gate (`run-emit.ps1` builds the C#,
  runs the TS) caught a cluster of output-only miscompiles the transpile gate missed — all fixed; **all 10
  samples now compile+run**. Added a faithful **`Option<T>`** generic union for nullable generics (§3.C),
  generic unions, interfaces/indexers/record-implements emission, `std.strings` (bound extension methods),
  char literals, and faithful bool/float printing. See PLAN P14.
- **P15 — Single-threaded async/await.** ✅ Done (2026-07-01; §4.7, from a 4-agent investigation).
  A "colored function" like iterators: `isAsync` on `ir::Function`/`Method` + an `Await` expr node; the author
  writes the unwrapped `T` and each backend synthesizes its own wrapper (C# `async Task<T>`, TS `async …
  Promise<T>`, Python `async def` + `asyncio.run` entry); `await` parses at unary precedence; `Feature::Async`
  gates it (all three current backends support it — the gate bites only for a future target like PHP). Sema
  validates `await` only inside `async fn` and refuses `async`+`yield` (async iterators out of scope). Closed
  the prior silent hole where `async` on a method parsed but was dropped. Full design + per-pass map: §4.7.
- **P16 — Editor tooling & the language server.** 🚧 In progress (2026-07-01; §4.8, from a 4-agent
  investigation). **Tier 1 ✅** (shared TextMate grammar + a build-free VS Code extension: highlighting +
  `fmt` formatting + `check --json` diagnostics; repo-root F5 launch). **Tier 2** = a zero-dep `polyglot lsp`
  server over the frontend-as-a-library — go-to-def / hover / completion / document-symbols / semantic-tokens —
  with VS Code and Visual Studio as thin LSP clients. Four load-bearing changes: `SourcePos.fileId`, parser
  name-token positions, an `analyze()` seam (checked AST without emit), and a sema-hook `SymbolIndex`
  (occurrence → definition). Same-file first; cross-module go-to-def (via `fileId` stamping + `polyglot:` virtual
  URIs for embedded std) and a minimal `pgconfig.json` follow. Full design + per-pass map: §4.8; slice plan: PLAN §P16.
- **P17 — Live generated-output preview.** ✅ Done (2026-07-01; §4.9, from a 2-agent investigation).
  See the code a `.pg` becomes — emitted C#/TS/Python — **live as you type**, rendered into a read-only
  virtual document (`polyglot-gen:` scheme) opened beside the source and colored for free by the built-in
  target-language grammars. One new in-memory LSP request (`polyglot/emit` → `compile()`, no disk I/O, no Core
  change), client-debounced request/response, last-good-with-stale-banner error UX (never a miscompile shown as
  valid); a status-bar target switcher + an Explorer "Polyglot Outputs" tree for discovery. A follow of `P16`'s
  virtual-doc + custom-request plumbing. Full design + slice plan: §4.9 / PLAN §P17.
- **Stretch:** further targets as downloadable backends, source maps, a plugin registry + signing/trust
  infrastructure. (See PLAN Stretch.)

---

## 7. Honest ceiling & risks
| Risk | Mitigation |
|---|---|
| **Scope creep kills it** (the universal cause of death) | The §3 support/refuse contract is the law; new features must justify themselves against it. Faithful-by-default + *published* relaxations, never silent ones. |
| Hand-written emitters are tedious (C++ choice) | Accepted, normal for native compilers; the §4.3 alternative (C#/Roslyn) is on record if it bites. |
| Stdlib/platform surface is unbounded | Bind-what's-used + target-gated + FFI hatch; explicitly *no* broad-coverage promise. |
| Cross-target float determinism expectations | §3.D honesty clause: only `+−×÷√` reproducible; fixed-point std type for code that needs identity; tests gate on tolerance. |
| Solo bus-factor / long timeline | This is a **craft/long-haul project**, not on any delivery critical path. Decoupled from MintPlayer.AI's M30 (which ships via hand-ports + conformance test regardless). Polyglot *earns* the right to generate the physics only once mature. |

## 8. Relationship to MintPlayer.AI (M30)
MintPlayer.AI's FruitCake client-side move ships **now** with hand-ported physics locked by a conformance
test (M30). Polyglot is the **long game**: if/when it matures through P8, it generates that physics from
one source and the hand-ports retire — while the *same* conformance test remains, now guarding the
generator. The dream never blocks the delivery; the delivery gives the dream its first real target.

## 9. Sources (the investigation this PRD distills)
- [Semantics of Scala.js](https://www.scala-js.org/doc/semantics.html) · [Scala.js strict floats 1.9.0](https://www.scala-js.org/news/2022/02/14/announcing-scalajs-1.9.0/)
- [J2CL limitations](https://github.com/google/j2cl/blob/master/docs/limitations.md) · [J2CL repo](https://github.com/google/j2cl)
- [Haxe — Overflow](https://haxe.org/manual/types-overflow.html) · [Abstracts](https://haxe.org/manual/types-abstract.html) · [Externs](https://haxe.org/manual/lf-externs.html) · [Compiler targets](https://haxe.org/documentation/introduction/compiler-targets.html)
- [Fable](https://github.com/fable-compiler/Fable) · [Fable .NET/F# compatibility](https://fable.io/docs/javascript/compatibility.html) · [ts2fable](https://github.com/fable-compiler/ts2fable)
- [Kotlin Multiplatform — expect/actual](https://kotlinlang.org/docs/multiplatform/multiplatform-expect-actual.html) · [project structure](https://kotlinlang.org/docs/multiplatform/multiplatform-discover-project.html)
- [TypeScript Design Goals](https://github.com/microsoft/TypeScript/wiki/TypeScript-Design-Goals) · [DOM-lib-generator](https://github.com/microsoft/TypeScript-DOM-lib-generator) · [spec-conformance testing](https://github.com/microsoft/TypeScript/wiki/Spec-conformance-testing)
- [InfoQ: JSIL challenges](https://www.infoq.com/articles/jsil/) · [GWT JRE compatibility](https://www.gwtproject.org/doc/latest/DevGuideCodingBasicsCompatibility.html) · [why GWT→J2CL](https://blog.kie.org/2022/04/rise-of-j2cl-java-web-development-after-gwt.html)
- [Random ASCII — Floating-Point Determinism](https://randomascii.wordpress.com/2013/07/16/floating-point-determinism/) · [Gaffer on Games — FP Determinism](https://gafferongames.com/post/floating_point_determinism/)
- [MDN Math.imul](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Math/imul) · [MDN Math.fround](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Math/fround)
