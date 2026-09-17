# P38 — Origin attribution: `#line` directives (C#) and a v3 source map (TS) — PRD

> Issue: [#69](https://github.com/MintPlayer/MintPlayer.Polyglot/issues/69) · slug `docs/prd/issue-69-source-attribution/`
> Provenance: maintainer-filed issue with its own empirical probe, plus a 5-agent code-grounded
> investigation of this repo, then a maintainer design review (decisions **D1–D8**, 2026-09-17).
> Companions: `PLAN.md` (slices + decision log), `ANALYSIS.md` (the investigation evidence, and the
> corrections the review forced).
> Milestone number **P38 is provisional** — PLAN.md's last numbered milestone is P37 and the
> code-coverage-upload work landed unnumbered; renumber at merge if that one claims P38.

## 0. Prime directive check (POLYGLOT_PRD §3)

**This is not a language feature.** Nothing in §3.A gains or loses surface; no §3.B refusal is weakened;
no §3.C faithfulness corner moves. `.pg` programs are unchanged, and the *semantics* of the emitted code
are unchanged — `#line` is a compiler pragma that only relabels sequence points, and a v3 source map is a
sidecar file that no runtime reads. The feature is **emission metadata**, the same category as P37-D
Tier 1 attributes ("outside §3.C").

Three contract-adjacent obligations it does inherit:

- **§3.D determinism honesty.** Origin attribution must not over-promise. A `.pg` line maps to *one or
  more* output lines and many output lines collapse onto one `.pg` line; coverage counters therefore
  merge (verified, §2). We publish that collapse semantics rather than implying 1:1 fidelity.
- **Never a miscompile (§3.B's spirit).** A directive placed in a position the target compiler rejects,
  or spliced mid-expression, turns a readable emitter into a broken one. §4.E makes "never mid-line" a
  structural invariant, not a convention.
- **Core is language-agnostic (standing project directive).** `#line` is C# syntax and a v3 map is a TS
  convention. Neither may appear as a target-name comparison in the C++ Core; both are **plugin-manifest
  data**. §4.B is the whole design consequence of that rule.

## 1. Problem

The generated code is invisible to coverage tooling, and the invisibility is structural, not
configuration.

In `MintPlayer/MintPlayer.AI`, 9 `.pg` solvers (7,097 lines across the tracked set) transpile to C# that
lands in `obj/`, plus TS twins routed into an Angular app. Coverage tools exclude `obj/` by glob and, even
if they did not, would attribute hits to a machine-written `.cs` nobody edits. The consumer is driving to
90% coverage (its milestone M63, `docs/prd/COVERAGE_90_PRD.md`), and the `.pg` engine cores are among its
most heavily exercised code — so the most-tested code in that repo actively *depresses* its coverage
number by contributing to neither numerator nor denominator.

That consumer PRD wants **both** halves (`COVERAGE_90_PRD.md:108` — "TS hits, `.pg`-attributed via v3
source map") and already owns the downstream chaining risk as its own spike **S5** (`:225-231`), with two
documented fallbacks, one of which (`@vitest/coverage-istanbul` + explicit `inputSourceMap`) *requires the
map to exist*. See D1.

This is already recorded here as an unbuilt stretch goal: `POLYGLOT_PRD.md:234` ("+ source map where
applicable"), `:610` (gutter source-map lines, post-P17 stretch), `:1532` (Stretch bullet), and
`PLAN.md:2386` — which explicitly leaves **"decide the C# debug story"** open. This PRD decides it.

### 1.1 Why it is cheap here — and the two places the issue was optimistic

Both prerequisites the issue claims **hold**, verified against HEAD:

- **Positions survive to the emitter.** `SourcePos{line,col,fileId}` (`diagnostics.hpp:12-17` — *not*
  polyglot.hpp) is a base field on `ir::Expr` (`ir.hpp:29`) and `ir::Stmt` (`ir.hpp:279`). `lower.cpp`
  references a real position 72 times across 68 IR-node constructions, with **zero** default-constructed
  `SourcePos{}` anywhere in the file. Even the Python block-lambda hoist preserves it
  (`hoist_block_lambdas.cpp:76-84`).
- **One line-writing chokepoint.** `EmitterBase::line()` (`emitter_base.cpp:1598`) — 28 call sites (the
  issue said 27; `:1613` calls it twice), all inside that file.

One correction that changes the plan rather than the verdict:

- **`line()` is not the *only* writer of `out_`.** The prelude prepend at `emitter_base.cpp:1594`
  (`out_ = spec().preludes.at(k) + out_;`) bypasses it entirely. C# declares no preludes today, so this
  is inert for phase 1 — but it is a real hole in the "every line carries a directive" invariant and
  must be closed deliberately (§4.E-4), not assumed away.

> **A second "correction" in an earlier draft of this PRD was itself wrong** and has been removed. It
> claimed absolute `#line` paths would break `writeDedup`'s content-equality collapse. They do not:
> `writeDedup` compares content **within one build on one machine**, and a `.pg` file has one absolute
> path regardless of which root imports it. If anything it is *repo-relative* paths that can diverge
> across roots. This mattered — it was an argument used to pick a path form. See D2 and `ANALYSIS.md` §9.

## 2. What is already proven (issue #69's probe, kept as the evidence base)

Empirically established with `coverlet.collector` 10.0.1 on net10.0, against a throwaway library:

- Cobertura reports the **`.pg` path as `filename`**, with **`.pg` line numbers**.
- Branch coverage survives the remap (`condition-coverage` attributes present).
- One physical `.cs` splits into multiple `<class>` entries keyed by filename — so partial mapping is
  well-defined: mapped bodies attribute to the `.pg`, unmapped scaffolding stays on the `.cs`.
- **Exclusion globs match the PDB-recorded path, not the physical file** — which is precisely why code in
  `obj/` stops being excluded once it claims a `.pg` origin.
- Many-to-one collapse is benign: multiple sequence points on one `(document, line)` produce **one**
  element; hits **sum**; a line is covered if **any** contributor ran; merging across methods works.

And the constraint that dictates the whole design:

- **Emitting a directive only when the source line changes is wrong, not merely imprecise.** A `#line 600`
  followed by drifting braces reported lines 601–603 — *different, innocent `.pg` lines* — as covered.
  Wrapping the structural lines in `#line hidden` suppressed the phantoms entirely.

Two properties of `#line hidden` that follow from the same probe, and that the design depends on:

- **`hidden` removes a line from the report entirely** — it does not mark it uncovered. Hidden scaffolding
  therefore leaves the *denominator*, it does not depress the percentage.
- **A directive only produces a report entry where there is IL** to hang a sequence point on. Stamping a
  line that emits no code yields nothing either way. This is why D3 narrowed the declaration work.

> **Standing caveat.** That probe was hand-written C#, not this emitter's output. SP1 (§5) re-runs it
> against real `polyglot`-emitted code before any of it is trusted as an acceptance basis.

## 3. Goals / Non-goals

**Goals**

- G1. Opt-in, per-build C# `#line` emission such that a coverage run over generated C# reports hits
  against `.pg` paths and `.pg` line numbers.
- G2. **No phantom coverage**: a `.pg` line with no executable content never reports as covered.
- G3. Correct multi-file attribution — each `.pg` in an import closure attributes to its own path.
- G4. Flag **off** ⇒ emitted bytes byte-identical to today, on all four targets.
- G5. The per-target behaviour is **manifest data**; the Core gains no target-name comparison.
- G6. A line-granular Source Map v3 sidecar for TypeScript, from the same recorded origins — built
  unconditionally (D1).
- G7. **Failures are detected here, loudly**, not downstream in a coverage number that silently didn't
  move (D2's hardening; §4.I).

**Non-goals**

- N1. Column-level mapping; branch coverage attributed to `.pg` columns.
- N2. A debugger or editor stepping story. Stepping will land in `.pg`, which no editor renders with C#
  semantics. (The PRD:610 gutter-preview idea stays a separate, later thing.)
- N3. Changing the default output. Readable generated code remains the default, forever.
- N4. Consuming the TS map downstream (chained `.pg → .ts → .js` through a bundler) — consumer-repo work,
  tracked there as spike S5.
- N5. Attribution *inside* a flattened block lambda (§4.E-1) — a documented, bounded gap.
- N6. Python and PHP origin attribution. Manifest-driven, so they can opt in later by adding data; not
  built or tested here.

## 4. Design

### 4.A The invariant

> **Every emitted output line carries exactly one origin directive**: a positioned one when the line's
> origin is known, a `hidden` one when it is not (braces, declaration scaffolding, blank separators,
> prelude text). Because every directive states an **absolute line number** (never a relative offset),
> ordering and post-hoc prepending stop mattering — there is nothing left to shift.
>
> *(Terminology: "absolute" here is about line **numbering**. Whether the **path** inside a directive is
> absolute or relative is a separate question, settled in §4.F.)*

Corollary: a directive is **never** emitted anywhere but at the start of an output line. §4.E enumerates
every place the emitter can produce text that is not a fresh line, and what happens there.

### 4.B Manifest shape — `originMapping` (the agnosticism answer)

A single **top-level** manifest key, because its effect is on the host/compiler (does a sidecar file get
written?), not purely on emitted text — the same category as `crossDirImports`. Templates nest inside it
so there is one key, one vocabulary, one validation site.

```jsonc
// plugins/csharp/polyglot-plugin.json
"originMapping": {
  "style":  "directive",
  "line":   "#line $n \"$f\"",
  "hidden": "#line hidden",
  "column": 0                      // directives emit at column 0, not at indent_
}

// plugins/typescript/polyglot-plugin.json
"originMapping": {
  "style":            "sourceMapV3",
  "sidecarExtension": ".ts.map",
  "footer":           "//# sourceMappingURL=$f"
}
```

- `style` is a **closed, load-validated enum** (`directive` | `sourceMapV3`), following the `blockStyle`
  precedent (`backend_spec_json.cpp:50-57`) — the one place in the loader where an unknown *value* fails
  loudly instead of defaulting. That matters for version skew: a future `"style": "dwarf"` errors on an
  old CLI rather than silently emitting nothing.
- `$n` / `$f` reuse the existing `substX` family (`backend_spec.hpp:180-215`), generalized to two holes.
- **Absent key ⇒ today's behaviour, byte-for-byte.** Python and PHP ship unchanged; no manifest churn.
- Reading it follows `crossDirImports` end to end: `backend.cpp:325` read → `LoadedBackend` ctor/member →
  `backend.hpp` virtual → consumer → `docs/plugin-authoring.md:31-42` row → a flag-parity unit test in the
  `tests_main.cpp:2936` style.

**Deliberately NOT done:**

- **No new `kCoverage` entry** (`backend.cpp:173-187`). A new required rule name breaks the load of all
  four plugins simultaneously, and Python/PHP have nothing to say.
- **Not expressed as `rules`.** Rules are the arm-trace denominator (`backend.cpp:237-247`); an
  off-by-default rule would read as permanently uncovered — exactly the PHP `UnionDecl` friction recorded
  at `docs/prd/code-coverage-upload/PRD.md:508-520`. Spec/top-level data is not an arm, so it adds nothing
  to the denominator. (The behaviour still gets a live witness: §7's A10 requires gate legs with the flag
  **on**, for both sinks.)
- **Not a `capability`.** The capability vocabulary is closed and load-enforced (`backend.cpp:270-274`), so
  a manifest declaring a capability an older CLI doesn't know **fails to load**. An ignored-if-unknown
  top-level key is the right skew posture for an optional emission feature.

### 4.B.1 Unsupported targets — refuse only when *nothing* can honour it (D6)

The flag is enabled and a selected target has no `originMapping`. The `crossDirImports` precedent refuses
rather than half-applies (`tests_main.cpp:2936-2973`), and this repo's culture is loud failure everywhere
(closed capability vocabulary, the coverage tripwire, the registry gate). But that precedent refuses
because half-applying produces *wrong output*; here the unannotated target's output is correct, merely
unannotated — and refusing would break the normal mixed-target build that motivates the feature.

**Rule:** if **no** target in the resolved set declares `originMapping`, **refuse the build**. If at least
one does, honour it there and leave the others unannotated.

The diagnostic must be actionable by a downstream consumer who has never read this PRD — it names the flag,
the resolved target set, which targets *can* honour it, and where the setting came from (CLI flag vs
`pgconfig.json` key). `runBuild` groups inputs by nearest `pgconfig.json` (`main.cpp:626-634`), so the
support question is **per group**; the diagnostic must name the group, or a two-config project looks flaky.

### 4.C Blocking prerequisite — a real `SourceMap` on the build path

`#line` needs filenames. Today it cannot have them:

| | `compile()` (`compiler.cpp:629`) | `analyze()` (`compiler.cpp:766`) |
|---|---|---|
| entry lex | `lex(source, diags)` — no fileId ⇒ **0** (`:639`) | `lex(source, diags, entryFileId)` (`:771-772`) |
| front end | `runFrontEnd(…, **nullptr**, &importGraph)` (`:646`) | `runFrontEnd(…, **&result.sources**)` (`:790`) |
| result | `src == nullptr` ⇒ `:353` yields 0 ⇒ **every token of every module is fileId 0** | real ids |

The machinery exists (`SourceMap`, `polyglot.hpp:143-155`, id 0 reserved for "unknown"); only the build
path declines to use it. The fix is small and additive: `compile()` gains a defaulted `entryPath`, adds a
`SourceMap` to `EmitResult`, stamps the entry, and passes `&result.sources` at `:646`. Nothing below
changes — `runFrontEnd`/`linkCoreModule`/`linkModules`/`loadImports` already take `SourceMap*`.

**`SourceMap::add()` canonicalizes to an absolute, forward-slashed path** (D2 hardening 1). It stores the
resolver's string *verbatim* today, which may be relative — and the LSP discriminates on
`fs::path(canon).is_absolute()` (`main.cpp:1085-1094`), so a resolver returning a relative canonical path
silently kills cross-file go-to-definition. Canonicalizing fixes that latent bug in passing. Logical std
names (`std.io`) are not paths and are left alone.

**Ratified side effect, in scope (D4).** `reportDiagnostics` (`main.cpp:149-154`) and `emitDiagAt`
(`:305-308`) hardcode the entry path for *every* diagnostic, so an error inside an imported module is
reported today at the entry file's name with the imported module's line number — a real, latent
mislabeling that exists precisely because fileIds are all 0 on this path. Fixing it is the same root
cause; per the single-PR rule it lands here rather than as a follow-up. `check --json`
(`main.cpp:673-684`) gains a file field.

**Boundary:** diagnostics print the **resolver's canonical path**, not the directive's rebased form. The
two path forms are independent — otherwise the VS Code problem matcher's expected `file:line:col` shape
moves for reasons unrelated to this feature.

**Watch out:** the LSP filters on `d.pos.fileId != 1` (`main.cpp:1054`) to publish only the open file's
diagnostics. Entry-is-1 must stay an invariant (`compiler.cpp:771` already establishes it); a slice that
breaks it silently blanks LSP diagnostics.

### 4.D Positions on the declarations that actually execute (D3)

`ir::Function`, `Method`, `Record`, `RecordField`, `Class`, `ClassField`, `Enum`, `EnumCase`, `Union`,
`UnionCase`, `Interface`, `Global`, `ExternType`, `Param`, `GenericParam`, `ModuleImport`, `Module` carry
**no** `SourcePos` (`ir.hpp:398-625`).

An earlier draft proposed adding one to all 17, justified as protecting the denominator. That
justification was wrong (§2): `hidden` *removes* a line from the report rather than marking it uncovered,
and a bare signature line emits no IL, so stamping it produces no report entry either way. Declaration
positions buy **stepping**, which is N2.

**Empirically confirmed (SP3 + SP1).** A probe compiled 15 directive positions and inspected the PDB: the
documents for a `using` line and two type-declaration lines were **absent**, while field initializers,
expression-bodied members, method bodies, lambdas and iterators were all **present**. Then SP1's coverage
run showed `.pg` line 4 — the `fn main() {` line — reporting `hits="1"`, i.e. **method entry does become a
numerator contribution** when the body's opening brace carries the method's position.

**Consequent rule (refines §4.E-3):** a method body's **opening brace carries the method's position**;
every *other* brace is `hidden`. Without this, method entry is discarded and the `.pg` declaration line
never registers.

**Scope: three structs — `ClassField`, `Global`, `Method`** — the declarations whose emitted code can
execute:

- `ClassField` / `Global` — an initializer is IL (in the constructor / static constructor). Its `.pg`
  line is genuinely executed and belongs in the numerator.
- `Method` — Roslyn puts a sequence point on the body's opening brace. With a position, the `.pg`
  `fn`/method line records a hit on **method entry**; without one, that sequence point is discarded. This
  is a real numerator contribution, and it is why the narrowed scope kept `Method` rather than dropping
  all three.

Populate from the AST's `pos`/`namePos` at the sites tabulated in `ANALYSIS.md` §2.1. `ir::dump()`
(`src/ir.cpp`) must **not** print the new field, or every golden IR dump changes. `ast::ExtensionDecl`
(`ast.hpp:300-312`) has `pos` but **no `namePos`** — recorded, not fixed.

### 4.E The five places the invariant can break

> **SP3 verdict (2026-09-17): every position is legal.** A 15-case probe — directive before a `using`,
> before a type, between an attribute and its target, before an expression-bodied member, inside a
> `switch` section, before a closing brace, on blank separators, around field initializers, local
> functions, `try`/`catch`, iterators, interpolated strings, and **between the lines of a single wrapped
> expression** — compiled with **0 errors, 0 warnings**. Directives are processed lexically, so even a
> mid-expression line break tolerates one.
>
> So the `hidden` list is driven **entirely by phantom-avoidance, not by Roslyn's parser**. The only hard
> constraint below is #1: `inlineBlock` has no newline to host a directive at all, so a directive there
> would be spliced *into* a line rather than between two.

1. **`inlineBlock` flattens `\n` → space** (`emitter_base.cpp:1643-1655`) into a scratch buffer, and C#
   **always** uses it for block-bodied lambdas (`plugins/csharp/…:1111`; Python avoids it via
   `expressionOnlyLambdas`). A directive emitted inside would be spliced mid-expression and **break
   compilation**. Suppression there is mandatory, not cosmetic (`suppressDirectives_`), and attribution
   inside a block lambda is coarsened to the statement containing it — N5.
2. **`line()` receives strings with embedded `\n`.** Confirmed live path: `module.attrImportsBlock`
   (`emitter_base.cpp:1155-1161`) newline-joins N `using` lines into **one** `line()` call, fed straight
   into a `{"line": …}` in all four `Program` rules. Possible paths: `attrLines` / `ir::Extern` code /
   FFI `Bound` templates, where a user `actual("…")` string may contain a decoded `\n`. Such strings must
   be **split on `\n` and emitted line-by-line**, each carrying its own directive, or emitted wholesale
   under `hidden`. Splitting is preferred; `hidden` is the safe fallback.
3. **Blank separator lines** — C# emits `{"line": ""}` at three places (`plugins/csharp/…:2611, 2733,
   2829`). Each gets a `hidden`, which is semantically free but doubles those lines. Accepted; the flag is
   off by default and the output is machine-consumed when on.
4. **Prelude prepend** (`emitter_base.cpp:1594`) never passes `line()`. It must be wrapped in `hidden`
   directives by the prepender itself (Python-only today, but the hole is closed generically).
5. **`indent_` is manipulated outside `line()`** (`:1384-1386`, `:1413-1415`, `:1605/1608`, `:1760/1766`,
   `:1788/1791`, `:1647`), and `line()` unconditionally prefixes `indent_*4` spaces. C# `#line` is
   conventionally at column 0, so the directive writer must bypass the indent prefix — hence the manifest
   `column` field rather than a hardcoded choice.

### 4.F Path form — per sink, absolute for the directive (D2)

There is no single correct path form, because the two sinks are resolved by different consumers.

**C# directive: absolute, canonicalized, forward-slashed.**

Every hop of the absolute form has evidence (§2): directive → PDB → coverlet's `<sources>` split → `.pg`
filename, lines and branch coverage. The relative form has none, and puts **two unverified resolvers** in
the chain — Roslyn resolving a relative `#line` against the generated `.cs` location (`obj/polyglot/`),
and coverlet handling a relative document name. Absolute is also stable within a build: one `.pg` file has
one absolute path however many roots import it, so `writeDedup`'s content-equality collapse is untouched
(and it is *relative* paths that could diverge across roots — see the §1.1 note).

The single argument that pointed the other way — this repo's coverage path tripwire — rests on an
instrument that disagrees with its own documentation: `verify-coverage-paths.ps1:39-48` describes suffix
matching "or vice versa" but implements only `tracked.EndsWith("/$report")`, rejecting a report path
*longer* than the tracked one. Whether the **server** accepts that longer form (as "longest-suffix match"
suggests) is **an inference, not a verified fact**, so §4.I deliberately does *not* loosen the script on
it — it surfaces the direction instead, and SP1 or the consumer's spike S6 settles it. Note the absolute
form's viability does not actually hinge on this: coverlet emits a `<sources>` root plus a remainder, and
§2's probe showed that remainder landing in `.pg` shape. SP1 confirms it in the consumer's real layout.

Forward slashes on all platforms: uniform, and avoids any question about backslash handling inside a
directive's quoted filename.

**TS map: `sources` map-relative, plus embedded `sourcesContent`.**

A v3 map's `sources` resolve relative to the map file, and the consumer's `include` rules route the `.ts`
into an entirely different tree from the `.pg` — e.g. `…/Chess/polyglot/chess_solver.pg` emits to
`src/RLDemo.Web/ClientApp/src/app/chess/`. A root-relative path dropped into that map resolves nowhere.

So `sources` are computed **relative to the map's own location**, and the map **embeds `sourcesContent`**
with the original `.pg` text. `sourcesContent` is the robust half: with the source inline, the map needs
no path resolution at all, so it survives bundling, relocation and the routing above. `sources` remain as
a fallback for tools that prefer on-disk lookup. Cost is a few thousand lines of text in a build artifact.

**If SP1 disproves the absolute directive form**, rebase paths **in the report**, not in the emitted
directive. (`ANALYSIS.md` §5 notes other MintPlayer repos already rebase lcov paths before uploading.)

**SP1 verdict (2026-09-17): absolute confirmed on real emitter output.** A coverlet run over
directive-annotated output of this emitter reported `filename="pgprobe\sp1probe.pg"` with `.pg` line
numbers, branch coverage intact, zero phantoms, and a dead branch at `hits="0"`. Two operational notes:
coverlet **normalizes the separator to the platform's** (`\` on Windows) regardless of what the directive
wrote, and it **does not check that the path exists** — a directive naming a non-existent file is reported
verbatim, which is precisely why A12's liveness assertion is ours to make.

**The `<sources>` remainder is not ours to control.** Coverlet splits every document into a `<sources>`
root plus a remainder, where the root is the **common prefix across all documents in the report** — so the
remainder's shape depends on which other files the report happens to contain, and is unpredictable from
here. This is the residual unknown behind the tripwire question (§4.I-2): do **not** design against a
predicted remainder. Emit a correct absolute path and let the consumer's report-side configuration or the
server's matching resolve it.

### 4.F.1 The dedup regression SP2 found (real, and not what §4.F originally predicted)

`writeDedup` (`main.cpp:168-194`) keys on the output path and collapses when content is **identical**;
anything else is a hard error. Measured: building `rootA/p.pg` and `rootB/p.pg` — two *distinct* files
that happen to emit byte-identical C# — silently produces one `out/p.cs` today.

With directives on, those two files carry different origins, so the content differs and the same build
**fails**: `polyglot: conflicting output for '…p.cs' (two modules emit the same file with different
content)`.

This is arguably a latent bug being exposed rather than created — today one of those two sources is
silently unrepresented in the output — but it is a behaviour change for a scenario the dedup logic exists
to support, and it lands whichever path form is chosen (the files genuinely differ). The response is **not**
to strip directives from the dedup key: that would collapse two different origins into one and silently lie
about which source the output came from, which is the one thing this feature must never do.

**Decision:** let it fail, and make the failure explain itself — the diagnostic names the flag as the
cause and points at pgconfig `include` output rules, which already exist to route colliding basenames
apart. Documented as a known consequence of enabling the flag.

### 4.G Option plumbing — copy `--access`, not `--emit-arm-trace`

`--emit-arm-trace` is a process-global singleton parsed before dispatch. This feature is **per-compile and
emit-affecting**, so the structural precedent is `--access`:

CLI flag (`main.cpp:584-585`, and note `runBuild:588-590` hard-refuses unknown options) → merged with
config, flag wins (`:491-495`) → `LibConfig` field (`polyglot.hpp:113-117`) → `ir::Module` (`ir.hpp:624`,
set `compiler.cpp:677/716`) → `EmitterHooks` (`emitter_base.cpp:1569`, struct `emitter_base.hpp:136-140`).

- CLI: `--line-directives` on `build`; usage text `main.cpp:54-79`. The provenance (flag vs config) is
  retained for §4.B.1's diagnostic.
- pgconfig: one field on `PgConfig` (`pgconfig.hpp:79`, next to `access`) + one read (`:101`). No schema
  file exists; docs are `README.md:127-141` + the annotated `editors/vscode/testbench/pgconfig.json`.
- MSBuild: `<PolyglotLineDirectives>` defaulted in the `.props`, appended as `_PolyglotLineDirectivesArg`
  inside `PolyglotTranspile`'s `<PropertyGroup>`, exactly like `_PolyglotAccessArg`. Note the
  `_PolyglotAddGenerated` glob is `*.cs`, so a `.map` sidecar is not swept into `@(Compile)`/`@(FileWrites)`.
- LSP: `analyze()`/`compile()` already take `LibConfig`, so the preview can opt in later; diagnostics do
  not care. Out of scope to wire.

### 4.H The TS v3 source map

Same recorded origins, different sink. Built **unconditionally** (D1): the consumer wants both halves, and
its fallback path (`@vitest/coverage-istanbul` + explicit `inputSourceMap`) requires the map to exist, so
gating emission on the consumer's own spike would deadlock the two repos against each other. The map's
correctness is a local property — does it decode to the right `.pg` lines? — and that is what we test.

This is where the data model widens, because **nothing today emits a sibling file with a different
extension**:

- `EmitResult` (`polyglot.hpp:72-77`) and `ModuleFile{basename, code, sourcePath}` (`:61-65`) carry no
  sidecar slot; `Backend::emit` (`backend.hpp:71`) returns a bare `std::string`.
- `resolveClosureOutputs` (`pgconfig.hpp:190-238`) appends a single `ext` per file; `emitOne`
  (`main.cpp:221-243`), `writeDedup`, and the **duplicated watch writer** (`:384-403`) all assume one
  artifact per module.
- Base64-VLQ is ~30 lines; the repo has its own JSON writer (`json.hpp`/`json.cpp`) ⇒ **no third-party
  dependency**.
- TS's `Program` rule is a `seq` with no footer slot, so `//# sourceMappingURL=` needs either a new tail
  element or a Core-side append driven by `originMapping.footer`.
- **Line granularity only.** Columns would mean threading positions through the expression rule
  interpreter, which returns bare `std::string` — a far larger job, and line granularity is what coverage
  needs (N1).

### 4.I Failing loudly, here (D2 hardening)

The feature's failure mode is a coverage number that silently doesn't move, in someone else's repo. Three
measures pull detection back into this gate:

1. **Directive liveness assertion** — every path emitted in a directive must exist on disk at emit time.
   Cheap, local, and it catches canonicalization and rebasing bugs immediately.
2. **Make `verify-coverage-paths.ps1` honest about direction — but do not loosen it on an inference.**
   The script documents suffix matching "or vice versa" and implements only one direction
   (`tracked.EndsWith("/$report")`), so a report path *longer* than the tracked path is rejected. Whether
   the **server** accepts that longer form is an inference from the phrase "longest-suffix match", **not a
   verified fact** — and the script's whole purpose is to convert the server's silent drops into loud
   failures, so loosening it while the inference is unconfirmed would defeat it precisely where it
   matters. Therefore: **report the match direction, do not silently accept the unverified one.** A
   longer-path match is surfaced as a distinct warning naming both paths, and becomes an accepted match
   only once SP1 (or the consumer's spike S6, `COVERAGE_90_PRD.md:236`) confirms the server resolves it.
   Fix the docstring either way, so the script and its documentation stop disagreeing.
3. **Flag-on gate legs for both sinks** (§7 A10) — the feature is deliberately not an arm-trace arm, so
   without these it ships with no standing witness and the next refactor of `line()` breaks it silently.

## 5. Spikes

Each is throwaway and time-boxed. **Record the outcome in PLAN.md's Log; a spike that fails changes the
design here — it does not get worked around.**

- **SP1 (~60 min) — re-prove the probe against *this* emitter, and settle the path form.** Take 2–3 real
  conformance programs, emit C# with the flag on (a hand-hacked build is fine), run coverlet, inspect the
  Cobertura. Confirm: `.pg` paths and lines appear; phantoms are absent; the absolute form's `<sources>`
  remainder resolves against a `git ls-files` shape. **Gate:** if `.pg` attribution fails outright, stop
  and ask the maintainer — the entire motivation rests on this. If only the *path form* fails, fall back
  per §4.F (rebase the report, not the directive).
- **SP2 (~20 min) — dedup and the shared prelude.** Build a multi-root program with the shared prelude and
  confirm `writeDedup` still collapses. Expected to be a non-event under absolute paths; it is here
  because an earlier draft asserted the opposite and was wrong.
- **SP3 (~30 min) — directive legality in every position.** Roslyn's parser is the judge: `#line` before a
  member, between an attribute and its target, inside a `switch` section, before a closing brace, inside
  an expression-bodied member, before a flattened block-lambda line, and around a class with field
  initializers (the D3 case). **Gate:** any position that fails to parse must be classified `hidden` by
  construction, not by hope. Output: the authoritative `hidden` list slice 5 implements.
- **SP4 (~30 min, *not* a gate) — chained-remap reality check.** Decode a generated `.ts.map` and confirm
  it resolves to the right `.pg` lines, with and without `sourcesContent`. This informs what the docs
  promise about downstream chaining; it does **not** gate emission (D1). The consumer's own S5 owns the
  bundler half.
- **SP5 (~20 min) — cost when off.** Confirm the disabled path is unmeasurable in emit wall-clock, in the
  spirit of the arm-tracer's SP4 budget (`docs/prd/code-coverage-upload/`).

## 6. Decisions and their consequences

**6.1 A directive on *every* line, never "on change".** Consequence: output roughly doubles in line count
when on; there are no relative offsets, so the post-walk prelude prepend stops mattering. Accepted because
"on change" is not imprecise but *wrong* — it silently inflates coverage with phantoms (§2).

**6.2 Per-target behaviour is manifest data with a closed `style` enum.** Consequence: adding a target's
origin story is a plugin edit, not a Core edit; an unknown style errors loudly; an absent key is a silent,
correct no-op. Cost: an old CLI reading a new manifest silently drops an unknown top-level key — the known,
accepted skew posture of this loader.

**6.3 Off by default, and there is no golden-refresh switch.** Consequence: G4 (byte-identical when off) is
not a nicety — no `--update` mechanism exists anywhere in `scripts/` or `tests/`, so any churn is
hand-edited. This alone justifies the default.

**6.4 (D4) `compile()` gets a real `SourceMap`, canonicalized, and diagnostics get fixed with it.**
Consequence: one PR carries both; imported-module diagnostics stop being mislabeled with the entry file's
name; the LSP's relative-canon navigation gap closes in passing. The entry-is-fileId-1 invariant becomes
load-bearing. Risk: `tests/refusals`' 17 fixtures pin `<path>:<line>:<col>` text.

**6.5 (D3) Only the three executable declarations gain `pos`.** Consequence: 3 struct changes instead of
17, no `ir::dump()` hazard, and `Method` entry becomes a genuine numerator contribution. Signature lines of
non-executable declarations stay `hidden` — which removes them from the report, not from the numerator.

**6.6 Synthesized files (empty `sourcePath`) are `hidden`-only.** Consequence: prelude content has no `.pg`
origin to claim, so it stays uniform and `writeDedup` is unaffected.

**6.7 Block-lambda interiors are not attributed.** Consequence: a documented gap (N5), irreducible without
changing how C# lambdas are emitted. The containing statement still attributes correctly.

**6.8 (D1) Phase 2 is unconditional, not gated.** Consequence: `EmitResult`/`ModuleFile`/`Backend::emit`
and the write path widen in this PR. Justified because the consumer's fallback requires the artifact, and
gating our emission on their spike deadlocks both repos. SP4 becomes post-build validation that informs the
docs, not a gate.

**6.9 (D2) Absolute for the directive, map-relative + `sourcesContent` for the map, and fail loudly here.**
Consequence: two path forms, each matched to its resolver and each with its own test; a tripwire fix; and a
liveness assertion. The design explicitly prefers evidence over symmetry.

**6.10 (D6) Refuse only when no selected target can honour the flag.** Consequence: mixed C#+TS+Python
builds work; a Python-only build with the flag set is a loud, actionable error naming the group, the
resolved targets, and where the setting came from.

**6.11 (D7/D8) The always-on status block stops carrying version numbers.** Consequence: `CLAUDE.md` states
the lockstep rule and points at the source of truth (tags / the release workflows) instead of duplicating a
fact it doesn't own — which is how it drifted six minor versions out of date.

## 7. Acceptance criteria

- **A1.** Flag **off** ⇒ emitted bytes unchanged on all four targets; the full gate is green, including
  the ~517 substring assertions and ~22 raw-string expectations in `tests_main.cpp`.
- **A2.** Flag **on** ⇒ every emitted C# line carries either `#line <n> "<absolute, forward-slashed .pg
  path>"` or `#line hidden` — asserted by *placement* (a parse of the emitted file), never by
  byte-identity against a golden.
- **A3.** The emitted C# still compiles (the conformance oracle `csc` run is the instrument) and every
  conformance program's stdout still matches, with the flag on.
- **A4.** A coverage run over generated C# reports hits against the `.pg` path with `.pg` line numbers.
- **A5.** **No phantoms**: a `.pg` line with no executable content never reports as covered — asserted on a
  fixture with deliberate structural drift (a multi-line `if` whose braces would otherwise drift).
- **A6.** A multi-file `.pg` closure attributes each construct to its **own** path — i.e. §4.C is genuinely
  fixed, not masked by single-file testing. (Test with `programs/modular/geom/`.)
- **A7.** `originMapping` absent ⇒ no behaviour change; unknown `style` ⇒ **load error**, not a silent
  default. Python and PHP manifests are untouched.
- **A8.** *(rewritten after SP2 — the original criterion asserted the opposite and was wrong.)* With the
  flag **on**, two distinct `.pg` files that currently emit byte-identical C# to the same output path stop
  collapsing, because origin info correctly distinguishes them. The build **fails** at
  `writeDedup` (`main.cpp:174-177`) with a message that names the flag as the cause and points at
  `include` output rules as the fix — it must not silently pick one origin. With the flag **off**, the
  collapse behaves exactly as today. The shared prelude, being `hidden`-only, keeps collapsing either way.
- **A9.** `--line-directives`, the pgconfig key, and `<PolyglotLineDirectives>` all reach the emitter, and
  the flag wins over config (the `--access` precedent).
- **A10.** **Standing flag-on witnesses for both sinks.** A curated conformance subset runs flag-on —
  curated from the emitter's *irregular* paths (`inlineBlock`'d block lambdas, `TryStmt`/`ForStmt` rule
  arms, iterators, the multi-line `attrImportsBlock`, a class with field initializers), not from what looks
  representative. The same subset validates the emitted `.ts.map`, and `tests/library/run-library.ps1`
  (tsc strict) confirms the footer doesn't break compilation.
- **A11.** The `.ts.map` is valid v3 JSON, decodes to correct `.pg` line attributions, and carries
  `sourcesContent`.
- **A12.** Every path emitted in a directive exists on disk (the §4.I liveness assertion).
- **A13.** Flag enabled with **no** supporting target ⇒ refusal naming the flag, the group, the resolved
  targets, and the setting's provenance. Flag enabled with **some** supporting target ⇒ builds, annotating
  only those.

### 7.1 The residue, characterized

Known-unattributed after this lands, by construction: block-lambda interiors (§4.E-1); extension-function
name positions (§4.D); non-executable declaration lines; anything in a synthesized prelude (§4.F);
column-level anything (N1); Python and PHP entirely (N6). Each is a `hidden` directive — i.e. **absent from
the report**, never phantom-covered. The failure mode is always "attributes less than ideal", never
"attributes wrongly".

## 8. Out of scope / follow-ups

- Python / PHP `originMapping` data (the mechanism will accept it; nobody has asked).
- Column-level mapping and `.pg`-column branch coverage.
- Downstream consumption of the TS map in `MintPlayer.AI` (consumer-repo work, its spike S5).
- Editor/debugger stepping into `.pg`; the PRD:610 gutter-preview idea.
- `ast::ExtensionDecl.namePos` (a small parser change; recorded in §4.D).
- LSP preview honouring the flag.
- **Drive-by candidate, not committed:** `compiler.cpp:673`'s `target.name() == "csharp"` is the single
  remaining target-name comparison in Core (the issue-#14 shared-prelude hoist). This PRD's §4.B builds
  exactly the kind of trait flag that would retire it (`sharesPreludeFile`). Do it only if slice 6 makes it
  free; otherwise record it.

## 9. Open questions

- **Q1.** Does the directive volume (roughly doubling the line count) measurably slow `csc` or coverlet on
  the consumer's real output? Measure in SP1; if it hurts, the fallback is directives on statement-bearing
  lines plus collapsed `hidden` runs — which SP1 must then re-prove phantom-free.
- **Q2.** Milestone number (P38 vs renumber) — see the header note.
- **Q3.** The `CLAUDE.md` status-block audit (D7) covers the "In flight / gated" list — P23's marketplace
  publish, the P22 tail, P16d, P20. Anything resting on an interactive step performed outside the repo
  needs the maintainer's answer; the rest is read from the repo and tags.
