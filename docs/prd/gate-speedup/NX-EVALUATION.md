> **Provenance:** follow-up evaluation (2026-07-19) answering the maintainer's question "can we use NX?
> I already have a remote cache running at nx-cache.mintplayer.com" — one research agent over the repo,
> current NX docs, and the CVE/licensing record. Verdict adopted into [PRD.md](./PRD.md) §2 and
> [PLAN.md](./PLAN.md) slice 6.

# NX for the Polyglot gate — evaluation & verdict

**Bottom line:** the 5× headline win comes entirely from the restructure (slices 1–5) and is
NX-independent — ship those regardless. For the *cache* layer (slice 6): **NX at per-leg granularity
+ the existing HTTP remote cache replaces gate-cache.psm1, while the per-program L1 stays INSIDE
run-conformance.ps1.** The maintainer already operating nx-cache.mintplayer.com (and standardizing on
NX elsewhere) is the deciding weight.

## Probe: nx-cache.mintplayer.com
`HTTP/1.1 403 Forbidden`, 9-byte body, no `Server` header — the response shape of an NX self-hosted
HTTP remote cache (OpenAPI `/v1/cache/{hash}`) with no bearer token presented. No auth attempted, no
writes. It speaks the NX HTTP cache protocol.

## NX on a non-JS repo
Minimal scaffold: private `package.json` (devDep `nx@^21`), `nx.json`, one root `project.json` with
~13 `nx:run-commands` targets wrapping the existing pwsh runners (which stay directly invocable —
release.yml's POSIX legs never see NX). **Load-bearing mechanic confirmed:** a target's input hash can
include another target's OUTPUT FILES via `dependentTasksOutputFiles` — it hashes the files on disk,
so the CLI exe + plugins (outputs of a `cache:false` build target that declares them) enter every
leg's cache key. Toolchain versions enter via `runtime` inputs (`dotnet --version`, `node --version`,
`npx tsc --version`, python/php in `sharedGlobals`). Verify empirically: the hash moves on a relink
(nrwl/nx#22253 native-hasher wrinkle — keep the native hasher ON), and a no-op MSBuild does not relink.

## Granularity: per-leg, NOT per-program
NX tasks are processes — 95 per-program targets = 95 pwsh spawns, fighting the merged runner's shared
`csc /shared` + runspace pool. NX caches each LEG as one unit (~13 targets, negligible overhead); the
per-program L1 skip lives inside run-conformance.ps1. The composition delivers the warm loop: edit one
.pg → NX cache-hits the ~10 unaffected legs, the in-runner L1 skips 94/95 programs in the affected ones.
NX's unique add is cross-machine/remote hits.

## Remote cache — current (2026) facts
- Self-hosted NX remote caching is **free** (the Powerpack paywall era ended v20.8, Apr 2025).
- The official bucket plugins (`@nx/s3-cache`, `@nx/gcs-cache`, `@nx/azure-cache`,
  `@nx/shared-fs-cache`) are **deprecated 2026-05-21 due to CVE-2025-36852 ("CREEP", CVSS 9.4)** — an
  architectural, unpatchable cache-poisoning flaw. Do NOT adopt them.
- The surviving, supported path is the **custom HTTP OpenAPI remote cache**
  (`NX_SELF_HOSTED_REMOTE_CACHE_SERVER` + `..._ACCESS_TOKEN`, `PUT/GET /v1/cache/{hash}`) — exactly
  what nx-cache.mintplayer.com is. Bind ONLY to this stable interface.

## Correctness fit
- **FAIL never cached: native** — NX only reads entries with exit code 0.
- **"Comparisons always execute" is met by reframing:** a cache-hit leg replays stdout without
  re-comparing, but any input change (including an edited `.expected` golden — they're in the inputs
  glob) invalidates the hash and re-executes the whole leg. No-stale-green holds via complete input
  hashing rather than literal re-execution. Accepted.
- **CI stays COLD** (never reads/writes remote): keeps the un-keyed-dependency backstop AND fully
  sidesteps CREEP (CI, the artifact producer, never trusts the cache). Locals read+write with the
  bearer token.

## Verdict vs gate-cache.psm1
NX: ~1–1.5 days, battle-tested hashing (the exact closure ANALYSIS §4 hand-derives, first-class),
native remote sharing against a server that already exists. psm1: ~2–3 days, no remote story, but
zero new toolchain. **Adopt NX-hybrid for slice 6.** Two honest counterarguments, recorded: (1) the
restructure already fixes the stated pain — NX only accelerates the warm loop + adds sharing, and a
solo dev could live with local-only psm1; (2) NX cache governance went free→paid→free→CVE-deprecated
in ~18 months — a values mismatch with a zero-deps project. Mitigations: bind only the OpenAPI
interface, CI cold, every runner NX-free-invocable — NX is a removable convenience layer, never
load-bearing for correctness.

## Recommended shape
```
package.json          # { "private": true, "devDependencies": { "nx": "^21" } }
nx.json               # sharedGlobals runtime inputs; targetDefaults; remote-cache env
project.json          # root "polyglot" project, ~13 run-commands targets
tests/**/run-*.ps1    # UNCHANGED — still directly invocable (release.yml)
scripts/build-and-test.ps1  # stays the NX-free entry point; `nx run-many` is the cached path
```
Example target (conformance) — build is `cache:false` with declared exe+plugin outputs; conformance
depends on it, hashes `dependentTasksOutputFiles: **/*` + program/golden/runner/pgconfig globs +
runtime toolchain inputs; `outputs: []`.
