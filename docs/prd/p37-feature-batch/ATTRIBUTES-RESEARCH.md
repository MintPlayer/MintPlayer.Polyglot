# P37 §D — attributes investigation findings (4-agent team, 2026-07-23)

Condensed reports from the four-agent investigation (haxe-research / prior-art / target-semantics /
repo-grounding) that produced the three-tier attributes design now in PRD §D (originally drafted as
a separate "P38" and folded back into this batch — attributes are one feature, one PR). The PRD
carries the decisions; this file carries the evidence and citations. Context: the maintainer's
block on the first §D draft — attributes/decorators diverge fundamentally per target (C#/PHP:
inert, ctor never runs without explicit reflection; TS/Python: decorators execute eagerly at
definition time), so neither a single unified concept nor two intersection-gated concepts
("Attributes" vs "Decorators") seemed usable. Related: issue #64 (Haxe philosophy comparison).

## 1. How Haxe resolved the same friction (haxe-research)

Haxe never let one "attribute" concept be both inert-data and behavior-transform. It split along a
hard syntactic fault line — the leading colon — into **categorically different things**:

- **`@name` (runtime metadata) — pure inert data with NO constructor.** Not a class, not a callable:
  a name bound to a tuple of **constant-only** values. The compiler serializes it into a static
  per-type data table (`__meta__` on JS: `{ obj: {…}, fields: {…}, statics: {…} }`, name → array of
  arg values) read back via `haxe.rtti.Meta.getType/getFields/getStatics`. **No user code ever
  executes when metadata is attached — the "constructor runs eagerly vs never" fault line cannot
  arise because there is nothing to run on any target.**
  ([lf-metadata](https://haxe.org/manual/lf-metadata.html),
  [haxe.rtti.Meta](https://api.haxe.org/haxe/rtti/Meta.html))
- **`@:name` (compile-time metadata) — omitted from output entirely**; consumed by the compiler
  (`@:keep`, `@:generic`, `@:structInit`) or by **`@:build`/`@:autoBuild` macros**, which transform
  the AST at compile time and leave behind ordinary code. All decorator-like behavior lives here;
  **no target ever sees a decorator at runtime.**
  ([macro-type-building](https://haxe.org/manual/macro-type-building.html))
- **Typed native pass-through** — `@:strict(SerializableAttribute)` emits a **real** native C#/Java
  attribute and **is type-checked** against the native declaration (the ONE place Haxe type-checks
  metadata; it exists precisely for pass-through). `@:meta(...)` is the untyped sibling. Per-target
  generators consume them; on non-matching targets they are **silently inert** (no diagnostic — a
  Haxe weakness Polyglot's D12 loud-refusal improves on).
  ([cr-metadata](https://haxe.org/manual/cr-metadata.html),
  [#2065](https://github.com/HaxeFoundation/haxe/issues/2065))

Haxe's acknowledged regrets, i.e. what Polyglot can do better: metadata is **stringly-typed** (no
arg type-checking, no attachment-point constraints, misspelled names silently do nothing, `Meta`
returns `Dynamic`); the "Typed metadata" evolution proposal
([haxe-evolution #73](https://github.com/HaxeFoundation/haxe-evolution/pull/73)) was rejected on
mechanism, with the endorsed direction being **"metadata as typed declarations whose signature
declares the possible arguments"** — exactly the shape Tier 2 (PRD §D.2) adopts. Runtime metadata also
interacts badly with DCE (needs `@:keep`) and bloats output for types nobody queries — why it is a
separate opt-in channel.

## 2. Cross-compiler prior art (prior-art)

Model legend: **(a)** pass-through to native annotation · **(b)** synthesized uniform runtime
metadata table + reflection accessor · **(c)** compile-time consumption → generate plain code ·
**(d)** full macro system.

| Project | Chose | Refused | Note |
|---|---|---|---|
| Kotlin Multiplatform | (a) `expect`/`actual` annotation classes, `@OptionalExpectation` (erases where no actual); (c) kotlinx.serialization compiler plugin ("reflectionless") | runtime annotation reflection in common code | [expect/actual](https://kotlinlang.org/docs/multiplatform/multiplatform-expect-actual.html), [kotlinx.serialization](https://github.com/Kotlin/kotlinx.serialization) |
| Fable (F#→JS) | (a)+(c) `[<Emit>]`/`[<Import>]` consumed at compile time, never exist at runtime; compile-time-reflection JSON decoders | faithful runtime reflection ("pollutes the generated JS") | [features](https://fable.io/docs/javascript/features.html) |
| Scala.js/Native | (a) `@JSExport` steers the compiler; (d) macros/derivation as the reflection *replacement* | runtime reflection, explicitly to protect DCE/size | [semantics](https://www.scala-js.org/doc/semantics.html) |
| GWT / J2CL | (c) generators / annotation processors; annotations fully erased | "Reflection isn't possible with GWT" | [deferred binding](https://www.gwtproject.org/doc/latest/DevGuideCodingBasicsDeferred.html) |
| **Bridge.NET / JSIL / SharpKit** (all dead) | **(b)** — reflection metadata for all classes **by default**: `[app].meta.js` sidecar, ~60K runtime kernel, `[Reflectable]` opt-**out** filter language | — | [Bridge attributes](https://bridge.net/attributes/attribute_reference/) — the cautionary tale |
| C# source generators / Java APT | (c) — the industry convergence; additive-only, zero runtime cost | — | the reference model |

**The trap is precise:** what killed the dead cohort's metadata story was **default-on, whole-program
emulation of another platform's runtime reflection** (every class pays; sidecar files; runtime
kernel; an opt-out DSL bolted on later). It is NOT "constant data in output" per se — Angular Ivy,
STJ source-gen, and Symfony's compiled container all lower declarative markers to static data at
build time as their *normal modern operation*. The discipline that separates the two: **opt-in,
pay-only-per-annotated-and-queried, constants only, no reflection runtime, no open world.**

## 3. Target-language ground truth (target-semantics)

- **C#**: attributes inert in metadata; ctor runs **only** on the `GetCustomAttributes` family
  (fresh instance per call; `CustomAttributeData` reads without constructing). Args: compile-time
  constants (+`typeof`, 1-D arrays). Ecosystem actively migrating to **build-time** consumption
  (STJ source-gen, RDG, `[LibraryImport]`, AOT/trimming pressure).
  ([reflection vs source-gen](https://learn.microsoft.com/en-us/dotnet/standard/serialization/system-text-json/reflection-vs-source-generation))
- **TypeScript**: two dialects. Legacy `experimentalDecorators`+`emitDecoratorMetadata`/
  reflect-metadata (param decorators; no longer standardization-bound). **Standard TC39 stage-3
  decorators, default since TS 5.0**: class/method/accessor/field only, **no parameter decorators,
  no emitDecoratorMetadata**; `Symbol.metadata`/`context.metadata` IS stage 3 and in TS 5.2, but is
  one shared bag written only by decorators that execute. Decorators **run at definition time** —
  the wrong tool for inert data. **Angular Ivy reads `@Component` at build time and lowers to static
  `ɵcmp`/`ɵfac` fields, stripping the decorators from the bundle** — decorator-in-source →
  static-data-on-class is mainstream practice. Emit dialect if ever emitting decorators: TC39.
  ([TS 5.0](https://www.typescriptlang.org/docs/handbook/release-notes/typescript-5-0.html),
  [decorator-metadata](https://github.com/tc39/proposal-decorator-metadata),
  [Ivy architecture](https://github.com/angular/angular/blob/main/packages/compiler/design/architecture.md))
- **Python**: decorators execute at definition time (same problem as TS). Inert-metadata analogues:
  `typing.Annotated` (idiomatic for type-position/field metadata — Pydantic/FastAPI), or a plain
  generated dict — boring, readable, zero-import. No native inert class/method annotation.
- **PHP 8**: `#[Attr]` inert until `getAttributes()->newInstance()` (fresh instance per call); args
  constant expressions; targets declared via `#[Attribute(TARGET_*)]`. Symfony/Laravel read them at
  **container-compile/cache-warm time** and bake static data.
  ([newInstance](https://www.php.net/manual/en/reflectionattribute.newinstance.php),
  [php.watch](https://php.watch/articles/php-attributes))
- **Synthesized-constant-table assessment**: idiomatic and hazard-free on all four (C# static
  readonly/`FrozenDictionary` — trimming/AOT-safe; TS per-type `const` literals — tree-shakeable if
  not monolithic; Python literal dict; PHP constant array — OPcache-free). Coexists cleanly with
  pass-through native attributes (separate symbols, separate consumption paths).

## 4. Repo grounding (repo-grounding — file:line on master @ 92c795c)

- **The refusal wording already carves out what Tier 2 needs**: PRD §3.B refuses "runtime reflection …
  (**compile-time metadata only** — reflection defeats tree-shaking…)" (`POLYGLOT_PRD.md:90-91`);
  SPEC refuses "attribute introspection **at run time**" (`SPEC.md:469-470`). A compile-time-resolved
  metadata query that lowers to constants is inside the carve-out; the SPEC wording needs the
  emit/read-split made explicit (planned since the first §D draft).
- **Synthesis precedent**: the compiler already emits a uniform construct per-target from
  precomputed IR facts — structural equality (`ir.hpp:97-105` per-node facts; `lower.cpp:130-145`
  module-facts precompute; per-plugin idioms, e.g. TS `JSON.stringify` union eq at
  `plugins/typescript/polyglot-plugin.json:3456-3500`) — and already emits something on some targets
  and nothing on others (C# records get equality for free). A metadata lowering follows this grain.
- **Compiler-intrinsic std precedent**: `i32.parse` typed intrinsically (`compiler.cpp:185-189`),
  `Math.min/max/…` call-site-inlined generics (`compiler.cpp:65-77`), `List`/`Array` element access
  compiler-handled. A `std.meta` intrinsic API has direct precedent.
- **Explicit type-arg call syntax exists** for construction/call (`parser.cpp:1027-1032`,
  `Name<TypeArgs>(args)`) — `Meta.get<T, A>()` needs it on a member call, a small parser extension.
- **No uniform runtime type-id primitive across the four targets** (unions discriminate by string
  tag on TS/Py/PHP vs nominal record on C#; interfaces have no runtime identity on TS) — any
  *runtime*-dispatched metadata lookup would have to invent one. The compile-time-resolved query
  avoids needing it at all.
- **Capability system**: flat 16-member `Feature` enum on master (`backend.hpp:24-45`); the keyed
  `parent:child` vocabulary is this batch's Slice 0 (design-only until built). Tier 2 needs NO capabilities; Tier 1 keeps
  its §D gating.
- **This batch's PRD §D (first draft)** was the Tier 1 design + the §D.1 two-model analysis this investigation
  builds on; nothing there is invalidated.

## 5. Synthesis — what the evidence adds up to

1. The execution-timing divergence is dissolved by **removing the constructor**, not by bridging
   it: portable metadata must be a **data shape**, never a callable (Haxe proved this across 10+
   targets for 15 years).
2. The portable tier must **not map to native mechanisms at all** — the compiler emits plain
   constant data itself, so the tier works on **every** target unconditionally; the maintainer's
   intersection-kills-everything objection evaporates because there is nothing to intersect.
3. Read-back must be **compile-time-resolved** (source-generator model, the industry convergence),
   lowering to inline constants — not a runtime reflection surface (the Bridge/JSIL/SharpKit
   tombstone) and not a runtime table keyed by a type-id primitive the targets don't share.
4. Pass-through to native annotations stays a **separate, per-target-gated tier** (P37 §D) — the
   dominant surviving model (Kotlin expect/actual, Fable, Haxe @:strict) — and Polyglot's
   loud-refusal (D12) + typed bindings improve on Haxe's silent inertness.
5. Behavior-transforming decorators stay refused (P37 §D.1); Haxe's answer is a macro system, which
   is the named scope-creep killer for this project.
