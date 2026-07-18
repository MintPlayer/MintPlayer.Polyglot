# Real-language source inputs (`@mintplayer/polyglot-source-*`) — investigated & declined (decision record)

> **The ask (maintainer, 2026-07-18):** let developers author in real languages — C#, TypeScript,
> Python, … — instead of `.pg`, packaged as `@mintplayer/polyglot-source-<lang>` npm packages
> symmetric to the target packages, with **source-side capability negotiation**: just as the union of
> used `polyglot-target-*` packages narrows the usable feature surface, the chosen source language
> narrows it further.

- **Status:** 🚫 **Declined for the foreseeable future** (maintainer decision, 2026-07-18, same day as
  the investigation). `.pg` remains the **single authoring surface**; PRD §3.F and the P20 gates stand
  unchanged; **no `.pg` grammar changes** either (see §5). This document distills the investigation so
  the conclusion — and everything worth keeping from it — survives without a live milestone.
- **Provenance:** 5-agent investigation (packaging/seam architecture vs code · C# subset mapping vs
  AST/backends · TS+Python dialect re-litigation · prior art & risk, web-researched · editor
  tooling/LSP vs code). Ancestor: the 2026-07-02 "skins" investigation
  (`docs/design/frontend-skins.md`, PRD §4.12) — that record analyzed familiar *syntax* over `.pg`
  semantics; this one analyzed real-language *dialects* (the language's own semantics, loud refusal
  elsewhere) — a genuinely different framing, hence the re-litigation.

---

## 1. The finding (before the decision)

**Feasible for exactly one language, in exactly one honest shape — and still not worth it now.**

| source language | verdict | one-line reason |
|---|---|---|
| **C#** | technically feasible (would have been gated) | widths/casts/nominal typing map 1:1; the subset ≙ the C# backend's own emission image; **dual-compilable** — every accepted file is also valid Roslyn C# |
| **TypeScript** | dialect conceivable, refusal narrowed (see §4) | skin-era semantic inversions dissolve under TS-native semantics, but fixed-width integers stay inexpressible; **dual-checkable, never dual-runnable** — running the source under node diverges exactly on the §3 faithfulness surface |
| **Python** | refused | the divergence lives in the core operators — `/` is true division, `//` **floors** (−7//2 = −4) where `.pg` truncates (−3): *ban* them and it isn't Python, *accept* them and native-vs-transpiled answers differ (a miscompile-class lie), *honor* them and the IR's arithmetic forks per source language; plus arbitrary-precision `int` has no width spelling and annotations are unenforced |
| anything else | refused by default | every frontend is compiled-in Core work forever; WASM-sandboxed frontends recorded as the only future third-party escape hatch |

The general law the per-language verdicts instantiate: **a language works as a source only if its
designers' semantic decisions embed into `.pg`'s.** Targets are the opposite case — emitted code is
Polyglot's servant and can implement `.pg` semantics on any runtime (the Python target's
`_pg_idiv`/`_pg_irem` preludes exist for exactly this); a *source* file arrives already meaning
something to its native toolchain, and Polyglot may never silently mean something else. C# passes
because `.pg` deliberately follows the C-family mainstream C# defines (truncating division, wrapping
fixed-width ints, dividend-sign `%`); Python fails at that exact layer.

"Write real X" is unkeepable in full for *any* X — the prior-art base rate is decisive:
AssemblyScript (the one thriving TS-subset compiler) formally disclaims being a TS subset in its own
FAQ and pays a permanent "looks like TS but isn't" confusion tax; py2many (literally this ask — real
Python in, many languages out, one maintainer) is best-effort-by-construction with negligible
adoption; NRefactory/SharpDevelop show nobody hand-sustains a full C# parser anymore; and the dead
transpiler generation (JSIL/SharpKit/Bridge.NET) borrowed *full* real-language frontends, which is
precisely how their scope crept.

## 2. Why declined, even for the feasible case

- **Cost:** ~2–3 weeks one-time infrastructure, then a C# frontend at ≈3× the `.pg` parser
  (~2.5–4k lines — quality refusals require *parsing* the refused grammar to say "no LINQ — use
  `for`/`yield`"), ≈5–6 weeks of slices, plus the part with no end date: a yearly C#-version triage
  tax, per-frontend error-recovery/diagnostics upkeep, docs and samples bifurcated across two
  syntaxes — all landing on the one layer the JSON-plugin architecture *cannot* make data-driven, on
  a one-person project.
- **The refusal ambush, now automated:** LLM autocomplete is trained on real C# and would pour
  LINQ/`Task.Run`/attributes into a C#-looking buffer continuously; `.pg`'s deliberately distinct
  surface steers both humans and models away from that cliff (§3.F's scope-line defense, reaffirmed).
- **The need is already substantially met:** `.pg`'s surface is deliberately TS-flavored, the P17
  live preview shows a developer idiomatic C#/TS/Python beside their `.pg` as they type, the P20
  Rosetta docs cover the mapping — and the familiarity gap is smaller than assumed (§5).
- **Polyglot has enough degrees of freedom already** (maintainer's phrasing): the project's leverage
  is targets, tooling, and faithfulness — not a second authoring surface.

## 3. Preserved for any future revisit (the parts of the design worth keeping)

- **Packaging — "symmetric packaging, asymmetric implementation":** `@mintplayer/polyglot-source-<lang>`
  as a data manifest (`kind:"source"`, extensions, `parser: {id, requires}`, expressible-`features`
  list, `diagnosticsVocab`, keywords, docs/samples) flowing through the whole P30 pipeline
  (dependencies → lockfile → SRI cache) but **activating a parser compiled into the CLI**. Native
  dylibs refused (breaks P30's data-only trust model); grammar-as-data refusal *strengthened* (real
  grammars are more ambiguity/recovery-laden than any skin); the `parser.requires` handshake must be
  checked **in code at load** against a `Frontend::version()`, never docs-only.
- **Source-side capability negotiation** (the ask's genuinely novel contribution):
  `usable = expressible(source) ∩ ⋂ coverage(targets)` — §3.E pointed backward; mostly self-enforcing
  at the AST level (a parser that can't express iterators never produces `Yield`), with the manifest
  `features` list for surfacing/wording and a **module-graph-wide** intersection (import-boundary
  refusals, never silent mangled-name consumption).
- **The C# subset design, upgraded vs P20:** the subset ≙ the C# backend's **emission image**
  (whatever Polyglot writes in C#, a frontend can read back — a mechanical **fixpoint gate**: emit →
  re-ingest → emit → byte-identical); sealed-record-hierarchy → `union` *recognition* with
  checker-owned exhaustiveness + a required `_ => throw new UnreachableException()` bridge arm
  (overturns P20's "too lossy"); zero invented syntax remains, so **dual-compilable** is achievable,
  `csc`-enforced in CI; extension `.pgcs`, never bare `.cs` (two language servers on one file is
  unresolvable for C# — no tsconfig-equivalent narrows Roslyn).
- **The TS clause, restated honestly:** what is *permanently* refused is any TS-facing surface sold
  as runnable TypeScript or carrying `.pg` keyword semantics. An explicitly-subset dialect
  (TS-native `const`/`let`, `as` banned, discriminated unions recognized, `noLib` + own `.d.ts`
  turning tsc itself into the expectation-trap enforcer) fails on cost/benefit and the
  dual-runnable false promise — not on soundness. Recorded so it isn't mis-cited later.
- **Tooling facts, verified in code:** the LSP transfers to any frontend stamping accurate
  `SourcePos` (SemanticModel is a sema by-product); what doesn't transfer: TextMate grammar,
  completion keywords (`main.cpp:1277`), `fmt` (`main.cpp:1290` would silently rewrite a non-pg file
  into `.pg` — must be gated per-frontend before any second frontend exists), and per-parser panic
  recovery. TextMate grammars are static VS Code contributions — one extension bundles all official
  grammars; an npm package can't inject one.

## 4. Contract status

PRD **§3.F is unchanged**: one authoring syntax, reading aids + (demand-gated) one-way `convert`
supported, TS skin refused, C# skin gated behind observed demand — with this record as the current
statement of *why*, superseding the skin-era rationale where they differ (§3 above). PLAN §P20's
gates stand; its slices were **not** promoted. If real external demand for authoring-in-C# ever
appears, the honest first probe remains `polyglot convert` (C# subset → `.pg`, one-way, loud
failures) — and this record plus `docs/design/frontend-skins.md` are the design inputs to reopen.

## 5. Companion decision — the `.pg` statement syntax stays as-is (2026-07-18)

Raised in the same discussion: `if`/`for`/`match` headers lack C-family parentheses and this might
hurt learnability. Findings: braces are **mandatory** in `.pg` (`if n <= 1 { return 1 }` — the
grammar that removes the dangling-else/`goto fail` bug class C's optional braces invite), and
**C-style parenthesized conditions already parse today** — `if (x > 3) { … }` / `while (x > 0) { … }`
check clean (verified against the CLI, 2026-07-18), since parens are ordinary expression grouping.
The paren-less canonical style is the modern mainstream school (Go, Rust, and Swift — Objective-C's
own successor), not an invention. **Decision: no grammar or canonical-style change**; both spellings
remain accepted, the samples/formatter keep the paren-less form.
