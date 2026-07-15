# Plugin auto-download from `pgconfig.json` — implementation plan

Companion to [PRD.md](./PRD.md) (decisions D1–D6 referenced below). One PR resolves issue #30; slices are
commits, each independently gated. Discipline (project convention): run `scripts/build-and-test.ps1` (or
the `/build-and-test` skill) at the end of every slice; the emitted-output corpus must stay
**byte-identical** throughout — this milestone changes *resolution*, never *emission*.

Build: **VS 18 Insiders MSBuild (v145)** — see root `CLAUDE.md`. New sources must be added to **both**
`MintPlayer.Polyglot.Cli.vcxproj`/`Tests.vcxproj` and `CMakeLists.txt`
(`scripts/check-buildfile-parity.ps1` is the first stage of the gate). Core stays IO-free — every new
download/lockfile/cache unit lives in the **CLI layer**.

Status: **built + gated** (2026-07-16; slices 0–6 landed as commits on `p30-plugin-autodownload`, one
commit per slice; slice 7 remains follow-up-gated per issue #30). Post-plan finding recorded in the
master PLAN P30 entry: the lock's `resolved` URL is treated as a hint — a moved registry re-locates the
pinned version and the pinned integrity still gates acceptance.

---

## Slice 0 — Extract testable units + archive/hash primitives

Today the CLI flows are anonymous-namespace functions in `main.cpp` (1484 lines), invisible to the test
exe. New code goes into linkable CLI-layer translation units (shared by `Cli` and `Tests` projects):

- `src/MintPlayer.Polyglot.Cli/src/pkg/sha.{hpp,cpp}` — SHA-512 + SHA-1 (one vendored public-domain
  single-file implementation used on **all** platforms — smaller #ifdef surface than three OS crypto APIs;
  KB-scale inputs make speed irrelevant) + SRI helpers (`sha512-<base64>` format/verify, RFC 4648 padding).
- `src/MintPlayer.Polyglot.Cli/src/pkg/inflate.{hpp,cpp}` — gzip member parsing + DEFLATE decode via a
  vendored public-domain single-file inflater (puff.c-class or miniz's tinfl — decode-only; choose at
  implementation time, record provenance in a header comment). Output sized from the gzip ISIZE trailer;
  hard cap on inflated size (zip-bomb bound).
- `src/MintPlayer.Polyglot.Cli/src/pkg/tar.{hpp,cpp}` — ustar reader: 512-byte headers, octal fields,
  typeflags `0`/`\0`/`5`, **pax `x` extended headers** (`path=` records), `prefix` field; ~150 lines.
  **Zip-slip-safe**: refuse absolute paths, `..` segments, symlink/hardlink/device entries; strip the
  `package/` prefix. GNU `L` longname: refuse with a clear message (npm doesn't emit it for our payloads).
- Also move `PgConfig`/`loadPgConfig` (`main.cpp:100-141`) and `pluginCacheDir()` (`main.cpp:146-161`)
  into `src/pkg/config.{hpp,cpp}` so slices 2–4 are unit-testable.

*Gate:* unit tests over checked-in golden fixtures — a real `npm pack` tarball of `plugins/python`
(fixture bytes committed), a pax-long-path tarball, a traversal tarball that **must refuse**, SRI
verify/mismatch vectors, gzip trailer/cap cases. Buildfile-parity green; CLI byte-identical behavior
(pure refactor + dead-code-free additions).

## Slice 1 — Semver subset + registry client

- `src/pkg/semver.{hpp,cpp}` — parse `(major,minor,patch,prerelease[])`; compare (numeric ids numeric,
  alnum lexical, with-prerelease < without); desugar `^`/`~`/exact/`>=`/`latest` per PRD D4;
  `maxSatisfying(versions, range)` with the prerelease-exclusion rule.
- `src/pkg/registry.{hpp,cpp}` — `HttpGet(url) → bytes` ladder: **WinHTTP** (Windows) / **libcurl**
  (macOS, linked) / **dlopen libcurl.so.4 → subprocess `curl --fail -sSL` → diagnostic** (Linux), honoring
  `HTTPS_PROXY`/`NO_PROXY`. Abbreviated-packument fetch (`Accept: application/vnd.npm.install-v1+json`,
  scope slash `%2F`-encoded), parsed with the existing JSON reader into
  `{versions: [{version, tarball, integrity, shasum}], distTags}`. Registry base resolution:
  `POLYGLOT_REGISTRY` env → `.npmrc` `@scope:registry=`/`registry=` (project dir upward, then home) →
  `https://registry.npmjs.org`.

*Gate:* semver table tests (node-semver-derived, incl. `^0.x`, prerelease exclusion, `latest`); packument
parsing from fixture JSON; `.npmrc`/env precedence tests. No network in unit tests — `HttpGet` is injected
(function ref) so resolution logic is tested against fixtures.

## Slice 2 — Lockfile + versioned cache

- `src/pkg/lockfile.{hpp,cpp}` — `pgconfig.lock.json` read/write next to `pgconfig.json`
  (`lockfileVersion: 1`, `packages: { name → {version, resolved, integrity} }`), written via the existing
  JSON writer, stable key order (deterministic diffs).
- Cache re-key (PRD D3): `<cache>/<npm-name>/<version>/{polyglot-plugin.json, meta.json}` (scoped names
  are two directory levels, like node_modules). `meta.json` = `{name, version, resolved, integrity,
  manifestSha512}`. Atomic install: write to `<cache>/.tmp-<pid>-…` then `rename`. On-load verification:
  manifest re-hash vs `manifestSha512`, `meta.integrity` vs lock — mismatch ⇒ entry refused (treated as
  absent). Old flat `<cache>/<target>/polyglot-plugin.json` entries are simply never consulted again
  (clean cutover; no migration).

*Gate:* lockfile round-trip; cache write/load/verify unit tests incl. tamper (flip a manifest byte ⇒
refused) and torn-write (missing meta ⇒ refused).

## Slice 3 — Auto-download in `resolveConfiguredTargets` + `install` rewrite

The heart. `resolveConfiguredTargets` (`main.cpp:178-189`) becomes the full pipeline — one hook covers
`build`, watch (per cycle), and the LSP (per analysis) since all three already call it:

For each `dependencies` entry (bare key normalized to `@mintplayer/polyglot-target-<key>`):
1. `file:` → load in place (unchanged).
2. In-box satisfied (CLI version satisfies range, or dev build `0.0.0-dev`) → use in-box (PRD D4).
3. Lock entry satisfies range **and** cache verifies → `loadPluginFile` from cache. **No network.**
4. Else: packument fetch → `maxSatisfying` → tarball download → SRI verify → in-exe extract →
   `validateBackend` → cache write → lock write → register. A fetched version differing from a loaded
   in-box registration **shadows** it for this process.
5. Failures → the PRD §5 diagnostic table; in the LSP, failures are memoized per config generation and
   surfaced as diagnostics (no per-keystroke retries, no hangs).

`targets` entries then resolve against loaded backends only; an uncovered target name ⇒ "add a
`dependencies` entry for …" (the bare-name cache probe at `main.cpp:186-188` and the lazy probes at
`main.cpp:539`/`main.cpp:398` die).

`runInstall` (`main.cpp:1357-1428`) is rewritten over the same pipeline: **delete** the `npm pack` and
`tar` `std::system` shell-outs; `polyglot install` = resolve+fetch+cache (+ lock write when a
`pgconfig.json` is in scope); add `--update` (re-resolve ranges past the lock).

*Gate:* unit tests over the pipeline with an injected fake `HttpGet` (happy path, integrity mismatch,
offline-with/without-lock, shadowing, unknown version). Full `build-and-test.ps1` green; conformance
byte-identical (in-box resolution path unchanged for the four bundled targets).

## Slice 4 — Remove the hardcoded target fallbacks (PRD D5)

- Delete the `{csharp ".cs", typescript ".ts"}` default pair (`main.cpp:535-537`) and the watch twin
  (`main.cpp:395`): no config + no `--target` ⇒ diagnostic listing loaded targets. Extensions from
  `fileExtension()` everywhere.
- `referenceTarget()` — derive the check/analysis reference backend by **capability completeness** (first
  loaded backend with full §3.A coverage and no `false` stances), replacing the literal `"csharp"` at
  `main.cpp:676` and `main.cpp:388-389`.
- LSP (`main.cpp:930-931`): configured targets when present; otherwise `referenceTarget()` for diagnostics
  + all loaded targets for reserved-name checks.

*Gate:* unit tests for `referenceTarget()` derivation (stub backends with gaps are skipped); script checks
that no-config `build` errors usefully and `--target python` still works; conformance + watch-protocol
gates green (harness invocations always pass explicit targets/configs).

## Slice 5 — Fake-registry script gate (end-to-end proof)

`tests/registry/run-registry.ps1` + a ~60-line Node HTTP server (Node already required by the harness)
serving fixtures generated at test time from the repo's own `plugins/` (`npm pack`-shaped tarballs +
abbreviated packuments, integrity computed by the fixture builder). Wired into `build-and-test.ps1`:

1. Fresh `POLYGLOT_CACHE`-overridden cache dir (add this env override to `pluginCacheDir()` for tests) +
   a temp project whose `pgconfig.json` declares `@mintplayer/polyglot-target-python: ^0.0.0-…` against
   `POLYGLOT_REGISTRY=http://127.0.0.1:<port>` → `polyglot build` downloads, writes
   `pgconfig.lock.json`, emits Python; diff against the in-box python plugin's emission (byte-equal —
   lockstep fixture).
2. Kill the server → clean cache-hit rebuild (offline, lock-first) succeeds.
3. Tamper the cache manifest → build refuses that entry and (offline) fails with the right message.
4. Serve a tarball whose bytes don't match `integrity` → refused.
5. `--target` override + no-config error-path assertions from slice 4.

*Gate:* the new script stage green on Windows CI + WSL (`cmake` leg), full suite green.

## Slice 6 — Docs + release wrap-up

- `docs/design/plugins-and-targets.md` §6/§6.1/§6.3: mark the download flow **built**, record the
  `registry.json` supersession (PRD D3) and the real cache paths; `docs/design/json-plugins.md` §5.4
  likewise.
- `docs/prd/POLYGLOT_PRD.md` — add design record **§4.20** (build-time plugin auto-download, lock-first
  resolution, transport ladder, D3 supersession).
- Root `CLAUDE.md` status pointer + master `docs/prd/PLAN.md` P30 entry updated to as-built.
- PR carries `release:minor` (lockstep, tag-driven — no source bump).

*Gate:* `/build-and-test` green end-to-end; manual smoke on a real network: clean machine cache,
`pgconfig.json` declaring `@mintplayer/polyglot-target-python@<latest tag>` → `polyglot build` emits
Python with no npm/node on PATH (the real issue-#30 acceptance).

## Slice 7 — MSBuild multi-target (**follow-up-gated** — likely its own issue, per issue #30)

Design sketch only, so the follow-up starts warm: drop `--target csharp`
(`MintPlayer.Polyglot.MSBuild.targets:69`) in favor of pgconfig-driven targets; keep the single-Exec /
single-stamp shape (the P28 incrementality fix is load-bearing — per-file batching caused CS0101
miscompiles); `.cs` outputs keep flowing into `@(Compile)` via the existing glob; non-`.cs` outputs need a
home — an `outputs: { <target>: <dir> }` mapping in `pgconfig.json` (relative to the config) that the CLI
honors directly, so MSBuild never learns about languages (the "hosts pass files and paths, never
languages" intent). Open questions to settle in that issue: default non-C# out dir, `@(FileWrites)`/clean
semantics for outputs escaping `obj/`, and whether `check`-only targets belong in `targets`.

---

## Risks & mitigations

- **Linux transport ladder** (no libcurl and no curl binary) — rare in practice; the diagnostic names the
  fix and `file:`/pre-warmed-cache alternatives; vendored-TLS fork recorded in the PRD if reality bites.
- **Vendored inflate correctness** — mitigated by using a battle-tested public-domain decoder verbatim +
  golden-tarball tests; only decode, bounded output.
- **LSP jank from network** — lock-first + per-config-generation memoization keeps the steady state
  network-free; first-ever resolution of a new dependency is the only slow path, and it reports as a
  diagnostic rather than blocking.
- **Behavior changes** (no-config default pair, bare-name cache probe) — deliberate clean cutovers,
  documented in PRD §8 with exact diagnostics guiding users to the new shape.
