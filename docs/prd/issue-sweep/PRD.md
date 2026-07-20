# Open-issue sweep — resolve all 19 open compiler defects in one PR (PRD)

> **GitHub:** maintainer request (2026-07-20): resolve every open issue in one full sweep, one PR, after
> the wave-2 + P35 branch (PR #58) landed the test coverage that surfaced most of them.
> **Provenance:** a 13-agent root-cause investigation (six clusters, each skeptic-verified against
> current code) + a follow-up operators-members investigation. Full evidence: [ANALYSIS.md](./ANALYSIS.md).

- **Status:** Draft v1.0 · 2026-07-20 · investigation complete; **four scope decisions gate coding** (§2).
- **Branch:** off `e2e-coverage-wave2` (or off master once PR #58 merges). One PR, acknowledged
  large-but-cohesive (~20 slices, individually-reviewable commits sharing deliberate machinery).

---

## 1. Problem

Wave-2's e2e coverage (PR #58) filed 12 probe-proven defects and inherited 7 pre-existing ones — **19
open issues, #38–#57** (there is **no #45**). They were deliberately deferred with acceptance-program
names but no fixes: the discipline was "land the test wave first, fix the defects in a dedicated PR."
This is that PR. Every issue is a correctness defect the differential suite now *would* catch if the
acceptance program existed — most are silent cross-target divergences or invalid emitted code the PRD §3
contract forbids.

The 19 issues collapse to a **small set of shared-machinery fixes** (§3), which is why one cohesive PR
is the right shape rather than 19 fragments: the pattern-lowering rewrite fixes three issues and enables
a fourth; one equality model fixes two; one method-plumbing surface fixes two; etc.

## 2. Scope decisions — ratify BEFORE coding (each stays inside PRD §3)

| Issue | Decision | Recommended call | Rationale |
|---|---|---|---|
| **#38** `is` / typed-match narrowing | implement vs refuse | **IMPLEMENT** for concrete class/union types; **REFUSE** interface type-tests on TS (erased there → silent mis-narrow) | promised as #29 follow-up; record the interface boundary in SPEC §3 |
| **#46** loop-var capture semantics | per-iteration vs shared (SPEC silent) | **IMPLEMENT per-iteration**; amend SPEC §6.4/§7 | least surprising; matches C# `foreach` + JS `let`; the IR already commits to it. **The SPEC amendment must land** or it's an undocumented behavior change |
| **#39e** `import * as` namespace | implement vs refuse | **DECIDE FIRST** — recommend implement a synthesized module-namespace symbol; else refuse and delete from grammar/SPEC | must NOT stay "parses but silently unresolved"; renaming imports (`x as flat`) is a sound implement regardless |
| **#51** expression-position block match arms | implement vs bounded refuse | implement statement-position block arms for real; **fallback: REFUSE expr-position block arms** only if Python-lambda/PHP-arrow hoisting proves intractable, recorded in SPEC §3.B | legitimate §3.B call, not a silent relax |
| **#53** member overloading | implement vs refuse | **IMPLEMENT** (resolve→mangle machinery already exists for top-level fns) | §3.A lists overloading; a member carve-out is an inconsistent gap |
| **#57** union `==` | implement vs refuse | **IMPLEMENT** structural equality (§3.C); document in SPEC §9 | C#/Python already structural; refusing on TS/PHP would be an inconsistent regression |
| **#55** PHP builtin-name collision | mangle vs refuse | **IMPLEMENT** function-position suffix-mangle | matches the other three targets; refuse-via-reserved-names wrongly rejects legal params/fields named `count` |
| **#54** finalizer, **#56a** unresolvable member | (settled §3.B) | **REFUSE with a targeted diagnostic** | silent verbatim emit / raw parse error IS the miscompile these remove |

## 3. Root-cause groups (the actual work, by code touched)

- **A — Pattern/match lowering** (`lower.cpp::pattern()`/`matchExpr()`, `ir::Pattern/MatchArm/Match`, all
  four `Match` rules): **#43** (ordered literal ctor sub-slots), **#52** (statement-form match), **#51**
  (block-arm bodies), enables **#38**. Order: #43 → #52 → #51.
- **B — Equality model** (`ir::Binary` + `lower.cpp` Binary + TS/PHP `Binary` rules + C# record equals):
  **#49 + #57 together** — one `lhsHasUserEq` fact + one union-structural fact + one reordered
  Binary-case ladder per target; C# emits a user `eq` as `override Equals` (not `operator ==`, which
  collides).
- **C — Indexer path** (`ir::Index` indices vector, `lower.cpp` Index, `emitter_base.cpp` Assign/index-write,
  csharp indexer synth): **#42** (multi-arg) then **#40** (get/set merge + `r[i]=v` write).
- **D — Method modifier/iterator plumbing** (`ir::Method` +/lower/`MethodDeclCtx`): **#47**
  (C# virtual/override), **#50** (TS member generator `*`).
- **E — Loop-var capture** (`ir::For` captured/needsCell already stamped, `capture_analysis.cpp`, C#
  range-for + Python lambda arms): **#46**.
- **F — PHP ctor/super** (transitive `baseHasInit` gate): **#48**.
- **G — Front-end parser** (`parser.cpp`): **#39a** do-while, **#39b** let-tuple, **#39c** accessor blocks
  (co-design C# `{get;set;}` with #40), **#39d** ctor-pattern guards (parser-only, co-lands #43).
- **H — Import resolution** (`compiler.cpp`/`sema.cpp`): **#39e**.
- **I — C# scope-legalization pass** (new target-gated `lower.cpp` pass): **#41** (CS0136 loop-binder reuse).
- **J — Sema refusal/inference** (`sema.cpp checkCall`/`checkMember`, `parser.cpp`): **#54** (finalizer
  refusal), **#56a** (unresolvable-member refusal), **#56b** (bidirectional lambda-param inference).
- **K — Member overloading** (`sema.cpp findMember`/resolve, `MethodCall::mangledMethod`): **#53**.
- **L — PHP builtin mangle** (php manifest identifiers/Call/fnSig): **#55**.
- **M — Py/PHP try-guard rewrite** (mirror TS `__handled` if-chain): **#44**.

## 4. Non-goals

- **No scope creep beyond these 19 issues.** New std surface, new targets, and features not already in
  grammar/SPEC stay out.
- **No silent relaxations** — every "refuse" decision emits a clean §3.B diagnostic; every "implement"
  is documented in SPEC where it defines behavior (§3.C/§3.D/§9).
- **No per-slice full gate** — CLAUDE.md discipline: implement all slices, cheap unit-exe/`-Tier fast`
  mid-flight, the full `build-and-test.ps1` gate ONCE at the end + one POSIX leg (Groups A/G touch
  platform-neutral IR + all four backends).

## 5. Acceptance criteria

1. Every one of the 19 issues has a **passing acceptance test** — a conformance program (new or an
   un-narrow of an existing narrowed program) or a unit test — that fails before its fix and passes
   after. Un-narrows: `indexer_grid` (#40/#42), `match_nested` (#43/#39d), `loop_control` (#41),
   `catch_when` (#44), `extension_generic` (#56b), `tuples` (#39b).
2. **`.expected` goldens are mandatory** for #43/#51/#49/#57 — those defects make all four targets
   *identically* wrong, so the pairwise C#-oracle diff false-passes; only a golden catches it.
3. Each scope decision from §2 is **recorded in SPEC.md** as decided.
4. Each fixed issue is **closed on GitHub with a comment linking this PR** and naming its acceptance test.
5. Full `scripts/build-and-test.ps1 -Tier full` green once at the end + one POSIX (WSL/ci.yml) leg.

---

*Implementation plan (dependency-ordered slices, acceptance-test matrix, risk matrix):
[PLAN.md](./PLAN.md). Full per-issue root-cause evidence: [ANALYSIS.md](./ANALYSIS.md).*
