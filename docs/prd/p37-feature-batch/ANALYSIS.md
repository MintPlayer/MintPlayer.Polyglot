# P37 — code-grounded validation findings

Condensed from the 4-agent adversarial validation (val-operators / val-attributes / val-surface / val-scope)
+ recon helpers. File:line references are the current-master ground truth the implementation follows.
Overall validation verdict: **GO-WITH-CONDITIONS** (conditions are in PRD §F + the slice notes).

## A — `init`→`constructor` (val-surface: SAFE-WITH-CAVEATS)

- Ctor identity is a **flag** (`TokKind::KwInit` → `MemberKind::Init`), not the source string. The string
  `"init"` is *re-derived* from the flag at three sites that must flip in lockstep with the plugin keys:
  - `compiler.cpp:507` — `c.name + "." + (mem.kind==Init?"init":mem.name)` → std-binding lookup key.
  - `capability.cpp:215` — same ternary → bound-member registry key.
  - `capability.cpp:137` — `lhs->text + ".init"` → construction *use* key (compared against `:215`'s
    registry; if `:215`→`constructor` but `:137` stays `.init`, an unbound ctor use silently passes the
    anti-silent-drop gate — a §3 violation).
- Must-rename set: `lexer.cpp:18`, `token.hpp:30`, `parser.cpp:257-259`, `ast.hpp:197` (`MemberKind`) + 11
  `MemberKind::Init` usages (capability.cpp:215,291; compiler.cpp:507; lower.cpp:125,455; sema.cpp:584,763,
  913,931,935,941; pg_printer.cpp:167), the 3 derivation literals, `pg_printer.cpp:168`/`ir.cpp:69` printer
  literals, and 12 plugin skeleton keys (csharp 3284/3344/3348; ts 3971/4031/4035; python 3503/3563/3567;
  php 181/208/212).
- **Must-NOT-rename:** field-initializer `.init` (`stmt.init`/`item.init`/`m.init` across emitter_base +
  plugins) and the ctor *data* fields `hasInit`/`initParams`/`initBody`/`baseHasInit`/`hasSuper`/`superArgs`
  (ir.hpp:522-532 + `decl.*` template paths in all 4 plugins) — decoupled from the keyword; renaming adds a
  second sync surface for cosmetic gain only.
- Surface occurrences: 42 `init(` across 28 `.pg` files + 14 in `tests_main.cpp` + SPEC.md:161,202,278,
  grammar.ebnf:87, plugin-authoring.md:148, backend.hpp:89.
- **M4:** `constructor` is a live JS/TS prototype member — confirm skeleton keys stay internal (never emitted)
  + golden no bare `.constructor` access.

## B — `is`/`as` (val-surface: SAFE-WITH-CAVEATS)

- Reusable: `PatternKind::TypeTest` + `Pattern.testType`/`.binding` (ir.hpp:208,222), lowered at
  lower.cpp:354; the instanceof/isinstance/decl-pattern predicate already emitted by all 4 plugins (#38).
  `ExprKind::Cast` exists (numeric-only). Native nullable is a `bool nullable` flag on `TypeRef`
  (ast.hpp:37), distinct from `Option<T>`.
- `ir::If` (ir.hpp:299-304) has cond/thenBody/elseBody, **no binding slot** → lower prepends a narrowed
  `Let` to `thenBody` (B3 branch-scope becomes structural; zero IR change).
- **B4 correction:** the P36 #38 interface refusal (sema.cpp:1126) fires **unconditionally** — sema has no
  target awareness (`req_` carries only boundary counts). B4's "when TS is configured" is a *relaxation*:
  move the refusal (and the match-arm #38 check) into the target-aware capability pass so interface
  `is`/`as` is refused iff a configured target lacks runtime interface identity.
- **M1 (val-scope, CRITICAL):** `as` must emit a runtime guard (`instanceof ? x : null`) on TS/Python/PHP —
  never a bare TS `as` (compile-time assertion, no runtime check → silent miscompile: C# returns null on a
  bad cast, TS returns the mis-typed object).

## C — operators (val-operators: C5 PROBLEMATIC as written; C6/C7 sound-with-caveats)

- **Dispatch is NOT uniform today** (this refutes C5's premise): C# native static `operator +`
  (csharp:1195-1237; `insideOperator`→`lhs` rebind at lower.cpp:528-540 + This-rule csharp:704-718); Python
  native dunders (`pyDunder`, python:1919-2021); **only TS** rewrites to instance `a.plus(b)` (opMethod
  table typescript:82-93, Binary case :3613-3642, `hasOpMethod` at emitter_base.cpp:69-72); PHP refuses
  (`operatorOverloading:false`, php:14).
- **Forcing a separate static class regresses C#/Python:** a free `Ops.eq(a,b)` is not consulted by C#
  `Dictionary`/`HashSet`/`.Contains`/`.Distinct`/LINQ/record value semantics (all go through instance
  `Equals`/`GetHashCode`), nor by Python's dunder protocol. → keep native on C#/Python; only TS moves to
  static-on-type. The null rationale holds only for TS-**eq** (arithmetic should still throw on null to stay
  faithful).
- **Equality model to preserve (P36):** `==` routes to C# `override Equals`+synth record `==` (csharp:1682-
  1753); TS `.eq()`/`.equals()`/`JSON.stringify` (typescript:3386-3571); Python `__eq__`; PHP `->equals()`
  for records, refuse for user ops. Null guard `rhsIsNullLit` (lower.cpp:588) keeps `== null` a null test —
  **M3, must survive.** 9 conformance programs pin this (vec2, operators_full, operator_eq(.expected),
  indexer_grid, equality, equality_composite, union_eq(.expected), class_identity_eq, string_eq_numeric) —
  regeneration surface for the TS-shape change.
- Operator name vocab is duplicated in 3 places to keep in sync: sema.cpp:166-178, lower.cpp:421-434, and the
  TS `opMethod`/Python `pyDunder` tables. `operatorSymbol` currently stops at `neg`; no `band/bor/bxor`
  (#63). TS `Unary` (typescript:570-617) never routes user `neg` (#62).
- **C6:** capability map is exact-string `unordered_map<string,string>` (backend.cpp:64-66); no fallback,
  no hierarchy. `capability.cpp:92` blanket-marks `OperatorOverloading` for *any* operator member → must
  branch by `opSymbol`. Umbrella-fallback semantics must be defined; PHP flip from blanket-refuse to
  `:eq`/`:indexers` support is a ratified behavior change.
- **C7:** `a += b` stays `ir::Assign` op `+=` (lower.cpp:757-770); mis-emits on TS/Python for user types →
  rewrite to `a = <op>(a,b)`, hoisting the lvalue to a temp (existing `with`-base pattern, lower.cpp:724-727).
- **Explicit conversions:** no conversion mapping exists anywhere. Syntax `operator fn explicit T(...)`;
  stamp `userConversionTypes_` like `userEqTypes_` (lower.cpp:137); C# `explicit operator T`; method-form
  targets rewrite the `Cast` site (lower.cpp:575) to `x.toT()`. Implicit refused.

## D — attributes (val-attributes: mixed; D10/D11 need the capability refactor)

- **Entirely greenfield** — no attribute/annotation/decorator concept in lexer/token/parser/AST/IR.
- **D8 parsing:** `[`/`]` are always standalone tokens (lexer.cpp:265-266) — parser concern only. `[` is
  illegal at every declaration-prefix position today (top-level parseUnit:15-37, parseMember:225-336,
  parseParamList:153-166, parseEnum:456) → attribute `[...]` is unambiguous with one-token lookahead there.
  The ONLY genuine ambiguity is **local-statement position** (parseStmt:646 — a statement can start with `[`
  as a list literal, and statements are newline-terminated with optional `;`, so `[a,b]\nfoo()` collides
  with "two attrs on foo") → **exclude attributes on locals in v1.**
- **D8 example corrections:** Polyglot has **no `new` keyword** (construction is `Type(args)`; arrays are
  `[...]`) and **no named call-arg syntax** (positional only; `name=value` only in `with{}`,
  parser.cpp:1073-1077). So named attribute args are **new syntax** (`[ident '='] expr`); `[return: X]`
  return-target attributes have no grammar home → defer.
- **D9 binding:** `TargetBinding{target,code,pos}` (ast.hpp:190-194) has **no import/using field**; C# std
  bindings use `global::` FQN (csharp:3283-3360), TS uses intrinsics — there is **no per-file import
  accumulator** in the emitter (only user-`import` module linking, emitted at the `module.imports` site,
  typescript:2521-2538 / emitter_base.cpp:1095-1115). Reuse the arm syntax `actual(target) extern("...")`
  (parseBindingArms parser.cpp:188-205) + `$this`/`$0`/`$T` substitution (substBoundTemplate
  emitter_base.cpp:1542) + add an import/using field + a `refuse` arm. Arg typechecking reuses
  `checkArgs`/`checkConvert` (sema.cpp:1599/:1034), same path as extern-class methods and `expect fn`
  signatures.
- **D10/D11 (PROBLEMATIC on the flat enum):** the `Feature` enum is flat/closed, 16 members, no
  parameterization (backend.hpp:24-45); the "intersection" is procedural (checkCapabilities run once per
  target, capability.cpp:168-231). Per-attachment-point granularity is **mandatory** — Python has class
  decorators but **no parameter decorators**, so a coarse `attributes` flag would silently miscompile.
  Expressing target×arg-kind as flat booleans = ~9+ new flags into a set §3.E calls closed → must ride the
  **Slice-0 keyed refactor** with structured keys `attributes.target.{class,method,field,param,return}` +
  `attributes.arg.{const,nonConst,enum,array}`. Non-const intersection is correct: C#/PHP const-only →
  variable values refused whenever C#/PHP configured.
- **D13:** plugins are **always full backends** (buildBackend mandates Program+Type rules + full
  anti-silent-drop coverage, backend.cpp:160-260); the `kind: backend|binding|std` taxonomy exists only in
  `docs/design/json-plugins.md:167`, never implemented. A binding-only package (cross-target `extern
  attribute` decls, no backend) is a **new package payload kind**. P30 carries it (SRI-verified, data-only,
  no code execution — pluginresolve.hpp / registry.hpp) but SRI = integrity-of-fetch, **not** safety of the
  `import`/`using` a binding injects into user output (consumer-audit responsibility, like any dependency).
  No auto-included stdlib precedent (std skeletons are Core-embedded `.pg`, not packages) → explicit deps +
  a curated first-party `polyglot-attributes-*` package.

## Whole-set (val-scope: GO-WITH-CONDITIONS)

- Silent-miscompile register: **M1** (`as` guard), **M2** (attributes metadata-vs-decorator divergence —
  dissolved by the emit-only/FFI framing + H2 honesty note; D10/D12 gate syntax + missing-binding, NOT
  semantic equivalence), **M3** (`==null` bypass), **M4** (`.constructor` access), **M5** (conversion at
  cast site). All differentially-executable features are covered by the conformance suite; attributes are
  golden-only.
- Contradictions: **X1** — C6 and D10/D11 both edit the capability vocabulary → do the keyed refactor once
  (Slice 0). **X2** — "operators in separate packages" implies unscoped cross-package resolution → adopt the
  static shape, defer resolution (S1). **X3** — `AllowAttributeNonConsts` is near-vacuous while C#/PHP is a
  target (maintainer chose to build it anyway from day 1; it lights up on TS/Python-only sets).
- One PR feasible; the one clean optional fissure is **C (operator rework)** since it mutates shipped
  golden-covered behavior. Maintainer chose single PR.
