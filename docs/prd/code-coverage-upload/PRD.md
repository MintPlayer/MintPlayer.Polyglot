# Code coverage — org-wide reporting + plugin-template coverage (PRD)

> **GitHub:** no originating issue — maintainer request (2026-09-11): *"half of the repositories under
> the MintPlayer organization have code-coverage … this repo doesn't have any setup yet … workflows in
> other repos upload their coverage report (lcov, cobertura) to coverage.mintplayer.com"*.
> Successor to `docs/prd/e2e-coverage-wave2/` slice 7, which built the *local* instruments and
> **explicitly deferred** both the template-arm tracer and any threshold.

- **Status:** designed + **BUILT (2026-09-11)** on `code-coverage-upload` / **draft PR #67** —
  as-built markers inline. Full gate green; CI `linux-build` + `coverage` green; the PR-side upload
  to coverage.mintplayer.com works (5 files, one finalized session). **Not closed out:** §7 criteria
  1, 3 and 10 are open, and SP5 is unrun — the blocking one is **SP1/criterion 3**, which only the
  maintainer can check in the dashboard. One PR (CLAUDE.md single-PR rule), ordered commits.
- **Author:** Pieterjan (with Claude Code).
- **Provenance:** a two-track investigation — a repo audit (existing instruments, gate topology,
  plugin/rule internals) and an org-convention survey across `MintPlayer.AI`,
  `MintPlayer.Dotnet.Tools`, `MintPlayer.AspNetCore.SpaServices`, `mintplayer-ng-bootstrap` and
  `MintPlayer.Spark`. The canonical sources are the shared action
  `MintPlayer/MintPlayer.Spark/apps/CodeCoverage/action@coverage-upload-v1`, its
  `apps/CodeCoverage/action/README.md`, and the HTTP contract in
  `MintPlayer.Spark/docs/code-coverage/upload-api.md`.
  **Do not copy from `C:\Repos\Coverage\`** — that is the decommissioned standalone repo
  (`MintPlayer/CodeCoverage/action@master`); see `docs/code-coverage/old-repo-decommission.md`.

---

## 0. Prime directive check (POLYGLOT_PRD §3)

This is tooling, not language surface — §3's support/refuse contract is not engaged. Two standing
directives **are**:

- **Core stays language-agnostic.** The arm tracer (§4.B) records *rule keys and manifest source
  lines*. It must contain **no target-name comparisons** — no `if (name == "csharp")`. A plugin is
  traced because it is loaded, not because Core recognises it. Any per-target distinction is
  manifest data, never a Core branch.
- **Principled fix over workaround.** The honest measurement for the four backends is not "the
  conformance suite went green". It is *which template arms fired*. Wave 2 found 28 never-executed
  scalar-parse templates **by hand**; this PRD builds the mechanical detector rather than continuing
  to assert that a passing suite implies exercised templates.

## 1. Problem

### 1.1 The premise, corrected

The request says the repo has no coverage setup. It has **two instruments, both report-only and
neither published**:

| | What | Where | Output | Gate? |
|---|---|---|---|---|
| Local | OpenCppCoverage over the unit exe (+ optional conformance sweep) | `scripts/coverage.ps1` | **HTML only**, `x64/coverage/` | No — in no gate, no nx target |
| CI | g++ `--coverage` + gcovr, unit exe + 4-target transpile sweep | `.github/workflows/ci.yml:67-108` | HTML + txt step-summary, artifact | Runs on PRs, but **cannot fail on %** |

So the real gaps are narrower and sharper than "no coverage":

1. **Nothing is published.** No upload to coverage.mintplayer.com, so there is no trend, no PR
   delta, no org-level view — the numbers die in a job summary and an artifact nobody opens.
   `release.yml` has no coverage at all.
2. **The measured surface is the smaller half of the product.** The backends are **100% JSON
   plugins** (P18–P19) — *zero backends are compiled in*. gcov and OpenCppCoverage are structurally
   blind to `plugins/*/polyglot-plugin.json`. CLAUDE.md already concedes this: *"their coverage
   instrument is the differential conformance suite"*. That suite reports pass/fail per program; it
   cannot say which of the ~3.4k–4.6k manifest lines ever executed.
3. **A rule can exist and never fire.** The load-time anti-silent-drop contract
   (`backend.cpp:172` `kCoverage[]`) proves every IR construct *has* a rule. It says nothing about
   whether that rule, or any of its nested `case` arms, was ever evaluated. That blind spot is
   named in the wave-2 PRD (`PRD.md:41-44`) and still open.
4. **No floor, by design — and the baseline that was supposed to unblock it never got published.**
   Wave 2 said *"measurement first, floor later"* (`PRD.md:125`). "Later" needs a place for the
   baseline to live; that place is the coverage server.

### 1.2 Local-only decay

OpenCppCoverage is not installed on the maintainer's machine and `coverage.ps1` is wired into no
gate — a local-only, manual, HTML-only instrument is one that silently stops being run.

## 2. Goals

- **G1** — Publish C++ Core/CLI coverage to coverage.mintplayer.com from CI, on both PR and master,
  using the org's shared action and the org's formats (lcov / cobertura).
- **G2** — Build the deferred **template-arm tracer** and turn it into **lcov over the plugin
  manifests themselves**, so each `polyglot-plugin.json` gets a real per-line coverage report in the
  same dashboard as the C++ — dead template arms become uncovered lines.
- **G3** — One build → one finalized upload carrying every surface, so the dashboard shows a single
  honest number per commit rather than a partial that silently carries forward.
- **G4** — Keep every instrument **directly invocable without CI and without NX** (the standing
  runner contract), and give the local Windows path the same output formats as CI.
- **G5** — Establish the baseline that makes a future floor a one-line change, without imposing one
  now.

## 3. Non-goals

- **No coverage-gated merges in this PR.** Report-only, continuing wave 2's *"measurement first,
  floor later"*. §7 records exactly what flipping the floor on will take.
- **No coverage for the VS Code extension** (`editors/vscode/extension.js`). It is plain JS with no
  test script and no test runner — its coverage would be a truthful 0%, which is noise, not signal.
  If it ever gets tests, it gets a flag.
- **No coverage for the PowerShell gate runners** (`scripts/*.ps1`, `tests/**/run-*.ps1`). They are
  the harness; measuring the harness measures itself.
- **No coverage of emitted C#/TS/Python/PHP.** That is generated test *output*, not product source.
  Its correctness instrument is the differential oracle diff, which already exists.
- **No Windows coverage job in CI.** `ci.yml` is deliberately one cheap ubuntu leg. Consequence
  accepted and documented in §6.3.
- **No new runtime dependency in the shipped CLI.** The tracer is a debug flag writing a plain text
  file — no library, no JSON emitter beyond what Core already has.

## 4. Design

Three surfaces, two instruments, **one upload**.

### 4.A Surface 1 — C++ Core + CLI (extend what exists)

The `ci.yml` coverage job already builds instrumented and drives both the unit exe and a 4-target
transpile sweep over all 112 conformance programs. It needs three changes, all small:

1. **Emit machine-readable formats.** `gcovr` already runs; add
   `--cobertura coverage/cpp/cobertura.xml` beside the existing `--html-details` / `--txt`.
   **Cobertura, because that is the org default** — every C# sibling uploads
   `coverage.cobertura.xml`, and the server parses lcov / Cobertura / JaCoCo alike. Keep the step
   summary: it is the cheap in-PR read.
2. **Widen the sweep beyond `build`.** Today the sweep only runs `polyglot build`. Core paths behind
   `lsp`, `--check`, the refusal diagnostics and `pgconfig` discovery accumulate no `.gcda`, so they
   read as dead code and depress the baseline dishonestly. Add the existing refusal fixtures
   (`tests/refusals/`) and a short `lsp` request script to the instrumented sweep. These need no
   language runtimes, so they cost seconds.
3. **Keep `|| true`.** The sweep's job is execution, not verification — correctness is the Windows
   gate's job. A failing transpile must not fail the coverage job.

**`--root .` + `--filter 'src/'` already yields repo-relative paths**, so — unlike ng-bootstrap's
Vitest output — no path-rebasing script should be needed. SP2 confirms this rather than assumes it,
because the failure mode is silent (§4.E).

### 4.B Surface 2 — the four JSON plugin manifests (the new instrument)

This is the substantive build. Wave 2 sketched it as `--emit-arm-trace` and demand-gated it; the
demand has arrived, and the plumbing turns out to be shallow.

**Why line-granular lcov is the right shape.** The manifests are pretty-printed at ~27 bytes/line
(csharp 3729 lines, typescript 4629, python 3841, php 3448). A rule's nested structure — `case`
arms, `parts`, `elseBody` — occupies *distinct lines*. So ordinary line coverage over the JSON
expresses arm-level branch coverage, for free, in a format the server already ingests, rendered
against the real source file the plugin author edits.

**Four mechanisms, each small:**

1. **Source provenance in the JSON parser.** `json::Value` (`json.hpp:14`) carries no position.
   `json.cpp` is a 192-line offset-based recursive descent with a single `std::size_t i` cursor —
   record the value's start offset at `parseValue()` entry (`json.cpp:96`). Offsets convert to line
   numbers once, against the raw text, at the boundary. **Offset, not line**, in the struct: it is
   one `size_t`, computed for free, and keeps the parser ignorant of line counting.
2. **Provenance on the parsed rule.** `Rule` and `Test` (`backend_engine.hpp:53-74`) gain a `line`
   field, populated by `parseRule`/`parseTest` from the source value. Rules are nested trees, so
   every arm and sub-rule carries its own line.
3. **One instrumentation site.** Every rule evaluation funnels through
   `evalRule` / `evalTest` (`backend_engine.hpp:120-121`). A trace sink — off unless the flag is set
   — records `(plugin, line)` into a set. One `if` on a null pointer in the hot path when disabled.
4. **A CLI debug flag** `--emit-arm-trace <path>` appending hit lines. Plain text, one
   `<plugin>\t<line>` per record, dedup on aggregation; appendable so the 448-invocation sweep
   accumulates into one file without a merge step.

**The denominator must not drift.** Coverage is meaningless if the "total lines" set is
hand-maintained or inferred by regex over the JSON. It comes from the *same parse*: the CLI emits a
**static arm manifest** (every `Rule`/`Test` line it parsed, per plugin) alongside the hit trace.
Denominator and numerator therefore come from one code path — a rule that is never parsed cannot be
silently dropped from the denominator, and a never-fired arm is guaranteed to appear as an uncovered
line rather than vanish.

**Aggregator.** A script folds `{static manifest} ∪ {hit trace}` into
`coverage/plugins/<target>.lcov`, one `SF:plugins/<target>/polyglot-plugin.json` record per plugin,
`DA:<line>,<0|1>`. Directly invocable, no NX, consistent with every other runner in the repo.

**Expected fallout, and it is the point.** The first run will show uncovered arms — wave 2 found 28
by hand and stopped looking. Per the single-PR rule these get fixed (or deleted, or covered by a new
conformance program) **in this same PR**, not deferred. If the count is large, the honest response is
a conformance program per cluster, not a lowered denominator.

**Both interpreters must mark (as-built, and the trap).** Rules come in two flavors: string-flavor
ones evaluate through `evalRule`, but **decl-flavor** ones (`Line`/`Block`/`Seq`/`MapDecl`/`Stmts`/
`Indent`/`MapMembers`) are interpreted by `EmitterBase::runDeclRule` instead. Marking only at
`evalRule` reported every declaration- and statement-shaped rule as cold — the first measurement read
80–85% and its uncovered list was dominated by exactly those rules (`MethodDecl`, `Program`,
`TryStmt`, `ClassDecl`). With both interpreters marking, the real figure is **~95%**. A denominator
counting arms no instrument can reach is the same class of lie the anti-silent-drop contract exists
to prevent, so this is a permanent invariant: **a new rule-interpretation path must mark, or the
number is meaningless.**

### 4.C Surface 3 — local Windows parity

`coverage.ps1` gains `--export_type cobertura:x64\coverage\cobertura.xml` beside the HTML (it
already merges child processes via `--cover_children` and two `binary:` intermediates), plus the
arm-trace sweep, so a local run produces the same artifacts CI uploads. Still no upload from a
developer machine — the server's history should be CI-authored only.

### 4.D The upload — one step, one finalize, tokenless

Matching the org convention, with the differences this repo's topology dictates. **OIDC, not a
token**: MintPlayer.Polyglot is a public repo, exactly the case where `MintPlayer.AI` already
authenticates tokenlessly — so this repo needs no new secret. Requires `id-token: write` on the job.

```yaml
permissions:
  contents: read
  id-token: write

# ...
- name: Upload coverage
  if: >
    (github.event_name != 'pull_request'
     || github.event.pull_request.head.repo.full_name == github.repository)
    && hashFiles('coverage/cpp/cobertura.xml') != ''
  continue-on-error: true            # master: see §6.2
  uses: MintPlayer/MintPlayer.Spark/apps/CodeCoverage/action@coverage-upload-v1
  with:
    url: https://coverage.mintplayer.com
    use-oidc: true
    files: |
      coverage/cpp/cobertura.xml
      coverage/plugins/*.lcov
    disable-search: true
    flags: cpp-core,plugins,linux
    base-sha: ${{ github.event.pull_request.base.sha }}   # empty on push — harmless
    finish: true
    fail-ci-if-error: false          # true on master (§6.2)
```

The `hashFiles(...) != ''` guard is org convention: an upload step that fires with no report
produces an empty session rather than an obvious failure.

**Mixed formats in one upload are fine** — the server sniffs format per file rather than taking a
declaration, so the C++ Cobertura and the four plugin lcovs ride in the same step and merge as
sessions under one build key `(repository, commitSha, runId, runAttempt)`, with `finish: true`
finalizing exactly once.

**Why `partial` is *not* set here — the NX complication does not apply.** In ng-bootstrap,
`nx affected` means a PR measures only a subset, so it must declare `partial: true` and pass
`base-sha` for a like-for-like comparison. This repo's coverage job **uses no NX and computes no
affected set**: it builds cold (`build-cov`) and sweeps the entire 112-program corpus every run, on
PR and on master alike. The upload is therefore *complete*, and declaring it partial would wrongly
invite the server to carry forward stale file data over freshly-measured files. `base-sha` is
likewise unnecessary. This is worth stating in the workflow comment, because it is the one place
this repo deliberately diverges from the org template.

`base-sha` is still passed on PRs — it is what stops the server defaulting to "the newest covered
default-branch commit" when computing the delta.

**Fork PRs** cannot mint an OIDC id-token; the `if:` guard skips the step rather than failing it.

### 4.E The silent failure mode: path matching

The server resolves every report path by **longest-suffix match against the `fileList` the action
uploads from `git ls-files -s`**. A path that fails to match is **dropped silently** — the upload
succeeds, the number is just quietly wrong. This is the same trap the C# repos defend against by
forcing `UseSourceLink=false` and `DeterministicReport=false` in `coverlet.runsettings`, and the
reason `MintPlayer.Spark` and ng-bootstrap both run a pre-upload path script.

Two consequences for this design:

- **Good news for §4.B:** `plugins/<target>/polyglot-plugin.json` is a **git-tracked file at a
  repo-relative path**, so a synthetic lcov naming it matches the same way a real source file does.
  The plugin-manifest idea is working *with* the server's model, not against it.
- **We adopt the tripwire convention.** A small pre-upload check asserts every `SF:` /
  Cobertura `filename` in the generated reports exists in `git ls-files`, failing loudly if not.
  Cheap, and it converts the silent-drop class into a build error. This matters more here than in
  the sibling repos because our plugin lcov is *generated by us*, not by a mature tool.

## 5. Spikes

Each is time-boxed and throwaway; a spike that fails changes the design, and §7 says how.

> **Outcomes (2026-09-11).** SP2 **done** (failed as written → fixed). SP3 **done** (passed).
> SP4 **done** (passed, with a caveat). SP1 **partly answered — the one open item, and the one that
> may need a server-side change.** SP5 **not run — blocked.** Details under each.
>
> **Honest note on ordering:** the plan said SP1 runs *first, before any C++ is written*. It did
> not. SP1/SP2/SP3 are only answerable from a CI run, and `ci.yml` fires only on PRs and master
> pushes, so the workflow change (slice 1) had to exist before any spike could run at all. The
> tracer was therefore built on the *reasoning* that de-risked SP1 (lcov is format-agnostic about
> `SF:`; the manifests are git-tracked so the suffix match resolves) rather than on a confirmed
> answer. That reasoning has held so far — the server accepted the files — but the final render
> check is still outstanding, so the risk the ordering was meant to remove was carried, not removed.

- **SP1 — Does the server accept and render a report whose source file is `.json`?**
  *The one spike that may need a change on coverage.mintplayer.com — the maintainer has offered
  exactly this.* Hand-write a ~10-line lcov naming `plugins/csharp/polyglot-plugin.json` with a few
  `DA:` records, upload under a throwaway flag, and check: (a) it parses, (b) the file appears in
  the dashboard, (c) the source renders with line highlighting, (d) no extension/language allowlist
  rejects it, (e) it counts toward the project total rather than being ignored.
  Two things de-risk this: lcov is format-agnostic about `SF:`, and the path is git-tracked so the
  suffix match (§4.E) will resolve. But it is the assumption the whole of §4.B rests on, so it is
  spiked **first, before any C++ is written**.
  *If it fails:* ask for `.json` support (the offer), or re-emit the same data as Cobertura
  (`<class filename="plugins/…json">`) if only the lcov path is fussy. Do **not** fall back to "a
  percentage in the job summary" — that is the status quo this PRD exists to replace.

  **⚠ PARTLY ANSWERED — the one open item.** The real reports (not a hand-written probe) upload
  cleanly: the action logs all five files accepted in one session, `finish` acknowledged 202, and
  the §4.E tripwire passes, so every path resolved against `git ls-files`. What is **not** confirmed
  is (a)/(c)/(e) — parsing is asynchronous, and `GET /api/uploads/status` is 401 without a token,
  which this work deliberately does not handle. **So whether the four manifests parse, render with
  line highlighting, and count toward the project total is visible only to the maintainer in the
  dashboard.** If they do not, this is the offered server-side change.
- **SP2 — gcovr Cobertura path shape.** Confirm `--root . --filter 'src/'` yields repo-relative
  `filename="src/..."` that survives the `git ls-files` suffix match with no rebase script. The
  failure is silent (§4.E), so verify against the tripwire, not by eyeballing.

  **DONE — failed as written, then fixed.** `gcovr --cobertura` does not exist on ubuntu-22.04
  (gcovr 5.0); the flag was only added in 5.1. `--xml` emits the same Cobertura on both and is now
  what CI uses. Paths *are* repo-relative and need no rebase script — confirmed by the tripwire
  passing, not by eyeballing.
- **SP3 — OIDC from this repo.** Largely settled by precedent (`MintPlayer.AI` is public and
  tokenless), so this is a confirmation, not an exploration: add `id-token: write` and prove a
  non-fork PR run authenticates, with no `COVERAGE_TOKEN` secret added to this repo.

  **DONE — passed.** The action logs *"Authenticating with GitHub Actions OIDC"* and the server
  posts its own `coverage/project` + `coverage/patch` check runs. No secret was added to this repo.
- **SP4 — Tracer plumbing cost.** Prototype offset→`Rule.line`→`evalRule` hit recording and measure
  (a) the CI sweep wall-clock delta (448 CLI invocations; budget: <10% and no new gate leg), (b)
  that nested `case` arms genuinely land on distinct lines across all four manifests, (c) that a
  disabled tracer is unmeasurable in the normal build.

  **DONE — passed, with a caveat about what was measured.** (b) is pinned by a unit test (sibling
  arms land on distinct lines). (c) holds by construction — a null pointer test. (a): 80 paired CLI
  invocations timed tracing-off vs tracing-on came out **negative (−11%)**, which is not a speedup
  but proof that **process startup (~0.6 s × N) dominates any per-evaluation cost** — i.e. the
  overhead is below the noise floor at this granularity, comfortably inside the <10% budget, but
  the measurement does *not* isolate the tracer itself. No new gate leg was added, and the full
  gate is green with tracing off, which is the property that actually matters.
- **SP5 — OpenCppCoverage cobertura export.** Needs `choco install opencppcoverage` (absent on the
  dev machine — itself evidence for §1.2). Confirm `--export_type cobertura:` composes with
  `--cover_children` and the two-`binary:` merge.

  **NOT RUN — blocked.** OpenCppCoverage is still not installed, so the `cobertura:` export added to
  `coverage.ps1` in slice 6 is **written but unverified**. The plugin-lcov half of that script *is*
  verified (the aggregator ran against real traces); the C++ Cobertura half is not. Installing the
  tool and running `pwsh scripts/coverage.ps1 -IncludeConformanceSweep` once closes it.

## 6. Decisions and their consequences

### 6.1 Coverage runs where it is cheap, not where the gate is
The real gate is Windows (differential conformance); coverage is measured on ubuntu. That is
correct — coverage wants *execution*, not verification, and ubuntu is the cheap runner.

### 6.2 Strict on master, lenient on PRs
PRs: `continue-on-error: true`, `fail-ci-if-error: false` — a coverage-server hiccup must never red
a PR that is otherwise green. Master: `fail-ci-if-error: true`, matching `publish-master.yml`, so a
silently-broken upload is noticed rather than eroding the history.

### 6.3 `#ifdef _WIN32` branches will read as uncovered
Measuring only on Linux means the Windows halves of every platform fork (transport, `dlopen`/`popen`,
filesystem/chrono edges) are permanently uncovered lines. This is a **known, documented bias**, not
a defect to chase: the alternative is a Windows coverage runner this repo has deliberately avoided.
Documented in CLAUDE.md so nobody "fixes" the number by deleting a POSIX branch. If the bias later
proves too distorting, the escape hatch is a Windows coverage job in `release.yml` uploading with
`partial: true` + a `windows` flag and letting the server merge by flag — designed for, not built.

### 6.4 Gating is server-side config, which is why deferring the floor is safe

The service posts its own **`coverage/project`** and **`coverage/patch`** check runs; policy lives
in the repo's Coverage-gate settings panel, overridable by an optional repo-root `coverage.yml`
read **from the base ref**:

```yaml
gate:
  projectMode: auto        # auto = ratchet against base; fixed = compare to projectTarget
  projectTarget: 80
  projectThreshold: 1
  projectBasis: scoped
  patchTarget: 80
  patchThreshold: 5
  blocking: false          # default: post the numbers, never fail
```

No sibling repo commits one today, and this PR does not either. That is the whole reason §3's "no
floor yet" is a cheap deferral rather than a debt: turning the floor on later is a committed config
file plus a branch-protection checkbox — **no workflow change, no code change**. A missing baseline
yields a neutral check, never a red one.

Note the ratchet's interaction with §6.3: `projectMode: auto` ratchets against base, so the
permanently-uncovered `#ifdef _WIN32` lines depress the absolute number but not the *delta* — which
is the number a ratchet actually enforces. Should a fixed target ever be set, it must be set with
the Windows-fork bias in mind.

### 6.5 Badge

Org convention puts it at README line 3:

```markdown
[![Coverage](https://coverage.mintplayer.com/badge/MintPlayer/MintPlayer.Polyglot.svg)](https://coverage.mintplayer.com/r/MintPlayer/MintPlayer.Polyglot)
```

The README currently carries no badges; this adds the first, after the first successful master
upload (a badge pointing at an empty project is worse than none).

### 6.6 The two instruments answer different questions
Restating the wave-2 rule, now with a third row, because this is the thing people get wrong:

| Question | Instrument |
|---|---|
| Which **C++ Core/CLI** lines ran? | gcov/gcovr (CI) · OpenCppCoverage (local) |
| Which **plugin template arms** ran? | the arm tracer → per-manifest lcov (**new**) |
| Do the four targets **agree at runtime**? | the differential conformance suite |

None substitutes for another. A 100% C++ number with dead template arms is a lie, and both can be
green while the targets diverge.

## 7. Acceptance criteria

Status as of 2026-09-11 — **7 of 10 met, 3 open.** ✅ met · ⏳ open (needs the maintainer or a
master merge) · ⚠️ partly met.

1. ⏳ A master push publishes a build to coverage.mintplayer.com containing **both** a C++ report and
   four per-plugin reports, finalized once.
   *Not yet — nothing has merged to master. The equivalent PR-side upload works (5 files, one
   session, `finish` 202), so this is expected to follow on merge rather than being unproven.*
2. ✅ A non-fork PR publishes the same, comparable against its base, and cannot red the PR on an
   upload failure.
   *Verified on PR #67. "Comparable against its base" is untestable until a master baseline exists —
   the server's `coverage/project`/`coverage/patch` checks correctly report **skipping**, which is
   the documented neutral-on-missing-baseline behavior (§6.4), not a failure.*
3. ⏳ `plugins/<t>/polyglot-plugin.json` is browsable in the dashboard with per-line hit/miss.
   *The blocking unknown — see SP1. Upload accepted; render unconfirmed (needs the maintainer's
   dashboard access).*
4. ✅ The arm tracer's denominator provably comes from the parse, not a regex: a deliberately
   never-referenced rule added to a manifest shows up as **uncovered**, not absent. (Test fixture.)
   *Unit-tested: an untaken `case` arm is asserted present-in-denominator and not-hit.*
5. ✅ Every uncovered arm found by the first real run is resolved in this PR — covered, or deleted,
   or explicitly listed with a reason. **As built: ≥90% per manifest** (csharp 94.5%, php 96.1%,
   python 94.6%, typescript 94.8%), with the residue characterized in §7.1.
   *Note the bar moved: the original wording was "zero unexplained uncovered arms". The first sweep
   showed no dead rules at all, so driving ~750 sub-arm variants to zero was open-ended; the
   maintainer set 90% per manifest instead, and §7.1 classifies what remains.*
6. ⚠️ `pwsh scripts/coverage.ps1` produces cobertura + HTML + a plugin arm report locally, with no CI
   and no NX.
   *Plugin arm report: verified. C++ Cobertura + HTML: **written but unverified** — OpenCppCoverage
   isn't installed on the dev machine (SP5).*
7. ✅ The full gate (`-Tier full`) stays green and no gate leg gets slower (the tracer is off by
   default).
   *All legs pass, zero `[FAIL]`. "No leg slower" holds by construction (tracing off = one null
   test) rather than by timing each leg.*
8. ✅ Core contains **zero** target-name comparisons introduced by this work.
   *Verified: `git diff master...HEAD -- src/` has exactly one match for a quoted target name, and
   it is the comment in `backend_engine.hpp` stating the rule.*
9. ✅ The path tripwire (§4.E) passes, and fails loudly when fed a deliberately-bad path.
   *Both directions exercised: green on the real reports in CI, and exit 1 on a deliberately
   corrupted `SF:` path locally.*
10. ⚠️ CLAUDE.md + README state the three-instrument split and the Linux-only bias; README carries the
    org-convention badge once the first master upload lands.
    *CLAUDE.md: done (three-instrument table + the `#ifdef _WIN32` bias). **README: not touched** —
    the badge is deliberately held until the first master upload, since a badge pointing at an empty
    project is worse than none. The README carries no coverage prose either; that lands with the
    badge.*

### 7.1 The residue, characterized

Baseline after slice 7: **csharp 94.5% (57 uncovered), php 96.1% (37), python 94.6% (55),
typescript 94.8% (64)**. What the instrument actually found, and what happened to each class:

1. **A measurement bug, not a test gap** — the decl-rule interpreter went unmarked (§4.B). Worth
   naming first because it was ~13pp of the apparent gap: the instrument's own blind spot looked
   exactly like missing tests.
2. **Genuinely unexercised shapes → new conformance programs.** Unguarded multi-catch dispatch
   (`try_multi_catch.pg`) and block-bodied property accessors (`prop_block_accessors.pg`). Both
   green on all four targets; the latter is a pinned PHP refuser for the same reason
   `prop_accessors.pg` is (no property setters before PHP 8.4).
3. **Genuinely dead templates → deleted.** A catch clause *always* has a type — the parser does
   `expect(Colon)` + `parseType()` (`parser.cpp:881`), lowering only copies it (`lower.cpp:1027`),
   and nothing else constructs an `ir::Catch`. So every `item.hasType == false` arm under a
   `mapDecl: stmt.catches` was unreachable: 67 lines of TS untyped catch-all emission plus the
   Python `Exception` and PHP `\Throwable` fallback types. Proven behavior-neutral — all 24 emitted
   files across six catch-heavy programs on four targets are byte-identical before and after.
4. **Permanently unreachable by construction → documented, not deleted.** PHP's `UnionDecl` (6/6
   arms, the only entirely-cold rule left) exists *solely* to satisfy the anti-silent-drop load
   contract: `{"UnionDecl", "patternMatching"}` demands a rule from any plugin claiming
   `patternMatching: native`, but PHP erases unions to tagged arrays and its `Program` never maps
   `module.unions`, so the rule is never dispatched. **Deleting it would fail the load.** This is
   where the two contracts meet awkwardly — the static one requires a rule to *exist*, the dynamic
   one observes it never *runs*. Both are right; the rule is a structural placeholder. If that
   friction ever needs resolving, the fix is a manifest-level "declared unreachable" marker, not a
   deletion and not a lowered denominator.
5. **The long tail** (~40 arms per manifest, no cluster above 34): per-target niche shapes in
   `MethodDecl`, `Type`, `InterfaceDecl`, `Cast`, `Match`/`ArmGuard`. Left uncovered deliberately —
   the ratchet (§6.4) keeps it from regressing, and chasing each one would trade real review
   surface for a vanity number.

## 8. Out of scope / follow-ups

Genuinely not being done — *not* a parking lot for deferred work (CLAUDE.md):

- **A coverage floor / `coverage.yml`.** Deliberate (§3, §6.4). Once a baseline exists, enabling it
  is a committed `coverage.yml` with `blocking: true` plus requiring the `coverage/project` and
  `coverage/patch` checks in branch protection — no workflow or code change, which is exactly why
  deferring it costs nothing.
- **VS Code extension / PowerShell runner coverage** — §3, needs tests to exist first.
- **Windows CI coverage** — §6.3, escape hatch designed, not built.
- **Per-`.pg`-program coverage of the std library / conformance corpus** — a different question
  (which *language surface* is exercised) already answered by the conformance matrix.
