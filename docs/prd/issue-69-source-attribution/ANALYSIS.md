# P38 / issue #69 — code-grounded investigation

Five parallel read-only investigations against HEAD `d5dd2b4`: the emitter's line-writing machinery ·
`SourcePos`/`fileId` plumbing · CLI/pgconfig/MSBuild option plumbing · plugin-manifest agnosticism ·
docs conventions + byte-sensitive test surfaces. This file is the evidence; `PRD.md` is the design it
produced and `PLAN.md` the slices.

## 0. Verdict on issue #69's claims

| Claim | Verdict |
|---|---|
| `SourcePos{line,col,fileId}` on `ir::Expr` and `ir::Stmt` | **TRUE** — `ir.hpp:29`, `ir.hpp:279`. Definition lives in `diagnostics.hpp:12-17`, not polyglot.hpp |
| `lower.cpp` propagates it, zero `SourcePos{}` | **TRUE** — 72 references (55 `e.pos` + 17 `s.pos`) over 68 IR constructions; zero defaulted |
| `EmitterBase::line()` at `emitter_base.cpp:1598` is the chokepoint | **MOSTLY** — 28 call sites (not 27; `:1613` calls twice), all in that file. **One bypass exists:** `:1594` |
| `blockBody:1604` / `openBlock:1611` / `closeBlock:1619` / `headBlock:1623` route through `line()` | **TRUE**, all four exact |
| `inlineBlock:1643` flattens `\n`→space | **TRUE** |
| Preludes prepended post-walk at `:1594` | **TRUE** |
| `line()` sometimes gets embedded `\n` | **TRUE** — at least one live path (§3) |
| `emitStmt:1683` has `s.pos` for every statement | **TRUE** |
| IR declarations carry no position | **TRUE** — all 17 decl structs |
| `compile()` stamps every token `fileId = 0` | **TRUE**, but via `src ? src->add(canon) : 0` (`compiler.cpp:353`) given the `nullptr` at `:646` — not a hardcoded 0 |

**Two findings the issue did not have**, both design-changing: §1.1 (the `out_` bypass) and §5 (the
`writeDedup` collision).

## 1. The emitter

```cpp
// emitter_base.cpp:1598
void EmitterBase::line(const std::string& s) {
    out_.append(static_cast<std::size_t>(indent_) * 4, ' ');
    out_ += s; out_ += '\n';
}
```
`out_` at `emitter_base.hpp:350`, `indent_` at `:351`.

### 1.1 Everything that touches `out_`

`:1564` clear · **`:1594` `out_ = spec().preludes.at(k) + out_;`** · `:1599-1601` inside `line()` ·
`:1644-1650` `inlineBlock`'s save/clear/restore · `:1595` return. **No other append exists.** So: one
chokepoint **plus one post-walk prepend** that never sees `line()` — unindented, verbatim, sorted so the
last key ends outermost. C# declares no preludes; only `plugins/python/…:42-46` has any. Inert for phase 1,
but a real hole in the invariant.

(`src/ir.cpp:87-93` and `src/pg_printer.cpp:61-67` have their own identically-named `line()` — different
classes, different buffers, not the emit path.)

### 1.2 The 28 call sites, by category

- **Decl/rule text** (`runDeclRule`, `:1370-1451`): `:1380` `K::Line` (every `{"line": …}` — decl headers,
  `using` lines, `#nullable enable`, and blank separators `"line": ""` at `plugins/csharp/…:2611, 2733,
  2829`) · `:1399` `K::MapDecl` attrLines · `:1431` `K::MapMembers` attrLines · `:1448` default.
- **Block scaffolding:** `:1606` `pass` · `:1613` `line(head)` **and** `line("{")` (Allman) · `:1614` K&R
  `head + " {"` · `:1615` `head + ":"` · `:1620` `}`.
- **Statement content** (`emitStmt`): `:1687, 1691, 1734, 1741, 1746, 1747, 1752, 1765, 1772, 1775, 1776,
  1786, 1789, 1796, 1809, 1811`.
- Helpers: `openBlock` ×9, `closeBlock` ×9, `blockBody` ×7, `headBlock` ×1 (`:1800`, While).
- `K::Block` (`:1382-1389`) and `K::Indent` (`:1412-1416`) move `indent_` directly around recursive
  `runDeclRule`; `K::Stmts` (`:1404-1408`) calls `emitStmt` per statement.

### 1.3 `inlineBlock` (`:1643-1655`)

Saves `out_`/`indent_`, clears both (`indent_ = 0`), runs `emitStmt` into the scratch buffer, restores,
then flattens **every** `\n` to a space (`:1653`). Exposed as `{"fn":"inlineBlock"}` (`:468-469`, catalog
`backend.cpp:146`), guarded to `ExprKind::Lambda`.

Used by exactly one construct — the **block-bodied lambda** — in `plugins/csharp/…:1111`,
`typescript/…:1561`, `php/…:712`. Python sets `expressionOnlyLambdas: true`
(`plugins/python/…:63`) so `lower.cpp:1257` hoists them instead. **C# always uses it.** An arbitrarily
large statement body can therefore land on one physical output line — the hard floor on granularity, and
the reason `suppressDirectives_` is mandatory rather than cosmetic.

### 1.4 `emitStmt` (`:1683`)

`switch (s.kind)`. Hand-written: Assign, ExprStmt, Let (incl. `localDeclTyped` `:1707-1733`), Yield,
Throw, Use, Return, Break, Continue, While (incl. Python do-while emulation `:1779-1799`), If. Rule-driven
via `runDeclRule`: `For→"ForStmt"`, `Try→"TryStmt"`, `IndexAssign`, `LocalFunc` (`:1815-1836`), plus tuple
`Let→"TupleLet"` (`:1696-1706`). Fallback `emitStmtTarget` is a no-op in `InterpretedEmitter`
(`emitter_base.hpp:399`). All paths build their `StmtCtx` from the same `s`, so `s.pos` is universally
available. C# defines `IndexAssign` (`:382`), `TupleLet` (`:402`), `TryStmt` (`:2249`), `ForStmt`
(`:2377`); it does not define `LocalFunc` (only Python does) — consistent, since C# doesn't hoist.

### 1.5 Other invariant hazards

`indent_` is moved outside `line()` at `:1384-1386, 1413-1415, 1605/1608, 1760/1766, 1788/1791, 1647`, and
`line()` unconditionally prefixes `indent_*4` spaces — so a column-0 directive must bypass that prefix.
There is **no** post-processing, trimming, dedup or formatting of `out_`: `emit()` returns the buffer
verbatim (`:1595`), and `compiler.cpp:678/720/730` assigns it straight to `result.code`.

## 2. Positions

**Definition:** `diagnostics.hpp:12-17` `struct SourcePos { int line = 1; int col = 1; int fileId = 0; };`.
Minted only by the lexer (`lexer.cpp:45`, `fileId_` from `lex(source, diags, fileId)`, `lexer.hpp:16`,
**default 0**). The parser only copies token positions; nothing is ever synthesized.

**Consumers of IR positions today: essentially none** — the sole read is `emitter_base.cpp:1764`
(re-wrapping an `ir::Var`). **No plugin references `pos`/`SourcePos`/`fileId` at all**, and `ir::dump()`
doesn't print positions — so adding `pos` to IR decls cannot break plugins or golden IR dumps.

### 2.1 IR decls lacking `pos`, and their positioned AST sources

| IR decl | AST source | available | lowering site |
|---|---|---|---|
| `Enum`/`EnumCase` | `EnumDecl` `ast.hpp:330-336` / `:324-329` | `pos`, `namePos` / `pos` | `lower.cpp:260-268` |
| `Union`/`UnionCase` | `:343-351` / `:338-342` | `pos`, `namePos` / `pos` | `:269-284` |
| `Record`/`RecordField` | `RecordDecl` `:264-277`; fields `ast::Param` `:78-86` | `pos`, `namePos` / `pos` | `:285-298` |
| `Class`/`ClassField` | `ClassDecl` `:278-290`; `ast::Member` `:236-251` | `pos`, `namePos`, `bodyEnd` | `:299`, `:550-617` |
| `Interface` | `:291-299` | `pos`, `namePos` | `:300-308` |
| `Global` | `ValueDecl` `:313-322` | `pos`, `namePos` | `:319-328` |
| `Function` (extensions) | `ExtensionDecl` `:300-312` | `pos` only — **no `namePos`** | `:329-345` |
| `Function` | `FunctionDecl` `:187-206` | `pos`, `namePos`, `bodyEnd` | `:346+` |
| `Method` | `ast::Member` `:236-251` | `pos`, `namePos`, `bodyEnd` | `:618+` |
| `Param` | `ast::Param` `:78-86` | `pos` | `irParam()` |

Note IR decls already carry a *coarse* file identity: `originModule` (a canonical path; `""` = entry,
`"<prelude>"` = std/lib) on Function/Record/Class/Enum/Union/Interface/Global (`ir.hpp:474, 526, 535, 549,
577, 584, 591) — a parallel string mechanism used by multi-file emit partitioning (`compiler.cpp:657-670,
704-731`).

`SemanticModel` (`semantic_model.hpp:23-46`) already receives decl-level `namePos` (`sema.cpp:450-470`) —
so AST→SemanticModel carries decl positions; only the AST→**IR** hop is missing.

### 2.2 `SourceMap` and the `compile()`/`analyze()` delta

`polyglot.hpp:143-155`: `files` (index 0 reserved "unknown"), `add(canon)`, `canon(id)`. Owned only by
`AnalysisResult::sources` (`:162-167`); travels internally as a nullable `SourceMap* src`
(`compiler.cpp:311, 369, 383, 410, 461`). Stores the resolver's `canonicalPath` **verbatim, unnormalized**
— std modules store logical `"std.io"`, the entry stores the caller's `entryPath` or literal `"<entry>"`
(`:771`). Id order: entry 1, then `std.core`, then imports post-order, then lib preludes; deduped by canon
(`:351`).

The LSP treats absoluteness as the discriminator (`main.cpp:1085-1094`): absolute ⇒ `file://`, `std.`
prefix ⇒ `polyglot:` virtual doc, else not navigable. A resolver returning a relative canonical path
silently kills cross-file go-to-definition (the unit test at `tests_main.cpp:851-858` uses relative
`"./geo"` and asserts only the name).

`format()` (`compiler.cpp:755`) and `importSpecifiers()` (`:742`) share the fileId-0 blindness.

**Multi-file today:** `loadImports` (`:308-364`) lexes/parses each module then **merges into one flat
`CompilationUnit`** (`mergeDecls`, `:361`), tagged by `originModule`. Distinct fileIds are produced **only**
on the `analyze()` path. The CLI's multi-input build compiles each non-imported root separately
(`main.cpp:733-743`), so each `compile()` is its own closure with everything at 0.

**Other fileId consumers:** the LSP's `sources_` map (`main.cpp:940`, filled `:1009`), the
`d.pos.fileId != 1` diagnostic filter (`:1054`), `definition` (`:1102`), `references` (`:1126`), rename
(`:1141`). All sema/capability/parser/lexer diagnostics inherit correct fileIds automatically once
`compile()` passes a map — no signature changes there.

## 3. Multi-line strings reaching `line()`

- **Confirmed live:** `module.attrImportsBlock` (`emitter_base.cpp:1155-1161`) newline-joins
  `ir::Module::attrImports` (`ir.hpp:612`, filled `lower.cpp:364` from `:198-199`) and all four `Program`
  rules feed it into one `{"line": …}` (`csharp:2583-2585`, `php:3331`, `python:1268`, `typescript:2957`).
  N `using` lines, one `line()` call.
- **Possible:** `attrLines` from an `extern attribute`'s `actual(<target>)` string (`lower.cpp:160-188`) —
  the lexer decodes escapes (`lexer.cpp:63`), so a user `\n` produces a genuinely multi-line entry flowing
  through `:1399`/`:1431`. Same for `ir::Extern` node code (`:40`) and FFI `Bound` templates
  (`substBoundTemplate`, `:1629-1641`) which land inside statement `line()` calls. A JSON scan of all four
  manifests found **zero** raw newlines in any string, so today this requires user input.
- **Not a path:** string literals — `renderString`/`specEscape` (`backend_spec.hpp:278-291`, `:229-244`)
  escape `\n`.

## 4. Manifest: schema, validation, and what "anti-silent-drop" actually covers

`BackendSpec` at `backend_spec.hpp:24-153`; loader `backend_spec_json.cpp:26-127` (only two hard
validations: object-ness `:28`, non-empty `name` `:32-36`, plus the `blockStyle` **enum** `:50-57` where an
unknown *value* is a load error); inverse `backendSpecToJson:129-251` — **a new spec field must be added in
both directions** or the round-trip silently loses it. Manifest validator: `buildBackend`
(`backend.cpp:214-329`).

| Check | Where | Fails on |
|---|---|---|
| Capability vocabulary **closed** | `backend.cpp:106-133`, enforced `:270-274` | unknown/typo'd capability key |
| Capability stance tri-state | `:256-267` | anything but `native`/`emulated`/`false` |
| Construct coverage | `kCoverage[] :173-187`, enforced `:279-294` | a construct with no rule and no declared stance; a `native` claim with no rule |
| Builtin catalog closed | `fnCatalog() :143-151` (15 names), enforced `:301-305` | unknown `{"fn":…}` |
| Dangling `call`/`mapMembers` | `:154-162`, enforced `:306-310` | reference to a missing rule |

**Not checked:** unknown *top-level* keys and unknown *`spec`* keys — both silently ignored. So a new
top-level key is additive and non-breaking for old plugins, at the cost of an old CLI silently dropping it.
Conversely, adding to `kCoverage` breaks the load of **all four plugins at once**, and adding a capability
key makes any manifest using it fail on older CLIs. Docs mirror: `docs/plugin-authoring.md:31-42`,
`:83-114`, `:92-160`, `:314-337`.

### 4.1 The trait-flag precedent (`crossDirImports`)

JSON `plugins/typescript/…:4` → read `backend.cpp:325` → ctor `:50-53` → member `:91` → accessor `:86` →
virtual `backend.hpp:108-112` → consumed `compiler.cpp:719` → documented `plugin-authoring.md:40` → pinned
by `tests_main.cpp:2936-2973` (TS yes / Python+C# no, **and** a non-flag target refuses rather than
half-applies). Spec-level booleans (`linksWithoutImports`, `forbidsShadowedLocals`,
`expressionOnlyLambdas`, `backend_spec.hpp:84-88`) gate *Core passes* instead.

**Rule of thumb visible in the code:** a flag gating a compiler pass or host behaviour is **top-level**; a
flag that is emission data/spelling lives in **`spec`**. Origin mapping makes the host write a sidecar ⇒
top-level.

### 4.2 Closest existing parameterized-text keys

`preludes` (`backend_spec.hpp:98`) — the precedent for Core prepending raw per-target text; `tables` +
`{"fn":"subst"}` with `$x` holes (`:119`, `substX:180`, `specSubst:198`); `wrapInt` (`:124`) — per-key `$x`
template, the closest analogue to `#line $n "$f"`; `blockStyle`/`stmtEnd`/`rethrow`/`memberOp` (`:59-69`).
**There is no comment-syntax key anywhere** — the Core never emits a comment, so TS's
`//# sourceMappingURL=` footer needs the manifest to carry the literal text.

### 4.3 Per-target text produced by C++ rather than data

- **The one real violation:** `compiler.cpp:673` `const bool splitPrelude = (target.name() == "csharp") && lib.sharedPrelude;`
  (issue #14's shared-prelude hoist; documented at `polyglot.hpp:120-123`, `POLYGLOT_PRD.md:1249`).
  Candidate for a `sharesPreludeFile` trait flag.
- **Sanctioned:** `ir::Module::attrImports` is verbatim target text built by lowering — legal because the
  spellings it bakes come from `ConstSpelling` spec data (`lower.cpp:154`).
- **Deliberately constant:** the 4-space indent (`emitter_base.cpp:1599`), `renderString`'s escape set,
  `renderArgs`, `operatorPrecedence` (`backend_spec.hpp:305-318`, with a miscompile post-mortem in the
  comment).
- `backend.cpp:199, 347` compare a loaded plugin's own declared name to a lookup key — registry mechanics,
  not a language distinction.

### 4.4 Arm-trace consequence

Only **rules** enter the coverage denominator (`armtrace::registerManifest` + `engine::indexRule`,
`backend.cpp:237-247`). Spec/top-level data is **not** an arm. Expressing origin mapping as rules would
create permanently-uncovered arms for an off-by-default feature — the documented PHP `UnionDecl` friction
(`docs/prd/code-coverage-upload/PRD.md:508-520`). Hence: data, not rules — plus at least one gate
invocation with the flag on, so the behaviour has a live witness.

## 5. The write path — the collision the issue did not anticipate

- `EmitResult{ok, code, modules, diagnostics}` (`polyglot.hpp:72-77`); `ModuleFile{basename, code,
  sourcePath}` (`:61-65`, empty `sourcePath` = synthesized prelude). `Backend::emit` returns a bare
  `std::string` (`backend.hpp:71`).
- `emitOne` (`main.cpp:221-243`) → `compile` (`:225`) → `resolveClosureOutputs` (`:233`) → `writeDedup`
  per file (`:238-241`). `writeDedup` (`:168-194`): cross-root dedup by **content equality**, conflict is a
  hard error (`:175-177`), write-if-changed for mtime stability (`:182-187`). **Watch mode has a separate,
  duplicated writer** (`:384-403`).
- **Precedent for a second output file exists but is per-build, not per-input:** the C# shared prelude
  (`main.cpp:537` → `compiler.cpp:673`, `:733`), routed as prelude-like (`pgconfig.hpp:197-201, 227`) and
  collapsed by `writeDedup`. It uses the **same** extension — so a `.ts.map` sibling has **no** precedent;
  `resolveClosureOutputs` appends exactly one `ext` per file (`pgconfig.hpp:200-201, 208-209, 214-215`).
- ⇒ **CORRECTED (2026-09-17, maintainer review).** An earlier version of this section concluded that
  *absolute* `#line` paths would make previously-identical content differ per root, breaking the collapse.
  That is wrong, and it was load-bearing for a path-form decision. `writeDedup` compares content **within
  one build on one machine**, and a `.pg` file has **one** absolute path however many roots import it — so
  absolute paths are byte-identical across roots and dedup is untouched. If anything, *repo-relative* paths
  are the ones that can diverge (the same shared module reached from two roots yields two different
  strings). See PRD §4.F and the decision log in `PLAN.md` (D2). SP2 confirms empirically.
- MSBuild's `_PolyglotAddGenerated` globs `$(PolyglotOutDir)*.cs`, so a `.map` is not swept into
  `@(Compile)`/`@(FileWrites)`.

## 6. Option plumbing precedents

**`--access` (the model to copy — CLI + pgconfig + MSBuild + emitter hook):** parse `main.cpp:584-585` →
`buildGroup :638-639` → merge, flag wins `:491-495` → `LibConfig::access` (`polyglot.hpp:113-117`) →
`ir::Module::access` (`ir.hpp:624`, set `compiler.cpp:677, 716`) → `hooks_.access` (`emitter_base.cpp:1569`,
struct `emitter_base.hpp:136-140`) → the `{"fn":"access"}` builtin (`backend.cpp:147`; used
`plugins/csharp/…:1123, 2145, …`). pgconfig `PgConfig::access` (`pgconfig.hpp:79`), parsed `:101`.
MSBuild `_PolyglotAccessArg`.

**`--emit-arm-trace` (a *different* shape — a process-global singleton):** parsed in `main` **before**
dispatch and erased from `args` (`main.cpp:1514-1530`) so every subcommand parses unchanged; flips a Core
global (`arm_trace.hpp`, `arm_trace.cpp:10-86`); consulted at backend load (`backend.cpp:235-247`); the
side-file is written after `run` returns even on failure (`:1540-1546`); self-documented as "a DEBUG flag,
not part of the stable CLI contract". Useful for the *framing* (off by default, unmeasurable when off,
documented in the coverage-instrument table) but **not** the structural model — origin mapping is
per-compile and emit-affecting.

**Other facts:** `runBuild`'s parse loop hard-refuses unknown options (`main.cpp:588-590`), so a new flag
must be registered there; usage text at `:54-79`; `runCheck` (`:697-706`) and the LSP (`:1324`, configured
via `initializationOptions`) have their own parsers; `watchBuildOnce` builds a `LibConfig` **without**
`access` (`:346-347`) — an existing gap not to replicate. `compile()` callers: `:225, 374, 735, 1043`;
`analyze()` callers: `:999, 1023`. No C API, no shared-library boundary — Core is a static lib.
`pgconfig.lock.json` (`lockfile.hpp`) is plugin pins only; unaffected.

## 7. Byte-sensitive test surfaces (what "off by default" protects)

- **`tests/MintPlayer.Polyglot.Tests/src/tests_main.cpp`** — 3258 lines, 517 `check(...)`, emission tests are
  **substring** assertions (`has()` at `:66`; sections "C# emission (golden substrings)" `:297`, TS `:311`).
  ~165 needles span a `\n`, and there are ~22 `R"(` raw-string expectations — those are what a directive
  injection breaks.
- **Conformance stdout goldens** — `tests/conformance/programs/*.expected`, **17 tracked files**. They pin
  **program stdout**, not emitted source ⇒ **safe** from this feature. Compared by gate **G32**
  (`run-conformance.ps1:299-311`, CRLF→LF + trailing-whitespace normalization). The `.cs`/`.ts` in
  `programs/` are untracked build leftovers, not goldens.
- **Conformance suite** — 114 `.pg` programs, C# is always the oracle (`scripts/lib/OracleCompile.ps1`,
  one `csc /shared`), parallel per-program work dirs, gates G30 (60 s timeout), G31 (unique root), G32,
  #9 (the C# must compile), G28 (pinned PHP-refuser set, `run-conformance.ps1:68-73`), plus anti-silent-drop
  (every configured target's output file must exist). Sub-dirs `programs/library/`, `programs/modular/geom/`
  (the multi-file case A6 needs).
- **Other readers of emitted text:** `tests/refusals` (17 fixtures; asserts **no** output file is written
  and a pre-seeded twin keeps its sentinel byte-for-byte), `tests/fidelity/run-roundtrip.ps1` (`polyglot fmt`
  idempotence over `docs/lang/samples/*.pg` — source-level, unaffected), `tests/library/run-library.ps1`
  (tsc strict over staged `.ts`), `tests/samples/run-emit.ps1`, `tests/nullable`, `tests/msbuild`,
  `tests/cli`, `tests/lsp`, `tests/watch`, `tests/registry`.
- **There is no `--update`/`-Update` golden-refresh switch anywhere** in `scripts/`, `tests/` or CI. The 17
  `.expected` are hand-maintained. This is itself an argument for the feature being off by default.

## 8. Docs conventions this PRD follows

One slug directory per feature: `PRD.md` (design contract) + `PLAN.md` (dependency-ordered slices +
acceptance matrix + Log), optional `ANALYSIS.md`. Every recent PRD opens with **§0 Prime directive check
(POLYGLOT_PRD §3)**. Slices are `## Slice N — <name>` from 0, sub-lettered when large, each with explicit
acceptance. Spikes are a PRD `## 5. Spikes` section (`**SPn** (~time) — … **Gate:** …`) plus a PLAN
`## Slice 0 — spikes (throwaway, time-boxed)`. PLAN carries a build-discipline preamble and a `## Log`
appended per slice with an explicit **Surprises** note. `docs/prd/code-coverage-upload/` and
`docs/prd/p37-feature-batch/` are the two models. The `dcg:issue_plan` skill's generic
`docs/issue_<n>_plan.md` template is **deliberately not** used here — but its mandated **Out of Scope** and
**Open Questions** sections are kept.

Master-doc touchpoints: `docs/prd/PLAN.md` milestone entry (last is **P37** at `:3435`; the Stretch bullet
to strike is `:2386` — "Source maps: … decide the C# debug story"); `POLYGLOT_PRD.md` §6 roadmap (`:1356`)
and the three existing mentions at `:234`, `:610`, `:1532`. Careful: Core's existing `SourceMap` type means
the **input** file table, not a v3 output map.

## 9. Findings added by the maintainer design review (2026-09-17)

Facts established during the review that were not in the original five investigations. Decisions they
produced are logged in `PLAN.md`; the design consequences are in `PRD.md`.

1. **This document's `writeDedup` conclusion was backwards** — see the correction in §5. It had been used
   as an argument for repo-relative directive paths, so the correction flipped the path-form decision.
2. **`scripts/verify-coverage-paths.ps1:39-48` has a comment/code mismatch.** The docstring promises the
   server's suffix matching "or vice versa", but the code implements only `tracked.EndsWith("/$report")` —
   i.e. it accepts a report path that is a *suffix of* a tracked path, and rejects one that is *longer*.
   The server does longest-suffix matching (stripping from the left), so the script is **stricter than the
   server it models** and would fail a build over an absolute-derived report path the server resolves
   fine. Fixed in PLAN slice 6.
3. **`#line hidden` removes a line from the report, it does not mark it uncovered**, and a directive only
   produces a report entry where there is IL to attach a sequence point to. Both follow from issue #69's
   probe. This narrowed the declaration-position work from 17 structs to 3 (`ClassField`, `Global`,
   `Method` — the declarations whose emitted code executes). `Method` was kept because Roslyn puts a
   sequence point on the body's opening brace, so a position there turns method entry into a genuine
   numerator contribution instead of a discarded point.
4. **One path form cannot serve both sinks.** Verified from the consumer's `pgconfig.json`:
   `…/Chess/polyglot/chess_solver.pg` routes its TS output to `src/RLDemo.Web/ClientApp/src/app/chess/`.
   A v3 map's `sources` resolve relative to the **map file**, so any root-relative path placed there
   resolves nowhere. Hence per-sink forms, plus `sourcesContent` to remove path resolution from the TS
   side entirely.
5. **`SourceMap` stores the resolver's path verbatim and may therefore be relative**, while the LSP
   discriminates on `fs::path(canon).is_absolute()` (`main.cpp:1085-1094`) — so a relative canonical path
   silently kills cross-file go-to-definition. Canonicalizing on `add()` (PLAN slice 1) closes this latent
   bug as a side effect of the work `#line` needed anyway.
6. **`tests/cli/run-cli-smoke.ps1` compiles nothing.** Only the conformance leg has the `csc` oracle, so a
   witness that proves "the C# still compiles with the flag on" has to live there.

## 10. Consumer-side facts (`C:\Repos\MintPlayer.AI`, read-only)

Gathered to ground the design; recorded so nobody has to re-derive them. This repo does not build or test
that one.

- `docs/prd/COVERAGE_90_PRD.md:108` — the consumer wants **both** sinks ("TS hits, `.pg`-attributed via v3
  source map"), not just C#.
- `:225-231` — its spike **S5** owns the chained `.pg → .ts → .js` remap risk, with two fallbacks; the
  `@vitest/coverage-istanbul` + explicit `inputSourceMap` fallback **requires the map to exist**.
- `:92` — `.pg` files are in `git ls-files` (verified there), which is what coverage.mintplayer.com needs.
- `:236` — its spike **S6** confirms the server resolves a `.pg` path and merges two reports naming the same
  file.
- 9 tracked `.pg` files, 7,097 lines total; ~1,832 declaration-ish lines (~26%).
- `MintPlayer.AI.ReinforcementLearning.Environments.csproj:15` pins
  `MintPlayer.Polyglot.MSBuild` **0.9.9**; this repo's latest tag is **v0.9.10**, and `CLAUDE.md`'s status
  block claims **0.3.2** — i.e. the always-on context is six minor versions stale. Everything in-tree is
  `0.0.0-dev`, stamped at release by `release.yml` / `publish-plugins.yml` / `publish-vscode.yml`, with
  lockstep enforced in `main.cpp` / `pluginresolve.hpp`. PLAN slice 9 replaces the numbers with the rule.
