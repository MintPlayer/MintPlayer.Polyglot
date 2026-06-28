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
VS 18 "Insiders" MSBuild (no VS 2022 on this box; it resolves toolset v143 to MSVC 14.44 — see CLAUDE.md
for the exact paths). `MintPlayer.Polyglot.Cli.exe --version` prints `0.0.1`; `...Tests.exe` reports
all-pass.

## P1 — Language design v0.1 🟡 drafted, pending review (2026-06-28)
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
*Gate (open — needs human review):* confirm the samples express real logic cleanly and the FruitCake
sketch validates the surface, then lock v0.1 before P2.

## P2 — Walking skeleton (MVP) ★ thinnest end-to-end slice
Take a *minimal* language subset — `fn`, `i32`/`f64` + arithmetic, `let`/`var`, `if`/`while`, function
calls, `print` — **all the way through** the pipeline: a minimal trivia-bearing lexer → recursive-descent
parser → minimal typer → the single high-level typed tree IR (§4.2) → **hand-written C# *and* TS
pretty-printers** (no Roslyn/ts-morph — §4.3). `polyglot build foo.pg` emits `foo.cs` + `foo.ts`. This
front-loads the project's biggest architectural bet — that **one high-level IR serves both targets** — and
stands up the crown-jewel **differential conformance test** on day one instead of at P5. Later milestones
*widen* each pass from this baseline.
*Gate:* a tiny `.pg` program emits C# that compiles under `dotnet` and TS that type-checks under `tsc`,
both run, and a **differential conformance test asserts identical stdout** — green in CI.

## P3 — Full front-end (lexer + parser)
Widen the MVP's front-end to the entire P1 grammar. Trivia-bearing lexer (keeps comments/whitespace for
readable output later) → recursive-descent parser → AST with source positions on every node; parser error
recovery + good diagnostics.
*Gate:* every P1 sample round-trips source → AST → re-printed source (a parser fidelity test); malformed
input yields clear, positioned errors.

## P4 — Full semantics + typed IR
Widen the MVP's minimal typer to the full **minimal static type system** (enough for the §3.A surface:
nominal types, generics, overload resolution, nullability, pattern-match exhaustiveness) + name/scope
resolution. Lower the full AST into the single high-level typed tree IR (§4.2), folding in desugaring. All
diagnostics emitted here, on a near-source form.
*Gate:* type errors are reported with positions; valid samples produce a typed IR dump that round-trips;
overload/closure/generic resolution covered by unit tests.

## P5 — Backends to the full §3.A surface
Widen both hand-written pretty-printers from the MVP subset to the **entire supported surface**: records,
enums, unions + pattern matching, iterators, exceptions, `using`/disposal, extension methods, operators,
properties/indexers, closures — idiomatic in each target. Golden-output baselines checked in for **both**
targets; the differential conformance suite (stood up at P2) grows to cover the surface.
*Gate:* P1 samples emit C# that compiles under `dotnet build` and TS that type-checks under `tsc` + runs
under Node, both with expected output; golden baselines green; the differential suite passes.

## P6 — Faithfulness pass
Implement the §3.C relaxations *as documented behaviour*: int32/uint masking (`|0`/`>>>0`/`Math.imul`),
opt-in `Math.fround` strict floats, `BigInt` for int64, structural equality/hashing, null/undefined
normalization. Enforce the §3.B **refusals** with clear, actionable compiler errors (not miscompiles).
*Gate:* a numeric conformance suite passes within tolerance across both targets; every refused feature has
a refusal test asserting the diagnostic; the relaxation list is written up in the spec.

## P7 — Std core + expect/actual + FFI (the three plugin mechanisms, as first-party code)
A minimal portable standard library in `.pg` (math, basic collections, iterators) compiled to both
targets. The **target-gated `expect`/`actual`** mechanism (portable core forbidden from touching platform
APIs; per-target `actual` impls) and an `extern`/inline-target **FFI hatch**. This is where the three
mechanisms of the plugin architecture — *binding*, *replacement*, *capability* — are proven **as
first-party code** behind a backend interface designed to become the plugin API (see the plugin design
note, below).
*Gate:* a program using a portable std API + one `expect`/`actual` capability (e.g. current time) builds
and runs identically on both targets; the portable core cannot reference `document`/`System.*` (compiler-
enforced, with a test proving the rejection).

## P8 — Dogfood: the FruitCake physics ★ north star
Express the MintPlayer.AI FruitCake circle-physics solver in `.pg`. Generate `FruitCakeWorld`-equivalent
C# and `fruit-cake-physics`-equivalent TS. Wire the **differential conformance test** against the existing
hand-ported twins (same seed + scripted drops → settled board within tolerance + identical merge counts;
per §3.D, tolerance/behavioural, not bit-exact).
*Gate:* generated C# and generated TS each match their hand-ported counterpart's behaviour on the
conformance suite. At this point Polyglot has *earned* the right to own that physics; the hand-ports can
retire while the conformance test stays (now guarding the generator).

## P9 — Declarative backend engine + DSL
The backends, generalized. *Extract* a declarative backend format from the two **native** C#/TS backends
(P4/P5) — a rule/template per IR node (context-aware: precedence, expr-vs-stmt position), the std-type
mapping, operator/keyword tables, naming/import rules, and the build-project scaffold. Build the core's
**declarative emit engine** that interprets a spec, and **re-express C# and TS as declarative specs**.
Critically, the DSL is extracted from working backends, never guessed (the §4.3 "design it twice"
discipline applied to the format itself); a **local full-power plugin tier** covers what the DSL can't yet
express. See **[`../design/plugins-and-targets.md`](../design/plugins-and-targets.md)** §4/§7.
*Gate:* C#/TS emitted via the declarative specs match the native backends' golden output byte-for-byte.

## P10 — Plugin distribution + ecosystem (the endpoint of §4.4)
The downloadable, declarative plugin system: a **workspace config (`pgconfig.json`)** declaring target
*environments* (desktop/web/mobile/…) + plugins+versions; **download → shared cache → verify → lockfile**
(declarative data only — no executable code fetched; integrity-pinned, zip-slip-safe); **availability
resolution** by target + environment (off-target use is a compile error, never a miscompile); and
**build-dependency threading** (a plugin declares the NuGet `PackageReference`s/SDK or npm deps its output
needs; the core emits a buildable project including them). Trust model + open decisions in the design note.
*Gate:* adding a **downloaded declarative Python backend** *and* a target-scoped binding plugin (e.g.
WinForms, with its PackageReferences) requires **no core change** — only `pgconfig.json` + downloads; a program
using them emits a buildable project, and wrong-target/-environment use is rejected with a clear diagnostic.

## Stretch (unordered, post-P10)
- **Further targets** as downloadable declarative backends (the IR is target-neutral by design).
- **Source maps:** thread positions through every pass for debuggable JS output; decide the C# debug story.
- **LSP:** build the frontend as a reusable library; make the CLI and a future language server thin clients.
- **Binding auto-generation:** shape-only bindings from `.d.ts` / .NET metadata / WebIDL + hand overrides
  (feeds the plugin ecosystem; produces declarative data at authoring time).
- **Plugin registry & signing:** distribution/versioning infrastructure + signature trust for downloads.
