# Polyglot vs Haxe — capabilities and reasoning

*Issue #64. Written 2026-07-24 from a four-agent investigation (Haxe philosophy/architecture ·
cross-target fidelity · output/interop/DX · Polyglot pillars grounding); every load-bearing claim
cites a primary source. An earlier, narrower leg of this comparison — Haxe's metadata/macro taxonomy —
already shaped P37's three-tier attributes design (`docs/prd/p37-feature-batch/ATTRIBUTES-RESEARCH.md`).*

Haxe is the **most important prior art Polyglot has**: the same north star (one small typed language →
many targets), twenty years old, technically excellent, still alive — and therefore the best available
evidence for which bets pay off and which scars to avoid. This document compares the two projects
axis by axis and states, for each divergence, *why* Polyglot chose differently. It is deliberately not
a scorecard: Haxe survived where JSIL/SharpKit/Bridge.NET died, and several of Polyglot's choices are
Haxe's choices, adopted with gratitude.

---

## 0. At a glance

| Axis | Haxe | Polyglot |
|---|---|---|
| Born / status | 2005, Nicolas Cannasse (Motion-Twin); Haxe Foundation + Shiro Games; v5 in preview | 2026, solo long-haul craft project; CLI 0.3.x |
| Compiler | OCaml monolith; **targets are code generators compiled into the compiler** | C++20 zero-dep engine; **targets are data-only JSON plugins** (zero compiled-in backends) |
| Targets | JS, HashLink, JVM, C++, CPPia, Lua, Neko, Python, PHP (**C# and Java removed in v5**) | C#, TypeScript, Python, PHP (few by design) |
| Fidelity stance | faithful-where-cheap, native-where-not; divergences in scattered per-target notes; `#if target` forks the source | **faithful-by-default**; short **published relaxation list** (§3.C); refuse-the-unfaithfulable with a diagnostic (§3.B); differential conformance as a CI gate |
| Output stance | build artifact to *run* (debug the `.hx` via source maps); ships a runtime (`Boot`, `HxOverrides`, `Std`) | **source a human reads and commits**; idiomatic per target; zero runtime in output |
| Metaprogramming | compile-time AST **macros as the central bet** | **refused**; fixed compiler-authored lowerings only |
| Operator overloading | abstracts only (zero-cost wrappers) | fixed-named operator methods on types (closed set) |
| Reflection | supported best-effort; DCE silently eats reflected fields | **refused** (§3.B); compile-time metadata only (`Meta`, resolved at transpile time) |
| Primary habitat | whole-app single-target, dominated by gamedev (Heaps/HashLink, OpenFL) | twin libraries: portable logic delivered *into* existing .NET/Angular/Python/PHP projects |

---

## 1. Architecture: compiled-in generators vs data-only plugins

Haxe's compiler is OCaml, and every target is an **in-tree code generator compiled into the binary**
([Wikipedia](https://en.wikipedia.org/wiki/Haxe)). The cost of that choice is now empirical:
**Haxe 5.0-preview.1 removed the C# and Java targets entirely** — changelog: *"java/cs: remove C# and
Java targets (#11551)"* ([Haxe 5.0.0-preview.1](https://haxe.org/download/version/5.0.0-preview.1/)) —
after years in the barely-maintained "Tier 3" of the official target list
([compiler targets](https://haxe.org/documentation/introduction/compiler-targets.html)). A target's
health is gated on core-team OCaml bandwidth, and when that runs out, the target dies.

Two details of the aftermath matter most for Polyglot:

- The community C# reboot, **reflaxe.CSharp**, abandoned the old emitter's approach ("gencommon":
  bend the Haxe AST until it looks C#-shaped), which its maintainers describe as *"assumption hell"*
  where *"the data structure wouldn't accurately match the output"* — and rebuilt around an
  **intermediate typed C# AST fed to a hand-written printer**
  ([reflaxe.CSharp#13](https://github.com/SomeRanDev/reflaxe.CSharp/issues/13),
  [community thread](https://community.haxe.org/t/some-notes-feedback-about-c-target/3009)).
  That is, independently and the hard way, **Polyglot's PRD §4.2 decision**: one high-level typed
  tree IR — not SSA, not a common denominator — specialized per target by a hand-written
  pretty-printer.
- Even so, the candidate for *mainline* Haxe 5 C# support is again an **in-compiler OCaml
  generator**, chosen over the external Reflaxe framework for performance
  ([new C# target thread](https://community.haxe.org/t/a-new-c-target-for-haxe-5/4764)) — the
  monolith's gravity is that strong.

Polyglot's answer to the same maintenance problem is structural: **the engine contains zero target
knowledge**. Every backend — including the first-party four — is one `polyglot-plugin.json`: data
tables interpreted by a bounded, non-Turing-complete DSL, load-validated against an anti-silent-drop
coverage contract, installable without a compiler release (PRD §4.10–4.11;
`docs/plugin-authoring.md`). The rule is absolute enough that the Core is forbidden from *comparing
target names*: every per-target distinction — down to how a constant enum member is spelled — is a
manifest property (`enumMemberOp`, `constArrayOpen/Close`, trait flags like
`expressionOnlyLambdas`). A plugin cannot run code; it can only describe output shapes. Whether this
scales to a community is unproven — Haxe has shipped twenty years of real targets and Polyglot four —
but the failure mode Haxe just demonstrated (a major target dying inside the compiler) is
structurally unavailable here.

## 2. The fidelity contract: inverted defaults

Haxe optimizes for native performance and native interop per target; cross-target sameness is a
best-effort default it will abandon for speed, usually silently. The evidence is broad and
first-party:

- **Int overflow is explicitly undefined.** The manual: *"the Haxe Compiler does not enforce any
  overflow behavior"* — 32-bit wraparound on C++/JVM, silent float-precision loss on JS/PHP past
  2^52. Faithful integers are a cost-bearing opt-in (`haxe.Int32`/`Int64`)
  ([Overflow](https://haxe.org/manual/types-overflow.html)).
- **Strings are the deepest wound.** Targets split between UTF-16, UTF-8, UCS-2, and "binary"
  encodings; `length`/`charCodeAt` return *different numbers for the same source string* on
  different targets, with no diagnostic ([encoding](https://haxe.org/manual/std-String-encoding.html),
  [#1975](https://github.com/HaxeFoundation/haxe/issues/1975)). The Haxe 4 Unicode redesign chose
  UCS-2 over UTF-8 for the targets it controls *because "UTF-8 encoding was too slow"*
  ([Unicode blog](https://haxe.org/blog/unicode/)) — fidelity traded for speed, again.
- **Regex binds each target's native engine**, documented as *"a necessary trade-off to retain a
  certain level of performance"*; full reproducibility requires a third-party pure-Haxe engine
  ([regex](https://haxe.org/manual/std-regex.html), [HRE](https://lib.haxe.org/p/hre/)).
- **Reflection diverges per target and DCE silently eats it** — dead-code elimination *"can
  eliminate types or fields which are only used via reflection"*; the fix is manual `@:keep`
  ([DCE](https://haxe.org/manual/cr-dce.html)). `Dynamic` comparison is *"unspecified and
  platform-specific"* ([binops](https://haxe.org/manual/expression-operators-binops.html)).
- **There is no single published divergence contract.** Differences live in scattered per-target
  notes ([target details](https://haxe.org/manual/target-details.html)), and the sanctioned fix is
  `#if <target>` conditional compilation — which forks the source and voids the portable-code
  guarantee for that path ([conditional compilation](https://haxe.org/manual/lf-condition-compilation.html)).

Polyglot inverts the default. Faithful semantics are the contract (PRD §3.C lists the *complete* set
of relaxations: int masking, i64→BigInt, opt-in strict floats, structural equality — "never relax
silently"); the unfaithfulable is **refused with a diagnostic** (§3.B: threads, runtime reflection,
finalizers, `decimal`, `unsafe`, `dynamic`, expression trees, bit-exact transcendental floats); and
the claim is *enforced*, not aspirational — the differential conformance suite runs every program on
all four targets against a C# oracle and fails CI on any divergence. Where Haxe's user discovers a
divergence by hitting it in production, Polyglot's is either identical, published, or a compile error.

Two honest costs of Polyglot's stance: the supported surface is far smaller (that is the point — the
"portable-logic sweet spot", PRD §1), and the refusal list forecloses whole application classes Haxe
serves happily (anything needing threads or runtime reflection). Haxe also deserves credit where its
own defaults are the more principled ones Polyglot echoes: `/` always yields `Float` on every target;
threads are absent-by-package on JS rather than faked (`sys.thread` + `target.threaded` — a
capability-gate in spirit, PRD §3.E cites it as survivor-pattern prior art); and
`haxe.Exception` (4.1) unified exception semantics across targets much as Polyglot's typed-catch
lowering does.

## 3. The output contract: artifact to run vs source to read

This is the axis where the projects differ most, and it explains nearly everything else.

Haxe's generated code is a **build artifact**. The default JS emit is function/prototype scaffolding
inside an IIFE with compiler temporaries (`_g`, `_g1`) and an injected runtime (`Boot`,
`HxOverrides`, `Std`) — `HxOverrides.substr` exists precisely to correct a JS/Haxe semantic gap in
shipped code ([#4365](https://github.com/HaxeFoundation/haxe/issues/4365)). ES6 class output is
opt-in and still injects `_hx_constructor` shims
([ES6 docs](https://haxe.org/manual/target-javascript-es6.html)); idiomatic ES modules with `.d.ts`
require the third-party, experimental **genes** ([genes](https://github.com/benmerckx/genes)).
Debugging is *designed* to bypass the output: source maps take you back to the `.hx`
([source maps](https://haxe.org/manual/debugging-source-map.html)). All of this is coherent for
Haxe's model — nobody reads the artifact — and precisely wrong for Polyglot's, where the emitted
TypeScript twin lands inside an Angular repo and the C# twin inside a .NET solution, to be read,
reviewed, and consumed by teams who may never see the `.pg` source. Hence Polyglot's contract:
idiomatic-by-construction output, zero runtime in the artifact (semantic gaps are closed in the
emitter or refused), and per-file emission that routes into npm/NuGet projects via pgconfig
`include` rules rather than through a separate hxml/haxelib world.

The direction of interop is likewise inverted. Haxe is strong going **into** targets (fifteen years
of externs, `js.Syntax`, conditional compilation) and weak going **out**: `.d.ts` generation is not
native ([#6040](https://github.com/HaxeFoundation/haxe/issues/6040)), module-format friction is
real ([extern blues](https://community.haxe.org/t/having-the-extern-blues-again/4329)), and the C#
consumption path was retired outright with the v5 target removal. Polyglot is built output-first and
is correspondingly narrower on the inbound side (thin `extern` bindings, `expect`/`actual`, no
ambition of broad platform-API coverage — PRD §4.4: "bind what's used").

In practice Haxe's multi-target story is dominated by **whole-app, single-target** builds (gamedev:
Dead Cells, Northgard, Papers Please — [use cases](https://haxe.org/use-cases/games/)); shared
client/server logic exists but assumes both sides own the Haxe source
([nadako](https://nadako.github.io/rants/posts/2016-07-22_is-haxe-for-you.html)). The twin-library
niche Polyglot targets — readable output consumed by teams who never adopt the source language — is
one Haxe's architecture never tried to occupy.

## 4. Metaprogramming: the macro bet vs the refusal

Haxe's central design bet is **compile-time AST macros** — ordinary Haxe code rewriting the program
at compile time ([macros](https://haxe.org/manual/macro.html)). It buys enormous leverage (DSLs,
frameworks, zero-cost abstractions, even whole targets via Reflaxe) and pays in exactly the
currencies the manual itself concedes: errors surface *"in code they did not explicitly write"*,
compile-order subtleties (reworked again in v5), and dialect fragmentation — any library can make
"Haxe" read differently. Polyglot refuses the entire tier: no user-programmable compile-time
metaprogramming, ever; if a specific transform is genuinely needed it becomes a fixed,
first-party, compiler-authored lowering (the same way constructors and value equality are
synthesized). The reasoning is the readable-output promise plus the scope line: a macro layer is the
maximal form of the surface-area growth that killed the dead cohort, and prior art is unanimous
about its costs (P37 §D.1 surveys Lisp hygiene, Rust proc-macro spans, Scala's deleted-and-rebuilt
macros, Nim's dialect problem).

The **attributes case study** shows both the debt and the improvements. Haxe solved the
"decorators execute on TS/Python but attributes never run on C#/PHP" fault line a decade before
Polyglot hit it, by splitting metadata into categorically different things: inert runtime data
(`@name` — not a class, no constructor, nothing to run), compile-time metadata consumed by the
compiler or macros (`@:name`, `@:build`), and typed native pass-through (`@:strict`). P37's
three-tier taxonomy adopts that split outright. It then fixes Haxe's own documented regrets:
Polyglot attributes are **typed declarations** checked at every attachment (the shape Haxe's
rejected "typed metadata" proposal wanted —
[haxe-evolution #73](https://github.com/HaxeFoundation/haxe-evolution/pull/73) — where Haxe metadata
is stringly-typed with `Dynamic` returns); queries resolve **at transpile time** to inline constants
instead of an always-emitted `__meta__` table (Haxe's documented DCE/bloat interaction); and a
missing per-target binding is a **loud refusal** where Haxe's `@:meta` is silently inert on
non-matching targets.

## 5. Type-system notes

- **Operator overloading**: Haxe confines it to abstracts — one zero-cost mechanism, refused on
  classes ([abstracts](https://haxe.org/manual/types-abstract-operator-overloading.html),
  [#2574](https://github.com/HaxeFoundation/haxe/issues/2574)). Polyglot's closed set of fixed-named
  operator methods is the same discipline in different clothes: both projects refuse open-ended
  operator surface; both make TS-style targets pay in method calls.
- **ADTs + exhaustive matching**: both have them as core features; convergent.
- **Structural vs nominal**: Haxe mixes nominal classes with structural typedefs/anonymous
  structures; Polyglot stays nominal (with checker-enforced interfaces) — smaller surface, fewer
  per-target representation questions.
- **Null safety**: Haxe shipped ~14 years before an opt-in null-safety checker (4.0, 2019); Polyglot
  had native nullable types (`T?`) from the start — the late-vs-built-in contrast is a familiar
  lesson from TypeScript/Kotlin history.
- **Int64**: Haxe pays in a two-word abstract with per-op cost
  ([Int64](https://api.haxe.org/haxe/Int64.html)); Polyglot pays in a published relaxation
  (i64 → JS `BigInt`). Same problem, different honest price.

## 6. Trajectory lessons

Haxe's twenty years compress into four lessons Polyglot's PRD already encodes, now with sharper
evidence:

1. **Survival came from narrowing, not breadth.** Haxe outlived the dead cohort by contracting to
   the vertical where it is genuinely loved (games on Heaps/HashLink), while its original web niche
   was taken by TypeScript — the "typed JS" idea Haxe pioneered
   (["is Haxe alive or dead?"](https://community.haxe.org/t/is-haxe-alive-or-dead/4689)). Polyglot's
   §3 scope contract is the same move made at design time instead of under duress.
2. **Target count is a liability curve, not a feature list.** Every target is a permanent
   maintenance mortgage; Haxe 5 just foreclosed on two. Polyglot's plugin model lowers each target's
   carrying cost but does not repeal the principle — hence four targets, demand-gated growth.
3. **A small core team sets the release clock.** Haxe 3→4 took six years; 4.3→5.0 is still in
   preview. For a solo craft project the mitigation is the same one Haxe found late: make the
   engine boring and the language data external.
4. **The niche must be one someone actually inhabits.** Haxe's whole-app model needed teams to
   *adopt Haxe*; that ceiling defined its size. Polyglot's twin-library model asks nothing of the
   output's consumers — which is either its escape from that ceiling or an unproven bet, and honesty
   requires saying it is currently the latter.

## 7. Summary: adopted, improved, inverted, unavailable

- **Adopted from Haxe** (with thanks): the metadata taxonomy behind the three-tier attributes; the
  capability-gate spirit of `sys.*`/`target.threaded`; operator discipline via one closed mechanism;
  portable std + target packages + on-demand bindings; typed tree IR over SSA; `/`-yields-Float-style
  operator consistency; native-compiler distribution (single zero-dep binary).
- **Improved where Haxe recorded regrets**: typed, checker-enforced attributes (vs stringly
  metadata); pay-only-for-what-you-query metadata (vs always-emitted `__meta__`); loud per-target
  refusals (vs silent inertness); one published relaxation list (vs scattered target notes).
- **Inverted by design**: fidelity default (faithful vs fast), output contract (readable source vs
  runnable artifact), metaprogramming (refusal vs macros), reflection (refused vs best-effort),
  engine/target coupling (data plugins vs compiled-in generators).
- **Unavailable to Polyglot** (Haxe's genuine advantages): twenty years of production hardening; a
  real community and package ecosystem; ten-plus targets including native (C++/HashLink) with
  world-class compile speed via the compilation server; deep inbound-interop machinery. None of
  these are on Polyglot's roadmap at any price — which is the scope line, restated.

---

## Sources

Haxe manual: [history](https://haxe.org/manual/introduction-haxe-history.html) ·
[overflow](https://haxe.org/manual/types-overflow.html) ·
[string encoding](https://haxe.org/manual/std-String-encoding.html) ·
[regex](https://haxe.org/manual/std-regex.html) · [DCE](https://haxe.org/manual/cr-dce.html) ·
[reflection](https://haxe.org/manual/std-reflection.html) ·
[binops](https://haxe.org/manual/expression-operators-binops.html) ·
[threading](https://haxe.org/manual/std-threading.html) ·
[try/catch](https://haxe.org/manual/expression-try-catch.html) ·
[macros](https://haxe.org/manual/macro.html) ·
[abstract operators](https://haxe.org/manual/types-abstract-operator-overloading.html) ·
[externs](https://haxe.org/manual/lf-externs.html) ·
[conditional compilation](https://haxe.org/manual/lf-condition-compilation.html) ·
[target details](https://haxe.org/manual/target-details.html) ·
[JS target](https://haxe.org/manual/target-javascript.html) ·
[ES6 output](https://haxe.org/manual/target-javascript-es6.html) ·
[source maps](https://haxe.org/manual/debugging-source-map.html) ·
[completion server](https://haxe.org/manual/cr-completion-server.html).
Releases/blog: [Haxe 5.0.0-preview.1](https://haxe.org/download/version/5.0.0-preview.1/) ·
[Haxe 4.0.0](https://haxe.org/download/version/4.0.0/) ·
[Haxe 4.1.0](https://haxe.org/blog/haxe-4.1.0-release/) ·
[Unicode in Haxe 4](https://haxe.org/blog/unicode/) ·
[zero-cost abstracts](https://haxe.org/blog/zero-cost-abstracts/) ·
[Shiro Games stack](https://haxe.org/blog/shirogames-stack/) ·
[compiler targets](https://haxe.org/documentation/introduction/compiler-targets.html) ·
[foundation](https://haxe.org/foundation/) · [games use cases](https://haxe.org/use-cases/games/).
Issues/evolution: [#11551 via v5 changelog](https://haxe.org/download/version/5.0.0-preview.1/) ·
[#1975](https://github.com/HaxeFoundation/haxe/issues/1975) ·
[#2382](https://github.com/HaxeFoundation/haxe/issues/2382) ·
[#11613](https://github.com/HaxeFoundation/haxe/issues/11613) ·
[#4365](https://github.com/HaxeFoundation/haxe/issues/4365) ·
[#6040](https://github.com/HaxeFoundation/haxe/issues/6040) ·
[#9327](https://github.com/HaxeFoundation/haxe/issues/9327) ·
[#2574](https://github.com/HaxeFoundation/haxe/issues/2574) ·
[haxe-evolution #73](https://github.com/HaxeFoundation/haxe-evolution/pull/73).
Community/third-party: [reflaxe.CSharp](https://github.com/SomeRanDev/reflaxe.CSharp) ·
[reflaxe.CSharp#13](https://github.com/SomeRanDev/reflaxe.CSharp/issues/13) ·
[new C# target for Haxe 5](https://community.haxe.org/t/a-new-c-target-for-haxe-5/4764) ·
[C# target feedback](https://community.haxe.org/t/some-notes-feedback-about-c-target/3009) ·
[extern blues](https://community.haxe.org/t/having-the-extern-blues-again/4329) ·
["is Haxe alive or dead?"](https://community.haxe.org/t/is-haxe-alive-or-dead/4689) ·
[genes](https://github.com/benmerckx/genes) · [dts2hx](https://github.com/haxiomic/dts2hx) ·
[hxtsdgen](https://github.com/elsassph/hxtsdgen) · [HRE](https://lib.haxe.org/p/hre/) ·
[nadako: is Haxe for you](https://nadako.github.io/rants/posts/2016-07-22_is-haxe-for-you.html) ·
[HN 27571995](https://news.ycombinator.com/item?id=27571995) ·
[HN 26085140](https://news.ycombinator.com/item?id=26085140).
Polyglot anchors: `docs/prd/POLYGLOT_PRD.md` §1, §3.A–F, §4.2–4.4, §4.10–4.11, §5, §9;
`docs/plugin-authoring.md`; `docs/prd/p37-feature-batch/{PRD.md §D, ATTRIBUTES-RESEARCH.md}` (PR #65).
