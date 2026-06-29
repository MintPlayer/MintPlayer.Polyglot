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
both sides), function overloading (→ compile-time name-mangling),
strings/char (**both targets are UTF-16** — near 1:1, with surrogate-pair care).

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
- **nullability:** normalize `null`/`undefined`; pick one and stick to it.
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
  **designed now and enforced when the third backend lands** (P9/P10) — added *before* shipping a target
  that can't do a feature already in the wild, never retrofitted after (the lesson from Haxe's late
  threading-capability retrofit).

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

### 4.4 Standard library & platform APIs — the bounded strategy
The investigation's headline cost. Polyglot bounds it deliberately:
- **A small portable std** written in the source language (collections, strings, math, iterators) compiled
  to every target — the only thing guaranteed identical everywhere.
- **Target-gated native access via an `expect`/`actual` split** (Kotlin's model): portable code may name a
  capability (time, env, IO) via an `expect` declaration; each target supplies an `actual`. Platform APIs
  (`document`/`window` on JS, `System.*` on .NET) live **only** in target-gated regions — the portable
  core is compiler-forbidden from touching them (kills the #1 portability bug class).
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
- **P9 — Declarative backend engine + DSL.** Extract a declarative backend format from the two native
  backends; re-express C#/TS as specs the core interprets (gate: byte-for-byte vs. native golden output).
- **P10 — Plugin distribution + ecosystem.** `pgconfig.json` + download/cache/verify/version; availability by
  target+environment; build-dependency threading; the local full-power tier; proof = a **downloaded
  declarative Python backend** + a binding plugin, with **no core change**. The endpoint of §4.4 — see
  [`../design/plugins-and-targets.md`](../design/plugins-and-targets.md).
- **Stretch:** further targets as downloadable backends, source maps, an LSP built on the frontend-as-
  library, a plugin registry + signing/trust infrastructure.

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
