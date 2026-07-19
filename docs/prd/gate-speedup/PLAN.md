# Gate speed-up — implementation plan

Companion to [PRD.md](./PRD.md); measurements and design rationale in [ANALYSIS.md](./ANALYSIS.md).
**One branch, one PR** (house rule). No C++ changes in slices 0–5; slices 6–7 are pure dev-loop layers
that can land later (or never) without weakening the gate.

Ordering principle: **restructure → parallelize → cache**. Each slice is ~one commit; the sequential
merged runner lands BEFORE its parallelization so correctness fallout is attributable to the right
change. The gate's coverage must never shrink — every current assertion has a named home in the new
shape (mapping in ANALYSIS.md §1.1–1.4).

## Slice 0 — reproduce-or-refute the 4-target silent-drop, file if real (XS)

Two investigation agents observed contradictory behavior for a 4-target pgconfig build (one saw
python/php silently dropped with exit 0 in a particular cwd; one saw all four emitted with per-workdir
configs). Reproduce under both conditions; if the silent drop is real, file it as an anti-silent-drop
contract violation (prime-directive territory) with the repro. Either way the merged runner keeps the
per-target file-presence assertion — the runner-side hole is closed regardless.

## Slice 1 — tiered gate + explicit registry skip (S)

`build-and-test.ps1 -Tier fast|full` (default full). fast = parity → MSBuild → unit exe → cli-smoke →
refusals → lsp (~18 s + build). Delete `-SkipConformance`. Registry leg: skip ONLY when
`POLYGLOT_ALLOW_REGISTRY_SKIP=1` (set locally on this machine — loopback broken); absent ⇒ a bind
failure fails the gate; CI never sets it.

## Slice 2 — `scripts/lib/OracleCompile.ps1` + adopt in samples/nullable (S–M)

The shared csc-oracle helper: `csc /shared` discovered from the SDK `dotnet` actually resolves
(`dotnet --version`, NOT newest-listed — this machine has a preview SDK), refs.rsp, 6-line
runtimeconfig (`rollForward: latestMinor`), `/nullable:disable` + pinned `/langversion`,
`POLYGLOT_ORACLE=msbuild` fallback, `dotnet build-server shutdown` in `finally`. Switch
`tests/samples/run-emit.ps1` (keeps xfail per-program) and `tests/nullable/run-nullable.ps1`
(`/nullable:enable /warnaserror` — verified exit-0-equivalent) to it first: de-risks the mechanism on
small corpora before the big cutover. −20 s.

## Slice 3 — merged `run-conformance.ps1`, SEQUENTIAL (M) — the headline win (−~620 s)

Replaces run-diff/run-python/run-php + the library gate's transpiles. Per program: per-workdir
pgconfig (4 targets; 3 for the 7 pinned PHP refusers + dedicated `--target php` refusal probe:
PASS refused-by-design / WARN if it now transpiles / unpinned refusal = FAIL), ONE CLI invocation,
**per-target output-file presence asserted**, pristine `.ts` staged to `library-staging/` before the
node import rewrite, ONE `csc /shared` oracle compile (issue-#9 guard verbatim: exit 0 AND dll
exists), all four runtimes via `Invoke-WithTimeout` with per-program `WorkingDirectory` (file_io*
writes relative paths), exit-code-first for refusers, pairwise oracle comparisons + `.expected`
goldens (skip php-vs-golden for pinned refusers), `[Console]::OutputEncoding = UTF8`.
G28/G30/G31/G32 semantics fold in verbatim.

Also in this slice: `-Targets` subset parameter (runtime-presence guarded, POSIX-pwsh-clean — no
`$env:LOCALAPPDATA` php default, no `\` assumptions); update BOTH `release.yml` call sites (or leave
a thin `run-diff.ps1` shim); `tests/library/run-library.ps1 -Staged` consumes the staging dir (one
tsc pass; standalone self-emitting mode retained); extend **G39** in cli-smoke from one program to a
representative set (multi-file, pinned refuser under 3-target config, generics-heavy) — G39 now
carries the co-emission≡per-target burden for the corpus.

## Slice 4 — parallelize the merged runner (S–M): ~180 s → ~70–90 s

`ForEach-Object -Parallel`: throttle 16 for transpile, 8 for the run phase (measured 3.4× effective;
parallel `csc /shared` and parallel dotnet verified contention-free). Helpers passed into the
runspace block; results collected and printed sorted; `-Sequential` switch retained for debugging.
NO gate-wide `MSBUILDDISABLENODEREUSE` (measured ~60% penalty).

## Slice 5 — NuGet-gate parallelization (S–M): 75 s → ~45 s

Independent fixtures (Shared/MultiPg/Twin/NoCfg) run parallel, OVERLAPPING the strictly-serial App
chain (fixtures 0–4: pack→mutate→clean). One serial fixture warms the freshly-packed nupkg first
(the one real NuGet race). Budget honestly: ~45 s (35 only if the overlap schedule is perfect).

## Slice 6 — `scripts/gate-cache.psm1`: the NX layer (M) — .pg dev loop → ~25–40 s

L1 program key = SHA256(program bytes ∥ exe-adjacent plugin manifests — ALL of them ∥ CLI exe ∥
discovered pgconfig+lock ∥ runner script bytes ∥ cliArgs ∥ `.expected` bytes ∥ toolchain fingerprint
incl. tsc). Per-run **snapshot of exe + plugins** (hash and execute the same frozen bytes — closes
xcopy drift AND the concurrent-sibling-agent relink race). Value = verified stdout per target, PASS
only. **FAIL is never written; comparisons/goldens always execute; CI (`$env:CI`) always cold;
`-NoCache`/`POLYGLOT_NO_CACHE=1`; audit line per leg** ("82 cached (L1), 9 (L2), 4 executed").
In-process SHA256 (no per-file `Get-FileHash`), atomic temp+`Move-Item -Force`, LastWriteTime 30-day
eviction, schema-versioned root at `%LOCALAPPDATA%` (`POLYGLOT_GATE_CACHE` override). Before quoting
warm numbers: verify a no-op MSBuild run does NOT relink the exe. Declined: caching the unit exe (3.2 s).

## Slice 7 (optional) — L2 emitted-bytes key (S–M): C++ rebuild loop → ~3.5 min

Key = emitted bytes + names + rsp/csproj text + toolchain + runner hash. The transpile ALWAYS
re-executes (so "did the compiler change?" is never answered by a hash); only compile/run are
skipped. Strictly stronger than L1; correctness argument in ANALYSIS.md §4.

## Gate & docs

Full `-Tier full` gate once at the end + one POSIX pwsh syntax pass of the new runners (release.yml
legs depend on them). Update CLAUDE.md's gate section (`-Tier`, the cache, the registry opt-in) and
append the milestone log to `docs/prd/PLAN.md`. Record follow-up candidates: Release-built gate CLI /
batch-mode `polyglot build` (the residual ~40 s Debug spawn tax), root-causing the local registry
loopback failure.

---

## Log

*(empty — populated during implementation)*
