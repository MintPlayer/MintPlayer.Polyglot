# E2E coverage — wave 2: divergence pins, harness hardening, code coverage (PRD)

> **GitHub:** no originating issue — maintainer request (2026-07-19): *"more e2e tests to see if
> transpiling works as expected in all scenarios"* + *"also set up code-coverage on the repo"*.
> Successor to the wave-1 language-surface fill (`docs/prd/e2e-scenario-coverage/`, PR #45).

- **Status:** Draft v1.0 · 2026-07-19.
- **Author:** Pieterjan (with Claude Code).
- **Provenance:** a 13-agent read-only coverage analysis (six dimensions, each skeptic-verified) —
  full findings with file evidence in [ANALYSIS.md](./ANALYSIS.md). Gap IDs **G1–G48** and new-defect
  IDs **D1–D8** below refer to that document.

---

## 1. Problem

Wave 1 (PR #45) closed the *single-feature* §3.A gaps: 71 conformance programs now run on all four
targets with byte-identical stdout. The analysis shows the remaining risk concentrates in four places
wave 1 deliberately didn't touch:

1. **Live silent divergences the probes proved today** (Tier 1). The suite never feeds the divergent
   inputs, so it stays green while targets disagree: PHP's loose `==` family (numeric strings,
   `!= null` on `0`/`""`, class identity), TS/Python `List.clear/removeAll` rebinding the receiver
   instead of mutating it (aliases keep the old list), TS `BigInt(<rounded number>)` precision loss on
   annotation-typed i64 literals, unmasked shifts and float `%` on Python/PHP, and three different
   `Math.round` half-value modes across the four targets. This is exactly the miscompile class the
   PRD §3 contract forbids.
2. **Feature-composition holes** (Tier 2). Programs are single-purpose by house style; composing
   features is where transpilers break. Probing compositions surfaced **eight new unfiled defects
   (D1–D8)**, including C# emitting no `virtual`/`override` (method *hiding* — silent wrong numbers)
   and block-bodied match arms silently lowering to `return 0` on **all four targets** (pairwise
   run-diff false-passes it — correlated wrongness).
3. **Shipped toolchain surfaces with zero automated execution** (Tier 3).
   `tests/msbuild/run-nuget.ps1` is wired into no gate; `polyglot lsp` (two shipped clients!) has no
   protocol-level test; `run-php.ps1` counts *any* capability refusal as a pass, so one plugin-JSON
   edit silently converts coverage to green; a single miscompiled `while(true)` wedges the gate
   forever (no per-program timeout); §3.B refusals are unit-tested in-process but no e2e leg drives
   the CLI over bad input (exit code, `path:line:col: error:` shape, no output file written).
4. **No code-coverage instrumentation at all.** Nothing measures which Core paths the unit + e2e
   suites actually execute, and nothing measures which plugin template arms ever fire (the analysis
   found e.g. 28 never-executed scalar-parse templates by hand — a measurement should have found
   them).

## 2. The change

Four workstreams (full gap tables: [ANALYSIS.md](./ANALYSIS.md) §1; slice mapping: [PLAN.md](./PLAN.md)).

### A. Conformance programs + in-wave fixes (G1–G26)

Wave-1 rules apply unchanged: auto-discovered `programs/*.pg`, integer/string output only, ASCII
strings, one scenario per program, header comment naming the pinned divergence. Two tiers:

- **Tier 1 (G1–G12) — programs that FAIL today**, each landing *with* its plugin/Core fix (PR #45
  pattern): PHP strict-comparison cutover, TS/Py in-place list-clear templates, TS exact-BigInt i64
  literals, shift masking, `fmod` arms, one SPEC'd round-half mode, C# Rune→string projection for
  `codePoints()` values, i64/i32 boundary wrap, composite record equality. Four of these carry small
  **spec decisions** (round-half mode; PHP i64 wrap shim-vs-§3.B-refusal; float→int out-of-range;
  `INT_MIN / -1`; uninitialized-field policy G11; float→int masking G12) — each decision is recorded
  in SPEC.md / §3.C, never made silently.
- **Tier 2 (G13–G26) — probe-verified passing today**, pure regression pins: disposal-on-throw
  ordering, async composition, recursive union trees, generic extensions, `with`-copy aliasing,
  i64-through-union binders, i64 comparisons past 2^53, math/string/file-io edges, f32 §3.C subset,
  closure `this` capture at runtime, and a ~1000-line generated scale program.

### B. Harness hardening + new legs (G27–G41)

- Wire the existing `tests/msbuild/run-nuget.ps1` into `scripts/build-and-test.ps1` (G27).
- Runner hardening: per-program timeout (G30), loud cleanup + unique temp roots (G31), pinned
  expected-refusal set for `run-php.ps1` (G28), CLI-surface smoke (G41), one four-target
  single-invocation build (G39).
- **`<name>.expected` golden-stdout support** in all three conformance runners (G32) for
  semantics-defining programs — closes the C#-oracle *correlated-wrongness* blind spot (pairwise
  equality can't catch all targets being identically wrong).
- **Two new legs:** `tests/refusals/run-refusals.ps1` — CLI over §3.B fixtures asserting nonzero
  exit, diagnostic shape, multi-error output, and *no output file written* (G33); and
  `tests/lsp/run-lsp.ps1` — a scripted Content-Length-framed stdio JSON-RPC session (G29).
- Gate extensions: bare-`build` include discovery (G34), `polyglot install` offline flow (G35),
  watch delete/recreate cycles (G36), stale-lock re-resolve units (G37), template-token routing to
  disk (G40), POSIX watch/registry legs in ci.yml (G38).

### C. Front-end robustness (G42–G48)

Unit tests with their small guarded fixes: `std::stoll` process-abort on huge enum values (G42), BOM
handling (G43), unterminated `/*` swallowing code silently (G44 — miscompile-adjacent), recursion-depth
guard (G45), `Activator` refusal row (G46), non-ASCII literal/identifier diagnostics (G47–G48).

### D. Code-coverage instrumentation (new, maintainer request)

Two complementary instruments, no external services:

1. **C++ line/branch coverage of the Core** — a `coverage` job in `.github/workflows/ci.yml`:
   CMake Debug build with `--coverage` (gcc/gcov), run `polyglot-tests` **and the conformance-style
   smoke** (subprocess CLI runs accumulate `.gcda` naturally), then `gcovr` → summary in
   `$GITHUB_STEP_SUMMARY` + HTML-details artifact. **Report-only at first** (no threshold); a floor
   is added once the baseline is known. Locally on Windows: `scripts/coverage.ps1` wrapping
   **OpenCppCoverage** (`--cover_children` so CLI subprocess runs count) over the MSVC unit-test exe
   → HTML report. Optional dependency — the script degrades to a clear "install via
   `choco install opencppcoverage`" message.
2. **Plugin template-arm coverage** (the honest metric for the four backends, which are JSON, not
   C++): a CLI debug flag (e.g. `--emit-arm-trace`) that records which manifest template arms fired,
   aggregated across a conformance run into a per-plugin fired/total report. This is what would have
   found G13's 28 dead templates mechanically. Scoped as the *last* slice and demand-gated — if the
   flag turns out to need invasive plumbing, it degrades to a filed follow-up issue, not a hack.

### E. Defect filing (D1–D8) and blocked pins

The eight probe-proven new defects are re-verified and **filed as issues first** (arguably the
analysis's highest-value output): D1 loop-closure capture divergence, D2 missing C#
`virtual`/`override` + PHP `super()` fatal, D3 `operator fn eq` breaking C#/mis-routing on TS,
D4 TS member generator missing `*`, D5 block-bodied match arms lowering to `return 0` four-way,
D6 match-as-statement invalid C#, D7 member-overloading scope decision, D8 missing finalizer
refusal. Each carries its acceptance program (named in ANALYSIS.md §4), landing **with the fix**,
not in this wave. Likewise the wave-1 narrowed programs un-narrow when #39–#44 close
(ANALYSIS.md §3) — out of scope here.

## 3. Non-goals

- **No new language/std surface** — spec *decisions* on existing-surface edge semantics (§2.A's list)
  are in scope; new features are not. `tryParse`, the `Math.fround` opt-in flag, and the decimal
  fixed-point type stay follow-up issues.
- **No fixing D1–D8 in this wave** — they're filed with acceptance programs and fixed in their own
  PRs (single-PR-per-defect house rule). Only Tier-1 (G1–G12) fixes ride along, PR #45 style.
- **No coverage *threshold* enforcement initially** — measurement first, floor later.
- **No golden-file snapshots of emitted code** — G32's `.expected` files pin *runtime stdout* only.

## 4. Acceptance criteria

1. Every Tier-1 program passes on all differential legs *with its fix*, or is filed + narrowed with a
   `TODO(#N)` (never a silent xfail); every spec decision from §2.A is recorded in SPEC.md/§3.C.
2. Every Tier-2 program passes on all legs unmodified (they pin today's agreement).
3. `run-nuget`, `run-refusals`, and `run-lsp` run green as stages of `scripts/build-and-test.ps1`
   (dotnet/node-guarded where applicable); a hanging generated program fails its program with
   `[FAIL] timed out` instead of wedging the gate.
4. `run-php.ps1` fails on any refusal outside the pinned expected set.
5. ci.yml's coverage job publishes a gcovr summary + artifact on every PR touching `src/**`;
   `scripts/coverage.ps1` produces a local HTML report when OpenCppCoverage is installed.
6. D1–D8 exist as GitHub issues with repro programs attached.
7. Full `scripts/build-and-test.ps1` green, run once at the end of each PR (house rule), plus one
   POSIX leg since ci.yml and runner scripts change.

---

*Implementation plan: see [PLAN.md](./PLAN.md). Full evidence: [ANALYSIS.md](./ANALYSIS.md).*
