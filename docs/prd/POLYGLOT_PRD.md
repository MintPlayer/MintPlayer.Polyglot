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
- **Transcendental `std.math` tier (✅ 0.3.0, issue #11):** `sin cos tan asin acos atan atan2 sinh cosh tanh
  exp log log2 log10 pow trunc` ship as an explicitly documented **best-effort tier** — available on every
  target (plain 1:1 bindings to each runtime's `Math`/`math`), but results may differ by ≤1 ULP across the
  .NET JIT and a JS/Python/PHP runtime. Authors opt in knowingly; code needing cross-target identity uses
  `+ − × ÷ √` (or the planned fixed-point type), never these. Their conformance program gates on **quantized
  equality** (scale + truncate), not bit-equality. `cbrt`/`sign` are intentionally omitted — not uniformly a
  clean 1:1 binding (cbrt absent on PHP / Python<3.11; C# `Math.Sign` throws on NaN where JS returns NaN).

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

### F. Input surface — one authoring syntax, deliberately distinct (added 2026-07-02, §4.12)
Polyglot has exactly **one** authoring syntax (`.pg`), intentionally *not* a clone of any target
language, so the surface never implies target-specific semantics Polyglot refuses (§3.B) — the distinct
syntax is itself a scope-line defense. **Supported:** alternative *reading* aids that don't create a
second maintained front-end — the live generated-output preview (§4.9), "Polyglot for C#/TS developers"
Rosetta docs, and (demand-gated) a **one-way, best-effort `polyglot convert`** import aid (C# *subset* →
`.pg`; fails loudly on unsupported constructs, never round-trips, migration-only). **Refused
permanently:** a **TypeScript authoring skin** — TS surface syntax *inverts* `.pg` semantics exactly
where the faithfulness machinery lives (one `number` can't express integer widths; `as` erases where
`(T)x` truncates; `let` mutability is inverted; `for..in` iterates keys; `A | B` is structural/open).
**Gated, not scheduled:** a **C#-flavored authoring skin** (`.pgcs`) — admissible only after P19 ships,
the `.pg` grammar freezes, and `convert` demonstrates real sustained demand; always a compiled-in
front-end over the shared AST (never a data plugin — §4.12), always a distinct extension + dialect
framing, still exposing exactly the §3.A surface. Design + evidence: `docs/design/frontend-skins.md`.

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
Visual Studio (P16d, designed 2026-07-01): a single VSIX at `editors/vs/` — a MEF `[Export(typeof(ILanguageClient))]
[ContentType("polyglot")]` client whose `ActivateAsync` launches `polyglot lsp` over stdio (`Connection(stdout,
stdin)`), a `polyglot` content type (`BaseDefinition = code.remote`) + `.pg` file association, and the **same shared
TextMate grammar** bundled for the offline coloring floor (semantic tokens refine on top). The full standard LSP set
flows through `ILanguageClient` with no VS-specific code; VS sends utf-16 positions, which the server already handles.
v1 ships the standard set + coloring; the VS-Code-specific extras (std virtual-doc click-through, generated-output
preview) are deferred behind `ILanguageClientCustomMessage2` follow-ups. The VSSDK is installed on the VS 18 build
host and the VSIX builds headlessly with the repo's VS 18 MSBuild; testing the running extension needs an interactive
`devenv /rootsuffix Exp`. Legacy VSSDK `.csproj`, `net472`, `InstallationTarget [17.0,)`. Slice plan: PLAN §P16d.

**Scope for v1 (same-file):** definition/hover/document-symbols/semantic-tokens/diagnostics for symbols in the file
being edited (locals, params, functions, types, members). *(All since delivered beyond v1: cross-module go-to-def
via `fileId` stamping + virtual URIs, find-references/rename, live cross-file edits, semantic tokens in std virtual
docs, and member `completion` via a repaired-buffer receiver-type resolution — PLAN §P16c + tail. Remaining minor
deferrals: in-scope-only local filtering + a non-ASCII UTF-16 position walk.)*

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

**All targets, following focus** (as built — the design initially sketched one switchable tab, but a `.pg` genuinely
emits C#/TS/Python, so "Show Generated Output" opens *all three* at once, each its own tab beside the source, and
that's the least-surprising behavior): the tabs are `preview:false`+`preserveFocus:true` (permanent, cursor stays in
the `.pg`) and all **follow the focused `.pg`** via `window.onDidChangeActiveTextEditor`. To follow without
per-source tab churn, each gen URI is keyed by **target only** and renders whatever the current `previewSourceUri`
points at — so following just re-fires the open tabs in place. A single target on demand is the **Explorer
`TreeView` "Polyglot Outputs"**'s job (each open `.pg` → C#/TypeScript/Python leaves whose command opens that one
target) — a thin navigator over the identical provider. (The first cut added a status-bar target switcher for a
single-tab model; it was dropped on user feedback in favor of all-targets + the tree.)

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

**Deferred — the target list must not stay hardcoded (ties into P10).** The client currently hardcodes the three
targets and their `{name, ext, langId, comment}` (`extension.js` `TARGETS`). The **real** target set is the CLI's
backend registry — and, once P10 lands, the *downloadable* backends too. The principled fix is a server-advertised
target list (a `polyglot/targets` request, or a field in the `initialize` result) returning each registered
backend's descriptor `{ id, displayName, fileExtension }`; the client derives `TARGETS` from it (langId falls out of
the extension via VS Code's own detection; the comment prefix defaults to `//` with a per-target override), so a
plugin backend appears in "Show Generated Output" and the Outputs tree **with no extension change**. This is small
but blocked on the backend registry being queryable across the LSP seam; it belongs with P10's downloadable-backend
work. Until then the hardcoded list is a knowing stopgap, flagged at the code site.

### 4.10 Data-driven backends — languages as pure-JSON plugins (design — 2026-07-01; investigated by a 4-agent team)

**The goal.** A target language should be an **installable, pure-data JSON plugin** — `polyglot install @polyglot/kotlin` — not hard-coded C++. Adding a language must need **no Core change and no executable code** (RCE-safe by construction). Today C#/TS/Python are compiled-in `Backend` classes and `Target` is a closed enum; the closed-target surface is ~a dozen sites (the enum + dispatch, the `kRegistry[]`, CLI `--target`/extensions, `lower.cpp`'s 3-way template/type/ctor `if`s, `ir::Bound`/`ExternType`'s fixed `cs/ts/py` fields, per-backend `supports()`), plus the **~90 `actual(target)` arms in the embedded std** (`compiler.cpp`) — the hidden per-`(module×member×target)` scaling cost. Full detail + the design notes: `docs/design/backend-spec.md` (DSL + interpreter) and `docs/design/plugins-and-targets.md` §6.1/§4 (distribution). This is the endpoint of §4.4 and the completion of PLAN P9.

**The reframe — the "irreducible 30%" was overstated.** P9 split a backend into `{Spec (≈70% tabular data) + Hooks (≈30% imperative C++)}` and concluded the Hooks "can't be flattened to data without an embedded DSL the zero-dep core forbids." Re-reading the actual hooks: almost all of that 30% is a **fixed decision tree over IR-node fields that renders strings** — precisely what a *bounded template interpreter* evaluates. P9's real, correct point was narrower: **don't guess the DSL format from ≤3 backends, and don't ship a scripting runtime.** With three working backends to extract from, the format can now be *derived*, not guessed. Verdict: **≈85%** of the three backends flattens to pure data with a small interpreter; **≈95%+** with a fixed, audited set of extra primitives; the remaining <5% is genuine *target* limitations (e.g. Python's expression-only `lambda`) that the §3.E capability gate already refuses correctly — never a miscompile.

**The DSL (Design A — chosen over a "strategy vocabulary").** One JSON `Rule` per `ir` node kind (`expr`/`stmt`/`decl`), evaluated by a fixed, **non-Turing-complete** interpreter with a ~10-primitive vocabulary: `tmpl` (concat), `get` (read an IR field), `emit`/`emitChild` (recurse a child — `emitChild` applies **precedence-driven parenthesization the interpreter computes from a declared `precedence` table**, so plugins never author paren logic), `type` (render a `TypeRef`), `map`/`fold`/`interleave` (bounded child-list iteration — count = list length, plugin can't loop), `case`+a weak `Test` language (`eq`/`in`/`has`/`isKind`/`typeIs`/`any`/`all` + boolean combinators — no arithmetic), `call` (named helper sub-rules, depth-capped), `let`/`fresh` (single-eval + fresh temp names), `require` (dedup'd import/preamble buckets placed by the `program` rule — the one controlled side effect), and a small **fixed builtin set** (`ident`/keyword-escape, `pascalCase`/`camelCase`, `escapeString`/`escapeInterp`, `opSpelling`, `wrap` for numeric faithfulness). Adding a builtin is a *Core* change (trusted, auditable); a plugin can only *select among* and *substitute into* its own templates. Design B (a closed vocabulary of `MatchOp{style:…}`-style strategies) was rejected: it puts the Core in the business of *predicting every future target's idioms* — the exact speculation to avoid — whereas Design A's template neutrality means a novel target stresses the *data*, not the Core. Worked JSON exists for every hard case: precedence/parenthesization, `Match` (C# switch-expr / TS if-chain / Python ternary-fold, all as `map`/`fold` + per-arm pattern `case`), `Try` (native vs `instanceof`-dispatch), records/unions/enums, iterators, async/await, interpolation, closures, and the numeric-wrap matrices (which flatten to `intWrap` tables + a `wrap` builtin + `require`d prelude fragments).

**The honest ceiling — the tension.** Because a downloaded plugin gets **data only, never code**, there is **no per-plugin escape hatch**. So each residual decision is binary: either *(a) grow a fixed, bounded, read-only Core primitive/predicate* (viable for the handful that need it — a `typeIsRecord` module query for TS structural `.equals()`, `any`/`all` quantifiers for C#'s exhaustiveness default, folding `hasCatchAll`-style facts into the shared **lowering** pass as precomputed IR bits so the interpreter context stays node-local), or *(b) the feature is unsupportable for external plugins* and the capability gate refuses it (correct for Python block-lambdas). What you **cannot** do is let a plugin ship a code snippet to cover its gap — that reintroduces the RCE surface the whole design exists to avoid. Completeness therefore comes from **Core growth, bounded by** "can this decision be expressed as *selection-among + substitution-into* plugin templates using only fixed primitives?" — everything in the three current backends answers yes except the <5%.

**The interpreter + `Target` becomes dynamic.** `EmitterBase` (which already owns the target-neutral statement walk + buffer/indent/block machinery) generalizes into the rule interpreter; the `Target` enum is deleted from the public surface and a target becomes a **string name + a resolved `BackendHandle`** (a parsed, schema-validated, immutable spec the host hands Core as bytes — Core stays IO-free). `compile()` takes a `BackendHandle` instead of `Target`; **`analyze()` is unchanged** (front-end-only — the LSP needs no backend for diagnostics/hover/def). Specs are **validated exhaustively at load** (every claimed feature has a rule; unknown primitives/slots rejected) so a broken plugin *fails fast at load*, never miscompiles; a "no rule for this node" is a hard error (the opposite of the old silent `default:` that hid the P9-V `break`/`continue` drop). ~~The built-in three ship as **in-box JSON specs embedded in the binary**~~ — **superseded 2026-07-02 (§4.11): the CLI embeds *no* target specs; C#/TS/Python are ordinary plugin packages resolved via `pgconfig.json` like any other target.**

**Distribution (extends §6.1, RCE-safe).** A language plugin is an **npm package whose payload is data** — a `polyglot-plugin.json` manifest + `backend/spec.json` (the DSL) + capability set + `externTypes` (type mappings) + `builddeps.json` (the SDK/PackageReference/npm/pip deps to thread into the emitted project) + any `std/*.pg` modules it provides. The zero-dep CLI speaks the **npm registry HTTP API** directly (no Node), fetches the tarball, **verifies `dist.integrity` (SHA-512), extracts zip-slip-safe, and never runs lifecycle scripts** — `polyglot install` is the single trusted writer of the global registry (§6.1). `--target <name>` resolves name→registry→cache→spec bytes→`BackendHandle`; **`pgconfig.json` gains a `dependencies` map** (the umbrella set of plugin packages+versions this codebase needs — *language* plugins you compile to and *library* plugins you import), a `targets` selection (which languages to emit), and keeps `lib` (ambient no-import modules — a subset of `dependencies`), all integrity-pinned in `pgconfig.lock.json` (full schema: design note §6.3). The editor's `polyglot/targets` reads the same registry so a new language appears in "Show Generated Output" + the VS Code/VS extensions **with no client change**. **Honest residual** (unchanged from §6.1): data-only kills *transpile-time* RCE, not *runtime* trust — a malicious declarative binding can still emit hostile *target* code (`File.Delete(…)`), which runs later with the dev's privileges. Plugin output needs the same trust as any dependency; signing/trust is the deferred mitigation.

**Migration discipline (keep the gate byte-identical).** This is P9's "extract the DSL from the *working* native backends, dual-run byte-for-byte, then delete the C++" taken to completion: (1) build the interpreter alongside the C++ backends + a dual-run harness; (2) re-express C# (the differential *oracle*) then TS then Python as JSON specs, one node-family at a time, each proven a no-op across all 38+37 conformance programs before it counts as migrated; (3) push the two module-semantic queries into lowering; (4) flip the default to interpreted (no compat fallback — superseded 2026-07-02, §4.11: the C++ deletes in the same slice its byte gate passes); (5) delete `emit_csharp/typescript/python.cpp` + `kRegistry[]`. Only after all three are pure JSON is "a downloaded backend is JSON in an npm package" *earned* rather than guessed. Slice plan: PLAN §P18.

### 4.11 100% JSON plugins — the complete artifact (design — 2026-07-02; investigated by a 4-agent team)

**The question P18 left open, answered.** P18 proved the *expression walks* flatten to JSON rules over one fixed interpreter (built: all three backends, byte-identical). This section closes the rest: **the entire plugin — declarations, type rendering, the hard expression residue, the std arms, capabilities — is data**, so a new language is an npm-installable JSON artifact with zero C++ in the plugin and, in the steady state, **zero Core changes**. Full detail: `docs/design/json-plugins.md`; slice plan: PLAN §P19 (which supersedes P18's remaining tail — P18 slices 1–15 are ✅ built).

**Findings (per front).** *Declarations* flatten **further than expressions did** (~90% with the base decl primitives, ~98% with the fixed Core additions, <2% genuine target limits → §3.E): every emitter is "head template + map over members + per-member templates"; cross-target *shape* divergence (C# positional record vs TS class+ctor+equals vs Python `__init__`+`__eq__`) is one rule table per target — the model the expression layer already uses. Synthesized members (TS `equals`, Python `__eq__`, C#'s exhaustive-match default) are plugin templates over field loops. *Hard expressions + types*: ~95% flattens; `Match` — the hardest — works in Design A via `fold` (extracted from Python's working `matchChain`), **re-vindicating the Design-B rejection with evidence**; type rendering becomes per-target **type-rule tables** evaluated by a `type` primitive. *Builtins*: today's ~15 per-target builtins + 3 paren policies collapse into a **~10-entry generic catalog + spec parameters** (keyword sets, escape sets, an `intRepr` wrap-strategy enum `arbitrary|nativeFixed|f64+bigint`, a `(fromClass,toClass)→Rule` conversion matrix, `wrapAtom` kind-sets as data) — so **"every new language needs a Core PR" is false**: only the *pioneer* of a new representational class pays a one-time additive Core bump (PHP's `$`-escape set; Rust's `wrapping_add`), governed by **`requiresCore`** semver the loader enforces. *Artifact*: a multi-file npm payload (`polyglot-plugin.json` manifest + spec/expr/stmt/decl/precedence rules + externTypes + **tri-state capabilities `native|emulated|false`** + std **overlays** + preludes + builddeps), validated exhaustively at load.

**The primitive set is now closed.** §4.10's reserved primitives are confirmed and shaped by extraction: `interleave` (Interp), `fold` (Match), `emitBlock` (statement-bodied lambdas), `type` (TypeRef recursion), `fresh`/`let` (single-eval temps), `require` (dedup'd preamble buckets placed by the `program` rule) — plus the **emitter-rule flavor** for declarations (`line`, `block`, `mapDecl`, per-target decl tables + a `program` scaffold rule, reusing `EmitterBase`'s existing block machinery) and `Test` additions (`any`/`all`, `typeIs`/`isKind`). **Lowering absorbs all module facts and temp allocation** (record/interface/indexer bits, `With`→ordered ctorArgs + tempName, `This`→`Var("lhs")`, match `hasCatchAll`/binder accessors, walrus temps) so the interpreter context stays node-local and stateless — with the discipline that **each lowering absorption gets its own byte-identity gate** (a lowering bug is invisible to a diff of two backends consuming the same wrong fact).

**Std de-hardcoding.** Each std module splits into a **target-neutral skeleton** (signatures, stays embedded) + per-plugin **overlay files** (`{module → member → template}`, the unchanged `$this`/`$0`/`$T` grammar). Link-time merge selects the active target's arm, collapsing the last hardcoded-target IR surface (`ir::Bound{cs/ts/py}` → one `template`; `ExternType`'s six fields → two). A *used* member with no arm refuses at the call site (the existing mechanism); the first-party three become in-box overlays extracted from the ~97 `actual(...)` arms under a byte-identity gate.

**Loader + trust.** `loadBackend(bytes) → BackendHandle` (same handle as `findTarget`; `compile()` unchanged; built-ins constructed through the same loader over embedded JSON). Load-time obligations include the **anti-silent-drop coverage rule**: *every IR node kind has a rule OR its capability is declared `false`* — "no rule" is never "emit nothing" (the P9-V lesson made structural). Two version axes: `schema` (manifest layout) + `requiresCore` (interpreter contract). The LSP gains `polyglot/targets` (closes the editor's hardcoded target list). Trust stance unchanged: data-only kills transpile-time RCE; std overlays/preludes are raw target-code *templates* that land in output — add an install-time **warn-only** sink lint as a review prompt; signing stays the deferred real mitigation.

**Immediate fixes surfaced (independent of P19):** two latent §3.B silent-broken-output bugs in the shipping Python backend — statement-bodied lambda emits the sentinel `__py_unsupported_block_lambda__` and `With` falls to `__py_unsupported_expr__` into "valid" output; both must become refusals (or, for `With`, the ctor-rebuild lowering). Also: `i32.parse`/`f64.parse` should become std `Bound` bindings (the P13 "std, not compiler" move), deleting `MethodCall`'s per-target parse special case.

**Reserved & forbidden identifiers (user request 2026-07-02; 2-agent investigation — design: `json-plugins.md` §7).** Each language plugin declares an **`identifiers` manifest block** — the target's keywords + escape strategy (one source of truth with the `ident` builtin), its **`reserved` generated-scaffolding names** (`Main`/`Program`/`Extensions` on C#), and the **runtime `globals`** emitted code relies on (`console` on TS, `str` on Python, `$_SERVER` on a future PHP); **`pgconfig.json` adds per-project `forbiddenIdentifiers`** (target-scoped, `"*"` wildcard). Policy: escape silently only where a total reference-preserving transform is declared; otherwise **refuse with a targeted per-target diagnostic** via a `checkReservedNames` pass beside the capability gate (LSP squiggles per configured target, no client change); never auto-rename. **Hard invariant: identifiers only** — the check runs over sema's symbol tables, so string literals, interpolation chunk text, comments (lexer trivia), and `extern("…")` FFI templates can never trigger it, by construction. The investigation also surfaced **seven verified collision miscompiles shipping today** — incl. three silent-wrong-answer cases (a local `_m` in a match arm, a union field named `tag`, a user `fn _pg_idiv` on Python — the last invisible to every current gate) — fixed by collision-aware `fresh` temps + data-shape guards *before* the config feature lands (P19 slice order: hygiene first).

**Two scope decisions (user, 2026-07-02).** (1) **No backward compatibility:** no `--legacy-backend` fallback, no C++ backends kept a release behind — each layer's C++ deletes in the same slice its byte-identity gate passes (the gates verify the *extraction*, not a compat contract). (2) **The CLI is a pure engine — zero embedded target specs:** C#/TS/Python are **ordinary plugin packages** (developed in this repo under `plugins/<target>/`, published to npm like any third-party target); the tool reads `pgconfig.json` → `targets`/`dependencies` → resolves the npm packages (local `file:` path → lockfile-pinned cache → registry) → interprets the JSON, which carries **all** transpiling instructions. No pgconfig/targets → an actionable error, no fallback. "Zero-dep single binary" still describes the executable; the language data is simply not welded into it. Std *skeletons* (the target-neutral `.pg` API) stay in Core — they are the source language; every per-target arm ships in its target's plugin. Detail: `json-plugins.md` (scope-decisions note).

### 4.12 Alternative input syntaxes — "skins" (design — 2026-07-02; investigated by a 4-agent team; **gated, not scheduled**)

**The user need:** let a developer who doesn't want to learn `.pg` author in a familiar C#- or TS-flavored surface over the *same* semantics (Reason-over-OCaml, not "compile arbitrary C#"). Full design + evidence: `docs/design/frontend-skins.md`; contract: §3.F; roadmap: PLAN §P20.

**Findings (per front).** *Seam:* already clean — everything downstream of `parse()` consumes only the unchecked AST (`CompilationUnit`), which is syntax-neutral (semantic node kinds, canonical modifier vocabulary, structural `TypeRef`); a `Frontend`/`FrontendHandle` abstraction mirroring `BackendHandle` is ~1–2 days of plumbing, extension-dispatched in the CLI (`.pg`/`.pgcs`), `ResolvedModule` gains a front-end tag for mixed projects, std stays `.pg`, and the whole LSP transfers for free (SemanticModel is built from AST spans). The seam must be the AST, never the IR (sema/lower are shared, not reimplemented). `fmt` needs a per-skin printer or it silently rewrites skins to `.pg`; `convert` is parse-A→print-B (backends can't help — IR is desugared one-way), so **skin→`.pg` convert needs only the skin parser + the existing `.pg` printer**. *Mapping:* the **TS skin is net-negative** (widths inexpressible in one `number`; `as` erases vs `(T)x` truncates; `let` inverted; `for..in` iterates keys; `A|B` structural) — refused permanently; the **C# skin is defensible but "C#-flavored"** (widths/casts map perfectly — `int↔i32`, truncating casts; records/`with`/null-ops/switch-expr/indexers ≈1:1 — but it must *invent* `union`, selective imports, range-`for`, and reshape operators/extensions/async). *Data-driven front-ends:* **rejected** — parsing is partial/ambiguous/recovery-laden where emitting is total over a valid tree; a declarative AST-action language is a second, harder interpreter that can't stay non-Turing-complete; PEG data grammars regress diagnostics and can't do editor-grade recovery without a forbidden runtime dep; precedent is unanimous (Reason/ReScript hand-write). Front-ends are compiled-in C++; the P19 manifest may *declare* one by name (symmetric packaging, asymmetric implementation); the target/front-end asymmetry is principled — targets are open-ended, front-ends are few and stable. *Risk:* prior art warns hard (Reason's three-way fork; CoffeeScript's fade; Kotlin/Swift prove syntax familiarity is not the adoption lever); the maintenance multiplier lands on the one layer P19 can't make data-driven; a C#-looking surface is the most effective scope-creep vector against §3.B ("looks like C# → why not LINQ?").

**Decision — staged and gated (§3.F):** (1) now, near-free: Rosetta cheat-sheets ("Polyglot for C#/TS developers", incl. the `let`-immutability false-friend) + lean on the shipped P17 preview — the need is already ~80% met by `.pg`'s deliberately TS-flavored surface and the live preview; (2) the cheap `Frontend` seam lands post-P19 (P20 slice 1) to keep the door open; (3) one-way `polyglot convert` only on *observed* demand — it doubles as the cheapest honest demand test for a skin; (4) the C# authoring skin only if convert proves sustained demand and the grammar is frozen — compiled-in, `.pgcs`, dialect-banner framing, skin-scoped refusal diagnostics; (5) the TS skin never.

### 4.13 Watch mode (design — 2026-07-04; investigated by a 4-agent team; **implemented 2026-07-04**, PLAN §P21)

**The user need.** `polyglot build --watch`: keep the emitted output files on disk fresh as `.pg` sources change, so a host project consuming them (a C# solution, a bundler-watched TS app) rebuilds live. This is the **disk-file sibling of §4.9's preview** — keep the mental model crisp: *preview = unsaved in-memory emit, on-type, virtual doc; watch = saved-file emit to disk, on-change*. They can momentarily differ; they are deliberately not unified (watch never routes through the LSP). Then surface watch in both editor extensions. Slice plan: PLAN §P21.

**Findings (per front).** *Seams:* the watch loop lives **entirely in the CLI layer** — Core stays IO-free and needs **zero changes**. `compile()` discards the set of `.pg` files it loaded (only `analyze()`'s `SourceMap` records it), but no Core change is needed: every module load routes through `ModuleResolver::resolve`, so a **`RecordingResolver` decorator** over `FileModuleResolver` captures the exact transitive input closure (canonical absolute paths) per build. Rebuild cost is milliseconds (programs are 1–12 KB; every compile is already from-scratch; multi-target = an independent `compile()` per target) — **v1 is a full rebuild per change, no incrementality**. Two existing behaviors become stated guarantees: a failed compile **leaves the last-good outputs untouched** (`emitOne` returns before writing — the never-a-miscompile ethos applied to watch), and the plugin registry is **load-once per process** (`loadBackend` errors on duplicate names), so targets resolve once at watch startup and a plugin-manifest edit needs a watch restart (recorded limit). *Mechanism:* **portable timestamp polling of the exact input set** — a `(mtime,size)` baseline re-statted every ~250 ms — behind a **`FileWatcher` seam** (`watch(files)` / `waitNext(timeout)` / `stop()`) so a native `ReadDirectoryChangesW`/inotify impl can slot in later. Decisive because the watched set is a handful of files: polling is ~20 lines on the CLI's existing single thread (no thread), atomic-save/rename-over is transparent (stat by path), **self-triggering is impossible by construction** (outputs are never in the polled set — no rebuild loops, no extension filters), there is no re-arm race, and it works on network drives; every RDCW sharp edge (overflow-rescan, re-arm drops, `FILE_SHARE_DELETE`, short-name ambiguity) is cost with no benefit at this scale. Debounce = **250 ms quiet-window drain** after the first change (a multi-file save burst = one rebuild). Ctrl+C via `SetConsoleCtrlHandler` → atomic flag → clean exit 0. Mid-atomic-save partial reads: retry-on-sharing-violation (3×~30 ms) + skip-a-tick on a transient stat failure. *Precedent:* a **`--watch` flag, not a verb** (the tsc/esbuild/sass camp; `dotnet watch` is a generic process supervisor, a different beast; polyglot's verbs are first-class) — on `build` and on `check` (diagnostics-only). Full build immediately on start; **keep watching on error**; never clear the screen by default; **fixed un-localized English status lines with 24 h timestamps** (tsc's locale-sensitive anchors are a documented VS Code matcher breaker); **one begin/end anchor cycle per change event covering all targets** (not per-target pairs). Explicit v1 non-goals: interactive keys, JSON event stream, hot reload, screen-clear. *Editors:* **VS Code = a contributed task type + background problemMatcher** (`$polyglot-watch`, the ecosystem-standard `$tsc-watch` shape: free Problems-panel integration, no hand-rolled process management) plus **status-bar start/stop commands that run the task** (one code path; terminate is VS Code's job). LSP-side emit-on-save **rejected** — wrong layering (the LSP analyzes unsaved buffers and runs one instance per editor window; watch is a separate CLI process). No conflict with the §4.9 preview (in-memory, owns no status-bar item). **Visual Studio = near-zero VSIX work:** `dotnet watch` watches only `Compile`/`EmbeddedResource` by default but honors an explicit **`Watch` item group** — so the MSBuild NuGet adds one line, `<Watch Include="@(PolyglotFile)" />`, and `dotnet watch build/run` on a consuming project re-runs `PolyglotTranspile` on every `.pg` edit. That covers the C#-host path only (standalone TS/Python/PHP watch = the CLI — don't oversell); a native VS watch command would force the thin MEF VSIX to grow an `AsyncPackage` + `.vsct` and is rejected without demand.

**The frozen console protocol (the contract with the problemMatchers — golden-tested, or drift silently empties the Problems panel).** Begin sentinel `[HH:MM:SS] polyglot watch: building <entry>` (later cycles: `rebuilding`); per-diagnostic lines in **MSBuild-canonical form with absolute paths** — `ABSPATH(LINE,COL): error: message` — the one shape both VS Code's matcher and VS's Error List parse natively (the watch stream only; `build`/`check` keep their gcc-style `path:line:col:`); end sentinel `[HH:MM:SS] polyglot watch: N error(s) — watching for changes`. The sentinels are mandatory: without a begin/end bracket per cycle, background matchers never clear stale diagnostics (the classic bug). `activeOnStart` captures the initial build.

**What is watched.** Recomputed every cycle: the entry `.pg` + the `RecordingResolver` closure + the `pgconfig.json` walk-up chain, **plus the computed candidate path of each *unresolved* import** (cheap with polling — stat says "missing" until the file appears, so creating a file that fixes a broken import triggers the rebuild users expect). A `pgconfig.json` change triggers a **full context re-resolution** (root/lib/targets/forbiddenIdentifiers can change the whole build — the tsc-restarts-on-tsconfig behavior), with the recorded caveat that a `targets` change requiring a not-yet-loaded plugin still needs a restart (registry is load-once). `std.*` is embedded in the binary — correctly never watched.

**Decisions.** (1) `--watch` is a flag on `build` + `check`, no separate verb. (2) Polling watcher behind the `FileWatcher` seam, CLI-layer only, zero Core change. (3) The console protocol above is **frozen and golden-tested**. (4) VS Code: task type + `$polyglot-watch` matcher + status-bar toggle. (5) The NuGet ships the `Watch` item. (6) Failure never deletes or overwrites last-good outputs. (7) Deferred, recorded: native RDCW/inotify watcher, incremental module-graph rebuilds, `--clear`, a VS-native command (demand-gated), plugin-manifest hot reload.

### 4.14 Cross-platform CLI — Linux builds + multi-RID distribution (design — 2026-07-04; investigated by a 4-agent team; PLAN §P22)

> **macOS added to the shipping set** (user decision, 2026-07-11 — reverses the 2026-07-04 "not planned"
> call). The osx-x64/osx-arm64 design below (native CMake builds on `macos-13`/`macos-14`, ad-hoc `codesign`,
> the `_NSGetExecutablePath` exe-path branch) is now **built** in `release.yml` and staged into the fat NuGet;
> the shipping set is **Windows + Linux + macOS**. Gatekeeper is handled without an Apple Developer account
> (~$99/yr): natively-linked binaries are ad-hoc signed and the VS Code extension strips the
> `com.apple.quarantine` xattr on activation. Full Developer-ID **notarization is deferred** — reach for it
> only if the ad-hoc + quarantine-strip path proves insufficient on real hardware.

**The user need.** MintPlayer.AI (the first real consumer) builds on GitHub Actions **ubuntu-latest**; the
MSBuild NuGet ships only `tools/win-x64/`, so Linux CI cannot transpile `.pg` at build time — the pilot's
mitigation is committing the generated `.cs`. North-star gate: **a plain `dotnet build` on a Linux runner
transpiles `.pg` live via the NuGet, no committed output**. Same work unlocks the GitHub-Releases channel
for all platforms and the recorded P11 npm-sibling remainder.

**Findings (per front).** *Portability:* **Core is 100% portable standard C++** — zero Windows headers,
zero Win32, zero MSVC-isms (`__declspec`/`_s` funcs/pragmas), no wide-char/codecvt; ALL platform code sits
in the CLI's `main.cpp` + `tests_main.cpp`, and nearly every `#ifdef _WIN32` already has a POSIX branch.
Five fix sites, ~a day: (1) **exe-path discovery** (`main.cpp` `loadPluginsNextToExe`) falls back to bare
`argv0` on POSIX — the trap: **passes CI (relative-path invocation) yet silently breaks plugin discovery
for every PATH-invoked install** (npm/NuGet/tar put `polyglot` on PATH) → `readlink("/proc/self/exe")` on
Linux, `_NSGetExecutablePath` on macOS, argv0 last resort; (2) `tests_main.cpp` has NO `#else` for the same
lookup → all tests fail on POSIX; (3) the plugin cache dir's POSIX fallback is `fs::temp_directory_path()`
(volatile — installed plugins vanish on reboot) → `$XDG_DATA_HOME`/`~/.local/share/polyglot/plugins`
(macOS `~/Library/Application Support`); (4) `polyglot install`'s `npm pack` command ends with an
**unguarded cmd-ism `>nul 2>nul`** — on `/bin/sh` it creates a file named `nul` and hides nothing → `#ifdef`
to `>/dev/null 2>&1`; (5) a dead `windows.h` include in `watch.hpp` (nothing uses it). The compiler floor
is modest — **GCC ≥ 10 / Clang ≥ 12** (no `std::format`/ranges/coroutines/concepts; keep it that way).
*Build system:* **a parallel `CMakeLists.txt` for POSIX; the `.vcxproj`/`.sln` stays the untouched VS-2026
source of truth** (the user's IDE workflow is a hard constraint). The current build is easy to replicate:
explicit 14-file Core list, only `Core/include` (+`Cli/src` for Tests) includes, no defines beyond
`_DEBUG`/`NDEBUG`, no codegen (the embedded std is plain raw-string literals in `compiler.cpp`), one
post-build `plugins/` copy. Drift between the two definitions is the top risk → CMake
`file(GLOB … CONFIGURE_DEPENDS)` + a CI parity script diffing the `.vcxproj` `<ClCompile>` set against
disk. Runner-up recorded: full CMake migration (single source of truth) — revisit only if maintaining two
definitions bites; VS 2026's CMake path is presets-driven, a real workflow change. Cross-compiling
(zig/clang) rejected as primary: native GitHub-hosted runners are free for public repos and each leg runs
the test exe + gates it just built. *Static linking:* Windows keeps `/MT`; **Linux = `-static-libstdc++
-static-libgcc -pthread` built on ubuntu-22.04(-arm) → glibc 2.35 floor** (the protoc/LLVM model; covers
Ubuntu 22.04+/Debian 12+/RHEL 9+ — fine for a dev tool that already needs node + .NET; manylinux/musl
full-static recorded as the wider-reach fork); **macOS** has nothing to statically link but needs
`MACOSX_DEPLOYMENT_TARGET` (13.0) and — **mandatory on arm64, where unsigned binaries are SIGKILLed —
an ad-hoc codesign** (`codesign -s - -f` + `--verify`, no Developer ID, no notarization: the quarantine
xattr comes from browser downloads, not npm/NuGet/tar extraction). *Packaging:* the pleasant surprise —
**the NuGet's consume-side `.targets` is ALREADY fully multi-RID** (`PolyglotHostRid` from
`$(NETCoreSdkPortableRuntimeIdentifier)` → `tools/<rid>/polyglot[.exe]`, no `.exe` on Unix, `chmod +x`
on Unix, loud missing-RID error naming the override) — **zero changes**; the whole gap is the csproj's
hardcoded win-x64 pack ItemGroup + CI building one RID. **One fat package** (~2.6 MB for 5 RIDs — 1.12 MB
exe + 256 KB plugin JSON per RID, vs Grpc.Tools' accepted 22 MB); a per-RID split is rejected — NuGet has
no npm-style os/cpu restore-time selection for build-time tools. Each `tools/<rid>/` carries its own
`plugins/` copy (`loadPluginsNextToExe` wants them beside the binary; negligible). Grpc.Tools precedent
verified: it ships **no chmod at all**, relying on nupkg zip mode bits — documented as fragile
(NuGet/Home#13402, grpc#18338 "protoc not runnable on Alpine"); our existing chmod is belt-and-suspenders
the precedent lacks — keep it. *CI:* **one `release.yml`**: the existing windows job + a 4-leg POSIX
matrix — `ubuntu-22.04`, `ubuntu-22.04-arm` (arm64 runners GA + free for public repos since 2025-08;
this repo is public), `macos-15-intel` (**`macos-13` is retired**), `macos-15` — each CMake-building,
running the unit exe + conformance gates (pwsh/node preinstalled; `setup-dotnet` pins 10.0.x), attesting
provenance **per job** (archive + inner binary), uploading artifacts; then two fan-ins: `github-release`
(all archives; a dispatch-run derives the tag by executing the linux-x64 binary) and `nuget` (downloads
all artifacts into `tools/<rid>/`, packs once, pushes). PHP is preinstalled on ubuntu runners — **the
linux leg finally closes the open PHP runtime-differential TODO** (a new `run-php.ps1`). *npm sibling:*
the **esbuild `optionalDependencies` pattern** — an `@mintplayer/polyglot` wrapper (JS `bin` shim,
`require.resolve`, exact-pinned optionalDependencies) + per-platform payload packages
(`@mintplayer/polyglot-cli-<platform>-<arch>`, **Node tokens** `linux|darwin|win32`, `os`/`cpu` fields,
`preferUnplugged`, binary + `plugins/` beside it). npm **preserves the +x bit** — no chmod dance.
Postinstall-download rejected (offline/proxy/`--ignore-scripts`/CI-cache failures). Third naming scheme
alert: dotnet RID `osx-arm64` = npm `darwin-arm64` — one mapping table in the CI stage step.

**Decisions.** (1) **RID set v1: win-x64, linux-x64, linux-arm64** (macOS **not planned** — see below;
the `osx-x64`/`osx-arm64` design is retained for if that changes). Deferred: win-arm64, linux-musl-x64
(Alpine's `NETCoreSdkPortableRuntimeIdentifier` is `linux-musl-x64`, so the `.targets` already fails loudly
there — the correct failure mode, documented). (2) Parallel CMake for POSIX, `.vcxproj` untouched, glob +
CI parity gate. (3) glibc 2.35 floor (build on ubuntu-22.04); `-static-libstdc++ -static-libgcc`. *(macOS
ad-hoc codesign — mandatory on arm64 — applies only if macOS is picked up; not planned.)* (4) One fat NuGet;
`.targets` unchanged; the csproj packs a CI-staged `tools/` tree (`-p:PolyglotStageRoot=…`) and keeps the
historical single-RID local pack as fallback so `run-nuget.ps1` stays green offline. (5) NuGet publishing
moves from `publish-plugins.yml` (master-push) into `release.yml`'s fan-in → **NuGet becomes tag-gated**
(recorded behavior change: package cadence = release cadence; the npm plugin packages stay master-push).
(6) Artifacts: `polyglot-win-x64.zip` + `polyglot-<rid>.tar.gz` (tar preserves +x). (7) The five
portability fixes land first, byte-identical on Windows. (8) The npm sibling is the last slice (esbuild
pattern above) — NuGet-on-Linux is the driving need. (9) Deferred, recorded: win-arm64, linux-musl-x64,
manylinux/zig-musl wider floor, `lipo` universal macOS binary, notarization, full CMake migration.

### 4.15 Extension onboarding — bundle the CLI in the VSIX + branding (design 2026-07-11; slices 1–4 built, PR #16; investigated by a 2-agent team; PLAN §P23)

**The user need.** A user who installed the *released* VS Code extension from the marketplace and opened a
`.pg` file got a dead end: `spawn polyglot ENOENT` — "could not start the language server". **Highlighting
worked** (declarative TextMate grammar, no server), but every server-backed feature — diagnostics, hover,
go-to-def, formatting, the live preview (§4.9), watch (§4.13) — was dead. The released extension is a
client with no server to talk to. The bar the user set: **install the extension → open a `.pg` file →
it just works**, zero manual setup. Plus two branding asks: a real marketplace **icon** (there is none),
and a **rename** to "Polyglot language server".

**Findings (per front).** *Launch path (root cause).* The extension is a single hand-written
`editors/vscode/extension.js` (no build step). `resolveCli()` (`extension.js:30-38`) reads
`polyglot.cliPath` with a **bare-string default of `polyglot`** (`package.json:120-124`), and — if the value
has no slashes — returns it verbatim; that string is handed to `vscode-languageclient`'s `ServerOptions`
with the fixed subcommand `lsp` over stdio (`extension.js:45-48`) and spawned by Node **with no shell**, so
a bare word is resolved against the OS `PATH`. There is **zero discovery** — no bundled-binary lookup, no
probe of `%LOCALAPPDATA%`, no source-checkout fallback; the only "search" is that PATH resolution. On a
machine where the *separately distributed* CLI was never installed (the confirmed situation), PATH lookup
fails → `spawn polyglot ENOENT`, surfaced as a plain warning (`extension.js:97-102`). The same
`resolveCli()` also backs the watch task (`extension.js:271`), so watch fails identically. *Two correctness
nits found in passing:* (a) the failure dialog tells the user to point `cliPath` at
**`MintPlayer.Polyglot.Cli.exe`** — the *source-checkout* project name; the shipped/released binary is
**`polyglot.exe`** (`extension.js:100`); (b) `polyglot.cliPath` defaulting to a non-empty `polyglot` means
`resolveCli()` can't distinguish "user set it" from "unset" — an empty default is what a discovery ladder
wants. *Distribution today.* The vsix bundles **no server** — `.vscodeignore` excludes only
`.vscode/**`, `testbench/**`, `.gitignore`, `**/*.map`; there is no `bin/`. P22 ships the CLI on three
channels (GitHub-Releases `polyglot-<rid>.{zip,tar.gz}` → `polyglot.exe`/`polyglot` + `plugins/`; the fat
multi-RID NuGet; the npm sibling), all of which put the CLI *somewhere on disk* but none of which the
extension knows how to find. Note the CLI is a **native C++ binary** (not .NET, despite the `.Cli.exe`
name) — there is no `dotnet tool` global-install path. *Icon/branding.* **No icon asset exists anywhere in
the repo** — `package.json` has no `icon` field, no `images/`, no `.png`/`.svg` under the extension; the
marketplace shows the generic placeholder. The `.pg` language contribution (`package.json:102-109`) has no
`icon` either (generic file icon in the explorer). The rename is surgical: **only `displayName`
(`package.json:3`)** changes — `name` (`polyglot-lang`) + `publisher` (`mintplayer`) together form the
**immutable extension ID `mintplayer.polyglot-lang`**; changing either mints a new listing and breaks every
existing install + the marketplace URL. Marketplace-title caveat (CLAUDE.md:315): plain "Polyglot" was
already taken — "Polyglot language server" is a distinct, presumably-free title. The MintPlayer brand mark
is a **connected ti-ti** (two beamed eighth-notes), mint-green — the icon should be MintPlayer-brand-aware.

**The chosen strategy — bundle the CLI per-platform in the VSIX (user decision, 2026-07-11).** VS Code's
**platform-specific extension** mechanism is exactly this: `vsce package --target <target>` produces a vsix
carrying a native payload, and the marketplace serves the matching vsix per user platform, falling back to a
**platform-independent vsix** for everything else. So we publish one vsix per supported RID — each embedding
that RID's `polyglot(.exe)` + its `plugins/` under `editors/vscode/bin/` — plus a **universal
no-binary fallback vsix** (highlighting + `cliPath`/PATH) for unsupported platforms. This *pulls the setup
complexity entirely downward* into packaging: the user installs and it works. Bundling was previously gated
(PLAN P22 / §4.14 note) as future work behind "the same VS-2026-runner problem as per-RID packaging" — but
that gate was about *building* the CLI on many RIDs, which **P22 already solved** (`release.yml` builds +
provenance-attests win-x64, linux-x64, linux-arm64 and uploads them). The VSIX pipeline therefore does not
rebuild anything: it **downloads P22's already-released, attested CLI artifacts** for a pinned CLI version
and stages them — decoupling extension cadence from CLI cadence and pinning a known-good server. The RID set
is **identical to P22 — win-x64, linux-x64, linux-arm64** (VS Code targets `win32-x64`, `linux-x64`,
`linux-arm64`; a one-row dotnet-RID→VS-Code-target map, echoing P22's RID↔npm-token table); macOS, win-arm64
and alpine get the **universal fallback vsix** (consistent with "macOS not planned"). Two Unix gotchas the
investigation flagged and the design must handle: the vsix is a plain zip, so (1) the executable **+x bit**
may be lost on extraction → the extension `chmod 0755`s the bundled binary on activation (rust-analyzer's
belt-and-suspenders; P22's NuGet keeps an analogous chmod), and (2) the CLI finds its plugins **next to the
binary** (P22 slice-1 `exe_path.hpp`), so `plugins/` must sit at `bin/plugins/`, not elsewhere.

**The resolution ladder (replaces the bare-string spawn).** `resolveCli()` becomes an explicit, obvious
ladder — *define the error out of existence by finding the CLI before failing*: (1) **`polyglot.cliPath`**
if non-empty — the explicit dev/advanced override, absolute-or-relative-to-workspace, semantics unchanged
(the default flips to `""` = auto); (2) the **bundled binary** `<extensionPath>/bin/polyglot(.exe)` if
present (the happy path for the 3 supported RIDs; chmod +x on Unix); (3) **`polyglot` on `PATH`** (self-
installed CLI / universal-fallback users); (4) the **source checkout** — per platform's build output:
Windows `<workspace>/x64/{Release,Debug}/MintPlayer.Polyglot.Cli.exe`, Unix `<workspace>/build/polyglot`
(contributors working on this very repo — the dev testbench already points here); (5) **fail into an
actionable modal**, not a dead end: buttons "Install the CLI" (open the Releases page), "Locate
polyglot.exe…" (file picker → writes `polyglot.cliPath`), "Open Settings" — and the message names the
correct binary. Watch mode inherits the ladder for free (shared `resolveCli`).

**Branding.** *Marketplace icon:* a **256×256 PNG** at `editors/vscode/icon.png` (flat, non-transparent,
no gradients per marketplace guidance), added as `"icon": "icon.png"` in `package.json`, plus an optional
`galleryBanner`. Design direction (MintPlayer-brand-aware): the **connected ti-ti eighth-notes** wordmark
in **MintPlayer mint** on a clean tile, carrying a light "many-targets" cue (e.g. the two noteheads reading
as the two first-class targets) — recognizably MintPlayer, legible at 16 px. *Optional `.pg` file icon:* a
light/dark SVG pair on the language contribution so `.pg` files get a branded explorer glyph. *Rename:*
`displayName` → **"Polyglot language server"**; `name`/`publisher` **untouched**. *Version:* bump the
extension (0.1.0 → **0.4.0** to sit with the ecosystem's 0.3.x line and mark the bundled-server milestone),
so CI's `skipDuplicate` actually publishes; the vsixes **pin and bundle a specific CLI version** (0.3.1
today), declared once in the workflow for reproducibility.

**Decisions.** (1) **Bundle per-platform** via `vsce package --target`; publish win32-x64 + linux-x64 +
linux-arm64 platform vsixes **plus a universal no-binary fallback** (macOS/win-arm64/alpine → fallback,
matching P22's RID scope). (2) The VSIX pipeline **downloads P22's already-attested CLI release artifacts**
for a pinned version and stages `bin/<one-rid>/…` → `bin/` (one RID per platform vsix, so no per-RID subdir
at runtime); it never rebuilds the CLI. (3) `resolveCli()` becomes the 5-rung ladder above; `polyglot.cliPath`
default flips to `""` (auto), staying the documented escape hatch. (4) Unix: **chmod +x on activation** +
`plugins/` staged at `bin/plugins/` (next to the binary, per `exe_path.hpp`). (5) The dead-end warning
becomes an **actionable modal**, and the wrong exe name (`MintPlayer.Polyglot.Cli.exe` → `polyglot.exe`) is
corrected regardless of the rest. (6) Icon 256² PNG + `"icon"` field + optional `.pg` language icon +
optional `galleryBanner`; **`displayName` only** for the rename, ID frozen. (7) Extension → 0.4.0, bundling
CLI 0.3.1. (8) `publish-vscode.yml` reworks to a target matrix (each leg stages the matching CLI + `vsce
--target`), publishing all vsixes for one version bump. (9) Deferred, recorded: macOS/win-arm64/alpine
bundling (universal fallback covers them today), an in-extension one-click *downloader* (the bundle removes
the need), and CLI-version auto-update inside the extension.

### 4.16 Tag-driven release automation (A→B→C) + lockstep versioning (design — 2026-07-11; investigated by a 4-agent team; PLAN §P24)

**The user need.** Every release today needs **manual glue**: bump the version in source, merge, **hand-push a `v*` tag**, then **manually re-run** the extension publish. Worse, the version lives as a *committed constant* (`kVersion` in `polyglot.hpp`, `<Version>` in the NuGet csproj, `version` in `package.json`, and a hand-maintained `POLYGLOT_CLI_VERSION` in the extension workflow) — four numbers that drift. That drift is not hypothetical: `POLYGLOT_CLI_VERSION` was bumped to `0.3.2` before any `v0.3.2` release existed, so the extension publish `gh release download v0.3.2`'d a release that wasn't there and 404'd. The user's target: **one action — a version bump — and the whole chain releases itself, in order**, with the tag as the single source of truth.

**The architecture (three chained workflows).** *Workflow A (auto-tag)* reacts to commits on `master`, determines the next version, and creates + pushes the `v*` tag. *Workflow B (build/publish CLI + NuGet + plugins)* reacts to the tag push; the **tag name is injected as the version at build time** (no committed version constant), then it builds, packages, and publishes the CLI (5-RID GitHub Release), the NuGet, and the four npm plugins — all at the tag version. *Workflow C (extension publish)* reacts to Workflow B's **GitHub Release completing**, bundles the just-released, provenance-attested CLI into the per-platform vsixes, and publishes to the marketplace at the same version.

**Workflow A — auto-tag.** Triggers on `push: branches: [master]` (+ `workflow_dispatch`). It computes the next version, checks whether `v<version>` already exists (`git fetch --tags`), and if not, creates an annotated tag at the pushed SHA and pushes it. Idempotency is intrinsic — a merge that doesn't intend a release produces an already-existing (or no) tag and is a clean no-op; no `[skip ci]`/path-filter parsing needed. Concurrency `group: auto-tag, cancel-in-progress: false` serializes rapid merges. **The load-bearing gotcha (verified):** a tag pushed with the default `GITHUB_TOKEN` **does not trigger** the tag-listening Workflow B — GitHub suppresses recursive workflow runs. So A **must push the tag with a non-`GITHUB_TOKEN` identity**: a **GitHub App installation token** (`actions/create-github-app-token`, Contents:read+write, no expiry — recommended for a long-haul project) or a fine-grained repo-scoped PAT (simpler to stand up, but expires → releases silently stop). A load-bearing comment must pin this so a future "simplification" back to `GITHUB_TOKEN` doesn't silently break releases.

**Workflow B — version injection.** The version reaches the native C++ build via **one compile-line define, `-DPOLYGLOT_VERSION=<x.y.z>` (unquoted token)**, consumed by a stringizing macro in `polyglot.hpp` with a `#ifndef` fallback to `0.0.0-dev`: the *same bare token string* works verbatim in MSBuild `/p:`, on the `cl.exe` line, and in CMake `add_compile_definitions` — no quote/backslash-escaping fork between the two build systems. Because `kVersion` is a header `inline constexpr`, the define must reach **all three** projects (Core, Cli, Tests) — a repo-root `Directory.Build.props` carrying one `$(PolyglotVersion)` property + one `ItemDefinitionGroup` per `.vcxproj`, and a `POLYGLOT_VERSION` cache var in `CMakeLists.txt`. The `Compiler::version() == kVersion` self-test is *repurposed* (not retired): it now catches per-project define drift. `scripts/check-buildfile-parity.ps1` is **extended** to assert the define exists in all three `.vcxproj` **and** CMake (the existing gate only compares `.cpp` lists, so it wouldn't catch a missing define). The NuGet side is a plain `dotnet pack -p:Version=<tag>`; plugins are `npm version <tag> --no-git-tag-version` before publish. *(Alternative considered — the "single-definition-point" variant: move `kVersion` out of the header into `polyglot.cpp` (one TU), change the LSP `serverInfo` site to call `Compiler::version()`, so only Core needs the define; eliminates the per-project-drift risk at the cost of touching `main.cpp` + rewriting the self-test as a shape check.)* The tag grammar is `vMAJOR.MINOR.PATCH[-prerelease]` (no `+build` — NuGet strips it); B validates the tag and strips the `v` once. Local dev builds inject `git describe --tags --dirty` (or fall back to `0.0.0-dev`), so `--version` self-describes honestly instead of asserting a stale committed number.

**Workflow C — extension publish.** Triggers `on: release: types: [published]` (+ `workflow_dispatch` with an explicit `tag` input for manual re-runs), reading the version from `github.event.release.tag_name`. This is the tightest causal guarantee: the event *is* the release's existence with all five RID archives already attached, so the stage step cannot 404 — the entire "referenced a tag before it existed" failure class is gone. C downloads the **release assets by tag** (the attested bytes, not build artifacts) via `stage-cli.ps1` (which gains a `-Tag` parameter), stamps `package.json` to the tag version (`npm version --no-git-tag-version`), and publishes. **Every hard-won mechanic carries over unchanged**: the two-step `vsce package --target` → publish via `extensionFile`, the 6-leg platform matrix + universal fallback, macOS ad-hoc-sign + quarantine-strip, `skipDuplicate`, `fail-fast: false`, and the frozen `mintplayer.polyglot-lang` ID.

**Versioning — a single lockstep version, jump to 0.5.0.** Today four families version independently (CLI/NuGet `0.3.2`, extension `0.4.1`, plugins `0.3.0`), but the extension *bundles* a specific CLI + plugin set, so independence is a fiction that produces skew. **Decision: one tag drives ALL five families to one shared version.** One number identifies every shippable byte; `POLYGLOT_CLI_VERSION` disappears entirely (derived from the tag). The first unified tag is **`v0.5.0`** — strictly ahead of every published artifact (ext 0.4.1, CLI/NuGet 0.3.1, plugins 0.3.0), so every registry accepts it cleanly and it reads unambiguously as the unification boundary (0.4.2 would wrongly read as a patch on the extension line). The committed-but-never-released `0.3.2` is **abandoned** (its content — macOS bundling, the publish fix — folds into 0.5.0). The **plugins fold into tag-gated Workflow B** (their push-to-main cadence is retired), because the npm-installed plugin must equal the plugin bundled in the CLI archive at that release commit; the manifest-validation `loadBackend` pre-gate moves to a PR/CI check. Lockstep's honest cost is no-op version bumps (a UI-only extension change still cuts a new CLI/NuGet/plugin version with identical content) — accepted for simplicity; per-leg "skip if unchanged" diffing is explicitly *not* built first (it reintroduces the skew reasoning lockstep removes).

**Migration — one cutover commit, then one tag.** The old publishers trigger on *push to master*; the safety property is that for a `push` event GitHub evaluates the workflow file *from the pushed commit*, so a single cutover commit that **removes those push triggers** in the same commit that sets `0.0.0-dev` placeholders will not fire them on merge. The cutover PR: (1) neutralizes the `on: push` auto-publish triggers in `publish-vscode.yml` / `publish-plugins.yml`, repoints `release.yml` per B; (2) retires the committed version constants to `0.0.0-dev` placeholders (`polyglot.hpp` macro, csproj, `package.json`, each `plugins/*/package.json`) and **deletes `POLYGLOT_CLI_VERSION`** (the thing 404-stalling the extension); (3) adds Workflows A/B/C. Then `v0.5.0` is pushed (by hand once, or by A). No double-publish is possible: 0.5.0 is fresh-and-ahead on every registry, and all three publish paths are idempotent per version (`--skip-duplicate` / `npm view … && skip` / `skipDuplicate`).

**Decisions.** (1) **Three chained workflows A→B→C**, tag as the single source of truth. (2) **Version injected at build** via one `-DPOLYGLOT_VERSION` token (MSVC + CMake), committed sources are `0.0.0-dev` placeholders; `check-buildfile-parity.ps1` extended to guard the define. (3) **Workflow C triggers `on: release: published`**, bundles the tag's attested assets. (4) **Single lockstep version** across CLI + NuGet + extension + 4 plugins; **first tag `v0.5.0`**; abandon `0.3.2`. (5) **Plugins publish from the tag** in Workflow B (push-to-main cadence retired). (6) **A must push tags with a GitHub App token / fine-grained PAT**, never `GITHUB_TOKEN`. (7) One cutover commit (removes old push triggers + sets placeholders) makes the migration double-publish-proof.

**Open decisions (need the user's call before implementation).** *(D1)* **Unified `0.5.0`** vs `0.4.2` — recommend 0.5.0. *(D2 — the truth-direction decision)* how Workflow A **determines the next version** with no committed constant: (a) **PR-label-driven** (`semver:major|minor|patch` against the latest tag, default patch, no label ⇒ no release) — the pure tag-driven model matching this design; vs (b) keep a single committed **`VERSION` marker file** A reads (explicit, but a committed number the tag mirrors). Recommend (a). *(D3)* **GitHub App token** (no expiry, recommended) vs **fine-grained PAT** (live today, expires). *(D4)* version-injection **header-macro-in-3-projects** (minimal churn + parity-guard) vs **single-definition-point** (lower drift risk, touches `main.cpp` + the test). *(D5)* **extension pre-releases**: none (recommended — hold the extension for stable tags) vs the marketplace `--pre-release` flag. *(D6)* CLAUDE.md's inline release changelog → split into a tag-keyed `CHANGELOG.md` (optional). Nothing is built yet — this section + PLAN §P24 are the design for review.

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
  valid). "Show Generated Output" opens **all three targets** beside the source (one tab each, following the active
  `.pg`); an Explorer "Polyglot Outputs" tree opens a single target on demand. A follow of `P16`'s virtual-doc +
  custom-request plumbing. Full design + slice plan: §4.9 / PLAN §P17.
- **P18 — Data-driven backends (languages as pure-JSON plugins).** 🚧 Slices 1–15 ✅ built (2026-07-02; §4.10, from
  a 4-agent investigation). Replace the compiled-in C#/TS/Python `Backend` classes + `Target` enum with a **bounded
  JSON emission DSL** (Design A — fixed interpreter primitives, non-Turing-complete, no plugin code → RCE-safe) that
  `EmitterBase` interprets. **Built so far:** the interpreter (`backend_engine`, incl. `map`+item), all three
  backends' specs + expression rule tables over one shared `IrExprCtx` seam, `Target`→`BackendHandle`
  (`findTarget(name)`, validated at resolve) — every slice proven byte-identical (old-vs-new emitted-source diff,
  38 programs × 3 targets). The remaining tail (expression residue, declarations, loader, std arms, distribution)
  is superseded by **P19**. Full design: §4.10; as-built log: PLAN §P18.
- **P19 — 100% JSON plugins (the complete artifact).** 🚧 Designed (2026-07-02; §4.11, from a 4-agent
  investigation). Close everything P18 left imperative so a language plugin is **entirely JSON**: declaration rule
  tables + a `program` scaffold rule (`line`/`block`/`mapDecl`), type-rule tables (`type` primitive), the hard
  expression residue (`interleave`/`fold`/`emitBlock`/`fresh`/`require`), a **~10-entry generic builtin catalog**
  (steady-state new language = zero Core changes; pioneers pay one additive bump, gated by `requiresCore`),
  lowering-absorbed module facts, the `polyglot-plugin.json` artifact (tri-state capabilities, std **overlays**
  collapsing the last hardcoded cs/ts/py IR fields, load-time anti-silent-drop validation), `polyglot install` +
  registry, and the **proof: a downloaded 4th backend emitting with zero Core change**. Full design + slice plan:
  §4.11 / `docs/design/json-plugins.md` / PLAN §P19.
- **P20 — Alternative input syntaxes ("skins").** 🚦 Gated, not scheduled (2026-07-02; §4.12 + §3.F). Rosetta
  docs shipped (slice 0); the `Frontend` seam, one-way `polyglot convert`, and the `.pgcs` C# authoring skin
  open only on observed external demand with the grammar frozen. The TS skin is refused permanently.
- **P21 — Watch mode.** ✅ Done (2026-07-04; §4.13, from a 4-agent investigation). `polyglot build --watch`
  / `check --watch`: a CLI-layer polling watcher over the exact transitive input set (a `RecordingResolver`
  captures the closure — zero Core change), a frozen tsc-style console protocol parsed by a `$polyglot-watch`
  VS Code problemMatcher (task type + status-bar toggle in the extension), and one line in the MSBuild NuGet
  (`<Watch Include="@(PolyglotFile)" />`) so `dotnet watch` gives Visual Studio the C#-host path for free.
  Slice plan: PLAN §P21.
- **P22 — Cross-platform CLI (Linux) + multi-RID distribution.** 🚧 Slices 1–2 + 4–5 built; slices 3 & 6
  remain (**macOS added 2026-07-11** — shipping set = Windows + Linux + macOS; CLI → 0.3.2)
  (2026-07-04; §4.14, from a 4-agent investigation). Built: the POSIX resilience fixes + command-quoting
  audit (shared portable exe-path lookup, XDG cache dir, `>nul`→`/dev/null`, all `#ifdef`-guarded); a
  parallel `CMakeLists.txt` (`.vcxproj` untouched) + drift-parity guard; a **Windows + Linux (x64/arm64) +
  macOS (x64/arm64)** release matrix with per-job provenance attestation; and the **fat multi-RID NuGet**
  (all five RIDs in one package; the `.targets` was already RID-generic). **North star reached
  and verified on real Linux (WSL):** `dotnet build` on a net9.0 app consuming the multi-RID NuGet resolves
  + chmods + runs `tools/linux-x64/polyglot`, transpiles the `.pg`, and runs — no committed output. macOS
  legs (`macos-13`/`macos-14`, ad-hoc `codesign`) build + gate but await verification on real hardware.
  Remaining: the PHP runtime differential (slice 3) and the esbuild-pattern npm sibling (slice 6). Slice
  plan: PLAN §P22.
- **P23 — VS Code extension: bundle the CLI (zero-setup) + branding.** 🚧 Slices 1–4 built (2026-07-11,
  PR #16; §4.15, from a 2-agent investigation). Make the *released* marketplace extension work out of the
  box — install it, open a `.pg` file, the LSP starts — instead of failing `spawn polyglot ENOENT` (the vsix
  ships no server and `resolveCli()` does zero discovery). Fix: **bundle the CLI per-platform in the vsix**
  via VS Code's platform-specific-extension mechanism (one marketplace listing, ID `mintplayer.polyglot-lang`
  frozen; the marketplace serves the matching vsix per platform), reusing P22's already-attested CLI
  artifacts (win-x64/linux-x64/linux-arm64 + a universal no-binary fallback), a 5-rung `resolveCli()` ladder,
  plus branding (marketplace icon + rename to "Polyglot language server", extension → 0.4.1). Extension + CI
  only, no Core change. First publish run surfaced a HaaLeo packagePath+target incompatibility (fixed: two-step
  package/publish); **macOS bundling added** (darwin-x64/darwin-arm64 legs + quarantine-strip on activation,
  bundling CLI 0.3.2). Pending: interactive vsix install + confirming the fixed publish. Slice plan: PLAN §P23.
- **P24 — Tag-driven release automation (A→B→C) + lockstep versioning.** 🚧 Designed (2026-07-11; §4.16,
  from a 4-agent investigation). Kill the manual release glue: one version bump → three chained workflows
  release everything. **A** auto-tags on merge (must push with a GitHub App token, not `GITHUB_TOKEN`, or the
  chain breaks); **B** injects the tag as the version at build time (one `-DPOLYGLOT_VERSION` token across
  MSVC + CMake — no committed version constant) and publishes CLI + NuGet + plugins; **C** publishes the
  extension `on: release: published`, bundling the tag's attested assets. Single **lockstep version** across
  all five artifact families, first tag **`v0.5.0`** (abandons the stuck 0.3.2). Migration is one cutover
  commit that neutralizes the old push triggers + sets `0.0.0-dev` placeholders. Slice plan: PLAN §P24.
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
