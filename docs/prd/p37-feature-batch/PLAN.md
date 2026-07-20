# P37 — implementation plan (dependency-ordered slices)

One PR (`p37-feature-batch`), commits in this order so review + bisect stay sane. No back-compat: goldens
regenerate freely, old paths are deleted in the same change. **Build/test discipline (CLAUDE.md):** implement
ALL slices first, then run the full gate ONCE at the end + one POSIX leg via CI — no per-slice full gates
(`-Tier fast` or the unit exe is the only mid-flight ceiling).

## Slice 0 — keyed capability vocabulary (shared prerequisite for C6 + D10/D11)

- Generalize the capability lookup from the flat `Feature` enum to a **string-keyed set with a fixed
  `parent:child` form**, load-validated against a **closed enumerated vocabulary** (unknown key = load
  error). Keep the per-target intersection as set-AND (`checkCapabilities` run once per configured target).
- Touch: `backend.hpp` (`Feature` → keyed vocabulary or an augmenting keyed table), `backend.cpp`
  (`capabilityStance`/manifest parse/validation ~192-207), `capability.cpp` (collector marks + the gate
  ~168-231). Preserve the anti-silent-drop `kCoverage` contract.
- No behavior change yet — pure mechanism. Existing capability tests stay green.

## Slice 1 — A: `init` → `constructor` (full rename, isolated)

- Rename surface keyword + `KwInit`→`KwConstructor` (`lexer.cpp:18`, `token.hpp:30`, `parser.cpp:257-259`),
  `MemberKind::Init`→`::Constructor` (`ast.hpp:197` + 11 usages), the internal name string, and the 12
  plugin skeleton keys `List.init`/`Error.init`/`Array.init`→`.constructor` (4 plugins × 3).
- **Flip the 3 derivation sites in lockstep** (`compiler.cpp:507`, `capability.cpp:215`, `:137`) + the
  `pg_printer.cpp:168`/`ir.cpp` printer literals. **Do NOT touch** the `initParams`/`initBody`/`hasInit`/
  `baseHasInit`/`hasSuper`/`superArgs` data fields or their `decl.*` template paths.
- Rewrite ~42 `init(` occurrences across 28 `.pg` fixtures/samples + 14 embedded `.pg` strings in
  `tests_main.cpp` + SPEC.md/grammar.ebnf/plugin-authoring.md. (`\binit\s*\(` → zero in fixtures/tests when
  done. Do NOT sed Core `src/` broadly — `stmt.init`/`f.init`/`initParams` are unrelated.)
- **Acceptance:** all ctor goldens regenerate; differential behavior unchanged; **+ new golden: emitted
  TS/JS never contains a bare `.constructor` member access (M4).** `super_implicit_base.pg` still passes.

## Slice 2 — B: `is` / `as`

- IR: add `ExprKind::IsTest { operand, testType, binding? }` (type bool) and `ExprKind::AsCast { operand,
  castType }` (type = `castType` with `nullable=true`). Keep numeric `Cast` untouched.
- Parser: `is`/`as` as infix operators below comparison precedence; `is` optional trailing `Ident` binding;
  `as` stays contextual (imports keep parsing).
- Sema: require concrete class/union operand+target (result native-nullable); nominal-relatedness check
  (refuse provably-impossible casts); route the interface-refusal **and the existing #38 match check**
  through the target-aware capability pass (B4 — refuse iff a configured target lacks runtime interface
  identity).
- Lower: `IsTest` in an `if` cond → prepend a narrowed `Let` to `thenBody` (B3 branch-scope). `AsCast` →
  per-target conditional template (+ `fresh` temp for non-simple operands).
- Plugins: `IsTest` + `AsCast` rules on all four (`as` = **runtime guard**, never TS `as` — M1).
- **Acceptance (differential):** `is_binding.pg` (class + union, match/non-match, narrowed use in-branch);
  `as_cast.pg` (success→typed value, failure→null/None, `null as T`→null, then `?? default`/`!` to prove
  native-nullable interop). **Refusal fixture:** interface `is`/`as` refused when TS configured; allowed on
  a C#-only build. Bare boolean `is` (no binding).

## Slice 3 — C: operators complete

- **C5:** TS `Binary`/`Unary` rules emit **static-on-type** dispatch (`T.plus(a,b)`, `T.eq(a,b)`); `eq` is
  null-tolerant, arithmetic throws on null. C#/Python unchanged (native). Keep the `insideOperator`→`lhs`
  rebind for C#.
- Extend `operatorSymbol` (`lower.cpp:421-433`) with `band bor bxor shl shr bnot` (#63); add TS unary static
  routing (#62) + Python dunder rows (`__and__ __or__ __xor__ __lshift__ __rshift__ __invert__ __neg__`).
- **Explicit conversions:** parse `operator fn explicit T(...)`; stamp a `userConversionTypes_` set; C# →
  `explicit operator T`; method-form targets → `toT()` + `Cast`-site rewrite. Refuse `implicit` with a
  diagnostic.
- **C6:** `capability.cpp:92` branches by `opSymbol` → `operatorOverloading:{arithmetic,comparison,eq,
  indexers,conversion}`; PHP manifest flips to `:eq`+`:indexers` native, `:arithmetic`/`:conversion` false.
- **C7:** compound `a += b` on a user type → `a = <op>(a,b)` with lvalue hoisted to a temp.
- **M3:** keep the `rhsIsNullLit` guard — `== null` never routes to a user operator.
- **Acceptance (differential + goldens regen):** `operator_unary.pg` (catches #62 on TS); `operator_bitwise.pg`
  (+ `.expected` golden, #63); `operator_compound.pg` (eval-once on property/indexer LHS); `operator_convert.pg`
  (explicit conversion, all four); `operator_null_eq.pg` (`x == null` bypass, both-null + one-null). Regenerate
  vec2/operators_full/operator_eq/indexer_grid/equality*/union_eq/class_identity_eq goldens (new TS shape).
  **Refusal fixtures:** implicit-conversion refused; C6 sub-capability refusals via a stub backend lacking
  `:arithmetic`/`:indexers`/`:eq`.

## Slice 4 — D: attributes (greenfield)

- Lexer/parser: `[Name(args)]` attribute lists in declaration-prefix position (top-level, member, param,
  enum case) via `while (at(LBracket)) parseAttributeList();`. **No locals.** Named-arg production
  `[ident '='] expr`; const value grammar (literals, enum members, arrays) + non-const values behind the
  capability gate.
- IR/AST: an `attributes` vector on the decl nodes (parallel to `modifiers`); `extern attribute` declaration
  node reusing `TargetBinding` + a **new import/using field** + a `refuse` arm.
- Sema: type-check `[Name(args)]` against the binding's declared `(params)` via `checkArgs`/`checkConvert`;
  const-only enforcement unless `attributes.arg.nonConst` holds across the intersection; attachment-point +
  arg-kind gating via the Slice-0 keyed capabilities (`attributes.target.*`, `attributes.arg.*`); **D12**
  refusal when a configured target has no binding.
- Emit: per-target native annotation via a new attribute rule on each decl; accumulate required
  imports/usings into the existing `module.imports` emission site.
- Distribution: **binding-only package kind** (partial plugin: `extern attribute` decls + imports, no full
  backend) loadable by the P30 resolver; a curated first-party `polyglot-attributes-*` example package.
- **Acceptance (golden-only — framework not in harness):** `attr_emit.pg` — one binding, assert the exact
  per-target emitted annotation + required `using`/`import` on class/method/field/param; named args; const +
  (TS/Python-only) variable-value args. **Refusal fixtures:** unsupported attachment point for a configured
  target; param attribute when Python targeted; variable-value arg when C#/PHP targeted; `[MyAttr]` with no
  binding (D12); `python => refuse` arm hit. Round-trip test for the `.pg` printer.

## Cross-cutting: SPEC + docs

- Add honesty notes **H1–H5** to SPEC §3.C/§3.D/§10 (see PRD §F); extend the "Refused" list wording so
  attributes-as-syntax (allowed) vs attribute read-back (refused) is unambiguous.
- Update `grammar.ebnf` (constructor decl, `is`/`as`, attribute lists + named args, `operator fn explicit`,
  `extern attribute`), SPEC §6 (operators incl. conversions + the C6 capability grades), and
  `docs/plugin-authoring.md` (keyed capabilities, binding-only package kind, `extern attribute` bindings).
- Close **#62** and **#63** via this PR (fixed under Slice 3).

## Acceptance matrix (feature → instrument)

| Feature | Golden | Differential | Refusal fixture |
|---|---|---|---|
| A rename | ✅ (regen ctors; + no `.constructor` access) | ✅ behavior unchanged | — |
| B `is` (class/union, bare + binding) | ✅ | ✅ | — |
| B4 interface `is`/`as` w/ TS configured | — | — | ✅ |
| B `as` (class/union) | ✅ runtime guard shown | ✅ incl. fail→null, `null as T` | — |
| C5 operators → static (TS) / native (C#/Py) | ✅ regen 9 goldens | ✅ behavior identical | — |
| C5 explicit conversions | ✅ cast-site | ✅ | ✅ implicit refused |
| C5/M3 `x == null` bypass | ✅ | ✅ both-null + one-null | — |
| C6 sub-capability gating | — | — | ✅ stub backend |
| C7 compound `+=` on user type | ✅ | ✅ eval-once on prop/indexer LHS | — |
| D attribute emit + `extern attribute` binding | ✅ per-target text + imports | ❌ (framework absent) | — |
| D10/D11 unsupported attachment / arg-kind | — | — | ✅ |
| D12 no binding for configured target | — | — | ✅ |

## Deferred follow-ups (recorded, not in this PR)

Cross-package operator *resolution* (static shape adopted now); `return:`-target & type-parameter
attributes; auto-included attribute stdlib package.
