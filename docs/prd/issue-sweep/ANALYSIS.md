> **Provenance:** synthesis of a 13-agent issue-sweep investigation (2026-07-20) over all open issues
> #38–#57, clustered by root cause, each cluster skeptic-verified against current code. The
> operators-members cluster (#40, #42, #49, #53) is appended below from a follow-up agent (the
> original cluster hit the structured-output retry cap). This is the raw analysis; PRD.md + PLAN.md
> distil it. NOTE: there is no issue #45 (the synthesis mentions it in error; open set is #38–#44, #46–#57).

# Resolution Blueprint — Issues #38–#57 (one PR, branch off `e2e-coverage-wave2`)

> Synthesis of the five verified cluster investigations (match-unions, oo-closures, frontend-grammar, refusals-checker, php-and-catch). Every root cause below was verified against current code by the cluster editors; corrections they flagged are folded in as **[C]** notes and are binding on the plan author.

## 0. Coverage gap — read before planning
The brief names "19 open issues (#38–#57)". The findings actually blueprint **15 issue numbers**: #38, #39 (a–e), #41, #43, #44, #46, #47, #48, #50, #51, #52, #54, #55, #56 (a/b), #57.

**Not covered by any finding — need their own investigation before this PR can claim to close #38–#57:**
- **#40** (operator/indexer `set`) — referenced only as a *dependency* of #39c and as the driver of the `indexer_grid.pg` un-narrow. No root cause was produced. **It must be investigated and sliced with #39c (shared setter machinery).**
- **#42, #45, #49** — absent from all findings.
- **#53** — the brief lists it as a scope-decision issue ("#38, #53, #56-part"), but no finding characterizes it. **Flagged as an unresolved scope call.**

Treat #40/#42/#45/#49/#53 as **TODO(investigate)** rows; the slices below close the 15 verified issues.

---

## 1. Scope decisions (gate everything downstream)

| Issue | Decision needed | Recommended call | One-line rationale |
|---|---|---|---|
| **#38** `is` / typed match | 3.A implement vs 3.B refuse | **IMPLEMENT** for concrete class/union types; **REFUSE** interface type-tests on TS | PR #37 promised it as follow-up; interfaces erase on TS so an interface test is a silent §3.B mis-narrow — refuse it (or require a nominal marker). Record boundary in SPEC §3. **[C] scope call must be ratified before coding.** |
| **#46** loop-var capture | 3.A/§6.4 — per-iteration vs shared (SPEC silent) | **IMPLEMENT per-iteration** binding, amend SPEC §6.4/§7 | Least surprising, matches C# `foreach` + JS `let`, and the IR already commits to it (ir.hpp P25 §4.18). **[C] the SPEC amendment must actually land** or per-iteration is an undocumented behavior change. |
| **#39e** `import * as` (namespace) | 3.A implement vs 3.B refuse | **DECIDE FIRST** — recommend implement a synthesized module-namespace symbol; else refuse `import * as` and delete from grammar/SPEC | **[C] cannot be left "parsing-but-silently-unresolved".** The `import_namespace.pg` acceptance presupposes *implement*; if refuse is chosen, drop that program. Renaming imports (`x as flat`) is a sound independent implement regardless. |
| **#51** expression-position block arms | bounded 3.B fallback | Implement statement-position block arms for real; **fallback REFUSE** expr-position block arms only if Python-lambda/PHP-arrow hoisting proves intractable, recorded in SPEC | Legitimate §3.B call, not a silent relax. |
| **#57** union `==` | implement vs refuse (issue offered refuse) | **IMPLEMENT** structural equality (§3.C) | C#/Python already give structural union `==`; refusing on TS/PHP would be an inconsistent regression. Document in SPEC §9. |
| **#55** PHP builtin-name collision | 3.A-via-mangle vs 3.B-refuse | **IMPLEMENT** function-position suffix-mangle | Matches the other three targets; refuse-via-`reservedNames` is the wrong grain (rejects legal params/fields named `count`). |
| **#56a** scalar-receiver silent emit | (settled) | **REFUSE out loud** — this IS the required §3.B behavior | Not a scope call; silent verbatim emit is the miscompile. |
| **#54** finalizer | (settled §3.B row) | **REFUSE** with targeted diagnostic | Only supplies the missing message for an already-settled refusal. |
| **#53** | **UNKNOWN** | — | No finding. Must be investigated before it can be scoped. |

---

## 2. Root-cause groups (by code touched)

Collapsed to the minimal shared-machinery edits. Group letter drives slice ordering.

**Group A — Pattern/match lowering** · `lower.cpp::pattern()` (295–323) + `matchExpr()` (333–350) · `ir::Pattern`/`MatchArm`/`Match` (ir.hpp 203–229) · `emitter_base.cpp` Match ctx (161–193, 317–328) · all four `rules.Match` (+ `pyArm*`/`phpArm*`).
→ **#43** (extend Pattern IR with ordered literal sub-slots), **#52** (statement-form match), **#51** (block-arm bodies), and **enables #38** (typed-match arms reuse this). *One shared pattern-render helper; do #43 → #52 → #51.*

**Group B — Binary union flag** · `lower.cpp` Binary (512–527) · `ir::Binary` (add `lhsIsUnion`, ir.hpp 99–100) · `emitter_base.cpp` Binary ctx (43–44) · TS + PHP `rules.Binary`.
→ **#57**.

**Group C — Method modifier/iterator plumbing** · `ir::Method` (add `isVirtual`/`isOverride`/`isIterator`, ir.hpp 423–437) · `lower.cpp::method()` (446–473) · `emitter_base.cpp` `MethodDeclCtx::get` (700–736) · C# `csMethodSig` (#47) · TS `tsMethodSig` (#50). **Land the struct/lower/ctx surface once.**
→ **#47**, **#50**.

**Group D — Loop-var capture wiring** · `ir::For` `captured`/`needsCell` (already stamped) + `Capture.fromLoopVar` · `capture_analysis.cpp` · `emitter_base.cpp` StmtCtx For-arm (1115–1126, expose `captured`/`needsCell` **and a synthesized driver ident**) · C# range-for arm · Python Lambda arm.
→ **#46**. **[C] JSON engine cannot gensym — StmtCtx must expose a fresh driver name or do the `var <binding>=<driver>;` injection in C++.**

**Group E — PHP ctor/super** · `ir::Class.baseHasInit` (transitive) + `lower.cpp` lowerClass + `ClassDeclCtx` + PHP ctor template.
→ **#48**. **[C] use the surgical transitive `baseHasInit` gate, NOT the bare-empty-ctor approach — the latter interrupts PHP's implicit ctor chain-up and silently drops grandparent field initializers.**

**Group F — Front-end parser statements** · `parser.cpp` (+ `ast.hpp`/`ir.hpp`/`lower.cpp`/`emitter_base.cpp` as noted).
→ **#39a** do-while, **#39b** let-tuple, **#39c** accessor blocks (with #40), **#39d** guard-lambda suppression (parser-only).

**Group G — Import resolution** · `compiler.cpp` merge/link (289–350) · `sema.cpp` name resolution · `ast.hpp`.
→ **#39e**.

**Group H — C# scope-legalization pass** · new target-gated pass in `lower.cpp` (alongside capture_analysis/hoist_block_lambdas).
→ **#41**.

**Group I — Sema refusal/inference** · `sema.cpp` `checkCall()` (1571, 1602–1617) + `checkMember()` (1541–1550) · `parser.cpp` (finalizer, separate stage) · unit tests.
→ **#54** (parser refusal), **#56a** (concrete-receiver refuse), **#56b** (bidirectional lambda-param inference; depends #56a).

**Group J — PHP builtin mangle** · PHP manifest `identifiers`/`Call`/`phpFnSig` · optionally `backend_spec.hpp`/`backend_spec_json.cpp`/`emitter_base.cpp`/`backend.cpp` for the `identFn` primitive.
→ **#55**.

**Group K — Python/PHP try-guard rewrite** · Python + PHP `TryStmt` templates (mirror TS `__handled` if-chain) + `identifiers.reserved` (`__handled`).
→ **#44**. *No Core change — StmtCtx already exposes `catchesHaveGuard`/`hasCatchAll`/per-arm reads.*

---

## 3. Ordered slices (~one commit each)

Dependency-sorted: cheap independent single-target wins first, then shared machinery, then the scope-gated/large items. **⚑ = carries a scope decision (§1).**

| # | Slice | Closes | Group | Adds / un-narrows | Size |
|---|---|---|---|---|---|
| 1 | Finalizer refusal (parser arm + brace-skip, mirror lock/unsafe 613–633) | #54 | I | P6 `refuses()` row | S |
| 2 | PHP builtin-name mangle (function-position only, both sites) | #55 ⚑ | J | new `php_builtin_collision.pg` (or un-rename `disposal_throw.pg`) | M |
| 3 | PHP super → transitive `baseHasInit` gate | #48 | E | new `super_implicit_base.pg` **+ 3-level middle-class fixture** | S |
| 4 | Python/PHP try-guard `__handled` if-chain | #44 | K | **un-narrow `catch_when.pg`** (+ 2nd same-type arm) | M |
| 5 | Union structural `==` (`lhsIsUnion` + TS/PHP helper) | #57 ⚑ | B | new `union_eq.pg` + golden | M |
| 6 | Method virtual/override plumbing + C# emit | #47 | C | new `virtual_dispatch.pg` | M |
| 7 | Member iterator `*` (reuse Group-C plumbing) | #50 | C | new `iterator_compose.pg` | S |
| 8 | Pattern IR: ordered literal ctor sub-slots + 4 Match arms | #43 | A | **un-narrow `match_nested.pg`** + golden | M |
| 9 | Statement-form match (reuse #43 pattern render) | #52 | A | new statement-position program + golden | M |
| 10 | Block-arm bodies (stmt-pos + expr-pos hoist) | #51 ⚑ | A | extend `union_tree.pg` block arm + golden | L |
| 11 | Guard-context bare-lambda suppression (parser flag) | #39d | F | **un-narrow `match_nested.pg`** (co-lands with #8) | S |
| 12 | `do…while` (StmtKind + Python while-true w/ continue re-test) | #39a | F | new `do_while.pg` (or extend `loop_control.pg`) | M |
| 13 | `let (a,b)=t` (parseLet → tuple pattern only; refuse other pattern kinds) | #39b | F | **un-narrow `tuples.pg`** | M |
| 14 | **#40 investigate** + operator/property `set` shared machinery + accessor blocks | #40, #39c | F | new `prop_accessors.pg`; **un-narrow `indexer_grid.pg`** (#40) | L |
| 15 | Import resolution: alias table + (⚑ decision) namespace symbol | #39e ⚑ | G | `import_alias.pg` (+ `import_namespace.pg` iff implement) | L |
| 16 | C# scope-legalization pass (rename colliding loop binder) | #41 | H | **un-narrow `loop_control.pg`** (k→i) | M |
| 17 | Scalar/concrete-receiver refusal | #56a | I | checker unit test (+ negative w/ import) | M |
| 18 | Bidirectional lambda-param inference | #56b | I | **un-narrow `extension_generic.pg`** (+ add `import "std.strings"`) | L |
| 19 | Per-iteration loop-var capture + SPEC §6.4 amend | #46 ⚑ | D | new `closure_loop_capture.pg` | L |
| 20 | `is` / typed-match narrowing (concrete only, TS interface refuse) | #38 ⚑ | A+ | new hierarchy program + golden + refusal unit test (un-narrow `sema.cpp:1109`) | L |

**Ordering constraints (must hold):**
- #43 before #52 before #51 (shared pattern-render helper; #51 needs the statement-form from #52).
- #47 before #50 only as *soft* same-function ordering (both edit `method()`/`MethodDeclCtx`); no logic dependency.
- #56a before #56b (shared `checkCall` region; #56a must keep Unknown/generic receivers lenient so #56b's still-Unknown param isn't wrongly refused).
- #38 after the Group-A pattern machinery (#43/#51) — it reuses typed-pattern lowering.
- #41 after #39b (its collision walk must see let-tuple binders) — **[C]** coordinate binder-set if #39b co-lands.
- #14: **#40 must be investigated first** and its setter emission built as *one* shared pairing helper in the C# `MethodDecl` arm (property branch ~1201–1234 + operator-get branch ~1236) — not two hand-rolled setter paths.

---

## 4. Acceptance-test matrix

| Issue | Test | Type | Un-narrow? |
|---|---|---|---|
| #43 | `match_nested.pg` (restore `Leaf(0)`/`Pair(a,0)` literal-sub arms) + golden | conformance | ✅ un-narrow (narrowed for #43) |
| #52 | new statement-position program (void `print` arms); csc oracle proves CS0201/CS0029 gone | conformance | new |
| #51 | `union_tree.pg` + block-bodied arm + golden (golden load-bearing — pairwise diff can't catch identical-wrongness) | conformance | extend |
| #57 | `union_eq.pg` (`Full(1)==Full(1)`→1, `Full(1)==Full(2)`→0, `Empty==Empty`) + golden | conformance | new |
| #38 | `is`/typed-match on class hierarchy (C# decl pattern / TS+PHP `instanceof` / Py `isinstance`) + golden + refusal unit test | conformance + unit | new (un-narrows `sema.cpp:1109`) |
| #47 | `virtual_dispatch.pg` → all four print `9 90`; csc proves 90 not 0 | conformance | new |
| #50 | `iterator_compose.pg` → all four print `10`; TS must node-execute | conformance | new |
| #46 | `closure_loop_capture.pg` → all four print `1 11 21` | conformance | new |
| #48 | `super_implicit_base.pg` (2-level) **+ 3-level middle-class-without-init fixture** (pins the chain-up regression) | conformance | new |
| #39a | `do_while.pg` (counter + `continue` + at-end `break`) — pins Python continue-re-tests-condition | conformance | new (or extend `loop_control.pg`) |
| #39b | `tuples.pg` (`let (a,b)=swap((3,9))`) | conformance | ✅ un-narrow |
| #39c | `prop_accessors.pg` (SPEC diameter get/set) | conformance | new |
| #39d | `match_nested.pg` ctor-arm guard `Pair(a,b) if a==b =>` (co-lands #43) | conformance | ✅ un-narrow |
| #39e | `import_alias.pg` (+ `import_namespace.pg` iff implement) | conformance | new |
| #41 | `loop_control.pg` (rename later local `k`→`i`); csc oracle pins CS0136 | conformance | ✅ un-narrow |
| #54 | P6 `refuses("class Res { ~Res() {} }", "finalizer", …)` | unit | new |
| #56a | program w/ `s.codePointAt(0)` **no** `import "std.strings"` must diagnose; + negative (with import resolves) | unit | new |
| #56b | `extension_generic.pg` + `ss.totalWith((s)=>s.len())` **+ add `import "std.strings"`** | conformance | ✅ un-narrow |
| #55 | `php_builtin_collision.pg` (`fn explode()`) — PHP leg must transpile AND run | conformance | new |
| #44 | `catch_when.pg` (+ 2nd same-type `catch(e: AppError)` after guarded, code==2 falls to it) | conformance | ✅ un-narrow |
| #40 | `indexer_grid.pg` | conformance | ✅ un-narrow *(pending #40 investigation)* |

**Explicitly-marked un-narrows:** `indexer_grid` (#40), `match_nested` (#43+#39d), `loop_control` (#41+#39a), `catch_when` (#44), `extension_generic` (#56b), plus `tuples` (#39b).

**Golden (.expected) files are load-bearing, not optional, for #43/#51/#57** — the defect makes all four targets *identically* wrong, so the pairwise C#-oracle diff false-passes; only a `<name>.expected` golden (run-conformance.ps1 compares every target incl. oracle) catches it.

---

## 5. Risk & cross-target matrix

**Multi-plugin fixes (edit ≥2 manifests):** #43/#52/#51 (all 4 Match arms), #39b/#39c (all 4), #57 (TS+PHP Binary), #44 (Python+PHP TryStmt), #39a (3 C-style + Python lowering), #56a/#56b (sema — hits every target via oracle).
**Single-target:** #47 (C# only), #50 (TS only), #48/#55 (PHP only), #41/#40-C# (C# only), #46 (C# + Python; TS/PHP already correct), #54/#39d (parser, no plugin).

**Where fixing one target could regress another:**
- **#43/#51/#52 pattern IR shape** — a mis-ordered slot list breaks *existing* binding-only ctor patterns (`unions.pg`, `union_tree.pg`, `generic_union.pg`). C# positional literals must interleave `var x` and literals in declaration order or arity breaks.
- **#48** — **[C]** bare-empty-ctor interrupts PHP implicit chain-up → grandparent field initializers silently skipped (a §3-forbidden silent miscompile the 2-level test won't catch). Use transitive `baseHasInit`; if synth-ctor route is kept, it must `parent::__construct()` when extending a base *class* (distinguish from interfaces).
- **#46** — C# driver-rename must be gated strictly on `stmt.captured` so non-capturing loops stay byte-identical to existing goldens; Python default-arg shim must reuse the loop var's own name and apply **only** to the snapshot (non-`needsCell`) case, else a body-reassigned loop var snapshots wrongly.
- **#44** — broad `except Exception`/`catch(\Throwable)` changes propagation **unless** the trailing re-raise is gated exactly on `!hasCatchAll`; a bug there swallows unmatched types.
- **#55** — must not over-mangle methods/params/locals/class names (legal PHP); decl and call sites must stay byte-identical (Call uses `node.mangledCallee`, phpFnSig uses `decl.emitName` — apply `identFn` to the same base at both).
- **#41** — C#-only; must rewrite *all* references incl. closure captures, avoid fresh-name re-collision, and not rename legal sibling-scope reuse.
- **#56b** — reorders the hot `checkCall` path (overload/local/result-inference); must not double-check args or change arg-count diagnostics.

**What the differential suite already guards:** run-conformance.ps1 executes all four runtimes incl. **PHP and Python** (pins #44/#55/#48 at runtime); the csc oracle proves C# *compiles* (#52/#41/#47/#56a); the G32 `.expected` golden mechanism (compares every target to `<name>.expected`) is the only guard against identical-wrongness (#43/#51/#57).

**Files touched by ≥2 slices (coordination points):** `ir.hpp` (#43/#51/#52/#57/#46/#47/#50 — disjoint structs), `lower.cpp` (Groups A/B/C/D/F/H), `emitter_base.cpp` (disjoint ctx: Match vs Binary vs MethodDeclCtx vs StmtCtx), `sema.cpp` (#56a/#56b share `checkCall` 1602–1617; #38/#39b/#39c/#39e), `parser.cpp` (#54/#39a-d), PHP manifest (#55/#48/#44 — disjoint arms).

---

## 6. Rough size & PR shape

| Group | Slices | Size |
|---|---|---|
| A pattern/match | #43 M, #52 M, #51 L, #38 L | **L** |
| B union eq | #57 M | S–M |
| C method plumbing | #47 M, #50 S | M |
| D loop capture | #46 L | L |
| E PHP super | #48 S | S |
| F parser | #39a M, #39b M, #39c+#40 L, #39d S | **L** |
| G imports | #39e L | L |
| H C# scope | #41 M | M |
| I sema/refusal | #54 S, #56a M, #56b L | M–L |
| J PHP mangle | #55 M | M |
| K try-guard | #44 M | M |

**Verdict on PR shape:** This is a genuinely **large but cohesive** single PR (20 slices, ~15 verified issues + #40 to investigate). It matches the user's one-PR-per-task preference and the slices are individually reviewable commits sharing deliberate machinery (pattern IR, method plumbing, sema inference). **Recommend one PR, acknowledged large**, with the slice order above and the **full gate run once at the very end** (per CLAUDE.md — the ~15-min gate must not run per slice; unit-exe run is the mid-flight ceiling). Because Groups A/F touch platform-neutral IR + all four backends, the end gate must include **one POSIX compile+unit leg** (the pre-merge `ci.yml` ubuntu floor).

**Blockers to clear before coding starts:** ratify the four ⚑ scope calls (#38, #46, #39e, #51-fallback), and **investigate #40** (drives slice 14 and the `indexer_grid` un-narrow). #42/#45/#49/#53 are out of this blueprint's evidence base and need separate characterization before the PR can claim the full #38–#57 range.
---

# Operators/members cluster — follow-up investigation (#40, #42, #49, #53)

*Fills the synthesis's coverage gap. All root causes verified against current code.*

**Cluster summary:** all four live at the member-declaration / operator-emission boundary; three share
one shape — a per-member emission model that can't see/pair/count things spanning a member's siblings or
args. **#49 and #57 are ONE equality model (co-implement). #40 and #42 are the indexer path (#42 first).
#53 stands alone** (scope decision + parallels the existing top-level-overload machinery).

## #42 — multi-arg index read drops all but first arg
- **Root cause:** AST carries every arg (`ast.hpp:122`), but `ir::Index` (`ir.hpp:148-155`) has ONE
  `ExprPtr index`; `lower.cpp:626-628` builds from `e.args[0]` only; `emitter_base.cpp:283-287` exposes
  only `node.index`. sema never arity-checks (`sema.cpp:1309`, `elementType` `:1246-1262` finds indexer
  by name only).
- **Fix:** `Index::index` → `std::vector<ExprPtr> indices`; map all args in lowering; expose
  `node.indices.<i>` (mirror `node.elements.<i>` `:294-303`); 4 manifests map over indices (C# `recv[a,b]`,
  TS/Py/PHP `recv.get(a,b)`); sema arity-check vs the `get` operator params.
- **Files:** ir.hpp, lower.cpp, emitter_base.cpp, sema.cpp, all four Index rules.
- **Acceptance:** un-narrow `indexer_grid.pg` to 2-D `operator fn get(x,y)`. **Effort M**, risk low-med
  (single-arg `List[i]` is the hot path — vector must degrade to one element). **Depends: land with #40.**

## #40 — `operator fn set` emits invalid C#
- **Root cause (two halves):** *Decl:* `operatorSymbol("set")`→`"set"` (`lower.cpp:378-392`); the C#
  `MethodDecl` rule special-cases only `opSymbol=="get"`→`csIndexerSig` (`csharp:1236-1305`), else the
  generic operator arm → `public static void operator set(...)`. No get+set pairing exists. *Call site:*
  index-write `r[i]=v` unhandled — `ir::Assign` is hardcoded (`emitter_base.cpp:1506-1509`); on TS/Py/PHP
  `emitExpr(target)`→`recv.get(i)`→`recv.get(i)=v` (invalid). (Why programs use `put()` today.)
- **Fix:** (1) merge get/set at lowering into one indexer (add `setParam`/`setBody` to the get Method or a
  small `ir::Indexer`); C# renders `public T this[...]{get=>…;set=>…}`, TS/Py/PHP keep two methods.
  (2) index-write: C# native `this[i]=v`; TS/Py/PHP lower a user-indexer index-assign to a `set(i,v)`
  MethodCall (mirrors the read side's `receiverHasIndexer` split); carries #42's multi-arg vector.
- **Files:** ir.hpp, lower.cpp, emitter_base.cpp (Assign/index-write), csharp (merged accessor + set arm),
  TS/Py/PHP set templates. **Effort M-L**, risk med. **Depends: #42.** **Shares the C# `{get;set;}`
  accessor-block emission with #39c** (co-design the template; keep separate slices — #39c is a parser gap).
- **Acceptance:** un-narrowed `indexer_grid.pg` + `operator fn set` + `r[2]=41` write-then-read.

## #49 — `operator fn eq`: C# CS0111 + TS routes `==` to structural equals
- **Root cause:** *C#:* `operatorSymbol("eq")`→`"=="`→`public static bool operator ==` collides with the
  record's synthesized `==` (records/unions emit as C# `record`) → CS0111. *TS:* the `Binary` rule
  (`typescript:3135-3246`) checks case1 `op∈{==,!=}` AND `lhsIsRecord`→structural `.equals(...)`
  UNCONDITIONALLY, case2 remaining `==`→`l===r`; the user-eq opMethod arm (case3, table `"==":"eq"`) is
  UNREACHABLE. So user `eq` emits but never runs (TS 0/0); Python `__eq__` dispatches (1/0).
- **Fix (UNIFIED equality model — same slice as #57):** stamp `lhsHasUserEq` on `ir::Binary` (scan like
  `lower.cpp:133-142`). Resolution priority for `==`/`!=`: (1) user eq (TS/Py/PHP `.eq(...)`; **C#: emit
  user eq as `public override bool Equals(T)` on the record so the synthesized `==` calls it — resolves
  CS0111**); (2) else structural for records AND unions (#57 plugs in here); (3) else primitive/reference.
  Reorder TS + PHP Binary cases so userEq precedes lhsIsRecord/primitive.
- **Files:** ir.hpp (Binary flag), lower.cpp (stamp + user-eq type set), emitter_base.cpp
  (`node.lhsHasUserEq`), csharp (record→Equals, suppress `operator ==`), typescript+php (Binary arms).
  Python already correct. **Effort M**, risk med-high (every `==` on a user type flows here — regression
  surface equality.pg, equality_composite.pg, class_identity_eq.pg). **Pair with a `.expected` golden.**
- **Acceptance:** new `operator_eq.pg` (Money mod-100) → `1` then `0`; PHP refuses (operatorOverloading:false).
- **KEY DETERMINATION:** #49, the record's synthesized structural `==`, and #57 are ONE equality model —
  structural is the default for aggregates; a user `operator fn eq` overrides it. Doing #49 without #57
  leaves union `==` reference-wrong on TS; doing either without the shared model risks a second collision.

## #53 — member overloading: only first same-named member registers
- **Root cause:** `findMember` (`sema.cpp:649-657`) returns the FIRST name match; the member-call check
  (`sema.cpp:1614`) runs `checkArgs` against it → `add(a,b)` hits `add(a)` → "expects 1 arg, got 2". The
  overload machinery ALREADY EXISTS for top-level fns (`fns_` `:376`, `resolveOverload` `:277`, `mangleFn`
  `:264`, `Call.mangledCallee`, `Function.mangledName`); there's even `gatherMethods` (`:684-691`) and an
  UNUSED `ir::Method::mangledName` (`:400`). Missing on the member side: overload resolution at the call +
  a per-target mangled name on `ir::MethodCall` (`ir.hpp:120-127` has only `method`).
- **SCOPE DECISION → RECOMMEND IMPLEMENT:** §3.A lists overloading as supported; top-level ships
  (`overloading.pg`); the resolve→mangle machinery is built and proven; a member carve-out is an
  inconsistent gap and the call-site arity error is user-hostile. It's a parallel of an existing path.
- **Fix:** (1) sema: per-type member overload sets; gather same-named methods (reuse `gatherMethods`),
  `resolveOverload` by arg types, stamp the resolved mangled name; duplicate-signature error at the
  DECLARATION site. (2) mangle member names on TS/Py/PHP (populate `ir::Method::mangledName`); C# keeps
  source name. (3) add `MethodCall::mangledMethod`; (4) TS/Py/PHP MethodCall rule emits mangled name.
- **Files:** sema.cpp, ir.hpp, lower.cpp, emitter_base.cpp, TS/Py/PHP MethodCall rules. **Effort L**, risk
  med (mangling must not collide with fields/properties/extension-methods; static + inherited overloads).
- **Acceptance:** new `method_overloads.pg` (Calc.add/1 + add/2) + declaration-site duplicate-signature unit test.

## Recommended slicing (folds into the sweep)
- **Indexers:** #42 (`Index.indices` vector + arity check + 4 manifests) then #40 (get/set merge + index-write). One un-narrowed `indexer_grid.pg`.
- **Equality:** #49 + #57 as ONE `lhsHasUserEq`+union-structural model; reordered TS/PHP Binary ladders; C# record→`Equals`. New `operator_eq.pg` + `union_eq.pg`, both with goldens.
- **Member overloading:** #53 parallels the top-level path; new `method_overloads.pg` + declaration-site refusal unit test.

`operator_eq.pg`, `method_overloads.pg`, `union_eq.pg` do not exist yet (net-new). #43/#51/#52 do NOT share this cluster's machinery (they're match-arm lowering).
