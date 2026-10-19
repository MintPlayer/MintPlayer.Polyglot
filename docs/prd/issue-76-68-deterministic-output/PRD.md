# P41 — Deterministic per-module output (issue #76) + IDE-visible generated sources (issue #68) — PRD

> **Status:** 📝 designed + **grilled (2026-10-19)**: D1–D14 locked with the maintainer in a nine-question
> round (log in `PLAN.md`). Slice 0 spikes pending. Two maintainer directives shape the design: **good
> fixes over quick fixes**, and **the GitHub workflow must finish in 5 minutes**.
> **Issues:** [#76](https://github.com/MintPlayer/MintPlayer.Polyglot/issues/76),
> [#68](https://github.com/MintPlayer/MintPlayer.Polyglot/issues/68).
> **Evidence:** `ANALYSIS.md` (5-agent investigation, verified against HEAD `29d1376`, repro run on the
> existing Debug exe and on NuGet 0.10.2 — nothing was built).
> **Plan, spikes, decision log:** `PLAN.md`.
> **Delivery:** one PR, one minor release under the lockstep scheme. Both issues come from the same
> consumer (MintPlayer.Spark / MintPlayer.AI) and the same surface (what the build emits and how MSBuild
> sees it), so they land together.

## 1. Problem

**#76.** A module reached from two inputs is emitted twice, and the two copies differ, so `polyglot build`
refuses it with "conflicting output". The cause is not "one counter across the whole build" as the issue
guesses but close: lowering numbers its desugaring temporaries with one counter across the **whole import
closure of a root**, entry declarations first, and the partitioned emitter re-lowers that whole closure for
every output file. `lib.pg` compiled alone says `__end0`, reached from `a.pg` it says `__end1`, reached from
`b.pg` it says `__end2` (`ANALYSIS.md` A1). TS, C# and PHP all fail; Python passes only because its range
loop needs no temp. The byte-exact dedupe in the output sink was designed to collapse the duplicate
emission; it cannot, because the content is not a function of the module's own source.

This violates the guarantee P40 just made: committed output is stable and `build --check` only fails on
real drift (P40 G6). Spark commits the TS copy of its GS1 core and gates CI on `--check`; a module shared
by several `.pg` files (its `aitable.pg`) hits this on every build.

The investigation widened the issue to its class (`ANALYSIS.md` A3): **eleven** numbered-name families
share the closure-wide counter (`__end`, `__nn`, `__as`, `__opt`, `__ns`, `__w`, `__c`, the plugin `fresh`
temps, hoisted `__pg_lam` lambdas, C# `__cs` legalized bindings), plus one unrelated leak — Tier-1 attribute
`import`/`using` lines are collected over the whole closure and pasted into every emitted file.

**#68.** Adding a new `.pg` to a solution already open in Visual Studio leaves IntelliSense reporting the
generated types as missing until the project is reloaded, while `dotnet build` is green. Reported on 0.9.9.
P40 has since replaced both mechanisms the issue blames (the folder glob and the `CoreCompile`-only hook),
and the issue's suggested fix would **regress** a P40 decision and introduce CS2001 on `.pg` deletion
(`ANALYSIS.md` B2). The symptom must be re-verified against the current package before any XML changes.

## 2. Goals and non-goals

**Goals**
- G1. **A module's emitted text is a pure function of that module's source** (plus the plugin and the
  compiler version). It does not depend on which root reached it, on which other modules are in the
  closure, or on their order. Consequence: the issue's repro exits 0 on all four targets and writes one
  `lib.<ext>`.
- G2. **A function's emitted text is a pure function of that function's source.** Adding a temporary to one
  function never renumbers another. This is what keeps committed-output diffs local and `--check` quiet.
- G3. The P40 one-allocator invariant holds at the new scope: a lowering temp and a plugin `fresh` temp can
  never share a name inside one body.
- G4. The "conflicting output" refusal stays as the backstop and fires only for genuinely different
  sources.
- G5. **The IDE sees generated C# as soon as a build has produced it**, without reload, and never compiles a
  stale or deleted file (no CS2001, no CS2002, the P40 review-2b guarantee intact).
- G6. Regression tests pin G1 and G5 where CI runs them.

**Non-goals**
- Set-dependent naming that is by design: cross-module overload mangling (`name__i32`), extension
  disambiguation (`name$Receiver`), reach-driven attribute records and prelude content (`ANALYSIS.md` items
  12, 13, 15, 16). These are the P40 D16 flat-namespace contract. They are **documented** (D6), not changed.
- Shorter or prettier temp names (lowest-unused reuse, content hashes) — rejected in D1.
- Skipping the per-root re-lowering for performance (D4). Correctness first; the perf idea is recorded.
- Any IDE behaviour beyond Visual Studio + `dotnet build` (Rider/VS Code use the same MSBuild
  design-time build, so they benefit, but are not verified).

## 3. Scope contract check (PRD §3)

| Item | §3 bucket | Notes |
|---|---|---|
| Per-body temp numbering | §3.A infrastructure | no language-surface change; emitted names change (allowed: no backward compat) |
| attrImports per file | §3.A (P37 Tier-1 attributes) | fixes a leak in an existing feature |
| MSBuild eval-time Compile items | distribution | no `.pg` semantics change |

Nothing here touches §3.B refusals or §3.C relaxations.

## 4. Design

### 4.1 Numbering scope — **D1: per body**

A **body** is any top-level code container the Lowerer visits: a free function, an extension function, a
method, a getter/setter, a constructor, a class field-initializer block (`init`), a global's initializer, a
record/union method. The counter resets to 0 at body entry. **Lambdas nested in a body share the enclosing
body's allocator** (a lambda is not a body for numbering): on C# a nested scope redeclaring an enclosing
local is CS0136, and on Python/PHP (function scope) reuse would alias.

Why not the alternatives (`ANALYSIS.md` A7 and the design agent's table):
- *Per module* fixes #76 but adding a temp to function 1 renumbers functions 2..N, so committed-output diffs
  stay noisy and `--check` stays fragile. Also the Lowerer iterates by declaration kind, so "per module" is a
  map keyed by `originModule`, not a reset — more machinery for a weaker guarantee.
- *Per statement / lowest-unused* needs a scope model mirrored in the emitter and runs straight into CS0136
  and Python/PHP aliasing. The project already paid for `CsScopeLegalizer` for user names alone.
- *Content-derived names* are unreadable and still collide for the same expression twice in one body.
- *Emit each module once per build* masks the bug (the first-reached root's numbering wins, so output
  depends on `include` enumeration order), and breaks the `--check` producer naming that P40 review 3b tests.

Prior art agrees: TypeScript, dart2js, Kotlin/JS, Haxe and Roslyn all number synthesized locals per
function/method.

### 4.2 Preserving the one-allocator invariant — **D2: `tempsUsed` moves from the module to the body**

`ir::Module::tempsUsed` is replaced by a `tempsUsed` on every body-bearing IR node (`Function`, `Method`,
`Class.init`, `Global`, extension function, accessors). The Lowerer records `tmpCounter_` at body exit.
The emitter sets `tmp_` from the body's `tempsUsed` when it enters that body, so plugin `fresh` temps
continue the body's own sequence. The alternative, a disjoint prefix for plugin temps, is rejected: prefixes
are plugin-chosen and already overlap (`__c` is used by both lowering and the Python manifest).

### 4.3 Sibling counters — **D3: same scope for all of them**

- `Hoister::counter_` (`__pg_lam<N>`, `hoist_block_lambdas.cpp:26-28`) restarts per body. Hoisted defs are
  inserted adjacent to the statement, so sibling bodies may reuse N. Spike S4 checks the only edge — a
  global initializer's hoist on Python, which lands at module level.
- `CsScopeLegalizer::seq` (`<binding>__cs<N>`, `lower.cpp:1851-1888`) restarts per body. It is already
  gated on the `forbidsShadowedLocals` trait; the Core keeps comparing traits, never target names.
- `boundTmpSeq_` (`__pg_t`) and `doWhileSeq_` (`__dw`) are already per emitted file (the emitter is
  constructed per `emit()`). They are made per body too, for uniformity and G2.

### 4.4 Compile pipeline — **D13: lower once per root, partition the lowered IR**

Today `compile()` re-lowers the whole closure once per output file and prunes (`compiler.cpp:1593-1596,
1635, 1688-1692`). That structure is the reason the bug could exist, and it stays wasteful even when the
counters are fixed. P41 lowers each root's closure **once** and the partitioned emit **slices the lowered
module by `originModule`**; the per-file prelude pruning (`pruneUnreachablePrelude` for `preludePerFile`
targets) runs on the slice. "Lower per module" was considered and rejected: the D16 flat-namespace
features (overload mangling, extension disambiguation, Meta materialization, prelude reach) are
legitimately unit-wide, so a per-module lowering would still need the unit and buys little over slicing.

### 4.5 Build driver — **D4 (revised): canonical-module dedupe before emission; refusal kept for its real
purpose**

Roots are still compiled one closure at a time (sema needs the closure, and a build-wide unit would make
unrelated roots collide under D16). But a module whose canonical source has already been emitted for a
target in this build is **not re-emitted or re-written**: the driver keys produced modules by (canonical
path, target) and skips the second occurrence. The "conflicting output" refusal stays for two **different**
sources routed to one path and for the origin-info divergence case. The proof that two paths produce
identical text moves to where invariants belong — the unit pins and the CLI smoke test (§4.7) — and to
`build --check` on committed output. The P40 review-3b smoke test (both producers named) is re-pointed at a
genuine two-source conflict.

### 4.6 Attribute imports — **D5 (revised): derived from the file's slice**

With D13 there is a real per-file slice, so Tier-1 `import`/`using` lines are **derived by walking the
slice's declarations for attribute-binding uses** (declarations, parameters, members, enum cases) — the
same shape as the P40 prelude reachability pass — instead of being collected during lowering and
filtered. They cannot drift from the declarations they serve. Dedupe stays per file. The failure direction
if a use site is missed is a loud consumer-side compile error, never a silent extra `using`.

### 4.6b C# wrapper always `partial` — **D15** (found while verifying)

The C# manifest emitted `static partial class PolyglotProgram` only when the module was `linked` (a multi-file
closure) and `static class` otherwise, so a module compiled alone differed from the same module reached from a
root by exactly that word — the one remaining way a module's text depended on the closure's shape. A
`static partial class` with a single part is legal C#, so the wrapper (and `PolyglotExtensions`) is `partial`
unconditionally; the manifest no longer reads `module.linked`. Same for the test pin and the harness comment
that recorded the old flip.

### 4.6a Documented set-dependence — **D6**

`docs/lang/SPEC.md` (or the determinism section of the PRD, wherever the §3.D text lives) gains a short
"what is and is not a function of a module's own source" paragraph: temporaries and attribute imports are;
overload mangling, extension disambiguation, reach-driven attribute records and the prelude file are
closure-set-dependent by design (D16). Consumers who commit output should know that adding a colliding name
in another module can legitimately change a committed file.

### 4.7 Tests — **D7, D8**

- D7. Golden strings in `tests_main.cpp` renumber where they pinned cross-declaration sequences (about 25
  occurrences, all single-file programs; `ANALYSIS.md` A5). No compatibility shim.
- D8. New pins:
  - Unit: `lib` compiled alone, via root A (one loop), via root B (two loops) — the three `lib` texts are
    byte-identical on typescript, csharp and php, and equal to the `__end0` form. A second case shifts via
    `maybe(x)!` in the root. Pattern: `tests_main.cpp:376-388` (`MapModuleResolver` + `EmitResult.modules`).
  - Unit: a Tier-1 attribute used only in the root does not put its import line into `lib`'s output.
  - Unit: two sibling functions each with a range loop both emit `__end0`; a lambda inside a loop body that
    needs a temp gets `__end1`/`__c1`, not `0`.
  - CLI smoke (`tests/cli/run-cli-smoke.ps1`, next to P40 review 3b at `:433-440`): the issue's three files,
    `"targets":["typescript","csharp","php"]`, `build` exits 0, no "conflicting output", `gen/lib.ts`
    contains `__end0`; then `build --check` exits 0 on the committed result.
  - Conformance cannot host it (single entry per program) — unchanged.

### 4.8 Issue #68 — **D9, D10, D11, D12**

- **D9 (revised). Build the evaluation-time items regardless; S7 is the acceptance test, not the gate.**
  P40 moved the hook into `BeforeCompile` (which `CompileDesignTime` runs) and replaced the glob with the
  outputs list, so the 0.9.9 symptom may already be gone. But "correct only after a design-time build has
  run" is a weaker contract than "correct at evaluation", and the items are cheap. Spike S7 re-runs the
  0.9.9 scenario on the current package before and after D10 and records both results on #68. If S8 shows
  the evaluation-time parsing cannot be made robust, D10 is dropped and S7's verdict decides #68 alone.
- **D10. Outputs-list-derived evaluation-time items, target stays authoritative.** After the
  `_PolyglotOutputsList` property (`.targets:46`), an `ItemGroup` conditioned on
  `'@(PolyglotFile)' != '' And Exists('$(_PolyglotOutputsList)')` adds `Compile` items parsed from the list
  (spike S8 settles the parsing: `$([System.IO.File]::ReadAllText())` split on line ends, filtered to `.cs`
  under `PolyglotOutDir`), tagged with a marker metadata. `_PolyglotAddGenerated` first `Remove`s every item
  carrying the marker and then adds the fresh list-derived set, so a deleted `.pg` can never leave a
  concrete stale path for CoreCompile (no CS2001) and nothing is added twice (no CS2002). A **folder glob is
  explicitly rejected**: it is the P40 review-2b regression.
- **D11. Test in `tests/msbuild/run-nuget.ps1`** (Stale fixture, `:186-262`): after the first build
  `dotnet msbuild -getItem:Compile` lists `polyglot\a.cs` at evaluation; add `c.pg` → `build` green, no
  CS2002; delete `a.pg` → `build` has no CS2001 and `a.cs` is gone (extends `:224-227`); a design-time
  simulation `-t:CompileDesignTime -p:DesignTimeBuild=true -getItem:Compile` lists the new file.
- **D12 (locked). The `nuget` leg joins `ci.yml`**, together with the cli-smoke/refusals/lsp legs that
  carry the #76 pins — none of them runs in any workflow today (`ci.yml` runs `unit,watch,registry`). The
  maintainer's condition: **the whole workflow finishes in 5 minutes**, which D14 delivers.

### 4.9 CI under the 5-minute wall — **D14**

Evidence (`ci.yml` run of 2026-10-19 07:57, master): the Linux job took **11 minutes**. The Core compiled in
45 s; the single 6757-line `tests/MintPlayer.Polyglot.Tests/src/tests_main.cpp` translation unit took
**~9.5 minutes** in Release mode (07:58:33 → 08:08:22). Before P40 (run of 00:26) the same file took ~1
minute; P40 added 3115 lines to it. The coverage job, which builds Debug, compiled everything in 50 s.

- **Root cause first.** `tests_main.cpp` is split into per-area translation units (one per milestone band
  or feature area, each registering its checks with the flat `check()` harness), and the test binary is
  compiled **without optimization** on every platform (tests do not need `-O2`; the `.vcxproj` and
  `CMakeLists.txt` both change, guarded by `scripts/check-buildfile-parity.ps1`). Spike S9 identifies the
  construct that made g++ -O2 blow up, so the split lands it in its own unit and the regression is
  understood, not just dodged.
- **Build once, fan out.** `ci.yml` becomes: one Linux build job (cmake Release CLI + unoptimized tests,
  ~1.5 min) that uploads `build/polyglot`, `build/polyglot-tests` and `build/plugins/**` as an artifact;
  then a **matrix of leg jobs** that download it and run in parallel — `unit+watch+registry`,
  `cli-smoke+refusals+lsp`, `nuget`, and `conformance` **if** its toolchain setup (`dotnet`, `node`,
  `python`, `php`) fits the budget, otherwise it stays out of PR CI as today and the plan says so. The
  runners stay directly invocable (the contract); NX remains the local orchestrator. The coverage job stays
  its own parallel job (already ~4 min). Every job carries `timeout-minutes: 5` as the tripwire, and a
  `concurrency` group cancels superseded PR runs.
- **Expected wall time:** build ≈ 1.5 min + artifact round trip ≈ 0.5 min + slowest leg (nuget ≈ 41 s,
  smoke tier ≈ 19 s, conformance ≈ 97 s parallel per P35) → **≈ 3.5–4 min**. If the measured first run
  exceeds 5 minutes, conformance is the first leg to leave.

## 5. Acceptance

- The issue's repro exits 0 for `--only typescript|csharp|php|python` and `lib.<ext>` is written once with
  `__end0`.
- For every program in `tests/conformance/programs` with more than one module, each non-entry module's
  emitted text equals the text produced by compiling that module alone (a one-off assertion in the smoke
  leg, not a new gate leg).
- No conformance result changes (renaming temporaries cannot change behaviour; the differential suite
  proves it).
- `build --check` on a committed multi-module project is quiet across a rename of a root file.
- `ci.yml` finishes in **under 5 minutes** on a cache-cold PR run, with the nuget, cli-smoke, refusals and
  lsp legs included.
- #68: D9's verdict is recorded on the issue; if D10 is built, D11's checks pass and the `R3Owned` and
  Stale fixtures stay green.

## 6. Risks

- **Nested-scope collisions on C#** if a lambda or local function were treated as a new body: CS0136.
  Mitigated by D1's "lambdas share the enclosing allocator" and spike S1.
- **Python hoisted defs** at module level for global initializers could collide across bodies under per-body
  restart: spike S4 decides whether globals keep a module-level sequence (they are not bodies with temps
  today, so likely moot).
- **`tempsUsed` plumbing** touches every body-bearing IR node and the emitter's body-entry points; the
  anti-silent-drop manifest coverage contract is unaffected (no manifest key changes).
- **#68 cannot be verified headlessly end-to-end**: the design-time build simulation is a proxy for VS's
  project system; the final confirmation is an interactive VS check by the maintainer (as with P16d/P23).
