# P39 — Coverage attribution — investigation evidence

> The code-grounded findings behind `PRD.md` and `PLAN.md`. Produced 2026-09-18 by a 5-agent
> investigation: the Core's origin path, the consuming repo's interim tool and PRD, this repo's
> npm/gate/packaging surface, the C#/coverlet PDB path, and an ecosystem adopt-vs-build survey.
> **Three claims were overturned** (§5). Everything below is cited; where a claim could not be verified
> it says so rather than inferring.

---

## 1. The Core's origin path

### 1.1 Origin recording is genuinely target-neutral

`EmitterBase::emitLineWithOrigin` (`src/MintPlayer.Polyglot.Core/src/emitter_base.cpp:1733-1745`) collects
`OriginRecord{outputLine, fileId, sourceLine}` whenever `recordingOrigins()`. The serialiser choice is
made *later*: `emitOriginDirective` (`:1674-1697`) early-returns for any non-`Directive` style. `origin_`
is armed once per emit at `:1582`:

```cpp
origin_ = (m.originInfo && originMapping_ && originMapping_->recordsOrigins()) ? originMapping_ : nullptr;
```

Three supporting invariants, all style-agnostic: `line()` splits embedded newlines so there is one record
per *physical* line (`:1716-1730`); `++outLine_` inside `emitOriginDirective` (`:1696`) keeps recorded
line numbers accurate even for a hypothetical both-sinks target; and `inlineBlock()` (`:1794-1815`)
saves/restores `outLine_` and sets `suppressDirectives_` so scratch-buffer text cannot inflate numbers.

**Conclusion: Python and PHP are missing a sink declaration, not the data.**

### 1.2 The `style` vocabulary is closed and validated

Exactly `directive` and `sourceMapV3`, enforced at plugin load (`src/MintPlayer.Polyglot.Core/src/backend.cpp:246-273`)
with a named error per failure mode: unknown style (`:253`), `directive` missing `line` or `hidden`
(`:263`), `sourceMapV3` missing `sidecarExtension` (`:267`), `originMapping` not an object (`:271`).
Absent ⇒ `Style::None` ⇒ byte-identical output. Pinned by `tests/MintPlayer.Polyglot.Tests/src/tests_main.cpp:2997-3022`.

There is **no JSON Schema file** in the repo; the manifest contract is prose in `docs/plugin-authoring.md:41`
and §3a (`:46-79`). Unknown *top-level* manifest keys are **silently ignored** — additive for new plugins,
but an old CLI silently drops a new key.

Current declarations:

| plugin | `originMapping` |
|---|---|
| `plugins/csharp/polyglot-plugin.json:4-9` | `{"style":"directive","line":"#line $n \"$f\"","hidden":"#line hidden","column":0}` |
| `plugins/typescript/polyglot-plugin.json:5-9` | `{"style":"sourceMapV3","sidecarExtension":".map","footer":"//# sourceMappingURL=$f"}` |
| `plugins/python/polyglot-plugin.json` | **absent** |
| `plugins/php/polyglot-plugin.json` | **absent** |

### 1.3 The footer is 100% manifest data — PG-C1 costs zero C++

`main.cpp:311-329` does a generic `$f` → sidecar-filename substitution with **no comment-syntax knowledge
at all**, guarded by `if (!om.footer.empty())`. The only `//#` string in the tree is the TypeScript
manifest value. Sidecar path = emitted path + `sidecarExtension` (string concat, so `solver.ts` →
`solver.ts.map`).

**Verified: adding `originMapping` to the Python and PHP manifests is sufficient for the whole pipeline** —
validation, recording, prelude shift, sidecar write, footer append and the `--origin-info` refusal/notice
logic are already generic. `directive` is not a viable alternative for either language (no `#line`
analogue, and `directive` hard-requires `hidden`).

### 1.4 `sourcesContent` is already emitted (SP5)

`src/sourcemap.cpp` exposes `vlqEncode` only — **there is no decoder**. `buildSourceMapV3` (`:72-88`)
emits `sourcesContent` with one entry per source and `null` where the host could not read the file
(`:79-84`), filled by the CLI at `main.cpp:276`. `writeSourceMap` (`main.cpp:254-288`) makes `sources`
relative to the *map's own* directory — which is why pgconfig `include` routing can put the `.pg` in a
different tree and the map still resolves — and writes **no sidecar at all** when nothing is resolvable
(`:281`).

Line granularity only: every segment is at column 0 of both sides (P38 N1, deliberate — the expression
rule interpreter returns bare strings).

### 1.5 The expression collapse is engine-wide, not Python-specific (SP1)

Static evidence in `plugins/python/polyglot-plugin.json`: `Match` at `:2896` is
`{"tmpl": ["(lambda _m: ", {"fold": …}]}` — an N-arm match lowers to a single-line lambda chain;
`"List.removeAll"` at `:3765` is a comprehension; `Math.sign`/`Math.clamp` at `:3805-3806` are one-line
lambdas.

Empirical: `docs/lang/samples/03_enums_unions_match.pg` lines 25–30 (a 6-line `match`, four arms) emit as
**one** physical Python line. **The same sample collapses identically on the C# `directive` target** —
`outcs/m.cs:21` is a one-line `switch` expression. So multi-source-line → one-output-line is a property of
expression rendering in the shared engine, which the merged P38 already lives with.

Python is in fact *better* placed than C#: `expressionOnlyLambdas: true`
(`plugins/python/polyglot-plugin.json:63`) routes through `hoistBlockLambdas` (`src/hoist_block_lambdas.cpp`),
turning block lambdas into real `LocalFunc` statements that each get their own line and origin — whereas
C# uses `inlineBlock()` and *suppresses* directives inside it (P38's documented N5 gap,
`emitter_base.cpp:1804-1808`). No Python rule template contains an embedded `\n` (0 occurrences).

### 1.6 PHP never closes `?>` (SP2)

`plugins/php/polyglot-plugin.json:3315` — `Program` opens with `{"line": "<?php"}`, and there is **no
closing `?>` anywhere in the manifest** (0 occurrences). A PHP file always ends inside PHP mode, so the
existing generic tail-append is already correct; `#`, `//` and even the JS `//#` spelling all parse as PHP
comments. **No placement logic is needed** — but the assumption deserves a regression test, since adding
`?>` to `Program` later would move the footer into HTML output mode.

### 1.7 Two target-name comparisons sit inside the origins path

`grep '== "csharp"'` over the Core finds exactly two, both pre-existing (issue #14 prelude hoisting) and
both inside the function that produces origins, `src/MintPlayer.Polyglot.Core/src/compiler.cpp`:

- `:681` — `const bool splitPrelude = (target.name() == "csharp") && lib.sharedPrelude;`
- `:722` — `const bool preludeEverywhere = target.name() != "csharp";`

These matter because prelude prepending **shifts every recorded origin line by the prepended count**
(`emitter_base.cpp:1615-1626`, `if (origin_)`, style-agnostic), and **Python is the only target that
declares preludes** (`plugins/python/polyglot-plugin.json:42-46`). The moment Python gains a sourceMapV3
sink, correct line numbers depend on a branch keyed by a string comparison. The `crossDirImports`
precedent (`docs/prd/issue-69-source-attribution/ANALYSIS.md:191-199` — "a flag gating a compiler pass or
host behaviour is top-level") is the shape of the fix.

### 1.8 `--origin-info` and the absence of an introspection surface (SP6)

One flag, no sub-flags, no value (`main.cpp:718-723`), default off, also settable as pgconfig
`"originInfo": true` (`pgconfig.hpp:105`) with the flag winning. Refusal policy (`main.cpp:630-648`): per
pgconfig group, exit 64 if **no** selected target records origins; an informational line if only some do.

**There is no `polyglot plugins` subcommand and no `--json` plugin listing.** Dispatch is
`-h/--help`, `--version/-v`, `build`, `fmt`, `check`, `lsp`, `install` (`main.cpp:1697-1723`);
`--emit-arm-trace` is a pre-dispatch debug flag explicitly outside the stable CLI contract. Plugin
resolution lives in `pluginresolve.hpp` (`file:` refs → in-box → verified cache → npm registry, SRI-
verified) and `Backend::originMapping()` (`backend.hpp:126-130`) is already a public accessor. **This is
what D2 makes moot**: an in-process subcommand reuses all of it.

---

## 2. The consuming repo: the interim tool, and the measurements

### 2.1 The tool

`C:\Repos\MintPlayer.AI\tools\pg_coverage_remap.mjs` — 217 lines, zero dependencies, node ESM. Invoked
from CI only (`.github/workflows/pull-request.yml:150-157`, `build-master.yml:106-113`), never from a
package.json script. Reads istanbul `coverage-final.json` **only**; writes cobertura **only**.

### 2.2 Its three defects, verified

1. **First-wins** (`:72-75`): *"First segment on a generated line wins"* — every segment after the first is
   discarded. Deltas still accumulate correctly, so this is a credit bug, not a corruption bug.
2. **On-disk path resolution** (`:119-122`): resolves `sources[]` against `dirname(mapPath)` + `sourceRoot`.
   **`sourcesContent` appears nowhere in the file** — and Polyglot already embeds it.
3. **Name glob** (`:105`): `if (!tsPath.endsWith('_solver.ts')) continue;` — no `sourceMappingURL` check.
   It excludes the hand-written `mountaincar_solver.spec.ts` **by accident**, as the consuming PRD itself
   notes.

### 2.3 No branch data at all

`branches-covered="0"`, `branches-valid="0"`, `branch-rate="0"` and `branch="false"` are **hardcoded
literals** (`:184`, `:188`, `:196-197`, `:202`). The istanbul `branchMap`/`b` fields are never read. So
branches are **absent, not approximated** — which is why PG-C5 is a milestone rather than a refinement.

### 2.4 The measurements, and the merge-semantics warning

| number | source | method |
|---|---|---|
| **93.95% → 98.12%** (3,494 → 3,649 of 3,719) | `COVERAGE_90_PRD.md:930` | both pipelines run, merged server-side |
| **155 lines** | the arithmetic delta (`:932`) | *not* the 137 in the workflow comments, which was a hand-traced pre-estimate |
| **snake 516 mappable / 831 physical** | `:955` | per-target `.pg` line-set extraction |
| **tetris 1,244+ generated → 797 distinct `.pg`** | §4.1 of the consuming PRD | same table |
| **C# branches 6,169/8,932 = 69.1%** | §4.4 | coverlet's own cobertura totals, rendered in the UI |
| **vitest 1,176 `BRDA:` = `branches-valid="1176"`** | §3 | both artefacts from one run |

**The live confusion the merge rule must avoid:** with `SingleHit=true` the coverage UI shows `cfFruitOf`
as `1×` for something that executes ~368 million times, and a `4×` badge means *four contributing
statements*, not four executions. Hence PRD §4.3: if a genuine Σ is ever wanted it is a separate field,
never `hits`.

### 2.5 Symmetric difference zero — the strongest single finding

Extracting the `.pg` line set each target emits origin info for, across nine solvers — C# by scanning
`#line N "….pg"` in the generated `.cs`, TS by decoding the `.ts.map` `mappings` — gives **3,949 lines,
0 C#-only, 0 TS-only** (`COVERAGE_90_PRD.md:940-957`). Worked example: `snake_solver.pg:271` → `#line 271`
at `snake_solver.cs:583` → a mapping to `snake_solver.ts:271`.

**One emitter, one origin table, two renderers.** This is what justifies a single shared projector, and
what refutes the idea that coverage handling is per-language.

### 2.6 The C# consumer setup is one property

`<PolyglotOriginInfo>true</PolyglotOriginInfo>` in the environments csproj (`:23`), measured at
**59.86% → 68.11%**, with the nine solvers entering as 3,493/3,719 (93.9%). One consequential side effect:
the `**/obj/**` exclude in `coverlet.runsettings` had to be **left alone** — coverlet matches `ExcludeByFile`
against the **PDB-recorded path**, not the file on disk, so the solvers now enter the report as
`src/**/polyglot/*_solver.pg`, which *is* in `git ls-files`. `UseSourceLink=false` for the suffix-matching
reason.

---

## 3. The C# path: true with three caveats

The mechanism is real and verified at coverlet's source: `Instrumenter.cs` keys documents purely by
`sequencePoint.Document.Url`; nothing consults the physical `.cs` path; `IsHidden` sequence points are
skipped at instrumentation time and **never become a record** — the source-level proof of P38's
"hidden lines leave the report". But "no tooling, ever" is **false**:

1. **Duplicate `<class>` entries sharing one `.pg` filename, with repeated line numbers.**
   `Coverage.GetCoverageResult` groups **document → class name → method name** with no dedup by line;
   `CoberturaReporter` emits one `<class>` per class name, all sharing the `.pg` `filename`. Any `.pg`
   file emitting more than one C# type — or hoisting a closure into a generated class, or producing an
   iterator/async state machine — puts the same `.pg` line in several `<class>` blocks, so the package and
   overall `line-rate` **double-count it in numerator and denominator**. Coverlet's own "many-to-one
   collapse is benign" holds only *within one method's* line dictionary. ReportGenerator merges by
   filename and absorbs this; a naive consumer does not. **Whether coverage.mintplayer.com merges by
   filename is not established anywhere in-tree** — SP7.
2. **A silent assembly-drop gate P38 never documented.** `InstrumentationHelper.PortablePdbHasLocalSource`
   checks each PDB document **exists on disk**; `ExcludeAssembliesWithoutSources` defaults to `MissingAll`,
   which saves us only because the `obj/…/*.cs` documents survive alongside the `.pg` ones. `MissingAny`,
   or build-on-A/test-on-B, drops the **whole assembly** without an error.
3. **Exclusion globs invert** (§2.6) — zero *tooling*, but not zero *config*.

Also verified: no .NET coverage tool in the set uses the physical `.cs` path over the PDB, so none needs
*remapping*. AltCover has the same model plus a `--localSource` landmine; **Microsoft.CodeCoverage /
dotnet-coverage is inferred, not verified** — its docs never mention `#line`; Fine Code Coverage is a
wrapper and inherits its engine. Deterministic builds + PathMap rewrite the `.pg` path to `/_/…` exactly as
they do the `.cs`; keep `UseSourceLink=false` and `DeterministicReport=false` (both already default false).

Coverlet's cobertura `filename` is relative to the **longest common prefix across all documents in the
report**, so one document outside the repo pushes `.pg` filenames into a *longer* form —
and `scripts/verify-coverage-paths.ps1:44-48` accepts only `tracked.EndsWith("/$report")` while its
docstring claims "or vice versa". Flagged during P38 (§4.I measure 2) and **never landed**. Coverlet also
re-normalizes separators to the platform's (`\` on Windows) regardless of the forward slashes in the
directive.

---

## 4. Packaging, gate, and the ecosystem

### 4.1 This repo's surface

The four plugin npm packages are **data-only** (`files: ["polyglot-plugin.json"]`, no `main`, no `bin`, no
deps). Publishing is `release.yml`'s `plugins:` job (`:357-423`): stamp via `npm version "$ver"` over a
`plugins/*/` glob, then a raw `npm publish` (the shared action swallowed npmjs errors, diagnosed
2026-07-04), then GitHub Packages via the org action with `folder: plugins`.

**There is essentially no JS/TS source in this repo** — `git ls-files` finds two JS files
(`editors/vscode/extension.js`, `tests/registry/registry-server.js`), **no `tsconfig.json` anywhere**, no
bundler, no JS test runner. The root `package.json` is `private: true` with a single devDependency (`nx`)
and a description stating it exists for gate orchestration only. An npm package with logic in it would have
been the repo's **first** JS build target — one of several reasons D2 went the other way.

Lockstep is enforced in `pluginresolve.hpp`: `targetNameForPackage` (`:53-56`) maps an npm name to a target
only for the literal `@mintplayer/polyglot-target-` prefix; the in-box rule (`:121-131`) returns
`out.version = Compiler::version()`, injected at build time and asserted against the tag in `release.yml`
(`:87-88`, `:164-165`, `:254-255`).

`scripts/build-and-test.ps1` is a **flat hand-written sequence**, not a leg registry — each leg is three
lines. NX registration is two edits: a `switch` arm in `scripts/nx-leg.ps1:32-48` and a `project.json`
target. Every runner stays directly invocable without NX; `release.yml` never touches NX.

`scripts/arm-trace-to-lcov.ps1` is the precedent worth copying as a *contract shape*, not code: repo-
relative paths (explicitly because the server matches `git ls-files`), a denominator from the compiler's
own parse so an unexecuted unit reports **uncovered rather than missing**, `exit 2` on malformed or empty
input, and a minimal lcov subset with no `FN`/`BRDA`.

### 4.2 The MSBuild hook is dead (SP3)

`src/MintPlayer.Polyglot.MSBuild/build/MintPlayer.Polyglot.MSBuild.targets`: two targets, both
`BeforeTargets="CoreCompile"`; **no `VSTest`, no `Test`, no `AfterTargets="Test"` anywhere**; the csproj
packs only `build\**`. Decisively, the `Exec` passes **no `--target`** ("the consumer's pgconfig decides")
and the non-C# output locations are resolved inside the CLI from pgconfig `include` rules — they are
deliberately **not** in `@(FileWrites)` and never surface as MSBuild items. So a hook **cannot see the
targets where a remap is needed**. Expected answer confirmed for ten minutes' work.

### 4.3 Ecosystem

- **Map decoding:** had this been JS, `@jridgewell/trace-mapping@0.3.31` (226M dl/wk, pure JS, no WASM,
  synchronous) over `source-map@0.8.0` (still ships a 48 KB `mappings.wasm`, async consumer, manual
  `.destroy()`). Moot under D2.
- **`istanbul-lib-source-maps`** — never adoptable: its I/O is the istanbul coverage object on **both**
  ends, so every non-JS path needs a lossy adapter in and out around ~200 lines of real work. Dormant:
  no release in 26 months, `nyc@18` still pins v4. Three algorithms are worth porting as ideas
  (`originalPositionTryBoth`; end-position via the next segment falling back to `Infinity`; drop ranges
  whose endpoints resolve to different files). **`remap-istanbul` is dead** — v0.13.0, 2019, deprecated in
  its own README, depends on deprecated `istanbul@0.4.5`.
- **Format libraries:** nothing credible. No maintained lcov *writer* exists on npm at all; `lcov-parse` is
  9 years stale, matches `FNDA` by name (mis-assigning overloads) and drops `[exception]`/checksum fields.
  Every cobertura option drags in `xml2js`; `cobertura-parse` (2018) even lists `mocha` as a *runtime*
  dependency. Hand-rolling was the answer in either language.
- **Branch identity:** lcov's `BRDA:<line>,[<exception>]<block>,<branch>,<taken>` assigns `block`/`branch`
  **no semantics**; istanbul writes `block` = an opaque per-file counter, `branch` = the arm index. So
  synthesized ids are legitimate. `getLineCoverage()` merges lines with **max**; `getBranchCoverageByLine()`
  **unions** the arms. `taken='-'` ≠ `0`. Cobertura's `condition-coverage="p% (c/t)"` is already the
  per-line model with no arm identity.
- **Python/PHP precedent:** **nothing in the wild consumes a v3 source map for either.** Python solves it
  with coverage.py *file-tracer plugins* (Cython recovering `.pyx` lines from embedded C comments — the
  closest analogue to `#line`; `django_coverage_plugin` by coverage.py's own author; two Jinja2 plugins),
  which map at **trace time** so every reporter is then correct for free. PHP has **no precedent at all** —
  Xdebug/PCOV collect at the bytecode level, and PHPUnit's own docs note this "can make it impossible for
  coverage data to be mapped back exactly to the source code level". `coverage.py` emits xml (cobertura),
  json and lcov; **php-code-coverage emits Clover, Cobertura, Crap4j, OpenClover, Text, Xml and Jsonl —
  no lcov**, which is why cobertura is mandatory and first. PHP branch data requires **Xdebug**; PCOV is
  line-only (SP9).
- **Comparable transpilers:** the majority instrument at source/AST level — Haxe (`haxe-instrument`
  macros → JaCoCo; `mcover` before it), ReScript (`bisect_ppx`), the TS/babel path
  (`istanbul-lib-instrument` composing an `inputSourceMap`). Dart collects from the VM in original
  coordinates. **Kotlin/JS declined the problem** — Kover is JVM/Android only. Only the JS/V8 world does
  post-hoc remapping, and only because V8 forces it. This is D1's fork, named rather than discovered later.

---

## 5. The three claims this investigation overturned

1. **"C# needs no tooling, *ever*."** False as stated. True for a single-assembly, non-deterministic,
   same-machine build whose report goes through ReportGenerator or a filename-merging server. Otherwise
   coverlet double-counts its own roll-up rate (§3.1), and a second, undocumented gate can drop a whole
   assembly silently (§3.2).
2. **"Combine many-to-one with `max`, never `sum`."** Right for line hit counts, **wrong for branch arms**,
   which union — otherwise two 2-arm branches at 1/2 each onto one `.pg` line report 1/2 instead of 2/4,
   understating the denominator and overstating coverage. Both halves are settled by istanbul's own
   canonical projections.
3. **"A v3 sidecar may not serve Python, because a `.pg` block can collapse into a comprehension."** The
   collapse is real but **not Python-specific** — the same sample collapses identically on the shipping C#
   `directive` target. It is a property of expression rendering in the shared engine, which P38 already
   lives with, and it is the *reverse-direction* restatement of "hidden lines leave the report".

## 6. Loose ends noted but out of scope here

- The consuming repo's `pull-request.yml:124-127` still says *"tools/pg_coverage_remap.mjs is retained but
  NOT used here"* — contradicted by the invoking step 25 lines below it in the same file. Consumer-repo
  fix.
- P38's **Q0 is still open**: `--origin-info` now also turns on the TS map, so the flag name reads narrower
  than the behaviour. Renaming is cheap now and expensive once consumers depend on it — and this work is
  exactly what makes them depend on it (PRD Q2).
- Issue #71's first comment is unrelated spam soliciting a third-party tool; it carries no design content.
