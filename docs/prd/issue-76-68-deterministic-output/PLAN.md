# P41 — Deterministic per-module output (#76) + IDE-visible generated sources (#68) — implementation plan

> Design contract: `PRD.md`. Evidence: `ANALYSIS.md`. Issues: #76, #68.
> **Status (2026-10-19):** designed; slice 0 spikes not yet run. Grilling round with the maintainer next.

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
| S4 | Hoisted `__pg_lam<N>` placement for a global initializer on Python: does a per-body restart put two `__pg_lam0` defs at module level? | read `hoist_block_lambdas.cpp:10, 26-28, 122-125` + run the Debug exe on a 2-global snippet with block lambdas | D3's global edge |
| S5 | Determinism probe today: the issue repro with `a.pg`/`b.pg` swapped in `include` order — which `lib.ts` wins, and what `build --check` exits | Debug exe, scratchpad | pins the "emit once" masking argument (D4) and the smoke test's expected exit codes |
| S6 | Golden blast radius: which pinned `__end/__opt/__w/__c` numbers in `tests_main.cpp` encode a **cross-declaration** sequence (expectation changes) vs a within-body one (unchanged) | grep + read the ~25 sites | the size of slice 4 |
| S7 | **#68 still reproduces on the current package?** Pack the MSBuild package from the Debug exe into a scratchpad feed, consume it from a tiny 2-file project, `dotnet build`, then `dotnet msbuild -t:CompileDesignTime -p:DesignTimeBuild=true -getItem:Compile` before and after adding `c.pg`; finally the maintainer's interactive VS check | `dotnet` + scratchpad; VS check is the maintainer's | D9: whether slice 6 is built at all |
| S8 | Outputs-list → evaluation-time items parsing: does `$([System.IO.File]::ReadAllText('…'))` split on `%0D%0A`/`%0A` yield correct `Compile` items with `;`-free and spaced paths, and does the `.cs`-under-`PolyglotOutDir` metadata filter work at evaluation? | `dotnet msbuild -getItem:Compile` on a scratchpad `.proj` | D10's exact XML |

S5, S6 and half of S4 are grep/read-only or run the existing exe; S7 is the only one with a cost worth
mentioning (one `dotnet pack` + two small builds).

## Slice 1 — per-body temp allocator in the Lowerer (D1, D2)

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

## Slice 3 — attribute imports per file (D5)

- Record `attrImports_` entries as `{line, originModule}` (`lower.cpp:264-265, 482, 551`).
- In the partitioned emit's keep-prune (`compiler.cpp:1688-1692`) drop lines whose origin is not kept;
  `module.attrImportsBlock` (`emitter_base.cpp:1490-1492`) is unchanged.

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

## Slice 6 — issue #68 (D9–D12; **gated on S7**)

- If S7 shows the current package already works: close #68 with the evidence (hook now in `BeforeCompile`,
  outputs-list items), no XML change; still add the D11 evaluation checks that pin the current behaviour.
- If it persists: D10's `ItemGroup` after `.targets:46` + `Remove`-then-add in `_PolyglotAddGenerated`,
  documented next to the review-2b comment (`.targets:212-214`); D11 checks in `run-nuget.ps1`'s Stale
  fixture (`:186-262`).
- D12: add the `nuget` leg to `ci.yml` **if the maintainer approves**.

## Slice 7 — docs

- D6 paragraph (what is / is not a function of a module's own source).
- `docs/prd/PLAN.md` P41 entry (status, decisions, release note: emitted temp names renumber; third-party
  plugin authors: `fresh` temps now restart per body — no manifest key change, no schema bump).
- CLAUDE.md "Next step" pointer.

## Slice 8 — end gate (CI)

Push once; `ci.yml` is the gate. Read failures with `--log-failed`; fix only that.

---

## Decision log

| ID | Decision | Status |
|---|---|---|
| D1 | Temporaries numbered per **body**; lambdas share the enclosing allocator | proposed |
| D2 | `tempsUsed` moves from `ir::Module` to each body node; emitter seeds `tmp_` per body | proposed |
| D3 | `__pg_lam`, `__cs`, `__pg_t`, `__dw` counters per body too | proposed |
| D4 | Build driver and "conflicting output" refusal unchanged; no emit-once masking | proposed |
| D5 | Tier-1 attribute import lines filtered to the kept module | proposed |
| D6 | Set-dependent naming (overload mangling, extension disambiguation, reach-driven records, prelude file) documented as by-design | proposed |
| D7 | Golden strings renumber; no compat shim | proposed |
| D8 | Unit + CLI-smoke pins for per-module determinism; conformance unchanged | proposed |
| D9 | #68 re-verified against the current package before any XML change | proposed |
| D10 | If it persists: outputs-list-derived evaluation-time `Compile` items, `Remove`d and re-added by the target; folder glob rejected | proposed |
| D11 | #68 checks in `run-nuget.ps1` (eval set, add, delete → no CS2001/CS2002, design-time simulation) | proposed |
| D12 | Add the `nuget` leg to `ci.yml` so D11 actually runs | **needs maintainer sign-off** |
