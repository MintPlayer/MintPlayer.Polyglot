# Issues #76 + #68 — investigation evidence (5-agent team, 2026-10-19, verified against HEAD `29d1376`)

> Read-only investigation. No build, no test run. The only execution was the **already-built** in-tree
> `x64\Debug\MintPlayer.Polyglot.Cli.exe` (0.0.0-dev, built from the P40 merge) and the NuGet-cached
> `mintplayer.polyglot.msbuild\0.10.2\tools\win-x64\polyglot.exe` (the issue's exact version), run on the
> issue's repro files. Line numbers refer to HEAD `29d1376`.

## A. Issue #76 — a module's temporaries depend on which root reached it

### A1. Repro (confirmed on both exes, identical behaviour)

Issue files (`lib.pg` imported by `a.pg` and `b.pg`, each with `for i in 0..s.len()`), pgconfig
`{ "pattern": "*.pg", "output": "out/%(Filename)", "owned": true }`, `polyglot build --only typescript`:

```
polyglot: conflicting output for '…\out\lib.ts': '…\lib.pg' (reached from '…\a.pg') and
          '…\lib.pg' (reached from '…\b.pg') emit it with different content
exit 1
```

| Target | Result | lib's loop header |
|---|---|---|
| typescript | **FAIL** (exit 1) | `for (let i = 0, __end1 = s.length; i < __end1; i++)` |
| csharp | **FAIL** (exit 1) | `for (int i = 0, __end1 = s.Length; i < __end1; i++)` |
| php | **FAIL** (exit 1) | `for ($i = 0, $__end1 = __pg_len16($s); $i < $__end1; $i++)` |
| python | pass (exit 0) | `for i in builtins.range(0, _pg_len16(s)):` — no temp is needed, so no name appears |

Single-input builds show the counter is **per closure lowering, entry module numbered first**:

```
lib.pg alone : __end0
via a.pg     : __end1   (a.ts itself uses __end0)
via b.pg     : __end2   (b.ts uses __end0, __end1)
```

`f(x)!` (the issue's second trigger) does **not** conflict on its own on TS/C#: both emit the null-assert
without a temp (`?? __pgNullAssert(...)` / `?? throw new NullReferenceException(...)`). But
`lower.cpp:1043` still mints `__nn<N>` for any non-Name operand, so a `!` **shifts** every later number
in the closure. It is a trigger only in combination with an emitted temp (`__end`, `__c`, `__as`, `__ns`,
`__opt`, `__w`), which is how Spark hit it in a larger module.

### A2. Root cause (precise)

1. **One flat unit per root.** `compile()` merges the whole import closure into ONE `CompilationUnit`:
   entry decls first, imported modules appended in post-order at first visit
   (`src/MintPlayer.Polyglot.Core/src/compiler.cpp:1047-1058`, `mergeDecls` `:629-636`, tagging
   `originModule`).
2. **Partitioned emit re-lowers the whole unit per output file**, then prunes to that file's decls
   (`compiler.cpp:1593-1596` "lowering is pure … we lower the whole unit and then keep only this file's
   decls", `:1635`, prune lambda `:1688-1692`). The assumption holds per *unit*, not per *module*.
3. **`Lowerer::tmpCounter_` is one int per `Lowerer`** (`lower.cpp:553`, class at `:96`, one per
   `lower()` call `:1950-1952`), never reset between declarations. `Lowerer::run` iterates **by declaration
   kind** (enums → unions → records → classes → interfaces → globals → extensions → functions,
   `lower.cpp:336-440`), interleaving modules. So lib's first temp is `__end<N>` where N = temps minted by
   every declaration lowered before lib's in that root's unit.
4. **The emitter continues the same counter.** `m.tempsUsed = tmpCounter_` (`lower.cpp:542`,
   `ir.hpp:742`) → `tmp_ = m.tempsUsed` (`emitter_base.cpp:1999`). Plugin `{"fresh":…}` temps
   (`IrExprCtx::freshName`, `emitter_base.hpp:56-57`; `backend_engine.cpp:163, 403`;
   `emitter_base.cpp:507`) therefore also start at the closure-wide total. This is the deliberate
   **one-allocator invariant** from P40 review r3 2(c) (`ir.hpp:737-741`: a lowering temp and a plugin
   temp can never share a name; Python's `??` walrus `__cN` once rebound a compound's `__cN`). Any fix
   must preserve the disjointness **within the new scope**.
5. **The CLI writes every closure module once per root** (`main.cpp:1156-1194` `buildGroup`; roots that
   are themselves imported are dropped `:1176-1181`; `emitOne` `:585-660` → `produceEmitted` → `sinkPut`
   `:394-430` → `OutputSink::put` `outputsink.hpp:378-382`). The sink dedupes **byte-equal** content
   (`Put::Deduped`) and refuses otherwise (`Put::Conflict`, both producers named via `producerName`
   `:555-559`). The duplicate emission is by design ("identical content is written once",
   `main.cpp:388-391`); the defect is that the content is not a function of the module's own source.

Other paths: MSBuild NuGet = `build` over globbed inputs (`main.cpp:1156-1157`) — the exact issue
scenario; watch (`--watch` refuses >1 input, `:1459`) and LSP preview (`:2054`) are single-root, so no
conflict, but the same non-determinism of content.

### A3. Complete catalogue of closure-dependent output

**Order-dependent (numbered per closure lowering) — all fixed by the same change:**

| # | `.pg` construct | Name | Minted at | Targets |
|---|---|---|---|---|
| 1 | `for i in 0..f()` (non-simple range end) | `__end<N>` | `lower.cpp:1446` | C#/TS/PHP emit it; Python doesn't |
| 2 | `f(x)!` (null-assert, non-Name/This operand) | `__nn<N>` | `lower.cpp:1043` | PHP/Python emit it; shifts all |
| 3 | `expr as T` on a non-simple operand | `__as<N>` | `lower.cpp:1759` | all |
| 4 | `a ?? b` on Option / `optionMatchOn` | `__opt<N>` | `lower.cpp:710` | all |
| 5 | native-nullable → Option (`nativeToOption`) | `__opt<N>` | `lower.cpp:750` | all |
| 6 | `x?.m()` / `x?.f` on a non-Var receiver | `__ns<N>` | `lower.cpp:1802, 1827` | all |
| 7 | compound write on a non-Var base (`recv().f op= v`) | `__w<N>` | `lower.cpp:1289` | all |
| 8 | impure-operand hoisting (`pendingStmts_` `Let`) | `__c<N>` | `lower.cpp:1659` | all |
| 9 | plugin `{"fresh":…}` rules: PHP `operandSnapshot` `$__cN` (`plugins/php/polyglot-plugin.json:50`), Python `Member` `__o` (`:321/332`) and `Binary` `__c` (`:3561/3632`) | `__c<N>`, `__o<N>` | `emitter_base.cpp:507`, seeded `:1999` | per file, offset by the closure total |
| 10 | block-bodied lambda on an `expressionOnlyLambdas` target (Python manifest `:136`) | `def __pg_lam<N>` | `hoist_block_lambdas.cpp:26-28`, run on the whole unit at `lower.cpp:1959` | Python |
| 11 | shadowed nested `for` binding on a `forbidsShadowedLocals` target | `<binding>__cs<N>` | `CsScopeLegalizer::seq`, `lower.cpp:1851-1888`, run at `:1957` | C# |

**Set-dependent (same module, different closure → different output), mostly by design (P40 D16 flat
namespace), one genuine leak:**

| # | Construct | Effect | Where | Verdict |
|---|---|---|---|---|
| 12 | same free-function name in two modules / cross-module overloads | `name$T` mangling (`name__i32`); `crossModule = userOrigins[name].size() > 1` over the flat unit | `sema.cpp:1602-1623`, `lower.cpp:425-467`, `compiler.cpp:1646-1652` | by design; document |
| 13 | extension name colliding with anything in the closure | emitted `name$Receiver` | `lower.cpp:139-152` | by design; document |
| 14 | **Tier-1 attribute `importLine`s** | `attrImports_` collected over the WHOLE unit (`lower.cpp:264-265`, `:482`, `:551`) and emitted verbatim in EVERY kept file (`emitter_base.cpp:1490-1492`); never filtered by `keep` | — | **genuine leak, fix in this PR** |
| 15 | Tier-2 attribute records | emitted only if some Meta query anywhere in the closure materialised it (`unit.metaMaterialized`, `sema.cpp:3640`; `lower.cpp:219, 368`) | — | by design (reach-driven); document |
| 16 | prelude content of the prelude-holding file | union of reach across the closure; the CLI unions across roots | `compiler.cpp:1677-1686`, `main.cpp:629-649` | by design |

**Checked and properly scoped today (per emitted file, deterministic):** `__pg_t<N>` bound-template
temps (`emitter_base.cpp:2312`) and the Python do-while flag `__dw<N>` (`:2520`) — their counters live on
`InterpretedEmitter`, which is **constructed fresh per `emit()` call** (`backend.cpp:65-70`; verified
by the lead, correcting one agent's claim that it was process-wide); helper set `requires_` (cleared per
emit `:2000`, recorded over kept decls only `:1966-1971`, sorted emission `:2043-2050`); loop driver
`<binding>__it` (derived, `:1616`); imports (`buildImports`, `compiler.cpp:1341-1400`, keyed maps);
`externMap_` (cleared `:1998`); origin records and the TS v3 sidecar (per file, file ids remapped,
`main.cpp:496-512`); C# `#line` (path strings). No gensym in parser/sema/flow/capture analysis
(`capture_analysis.cpp:79` `nextId_` is reset per function and never emitted).

### A4. Constraints the fix must honour

- `ir.hpp:737-742` one-allocator invariant (above) — preserve per scope.
- Core never compares target names (FIRM): the fix is target-agnostic; `lower.cpp:1950-1962` already
  gates `CsScopeLegalizer` on `forbidsShadowedLocals()` and hoisting on `expressionOnlyLambdas()`.
- P40 G6 (`docs/prd/issue-73-library-grade/PRD.md:43-44`): `build --check` detects drift and committed
  output is checkout-independent. #76 is the same class of guarantee. Spark commits the TS copy and gates
  CI on `polyglot build --check --only typescript` (MintPlayer.Spark#528).
- P40 D8/S16 (`PRD.md:454`): CS0136/CS0128 are already a design constraint (that is why `__pg_t` is
  per-expansion unique).
- `tests/refusals/fixtures/double_underscore_name.pg`: user identifiers may not start with `__`, so
  compiler names cannot collide with user names under any numbering.
- CLAUDE.md: principled fix; no backward compatibility for emitted names.

### A5. Blast radius of renumbering (grep, HEAD)

Pinned temp names in `tests/MintPlayer.Polyglot.Tests/src/tests_main.cpp`: `__end` ×7, `__opt` ×3,
`__w` ×5, `__c` ×10 (incl. `__c1` at `:5735` inside one body, and the P19 reserved-prefix refusal test at
`:4128-4134`, which is about user names and unaffected). **None** in `tests/conformance/` or the four
manifests (the manifests only reference the prefixes, not numbers). All pinned programs are single-file,
mostly single-function, so most expectations stay literally the same under per-body numbering; the
cross-declaration ones renumber.

### A6. Existing tests and why none caught it

- `tests/cli/run-cli-smoke.ps1:431-440` (P40 review 3b, "a module imported by two roots") — the shared
  module is `fn a(): i32 { return 1 }`: no temp, so it cannot diverge. `:440` and `:665` assert the
  refusal for genuine conflicts (keep).
- `:65/:1334` (G39), `:668-681` (B7 two checkout roots), `tests/msbuild/run-nuget.ps1:277` — all compare
  the SAME closure twice.
- `tests/conformance/programs/<dir>/entry.pg` is single-entry (README line 7): cannot express two roots.
- Unit goldens hard-code `__opt0` etc. for single-file programs (`tests_main.cpp:3361, 3375`).

### A7. Prior art (brief)

TypeScript's downlevel emitter restarts `_a`, `_i` per function; dart2js numbers `t1..tN` per method;
Kotlin/JS `tmp0_…` per function; Haxe picks the lowest unused `_g`, `_gN` per function scope; Roslyn's
synthesized locals are per method body. Per-body numbering is the industry norm.

## B. Issue #68 — generated `.cs` invisible to IntelliSense until reload

### B1. The package has changed since 0.9.9 (P40 PR #75)

Files: `src/MintPlayer.Polyglot.MSBuild/build/MintPlayer.Polyglot.MSBuild.props` (30 lines) and
`.targets` (256 lines). The two things the issue blames are both gone:

- **The folder glob is gone, deliberately** (P40 review 2b). `@(Compile)` is built from the CLI-written
  outputs list: `.targets:42-46` `_PolyglotOutputsList = $(IntermediateOutputPath)polyglot.outputs.list`
  ("so a stray or stale file there … never compiles"); `_PolyglotAddGenerated` `.targets:215-233` reads it
  (`ReadLinesFromFile` → `_PolyglotListed` → `_PolyglotGenerated` filtered to `.cs` directly under
  `_PolyglotOutDirFull` `:227` → `<Compile Include>` `:228`). The glob survives only as
  `<FileWrites Include="$(PolyglotOutDir)*.cs" />` `:229` for `dotnet clean`.
- **The hook moved.** `_PolyglotGenerate` `.targets:251-254` runs `BeforeTargets="BeforeCompile;CoreCompile"`
  with `DependsOnTargets="PolyglotTranspile;_PolyglotAddGenerated"`, i.e. before
  `_GenerateCompileDependencyCache` hashes `@(Compile)` (rationale `:235-250`). `BeforeCompile` is in
  `CompileDesignTime`'s chain, so design-time builds run the real target.
- **RemoveDir is gone** (`.targets:139-142`); the CLI's `--record` deletes "previous minus current"
  (`:27-32`). Incremental: `PolyglotTranspile` `:143-147` `Inputs="@(PolyglotFile);$(PolyglotTool);
  $(_PolyglotInputsCache);@(_PolyglotDep)"`, `Outputs="$(_PolyglotStamp);$(_PolyglotOutputsList)"`;
  `_PolyglotPrepareInputs` `:88-121` writes an inputs cache (`WriteOnlyWhenDifferent`) and deletes the
  stamp when a listed output is missing.
- Design-time comment still present (`.targets:3-8`, "design-time builds are NOT skipped, Grpc.Tools'
  hard-won lesson"); no `$(DesignTimeBuild)` condition anywhere.
- `$(PolyglotOutDir)` is defined at `.targets:26` (not in the .props, `.props:9-11` defers
  IntermediateOutputPath-dependent values). `@(PolyglotFile)` is populated at evaluation by `.props:24`
  (`**\*.pg`); `UpToDateCheckInput` items at `.targets:70-74`.

### B2. The issue's suggested fix, assessed against HEAD

1. **It reintroduces the glob 2b removed.** A stale headerless `.cs` in the folder would compile again
   (the `R3Owned` fixture `run-nuget.ps1:600-611` plants exactly such a file).
2. **It produces CS2001 on delete/rename.** Ordering: evaluation expands the glob to concrete paths →
   `_PolyglotPrepareInputs` detects the changed `.pg` set → `PolyglotTranspile` runs and `--record` deletes
   the orphan `.cs` → the target adds the list-derived set → CoreCompile still holds the evaluation-time
   concrete path of the deleted file → **CS2001**. `Exclude="@(Compile)"` removes duplicates, not stale
   items. The Stale fixture's existing assertions (`run-nuget.ps1:218-227`: delete `a.pg` → CS0117 and
   `a.cs` gone) would flip to CS2001.
3. Without `Exclude`, CS2002 duplicates; with it, correct for `dotnet build`.
4. `--only` / `--check` / TS twins are irrelevant to the C# sink: C# always lands in `PolyglotOutDir`
   (`.targets:203-204` `--out "$(PolyglotOutDir)."` + `--build-owned`), TS twins never do (`:192-197`).
5. Multi-TFM: `$(IntermediateOutputPath)` is per inner build, so an eval-time set is TFM-correct; the
   outer evaluation sees `Exists()` false — acceptable.

### B3. Why the IDE goes red (hypothesis to verify first)

The current design already adds items in design-time builds. The symptom is therefore not "items are
never added" but "the project system does not re-run the design-time build / re-evaluate when a new `.pg`
appears". Adding a `.pg` *does* change the `**\*.pg` glob, which should re-evaluate and re-run the
design-time build (inputs cache changes → transpile runs → items added). On 0.9.9 the hook was `CoreCompile`
only (not part of `CompileDesignTime`) with a folder glob, which explains the 0.9.9 report. **The honest
first step is to re-verify #68 against the current package** (spike S7) before touching XML.

### B4. The right shape if it persists

Evaluation-time `Compile` items **derived from the outputs list** (not a folder glob), filtered to `.cs`
directly under `PolyglotOutDir`, placed after `.targets:46`, and explicitly `Remove`d then re-added by
`_PolyglotAddGenerated` so the target remains the authority and a stale path can never reach CoreCompile.
MSBuild has no eval-time file-read item, but `$([System.IO.File]::ReadAllText(...))` is a permitted
property function; splitting on line ends into an item `Include` is the brittle part (CRLF, `;` in paths)
and is what spike S8 must settle.

### B5. Testing

- Harness: `tests/msbuild/run-nuget.ps1` (888 lines; fixtures are ThreadJob scriptblocks `:791-792`,
  collected `:851-876`). Invoked by `scripts/build-and-test.ps1:136-141` and the NX `nuget` leg
  (`project.json:159-164` → `scripts/nx-leg.ps1:35`).
- **It is not run by any GitHub workflow**: `ci.yml:54-55` runs `unit,watch,registry`; `release.yml` only
  packs/pushes. Under the maintainer's "let GitHub Actions verify" rule, a #68 regression test is only
  meaningful if the `nuget` leg joins `ci.yml` (decision D12).
- The harness can host evaluation-only checks: the R3 fixture already uses `dotnet msbuild <proj>
  -getItem:UpToDateCheckInput -nologo` (`:626-629`) and `-getItem:Watch` (`:832`). `-getItem:Compile`
  combined with `-t:CompileDesignTime -p:DesignTimeBuild=true` returns the post-target set on MSBuild ≥17.8.
