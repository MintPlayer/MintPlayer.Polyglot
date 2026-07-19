# Gate speed-up: ~16 min → ~3–3.5 min full, ~25–40 s warm dev loop (PRD)

> **GitHub:** no originating issue — maintainer request (2026-07-19): *"running the build/unit-tests
> currently takes WAAAAY too long. Can we speed this up by parallelizing the tests (in batches) and/or
> caching the test results and only running the affected tests (like NX does)?"*

- **Status:** Draft v1.0 · 2026-07-19 · investigation complete, implementation not started.
- **Author:** Pieterjan (with Claude Code).
- **Provenance:** an 8-agent measured investigation — one profiling agent (real unit-cost timings on
  this machine), three competing designs (parallelization / NX-style caching / gate restructure), one
  skeptic per design, one synthesis. Full report with the arithmetic: [ANALYSIS.md](./ANALYSIS.md).

---

## 1. Problem — where the ~16 minutes actually go

The measured cost model (ANALYSIS.md §2, sibling-load-adjusted): the full local gate is **~958 s**, of
which **~767 s (80%)** is the three conformance legs — and the dominant cost inside them is
**`dotnet build` of the C# oracle, ~2 s × 95 programs × 3 legs ≈ 570 s**, because `run-python.ps1` and
`run-php.ps1` each *re-compile the identical C#* that `run-diff.ps1` already built. The library gate
re-transpiles all 95 programs a fourth time (~40 s). Everything else — parity, MSBuild no-op, unit exe,
smoke, refusals, LSP, fidelity, watch — totals ~32 s. The gate is not slow because tests are slow; it
is slow because the same work is done three-to-four times, serially, through the slowest possible
compiler entry point.

## 2. The recommended architecture (skeptic-adjusted; ANALYSIS.md §1)

Three layers, in order of principle — **restructure first, parallelize second, cache last** (the
restructure eliminates by construction most of what a cache would have had to memoize):

1. **Merged single-pass conformance runner** (`tests/conformance/run-conformance.ps1`, replacing the
   three legs *and* the library gate's transpiles): per program, ONE 4-target CLI invocation (per-program
   pgconfig; the 7 pinned PHP refusers get a 3-target config + a dedicated refusal probe), a mandatory
   per-target output-file-presence assertion (anti-silent-drop), ONE C# oracle compile via
   **`csc /shared`** (verified byte-identical to the MSBuild oracle; kills all 278 conformance
   `dotnet build`s), all four runtimes run against the single oracle stdout + `.expected` goldens,
   pristine TS staged for the library gate's one `tsc` pass. Target-subset parameter keeps the two
   `release.yml` call sites (Windows Release + POSIX) working.
2. **Bounded parallelism**: `ForEach-Object -Parallel` over programs (throttle 16 transpile / 8 run;
   parallel `csc /shared` verified contention-free), plus overlapping the NuGet gate's independent
   fixtures against its serial pack chain. Ships one commit AFTER the sequential merged runner, so
   correctness fallout is attributable.
3. **NX-style cache, reduced to its surviving core** (`scripts/gate-cache.psm1`): an L1 content key
   (program bytes + exe-adjacent plugin manifests + CLI exe + pgconfig/lock + runner script + `.expected`
   + toolchain fingerprint incl. tsc) that skips a program's compile/run when nothing it depends on
   changed, and an optional L2 emitted-bytes key that survives C++ rebuilds (transpile always
   re-executes; only compile/run are skipped). **FAIL is never cached, comparisons always execute,
   CI always runs cold, `-NoCache` end-to-end** — the full false-green analysis is ANALYSIS.md §4.
4. **Tiered gate**: `build-and-test.ps1 -Tier fast|full` — fast = parity→build→unit→smoke→refusals→LSP
   ≈ 18 s (codifies CLAUDE.md's "cheap mid-slice ceiling"); full stays the only pre-merge bar.
   `-SkipConformance` is deleted; the registry leg gets an explicit local-only skip opt-in
   (`POLYGLOT_ALLOW_REGISTRY_SKIP=1` — this machine's broken loopback) that CI never sets.

**Dropped after verification** (recorded in ANALYSIS.md §1.6): the batch-csproj/dispatcher design
(works, but `csc /shared` matches it without harness source transforms), gate-wide
`MSBUILDDISABLENODEREUSE` (measured ~60% penalty), caching the 3.2 s unit exe.

## 3. Projected budget (honest numbers — both skeptics rejected the optimistic ones)

| Scenario | Today | After |
|---|---:|---:|
| Full local gate (no-op C++ build) | ~958 s (~16 min) | **~190–215 s (~3–3.5 min), ~5×** |
| One `.pg` changed (with cache) | ~16 min | **~25–40 s** |
| One plugin JSON changed | ~16 min | ~1.5–2.5 min |
| Localized C++ change | rebuild + 16 min | rebuild + ~3.5–6.5 min (L2) |
| Mid-slice sanity (`-Tier fast`) | ad-hoc unit exe | ~18 s + incremental build |

Per-stage arithmetic: ANALYSIS.md §2. Do not quote "2.5 min"/"6×" — not defensible under the skeptics'
recomputation.

## 4. Non-goals

- **No coverage shrinkage** — same assertions, restructured execution. The two accepted deltas are
  named in ANALYSIS.md §4 (oracle runs once per gate instead of three times; the single-target CLI
  path drops to a representative set — compensated by extending G39's byte-identity check and the
  cold CI runs).
- **No C++ changes** in slices 1–5 (the compiler-side levers — Release-built gate CLI, batch-mode
  `polyglot build` — are recorded follow-up candidates for the residual ~40 s of Debug spawn tax).
- **No new dependencies** — pwsh + the toolchains already required.
- The dirty C++ solution build itself is untouched.

## 5. Acceptance criteria

1. `-Tier full` runs every assertion the current gate runs (registry modulo the explicit opt-in) and
   completes in ≤ 4 min on this machine with a no-op C++ build.
2. `-Tier fast` completes in ≤ 30 s + build and runs parity/build/unit/smoke/refusals/LSP.
3. Both `release.yml` conformance call sites work via the target-subset parameter (POSIX-clean).
4. With the cache enabled, a one-program change re-runs only that program (audit line names the
   cached/executed split); `POLYGLOT_NO_CACHE=1` and CI (`$env:CI`) force cold; a FAIL is never served
   from cache.
5. One CI leg runs `POLYGLOT_ORACLE=msbuild` (the csc-vs-SDK canary).
6. The slice-0 CLI question (4-target pgconfig silently dropping targets with exit 0 in some cwd —
   two agents observed contradictory behavior) is reproduced-or-refuted first; if real, it gets its
   own issue as an anti-silent-drop contract violation.

---

*Implementation plan: [PLAN.md](./PLAN.md). Full investigation with measurements: [ANALYSIS.md](./ANALYSIS.md).*
