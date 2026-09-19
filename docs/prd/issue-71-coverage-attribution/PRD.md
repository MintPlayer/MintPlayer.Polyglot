# P39 — Coverage attribution: overlay every target's coverage onto the `.pg` source — PRD

> Issue: [#71](https://github.com/MintPlayer/MintPlayer.Polyglot/issues/71) · slug `docs/prd/issue-71-coverage-attribution/`
> Provenance: maintainer-filed issue (body kept current, two corrections in its comments), plus a
> **5-agent code-grounded investigation** (2026-09-18) covering the Core's origin path, the consuming
> repo's interim tool, the npm/gate/packaging surface, the C#/coverlet PDB path, and an ecosystem
> adopt-vs-build survey. Then a maintainer decision round (**D1–D3**, 2026-09-18).
> Companions: `PLAN.md` (slices + spike log), `ANALYSIS.md` (the investigation evidence, and the three
> claims it overturned).
> Builds directly on **P38** (`docs/prd/issue-69-source-attribution/`, merged as `90dace1`), which
> produced the `--origin-info` artifacts this work consumes. Read P38's PRD §6 before this one.
> Milestone number **P39 is provisional** — renumber at merge if something else claims it first.

## 0. Prime directive check (POLYGLOT_PRD §3)

**This is not a language feature.** No §3.A surface moves, no §3.B refusal weakens, no §3.C faithfulness
corner shifts. `.pg` programs are unchanged and emitted bytes are unchanged — this work reads artifacts
P38 already emits and projects a *third-party report file* through them. It is the same category as P38:
emission metadata plus the tooling that consumes it.

Three contract-adjacent obligations it does inherit:

- **§3.D determinism honesty.** A coverage number that reads as an execution count and is not one is a
  dishonest number. §4.3 fixes the merge semantics precisely, and §4.2 fixes the denominator, because
  both have already produced a visibly wrong figure in a live UI (ANALYSIS §2.4).
- **Never a miscompile (§3.B's spirit), applied to reports.** The failure mode must always be
  *"attributes less than ideal"*, never *"attributes wrongly"* — P38's invariant, inherited verbatim. A
  `.pg` line the map does not describe leaves the report; it is never silently reported as uncovered.
- **Core is language-agnostic (standing project directive).** Nothing here may add a target-name
  comparison to the C++ Core. §4.7 goes further and *removes* the two that already exist in the origins
  code path, because PG-C1 lands on them (ANALYSIS §1.6).

## 1. Goal

One `.pg` source compiles to several targets, and **each target's test suite covers a different part of
the same source**, because each has a different caller. A `.pg` line should count as covered when *any*
target reached it. Measured in MintPlayer.AI: `.pg` coverage goes **93.95% → 98.12%** (3,494 → 3,649 of
3,719 lines) once the C# and TypeScript halves are overlaid.

The C# half needs no tooling in the common case. The TypeScript half works through a 217-line interim
tool in the **consuming** repo (`tools/pg_coverage_remap.mjs`). That is the wrong home: the vocabulary it
duplicates — `originMapping.style`, the `#line` template, the sidecar convention, the footer syntax — is
declared in `plugins/*/polyglot-plugin.json` and drifts from it *silently*. Solving this per consumer is
`consumers × targets`; solving it here is `1 × targets`.

**Deliverable:** a **`polyglot coverage` subcommand** that reads a coverage report in the consumer's own
format and writes the same format keyed on `.pg` files and lines — plus the manifest data that makes
Python and PHP eligible for it at all.

## 2. What is already true (verified, not assumed)

1. **Origin recording is target-neutral.** `EmitterBase::emitLineWithOrigin`
   (`src/MintPlayer.Polyglot.Core/src/emitter_base.cpp:1733`) collects `OriginRecord{outputLine, fileId,
   sourceLine}` whenever `recordsOrigins()`; `emitOriginDirective` early-returns for any non-`Directive`
   style. **`style` only picks the serialiser.** Python and PHP are missing a *sink declaration*, not the
   data.
2. **The footer is 100% manifest data.** `main.cpp:311-329` does a generic `$f` substitution with no
   comment-syntax knowledge, guarded by `if (!om.footer.empty())`. There is no hardcoded `//#` anywhere
   in the C++.
3. **`sourcesContent` is already emitted**, one entry per source, `null` where unreadable
   (`sourcemap.cpp:79-84`, filled by `main.cpp:276`). The interim tool's on-disk path resolution is
   therefore unnecessary work *and* a class of bug that has already bitten once.
4. **Line granularity only.** Every map segment points at column 0 of both sides (P38 N1, deliberate).
   Every rule below assumes this and must degrade gracefully rather than pretend otherwise.
5. **One origin table, two renderers.** Across nine solvers, the `.pg` line set C# emits `#line` for and
   the set TS maps have **symmetric difference zero** — 3,949 lines, 0 C#-only, 0 TS-only. This is the
   single strongest justification for a shared projector.
6. **The `style` vocabulary is closed and validated**: exactly `directive` and `sourceMapV3`, enforced at
   plugin load (`backend.cpp:246-273`) with a named error per failure. Nothing here opens it.

## 3. Architectural constraint — two orthogonal axes

| axis | varies with | declared by | determines |
|---|---|---|---|
| **origin** | the **language** | the **plugin** (`originMapping`) | *how to project* generated lines onto `.pg` lines |
| **format** | the **consumer's toolchain** | the **invocation** (`--format`, or sniffed) | *how to read* the report, and how to write the result |

**`originMapping` stays exactly as it is. Nothing about coverage formats belongs in the manifest.** A
plugin cannot know what format its consumer emits; that is a per-repo CI choice that can change tomorrow.
The earlier proposal to put `"coverage": {"reportFormat": "cobertura"}` in the manifest is **rejected**
(issue #71, third comment).

This preserves the zero-engine-code property for a new language, and for the right reason: format readers
are **language-independent**, so a new target declares `originMapping` and immediately reuses every reader
that exists. The reader count is bounded by *formats*, not targets.

**Read and write are separate sets, and conflating them was an error worth naming.** SP7's finding about
the ingest constrains only what a consumer can usefully *upload*; it says nothing about what this tool can
usefully *read*.

**Readers — open and extensible, priority-ordered:**

| # | format | emitted by | why |
|---|---|---|---|
| 1 | **cobertura** | coverlet, vitest, `coverage xml`, php-code-coverage | mandatory and first — the only format `php-code-coverage` emits that we also need (it has **no lcov writer** at all) |
| 2 | **lcov** | vitest, `coverage lcov`, most JS/Python tooling | line-keyed with branch data (`BRDA:`) |
| 3 | **istanbul JSON** | vitest, nyc, jest | statement-level; **what the interim tool reads today**, so it is the compatibility path for the existing consumer |
| 4 | **clover** | php-code-coverage, some JS tooling | completes the php-code-coverage surface |

1 and 2 are prerequisites; 3 and 4 land in the same work unit (one PR — they are not a follow-up), after
the projector proves out on the first two. The projector never sees a format: each reader lowers to the
same `(file, line, hits, branches)` intermediate, which is the whole point of §3's axis split. **A new
format is a reader, never manifest configuration.**

**Writers — same list.** `--out-format` defaults to the input format and stays overridable.

*Updated 2026-09-18:* the coverage.mintplayer.com ingest now accepts **lcov, cobertura, JaCoCo, istanbul
JSON and clover** — [Spark#420](https://github.com/MintPlayer/MintPlayer.Spark/issues/420) added the last
two, so the destination constraint that once applied to istanbul and clover is gone. A destination
constraint remains a *recipe* concern regardless: §8 forbids baking any coverage service's conventions
into the tool. (JaCoCo has no producer in our four ecosystems, so it is a reader/writer only on request.)

**One correction to carry forward.** This PRD and Spark#420 both claimed clover's `truecount`/`falsecount`
was real two-arm identity. **It is not** — they are the counts of taken and untaken arms *aggregated over
every branch on the line*, the same shape as cobertura's `(covered/total)`. Measured against istanbul for
the same run: a line carrying two branches with four arms between them, all taken, reports
`truecount="4" falsecount="0"`, which no true/false pair can express. **Clover is count-only.** Istanbul is
the genuinely arm-identified one, and the richest format in play.

### 3.1 The architecture fork, named rather than discovered later (D1)

The *majority* architecture in comparable transpilers is **source-level instrumentation** — emit probes so
coverage is born in original coordinates: Haxe (`haxe-instrument`, compiler macros → JaCoCo), ReScript
(`bisect_ppx`, a PPX), the TS/babel path (`istanbul-lib-instrument` composing an `inputSourceMap`). Only
the JS/V8 world does post-hoc report remapping, and only because V8 forces it. Kotlin/JS declined the
problem entirely (Kover is JVM/Android only); Dart collects from the VM in original coordinates.

**Decision D1: post-hoc remapping.** Instrumentation would dissolve §4.4's branch-identity problem
outright — arm identity would be assigned by our compiler, once, target-independently — but it changes
emitted bytes, requires a runtime collector per target, and abandons P38's design goal of leaving the
consumer's native toolchain (coverlet, vitest, `coverage.py`, php-code-coverage+Xdebug) untouched. Post-hoc
is the only option that keeps that goal. **Recorded as the considered-and-rejected fork**, with the
condition that would reopen it: if §4.4's per-line branch model proves insufficient in practice, the
answer is instrumentation, not a column-fidelity retrofit.

### 3.2 Why the projection does not route through the IR

Asked during design: with M languages and N report formats, should coverage map onto the **IR** as a
neutral coordinate space rather than onto `.pg` lines directly? **No** — and the reason is that the M×N
collapse has already happened, one layer lower.

**It is M+N today, not M×N.** §3's two axes are independent: each of the N readers lowers to the same
`(file, line, hits, branches)` record and never learns the language; each of the M plugins declares
`originMapping` and never learns the format. The projector between them knows neither. So a new format
costs one reader, and a new language costs one manifest entry (PG-C1 proves that literally — zero engine
code). An IR hop cannot improve on that ratio; there is nothing left to factor.

**The origin table already *is* the language-neutral intermediate.** That is what §2.5's symmetric
difference of zero means: one origin table, two renderers. An `OriginRecord` is
`generated-line → .pg-line` **directly**, so inserting IR coordinates adds a hop carrying no information
either end does not already have — and it cannot refine a mapping whose columns were never recorded (N1).

**Both boundaries speak file+line, not nodes.** coverlet and vitest report file+line; the coverage UI
renders file+line; `git ls-files` matches paths. IR node ids have no consumer at either end, and smuggling
them into the map (via `names`, say) would break the v3 sidecar for the debuggers and browsers that
consume it today — a second consumer currently obtained for free.

**Where the instinct is right, and where it actually leads.** Node identity would solve the one thing line
granularity cannot: **branch-arm identity at column 0** (§4.4). Two generated branches folding onto one
`.pg` line could be told apart instead of union-merged. But arm identity assigned once by the compiler,
target-independently, *is* **§3.1's instrumentation fork** — the same idea by another name. If the
per-line branch model proves insufficient, that fork reopens; an IR coordinate space bolted onto a
post-hoc remap is not a third option.

## 4. Projection rules

### 4.1 Many-to-one is the normal case, not an edge case

Tetris alone folds 1,244+ generated lines onto **797** distinct `.pg` lines. This is a property of
*expression rendering* in the shared engine — a 6-line `match` renders into one `line()` call — and it is
**not Python-specific**: the same sample collapses identically on the already-shipping C# `directive`
target (ANALYSIS §1.5). P38 already lives with it.

### 4.2 The denominator is the mapped set

**The denominator is the set of `.pg` lines the map/directives describe — never the file's physical line
count, and never the report's.** `class`/`record` heads carry no mapping from either side; snake is 516
mappable lines of 831 physical. A tool using raw LOC reports a permanent, meaningless shortfall.

This is the reverse-direction restatement of P38's **"hidden lines leave the report"** (P38 §6.5): a line
with no origin is *absent*, never phantom-covered and never phantom-uncovered. Confirmed at coverlet's
source — `IsHidden` sequence points are skipped at instrumentation time and never become a record.

### 4.2a The **file** set comes from the origin data, not from the report (D7)

§4.2 fixes the denominator *within* a file. The file set is the same question one level up, with the same
answer and a sharper failure mode: **a `.pg` module whose generated twin appears nowhere in the coverage
report — nothing imported it, no test exercised it — must still appear in the output, with its mapped
lines at zero.** Report-driven enumeration would drop it silently, and the percentage would read *higher*
precisely when a module is least tested.

This is the arm tracer's contract applied one level up: *the denominator comes from the compiler's own
parse, so a never-fired unit reports as **uncovered** rather than **missing**.*

**Each sink enumerates differently, and both must be implemented** — this is the load-bearing consequence:

| style | file set from | note |
|---|---|---|
| `sourceMapV3` | the `.map` sidecars in the routed output tree — union of their `sources` | TS, Python, PHP |
| `directive` | a scan of the generated `.cs` for `#line` directives | **C# has no sidecars**, so a no-op on this leg would leave C#-only modules unenumerable (§7, D8) |

Both readings must agree for a module compiled to both targets; the gate pins that as a fixture. The
generated output location is derived from pgconfig's `include` rules — which the subcommand already reads
to resolve `--target` — with an explicit override flag for consumers running the tool out of tree.

A `.pg` file with origin data but no report entry is emitted at zero and **counted in the summary line**;
it is a legitimate state, not a warning. The residual risk is the opposite of the one being fixed: a module
that compiled but was deliberately excluded from the test build is emitted at zero and *deflates* the
number. That is the safer direction, and it surfaces as an unexplained drop rather than silence.

### 4.3 Merge semantics — lines MAX, branch arms UNION

The issue's rule *"combine many-to-one with `max`, never `sum`"* is **correct for line hit counts and
wrong for branch arms.** Both halves are settled by istanbul's own canonical projections
(`istanbul-lib-coverage/lib/file-coverage.js`):

- **`getLineCoverage()` merges statements onto lines with `max`.** A `.pg` line reached from two generated
  lines was executed; summing produces a number that *reads* as an execution count and is not one. There
  is already a live example of that confusion: a `4×` badge in the coverage UI meaning "four contributing
  statements", and `cfFruitOf` showing `1×` for something that executes ~368 million times. **If a genuine
  Σ is ever wanted it must be a separate field, never `hits`.**
- **`getBranchCoverageByLine()` UNIONS the arms.** If generated lines 10 and 11 both map to `.pg` line 7
  and each is a 2-arm branch with 1 arm covered, the truthful result for line 7 is **2/4, not 1/2**. Max
  understates the denominator and reports the line as better covered than it is.

`BRF`/`BRH` (and cobertura's `branches-valid`/`branches-covered`) **must be recomputed from the merged
arms**, never carried over, or the file totals contradict the per-line records.

### 4.4 Branch identity at column 0

lcov's `BRDA:<line>,[<exception>]<block>,<branch>,<taken>` assigns `block` and `branch` **no semantics** —
they are opaque identity tokens. istanbul itself writes `block` = an opaque per-file counter and `branch` =
the arm index. **So we may synthesize block ids freely**, provided they are stable and distinct per `.pg`
line. Cobertura's `condition-coverage="p% (c/t)"` is already per-line `(covered, total)` with no arm
identity at all — which is exactly §4.3's model, so cobertura is the lossless target and lcov is the one
that needs synthesis.

One trap: **`taken = '-'` is not `0`.** `-` means the enclosing block never ran; `0` means it ran and the
arm was not taken. Merging `-` with `0` yields `0`; `-` with `-` stays `-`. (`lcov-parse` flattens this and
loses it — one of several reasons §6 hand-rolls the readers. The ingest server gets this right: `-`
becomes `null`, distinct from `0`, and `null` is treated as "no information" by its max-merge.)

### 4.4a Cross-target arm identity — the question the server fix *created*

**Superseded history.** SP7 found that the ingest stamped a `BranchFormat` per file and silently dropped
branch edges arriving later in a different format, which would have discarded the TypeScript half of every
overlay. That was fixed upstream in [Spark#420](https://github.com/MintPlayer/MintPlayer.Spark/issues/420)
(`354de79`, deployed 2026-09-18). The former rule here — *"emit one format everywhere"* — is **withdrawn**;
it was a workaround for a bug that no longer exists.

**The ingest's model now**, per `(file, line)`: an arm **set** `S` from formats that identify arms, a
**floor** `F` from formats that only count them, `covered = max(F, |S|)`, `total = arity`. Union and max
are commutative, so order-independence is structural. Arm identity is declared **per line, per report** —
lcov, istanbul and cobertura's `<conditions>` are arm-identified; cobertura's `condition-coverage`,
JaCoCo's `mb`/`cb` and clover are count-only.

**And that is exactly what raises a new problem for an *overlay*.** Set union is only correct when two
reports' arm keys are **comparable**. Ours are not, by construction: the C# leg's keys come from coverlet
over IL, and the TypeScript leg's from vitest/istanbul over JS. For one `.pg` line these are unrelated id
spaces. Two failure directions:

- keys that *should* collide but do not ⇒ the same logical arm counted twice ⇒ **over-credit**, the
  §3.D failure this project must not ship;
- keys that collide by accident ⇒ under-credit.

A canonical per-`.pg`-line key (`"<pgLine>:<index>"`) would fix this *if* both targets enumerate a line's
arms in the same order — plausible, since both are rendered from one IR — but **line-granular mapping
cannot establish it**: when two generated lines both map to `.pg` line 7, the relative order of their arms
is undefined (N1, column 0).

**Worked case, to show this is frequent rather than theoretical.** `.pg` line 7 is `if (x) a else b`, two
arms, and **both suites cover the same arm** — the common situation, since most code is exercised by both
targets. C# contributes arm key `"0:0"` (coverlet, IL-derived); TypeScript contributes `"3:0"` (istanbul
`branchMap`-derived). The union is `{"0:0","3:0"}`, so `|S| = 2` against arity 2 ⇒ **2/2**, where the truth
is **1/2**. When the suites happen to cover *different* arms the union is correct — which is worse, because
it makes the error data-dependent and therefore invisible.

**This is not a defect in the ingest.** Its contract is "arm keys identify arms; union them", and that
holds for everything it was built for — repeated runs of one instrumented build, or cobertura + lcov from a
single vitest run, where `F` and `|S|` agree because they describe the same execution. No current consumer
can trigger it; even this repo's `ci.yml`, which uploads gcovr cobertura *and* arm-trace lcov, is safe
because those describe **different files**, so the keys never meet. Only a transpiler overlay puts two
genuinely different instrumenters on one file, and the server cannot detect that. **Supplying
non-comparable keys is the producer's contract violation — ours.**

**Decision: count-only everywhere, and this reverses the earlier "lcov everywhere" recipe.** The two
options are not symmetric:

| emission | failure mode |
|---|---|
| **count-only** (`covered = max(floors)`) | **under**-credits when the targets cover different arms; never over-credits |
| **arm-identified** (`covered = |S|`) | **over**-credits whenever the targets cover the same arm |

§3.D decides it: an under-credit is honest, an over-credit claims coverage nobody has. So `.pg`-keyed
reports emit branch data **count-only**, and the C# leg keeps coverlet's **cobertura** rather than
switching to its lcov reporter — the opposite of what fidelity reasoning suggested before the upstream fix
made arm sets mergeable. It is the same principle that ruled out converting a count-only format into a
per-arm one: **never manufacture identity the data does not carry.**

Resolving this upward is the strongest argument for §3.1's instrumentation fork, which assigns arm
identity in the compiler once, target-independently — the only mechanism that makes cross-target arm keys
genuinely comparable. See Q3.

A branch-less report remains **harmless**: the merge contributes nothing rather than diluting, so the
averaging risk recorded in the consuming repo's PRD stays **refuted**.

### 4.5 Identify generated files by contract, not by name

The interim tool selects twins with `tsPath.endsWith('_solver.ts')`. A glob wrongly swallows hand-written
`*_solver.spec.ts`; the current spelling excludes it **by accident**. Selection must be by the
**contractual marker**: the `sourceMappingURL` footer and/or the presence of the sidecar named by
`originMapping.sidecarExtension`. Never a name pattern.

### 4.6 Credit every segment, and prefer `sourcesContent`

- **Credit all segments on a generated line**, not first-wins. The interim tool discards every segment
  after the first (`pg_coverage_remap.mjs:72-75`).
- **Prefer `sourcesContent` over on-disk resolution.** Polyglot already embeds it. Path resolution is only
  a fallback, and is the one thing that has silently produced an empty report before.
- Port three algorithms from `istanbul-lib-source-maps` (the ideas, **not** the package — see §6):
  `originalPositionTryBoth` (GREATEST_LOWER_BOUND then LEAST_UPPER_BOUND); end-position resolution via
  `allGeneratedPositionsFor` on the next segment, falling back to `Infinity`; and **drop any range whose
  start and end resolve to different original files**.

### 4.7 The two target-name comparisons in the origins path (principled fix, not a deferral)

`src/MintPlayer.Polyglot.Core/src/compiler.cpp` contains exactly two violations of the language-agnostic
directive, both pre-existing (issue #14 prelude hoisting) and both **inside the function that produces
origins**:

```cpp
:681  const bool splitPrelude      = (target.name() == "csharp") && lib.sharedPrelude;
:722  const bool preludeEverywhere = target.name() != "csharp";
```

These are not incidental to this work. `preludeEverywhere` decides whether the prelude is inlined into
*every* emitted file, and prelude prepending **shifts every recorded origin line by the prepended count**
(`emitter_base.cpp:1615-1626`, `if (origin_)`, style-agnostic). **Python is the only target that declares
preludes today** — so the moment PG-C1 gives Python a sourceMapV3 sink, correct origin line numbers depend
on a branch keyed by a string comparison with `"csharp"`. It works today by accident.

**Fix:** two manifest trait flags following the documented `crossDirImports` precedent (a flag gating a
compiler pass is top-level, not a spec property) — `sharedPreludeFile` and `preludePerFile` — with the
existing behaviour declared, not inferred. This is the "principled fix over workaround" rule applied
exactly where CLAUDE.md says to apply it.

## 5. The C# path: "zero tooling" is true with three caveats

The issue claims C# needs no tooling **ever**. Verified against coverlet's source: the mechanism is real —
`Instrumenter.cs` keys documents purely by `sequencePoint.Document.Url`, nothing consults the physical
`.cs` path, so `#line` fully redirects the key. Setup in the consumer is exactly one MSBuild property
(`<PolyglotOriginInfo>true</PolyglotOriginInfo>`), measured at 59.86% → 68.11%. But the unqualified claim
is **false**, and the caveats are not theoretical:

1. **Duplicate `<class>` entries sharing one `.pg` filename, with repeated line numbers — real, but
   absorbed by the server (SP7, resolved 2026-09-18).** `Coverage.GetCoverageResult` groups **document →
   class name → method name** with no dedup by line; `CoberturaReporter` emits one `<class>` per class
   name, all sharing the `.pg` `filename`. Any `.pg` file that emits more than one C# type — or hoists a
   closure into a generated class, or produces an iterator/async state machine — puts the same `.pg` line
   in several `<class>` blocks, so coverlet's own `line-rate` double-counts it.

   **SP7 read the ingest source and settled it: this does not reach the numbers.** `CoberturaParser`
   groups by `filename` across `root.Descendants("class")`, so `<package>` nesting is irrelevant; lines
   land in a `SortedDictionary<int, …>`, **one entry per line number**; and the rates are computed from
   the *stored* set (`LinesCoverable = file.Lines.Count`). **The report's own `line-rate`, `lines-valid`,
   `lines-covered`, `branch-rate`, `branches-valid` and `branches-covered` attributes are never read by
   any parser.** So the denominator counts each `.pg` line **once**, and a line reported `hits=0` in one
   class and `hits=7` in another is **covered**, never lost.

   One residue, cosmetic only: **within a single report duplicates SUM** (`ParsedFile.AddLine`
   accumulates, with a comment saying duplicate records in one report are the same run), so a `.pg` line
   reached from two C# classes with 3 and 5 hits displays **8**. Coverage percentages are unaffected —
   status is decided by `sum > 0` and the line is counted once. This is the §4.3 "a number that reads as
   an execution count and is not one" failure in the server's own store; it is **documented, not fixed
   here**, because fixing it is the server's call, not the transpiler's.
2. **A silent assembly-drop gate P38 never documented.** `InstrumentationHelper.PortablePdbHasLocalSource`
   checks each PDB document *exists on disk*; `ExcludeAssembliesWithoutSources` defaults to `MissingAll`,
   which saves us only because the `obj/…/*.cs` documents survive alongside the `.pg` ones. Set
   `MissingAny`, or build on machine A and test on machine B, and **the whole assembly vanishes from
   coverage without an error**.
3. **Exclusion globs invert.** `ExcludeByFile=**/obj/**` matches the PDB-recorded path, so it stops
   matching mapped lines the moment they claim a `.pg` origin, while still matching scaffolding. Zero
   *tooling*, but not zero *config* — and the consuming repo has a "do not fix this back" comment proving
   it.

Also: deterministic builds (`DeterministicSourcePaths` + PathMap) rewrite the `.pg` path to `/_/…` exactly
as they do the `.cs`; keep `UseSourceLink=false` and `DeterministicReport=false` (both already default
false). And coverlet's cobertura `filename` is relative to the **longest common prefix across all
documents in the report**, so one document outside the repo pushes `.pg` filenames into a *longer* form —
which `scripts/verify-coverage-paths.ps1:44-48` rejects, because it implements only the
`tracked.EndsWith("/$report")` direction while its own docstring claims "or vice versa". That docstring/
implementation disagreement was flagged during P38 and never landed; it is a free drive-by here.

**SP7 settled what the server actually does, and the tripwire is wrong in two *opposite* directions.**
`PathNormalizer` matches `EndsWithPath(tracked, report) || EndsWithPath(report, tracked)` — **both**
directions, case-insensitively, after unifying `\`→`/`, stripping the root dir and stripping the first
matching `<sources><source>` prefix — but it then requires **exactly one** candidate. So the script is
simultaneously **too strict** (it rejects the report-longer shape the server accepts, which is precisely
the shape coverlet's common-prefix computation produces) and **too permissive** (it accepts the first
suffix hit where the server demands uniqueness, so an ambiguous basename passes the script and comes back
unmatched). An unmatched file is not an error: it is stored with `Matched = false` and **excluded from
every number**, visible only through the upload status — quiet from the server, loud from the action,
which warns on some and throws when all are unmatched. The upload action also strips the
`GITHUB_WORKSPACE` prefix from `filename=`/`SF:` before upload, so **emitting repo-relative
forward-slash paths makes that rewrite a no-op** — the shape to target.

## 6. Vehicle, dependencies, versioning

**Decision D2 — a `polyglot coverage` subcommand in the CLI, not an npm package.** The issue argued for
npm on build cost, and that cost is real but bounded. **Reach** decides it the other way, and the issue
never weighed reach:

| | `polyglot coverage` subcommand | `@mintplayer/polyglot-coverage` on npm |
|---|---|---|
| reaches a **.NET-only** consumer | **yes** — `MintPlayer.Polyglot.MSBuild` already packs the CLI at `tools/<rid>/` | no — requires installing Node purely to remap a report |
| reaches a **PHP/Python** consumer | **yes** — release archive, already cross-platform (Windows, Linux x64/arm64, macOS x64/arm64) | no — same |
| plugin resolution | **reused in-process** (`pluginresolve.hpp`: `file:` refs, in-box, verified cache, registry) | needs a **second** implementation, or a new `polyglot plugins --json` surface built only to feed it |
| `originMapping` vocabulary | one reader, in the language that already owns the closed `style` enum | duplicated, and drifts silently — the exact failure moving this in-repo is meant to stop |
| version skew | impossible by construction | needs a compatibility statement, or lockstep bolted on |
| "self-contained native CLI, zero runtime deps" | **stays literally true** | needs an honesty note explaining the new category |

**Build cost, stated honestly.** The C++ side today has *no VLQ decoder* (`sourcemap.hpp` exposes
`vlqEncode` only), a **lenient** JSON reader built for JSON-RPC (returns `Null` on malformed input — the
wrong posture for a third-party report file), and **no XML reader, writer or escaping anywhere**. So this
adds, in round numbers: VLQ decode ≈40 lines, lcov read+write ≈150, cobertura read+write ≈300 including a
minimal XML subset, plus a strict-mode wrapper over the JSON reader if istanbul input is ever supported.
All `std` C++, no new third-party dependency, but all of it in CMake/`.vcxproj` parity and shipped across
five RIDs. **The XML round-trip is the single hardest part** and the one that threatens A7 (§9) — without
a real DOM, retaining unknown attributes verbatim takes deliberate design rather than falling out for free.

*(A script in `scripts/` was considered and rejected on reach too: `MintPlayer.Polyglot.MSBuild.csproj`
packs only `build\**`, so it never arrives at a consumer. `scripts/arm-trace-to-lcov.ps1` remains the
in-repo precedent for *format conversion as a separate step* — that shape is preserved; only its host
moves.)*

**No `polyglot-coverage.json`.** A per-language coverage manifest beside `polyglot-plugin.json` was
proposed and rejected. Everything it would carry — the style, the sidecar extension, the footer, and
"does this target need a remap at all" — is **already in `originMapping`**, and the projection logic is
*not* per-language: across nine solvers the C# `#line` line-set and the TS map line-set have symmetric
difference zero (§2.5). The one thing that genuinely varies, the report **format**, is the consumer's CI
choice and not a language property at all (§3) — one vitest run already emits lcov, istanbul JSON and
cobertura simultaneously. A second manifest file would mean a second place for the same vocabulary to
drift. **If PG-C7 surfaces a real per-language knob it becomes a key in the existing manifest** (unknown
top-level keys are silently ignored, so it is additive for new plugins), never a new file — which would
also require editing `files: ["polyglot-plugin.json"]` in all four plugin `package.json`s.

**Dependencies: none, and the question dissolves.** The npm design would have adopted exactly one
(`@jridgewell/trace-mapping` — pure JS, no WASM, the library istanbul itself migrated to). In C++ there is
nothing to adopt and nothing to justify. Three algorithms are **ported as ideas** from
`istanbul-lib-source-maps` (whose *package* was never adoptable: its I/O is the istanbul coverage object
on both ends, and it is dormant — no release in 26 months, `nyc@18` still pins v4): see §4.6.

**Round-trip fidelity is a design decision, not a dependency.** Parse into a record that retains
unrecognized records and attributes **verbatim** and re-emits them, so read → remap → write never silently
drops a field a consumer cared about. `--out-format` defaults to the **input** format; the consumer's
pipeline already consumes that, and handing back something else makes the tool a format converter it was
never asked to be.

**Decision D3 — versioning: lockstep, now for free.** As a subcommand the tool *is* the CLI, so it ships
at the tag version by construction, `--version` already asserts against the tag in `release.yml`, and
CLAUDE.md's lockstep sentence stays true with no exception and no new publish plumbing. (Under the npm
design this needed explicit stamp and publish steps **outside** `plugins/`, because that directory is
glob-swept by `publish-plugins.yml`'s manifest validator, which would have failed a package carrying no
`polyglot-plugin.json`.)

## 7. Interface

```
polyglot coverage remap <report> --target <name> [--format <fmt>] [--out-format <fmt>]
                                 --out <path> [--root <dir>]
```

**One verb, not two.** A `normalize` verb (merge duplicate `<class>` entries, dedup repeated lines,
recompute rates) was specified and then **cut** when SP7 showed the ingest already does all three and
never reads the report's own rate attributes (§5.1, §8).

- `--format` sniffed by default (`TN:`/`SF:` ⇒ lcov; `<coverage>` with `<class filename=…>` ⇒ cobertura,
  otherwise clover; a JSON object of paths ⇒ istanbul), always overridable — **sniffing must never be the
  only option**, and cobertura-vs-clover is exactly why: they share a root element.
- `--out-format` defaults to the input format. **For a branch-carrying `.pg` report, set it to cobertura**
  — branch data is emitted count-only (§4.4a) and only cobertura, clover and JaCoCo can express a count;
  lcov and istanbul are arm-keyed, so those outputs are line-only.
- `--branch-arms` opts back into arm-keyed emission. Safe **only** for a single-target consumer, where no
  cross-instrumenter union can occur; with two or more targets on one `.pg` file it inflates the number.
- `--target` names the plugin whose `originMapping` to use, resolved through the **existing** pgconfig
  plugin pipeline.
- A **`directive`-style target is a validating pass-through, not a no-op** (D8). It performs no remap —
  the report is already `.pg`-keyed — but it *does*: verify that it really is (catching
  `PolyglotOriginInfo` being off, whose only symptom today is a mysteriously lower percentage), enumerate
  the complete file set by scanning the generated output for `#line` directives and zero-fill the
  untouched modules (§4.2a), and rebase paths against `--root`. Format conversion only where lossless;
  count-only→per-arm is refused with a named diagnostic.
- `--generated-dir` overrides where the emitted output is looked for; the default is derived from
  pgconfig's `include` rules.
- `--root` states the directory paths are emitted relative to. The tool emits **repo-relative-from-a-
  stated-root paths and stops there**: report-path conventions for a specific coverage service are the
  consumer's business, not Polyglot's.
- Exit codes follow the CLI's existing convention; a malformed report is a **named diagnostic**, never a
  silent `Null` (§6's strict-posture requirement).

The end-to-end consumer shape this produces, with no Node required anywhere:

```
coverlet  → coverage.cobertura.xml  → (already .pg-keyed — upload as-is)                ─┐
vitest    → lcov.info | cobertura   → polyglot coverage remap → .pg-keyed report        ─┼→ upload
coverage.py / php-code-coverage → … → polyglot coverage remap → .pg-keyed report        ─┘
```

The C# leg needs **no Polyglot step at all** — coverlet's report is already `.pg`-keyed with native branch
data — and it stays on coverlet's **cobertura** reporter, whose branch data is count-only and therefore
safe to overlay. Every other leg passes `--out-format cobertura` for the same reason (§4.4a). An earlier
draft recommended lcov everywhere on fidelity grounds; that is **withdrawn** — arm-keyed emission
over-credits once two targets report the same `.pg` file.

Both land on the same `.pg` files, and the server's existing max-merge over
`(repo, sha, runId, runAttempt)` overlays them — which is exactly what produced the 98.12%.

## 8. What must NOT be built

- **No merging across targets.** coverage.mintplayer.com already max-merges reports sharing
  `(repo, sha, runId, runAttempt)` — verified by SP7 as the literal document id
  `Commits/{repoId}/{sha}/builds/{runId}-{runAttempt}`, with a per-line `Math.Max` on hits and on status.
  That is what produced the 98.12%. Polyglot's job ends at *"emit one `.pg`-keyed report per target."*
- **No `normalize` verb.** Specified, then **cut** when SP7 falsified its premise (§5.1). Merging
  duplicate `<class>` entries, deduping line numbers and recomputing rates are all things the ingest
  already does, and the rate attributes it would have fixed are never read. Building it would have been
  ~200 lines answering a question the server had already answered.
- **No format DSL in the manifest.** `reportFormat` in a plugin was proposed and rejected (§3). A new
  format is a reader added once; it is never manifest configuration for parsing arbitrary reports.
- **No coverage-service path conventions.** Re-rooting for a particular server stays consumer-side.
- **No column fidelity.** Threading positions through the expression-rule interpreter (which returns bare
  strings) is a far larger job, deliberately deferred by P38 N1. If line granularity proves insufficient
  for branches, §3.1's fork reopens — a column retrofit is not the answer.
- **No `coverage.py` file-tracer plugin.** It is the architectural prior art (`django_coverage_plugin`,
  Cython's `.pyx` recovery from embedded C comments — the Python analogue of the `#line` trick) and it is
  genuinely cleaner *for Python*, since the mapping is consumed at trace time and every coverage.py
  reporter is then correct for free. Rejected because it needs the same sidecar as its data source, adds a
  Python package plus a runtime dependency to the consumer's test run, and **has no PHP counterpart** — a
  third `style` is not worth one ecosystem. Recorded, not forgotten.

## 9. Acceptance criteria

- **A1** — `plugins/python` and `plugins/php` declare `originMapping`; `--origin-info` produces a valid v3
  sidecar plus footer for all four targets, and **no C++ file changes to achieve it** (the trait-flag fix
  of §4.7 is a separate, deliberate change).
- **A2** — `polyglot coverage` resolves its target's `originMapping` through the **existing** plugin
  pipeline, with no second resolver and no new introspection surface; a malformed report produces a named
  diagnostic and a non-zero exit, never a silent empty result.
- **A3** — `polyglot coverage remap` projects **cobertura, lcov, istanbul JSON and clover** reports onto
  `.pg` files for TypeScript, Python and PHP; every `--format`×`--out-format` pair round-trips; sniffing
  distinguishes cobertura from clover by `<class filename=…>`, not by the root element; `--target csharp`
  exits 0 with the no-op message.
- **A4** — line hits merge with `max`. Branch arms merge by **union inside the projection** (one target,
  comparable keys) but are **emitted count-only** (§4.4a), so `--out-format cobertura` is the branch-
  carrying default and lcov/istanbul output is line-only unless `--branch-arms` is passed. All totals are
  recomputed from the merged set; `taken = '-'` survives distinctly from `0`. Two fixtures: two generated
  2-arm branches at 1/2 each onto one `.pg` line ⇒ **2/4**; two *targets* covering the same logical arm of
  one `.pg` line ⇒ **1/2, not 2/2**.
- **A5** — the denominator is the mapped-line set; a `.pg` line with no mapping is **absent** from the
  report, in either direction. **And the file set comes from the origin data, not the report** (§4.2a): a
  `.pg` module absent from the input report is emitted with its mapped lines at zero, from sidecars on
  `sourceMapV3` targets and from a `#line` scan on `directive` targets. Fixture: a module compiled to both
  targets yields **identical file sets** from both enumerations.
- **A6** — generated files are selected by footer/sidecar, never by name glob; a hand-written
  `*_solver.spec.ts` beside a generated `*_solver.ts` is not swallowed.
- **A7** — **revised (D4, 2026-09-18): the output is a freshly constructed, minimal, valid report**, not an
  annotated copy of the input. The original criterion (unknown records and attributes survive
  byte-identically) was **withdrawn as incoherent for this operation**: this is a translation between
  different entities, not a round trip — the input describes `snake_solver.ts`, the output describes
  `snake_solver.pg`, and *N* generated classes fold into one `.pg` file, so "preserve verbatim" has no
  defined answer precisely where the tool does its work. The writer emits only what the format requires
  plus what was computed. Byte-stability is still pinned for the **identity** path (same format in, same
  format out, no remap).
- **A8** — **cut.** The `normalize` verb it specified was removed when SP7 falsified its premise. Replaced
  by: the emitted report survives the ingest's real rules — a path resolvable under **both** suffix
  directions with no ambiguous basename, `<line>` elements that always carry an explicit `hits` (a missing
  `hits` is read as **not covered**, not as unknown), lines as direct children of `<class><lines>` (a
  `<methods>` section is ignored), and UTF-8 without BOM.
- **A9** — PG-C7 proves Python and PHP end to end with **zero lines of engine code** beyond A1's manifest
  entries. If either needs engine work, the design is wrong and is revisited before more targets are added.
- **A10** — the subcommand builds on **both** toolchains (MSVC v145 and the POSIX g++/clang CMake path),
  its sources are in `.vcxproj`↔CMake parity (`scripts/check-buildfile-parity.ps1` is the guard), and a
  gate leg exercises remap over committed fixtures with no external toolchain required.

## 10. Open questions

- **Q3 (new, 2026-09-18) — RESOLVED same day: count-only.** Created by the upstream fix rather than by
  this design: once the ingest unions arm sets, union correctness depends on key comparability that the C#
  and TypeScript legs do not have, and the failure is an **over**-credit in the common case (§4.4a).
  Assessed as **not a Spark defect** — the ingest's contract is satisfied by every consumer it was built
  for, and only a cross-instrumenter overlay violates it, which the server cannot detect. Resolution:
  `.pg`-keyed reports emit branch data count-only, and the C# leg keeps coverlet's cobertura. **This
  reverses the earlier "lcov everywhere" recipe.** A docs note upstream, recording that arm keys are
  assumed to share one instrumentation id space, is optional and blocks nothing. The exact answer remains
  §3.1's instrumentation fork.

- **Q1 — RESOLVED 2026-09-18 by SP7, read from `MintPlayer.Spark/apps/CodeCoverage`.** Yes, the ingest
  merges `<class>` entries sharing a `filename`, counts each line number once, and never reads the
  report's own rate attributes — so the C# double-count never reaches the numbers and **`normalize` was
  cut** (§5.1, §7). The spike also produced three findings the design did not anticipate: the
  `BranchFormat` stamping hazard (§4.4a), the fact that `verify-coverage-paths.ps1` is wrong in **two
  opposite directions** (§5, slice 9), and that the ingest supports **only lcov, cobertura and JaCoCo** —
  istanbul JSON is rejected outright and clover is rejected as `noFiles`. That last finding constrains
  what a consumer **uploads**, not what this tool **reads**: istanbul and clover are in scope as readers
  and writers (§3), and the constraint lives in the CI recipe.
- **Q2 — P38's Q0: RESOLVED 2026-09-18, no rename.** The maintainer granted a free hand to rename; the
  answer is to keep `--origin-info`. Q0 as written never named an alternative (it proposed renaming
  `--origin-info` to `--origin-info`), and "origin" is already the project's **target-neutral** vocabulary
  for this concept: the manifest key is `originMapping`, the Core carries `OriginRecord` and
  `recordsOrigins()`, and pgconfig spells it `originInfo`. The flag reading narrow was an artifact of the
  C# `directive` sink shipping first, not of the word. Every "wider"-sounding candidate is in fact
  narrower — `--source-maps` is wrong for C#, `--line-directives` is wrong for TS/Python/PHP — and would
  put one sink's spelling on the user-facing surface, which the language-agnostic directive forbids.
  Keeping the name also avoids a three-surface rename (flag, pgconfig `originInfo`, MSBuild
  `PolyglotOriginInfo`). **Closed; not to be reopened without a named alternative.**
- **Q4 — branch data on PHP** requires Xdebug; PCOV is line-only. PG-C7 should state which it proved with.
  (Renumbered from Q3, which the cross-target arm-identity question took.) Tracked as SP9.
- **Q5 — RESOLVED 2026-09-19: validating pass-through** (D8). Decided on denominator grounds rather than
  ergonomics: once the file set comes from origin data (Q6/D7), a no-op would leave **C#-only** `.pg`
  modules unenumerable, since C# has no sidecars — reopening the exact hole D7 closes. Two further
  arguments point the same way: a uniform CI line across all four legs, and catching `PolyglotOriginInfo`
  being off. Cost accepted: a second origin-extraction path (`#line` scan) alongside the sidecar reader,
  pinned against each other by an A5 fixture.
- **Q6 — RESOLVED 2026-09-19: the file set is origin-driven** (D7, §4.2a).

**All design questions are resolved.** What remains is implementation-time: SP8's residue (extractive
readers without a DOM), SP9/Q4 (Xdebug vs PCOV for the PHP proof), and the provisional milestone number.
