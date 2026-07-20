# Open-issue sweep — implementation plan

Companion to [PRD.md](./PRD.md); per-issue root-cause evidence (verified against current code, with exact
file:line refs) in [ANALYSIS.md](./ANALYSIS.md). **One branch, one PR**, acknowledged large-but-cohesive.

**Before coding: ratify the four ⚑ scope decisions** (PRD §2: #38, #46, #39e, #51-fallback; plus the
already-recommended #53/#57/#55 implements). Discipline (CLAUDE.md): implement all slices, cheap
unit-exe or `-Tier fast` mid-flight only, **full gate once at the end + one POSIX leg** (Groups A/G touch
platform-neutral IR + all four backends). `.expected` goldens are load-bearing for #43/#51/#49/#57
(identical-wrongness defeats the pairwise diff).

## Slices (dependency-ordered: cheap independent single-target wins → shared machinery → scope-gated/large)

| # | Slice | Closes | Adds / un-narrows | Size |
|---|---|---|---|---|
| 1 | **Finalizer refusal** — parser arm recognizes `~Name()` + brace-skip, routes to the §3.B refuses() table (mirror lock/unsafe at parser.cpp:613-633) | #54 | P6 `refuses()` unit-test row | S |
| 2 | **PHP builtin-name mangle** — function-position suffix-mangle vs the PHP builtin list (php manifest identifiers/Call/fnSig; decl+call sites byte-identical) | #55 | new `php_builtin_collision.pg` (`fn explode()` transpiles+runs); may un-rename `disposal_throw.pg` | M |
| 3 | **PHP super → transitive `baseHasInit` gate** — emit `parent::__construct()` only when a base *class* declares init (NOT the bare-empty-ctor route — it breaks PHP chain-up, silently dropping grandparent field inits) | #48 | new `super_implicit_base.pg` **+ a 3-level middle-class-without-init fixture** | S |
| 4 | **Py/PHP try-guard `__handled` if-chain** — mirror the TS guard emulation; trailing re-raise gated exactly on `!hasCatchAll` (add `__handled` to identifiers.reserved) | #44 | **un-narrow `catch_when.pg`** (+ 2nd same-type catch arm after the guarded one) | M |
| 5 | **Method modifier/iterator plumbing + C# virtual/override** — `ir::Method` gets isVirtual/isOverride/isIterator; lower + `MethodDeclCtx`; C# `open`→`virtual`, `override`→`override` | #47 | new `virtual_dispatch.pg` → all four print `9 90` (csc proves 90 not 0) | M |
| 6 | **TS member generator `*`** (reuse slice-5 plumbing: isIterator → `*method()`) | #50 | new `iterator_compose.pg` → all four print `10`; TS must node-execute | S |
| 7 | **Indexer multi-arg read** — `ir::Index.index`→`indices` vector; lower maps all args; expose `node.indices.<i>`; 4 Index rules; sema arity-check | #42 | (part of the `indexer_grid` un-narrow) | M |
| 8 | **Indexer get/set merge + index-write** — fold get+set into one C# `this[...]{get;set;}`; index-write `r[i]=v` → C# native / TS·Py·PHP `set(i,v)` (carries the slice-7 vector) | #40 | **un-narrow `indexer_grid.pg`** (2-D get/set + `r[2]=41` write-then-read) | M-L |
| 9 | **Unified equality model (#49 + #57 together)** — `lhsHasUserEq` + union-structural facts on `ir::Binary`; reorder TS+PHP Binary case ladder (userEq → structural(records+unions) → primitive); C# emits user `eq` as `override Equals` (resolves the CS0111 collision) | #49, #57 | new `operator_eq.pg` (Money mod-100 → `1 0`) + `union_eq.pg`, **both with `.expected` goldens** | M |
| 10 | **Pattern IR: ordered literal ctor sub-slots** — extend `ir::Pattern` so `Leaf(0)`/`Pair(a,0)` interleave literals + binders in declaration order; all four Match arms; C# positional-pattern literals | #43 | **un-narrow `match_nested.pg`** + golden | M |
| 11 | **Ctor-pattern guards** — parser suppresses bare-lambda misparse of `Pair(a,b) if …` (parser-only; co-lands slice 10) | #39d | **un-narrow `match_nested.pg`** (guard arm) | S |
| 12 | **Statement-form match** — reuse slice-10 pattern render; C# emits a switch *statement* (not expression) for void arms | #52 | new statement-position program + golden (csc proves CS0201/CS0029 gone) | M |
| 13 | **Block-bodied match arms** ⚑ — statement-position for real; expr-position via IIFE/lambda hoist, or bounded refuse per §2 | #51 | extend `union_tree.pg` with a block arm + **golden** | L |
| 14 | **`do…while`** — new StmtKind; Python lowering (while-true with a continue that re-tests the condition) | #39a | new `do_while.pg` (or extend `loop_control.pg`) | M |
| 15 | **`let (a,b) = t` tuple destructuring** — parseLet → tuple pattern only (refuse other pattern kinds for now) | #39b | **un-narrow `tuples.pg`** | M |
| 16 | **Property accessor blocks `set(v){}`** — parser gap (parser.cpp:270-285 only accepts `=>`-getter/field); co-design the C# `{get;set;}` template with slice 8 | #39c | new `prop_accessors.pg` (SPEC diameter get/set) | M-L |
| 17 | **Member overloading** — per-type overload sets; resolve→mangle (parallel the top-level fn path); duplicate-signature error at the declaration site; `MethodCall::mangledMethod`; TS/Py/PHP emit mangled name, C# keeps source name | #53 | new `method_overloads.pg` + declaration-site duplicate-signature unit test | L |
| 18 | **C# scope-legalization pass** — new target-gated lower pass renames a later local colliding with a for-binding (after slice 15 so it sees let-tuple binders) | #41 | **un-narrow `loop_control.pg`** (k→i; csc pins CS0136) | M |
| 19 | **Unresolvable-member refusal** — sema refuses a member call binding nothing on a concrete/primitive/std receiver (keep Unknown/generic receivers lenient for slice 20) | #56a | checker unit test (+ negative: with import it resolves) | M |
| 20 | **Bidirectional lambda-param inference** — instantiate generic lambda param types before body-checking (after slice 19) | #56b | **un-narrow `extension_generic.pg`** (+ add `import "std.strings"`, `s.len()`) | L |
| 21 | **Per-iteration loop-var capture** ⚑ — C# copy-into-loop-local (gated strictly on `stmt.captured`), Python default-arg snapshot (non-`needsCell` only); amend SPEC §6.4 | #46 | new `closure_loop_capture.pg` → all four print `1 11 21` | L |
| 22 | **`is` / typed-match narrowing** ⚑ — concrete class/union only (C# decl pattern / TS·PHP instanceof / Py isinstance); **refuse** interface type-tests on TS; reuse Group-A pattern lowering (after slices 10/13) | #38 | new class-hierarchy program + golden + refusal unit test (un-narrows sema.cpp:1109) | L |
| 23 | **`import * as` / renaming imports** ⚑ — renaming (`{ area as flat }`) is a sound implement; namespace (`* as geo`) per the §2 decision (implement a synthesized namespace symbol, or refuse + delete from grammar) | #39e | `import_alias.pg` (+ `import_namespace.pg` iff implement) | L |

**Hard ordering constraints:** #43→#52→#51 (shared pattern-render; #51 needs #52's statement form) ·
#42→#40 (indexer vector) · #49+#splitsame slice as #57 · #47 before #50 (soft, same functions) ·
#56a before #56b (shared checkCall region; #56a must stay lenient on still-Unknown params) · #38 after
#43/#51 (reuses typed-pattern lowering) · #41 after #39b (its collision walk must see let-tuple binders).

## Acceptance-test matrix (issue → test)

New conformance programs: `php_builtin_collision` (#55), `super_implicit_base` +3-level fixture (#48),
`virtual_dispatch` (#47), `iterator_compose` (#50), `operator_eq` (#49), `union_eq` (#57), a
statement-position match program (#52), `do_while` (#39a), `prop_accessors` (#39c), `method_overloads`
(#53), `closure_loop_capture` (#46), a typed-match hierarchy program (#38), `import_alias`
(+`import_namespace` iff #39e=implement).
Un-narrows: `indexer_grid` (#40+#42), `match_nested` (#43+#39d), `catch_when` (#44), `tuples` (#39b),
`loop_control` (#41), `extension_generic` (#56b).
Unit tests: finalizer refusal (#54), unresolvable-member refusal (#56a), member duplicate-signature (#53),
`is`-on-interface refusal (#38).
**Goldens (mandatory):** #43, #51, #49, #57 (identical-wrongness defeats the pairwise diff — only a
`<name>.expected`, compared against every target incl. the oracle by run-conformance.ps1, catches it).

## Risk & cross-target matrix

- **Pattern IR shape (#43/#51/#52)** — a mis-ordered slot list breaks existing binding-only ctor patterns
  (`unions.pg`, `union_tree.pg`, `generic_union.pg`); C# positional literals must interleave `var x` and
  literals in declaration order or arity breaks.
- **Equality model (#49/#57)** — every `==` on a user type flows through here; regression surface
  `equality.pg`, `equality_composite.pg`, `class_identity_eq.pg`. Golden-gated.
- **PHP super (#48)** — the bare-empty-ctor route silently drops grandparent field inits (a §3-forbidden
  miscompile the 2-level test won't catch) → use transitive `baseHasInit`; the 3-level fixture pins it.
- **Loop capture (#46)** — C# driver-rename gated strictly on `stmt.captured` (non-capturing loops stay
  byte-identical to existing goldens); Python snapshot shim only for the non-`needsCell` case.
- **Try-guard (#44)** — trailing re-raise gated exactly on `!hasCatchAll`, else unmatched types get swallowed.
- **PHP mangle (#55)** — must NOT over-mangle methods/params/locals/class names; decl + call sites apply
  the same transform to the same base.
- **Indexer (#42)** — single-arg `List[i]` is the hot path; the indices vector must degrade to one element.
- **Member overloading (#53)** — mangling must not collide with fields/properties/extension-methods;
  static + inherited-across-base overloads need the same resolution.
- **Single-target (lower blast radius):** #47/#41 (C#), #50 (TS), #48/#55 (PHP), #54/#39d (parser).
- **Guard:** run-conformance.ps1 runs all four runtimes incl. PHP+Python (pins #44/#55/#48 at runtime);
  the csc oracle proves C# compiles (#52/#41/#47/#56a); `.expected` goldens are the only guard against
  identical-wrongness (#43/#51/#49/#57).

## Post-fix hygiene (per acceptance criterion 4)

As each issue's slice lands green, comment on the GitHub issue linking this PR + naming the acceptance
test, and close it. Record each §2 scope decision in SPEC.md. Append the milestone log to
`docs/prd/PLAN.md` (house style) at the end.

---

## Log

*(empty — populated during implementation)*
