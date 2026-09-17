# P38 — Origin attribution — implementation plan (dependency-ordered slices)

> Design contract: `PRD.md`. Investigation evidence: `ANALYSIS.md`. Issue: #69.
> Design decisions **D1–D8** (maintainer review, 2026-09-17) are folded into both documents; the decision
> log and the cross-decision conflicts it surfaced are at the bottom of this file.

**Build discipline.** Implement ALL slices first, then build + run the full gate **ONCE** at the end
(`pwsh scripts/build-and-test.ps1`, or the `/build-and-test` skill). The mid-flight ceiling is
`-Tier fast` or a bare unit-test exe run — never the full gate per slice, never speculative extra legs.
Commit per slice is fine; it is the *test runs* that batch. Slice 0 (spikes) is the one exception: its
whole purpose is to run something early and cheaply.

**One PR.** Both sinks, the `compile()`/diagnostics root-cause fix, the manifest change, the tripwire fix,
the MSBuild property, docs and tests land together.

---

## Slice 0 — spikes (throwaway, time-boxed)

Run **SP1, SP2, SP3** now; **SP4** before slice 8 (it validates, it does not gate — D1). **SP5 moves to
after slice 5** — it measures the *disabled* path's cost, which cannot exist before the feature does;
listing it in slice 0 was a sequencing error in the first draft of this plan. Record each outcome in the
**Log** below — a spike that fails changes the design per PRD §5, it does not get worked around.

**Status: SP1, SP2, SP3 are DONE (2026-09-17) — see the Log.** All three produced design changes; SP2
overturned acceptance criterion A8.

- **SP1** — coverlet against real emitter output, **and** the path-form decision (PRD §4.F). Blocks
  everything; a total attribution failure means stop and ask.
- **SP2** — `writeDedup` / shared-prelude under absolute paths. Expected to be a non-event; it exists
  because an earlier draft asserted the opposite and was wrong (see Decision log, D2).
- **SP3** — Roslyn's acceptance of `#line` in every position the emitter can place it, including a class
  with field initializers. Produces the authoritative "must be `hidden`" list that slice 5 implements.
- **SP5** — disabled-path cost.

**Acceptance:** four recorded outcomes; PRD §4 amended in place where a spike contradicts it.

---

## Slice 1 — a real, canonicalized `SourceMap` on the `compile()` path (blocking prerequisite)

Root cause of PRD §4.C. No behaviour change to emitted code.

1. `polyglot.hpp:72-77` — `EmitResult` gains `SourceMap sources;`.
2. `polyglot.hpp:131-132` — `compile()` gains a **defaulted** `entryPath` parameter (mirrors `analyze`), so
   existing callers keep compiling.
3. `compiler.cpp:629/639/646` — stamp the entry (preserving **entry = fileId 1**), pass the id to `lex`,
   pass `&result.sources` instead of `nullptr`. Everything downstream already takes `SourceMap*`.
4. **`SourceMap::add()` canonicalizes** to an absolute, forward-slashed path (D2 hardening 1). Logical std
   names (`std.io`) are not paths — leave them alone. This also closes the latent LSP bug where a relative
   canonical path silently kills cross-file go-to-definition (`main.cpp:1085-1094`).
5. Pass the real path at all four call sites: `emitOne` (`main.cpp:225`), `watchBuildOnce` (`:374`),
   `runCheck` (`:735`), LSP preview (`:1043`, via `uriToPath`).
6. **Same root cause, in scope (D4):** `reportDiagnostics` (`main.cpp:149-154`) and `emitDiagAt`
   (`:305-308`) stop hardcoding the entry path — resolve through the map. `diagnosticsToJson` (`:673-684`)
   gains a file field. Diagnostics print the **resolver's canonical path**; the directive's path form is
   independent, so the VS Code problem matcher's `file:line:col` shape does not move.

**Acceptance:** a diagnostic raised inside an imported module prints that module's path, not the entry's;
the LSP `fileId != 1` filter (`main.cpp:1054`) still publishes exactly the open file's diagnostics;
`tests/refusals`' 17 pinned `<path>:<line>:<col>` fixtures reviewed for churn; unit test in the style of
`tests_main.cpp:842-874`.

---

## Slice 2 — `SourcePos` on the three executable declarations

PRD §4.D (D3). Narrowed from 17 structs to the declarations whose emitted code can run.

1. Add `SourcePos pos;` to **`ClassField`, `Global`, `Method`** (`ir.hpp`). Field/global initializers are IL
   in the (static) constructor; `Method` gives Roslyn's body-brace sequence point a real `.pg` line, so
   method entry becomes a numerator contribution instead of being discarded.
2. Populate in `lower.cpp` from the AST's `pos`/`namePos` (`ANALYSIS.md` §2.1 has the site table).
3. **`ir::dump()` (`src/ir.cpp`) must not print it** — otherwise every golden IR dump changes.
4. Leave the other 14 structs alone; their lines are non-executable and stay `hidden`. Record
   `ast::ExtensionDecl`'s missing `namePos` (`ast.hpp:300-312`); don't fix it.

**Acceptance:** golden IR dumps unchanged; a unit test asserts a lowered method and a field initializer
carry their AST positions; all four plugins load and emit identically (no plugin references `pos`).

---

## Slice 3 — `originMapping` in the manifest (data, not target names)

PRD §4.B. No emitter behaviour yet.

1. `backend.cpp buildBackend` (~`:325`, beside `crossDirImports`): read the object; validate `style`
   against the **closed enum** with a `blockStyle`-style load error (`backend_spec_json.cpp:50-57` is the
   model); absent key ⇒ default-constructed, no error.
2. `LoadedBackend` member + accessor (`backend.cpp:46-98`); `virtual` on `Backend` (`backend.hpp:108-112`).
3. Generalize `substX` (`backend_spec.hpp:180-215`) to the two-hole `$n`/`$f` form.
4. If any part lands in `BackendSpec`, add it to **both** `loadBackendSpec` and `backendSpecToJson`
   (`backend_spec_json.cpp:129-251`) — a one-sided add silently loses the field in round-trip.
5. Manifests: `plugins/csharp` (`style: directive`), `plugins/typescript` (`style: sourceMapV3`). Python
   and PHP **untouched**.
6. Docs: a row in `docs/plugin-authoring.md:31-42` and the key table at `:83-114`.

**Acceptance:** a flag-parity unit test in the `tests_main.cpp:2936` style — C#/TS declare it, Python/PHP
don't, an unknown `style` **fails the load**, an absent key is a clean no-op. No `kCoverage` change, no
new capability key, no new rule.

---

## Slice 4 — the option reaches the emitter, and refuses when nothing supports it

PRD §4.G + §4.B.1 — copy `--access` end to end.

1. `LibConfig` field (`polyglot.hpp:113-117`); `PgConfig` field + read (`pgconfig.hpp:79`, `:101`).
   **Retain provenance** (CLI flag vs config key) for the diagnostic.
2. `--line-directives` in `runBuild`'s parse loop (`main.cpp:584-585`) — note `:588-590` hard-refuses
   unknown options; update `printUsage` (`:54-79`); merge with config so the **flag wins** (`:491-495`).
3. Carry on `ir::Module` (set at `compiler.cpp:677` and `:716`), then into `EmitterHooks`
   (`emitter_base.cpp:1569`, struct `emitter_base.hpp:136-140`).
4. Mirror into `watchBuildOnce`'s `LibConfig` (`main.cpp:346-347`) — which today forgets `access`; do not
   repeat that gap.
5. **Refusal (D6):** if **no** target in the resolved set declares `originMapping`, refuse. Resolution is
   **per input group** (`runBuild` groups by nearest `pgconfig.json`, `main.cpp:626-634`), so the message
   names the group, the resolved targets, which of them *can* honour the flag, and where the setting came
   from. If at least one supports it, proceed and leave the others unannotated.
6. Emitter state next to `out_`/`indent_` (`emitter_base.hpp:350-351`): `curPos_`, `posValid_`,
   `suppressDirectives_`, plus the resolved path table from slice 1.

**Acceptance:** A9 and A13. With the flag off, `-Tier fast` is byte-identical.

---

## Slice 5 — emit the directives (the invariant)

PRD §4.A / §4.E. The behavioural heart. SP3's output is the authoritative `hidden` list.

1. Set `curPos_`/`posValid_` in `emitStmt` (`emitter_base.cpp:1683`) — `s.pos` is in hand for every kind,
   including the five rule-driven paths (`ForStmt`/`TryStmt`/`IndexAssign`/`LocalFunc` at `:1815-1836`
   and `TupleLet` at `:1696-1706`, which all build their `StmtCtx` from the same `s`). Add the three decl
   positions from slice 2 in `runDeclRule` (`:1370-1451`).
2. Emit in `line()` (`:1598`) — the single chokepoint. Directives write at **column 0**, bypassing the
   `indent_*4` prefix (manifest `column`). Paths are absolute and forward-slashed (PRD §4.F).
3. `hidden` for scaffolding: `openBlock`'s `{` (`:1611-1615`), `closeBlock`'s `}` (`:1619`), blank
   separators (`{"line": ""}`), and non-executable declaration lines. **Exception, proven by SP1:** a
   **method body's opening brace carries the method's position** — that is what makes method entry a
   numerator contribution (PRD §4.D). SP3 found no position Roslyn rejects, so this list is about phantoms
   only.
   **Also `hidden`: any fileId whose canon is not an on-disk path.** Std/lib modules are real `.pg`
   sources but `SourceMap` stores them under logical names (`std.io`, `std.core`), and SP1 confirmed their
   emitted helpers land in the *same* output file as user code. A directive naming `std.io` would claim a
   document that cannot be resolved — and would fail A12. This rule subsumes the synthesized-prelude case.
4. **Split multi-line strings** reaching `line()` on `\n` and emit line-by-line, each with its own
   directive — `module.attrImportsBlock` (`:1155-1161`) is the confirmed live case; `attrLines`,
   `ir::Extern` code and FFI `Bound` templates are the user-reachable ones. Fallback: whole-string `hidden`.
5. **`suppressDirectives_ = true` inside `inlineBlock`** (`:1643-1655`) — mandatory; a directive spliced
   mid-expression breaks compilation. C# block lambdas always take this path.
6. Wrap the post-walk prelude prepend (`:1594`) in `hidden` — it never passes `line()`.
7. **Liveness assertion (D2 hardening 3, A12):** every path emitted in a directive must exist on disk.

**Acceptance:** A2, A5, A12 — a placement test parses emitted C# and asserts every line is preceded by
exactly one directive; a drift fixture (multi-line `if` with braces) reports **only** the real `.pg` lines;
emitted C# compiles under the conformance oracle.

---

## Slice 6 — the write path

PRD §4.F, informed by SP2.

1. Synthesized `ModuleFile`s (empty `sourcePath`, incl. the C# shared prelude at `compiler.cpp:733`) emit
   `hidden`-only — they have no `.pg` origin to claim.
2. **Handle the dedup regression SP2 found** (PRD §4.F.1). Two *distinct* `.pg` files that emit
   byte-identical C# to the same output path collapse silently today (measured: `rootA/p.pg` + `rootB/p.pg`
   → one `out/p.cs`). With directives on they differ, so `writeDedup` (`main.cpp:174-177`) hard-errors and
   a build that works today fails. Do **not** strip directives from the dedup key — that would collapse two
   origins and silently lie about which source produced the output. Instead, extend the existing
   `conflicting output for …` diagnostic: when the flag is on, name it as the cause and point at pgconfig
   `include` output rules, which already exist to route colliding basenames apart. Mirror in the watch
   writer (`:384-403`) — a separate, duplicated implementation.
3. **Make `scripts/verify-coverage-paths.ps1` honest about direction** (D2 hardening 2) — *without*
   loosening it. It implements only `tracked.EndsWith("/$report")` while its docstring at `:39-48` claims
   "or vice versa". Whether the server accepts the longer form is **an inference, not verified**, and the
   script exists to turn the server's silent drops into loud failures — so loosening it now would defeat
   it exactly where it matters. Surface a longer-path match as a distinct warning naming both paths; fix
   the docstring so script and documentation agree; promote it to an accepted match only after SP1 or the
   consumer's spike S6 (`COVERAGE_90_PRD.md:236`) confirms the server's behaviour.
4. *Optional, only if free:* retire `compiler.cpp:673`'s `target.name() == "csharp"` into a
   `sharesPreludeFile` trait flag (PRD §8). Otherwise record and move on.

**Acceptance:** A8 as rewritten — with the flag **on**, the colliding-basename build fails with the
*explanatory* diagnostic (not the bare one), and never silently picks an origin; with the flag **off** the
collapse is byte-for-byte as today; the shared prelude collapses either way; a second identical build
rewrites nothing; the tripwire reports the match direction and still rejects a genuinely untracked path.

---

## Slice 7 — MSBuild, docs, and the flag-on gate witnesses

1. `<PolyglotLineDirectives>` defaulted in `MintPlayer.Polyglot.MSBuild.props`; `_PolyglotLineDirectivesArg`
   appended inside `PolyglotTranspile`'s `<PropertyGroup>` and to the `<Exec Command=…>`, exactly like
   `_PolyglotAccessArg`. Property-gated — the targets deliberately pass no language flag.
2. Docs: `README.md` flag list + the pgconfig block (`:127-141`); the annotated
   `editors/vscode/testbench/pgconfig.json`; a row in the three-instrument coverage table
   (`CLAUDE.md:71`, `README.md:216`) — this is a fourth answer to "which `.pg` lines ran downstream".
3. **A10 — the standing witnesses.** Extend the conformance leg with a **curated subset (~8–10 programs)
   run flag-on**, curated from the emitter's *irregular* paths, not from what looks representative:
   `inlineBlock`'d block lambdas, `TryStmt`/`ForStmt` rule arms, iterators, the multi-line
   `attrImportsBlock`, a class with field initializers, and a multi-file closure (`programs/modular/geom/`).
   Assert placement, that the C# compiles, and that stdout still matches. The **same subset validates the
   emitted `.ts.map`** (decode + `sourcesContent` present), and `tests/library/run-library.ps1` (tsc strict)
   confirms the footer doesn't break compilation.
4. `tests/msbuild/run-nuget.ps1` gains a property-on case.

**Acceptance:** `-Tier fast` green; both flag-on witnesses green; docs mention the flag everywhere the
other flags are mentioned.

---

## Slice 8 — the TypeScript v3 source map

Unconditional (D1). Run **SP4** first as validation — it informs what the docs promise about downstream
chaining; it does not gate this slice.

1. Widen `ModuleFile`/`EmitResult` (`polyglot.hpp:61-77`) with a sidecar slot; widen `Backend::emit`
   (`backend.hpp:71`) or add a companion virtual, since it returns a bare `std::string` today.
2. Base64-VLQ encoder (~30 lines) + the existing `json.hpp`/`json.cpp` writer. **No new dependency.**
3. Record `(outputLine → SourcePos)` pairs in `line()` when `style == sourceMapV3` — the same hook, a
   different sink.
4. **`sources` are computed relative to the map's own location, and the map embeds `sourcesContent`**
   (PRD §4.F). `sourcesContent` is the robust half: the `include` rules route the `.ts` into a different
   tree from the `.pg` (verified in the consumer's `pgconfig.json`), so inline source removes path
   resolution from the equation entirely; `sources` remain as a fallback for tools that prefer disk lookup.
5. Sidecar extension handling in `resolveClosureOutputs` (`pgconfig.hpp:190-238`, which appends exactly one
   `ext` today), `emitOne` (`main.cpp:221-243`), `writeDedup`, and the watch writer.
6. `//# sourceMappingURL=` footer from `originMapping.footer` — TS's `Program` rule is a `seq` with no
   footer slot, so either a tail element or a Core-side append.
7. `_PolyglotAddGenerated` globs `*.cs`, so a `.map` is not swept into `@(Compile)`/`@(FileWrites)` —
   confirm nothing else needs it registered.

**Acceptance:** A11 — valid v3 JSON decoding to correct `.pg` line attributions, `sourcesContent` present;
`tests/library/run-library.ps1` green; node still runs the `.ts` twins unchanged.

---

## Slice 9 — record it, and fix the always-on context

1. `docs/prd/PLAN.md` — a `## P38 — …` milestone entry in the house shape: status line with provenance +
   PRD path + agent count, bold-lead bullets per workstream, a **Built (date, PR#, N commits)** paragraph,
   an **Acceptance coverage** paragraph, and **Deferred follow-ups**.
2. Strike/move the Stretch bullet at `docs/prd/PLAN.md:2386` ("Source maps: … decide the C# debug story")
   — this PRD decides it.
3. `POLYGLOT_PRD.md` — §6 roadmap bullet; reconcile `:234`, `:610`, `:1532`.
4. **`CLAUDE.md` status block (D7 + D8).** It is six minor versions stale — it claims CLI/NuGet 0.3.2 while
   `git tag` says v0.9.10 and the consumer pins 0.9.9. Everything in-tree is `0.0.0-dev`, stamped at release
   by `release.yml` / `publish-plugins.yml` / `publish-vscode.yml`, and lockstep is already enforced in code
   (`main.cpp`, `pluginresolve.hpp`). **Replace the numbers with the lockstep rule plus the source of
   truth** rather than re-typing today's values, which would simply rot again. Then audit the narrative
   items — the "In flight / gated" list (P23, P22 tail, P16d, P20) — from the repo and tags, and ask the
   maintainer about anything resting on an interactive step performed outside the repo (PRD Q3).
5. Append the **As-built notes** section here for every deviation decided during implementation.

---

## Acceptance matrix (criterion → instrument)

| # | Criterion | Instrument |
|---|---|---|
| A1 | Flag off ⇒ bytes unchanged | full gate: `tests_main.cpp` substrings + raw strings, all 114 conformance programs |
| A2 | Every line carries one directive | placement test (parse emitted C#), slice 5 |
| A3 | Emitted C# compiles + runs identically, flag on | conformance `csc` oracle + stdout compare, flag-on subset |
| A4 | Coverage reports `.pg` path/lines | SP1, then the flag-on leg's verification |
| A5 | No phantom lines | drift fixture (multi-line `if`), slice 5 |
| A6 | Multi-file attributes per-file | `programs/modular/geom/` in the flag-on subset (needs slice 1) |
| A7 | Manifest absent / unknown-style behaviour | flag-parity unit test, slice 3 |
| A8 | `writeDedup` still collapses; tripwire fixed | SP2 + slice 6 multi-root build |
| A9 | Flag / pgconfig / MSBuild all reach the emitter | `tests/cli/run-cli-smoke.ps1`, `tests/msbuild/run-nuget.ps1` |
| A10 | Live flag-on witnesses, both sinks | curated conformance subset + `tests/library/run-library.ps1`, slice 7 |
| A11 | v3 map valid, decodes, carries `sourcesContent` | slice 8 |
| A12 | Every directive path exists on disk | liveness assertion, slice 5 |
| A13 | Refuses only when nothing supports it | slice 4, cli-smoke |

## Risks

- **R1 — the probe was not this emitter's output.** Mitigated by SP1 as slice 0's blocking gate.
- **R2 — the path form is a bet on downstream resolvers.** Absolute has evidence at every hop and is
  build-stable; the fallback (PRD §4.F) rebases in the *report*, never in the emitted directive. SP1 decides.
- **R3 — no golden-refresh switch exists** anywhere in `scripts/`/`tests/`. Any accidental churn is
  hand-edited. Mitigated by A1 being non-negotiable and the default being off.
- **R4 — LSP diagnostics silently blank** if entry ≠ fileId 1 after slice 1 (`main.cpp:1054`). Mitigated by
  an explicit invariant test.
- **R5 — `#line` in an unexpected position fails to parse.** Mitigated by SP3 producing the `hidden` list
  before slice 5 is written.
- **R6 — directive volume slows `csc`/coverlet** on a real-size output (PRD Q1). Measured in SP1; fallback
  recorded there.
- **R7 — the curated subset misses an irregular path.** The dangerous constructs are exactly the irregular
  ones, so curate from the emitter's shape (slice 7 item 3), not from a representative sample.
- **R8 — platform-forked code.** Path canonicalization (slice 1) is the likely `#ifdef _WIN32`/POSIX spot.
  If any slice touches forked code, one POSIX compile+unit run (WSL cmake, or the PR's Linux check) is
  **part of the required end gate**, not an extra leg.

## Decision log (maintainer review, 2026-09-17)

| D | Decision | Why it mattered |
|---|---|---|
| **D1** | Phase 2 is **not** gated on a spike; the TS map builds unconditionally | The consumer owns the chain risk as its own spike S5 and its fallback *requires* the map to exist — gating our emission on their spike deadlocks both repos |
| **D2** | **Absolute** directive paths, **map-relative + `sourcesContent`** for the map, plus three hardening measures | Reversed an earlier "repo-relative" answer after the argument supporting it turned out to be wrong (see below) |
| **D3** | `SourcePos` on **3** executable decl structs, not 17 | `hidden` removes a line from the report rather than marking it uncovered, and a directive only yields an entry where there is IL |
| **D4** | The imported-module diagnostics fix rides along in slice 1 | Same root cause as the `SourceMap` threading; one-PR rule |
| **D5** | A10's witness is a **curated** conformance subset, curated from the emitter's irregular paths | A representative sample would miss exactly the constructs that can emit an illegal directive |
| **D6** | Refuse only when **no** selected target declares `originMapping`; clear, actionable diagnostics | Loud-failure culture, without breaking the mixed-target build that motivates the feature |
| **D7** | Full `CLAUDE.md` status-block audit; NuGet and exe versions are lockstep | The block is six minor versions stale and steers every session |
| **D8** | State the lockstep **rule** and the source of truth, not the numbers | The block drifted because it duplicated a fact it does not own |

### Three cross-decision conflicts the review surfaced

1. **An error in this PRD's own §4.F.** The first draft claimed absolute `#line` paths break `writeDedup`'s
   content-equality collapse. They do not — `writeDedup` compares content within one build on one machine,
   and a `.pg` file has one absolute path however many roots import it. If anything, *relative* paths are
   the ones that can diverge across roots. That wrong claim had been used as an argument for choosing the
   relative form; removing it flipped the analysis (D2).
2. **D1 × path form.** A v3 map's `sources` resolve relative to the map file, and the consumer's `include`
   rules route the `.ts` into a different tree from the `.pg` (verified in its `pgconfig.json`). One global
   path form cannot serve both sinks — hence per-sink forms, and `sourcesContent` to remove resolution from
   the TS side entirely.
3. **D1 × D5.** With phase 2 ungated, the map ships unconditionally — but the witness was C#-only, behind
   the `csc` oracle, so nothing would have observed the map. A10 grew a TS half.

Also noted: **the coverage tripwire we steered by disagrees with its own docstring.**
`verify-coverage-paths.ps1:39-48` documents suffix matching "or vice versa" but implements only one
direction. The review initially concluded the script was simply too strict and should be loosened — then
caught that this rests on an *inference* about the server ("longest-suffix match"), and that loosening an
instrument whose job is to convert silent drops into loud failures is precisely the wrong error to make
while unsure. Slice 6 therefore surfaces the direction rather than accepting it, pending SP1 / the
consumer's S6.

## Log

*(append per slice: date, what shipped, surprises)*

- **2026-09-17 — slice 0 spikes run (SP1, SP2, SP3). All three changed the design.**

  **SP3 — directive legality: every position is legal.** A 15-case probe (directive before a `using`,
  before a type, between an attribute and its target, before an expression-bodied member, inside a
  `switch` section, before a closing brace, on blank separators, around field initializers, local
  functions, `try`/`catch`, iterators, interpolated strings, and *between the lines of one wrapped
  expression*) compiled with **0 errors, 0 warnings** on net10.0. Directives are lexical, so even a
  mid-expression line break tolerates one. **Consequence:** the `hidden` list is driven purely by
  phantom-avoidance, not by the parser — a simplification. The only hard constraint left is `inlineBlock`,
  which has no newline to host a directive at all.
  *PDB cross-check:* 12 of the 15 fake documents appear in the PDB; the three absent are exactly the
  `using` line and two type-declaration lines — **empirically confirming that a directive on a line with
  no IL yields no document and no sequence point**, which is the premise D3 rests on.

  **SP1 — coverlet over real emitter output: confirmed, including D3's method-entry claim.** Emitted C#
  from a real `.pg` (block lambdas + a deliberately dead branch) via the Debug CLI, annotated it per the
  invariant, ran xunit + `coverlet.collector` 6.0.2. The report: `filename="pgprobe\sp1probe.pg"`,
  `line-rate="0.9166"`, `branch-rate="0.5"`. Lines 4,5,6,7,8,9,11,15,16,17,18 covered; **line 12 `hits="0"`**
  (the dead branch); **line 11 `branch="True" condition-coverage="50%"`**; and `.pg` lines 1,2,3,10,13,14,19
  — comments, blanks and closing braces — **absent entirely. Zero phantoms.**
  - **Line 4 (`fn main() {`) reported `hits="1"`** — so stamping the method body's opening brace with the
    method's position does turn method entry into a numerator contribution. D3's reason for keeping
    `Method` is now measured, not argued. Slice 5 gained the corresponding brace rule.
  - **Hits accumulate on a flattened block lambda's line** (line 6 → `hits="7"`, line 16 → `hits="4"`),
    consistent with the documented many-to-one collapse.
  - **Surprises:** coverlet **normalizes the path separator to the platform's** (`\` on Windows)
    regardless of what the directive wrote; and it **never checks the path exists** — a directive naming a
    non-existent file is reported verbatim, which is why A12's liveness assertion has to be ours.
  - **Residual unknown, recorded not resolved:** the `<sources>` root is the common prefix across *all*
    documents in a report, so the remainder's shape depends on what else the report contains and is not
    predictable from here. Do not design against a predicted remainder (PRD §4.F).
  - **New finding:** std/lib helpers (`print`, `readText`, …) are emitted into the *same* output file as
    user code, and `SourceMap` stores their origin as a **logical name** (`std.io`), not a path. A
    directive naming `std.io` would claim an unresolvable document and fail A12 — so slice 5 gained the
    rule "any fileId whose canon is not an on-disk path emits `hidden`", which also subsumes the
    synthesized-prelude case.

  **SP2 — the dedup regression is real, and A8 asserted the opposite.** Building `rootA/p.pg` and
  `rootB/p.pg` — two *distinct* files that emit byte-identical C# — silently produces **one** `out/p.cs`
  today, because `writeDedup` collapses on content equality. With directives on they carry different
  origins, so content differs, and `main.cpp:174-177` hard-errors: a build that works today **fails**.
  This is arguably a latent bug being exposed (one of the two sources is currently unrepresented), but it
  is a behaviour change for a supported scenario, and it lands under *either* path form. **A8 rewritten**
  to expect the failure, with slice 6 making the diagnostic explain itself and point at `include` output
  rules. Stripping directives from the dedup key was considered and rejected: it would collapse two origins
  and silently lie about which source produced the output.
  *Note:* this is a third variant of the dedup question. The original PRD claimed absolute paths break
  dedup (wrong — see `ANALYSIS.md` §5); the review corrected that to "dedup is untouched" (also wrong, for
  a different reason); SP2 found the actual mechanism. The lesson is that this interaction resisted two
  rounds of reasoning and only yielded to measurement.

  **SP5 was not run — it cannot be.** It measures the *disabled* path's cost, which requires the feature to
  exist. Moved to after slice 5; listing it in slice 0 was a sequencing error in this plan's first draft.

- **2026-09-17 — designed.** PRD + PLAN written from a 5-agent investigation (see `ANALYSIS.md`), then
  revised through a maintainer design review (D1–D8 above). Issue #69's two prerequisites verified true.
  One correction folded in from the investigation: `line()` is not the only writer of `out_` (the prelude
  prepend at `emitter_base.cpp:1594` bypasses it). One correction folded in from the review: this PRD's own
  `writeDedup` claim was backwards, which had been load-bearing for a path-form decision. Nothing built
  yet; slice 0 spikes are the next action.
