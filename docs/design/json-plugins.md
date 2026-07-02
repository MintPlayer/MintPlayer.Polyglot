# 100% JSON language plugins — the P19 design

*Design note, 2026-07-02 — synthesized from a 4-agent investigation (declarations / hard expressions +
types / the builtin tier / the plugin artifact). Companion to `backend-spec.md` §6 (the DSL + interpreter,
P18) and `plugins-and-targets.md` §6.1–6.3 (distribution + pgconfig). This note carries the detail behind
PRD §4.11; the slice plan is PLAN §P19.*

**The question.** P18 established that a backend's *expression walk* flattens to JSON rules over a fixed,
non-Turing-complete interpreter (proven: all three backends' expression families now run through one
interpreter, byte-identical). Can the **whole plugin** be JSON — declarations, types, the remaining hard
expressions, the std arms, the capability set — so `polyglot install @polyglot/kotlin` adds a language with
**zero C++ in the plugin and, in the steady state, zero Core changes**?

**The verdict.** Yes, with a bounded, audited growth of the fixed Core tier:

| Front | Flattens to data | Mechanism | Residual |
|---|---|---|---|
| Declarations | ~90% base / ~98% with Core additions | decl-keyed rule tables + `line`/`block`/`mapDecl` + `type` + `require` | <2% = genuine target limits → §3.E refusal |
| Hard expressions + type renderers | ~95% | `interleave`/`fold`/`emitBlock`/`fresh`+`let` + lowering-absorbed state | fixed §3.C builtins (Core, shared) + 2 capability refusals |
| Builtin tier | steady-state new language ≈ 90–95% pure data, **zero Core** | ~10-entry generic catalog + spec parameters | the *pioneer* of a new representational class pays one additive Core minor bump |
| Artifact/loader/std | fully data | manifest + overlays + load-time validation | runtime-trust residual (documented, unchanged) |

The honest floor is unchanged in *kind* from P18 §4.10: completeness comes from **Core growth bounded by
"expressible as selection-among + substitution-into plugin templates"**, plus the §3.E gate for genuine
target limits. Nothing found in any front requires plugin-shipped code.

**Two scope decisions (user, 2026-07-02) — supersede the corresponding P18 §4.10 lines:**
1. **No backward compatibility.** No `--legacy-backend` fallback, no keeping the C++ backends a release
   behind: when a family/layer migrates (and its byte-identity gate passes), the C++ is deleted in the
   same slice. The byte gates remain — they verify the *extraction*, not a compat contract.
2. **The CLI is a pure engine — no embedded target specs.** The binary ships **zero** language backends.
   C#/TypeScript/Python are **ordinary plugin packages**, exactly like a future `@polyglot/kotlin`: the
   tool reads `pgconfig.json` in the cwd, sees the requested `targets`, resolves their npm packages (via
   `dependencies` → lockfile → cache/registry; `polyglot install` fetches what's missing), and interprets
   the JSON inside — which must therefore carry **all** transpiling instructions. That is the point of the
   tool. Consequences: the three first-party plugins are developed **in this repo as real package sources**
   (`plugins/csharp/`, `plugins/typescript/`, `plugins/python/`) and published to npm like any plugin;
   `pgconfig.json` supports **local path dependencies** (npm `file:`-style) so the repo's own conformance
   gates and plugin developers load plugins from disk without a registry; with no pgconfig/targets, the CLI
   errors with actionable guidance ("no targets configured — add pgconfig.json + run `polyglot install`"),
   it does not fall back to anything. "Zero-dep single binary" still holds for the *executable* (no runtime
   library deps); the language *data* simply isn't welded into it. The **std skeletons** (`std.io` etc. —
   the target-neutral `.pg` API surface) remain in Core: they are the *source* language, not a target; every
   per-target arm lives in the target's plugin (§5.2).

---

## 1. The complete primitive set (closing §6's reserved list)

P18 shipped: `tmpl` / `get` / `emit` / `emitChild(side)` / `case`+`Test{eq,has,and,or,not}` / `fn` /
`map`(+`sep`,+`item` template). The investigation confirms the reserved primitives and fixes their shapes —
all fixed, in-Core, non-Turing-complete (iteration counts = list lengths; no arithmetic; `call` depth-capped):

**String-rule additions (expressions + types):**
- **`interleave`** — zip a literal list with a hole list (`Interp`'s chunks/holes): per-`lit` rule,
  per-`hole` rule, reusing the `ItemCtx` redirect with two prefixes. Sole consumer: `Interp`, all targets.
- **`fold`** — right-fold over a child list with `$acc` (Python `Match`'s ternary chain
  `v0 if c0 else (v1 if c1 else vLast)`). Sole consumer: `Match`; extracted from the working
  `matchChain`, not guessed.
- **`emitBlock`** — render child IR *statements* through the engine (statement-bodied lambdas; `style:
  "inline"` reuses `EmitterBase::inlineBlock`). Runs no plugin code — it re-enters the fixed statement walk.
- **`type`** — recurse a child `TypeRef` through the plugin's **type-rule table** (§2).
- **`fresh` / `let`** — mint a fresh temp name / bind a scoped value (single-eval IIFE/walrus shapes,
  the C# operator `this`→`lhs` rebind) — only where lowering can't pre-allocate (§3).
- **`require`** — the one controlled side effect: add `{bucket, key, text}` to a dedup'd preamble bucket,
  flushed where the `program` rule places it (Python's `import asyncio` / `_pg_idiv` prelude, and any
  future target's helper fragments, shipped as plugin `preludes/` files).

**Emitter-rule flavor (declarations):** rules that *write lines into the buffer* instead of returning a
string, reusing `EmitterBase`'s existing `line()`/`openBlock()`/`blockBody()` machinery (brace style
already spec data, proven 3-way):
- **`line`** — `{"line": <stringRule>}` one indented line.
- **`block`** — `{"block": {"head": <stringRule>, "body": [<declRule>…]}}` — open/indent/body/close per
  the spec's `blockStyle`.
- **`mapDecl`** — the line-emitting analog of `map` (loops over members/fields/methods); the *string*
  `map` remains for members spliced into a head (C# positional record header — the `MakeCase` pattern).
- **Decl rule tables** — `{ DeclKind → Rule }` per target (enum/interface/union/record/class/method/
  function/extension/global) + a top-level **`program` rule** expressing the whole module scaffold as
  ordered `mapDecl`s (C#'s `static class Program` wrapper, TS's flat file + trailing `main();`, Python's
  flat file + `asyncio.run` entry + prelude flush). Cross-target *shape* divergence (C# positional record
  vs TS class+ctor+equals vs Python `__init__`+`__eq__`) is handled exactly as the expression layer already
  handles it: **a different rule per target, each still pure data**. Synthesized members (TS `equals`,
  Python `__eq__`, C#'s exhaustive-match default) are plugin templates over field loops — no Core
  "derived member" hooks.

**Test additions:** `any`/`all` quantifiers (C# `hasCatchAll`, Python `hasFieldInit`) and `typeIs`/`isKind`
type-class predicates (driven by the plugin's type→class table, §4).

## 2. Type rendering as a rule table

`csType`/`tsType`/`pyTypeName` become per-target **type-rule tables** over `TypeRef` shape, evaluated by
the `type` primitive with a `TypeEvalContext` exposing `type.kind/.name/.nullable/.isValueType/.scalar/
.externType/.args.count/.args.<i>/.ret/.base/.returnsUnit`. Worked C# rule (function/tuple/nullable/
scalar/extern/named arms) reproduces `csType` exactly; TS differs only in affixes (`T | null`, `[A, B]`,
arrow types); Python collapses to `externType-or-name`. Fixed Core: the `type` recursion + the
`substTypeTmpl` builtin (extern `$0`/`$1` substitution, generalizing the existing per-target copies). The
value-type set derives from the scalar table (data). Generic bounds (`csWhere`/`tsGenerics`, INumber
erasure) are a filtered `map` in the *declaration* tables.

## 3. Lowering absorbs module facts + temp allocation

The interpreter context stays node-local and stateless; everything the emitters currently compute from
module scans or mutable counters becomes **precomputed IR bits in `lower.cpp`**:

| Today (emitter state) | Becomes (IR fact) |
|---|---|
| `recordNames_` / `lhsIsRecord`, `hasOpMethod` | bits on `Binary` (already fabricated in ctx; move to lowering) |
| `interfaceNames_` (TS `extends` vs `implements` split) | `base.isInterface` bit |
| `indexerTypes_` | `receiverHasIndexer` bit on `Index` |
| `recordFields_` + TS `with` IIFE + `tmp_` | `With` lowered to ordered `ctorArgs` + `baseIsSimple` + lowering-allocated `tempName` |
| C# `thisAlias_` | lower `This`→`Var("lhs")` inside operator bodies |
| match `hasCatchAll`, `genArgs`, binder accessors | bits/strings on `Match`/arms |
| Python walrus temps (`?.`/`??`) | lowering-allocated temp names (else `fresh`/`let`) |

**Gate caveat (from the expr front, load-bearing):** a lowering bug can't be caught by diffing two backends
that both consume the same wrong precomputed field — so **each lowering absorption is its own slice with
its own byte-identity gate** before any rule consumes the new fact.

## 4. The generic builtin catalog (the zero-Core-changes story)

Today's ~15 per-target builtins + 3 `wrapAtom` policies collapse into **~10 generic catalog entries +
plugin parameters**:

| # | Entry | Plugin parameters | Subsumes |
|---|---|---|---|
| 1 | `table(name, key)` | named string tables | `intSuffix`, `opSpelling`, `opMethod`, scalar map |
| 2 | `escapeString` | **escape-char set** (spec; default = today's) | the shared `renderString` |
| 3 | `ident` | keyword set + strategy `{prefix "@" / rename-suffix / none}` | `csIdent`, `pyId` |
| 4 | `mangle` | mangling strategy | `pyName` (`$`→`_`) |
| 5 | `renderType` | the §2 type-rule table | `elemType`/`castType`/`typeArgsSuffix` |
| 6 | `wrap(type, expr)` | **`intRepr` strategy enum**: `arbitrary` (mask — Python/Ruby) / `nativeFixed` (cast-back — C#/Java/Go) / `f64+bigint` (bitwise+BigInt — TS/JS) + max width | `wrapInt`, `subWordWrap`, `narrowWrap`/`i64Wrap`/`imul` |
| 7 | `convert(from, to, expr)` | type→{class,width} table + a **(fromClass,toClass) → Rule matrix** with `$x`/`$to` holes | `tsConvert`, Python `convert` (same shape, fewer rows) |
| 8 | `fresh(prefix)` | — | the `tmp_` counters |
| 9 | `require(bucket, key, text)` | `preludes/` fragments | `needsIdiv_`/`needsAsyncio_` |
| 10 | `wrapAtom` as spec data | per-side `ExprKind` sets + the precedence table | all three C++ policies |

**Walkthroughs.** *PHP*: pure data once two generic gaps close — (i) `escapeString`'s escape set becomes a
spec parameter (PHP must escape `$` in double-quoted strings; the first thing any new language breaks) and
(ii) the `typeIs` predicate ships (string `+`→`.` concat dispatch). u64 refuses (§3.E). *Rust*: installs
with zero Core changes; its semantic mismatch (ownership, no inheritance) surfaces as a **smaller supported
surface via loud §3.E refusals**, not Core PRs; its `wrapping_add` style would be one additive `intRepr`
value if ever wanted. **Steady state: "every new language needs a Core PR" is false. Only the pioneer of a
new representational class pays a one-time, additive Core minor bump that every later language in that
class gets for free.** Do (i) and (ii) proactively before the first external plugin.

**Governance:** the builtin/primitive registry is the trusted, versioned, in-Core vocabulary — never
plugin-authored. Plugins declare **`requiresCore: ">=x.y"`**; the loader hard-refuses a plugin needing a
newer Core, naming the version ("needs newer polyglot", never "unknown builtin"). New builtins/enum values
are strictly additive.

## 5. The artifact — package, manifest, capabilities, std overlays

**Package layout** (npm tarball payload; Polyglot reads `package.json` only for name/version/integrity):

```
@polyglot/kotlin/
  polyglot-plugin.json        # manifest: schema, id, version, kind, requiresCore, backend{…}
  backend/ spec.json expr.json stmt.json decl.json precedence.json
  types/externtypes.json      # extern-class type spellings + ctor templates
  capabilities.json           # Feature -> "native" | "emulated" | false
  builddeps.json              # SDK / PackageReference / npm / pip
  std/*.overlay.json          # this target's arms for the std modules (§5.2)
  prelude/*.kt                # raw target-code fragments require'd by rules
```

Manifest keys: `schema` (manifest format, integer) + `requiresCore` (interpreter contract, semver) — two
independent version axes; `kind: "backend" | "binding" | "std"` (binding/std plugins omit the `backend`
block and are *already fully interpretable today*); target info `{targetId, displayName, fileExtension,
commentSyntax, runCommand?}` (`runCommand` is conformance-harness-only, never executed at install/compile).

**5.1 Tri-state capabilities.** `Feature → "native" | "emulated" | false` supersedes the bare bool:
`native` = idiomatic call-site-preserving emission; `emulated` = faithful but non-idiomatic (extension
methods as `m(x)`, match as an if-chain) — usable, surfaced as a warning; `false` = §3.E refusal. The
`Feature` enum stays the closed vocabulary. Fixes the gap where PRD §3.E's "faithful-but-call-site-
changing" language has no home in a bool.

**5.2 Std de-hardcoding — skeleton + overlays.** Each std module splits into a **target-neutral skeleton**
(the `expect`/`extern class`/`extension` signatures, stays embedded) + **per-plugin overlay files**
(`{module → member → template}`, the unchanged `$this`/`$0`/`$T` grammar — only the carrier moves from
`.pg` `actual(target)` arms to member-keyed JSON). Link-time merge selects **the active target's** arm per
member, which collapses the last hardcoded-target IR surface: `ir::Bound{cs/ts/pyTemplate}` →
`{template}`, `ir::ExternType`'s six fields → `{typeTemplate, ctorTemplate}`. A *used* member with no arm
refuses **at the call site** (the existing call-site-keyed `checkCapabilities` mechanism — unused gaps are
harmless). The three first-party targets' overlays are mechanically extracted from the ~97 `actual(...)`
arms into **their plugin packages** (`plugins/<target>/std/*.overlay.json` in this repo) with a
byte-identity gate — after which `compiler.cpp` carries only the target-neutral skeletons.

**5.3 Loader + validation.** `loadBackend(manifestBytes, {file→bytes}) → BackendHandle` (Core stays
IO-free; the CLI/LSP read files; `compile()` unchanged — same handle type). There are **no built-ins**:
first-party and third-party plugins traverse the identical load path — the strongest possible guarantee the
plugin path is first-class. Validation obligations, all fail-loud at load: schema shape; every rule parses; **every IR
node kind has a rule OR its capability is declared `false`** (the anti-silent-drop rule — "no rule" is
never "emit nothing"; completes the P9-V `break`/`continue` lesson); referenced builtins exist in the Core
catalog; `get`/`emit` paths validate against a Core-published per-node field catalog; `require` keys exist
in `preludes` and land in the `program` rule; externTypes/std arms reference declared members;
`requiresCore` satisfied; depth/size caps + `call`-cycle check; unknown *manifest keys* warn+ignore
(forward-compat), unknown *rule primitives* refuse. Emit-time residue is only the program-dependent §3.E
gate (used feature/member lacking support) — still a clean refusal before lowering.

**5.4 CLI/LSP.** `--target <name>` (and the default = pgconfig `targets`) resolves through pgconfig
`dependencies` — local `file:` path → lockfile-pinned cache → registry; a miss says "run
`polyglot install @polyglot/<name>`". No compiled-in tier. The LSP gains **`polyglot/targets`** (returns
the workspace's resolved `{id, displayName, fileExtension}` set), closing `extension.js`'s hardcoded
`TARGETS` (`FIXME(P10)`) with no client change. `polyglot install` stays the single trusted registry
writer: npm HTTP API data-only, SHA-512 `dist.integrity` verify, zip-slip-safe extract, never runs
lifecycle scripts, **validates via `loadBackend` at install time**, records into `registry.json` +
`pgconfig.lock.json`.

**5.5 Trust.** The data-only stance covers every new section. The honest residual is unchanged and now
concentrated: **std overlays + preludes contain raw target-code templates** — zero transpile-time
execution, but they land verbatim in generated output and run later with the dev's privileges. Add an
**install-time lint that warns (never blocks)** on high-signal sinks (`Process`, `exec`, `eval`,
`child_process`, network) as a review prompt — explicitly not a guarantee (the built-in `std.io` itself
legitimately emits `File.Delete`); signing/trust remains the deferred real mitigation.

## 6. Two latent miscompiles found (fix immediately, independent of P19)

Both are the P9-V silent-broken-output failure mode, in today's shipping Python backend:
1. **Statement-bodied lambda** emits the sentinel `__py_unsupported_block_lambda__` into "valid" output
   (`emit_python.cpp`). Must become a real capability refusal (`Feature`-gated or a targeted diagnostic).
2. **`With` (record update)** falls through to `__py_unsupported_expr__`. Lower `With` to the ordered
   ctor rebuild (§3) — Python classes support it, so no capability is lost — or refuse until then.

Also recommended: **`i32.parse`/`f64.parse` become std `Bound` bindings** (like `Math`/`List`), deleting
the per-target `parseNumber` special case from `MethodCall` — the same "std, not compiler" move P13 made
for `print`/`Math` (module membership is unchanged; only the emission mechanism moves).

## 7. Reserved & forbidden identifiers (added 2026-07-02; 2-agent investigation)

**The ask:** configurable "forbidden keywords" per target (`Program` on C#, `this` on TS, `$_SERVER` on a
future PHP) — where do the lists live, and what does the compiler do on a hit?

**What the empirical investigation found first (all verified by compiling/running emitted output):** sema's
collision detection knows only *user-vs-user* and *user-vs-Polyglot-builtin* clashes — it has zero knowledge
of target keywords, emitter scaffolding, or runtime globals. The live surface: **(K)** target keywords that
aren't `.pg` keywords — C#/Python escape them at *value* sites only (decl names unescaped → `record object`
breaks both C# and TS), TS escapes nothing; **(S)** scaffolding — `record Program` / `fn Main` / operator
param `lhs` break the C# build; a local `_m` in a match arm, a union field named **`tag`**, a local `str`
in Python interpolation, and a user `fn _pg_idiv` on Python all produce **silent wrong answers** (the
`_pg_idiv` case evades every current gate — Python has no second oracle); a local `self` silently rebinds
the Python receiver; **(R)** runtime globals — a top-level `fn console` breaks TS at runtime, a user type
`System` breaks the C# SDK build despite `global::` (AssemblyInfo.cs is outside our emission).

**Layering (no Core data, per the scope decisions):**
- **Plugin manifest** — the target's own facts, ONE `identifiers` block so the escape table and forbidden
  lists can't drift (this *extends* the §4 `ident` catalog entry):
  ```jsonc
  "identifiers": {
    "keywords": ["class","this","for", …],              // the ESCAPE set (lexer keywords)
    "escape":   { "strategy": "prefix", "with": "@" },   // C#; Python {"strategy":"suffix","with":"_"}; null = unescapable
    "reserved": ["Main","Program","Extensions"],         // fixed generated-scaffolding names -> REFUSE
    "globals":  ["console","String","Math","BigInt","Number"] // runtime names emitted code relies on -> REFUSE at top level
  }
  ```
  Scaffolding names are **declared, not inferred** (they live inside template strings and raw prelude
  fragments — scraping them out would be brittle; declaration is auditable and load-validated; a warn-only
  lint can flag a declared name that appears in no rule).
- **pgconfig.json** — per-project additions, target-scoped with `"*"` wildcard (bare array = all targets):
  `"forbiddenIdentifiers": { "*": ["temp"], "csharp": ["Program"] }`.
- **Core** — mechanism only: the `ident` escape primitive + a **`checkReservedNames`** pass beside
  `checkCapabilities` (same per-target, refuse-out-loud pattern), + collision-aware `fresh`.

**Policy matrix.** *Escape silently* only for keywords with a declared total, reference-preserving
transform (today's `csIdent`/`pyId`, becoming data — and extended to **declaration-name sites**, which
today's code misses). *Refuse with a targeted diagnostic* for: unescapable keywords (`escape: null` — all
of TS), scaffolding `reserved` names, runtime `globals` (matched against the *emitted* spelling), and
project-forbidden names ("'Main' is reserved on target 'csharp' (generated entry point); rename it or drop
that target"). *Never auto-rename user identifiers* — no reference-preservation guarantee, and a silent
rename is the §3.B sin wearing a helpful face. **Exception worth recording:** for `Program`/`Extensions`
the *wrapper* owns the name, so the alternative to refusing is renaming/namespacing the wrapper itself —
deferred (it churns every emitted file); v1 refuses via `reserved` data, and un-reserving later stays open.

**Fresh-name & data-shape hygiene (the silent cases are NOT config — they're emitter bugs):** the `fresh`
primitive becomes a **collision-aware gensym** (consults the scope's used names; lowering reserves the temp
prefixes), covering `_m`/`lhs`/`self`/`__*` temps; the union discriminant `tag` and TS's structural
`equals` slot are *data-shape* collisions — v1 refuses a union field named `tag` (TS/Python) and a record
method named `equals` (TS) via the same `reserved`-style plugin data, keyed to member positions.

**Invariant — identifiers only, never text (user requirement).** `checkReservedNames` operates exclusively
on the checked AST/symbol tables — declaration names and `Name` reference nodes — never as a text/regex
scan over source or output. Consequences, all structural: a string literal containing `"Program"` is never
flagged; in interpolation the *hole* is a real `Name` node (checked naturally) while the surrounding chunk
text is an opaque payload (never seen); `extern("…")` raw FFI templates are the author's explicit escape
hatch — out of scope even though they land in output; comments are lexer trivia and never reach the AST
(verified), so the property is free there. The false-positive class is excluded by construction, not by a
maintained exclusion list.

**LSP:** squiggles before build, per *configured* target — the LSP layer parses pgconfig `targets` (it
needs them for `polyglot/targets` anyway) + `forbiddenIdentifiers`, and runs `checkReservedNames` once per
resolved backend over the already-analyzed unit; no client change.

**Slices (folded into PLAN §P19):** (i) *hygiene first* — collision-aware `fresh` + the `tag`/`equals`/
`self`/`_pg_idiv` guards (silent wrong output, one case invisible to all gates); (ii) `checkReservedNames`
+ the C# `Main`/`Program`/`System` refusals + pgconfig `forbiddenIdentifiers` (CLI+LSP); (iii) the
`identifiers` manifest block rides the existing builtin-catalog slice (`ident` gains decl-name coverage and
TS gets `escape: null` semantics); (iv) refusal unit tests via a StubBackend-style reserved set — these are
compile errors, so they live in the diagnostics suite, not the byte-diff gate.

## 8. Design B, revisited and re-rejected

The expr front stress-tested the rejected "strategy vocabulary" specifically for `Match` (the hardest
node). Finding: all three match shapes — C# switch-expression, TS IIFE if-chain, Python lambda ternary-fold
— are "iterate arms, wrap the same emitted body differently", expressible in Design A as `map`/`fold` +
per-arm pattern `case`. No `MatchOp{style}` enum is needed; a hypothetical match idiom that is *not*
fold/map-over-arms could not be constructed, and if one existed it would degrade to a §3.E refusal — the
same boundary Design B would hit. `fold` exists *because* Python's working `matchChain` demands it —
extracted, not guessed. The rejection stands, now with empirical backing.
