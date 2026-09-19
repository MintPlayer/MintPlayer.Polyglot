# P39 — Coverage attribution — implementation plan (dependency-ordered slices)

> Design contract: `PRD.md`. Investigation evidence: `ANALYSIS.md`. Issue: #71.
> Decisions **D1–D3** (maintainer review, 2026-09-18) are folded into both documents; the decision log is
> at the bottom of this file.
> Prerequisite: **P38** (`docs/prd/issue-69-source-attribution/`), merged as `90dace1`. This work consumes
> the artifacts `--origin-info` emits and does not re-open them.
>
> **UNBLOCKED 2026-09-18 (D5 satisfied).** [MintPlayer.Spark#420](https://github.com/MintPlayer/MintPlayer.Spark/issues/420)
> landed as `354de79` (PR #421) and **deployed successfully**. The ingest now merges branches
> format-agnostically and order-independently — per line it keeps an arm **set** from formats that
> identify arms and a **floor** from formats that only count them, with `covered = max(Floor, |Arms|)`
> and `total = Arity`; union and max make order-independence structural. The sum-vs-max asymmetry and the
> `null`-hits demotion are fixed in the same change, and **istanbul JSON and clover are now ingestible**.
>
> Parsers declare arm identity **per line**, not per format: lcov, istanbul and cobertura's `<conditions>`
> → arm-identified; cobertura's `condition-coverage`, JaCoCo's `mb`/`cb` and clover → count-only.
>
> **All design questions resolved (2026-09-19).** The last two — the file set's origin (D7) and the
> `directive`-style behaviour (D8) — are in the log below. What remains is implementation-time only:
> SP8's residue, SP9, and the provisional milestone number.

**Build discipline.** Implement ALL slices first, then build + run the full gate **ONCE** at the end
(`pwsh scripts/build-and-test.ps1`, or the `/build-and-test` skill). The mid-flight ceiling is
`-Tier fast` or a bare unit-test exe run — never the full gate per slice, never speculative extra legs.
Commit per slice is fine; it is the *test runs* that batch. Slice 0 (spikes) is the one exception.

**The POSIX leg is required, not optional.** This adds new C++ translation units and touches the build
files, so one WSL `cmake` + g++ compile and unit run is part of the **end** gate, not an extra. MSVC
accepting the code proves nothing about g++/clang, and `scripts/check-buildfile-parity.ps1` guards
`.vcxproj`↔CMake source-list drift as the first stage of `build-and-test.ps1`.

**One PR.** The manifest entries, the trait-flag fix, the subcommand, all four format readers/writers, the
projector, branch projection, the gate leg, the docs and the drive-by tripwire fix land together. No
follow-up PR, no phase 2.

---

## Slice 0 — spikes (throwaway, time-boxed)

**Six of the original spikes are already ANSWERED by the 5-agent investigation of 2026-09-18** — recorded
below rather than re-run. Three new ones replace them. Record every outcome in the **Log**; a spike that
fails changes the design per PRD §9, it does not get worked around.

| spike | status | outcome |
|---|---|---|
| **SP1** — does a v3 sidecar actually serve Python? | **ANSWERED** | Yes. The feared one-to-many *does* occur (a 6-line `match` renders to one line), **but it is not Python-specific** — the same sample collapses identically on the shipping C# `directive` target. It is a property of expression rendering in the shared engine, which P38 already lives with. Python is in fact *better* off than C#: `expressionOnlyLambdas: true` hoists block lambdas into real statements that each get their own origin, where C# inlines them and suppresses directives. **No design change.** |
| **SP2** — PHP footer placement, incl. a file ending in `?>` | **ANSWERED** | The PHP `Program` rule opens with `<?php` and **never emits a closing `?>`** (zero occurrences in the manifest), so a file always ends inside PHP mode and the existing generic tail-append is already correct. `# sourceMappingURL=$f`, `// sourceMappingURL=$f` and even the JS spelling all parse. **No placement logic needed**; add a regression test pinning the no-`?>` assumption. |
| **SP3** — is an MSBuild hook worth it? | **ANSWERED — NO** | Confirmed on all four points: the `.targets` is transpile-only, both targets hang off `CoreCompile`, nothing references `VSTest`/`Test`, and the csproj packs only `build\**`. Decisively, the `Exec` passes **no `--target`** and the non-C# output locations are resolved inside the CLI from pgconfig `include` rules and **never surface as MSBuild items** — so a hook cannot see the targets where a remap is actually needed. **Kills the MSBuild-automation milestone** for ten minutes' work, exactly as predicted. Automation is a documented CI line instead (slice 8). |
| **SP4** — adopt or build? | **DISSOLVED by D2** | Moot in C++: nothing to adopt. The npm branch would have adopted `@jridgewell/trace-mapping` and rejected `istanbul-lib-source-maps` (istanbul-shaped I/O on both ends, dormant 26 months) and `remap-istanbul` (dead since 2019). Three *algorithms* are ported as ideas — PRD §4.6. |
| **SP5** — the `sourcesContent` shortcut | **ANSWERED** | `sourcesContent` is **already emitted**, one entry per source, `null` where unreadable. Path resolution becomes a fallback only, removing the whole class of path-rooting bug that already bit once. |
| **SP6** — reuse the CLI's plugin resolution | **DISSOLVED by D2** | In-process reuse is now free. No `--plugin-dir`, no `polyglot plugins --json`. |
| **SP7** — Does coverage.mintplayer.com merge `<class>` entries sharing a `filename`? | **ANSWERED 2026-09-18 — read from `MintPlayer.Spark/apps/CodeCoverage`** | **Yes.** `CoberturaParser` groups by `filename` across `root.Descendants("class")`; lines land one-per-number in a `SortedDictionary`; rates are computed from the stored set and **the report's own rate attributes are never read**. So the coverlet double-count never reaches the numbers, and **slice 6 (`normalize`) is CUT.** Four findings the design did not anticipate: (1) the **`BranchFormat` stamping hazard** — branch edges arriving in a *second* format for an already-stamped file are silently discarded, which lands squarely on the C#-cobertura + TS-lcov overlay (PRD §4.4a); (2) `verify-coverage-paths.ps1` is wrong in **two opposite directions** (too strict *and* too permissive — slice 9); (3) the ingest supports **only lcov, cobertura and JaCoCo** — istanbul JSON and clover are rejected, which constrains what a consumer **uploads** but *not* what this tool reads or writes (both stay in scope, slice 3b; the constraint belongs in the CI recipe, never as a hardcoded refusal); (4) a branch-less report **cannot dilute** existing branch numbers (the merge skips the block entirely), refuting the risk recorded in the consuming repo's PRD. |
| **SP8** — XML round-trip fidelity with a subset parser in C++ | **DONE — an extractive scanner suffices** | Built and proved against a real `coverage xml` document and our own writers: a ~90-line tag scanner (comments, PIs, CDATA, entity-decoded attributes) plus per-format extraction handles cobertura and clover with no DOM. Two real defects it surfaced, both found by running the actual tool rather than by reasoning: a **self-closing `<methods/>`** was treated as an open section and silently skipped every following `<line>`; and `<sources>` must **not** be prepended to the filename — `coverage.py` writes an absolute `<source>` whose case does not match the filesystem, so joining it produced a path that then failed to match, while the bare filename matches by suffix without it. Path resolution belongs to the matcher, which is now case-insensitive and bidirectional like the ingest's. | The hard half was A7's "unknown attributes survive verbatim", and A7 was withdrawn as incoherent for a translation between different entities. What remains is small and no longer blocking: extractive readers for three XML dialects, and a **strict wrapper** over the Core's JSON reader for istanbul (it is lenient by design — returns `Null` on malformed input, the wrong posture for a third-party file). Residual spike: confirm an extractive reader handles real coverlet, vitest and php-code-coverage output without a DOM. |
| **SP9** — PHP branch data | **ANSWERED — neither driver is present** | The local PHP 8.5.8 has **no Xdebug and no PCOV**, so php-code-coverage cannot collect *any* coverage here, let alone branches. The PHP half of PG-C7 is therefore proved structurally (the sidecar + footer land, and the projector is format- and language-neutral) rather than by a live php-code-coverage run. This is a statement about this machine, not about the design: the PHP path needs no engine code, and its report — in whatever format that consumer emits — goes through the same reader set as everything else. A consumer wanting PHP **branch** data needs Xdebug; under PCOV the branch number is simply absent, which the count-only emission rule already treats as harmless. |

**Acceptance:** nine recorded outcomes; PRD §4–§6 amended in place where a spike contradicts them.

---

## Slice 1 — `originMapping` for Python and PHP, and the trait-flag fix (PG-C1)

Two changes that belong together: the manifest entries are trivially small, and the second is the reason
they are not *quite* safe on their own.

**1a — manifest data only, zero C++ change** (this is the whole point of the zero-engine-code claim):

```jsonc
// plugins/python/polyglot-plugin.json
"originMapping": { "style": "sourceMapV3", "sidecarExtension": ".map", "footer": "# sourceMappingURL=$f" }
// plugins/php/polyglot-plugin.json
"originMapping": { "style": "sourceMapV3", "sidecarExtension": ".map", "footer": "// sourceMappingURL=$f" }
```

Validation, origin recording, the prelude shift, the sidecar write, the footer append and the
`--origin-info` refusal/notice logic are **all already generic**. The only observable engine-side change
is that the refusal notice (`main.cpp:630-648`) stops naming python/php as unsupported.

**1b — replace the two target-name comparisons** in `compiler.cpp` (`:681`, `:722`) with manifest trait
flags `sharedPreludeFile` / `preludePerFile`, following the documented `crossDirImports` precedent. This
is **not** opportunistic cleanup: prelude prepending shifts every recorded origin line, Python is the only
target that declares preludes, and 1a is what makes those shifted line numbers load-bearing. Declaring
the existing behaviour rather than inferring it from a string comparison is the principled fix; leaving it
is a silent shortcut masquerading as design (PRD §4.7).

**Acceptance:** A1. `--origin-info` produces a valid sidecar + footer on all four targets; the cli-smoke
leg's P38 check extends from two targets to four; parity/refusal tests updated; no target-name comparison
remains in the origins code path.

---

## Slice 2 — the subcommand skeleton (PG-C2)

`polyglot coverage` dispatch alongside `build`/`fmt`/`check`/`lsp`/`install` (`main.cpp:1697-1723`):
argument parsing, `--format` sniffing with an explicit override, `--out-format` defaulting to the input,
`--root` and `--generated-dir` (default derived from pgconfig `include` rules), and target resolution
through the **existing** pgconfig plugin pipeline. Dispatch on `originMapping.style`: `sourceMapV3` →
project; `directive` → **validating pass-through** (D8), *not* an exit-0 no-op. No projection logic yet —
the skeleton's job is to make every failure mode a named diagnostic before there is anything to hide
behind. Help text and usage updated.

**Acceptance:** A2. A malformed report exits non-zero with a named diagnostic; an unknown `--format` lists
the known ones; a `directive` target reports what it validated rather than claiming a remap.

---

## Slice 3 — the format layer (PG-C3)

Reader + writer per format, over a **lossless record** that retains unrecognized records and attributes
verbatim (SP8 decides its exact shape). Every reader lowers to the same `(file, line, hits, branches)`
intermediate — the projector never learns which format it came from.

**3a — cobertura, then lcov (prerequisites).** Cobertura first: the only format php-code-coverage emits
that we also need, and coverlet, vitest and `coverage xml` all emit it. Neither reader parses anything it
does not need: `<class filename>` plus `<line number hits branch condition-coverage>` on one side; the
twelve `TN/SF/FN/FNDA/FNF/FNH/DA/LF/LH/BRDA/BRF/BRH` record types on the other. Model `taken = '-'`
**distinctly from `0`** (PRD §4.4). Mirror `istanbul-reports`' lcovonly and cobertura writers for the
output shapes; DTD `cobertura.sourceforge.net/xml/coverage-04.dtd`.

**3b — istanbul JSON and clover (same PR, after the projector proves out on 3a).** Istanbul is the
compatibility path — it is what the interim tool reads today, so supporting it lets the consuming repo
switch over without also changing its vitest reporter config. Its `statementMap`/`s` give statement-level
input, and `branchMap`/`b` give arms (which the interim tool never read). Clover completes the
php-code-coverage surface. **Sniffing must disambiguate cobertura from clover** — they share a `<coverage>`
root, so the discriminator is the presence of `<class filename=…>`; get this wrong and a clover file is
silently parsed as an empty cobertura.

Note the destination constraint is **documentation, not a refusal**: the tool writes istanbul and clover
on request even though coverage.mintplayer.com cannot ingest them (PRD §3, §8) — other services can.

**Acceptance:** A7 — read → write with no remap is byte-stable over committed fixtures taken from real
coverlet, vitest and php-code-coverage output, for all four formats.

---

## Slice 4 — the projector (PG-C4)

Format- and language-neutral: `(file, line, hits, branches)` in, the same shape keyed on `.pg` out.

- VLQ **decode** (new — `sourcemap.hpp` has `vlqEncode` only), map load, `sourcesContent` preferred over
  on-disk resolution with path resolution as fallback only.
- **Credit every segment** on a generated line, not first-wins.
- Select generated files by **footer/sidecar contract**, never by name glob.
- Port the three istanbul algorithms: `originalPositionTryBoth` (GREATEST_LOWER_BOUND then
  LEAST_UPPER_BOUND); end-position via the next segment falling back to `Infinity`; **drop any range whose
  start and end resolve to different original files**.
- **Line hits merge with `max`.** Denominator is the **mapped set** — a `.pg` line with no mapping is
  absent from the report, never reported as uncovered.
- **Enumerate the file set from origin data, not the report** (D7, PRD §4.2a): union the `sources` of
  every `.map` sidecar under `--generated-dir` on `sourceMapV3` targets, and scan generated output for
  `#line` directives on `directive` targets. A `.pg` module with origin data but no report entry is
  emitted with its mapped lines at **zero** and counted in the summary line. **Both enumerations must
  agree** for a module compiled to both targets — an A5 fixture pins it, since this is the one place the
  two origin-extraction paths can silently diverge.
- Emit repo-relative-from-`--root` paths, forward-slashed, and stop there.

**Acceptance:** A3, A5, A6 — including the regression that a hand-written `*_solver.spec.ts` beside a
generated `*_solver.ts` is **not** swallowed.

---

## Slice 5 — branch projection (PG-C5)

**Two merges, opposite answers. Keeping them apart is the whole of this slice.** They differ only in
whether the arm keys being combined came from *one* instrumenter or two.

**5a — inside the projection (one target): UNION the arms.** Many generated lines fold onto one `.pg`
line, and those arm keys all come from the same instrumenter, so they *are* comparable and union is
correct. This is PRD §4.3's correction to the issue's "always max" rule — right for line hits, wrong for
arm denominators. Two generated 2-arm branches at 1/2 each onto one `.pg` line yield **2/4**, not 1/2.
Recompute every total from the merged set; `taken = '-'` stays distinct from `0`.

**5b — what we emit (consumed cross-target): COUNT-ONLY.** The ingest unions arm sets across reports, and
our C# and TypeScript keys come from different instrumenters for the same `.pg` line, so a union
**over-credits** whenever both suites cover the same arm — the common case (PRD §4.4a). We therefore emit
the per-line pair as a *count*, letting the server take `max(floors)`: an under-credit, which is honest,
instead of an over-credit, which is not.

**This constrains `--out-format`, and the constraint is forced rather than preferred:**

| output format | branch representation | emits branch data? |
|---|---|---|
| **cobertura** | `condition-coverage="p% (c/t)"` — count | **yes** — the default for branch-carrying reports |
| **clover** | `truecount`/`falsecount` — counts, aggregated per line | yes |
| **JaCoCo** | `mb`/`cb` — counts | yes |
| **lcov** | `BRDA` — arm-**keyed**, lands in the server's arm set | **no by default** |
| **istanbul** | `branchMap`/`b` — arm-keyed | **no by default** |

So `--out-format cobertura` is what the documented recipe uses for every `.pg`-keyed report, **including
the C# leg, which keeps coverlet's cobertura rather than switching to its lcov reporter.** Emitting lcov
or istanbul is still supported; those outputs simply carry line data only.

**`--branch-arms` (opt-in) re-enables arm-keyed emission** for a consumer with a *single* target, where no
cross-instrumenter union can occur and arm identity is strictly better. It must be opt-in and its help
text must name the hazard: with two or more targets reporting the same `.pg` file it inflates the number.

**Acceptance:** A4. Fixtures pin both halves — 5a's 2/4 case, and a 5b case where the same `.pg` line is
covered by two targets on the *same* logical arm and the emitted report yields **1/2, not 2/2**.

---

## ~~Slice 6 — `normalize`, the C# answer (PG-C6)~~ — **CUT by SP7**

This slice was gated on SP7 and SP7 killed it. The ingest already groups `<class>` elements by `filename`,
already counts each line number once, and **never reads the report's own rate attributes** — so the
coverlet double-count that motivated a `normalize` verb never reaches any number the server computes.
Building it would have been ~200 lines answering a question that was already answered.

What survives is documentation, folded into slice 9: the double-count is real in the *file*, so a consumer
reading coverlet's cobertura directly (rather than through the server or ReportGenerator) still sees an
inflated roll-up; and within one report the ingest **sums** duplicate line hits, so a `.pg` line reached
from two C# classes displays an inflated hit count while its covered/not-covered status and the percentage
stay correct.

*Slice numbering is left as-is rather than compacted, so the cut stays visible in the record.*

---

## Slice 7 — prove Python and PHP end to end (PG-C7) — **DONE for Python, structurally for PHP**

**Python: proved live, and the gate is met.** A real `coverage.py 7.16.1` run over the generated Python —
`coverage run`, then `coverage xml` *and* `coverage lcov` — projected onto `docs/lang/samples/…​.pg` in
both formats. **Zero lines of engine code** beyond slice 1a's four-line manifest entry (A9). The two
defects it exposed were in the new tool, not the compiler, which is exactly the boundary the milestone
asserts. Both reports are committed as gate fixtures so the leg needs no Python.

**PHP: structural.** SP9 found no Xdebug and no PCOV on this machine, so php-code-coverage cannot collect
anything here. What *is* proved: the sidecar and footer land, the footer stays inside `<?php` mode, and
the projector is language-neutral by construction — PHP's report goes through the same readers as
Python's. No engine code was needed for PHP either.

---

## Slice 7 (original scope, for reference)

*Not* new adapters: slice 1's manifest entries, then a real `coverage.py` and a real php-code-coverage run
— in whichever format that consumer emits — projected onto a `.pg`. **The gate is that neither needs a
line of engine code.** A new *format* is legitimately a reader; **if a new *language* needs engine work,
the design is wrong and is revisited before more targets are added** (PRD A9).

Record SP9's answer here: which PHP driver produced the proof, and whether branch data was present.

---

## Slice 8 — gate leg, NX target, CI (PG-C8)

- `tests/coverage/run-coverage.ps1` over committed fixtures, exercising remap across all four formats.
  **No external toolchain needed** — fixtures are checked in, so unlike the nuget leg this needs no
  guard-and-skip. Registered in `scripts/build-and-test.ps1` in the **full** tier (after `nullable`,
  before `library`), following the flat three-line leg convention.
- `scripts/nx-leg.ps1` switch arm + a `project.json` target with `dependsOn: ["build"]`, `cache: true`,
  `inputs: [tests/coverage/**, scripts/nx-leg.ps1, compilerSources, sharedGlobals]`, `outputs: []`.
  **The runner stays directly invocable without NX** — that is the standing contract.
- `ci.yml` path triggers already cover `src/**`, `tests/**` and `plugins/**`, so no new trigger is needed —
  and the leg **does** run in CI — as its **own bounded step**, not as a fourth `nx run-many` target
  (see the CI entry in the Log: as a parallel task it hung the job twice while printing nothing). It
  earns a place in a deliberately cheap POSIX gate for the same reason `watch` and `registry` are there:
  it is path-resolution-heavy (walks output trees, canonicalizes and re-roots, matches case-insensitively
  against a case-SENSITIVE filesystem), which is the class of bug a green Windows gate does not catch.
  The `polyglot:coverage` NX target stays in `project.json` for local use and caching.
- **Automation is one documented CI line per consumer**, not an MSBuild hook (SP3). If a future option
  changes emitted bytes it must join the incrementality stamp the way `PolyglotOriginInfo` does via
  `_PolyglotOriginInfoTag` — omitting that once already caused a silent "the feature doesn't work" failure.

---

## Slice 9 — docs, and the drive-by fixes (PG-C9)

- `docs/plugin-authoring.md` §3a: the `originMapping` table gains the Python/PHP examples and the two new
  trait flags.
- A coverage section in the docs: the consumer recipe per target, the §5 C# caveats **stated rather than
  discovered** (the `PortablePdbHasLocalSource` silent assembly-drop, the `ExcludeByFile` inversion, the
  deterministic-build interaction), and the merge semantics published per §3.C's never-relax-silently rule.
- **Drive-by, now with the server's real rules in hand (SP7):** `scripts/verify-coverage-paths.ps1` is
  wrong in **two opposite directions**, and the fix is no longer guesswork —
  `MintPlayer.Spark/apps/CodeCoverage`'s `PathNormalizer` is the specification to match:
  - **Too strict.** It implements only `tracked.EndsWith("/$report")`; the server matches **both**
    directions (`EndsWithPath(tracked, report) || EndsWithPath(report, tracked)`), so the script rejects
    the report-longer shape the server accepts — exactly what coverlet's common-prefix `filename`
    computation produces. Its own docstring already claimed both directions.
  - **Too permissive.** It accepts the first suffix hit; the server requires **exactly one** candidate, so
    an ambiguous basename passes the tripwire and comes back `Matched = false` — silently excluded from
    every number. This is the failure mode the tripwire exists to catch, and it currently does not.
  - Also missing: `<sources>` prefix stripping and root-dir stripping (both applied server-side before
    matching), and `.TrimStart('./')` uses char-set semantics, trimming any leading `.`/`/` rather than
    the literal `./`.
  Match the server's algorithm, and make the uniqueness failure a **named** finding rather than a pass.
- CLAUDE.md's coverage table gains a fourth row for `.pg`-keyed consumer reports.

---

## Decision log

| # | decision | rationale |
|---|---|---|
| **D1** | **Post-hoc report remapping**, not source-level instrumentation | Instrumentation is the *majority* architecture (Haxe macros, ReScript's `bisect_ppx`, the TS/babel `inputSourceMap` path) and would dissolve the branch-identity problem outright by assigning arm identity in the compiler once. Rejected because it changes emitted bytes, needs a runtime collector per target, and abandons P38's goal of leaving the consumer's native toolchain untouched. **Recorded as the fork to reopen** if per-line branch fidelity proves insufficient — a column-fidelity retrofit is not the answer. |
| **D2** | **A `polyglot coverage` subcommand**, not an npm package; and **no `polyglot-coverage.json`** | Reach: the NuGet package already ships the CLI at `tools/<rid>/`, so the subcommand arrives at a .NET or PHP consumer with no Node at all; npm does not. Reuses the existing plugin resolver in-process, killing SP6 and the `plugins --json` surface. Keeps "self-contained native CLI, zero runtime deps" literally true. Cost accepted: ~490 lines of C++ incl. a cobertura XML subset, in CMake/`.vcxproj` parity across five RIDs, with SP8 guarding the round-trip risk — **actual ~1,480 lines**, 3× the estimate, because it scoped only three of the pieces (PRD §6 records what it left out). A per-language coverage manifest was rejected separately: everything it would carry is already in `originMapping`, the projection logic is *not* per-language (symmetric difference zero across nine solvers), and the one thing that varies — report format — is the consumer's CI choice, which is the correction the issue's own third comment already made. |
| **D7** | **The `.pg` FILE set comes from the origin data, not from the coverage report** | A `.pg` module whose twin appears nowhere in the report — nothing imported it, no test touched it — would silently vanish under report-driven enumeration, and the percentage would read *higher* exactly when a module is least tested. Instead: enumerate from `.map` sidecars on `sourceMapV3` targets and from a `#line` scan on `directive` targets, emitting absent modules at zero. This is the arm tracer's contract one level up — the denominator comes from the compiler's own parse, so a never-fired unit reports **uncovered, not missing**. Needs the generated-output location, derived from pgconfig `include` rules with a `--generated-dir` override. Trap accepted: a module excluded from the test build is emitted at zero and *deflates* the number — the safer direction, and it surfaces as an unexplained drop rather than silence. |
| **D8** | **A `directive`-style target is a validating pass-through, not a no-op exit 0** | Reversed on denominator grounds, not ergonomics: **C# has no sidecars**, so under D7 a no-op leaves C#-only `.pg` modules unenumerable and reopens the hole D7 closes. Two supporting arguments — a uniform CI line across all four legs, and catching `PolyglotOriginInfo` being off, whose only symptom today is a mysteriously lower percentage. Cost accepted: a parse-and-re-emit step on the one leg that previously could not fail, and a **second** origin-extraction path (`#line` scan) alongside the sidecar reader; the two are pinned against each other by an A5 fixture asserting identical file sets for a module compiled to both targets. |
| **D6** | **Branch data is emitted COUNT-ONLY; `--out-format cobertura` is the branch-carrying default; the C# leg keeps coverlet's cobertura** | The upstream fix (D5) made the ingest union arm sets across reports — which is correct for every consumer it was built for, and wrong for *us*, because our C# keys come from coverlet over IL and our TypeScript keys from istanbul over JS. For one `.pg` line those are unrelated id spaces, so a union **over-credits** whenever both suites cover the same arm (the common case): 2/2 where the truth is 1/2, and *correct* when they cover different arms, which makes the error data-dependent and invisible. Count-only under-credits instead, and §3.D prefers an honest under-credit to a number claiming coverage nobody has. Consequence: only cobertura, clover and JaCoCo can carry our branch data (lcov's `BRDA` and istanbul's `branchMap` are arm-keyed), so lcov/istanbul output is line-only unless `--branch-arms` is passed — safe only for a single-target consumer. **Reverses the earlier lcov-everywhere recipe.** Assessed as *not* a defect in the ingest: supplying non-comparable keys is the producer's contract violation, and the server cannot detect it. The exact fix remains D1's instrumentation fork. |
| **D4** | **A7 withdrawn — the output is a freshly constructed report**, not an annotated input | Lossless round-trip is incoherent for a translation between different entities: every `<class>` gets a new filename, every `<line>` a new number, and *N* generated classes fold into one `.pg` file, so unknown per-class attributes have no defined destination at the merge point. Collapses most of SP8. Trap accepted: a consumer feeding our output to a tool that reads a field we drop gets zeros — loud and fixable, unlike preserving an attribute whose meaning no longer matches the remapped entity. |
| **D5** | **The Spark coverage-ingest fix lands and deploys FIRST**, then the Polyglot work | The maintainer set three imperative requirements — upload order must not change the result, branch edges must never be discarded, full support in one version — and **all three are violated by the ingest today**, independent of anything Polyglot does (`BranchFormat` is stamped by the first report to bring branches and later formats are silently dropped; duplicates sum within a report but max across reports). Sequencing the server first fits the one-PR rule's own carve-out ("sequencing only where a publish must happen first") and surfaces any storage-migration risk before Polyglot code exists. Tracked as **[MintPlayer.Spark#420](https://github.com/MintPlayer/MintPlayer.Spark/issues/420)**. Hard constraint discovered: cobertura's `condition-coverage` carries a count with **no arm identity**, so "never discard edges" is achievable but "never lose information" is not — two cobertura reports covering *different* arms of a line cannot be unioned by any storage model. |
| **D3** | **Lockstep, by construction** | As a subcommand the tool *is* the CLI: it ships at the tag version, `release.yml` already asserts `--version` against the tag, and CLAUDE.md's lockstep sentence needs no exception and no new publish plumbing. |

## Log

*(Slice outcomes and spike results recorded here as they land.)*

- **2026-09-18** — 5-agent investigation completed; SP1, SP2, SP3, SP5 answered and SP4, SP6 dissolved
  before a line was written. Three claims overturned: "C# needs no tooling **ever**" (false — coverlet
  emits duplicate `<class>` entries sharing one `.pg` filename, double-counting its own roll-up rate);
  "combine many-to-one with max" (right for line hits, **wrong for branch arms**, which union); and the
  premise that Python's expression collapse is a Python problem (it is engine-wide and already shipping on
  C#). Full evidence in `ANALYSIS.md`.
- **2026-09-18** — **SP7 answered from `MintPlayer.Spark/apps/CodeCoverage`**, and it **cut a milestone**:
  the ingest already merges `<class>` by filename, counts each line once, and never reads the report's own
  rate attributes, so `normalize` (slice 6) was removed rather than built. It also turned up the
  `BranchFormat` stamping hazard — the single most consequential finding for an *overlay*, since C#
  cobertura branches landing first would silently discard TypeScript lcov branches for the same `.pg`
  file (PRD §4.4a) — and refuted the feared branch-dilution risk outright.
- **2026-09-19 (CI) — the new leg hung the Linux job twice, and the fix is worth reading.** Its first two
  CI outings ran for **~38 minutes** and were cancelled, having printed **not one line**. Every other job
  on this repo finishes in about two minutes.
  - **Why it was invisible:** a task's output sits in the pipe buffer until the process exits, and this
    leg's entire output is a few hundred bytes. `watch` and `registry` stream because they print
    kilobytes. So the leg looked like a task that never started, and the hang could not be located from
    the log at all — only `nx run polyglot:coverage` appearing in the task list proved it had begun.
  - **Fixed three ways, and the honest position is that the cause is not fully isolated**: every check
    now flushes; every CLI call runs through `Invoke-Cli` with a 60-second bound that **drains both
    redirected pipes on background tasks** (a child filling a redirected pipe blocks forever if the
    parent only waits — the classic form of this deadlock, and the most likely culprit); and the leg
    moved out of the `nx run-many` line into its own step with `timeout-minutes: 6`. Whether the trigger
    was the pipe handling or being a fourth parallel nx task is **not distinguished** by this change.
  - **The rule it earns:** a new gate leg goes in bounded and flushing from the start. An unbounded leg
    that prints nothing costs a full job budget per attempt and tells you nothing about itself.
- **2026-09-19 (later) — gate leg widened, and a `.gitignore` trap found.**
  - The leg now also pins the **`directive` sink end to end** (a `.pg`-keyed report passes through, keeps
    its hits, has its file set completed at zero from the `#line` directives, and a report that is *not*
    `.pg`-keyed refuses — the "`--origin-info` was off" signature), and every **output format** through
    the CLI rather than only the two the goldens pin.
  - Added to `ci.yml` (initially to the `nx run-many` set; moved to its own bounded step after the hang
    below). Verified on POSIX by running the leg's own operations under
    WSL against the CMake build: all four targets emit their sink, PHP still never emits `?>`, and the
    committed golden matches **byte-for-byte** on Linux.
  - **`.gitignore` carried a bare `coverage/`**, which git matches at any depth, so `tests/coverage/` —
    the entire leg plus fixtures — was silently untracked. The files existed on disk, so the leg ran and
    passed locally and in the full gate while CI would have had no such directory. Root-anchored to
    `/coverage/`, which is what both producers actually write. Worth remembering as a shape: a gate that
    passes locally and cannot possibly run in CI.
- **2026-09-19 — IMPLEMENTED.** All slices landed on `p39-coverage-attribution`; slice 6 stayed cut. Full
  gate green (build + unit + every leg + differential conformance across the four targets), plus the POSIX
  leg: WSL `cmake`/g++ 13.3 Release builds clean, its unit run passes, and `polyglot coverage remap`
  produces a byte-identical `.pg`-keyed report on Linux and Windows. The registry leg skipped under the
  documented local loopback opt-out; CI runs it.
  - **SP8 and SP9 answered by doing** (see the spike table). SP8's two findings were real defects a
    real `coverage.py` run exposed and reasoning had not: a self-closing `<methods/>` silently swallowed
    every following `<line>`, and prepending `<sources>` produced a path whose case did not match the
    filesystem's, breaking the match that the bare filename would have made. Matching is now
    case-insensitive and bidirectional, like the ingest's.
  - **Two in-tree tests had encoded the old behaviour as intent** and were inverted deliberately, not
    deleted: the unit test asserting Python/PHP declare *no* `originMapping`, and the cli-smoke check
    asserting `--origin-info` *refuses* for a Python-only build. The refusal path itself survives for a
    third-party plugin that declares no sink, but now has no in-box trigger to smoke-test — worth knowing
    before someone reads its absence as dead code.
  - Drive-by, as planned: `scripts/verify-coverage-paths.ps1` now mirrors the ingest's `PathNormalizer`
    in both directions **and** requires a unique match, so an ambiguous basename — previously a silent
    pass here and a silent drop there — is now a named finding.
- **2026-09-18** — maintainer widened the format scope: **istanbul JSON and clover join cobertura and lcov**
  as readers *and* writers (slice 3b). This corrected an over-narrow reading of SP7 on my part — the
  ingest's three-format limit constrains what a consumer uploads, not what this tool can read. Istanbul is
  also the compatibility path: it is what the consuming repo's interim tool reads today, so supporting it
  lets that repo switch over without changing its vitest reporter config.
