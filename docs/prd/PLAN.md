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

## P2 — Front-end (lexer + parser)
Trivia-bearing lexer (keeps comments/whitespace for readable output later) → recursive-descent parser →
AST with source positions on every node. Parser error recovery + good diagnostics.
*Gate:* every P1 sample round-trips source → AST → re-printed source (a parser fidelity test); malformed
input yields clear, positioned errors.

## P3 — Semantic analysis + typed IR
Name/scope resolution and a **minimal static type system** (enough for the §3.A surface: nominal types,
generics, overload resolution, nullability). Lower the AST into the **single high-level typed tree IR**
(§4.2), folding in desugaring. All diagnostics emitted here, on a near-source form.
*Gate:* type errors are reported with positions; valid samples produce a typed IR dump that round-trips;
overload/closure/generic resolution covered by unit tests.

## P4 — C# backend
Hand-written IR → C# pretty-printer (no Roslyn — §4.3). Emit idiomatic C# for functions, arithmetic,
control flow, records/structs, enums, pattern matching, exceptions, `using`, iterators.
*Gate:* P1 samples emit C# that compiles under `dotnet build` and produces the expected output; golden-
output baselines checked in.

## P5 — TypeScript backend
Hand-written IR → TS pretty-printer. Same surface as P4. Stand up the **first differential test**: a
sample emitted to both targets, run on both, results compared.
*Gate:* P1 samples emit TS that type-checks under `tsc` and runs under Node with expected output; the
first differential conformance test is green.

## P6 — Faithfulness pass
Implement the §3.C relaxations *as documented behaviour*: int32/uint masking (`|0`/`>>>0`/`Math.imul`),
opt-in `Math.fround` strict floats, `BigInt` for int64, structural equality/hashing, null/undefined
normalization. Enforce the §3.B **refusals** with clear, actionable compiler errors (not miscompiles).
*Gate:* a numeric conformance suite passes within tolerance across both targets; every refused feature has
a refusal test asserting the diagnostic; the relaxation list is written up in the spec.

## P7 — Std core + expect/actual + FFI
A minimal portable standard library in `.pg` (math, basic collections, iterators) compiled to both
targets. The **target-gated `expect`/`actual`** mechanism (portable core forbidden from touching platform
APIs; per-target `actual` impls) and an `extern`/inline-target **FFI hatch**.
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

## Stretch (unordered, post-P8)
- **More targets:** Python and/or C++ backends (the IR is target-neutral by design).
- **Source maps:** thread positions through every pass for debuggable JS output; decide the C# debug story.
- **LSP:** build the frontend as a reusable library; make the CLI and a future language server thin clients.
- **Binding auto-generation:** shape-only bindings from `.d.ts` / .NET metadata / WebIDL + hand overrides.
