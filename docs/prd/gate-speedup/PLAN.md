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

## Slice 6 — NX orchestration + remote cache, per-leg (M) — decision in [NX-EVALUATION.md](./NX-EVALUATION.md)

*(Supersedes the originally-planned hand-rolled `gate-cache.psm1` — the maintainer already operates
nx-cache.mintplayer.com and standardizes on NX elsewhere.)*

- Scaffold: private `package.json` (devDep `nx@^21`), `nx.json`, root `project.json` with ~13
  `nx:run-commands` targets wrapping the EXISTING runners verbatim (`pwsh -NoProfile -File …`). The
  runners stay directly invocable — release.yml's POSIX legs and `build-and-test.ps1` never require NX;
  `nx run-many` is the cached path.
- `build` target: `cache: false`, but **declares outputs** (`x64/Debug/*.Cli.exe`,
  `x64/Debug/plugins/**`) so every leg's `inputs` can include
  `{ "dependentTasksOutputFiles": "**/*" }` — the exe + plugins enter each cache key as file hashes.
- Leg inputs: program/fixture globs, `.expected` goldens, the runner's own script, pgconfig(+lock),
  `scripts/lib/OracleCompile.ps1`, plus `runtime` toolchain inputs in `sharedGlobals`
  (`dotnet --version`, `node --version`, `npx tsc --version`, python/php).
- Remote: the OpenAPI HTTP interface ONLY (the deprecated bucket plugins are barred:
  CVE-2025-36852). **Already configured on the dev machine** (verified 2026-07-19):
  `NX_SELF_HOSTED_REMOTE_CACHE_SERVER=https://nx-cache.mintplayer.com` and
  `NX_SELF_HOSTED_REMOTE_CACHE_ACCESS_TOKEN` are set as machine env vars — NX picks both up with
  zero repo config, so nothing token-shaped ever enters the repo. FAIL-never-served is NX-native
  (exit-0 entries only).
- **CI policy — adopt the ORG CONVENTION** (the maintainer's established pattern, documented in
  MintPlayer.Spark `docs/guide-nx-remote-cache.md`, using the org-wide secrets `NX_CACHE_SERVER` /
  `NX_CACHE_RO_TOKEN` / `NX_CACHE_RW_TOKEN`): every workflow sets

  ```yaml
  NX_SELF_HOSTED_REMOTE_CACHE_SERVER: ${{ secrets.NX_CACHE_SERVER }}
  NX_SELF_HOSTED_REMOTE_CACHE_ACCESS_TOKEN: ${{ (github.event_name == 'push' &&
    github.ref_name == github.event.repository.default_branch) &&
    secrets.NX_CACHE_RW_TOKEN || secrets.NX_CACHE_RO_TOKEN }}
  ```

  so **only post-merge runs on master can WRITE** — PR checks and branch pushes read but cannot
  poison (the CREEP mitigation, server-enforced by the token split; fork PRs get no secrets at all).
  Applied to this repo: `ci.yml` (PR checks incl. the gate legs) runs RO; a master push warms the
  cache RW. Two Polyglot-specific keeps on top of the convention:
  - **release.yml stays cache-free** — same spirit as Spark's deploy isolation (`--skip-nx-cache`):
    shipped bits are always rebuilt from source; the release matrix never touches NX anyway.
  - **The coverage job runs `--skip-nx-cache`** — its purpose is executing the code, a cache hit
    would defeat the instrumentation.
  Residual accepted risk vs the old "CI cold" idea: RO PR checks can be served entries written by
  master builds or the maintainer's own machine (both trusted, single-maintainer repo); the
  cache-free release legs remain the cold backstop for shipped artifacts.
- **Keep the per-program L1 inside `run-conformance.ps1`** (per the original design): per-program NX
  targets would spawn 95 processes and defeat the shared `csc /shared` pool. Warm loop = NX leg hits
  × in-runner program skips.
- Verify empirically before trusting warm numbers: `dependentTasksOutputFiles` moves the hash on a
  relink (keep the native hasher ON — nrwl/nx#22253), and a no-op MSBuild does not relink the exe.

## Slice 7 (optional) — in-runner L2 emitted-bytes key (S–M): C++ rebuild loop → ~3.5 min

Unchanged from the original design, and still in-runner (NX's `dependentTasksOutputFiles` gives the
LEG-level equivalent free, but not per-program granularity): key = emitted bytes + names + rsp text +
toolchain + runner hash. The transpile ALWAYS re-executes; only compile/run are skipped. Correctness
argument in ANALYSIS.md §4.

## Gate & docs

Full `-Tier full` gate once at the end + one POSIX pwsh syntax pass of the new runners (release.yml
legs depend on them). Update CLAUDE.md's gate section (`-Tier`, the cache, the registry opt-in) and
append the milestone log to `docs/prd/PLAN.md`. Record follow-up candidates: Release-built gate CLI /
batch-mode `polyglot build` (the residual ~40 s Debug spawn tax), root-causing the local registry
loopback failure.

---

## Log

**2026-07-19 — Slice 0: silent-drop REFUTED.** Four probes (pgconfig next to entry, cwd = project /
parent / unrelated dir; and pgconfig ONLY in the cwd with the entry elsewhere): config discovery is
ENTRY-relative — the first three emit all four targets with exit 0, the fourth refuses loudly with
exit 64 ("no --target given and no pgconfig.json declares `targets`"), never a partial emit. No issue
to file; the merged runner keeps the per-target file-presence assertion as designed (belt and braces).
