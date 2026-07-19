# RECOMMENDATION — Polyglot gate speed-up (synthesis of Designs A/B/C + skeptic verdicts)

## 0. Executive summary

Adopt **Design C's restructure as the backbone** (merged single-pass conformance runner + `csc /shared` oracle + tiered gate), **Design A's parallelism and hygiene** layered on top, and **Design B's cache reduced to its surviving core** (L1 input-key + stage keys, with every verdict-B closure fix) as the final dev-loop layer. Drop Design A's namespace-wrap batch-csproj/dispatcher entirely — superseded by the csc oracle, which is nearly as fast after parallelism and keeps per-program entry-point execution (no canary machinery, no harness source transform, no red-path rebuild dance).

Honest projected outcome (skeptic-adjusted, this machine, no-op C++ build):

- **Full local gate: ~958 s → ~190–215 s (~3–3.5 min), ~5×** — no coverage lost.
- **Warm dev loop (one `.pg` changed), with cache: ~25–40 s.**
- **Fast tier (mid-slice sanity): ~18 s + incremental C++ build.**

Do not publish "2.5 min" or "6×": both skeptics independently found phase-run and nuget-gate estimates optimistic; ~5× is the defensible claim.

---

## 1. The recommended architecture

### 1.1 Backbone: one merged conformance runner (`tests/conformance/run-conformance.ps1`) — from Design C

Replaces `run-diff.ps1` / `run-python.ps1` / `run-php.ps1` **and** the library gate's 95 transpiles (clean cutover per the no-backward-compat memory, *but see release.yml constraint below*). Per program (93 single + 2 multi-file):

1. Copy into a per-program work dir with a per-program `pgconfig.json` — `{"targets":["csharp","typescript","python","php"]}`; the 7 G28 pinned PHP refusers get a 3-target config (verified: a refusing target **partial-emits siblings**, so this is viable).
2. **One** CLI invocation (`polyglot build entry.pg --lib io`) emits all four targets. This cuts conformance CLI spawns ~469→~104.
3. **Assert every expected output file exists per program, or FAIL.** Mandatory: Design A observed a 4-target pgconfig silently dropping python/php with **exit 0** in one cwd, while Design C's experiment (per-workdir pgconfig, in-box resolution) emitted all four. The discrepancy is a plugin-resolution/cwd condition — the file-presence assertion closes the runner-side hole regardless, and the CLI-side silent drop gets its **own issue on day 0** (it smells like an anti-silent-drop contract violation, per the project's prime directive).
4. Stage the **pristine `.ts`** to `library-staging/` *before* the node extensionless-import rewrite.
5. Compile the C# oracle **once** with `csc /shared` via a shared `scripts/lib/OracleCompile.ps1` (verified: output byte-identical to the MSBuild-built oracle; dll runs under `dotnet <dll>` with a 6-line runtimeconfig, `rollForward: latestMinor`). This kills all 278 conformance `dotnet build`s. Keep the issue-#9 guard verbatim (exit 0 AND dll exists). Fixes folded in: **pin csc discovery to the SDK `dotnet` actually resolves** (`dotnet --version`, not newest-listed — this machine has a preview SDK), explicit `/nullable:disable` + `/langversion` pinned to the SDK default, `POLYGLOT_ORACLE=msbuild` fallback kept alive on one CI leg, `dotnet build-server shutdown` (or `/keepalive`) in a `finally`.
6. Run all four runtimes via the existing `Invoke-WithTimeout` — **with `WorkingDirectory` set to the program's dir** (from A; `file_io*.pg` writes relative paths — this is both a parallelism prerequisite and a strict isolation improvement over today). Check **exit code before touching emitted artifacts** for refusers (verified necessary). Compare each of TS/Py/PHP against the single oracle stdout per-pair (failure messages unchanged), plus `.expected` goldens (G32); skip the php-vs-expected assertion for pinned refusers. G28 semantics preserved: pinned refusers get a dedicated `--target php` refusal probe (PASS refused-by-design / WARN if it now transpiles); unpinned refusal = FAIL.
7. `[Console]::OutputEncoding = UTF8` retained (run-library.ps1 sets it today).

**Runner takes a target-subset parameter** (e.g. `-Targets csharp,typescript`) with runtime-presence guards — **required**, because `release.yml` invokes `run-diff.ps1` on two legs (Windows Release exe **and** POSIX `./build/polyglot`); the runner must be POSIX-pwsh-clean (no `$env:LOCALAPPDATA` php default, no `\` assumptions). Keep a thin `run-diff.ps1` shim or update both workflow call sites in the same commit.

**Extend G39** (cli-smoke byte-identity between co-emitted and per-target output) from one program to a representative set — a multi-file program, a pinned refuser under its 3-target config, a generics-heavy program — since G39 now carries the co-emission≡per-target burden for the whole corpus, and the single-target CLI path (the NuGet production path) otherwise loses corpus exercise.

### 1.2 Parallelism — from Design A, with verdict-A fixes

- `ForEach-Object -Parallel` over programs: throttle ~16 for the transpile phase (measured 3.4× effective), throttle ~8 for the run phase. `csc /shared` handles concurrent requests (verified: 4 concurrent compiles, no contention).
- Runspace gotchas: define/pass helper functions into the block, collect result objects and print `[PASS]/[FAIL]` sorted afterward, per-worker `WorkingDirectory`.
- **Drop gate-wide `MSBUILDDISABLENODEREUSE=1`** (measured ~60% build penalty — the proposal's own estimates assumed node-reuse speeds). Use `dotnet build-server shutdown` in a `finally`, or scope the env var to a stage that demonstrably exhibits lock trouble.
- Ship the merged runner **sequential first, parallel as the next commit** — isolates any correctness fallout from the restructure vs the parallelism.
- Drop A's stretch idea of overlapping the small gates via `Start-ThreadJob` (low value, registry port risk).

### 1.3 Downstream gates — from Design C

- **library**: `-Staged` param consumes the merged runner's pristine TS; export-regex + consumer stubs + **one** tsc pass. 40 s → ~5 s. Standalone self-emitting mode retained.
- **samples**: keeps its 10 transpiles (different corpus) but compiles via `OracleCompile.ps1`; xfail mechanics untouched (per-program csc means an xfail'd compile failure stays per-program — no batch-exclusion machinery needed, which is another reason to prefer csc over A's batch csproj). 25 s → ~7 s.
- **nullable**: `csc /nullable:enable /warnaserror` (verified exit-0-equivalent). 4 s → ~1.5 s.
- **msbuild/nuget**: from A3 — parallelize the *independent* fixtures (5–8: Shared/MultiPg/Twin/NoCfg) **overlapping** the strictly-serial App chain (fixtures 0–4 are pack→mutate→clean dependent; verdict A: 35 s only if overlapped, else 40–50 s). One serial fixture warms the freshly-packed nupkg first. Budget **~45 s**. Otherwise untouched — it is now the gate's real-MSBuild canary and must stay.
- **registry**: stays in `full`, but with **concrete skip signaling**: skip only when a local-only opt-in (`POLYGLOT_ALLOW_REGISTRY_SKIP=1`, set on this machine where loopback is broken) is present; otherwise a bind failure fails the gate. CI never sets it — "CI treats skip as failure" needs this mechanism, a warning + exit 0 is indistinguishable.

### 1.4 Tiered gate — from Design C

`scripts/build-and-test.ps1 -Tier fast|full` (default `full`). **fast** = parity → MSBuild → unit exe → cli-smoke → refusals → lsp ≈ 17.5 s + incremental C++ build — codifies the CLAUDE.md "cheap unit-test exe ceiling" with the two cheapest cross-cutting nets added free. **full** stays the only pre-merge bar; fast is a subset, never a substitute (explicitly: fast drops fidelity/watch/everything downstream). Delete `-SkipConformance`.

### 1.5 Cache layer — Design B, reduced and fixed

The merged runner **structurally eliminates** B's headline win (the 183 duplicated oracle builds dedup by construction), so B shrinks to what still pays: the dev-loop skip. Keep:

- **L1 program key** (skip transpile+compile+run on hit): sha256 of `.pg` bytes ++ **all exe-adjacent `plugins/*/polyglot-plugin.json`** (verdict B: the CLI loads from `exe.parent/plugins`, populated by an xcopy that never deletes — live drift evidence exists (`x64\Debug\plugins\kotlin\`); repo copies are the wrong files, and `loadAllPlugins` loads the whole adjacent set) ++ CLI exe hash ++ **the discovered `pgconfig.json` (+ lock)** ++ runner script bytes ++ cliArgs ++ `.expected` bytes ++ **toolchain fingerprint including tsc** (dotnet/node/python/php/tsc versions).
- **L2 emitted-bytes keys** (transpile always runs; compile+run skipped when emit unchanged) — this is what makes a *localized* C++ change cheap (rebuild + re-transpile ~1–3.5 min, all compiles/runs skipped for unaffected targets). Keys include **relative file names + entry name**, not just sorted bytes.
- **Stage keys** for the cheap gates (smoke/refusals/lsp/fidelity/watch/library/nuget), fingerprint included.
- Hygiene fixes: in-process SHA256 helper (never `Get-FileHash` per file), atomic temp+`Move-Item -Force` writes, LastWriteTime-based (or touch-on-hit) 30-day eviction, schema-versioned root, `%LOCALAPPDATA%` with `POLYGLOT_GATE_CACHE` override.
- **Concurrent-agent poisoning fix** (this session literally has three sibling agents sharing `x64\Debug`): take a **per-run snapshot of exe + plugins** at gate start and hash/execute from the snapshot — this simultaneously fixes the xcopy-drift problem.
- Invariants kept verbatim: **FAIL is never cached; the comparison/golden check is never cached; CI always runs cold** (`$env:CI` auto-disables); `-NoCache` / `POLYGLOT_NO_CACHE=1`; per-leg audit line (`82 cached (L1), 9 (L2), 4 executed`).
- Before quoting warm-loop numbers, **empirically verify a no-op MSBuild run does not relink the exe** (verdict B could not test it under concurrent-agent load).
- Explicitly declined (per B): caching the 3.2 s unit exe.

### 1.6 Dropped / deferred

- **Design A's namespace-wrap batch csproj + dispatcher** — dropped. It works (verified), but csc /shared reaches comparable wall-clock with zero harness source transform, per-program execution of the real emitted `Main()` wrapper (killing A's canary/wrapper-regex mitigations), natural per-program compile-failure isolation (no exclude-and-rebuild red path), and trivial xfail semantics. Keep the scratch artifact as a recorded alternative if csc discovery proves fragile cross-platform.
- **Compiler-side levers** (Release-built CLI in the gate; batch-mode `polyglot build` accepting many entries per process) — the ~300 ms/invocation Debug tax remains the biggest residual (~40 s across ~115 remaining spawns). Note as a follow-up PRD candidate; out of scope here (no C++ changes).
- The dirty C++ solution build — untouched by everything above.

---

## 2. Projected time budget (no-op C++ build, skeptic-adjusted)

| Stage | Today (s) | After, cold (s) | Arithmetic |
|---|---:|---:|---|
| parity/build/unit/smoke/refusals/lsp/fidelity/watch | 31.8 | 31.8 | unchanged fixed costs |
| registry | 15 | 0 local / 15 CI | opt-in loud skip on this machine; CI enforces |
| run-diff + run-python + run-php | 767 | **~70–90** | transpile 95 spawns × ~0.4 s ÷ ~3× parallel ≈ 13–20; csc 95 × 0.21–0.6 s ÷ ~4 ≈ 5–15; runs ~373 × 75–150 ms ÷ ~2.5 ≈ 15–25 (verdict A: budget 15–25, not 11); staging/overhead ~10; + load margin |
| library | 40 | ~5 | 95 transpiles → 0 (staged); one 1.5 s tsc pass |
| samples | 25 | ~7 | 10 transpiles + 10 csc + parallel node |
| nullable | 4 | ~1.5 | 1 transpile + 1 csc |
| msbuild/nuget | 75 | ~45 | independent fixtures parallel-overlapping the serial App chain (verdict A: 35 only if overlapped; 40–50 realistic) |
| **Total** | **~958 (16 min)** | **~190–215 (~3–3.5 min)** | **~5×** |

| Dev-loop scenario (with cache layer) | Today | After |
|---|---:|---:|
| one `.pg` changed | ~16 min | ~25–40 s (1 program × 4 targets + hashing + stage hits) |
| one plugin JSON changed | ~16 min | ~1.5–2.5 min (affected leg re-transpiles; oracles cached) |
| localized C++ change | rebuild + 16 min | rebuild + ~3.5–6.5 min (L1 void, L2 saves all compiles/runs; verdict B: use corpus-average transpile, not 0.34) |
| broad C++ change (printer whitespace) | rebuild + 16 min | rebuild + ~3.5 min (correctly near-cold — but the *restructured* cold gate) |
| mid-slice sanity (`-Tier fast`) | (unit exe only, ad hoc) | ~18 s + incremental build |

All figures measured under sibling-agent CPU load; ratios are the datum, absolutes are lower bounds of quiet-machine behavior.

---

## 3. Implementation plan — ordered slices (~one commit each)

| # | Slice | Size | Win | Measured risk |
|---|---|---|---|---|
| 0 | **File the CLI issue**: 4-target pgconfig silently dropping python/php with exit 0 in some cwd (anti-silent-drop contract). No gate change. | XS | correctness | none |
| 1 | **`-Tier fast\|full`** + **registry opt-in skip** (`POLYGLOT_ALLOW_REGISTRY_SKIP`, absent ⇒ bind failure fails; delete `-SkipConformance`) | S | dev loop ~18 s; −15 s local noise | **Low** — additive flag; skip signaling is explicit |
| 2 | **`scripts/lib/OracleCompile.ps1`** (csc /shared, active-SDK pinning, refs.rsp, runtimeconfig, msbuild fallback + `POLYGLOT_ORACLE=msbuild`, build-server shutdown) and switch **samples + nullable** to it | S–M | −20 s; de-risks the mechanism on small corpora before the big cutover | **Low-med** — csc≡msbuild output verified; nullable `/warnaserror` equivalence verified; SDK-pinning fix folded in; fallback keeps POSIX/CI robust |
| 3 | **Merged `run-conformance.ps1`, sequential** — single 4-target invocation, file-presence assertion, exit-code-first refusers, csc oracle, issue-#9 guard, G28/G30/G31/G32 folded, library `-Staged` consumption, **target-subset param**, update both `release.yml` call sites (or shim), POSIX-clean, UTF8 encoding, extend **G39** to the representative set | M | **−~620 s** (807 → ~180 seq incl. library) — the headline win | **Medium** — mechanism pieces individually verified (co-emission, csc identity, refusal partial-emit, pgconfig walk-up); risk is faithful folding of the four gates' semantics + the release.yml legs; sequential-first isolates it |
| 4 | **Parallelize the merged runner** (transpile throttle 16, runs throttle 8, per-worker WorkingDirectory, collected output) | S–M | ~180 → ~70–90 s | **Low-med** — parallel csc and parallel dotnet builds both verified contention-free; runspace gotchas enumerated; `-Sequential` switch retained for debugging |
| 5 | **nuget gate parallelization** (independent fixtures overlap the serial App chain; serial nupkg warm-first; no gate-wide node-reuse kill) | S–M | 75 → ~45 s | **Medium** — the one NuGet race (concurrent first-install) is designed around; verdict A confirmed concurrent restores of cached packages are safe; overlap scheduling is the fiddly part |
| 6 | **`scripts/gate-cache.psm1`** — L1 + stage keys with the full verdict-B fix list (exe-adjacent plugin hashing via per-run snapshot, pgconfig+lock in key, tsc in fingerprint, `Move-Item -Force`, LastWriteTime eviction, FAIL-never-cached, CI-cold, `-NoCache`, audit lines); verify no-op-MSBuild-no-relink first | M | .pg dev loop → ~25–40 s | **Medium** — invalidation-closure risk is the known failure mode; every hole the skeptic found has a named fix; CI-cold is the backstop |
| 7 | *(optional)* **L2 emitted-bytes compile/run cache** for the localized-C++-change scenario | S–M (on top of 6) | rebuild-loop → ~3.5 min | **Low** given 6 — strictly stronger key (transpile always re-executes) |

Slices 1–5 need no C++ changes and no new dependencies. After slice 5 the full gate is ~3–3.5 min without any caching; 6–7 are pure dev-loop layers that can land later or never.

---

## 4. Correctness guarantees

**What always runs, every gate, cache or no cache:**
- buildfile parity, the MSBuild solution build, the unit/golden exe (never cached — 3.2 s, the only in-process C++ coverage).
- Every **comparison**: cs-vs-ts/py/php diffs and `.expected` golden checks execute on every run against (fresh or cached) stdout — editing a golden takes effect immediately, no invalidation logic.
- The transpile itself, whenever L2 is the operative layer (any C++ rebuild) — so "did the compiler change?" is never answered by a hash alone.
- The msbuild/nuget gate's real `dotnet build` path, plus the `POLYGLOT_ORACLE=msbuild` CI leg — the standing canaries against csc-vs-SDK drift.
- G28 refusal classification (pinned probe + unpinned-refusal FAIL) and the per-program file-presence assertion (anti-silent-drop).

**Why the cache cannot produce a stale false-green:**
1. **Complete, audited input closure**: program bytes, **exe-adjacent** plugin manifests (all of them — the set the CLI actually loads), CLI exe, discovered pgconfig(+lock), the runner script's own bytes, cliArgs, `.expected` bytes, and a toolchain fingerprint covering dotnet/node/python/php **and tsc**. The recipe lives in one psm1 function with the closure enumerated in a comment; any recipe change bumps the schema version, making all old entries unreachable.
2. **Per-run snapshot of exe + plugins** — hashing and executing the same frozen bytes closes both the xcopy-drift hole and the concurrent-agent relink race (entries can never be written under an old exe hash with a new exe's behavior).
3. **FAIL is never written; only a full end-to-end PASS is** — a red program re-executes completely on every run. Atomic temp+rename writes mean an interrupted gate can't leave half-entries; there is no global "last green" marker to go stale.
4. **L2 is strictly stronger than L1** for compile/run: those stages are pure functions of (emitted bytes + file/entry names, csproj/rsp text, toolchain, runner hash), all of which are in the key — and the emit itself is freshly recomputed.
5. **CI always runs cold** — the flake detector and the backstop against any residual un-keyed dependency (environment-only regressions, nondeterministic output freezing a lucky sample). Cross-branch/worktree reuse is safe by construction: keys are content hashes.
6. Escape hatches: `-NoCache` / `POLYGLOT_NO_CACHE=1` end-to-end; per-leg audit summary so a suspicious green is inspectable.

**Restructure-specific guarantees** (independent of the cache): per-program dlls execute the real emitted `Main()` wrapper (no dispatcher shrinkage); one-assembly-per-program preserves the CS0101 property; refuser exit codes are checked before artifacts; G39 (extended) gates co-emission≡per-target byte identity; the merged runner's script hash is in every cache key so editing it voids its own entries.

**Acknowledged, accepted deltas**: the C# oracle runs once per gate instead of three independent times (a nondeterministic emit loses two flake-chances — compensated by cold CI); the `--target csharp` single-target CLI path drops from ~190 corpus exercises to G39's representative set + cli-smoke + the NuGet gate.

---

## 5. What stays CI-only

- **Registry gate enforcement** — locally skippable only via the explicit env opt-in (this machine's loopback is broken regardless); CI never sets it, so a bind failure or regression fails CI.
- **Always-cold runs** — CI never reads the cache; it is the standing verification that a cold gate is green and the compensating control for every residual cache risk.
- **`POLYGLOT_ORACLE=msbuild` leg** — one CI leg runs the conformance oracle through real per-program `dotnet build`, keeping the fallback path exercised and csc-vs-MSBuild semantics honest.
- **POSIX + Release-CLI conformance legs** (`release.yml`, via the target-subset parameter) and the `ci.yml` ubuntu build floor — unchanged obligations the merged runner must keep satisfying; slower runners keep their own multipliers, but scale by the same ~5×.
- The full cross-platform release matrix (macOS legs, provenance channel) — untouched by this work.

**Recorded but out of scope**: Release-built CLI in the gate or a multi-entry batch-mode `polyglot build` (attacks the ~300 ms/invocation Debug tax, the largest post-restructure residual at ~40 s); root-causing the local registry loopback failure; the dirty C++ build itself.

**Key files**: `C:\Repos\MintPlayer.Polyglot\scripts\build-and-test.ps1`; `C:\Repos\MintPlayer.Polyglot\tests\conformance\run-diff.ps1`, `run-python.ps1`, `run-php.ps1` (replaced by `run-conformance.ps1`); `C:\Repos\MintPlayer.Polyglot\tests\library\run-library.ps1`; `C:\Repos\MintPlayer.Polyglot\tests\samples\run-emit.ps1`; `C:\Repos\MintPlayer.Polyglot\tests\nullable\run-nullable.ps1`; `C:\Repos\MintPlayer.Polyglot\tests\msbuild\run-nuget.ps1`; `C:\Repos\MintPlayer.Polyglot\tests\cli\run-cli-smoke.ps1` (G39); `C:\Repos\MintPlayer.Polyglot\.github\workflows\release.yml` (two run-diff call sites); `C:\Repos\MintPlayer.Polyglot\src\MintPlayer.Polyglot.Cli\src\main.cpp:1490` (exe-adjacent plugin load) and the `.vcxproj` xcopy post-build (no-delete drift); new files `tests/conformance/run-conformance.ps1`, `scripts/lib/OracleCompile.ps1`, `scripts/gate-cache.psm1`. Working experiment artifacts: `C:\Users\piete\AppData\Local\Temp\claude\C--Repos-MintPlayer-Polyglot\04b560da-57c6-4859-b3c8-26c68fe1d5af\scratchpad\batch\proj` (A's batch csproj, kept as recorded alternative) and `scratchpad\exp-*` (C's csc /shared oracle).