# P41 — Deterministic per-module output (#76) + IDE-visible generated sources (#68) — implementation plan

> Design contract: `PRD.md`. Evidence: `ANALYSIS.md`. Issues: #76, #68.
> **Status (2026-10-19):** designed and **grilled** (nine questions, D1–D14 locked; log at the bottom).
> Slice 0 spikes not yet run. Maintainer directives: good fixes over quick fixes; **the GitHub workflow
> must finish in 5 minutes**.

**Build discipline (the maintainer's standing rule).** Do **not** build or run tests locally. Implement
every slice, commit per slice, push once, and read CI (`gh pr checks` / `gh run view --log-failed`). The
most a slice may get locally is a single compile when it truly cannot be avoided. Spikes in slice 0 compile
**tiny standalone snippets** with `csc`/`tsc`/`python`/`php` or run the **already-built** exe; they never
build the solution.

**The POSIX leg is CI's job.** Slices 1–3 touch Core only (no `#ifdef`, no filesystem), so `ci.yml`'s
ubuntu build is the floor. Slice 6 touches the MSBuild `.targets` + PowerShell harness, which CI does not
run today (D12).

**One PR.** Every slice — Core, IR, unit goldens, CLI smoke, MSBuild targets, harness, docs, PLAN.md
entry — lands together. Nothing is deferred to a follow-up.

---

## Slice 0 — spikes (throwaway, minutes each, no project build)

| ID | Question | How | Decides |
|---|---|---|---|
| S1 | Per-body numbering is CS0136/CS0128-safe on C#: two sibling `for (int i = 0, __end0 = F(); …)` loops in one method compile; a nested loop using `__end1` compiles; a nested loop *reusing* `__end0` fails; a lambda inside the body declaring `__end2` compiles | `csc` (or `dotnet build` of a 1-file console project in the scratchpad) on a hand-written snippet | D1 (lambdas share the allocator), rejection of lowest-unused |
| S2 | TS has no CS0136 analogue: same shape, `npx tsc --noEmit --strict` on a snippet | tsc | D1 is strictly safe on TS |
| S3 | Python/PHP function scope: an inner loop's `__end0` would overwrite the outer's if reused — confirm the aliasing | `python -c` / `php -r` | rejection of lowest-unused is a miscompile avoidance, not taste |
| ~~S4~~ | ~~Hoisted `__pg_lam<N>` for a global initializer~~ — **answered during the grilling by reading the code**: `Hoister::run` (`hoist_block_lambdas.cpp:14-23`) visits exactly functions, extensions, record methods, class methods and init blocks, never globals, so the per-body reset has no module-level edge | — | D3 |
| S9 | **Which construct made `tests_main.cpp` take ~9.5 min at g++ -O2 after P40?** Compile the file once with `-ftime-report` (or bisect by `#if 0`-ing the P40 band) **on a CI runner via a throwaway workflow_dispatch job**, not locally | CI | D14's split lands the offender in its own unit and records the cause |
| S5 | Determinism probe today: the issue repro with `a.pg`/`b.pg` swapped in `include` order — which `lib.ts` wins, and what `build --check` exits | Debug exe, scratchpad | pins the "emit once" masking argument (D4) and the smoke test's expected exit codes |
| S6 | Golden blast radius: which pinned `__end/__opt/__w/__c` numbers in `tests_main.cpp` encode a **cross-declaration** sequence (expectation changes) vs a within-body one (unchanged) | grep + read the ~25 sites | the size of slice 4 |
| S7 | **#68 still reproduces on the current package?** Pack the MSBuild package from the Debug exe into a scratchpad feed, consume it from a tiny 2-file project, `dotnet build`, then `dotnet msbuild -t:CompileDesignTime -p:DesignTimeBuild=true -getItem:Compile` before and after adding `c.pg`; finally the maintainer's interactive VS check | `dotnet` + scratchpad; VS check is the maintainer's | D9: whether slice 6 is built at all |
| S8 | Outputs-list → evaluation-time items parsing: does `$([System.IO.File]::ReadAllText('…'))` split on `%0D%0A`/`%0A` yield correct `Compile` items with `;`-free and spaced paths, and does the `.cs`-under-`PolyglotOutDir` metadata filter work at evaluation? | `dotnet msbuild -getItem:Compile` on a scratchpad `.proj` | D10's exact XML |

S5 and S6 are grep/read-only or run the existing exe; S7 costs one `dotnet pack` + two small builds; S9
runs on a GitHub runner.

## Slice 1 — lower once per root, partition the lowered IR (D13) + per-body allocator (D1, D2)

- `compile()`'s partitioned emit (`compiler.cpp:1593-1696`): lower the closure **once**; for each output file
  build a slice of the lowered `ir::Module` containing the decls whose `originModule` is kept (plus the
  per-file prelude slot), and run `pruneUnreachablePrelude` on the slice for `preludePerFile` targets.
  Delete the "lowering is pure … re-lower the whole unit" path and its comment.

- `Lowerer`: reset `tmpCounter_ = 0` at every body entry (functions, extension functions, methods,
  accessors, constructors, class `init`, global initializers, record/union methods). Lambdas do **not**
  reset. Identify the body-entry points by reading `Lowerer::run` (`lower.cpp:336-440`) and the per-decl
  lowering functions it calls.
- IR: remove `ir::Module::tempsUsed` (`ir.hpp:742`); add `int tempsUsed` to each body-bearing node. Update
  the comment at `ir.hpp:737-741` to state the invariant per body.
- Emitter: set `tmp_` from the body's `tempsUsed` on body entry (today `emitter_base.cpp:1999` seeds once
  per module). Find every body-walking entry in `InterpretedEmitter` (function/method/accessor/init/global
  emission) and seed there.

## Slice 2 — sibling counters (D3)

- `Hoister::counter_` → per body (`hoist_block_lambdas.cpp`), with S4's answer for globals.
- `CsScopeLegalizer::seq` → per body (`lower.cpp:1851-1888`).
- `boundTmpSeq_`, `doWhileSeq_` → reset at body entry in the emitter (`emitter_base.hpp:383-384`).

## Slice 3 — attribute imports derived from the slice (D5) + driver dedupe (D4)

- Remove `attrImports_` collection from lowering (`lower.cpp:264-265, 482, 551`). After slicing, a small
  pass walks the slice's declarations (decls, parameters, members, enum cases) for Tier-1 attribute
  bindings with an `importLine` and fills `module.attrImports` for that file; `module.attrImportsBlock`
  (`emitter_base.cpp:1490-1492`) is unchanged.
- CLI driver (`main.cpp:585-660` `emitOne`, `:1156-1194` `buildGroup`): key produced modules by
  (canonical source path, target); a module already produced this build is skipped before `sinkPut`. The
  sink's "conflicting output" stays for different sources to one path and for origin-info divergence.
  Re-point the P40 review-3b smoke assertion (`run-cli-smoke.ps1:440`) at a genuine two-source conflict.

## Slice 4 — unit goldens + new unit pins (D7, D8)

- Renumber the cross-declaration pins found in S6.
- Add the three unit cases from PRD §4.7 (lib via A / via B / alone byte-identical on TS, C#, PHP; `!`
  shift; attribute import not leaking; sibling bodies both `__end0`, nested lambda temp continues).

## Slice 5 — CLI smoke (D8)

- `tests/cli/run-cli-smoke.ps1`, next to P40 review 3b (`:433-440`): the issue's three files on
  `["typescript","csharp","php"]`; `build` → exit 0, no "conflicting output", `gen/lib.ts` has `__end0`;
  `build --check` → exit 0.
- The acceptance assertion "each non-entry module of a multi-module conformance program equals the module
  compiled alone": one loop over `tests/conformance/programs/*` with imports, using the CLI, inside the
  smoke leg (no new leg).

## Slice 6 — issue #68 (D9–D11; built regardless, S7 is the acceptance test)

- D10's `ItemGroup` after `.targets:46` (outputs-list-derived `Compile` items with a marker metadata, exact
  XML from S8) + `Remove`-then-add in `_PolyglotAddGenerated`, documented next to the review-2b comment
  (`.targets:212-214`). A folder glob is rejected.
- D11 checks in `run-nuget.ps1`'s Stale fixture (`:186-262`): eval-time set lists `polyglot\a.cs`; add `c.pg`
  → green, no CS2002; delete `a.pg` → no CS2001, `a.cs` gone; design-time simulation lists the new file.
- S7 result (before/after, current package) recorded on #68; the maintainer's interactive VS check is the
  final word.

## Slice 7 — CI under the 5-minute wall (D12, D14)

- **Split `tests_main.cpp`** into per-area translation units (the flat `check()` harness, `has()`,
  `compileStd` and the resolvers move to a shared `test_support.hpp/.cpp`; each area file exposes one
  `run_<area>()` called from a small `tests_main.cpp`). S9's offender gets its own unit. Update
  `.vcxproj`, `.vcxproj.filters` and `CMakeLists.txt` (`check-buildfile-parity.ps1` guards the pair).
- **Unoptimized test binary** on both toolchains (`/Od` for the tests project in Release, `-O0` for
  `polyglot-tests` in CMake). The CLI stays Release-optimized.
- **`ci.yml` build-once fan-out:** `linux-build` builds and uploads `build/polyglot`, `build/polyglot-tests`,
  `build/plugins/**` (`actions/upload-artifact@v4`); leg jobs `needs: linux-build`, download the artifact,
  install only their own toolchain, and run the runner scripts directly (`tests/*/run-*.ps1 -Cli build/polyglot`)
  — matrix: `unit+watch+registry`, `cli-smoke+refusals+lsp`, `nuget`, `conformance` (kept only if its first
  measured run fits). The coverage-attribution step and the smoke transpile move into the leg jobs.
  `timeout-minutes: 5` on every job; `concurrency: { group: ci-${{ github.ref }}, cancel-in-progress: true }`.
- NX stays the local orchestrator; the runners stay directly invocable (the standing contract).
- Acceptance: the PR's own CI run finishes under 5 minutes cold.

## Slice 8 — docs

- D6 paragraph (what is / is not a function of a module's own source).
- `docs/prd/PLAN.md` P41 entry (status, decisions, release note: emitted temp names renumber; third-party
  plugin authors: `fresh` temps now restart per body — no manifest key change, no schema bump).
- CLAUDE.md "Next step" pointer.

## Slice 9 — end gate (CI)

Push once; `ci.yml` is the gate. Read failures with `--log-failed`; fix only that.

---

## Decision log (grilling round 2026-10-19, nine questions, all locked)

| ID | Decision | Status |
|---|---|---|
| D1 | Temporaries numbered per **body**; lambdas share the enclosing allocator (Q1) | locked |
| D2 | `tempsUsed` moves from `ir::Module` to each body node; emitter seeds `tmp_` per body (Q2) | locked |
| D3 | `__pg_lam`, `__cs`, `__pg_t`, `__dw` counters per body too; one definition of "body" in the IR (Q3) | locked |
| D13 | Lower each root's closure **once**; the partitioned emit slices the lowered IR by origin module (Q4, "good fix over quick fix") | locked |
| D4 | **Revised (Q5, delegated: "most efficient and professional"):** per-root compiles stay; a module already produced for a target this build is not re-emitted; "conflicting output" stays for different sources to one path; the G1 proof lives in tests and `--check` | locked |
| D5 | **Revised (Q6):** Tier-1 attribute import lines derived from the file's slice, not collected during lowering | locked |
| D6 | Set-dependent naming (overload mangling, extension disambiguation, reach-driven records, prelude file) documented as by-design | locked (not asked; follows D16) |
| D7 | Golden strings renumber; no compat shim | locked |
| D8 | Unit + CLI-smoke pins for per-module determinism; conformance unchanged | locked |
| D9 | **Revised (Q7):** #68 evaluation-time items built regardless; S7 re-verification is the acceptance test | locked |
| D10 | Outputs-list-derived evaluation-time `Compile` items, `Remove`d and re-added by the target; folder glob rejected | locked |
| D11 | #68 checks in `run-nuget.ps1` (eval set, add, delete → no CS2001/CS2002, design-time simulation) | locked |
| D12 | **(Q8):** `nuget` + cli-smoke/refusals/lsp legs join `ci.yml`, under the 5-minute wall | locked |
| D14 | **(Q9):** split `tests_main.cpp` + unoptimized test binary (root cause of the 11-min run); build-once fan-out matrix with `timeout-minutes: 5`; conformance in PR CI only if it fits | locked |
