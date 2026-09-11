# Code coverage — implementation plan (dependency-ordered slices)

One PR (`code-coverage-upload`), commits in this order so review + bisect stay sane.
`PRD.md` is the design contract; this is the slice plan + acceptance matrix.

**Build/test discipline (CLAUDE.md):** implement ALL slices first, then run the full gate ONCE at
the end. Mid-flight ceiling is `-Tier fast` or the unit exe — never the full gate, never speculative
extra legs. **Exception that applies here:** slices 2–4 touch Core C++ that CI compiles with g++,
so one POSIX compile+unit run (WSL cmake, or the PR's `ci.yml` Linux check) is part of the required
end gate, not an extra leg.

**Ordering rationale:** the spikes come first because SP1 can invalidate §4.B outright, and slice 1
ships standalone value even if the plugin work later stalls.

---

## Slice 0 — spikes (throwaway, time-boxed)

Run SP1 **before writing any C++**. Record each outcome in the Log below; a spike that fails
changes the design per PRD §5, it does not get worked around.

- **SP1** (`~30 min`) — hand-write `spike/plugin.lcov` with `SF:plugins/csharp/polyglot-plugin.json`
  and a handful of `DA:` records; upload under flag `spike-json` from a throwaway branch. Verify
  parse / listing / source rendering / no extension allowlist / counts toward the total.
  **Gate: if the server cannot render `.json` sources, stop and ask the maintainer for support
  before proceeding to slices 2–4.** Slices 1, 5(partial), 6 remain shippable meanwhile.
- **SP2** (`~15 min`) — add `--cobertura` to the existing gcovr call in a scratch CI run; assert
  `filename=` values are repo-relative and resolve against `git ls-files`.
- **SP3** (`~15 min`) — `id-token: write` + `use-oidc: true` on a non-fork PR run; confirm 2xx and
  no secret needed.
- **SP4** (`~1–2 h`) — throwaway prototype of offset→`Rule.line`→`evalRule` hit recording. Measure:
  sweep wall-clock delta (budget <10%), distinct-line attribution for nested `case` arms across all
  four manifests, and that the disabled path is unmeasurable.
- **SP5** (`~30 min`) — `choco install opencppcoverage`; confirm `--export_type cobertura:` composes
  with `--cover_children` and the two-`binary:` merge.

**Acceptance:** five recorded outcomes + a go/no-go on §4.B. Spike artifacts are deleted, not
committed.

## Slice 1 — publish the C++ coverage that already exists

The standalone quick win; independent of every plugin slice.

- `ci.yml` coverage job: add `--cobertura coverage/cpp/cobertura.xml` to the gcovr invocation
  (`ci.yml:96-98`), keeping `--html-details` + `--txt` + the step summary.
- Add `permissions: { contents: read, id-token: write }` to the job.
- Add the upload step exactly as PRD §4.D (OIDC, `disable-search: true`, `hashFiles` guard,
  non-fork guard, `finish: true`).
- Branch the strictness: `fail-ci-if-error: true` + no `continue-on-error` on `push` to master;
  lenient on PRs (§6.2).
- **Workflow comment** explaining why `partial:` is deliberately absent here (no NX, no affected
  set, whole corpus every run) — this is the documented divergence from the org template and will
  otherwise read as an omission.
- Keep the existing artifact upload: it is the offline debugging path.

**Acceptance:** a push to the PR branch produces a build on coverage.mintplayer.com with the C++
report attached; fork-PR simulation skips cleanly.

## Slice 2 — source provenance in the JSON parser (no behavior change)

- `json::Value` (`json.hpp:14`) gains `std::size_t offset = 0`.
- `json.cpp` `parseValue()` (`:96`) records the cursor at entry. Offsets only — line conversion
  happens at the consumer boundary, keeping the parser free of line counting.
- A small `offsetToLine(text, offset)` helper where the manifest text is already in hand.
- **Do not** change parse semantics, error recovery, or the LSP JSON path that shares this parser.

**Acceptance:** unit tests asserting offsets for nested object/array/scalar values; existing LSP +
plugin-load tests unchanged. Pure mechanism, zero behavioral delta.

## Slice 3 — rule provenance + the trace sink

- `Rule` and `Test` (`backend_engine.hpp:53-74`) gain `offset` + an opaque `srcId`, populated in
  `parseRule`/`parseTest` from the source `Value.offset`. Nested arms each carry their own.
  *(As built: offset + srcId rather than a resolved `line`, so the engine stays as free of line
  counting as the parser — conversion happens once, at the reporting boundary.)*
- A trace sink in `engine`: null by default, recording `(pluginName, line)` into a set. Marked in
  `evalRule` and `evalTest` (`backend_engine.hpp:120-121`) — the single choke point.
- **Language-agnostic:** the sink keys on the plugin's own name string as loaded. No target-name
  comparison anywhere (PRD §0). A reviewer should be able to grep the diff for `"csharp"` and find
  nothing.
- Emit the **static arm manifest** from the same parse: every `(plugin, line)` a `Rule`/`Test` was
  parsed at. Denominator and numerator share one code path (PRD §4.B).

**Acceptance:** unit test over a synthetic in-memory manifest — evaluate one arm, assert exactly
that arm's line is hit while its sibling `case` arm is recorded-but-unhit. Disabled sink costs one
null check.

## Slice 4 — CLI flag + aggregator

- `--emit-arm-trace <path>` on the CLI: appends `<plugin>\t<line>` records plus the static manifest
  header. Appendable so a 448-invocation sweep accumulates without a merge step.
  Debug flag — documented as such, not part of the stable CLI contract.
- `scripts/arm-trace-to-lcov.ps1`: folds `{static manifest} ∪ {hits}` into
  `coverage/plugins/<target>.lcov`, one `SF:plugins/<target>/polyglot-plugin.json` per plugin,
  `DA:<line>,<count>`. Directly invocable, no NX (the standing runner contract).
- `scripts/verify-coverage-paths.ps1` (PRD §4.E tripwire): every `SF:`/Cobertura `filename` in
  `coverage/**` must appear in `git ls-files`; fail loudly otherwise.

**Acceptance:** the fixture from PRD §7.4 — a rule added to a manifest and never referenced by any
conformance program appears in the lcov as an **uncovered** line, not as an absent one. Tripwire
fails on a deliberately-corrupted path.

## Slice 5 — wire both surfaces into CI

- Coverage job: pass `--emit-arm-trace` through the existing four-target sweep (`ci.yml:85-92`).
- Widen the sweep beyond `build` (PRD §4.A.2): add the `tests/refusals/` fixtures and `check`, so
  diagnostic and front-end-only paths stop reading as dead code. No language runtimes needed —
  seconds, not minutes.
  *(As built: the LSP leg is swept LOCALLY only. `tests/lsp/run-lsp.ps1` already drives a scripted
  stdio session and takes a `-Cli` path, so the local Windows sweep reuses it rather than
  reinventing one; it is left out of the ubuntu job because that driver is Windows-proven and a
  hung stdio session in CI costs more than the coverage it adds. Enabling it there is a follow-up
  worth doing only if the LSP paths prove materially under-reported.)*
- Run the aggregator + tripwire, then extend the single upload's `files:` with
  `coverage/plugins/*.lcov`.
- Keep `|| true` on the sweep: execution, not verification (correctness is the Windows gate's job).

**Acceptance:** one build on the server carrying C++ **and** four plugin reports, finalized once;
job wall-clock within ~10% of today's.

## Slice 6 — local parity + docs

- `scripts/coverage.ps1`: add `--export_type cobertura:x64\coverage\cobertura.xml` beside the HTML,
  and run the arm-trace sweep + aggregator so a local run yields the same artifacts CI uploads.
  Still **no upload from a dev machine** (PRD §4.C).
- Optional `coverage` nx target wrapping the local script, consistent with the other legs.
- CLAUDE.md: replace the two-instrument coverage paragraph with the three-instrument table
  (PRD §6.6) + the Linux-only `#ifdef _WIN32` bias (§6.3).
- README: the org badge (PRD §6.5) — **added last, after the first successful master upload.**
- `docs/prd/PLAN.md`: roadmap entry + slice log, per house convention.

**Acceptance:** `pwsh scripts/coverage.ps1` on a clean machine produces HTML + cobertura + plugin
lcov with no CI and no NX.

## Slice 7 — resolve what the tracer finds

Not optional, and not a follow-up PR (CLAUDE.md single-PR rule). The first real run will surface
never-fired arms — wave 2 found 28 by hand and stopped looking.

For each uncovered arm: **cover it** (a conformance program — preferred, it also strengthens the
differential suite), **delete it** (dead template), or **document it** (reachable only under a
config no gate exercises — listed explicitly with a reason, not silently tolerated).

**Acceptance:** zero unexplained uncovered arms. Any conformance programs added here are green on
all four targets, so the count of gate legs is unchanged but the corpus grows.

---

## Acceptance matrix

| # | Criterion (PRD §7) | Slice | Verified by |
|---|---|---|---|
| 1 | master push → finalized build, C++ + 4 plugin reports | 1, 5 | dashboard |
| 2 | non-fork PR uploads, cannot red the PR | 1 | PR run |
| 3 | manifests browsable per-line | 0 (SP1), 4, 5 | dashboard |
| 4 | denominator from the parse, not a regex | 3, 4 | fixture test |
| 5 | uncovered arms resolved in this PR | 7 | review |
| 6 | local run → cobertura + HTML + plugin lcov | 6 | `scripts/coverage.ps1` |
| 7 | full gate green, no leg slower | all | `-Tier full` (once, at end) |
| 8 | zero target-name comparisons in Core | 3 | diff grep |
| 9 | path tripwire passes + fails loudly on bad input | 4 | tripwire test |
| 10 | docs + badge updated | 6 | review |

## Risks

- **SP1 is load-bearing.** If the server rejects `.json` sources, slices 2–4 stall pending a
  server-side change. Mitigation: SP1 runs first; slices 1 and 6 still ship real value alone.
- **Tracer in the hot path.** A per-`evalRule` set insertion across 448 CLI invocations. Mitigation:
  SP4 measures before committing; sink is null unless the flag is passed, so the normal gate is
  untouched by construction.
- **A large dead-arm count** could balloon slice 7. Mitigation: cluster by cause and add one
  conformance program per cluster; if the honest answer is "this target's arm is unreachable",
  delete it — the anti-silent-drop contract will catch an over-deletion at load time.
- **Coverage number looks bad on day one** (Linux-only bias + newly-visible plugin gaps). This is
  the instrument working. Documented in §6.3, and the ratchet in §6.4 measures the delta, not the
  absolute.

## Log

*(append per slice: date, what shipped, surprises)*

- **2026-09-11 — slices 1–6 built** (local build + unit suite green; full gate still pending).
  - *Slice 1:* gcovr `--cobertura`, `id-token: write`, the org upload step. OIDC rather than a
    token — the repo is public, so the `MintPlayer.AI` precedent applies and no secret is added.
  - *Slices 2–4:* `json::Value.offset` (stamped once around the parse dispatch) + `json::LineIndex`;
    `Rule`/`Test` offset + srcId; `TraceSink` marked at `evalRule`/`evalTest`; `indexRule`/
    `indexTest` collecting the denominator from the same parse; `armtrace` module; the
    `--emit-arm-trace` CLI flag (pre-dispatch, so it traces `build`/`check`/`lsp` alike, and writes
    even on failure — a refusal still exercised rules); `scripts/arm-trace-to-lcov.ps1` and
    `scripts/verify-coverage-paths.ps1`. 11 unit checks, including the denominator guarantee (an
    untaken `case` arm is present-and-uncovered, never absent) and distinct lines for sibling arms.
  - *Slice 5:* trace wired through the CI sweep, plus a refusals + `check` sweep; aggregator and
    tripwire run before the single upload.
  - *Slice 6:* `coverage.ps1` emits Cobertura, sweeps refusals/check/LSP, and runs the aggregator;
    CLAUDE.md now carries the three-instrument table and the Linux-only bias.
  - **Surprises:** (1) The premise "this repo has no coverage" was wrong — two report-only
    instruments already existed; the real gap was publishing, plus the plugin blindness. (2) The
    tracer plumbing was shallower than wave 2 feared: one choke point, a 192-line parser, and
    pretty-printed manifests. (3) First smoke run: **224/1034 csharp arms** from a single
    `counter.pg` build, with the other three manifests correctly at 0 (denominator-only, since they
    were loaded but not targeted) — the instrument distinguishes loaded from exercised, as intended.
- **2026-09-11 — slice 7 + SP2, target met.** Per-manifest arm coverage now **csharp 94.5%,
  php 96.1%, python 94.6%, typescript 94.8%** (maintainer's bar: 90%). Full classification of the
  residue in PRD §7.1. Four things happened, in the order they were found:
  - **SP2 answered by the first CI run:** `gcovr --cobertura` doesn't exist on ubuntu-22.04
    (gcovr 5.0); `--xml` emits the same Cobertura and works on both. Fixed.
  - **The multi-file programs weren't being swept** the way the conformance runner builds them
    (`entry.pg` + `--root` + a pgconfig naming every target). Added to both sweeps — the linked-module
    arms measured ~20% of each manifest on their own.
  - **Two new conformance programs** for genuinely unexercised shapes, plus **67+2 lines of
    unreachable untyped-catch templates deleted**, proven behavior-neutral by byte-comparing 24
    emitted files.
  - **The big one: the tracer itself had a blind spot.** Decl-flavor rules are interpreted by
    `EmitterBase::runDeclRule`, not `evalRule`, so every declaration- and statement-shaped rule read
    as cold — which is precisely why the uncovered list looked like `MethodDecl`/`Program`/`TryStmt`/
    `ClassDecl`. One line fixed it and the real figure jumped ~13pp. **Lesson worth keeping: the
    instrument's own blind spot is indistinguishable from missing tests.** Recorded as a permanent
    invariant in PRD §4.B.
  - **Still open:** SP1 + SP3 need the CI run now in flight (does the server render a `.json`
    source; does OIDC authenticate). SP5 needs OpenCppCoverage installed locally. Final full gate
    + the README badge remain.
