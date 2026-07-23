# P37 — Feature batch: constructor rename · `is`/`as` · operators-complete · attributes

**Status:** designed (2026-07-20), §D revised (2026-07-23), **BUILT + gated (2026-07-23/24, as-built
markers inline)**. One cohesive PR
(`p37-feature-batch`), ordered commits, no back-compat (clean cutovers). Investigated by an 8-agent scope
investigation + a 4-agent adversarial validation team; validation verdict **GO-WITH-CONDITIONS**
(conditions folded in below). §D was subsequently revised by a 6-agent follow-up investigation
(Haxe / prior art / target semantics / repo + generics + LSP grounding — evidence in
`ATTRIBUTES-RESEARCH.md`) that resolved the maintainer's block on the attributes concept with the
**three-tier taxonomy** (§D below); it was briefly drafted as a separate "P38" and folded back here.

This PRD is the design contract. `PLAN.md` is the dependency-ordered slice plan + acceptance matrix.
`ANALYSIS.md` is the code-grounded validation findings (file:line) the implementation follows.

## 0. Prime directive check (PRD §3)

Every item here was weighed against the support/refuse/faithfulness/determinism contract. The batch adds
language surface **without** opening a lowest-common-denominator seam:

- **Runtime reflection stays refused, forever.** `.pg` is fully transpiled away — nothing exists at runtime
  to reflect against. Attributes (workstream D) therefore come in exactly two allowed forms and one refusal:
  **Tier 1 emit-only pass-through** (Polyglot parses, type-checks against a binding, emits each target's
  native annotation for *that target's* ecosystem to consume — never read back), **Tier 2 portable inert
  metadata** (queries resolved **at transpile time**, lowering to inline constants — nothing resolves at
  run time, so nothing is reflected against), and **Tier 3 behavior transforms — refused** (§D.1). SPEC §10
  must state the three-way split explicitly so "attribute introspection at run time" (refused, forever —
  the native `GetCustomAttributes`/`ReflectionAttribute`/reflect-metadata surfaces) doesn't read as
  contradicting the two allowed tiers.

## A. `init` → `constructor` (surface rename, full)

Rename the constructor keyword `init` → `constructor` on the language surface **and** through the internal
identity (`KwInit`→`KwConstructor`, `MemberKind::Init`→`MemberKind::Constructor`, the internal name string
`"init"`→`"constructor"`, and the plugin skeleton keys `List.init`/`Error.init`/`Array.init`→`.constructor`).
No back-compat alias. `super()` is unaffected (separate keyword).

**The one trap (ANALYSIS §A):** the ctor's identity flows as a flag, but three sites *derive* the string
`"init"` from that flag at runtime — `compiler.cpp:507` (std-binding lookup key), `capability.cpp:215`
(bound-member registry key), `capability.cpp:137` (construction *use* key). All three must flip to
`"constructor"` in lockstep with the 12 plugin skeleton keys, or std `List()`/`Error()`/`Array()`
construction silently loses its binding (and `:137` vs `:215` drifting apart would let an unbound ctor use
pass the anti-silent-drop gate — a §3 violation). **Leave the internal ctor *data* fields alone**
(`hasInit`/`initParams`/`initBody`/`baseHasInit`/`hasSuper`/`superArgs` and their `decl.*` template paths):
they are decoupled from the keyword, and renaming them only adds a second sync surface for cosmetic gain.

**M4 guard:** `constructor` is a live JS/TS prototype member. Confirm the skeleton keys are internal lookup
keys never emitted as identifiers, and add a golden asserting emitted TS/JS never produces a bare
`.constructor` access. Verify the new internal string doesn't trip `checkReservedNames`/the `identifiers`
manifest.

## B. `is` / `as`

Two new expression operators over **class/record hierarchies** (as built: union types/cases refuse toward
`match` — no case-as-type exists to narrow to; interfaces gate per-target, see the refusal):

- **`is`** — type test. `x is T` → `bool`. `x is T name` binds a narrowed local `name` **scoped to the
  guarded branch only, permanently** (B3 — no flow-typing out, ever). Emits the P36 #38 shapes: C# `is`,
  TS/PHP `instanceof`, Python `isinstance`. As built, the binding lowers to a hoisted
  `let name = x as T` + `name != null` BEFORE the `if` (single-eval by construction, idiomatic on all
  four targets); the emitted binding outlives the branch, so redeclaring the name later in the same
  block is refused.
- **`as`** — checked conversion, result type `T?` (native nullable, **not** `Option<T>`, because operands
  are always concrete reference types). **Semantics pinned (M1, non-negotiable):** yields the value typed as
  `T` on success, **`null` on failure — never throws.** Emit: C# `x as T`; TS `(x instanceof T ? x : null)`;
  Python `x if isinstance(x, T) else None`; PHP `$x instanceof T ? $x : null`. **Never a bare TS `as`** (TS
  `as` is a compile-time assertion with no runtime check → silent miscompile). `null as T` → `null`;
  `null is T` → `false`. Non-simple operands bind to a single-eval temp (existing `fresh` mechanism).

**Refusal (B4, config-driven — a principled relaxation of P36 #38):** interface-typed `is`/`as` is refused
**only when a configured target lacks runtime interface identity** (TS erases interfaces → `instanceof`
can't back them). C#/Python/PHP-only builds *allow* it. This moves the check (and the existing match-arm
#38 check) into the target-aware capability-intersection pass, making it config-driven like every other
gate — an improvement over today's unconditional refusal.

## C. Operators — complete the surface

**C5 — static dispatch where dispatch is synthetic; native where it's already native.** Validation
(ANALYSIS §C) established that the instance-method rewrite (`a.plus(b)`) exists on **only TypeScript** today
— C# already emits native static `operator +` (null-safe, travels with its type) and Python native
`__add__` dunders. Forcing *all* operators into a free static class would abandon C#'s native
`operator`/`Equals`/`GetHashCode` (silently breaking `Dictionary`/`HashSet`/records/LINQ) and Python's dunder
protocol — a regression. So:

| kind | C# | TypeScript | Python | PHP |
|---|---|---|---|---|
| arithmetic (`+ - * / % & \| ^ << >>`) | native `operator` | **static-on-type** `T.plus(a,b)` | native dunder | refuse (`operatorOverloading:arithmetic`) |
| unary (`neg`, `bnot`) | native | **static-on-type** (fixes #62) | native dunder | refuse |
| `==`/`!=` (`eq`) | `override Equals`+`GetHashCode` | **static, null-tolerant** `T.eq(a,b)` | native `__eq__` | structural (`:eq`) |
| comparison (`< <= > >=`) | native | static-on-type | native dunder | `:eq`/`:comparison` |
| indexer (`get`/`set`) | native `this[...]` | `.get`/`.set` | `__getitem__`/`__setitem__` | `:indexers` (ArrayAccess) |
| explicit conversion | `explicit operator T` | `.toT()` / cast-site rewrite | `.toT()` | `.toT()` |

TS moves from instance (`a.plus(b)`) to **static-on-the-type** (`Vec2.plus(a, b)`): arithmetic still throws
on a null operand (faithful to C#/Python deref), and `eq` becomes null-tolerant (the one real bug — instance
`a.eq(b)` NREs when `a` is null). **M3 (pinned):** a comparison against the `null` literal is always a
null/reference check, never a user-`eq` call (keep the `rhsIsNullLit` guard).

- **Explicit conversion operators** — new, as built: `operator fn explicit(): T` (the RETURN TYPE is the
  conversion target; no parameters). C# native `explicit operator T`;
  the method-form targets emit a named `toT()` and rewrite the `Cast` site when from→to is a registered user
  conversion (a `userConversionTypes_` set + a `Cast`-rule branch, mirroring `userEqTypes_`). **Implicit
  user conversions are refused** with a diagnostic (invisible call-site injection has no TS hook and violates
  "code should be obvious"; a named method is the faithful substitute). No open-ended new operator *symbols*
  — the overloadable set is the closed SPEC §6.1 table + indexers + the one conversion spelling.
- **C7 compound assignment** — `a += b` on a user type lowers to `a = <op>(a, b)`; the lvalue is hoisted to
  a temp to avoid double side-effects (existing `with`-base pattern). Native on C#/Python.
- **Fixes #62 (TS unary `neg` mis-emit) and #63 (bitwise `band/bor/bxor` SPEC-vs-code drift)** as part of
  completing the surface: extend `operatorSymbol` with `band→& bor→| bxor→^ shl→<< shr→>> bnot→~`, add TS
  static routing for unary + bitwise, add Python dunder rows.

**C6 — hierarchical operator capabilities.** Split the coarse `operatorOverloading` flag into keyed
sub-capabilities `operatorOverloading:arithmetic | :comparison | :eq | :indexers | :conversion`. This
requires the capability-vocabulary refactor (§E). `capability.cpp:92` must branch by `opSymbol` instead of
one blanket mark. **PHP flips** from blanket-refuse to supporting `:eq` (structural value equality) and
`:indexers` (ArrayAccess) while refusing `:arithmetic`/`:conversion` — a deliberate, ratified behavior
change that widens PHP support. Umbrella semantics: a bare `operatorOverloading` stance applies to all
sub-keys unless a sub-key overrides it.

## D. Attributes — the three-tier taxonomy

**The block this resolves (2026-07-23).** The four targets split into two camps by *when an attribute's
own logic runs* (§D.1): C#/PHP attributes are inert metadata whose constructor never runs unless a program
makes an explicit reflection call Polyglot never emits; TS/Python decorators execute eagerly at definition
time. So one native-mapped concept can't have one semantics — and the fallback of exposing *two* concepts
("Attributes" and "Decorators"), each usable only when every configured target supports it natively, would
leave a typical multi-target user able to use **neither**. The resolution (Haxe-informed — see
`ATTRIBUTES-RESEARCH.md`; Haxe survived the same fault line across 10+ targets): **the portable tier must
not map to native mechanisms at all.** One `[Name(args)]` usage syntax; the *declaration* picks the tier:

| Tier | Declared as | What it is | Works on | Faithfulness |
|---|---|---|---|---|
| **1 — pass-through** (§D, this section) | `extern attribute` | typed binding to each target's **native** annotation, for that target's *own* framework | targets with a binding; intersection-gated, D12 loud refusal | outside §3.C (H2) |
| **2 — portable metadata** (§D.2) | `attribute` | a **pure data shape** — no body, no constructor, const-only args — queried via `std.meta` at **transpile time** | **all targets, unconditionally** — no capability, no gate | inside §3.C (H6) |
| **3 — behavior transforms** | — | decorators that wrap/replace declarations | **refused** (§D.1) | — |

The intersection objection applies only to Tier 1 — and there it is *correct*: Tier 1 exists to feed one
target's framework (Angular reads `@Component`, Symfony reads `#[Route]`), which is per-target by nature
and typically already routed per-target by pgconfig `include` rules. Tier 2 — the tier a multi-target user
reaches for — requires nothing from any target because the compiler both writes and reads the data. And
the constructor-timing divergence cannot arise: Tier 2 attributes have no constructor (Haxe's `@name`
move: metadata is data, not a class); Tier 1 disclaims cross-target runtime behavior (H2); Tier 3 (the
only place eager execution would matter) is refused.

### Tier 1 — emit-only pass-through (`extern attribute`)

A Tier 1 `.pg` attribute is parsed, type-checked against a binding, and emitted as each target's native
annotation. Polyglot never executes or reads it. This is an **FFI-class** feature (like `extern`),
governed by the same per-target binding + capability-intersection machinery.

*(This section is AS-BUILT, 2026-07-24 — the original design bullets were revised where implementation
found a better or narrower shape; each deviation is also logged in `PLAN.md` §As-built.)*

- **Syntax (D8, as built):** C#-style `[Name(args)]` in **declaration-prefix position** — before class,
  record, function, method, field/property, and inline before a parameter. Stacked `[A] [B]` and grouped
  `[A, B]` are both accepted (equivalent). **No attributes on local variables** (the one position where
  `[` already begins a list literal) and no `return:`-target attributes (no grammar home) — both recorded
  deferrals. Implementation found one more ambiguity D8's analysis missed: an attribute list after an
  *expression-bodied* declaration would be swallowed as a subscript, so a **subscript `[` must start on
  the same line as the expression it indexes** (a next-line `[…]` is a new statement).
- **Arguments (as built):** positional and **named** (`name = value`), with declared defaults filled in.
  Value grammar: `bool`/int/float/string literals (incl. negative numerics), **enum members**, and
  **non-empty arrays** of those — each rendered from plugin-spec DATA (`trueLit`/`falseLit`,
  `enumMemberOp`, `constArrayOpen/Close`), never target-name checks in Core. **Non-const / variable
  values (D11) are DEFERRED** (not built): validation X3 already called the capability near-vacuous
  (it could only ever light up on TS/Python-only target sets), so v1 is const-only everywhere with a
  loud refusal; `attributes:arg.*` capability keys enter the vocabulary only when a consumer ships (H5).
- **Binding (D9, as built):** one verbatim spelling instead of the per-target import grammar sketched at
  design time — the extern template is the ENTIRE native annotation line, and the optional import is a
  verbatim header line:
  ```
  extern attribute JsonProp(name: string) {
    actual(csharp)     extern("[Newtonsoft.Json.JsonProperty($0)]") import "using Newtonsoft.Json;"
    actual(typescript) extern("@JsonProperty($name)")               import "import { JsonProperty } from 'class-transformer';"
    actual(python)     refuse
  }
  ```
  The declared `(params)` signature type-checks each usage (`checkArgs`/`checkConvert` path);
  `$0`…/`$name` substitute the constant args. Because the template is the whole line, Tier 1 emission
  needs zero plugin-template knowledge: one shared injection point (the MapMembers/MapDecl walk) prints
  the lines above classes/records/methods/functions/fields, and C#/PHP param templates prepend the
  inline text; import lines dedupe onto the module header.
- **Attachment points gated by intersection (D10, as built):** keyed capabilities
  `attributes:target.{type,method,function,field,param}`, set-AND across configured targets.
  Per-attachment-point granularity proved mandatory exactly as designed: TS declares `function` and
  `param` false (TC39 has neither), Python declares `field` and `param` false. Arg-kind keys
  (`attributes:arg.*`) ship with the nonConst follow-up, not before (H5).
- **D12 (non-negotiable, as built):** a used attribute with no arm — or an explicit `refuse` arm — for a
  configured target → **loud compile-time refusal** naming the attribute and target. The backstop that
  makes emit-only honest.
- **D13 — mechanism-only distribution: DEFERRED.** The binding-only P30 package kind (a partial plugin
  contributing `extern attribute` decls, not a full backend) is additive and ships as a follow-up;
  today `extern attribute` declarations live in user `.pg` / imported modules.

### D.1 Design note — why attributes are *pass-through only*, not "functional" (behavior-affecting)

Recorded after a maintainer challenge (2026-07-21): *"if a polyglot attribute's constructor does logic, and
that logic is reflected in the generated code, why isn't it portable?"* A 3-agent investigation
(attr-semantics / attr-models / attr-interop) answered this precisely. The distinction is real and this note
captures it next to the decision it justifies.

**"Functional attribute" names one of two fundamentally different mechanisms:**
- **Model 1 — pass-through annotation (logic runs at the *target's* runtime).** Polyglot emits the native
  annotation (`[Attr]`/`@dec`/`#[Attr]`) verbatim; whatever happens is up to the target's own
  runtime/framework. **This is what P37 §D builds.** Nothing is "reflected into" the output — the annotation
  *is* the output.
- **Model 2 — compile-time transform / macro (logic runs at *Polyglot's* transpile time).** The attribute's
  logic rewrites the annotated declaration's IR at transpile time; every target then emits plain, already-
  transformed code with **no** attribute/decorator at all. *This* is "logic reflected in the generated code"
  — and it is the model the maintainer's phrasing actually describes.

**The execution-timing fault line (the semantic bedrock).** The four targets split into two camps by *when*
an attribute's own logic runs:

| | logic runs… | transforms the declaration? | ctor runs without a `GetCustomAttributes`-style call? |
|---|---|---|---|
| **C#**, **PHP 8+** | **only when reflection explicitly materializes it** | no — inert metadata | **never** |
| **TypeScript** (legacy + TC39), **Python** | **eagerly, at definition time** | yes — wraps/replaces | always |

**Empirical confirmation (C#, maintainer-run 2026-07-21).** Even *reflecting* over a decorated type/method
does not run the attribute constructor — because ordinary introspection never touches the custom-attribute
blob:
```csharp
[ClassTest] class TestClass { [MethodTest] public void TestMethod() { } }
// ClassTestAttribute()/MethodTestAttribute() ctors each Console.WriteLine on construction.

var t  = typeof(TestClass);                                  // metadata handle — ctor does NOT fire
var m  = t.GetMethod(nameof(TestClass.TestMethod));          // metadata handle — ctor does NOT fire
// Output contains NO "... attribute constructor triggered" line.
```
The attribute ctor fires **only** on the specific `GetCustomAttributes()` / `GetCustomAttribute<T>()` family
(`m.GetCustomAttributes(true)` would print "Method attribute constructor triggered"). So a C#/PHP attribute
is inert not just "until reflected" but "until a program deliberately makes the one reflection call that
materializes it." Polyglot emits **no** reflection and transpiles the `.pg` away, so that call site never
exists — a C#/PHP attribute constructor provably runs **zero times**.

**Consequence — why a behavior-affecting pass-through attribute cannot be portable (Model 1).** Take
`@Memoize`: on TS/Python the decorator is a function that runs at definition time and wraps the method
(**live**); on C#/PHP it is inert metadata that does nothing unless an external weaver/generator/reflection
host acts on it — which Polyglot refuses (§3.B) and does not emit (**dead no-op**). Same attribute, live on
two targets, dead on two — a silent behavioral divergence, the one unforgivable sin (§3). This is *intrinsic*
to Model 1, not a gap to close, which is exactly why §D restricts pass-through to **semantically-passive**
annotations and H2 puts behavior-transforming decorators out of scope.

**Model 2 *would* make it portable** — one IR rewrite feeds all four emitters as plain code, no reflection,
no divergence — but Model 2 is a **hygienic-macro subsystem**, not a feature: a transpile-time `.pg`
interpreter beside the emitters, plus hygiene, ordering, error-span mapping, and a `--expand` view. Prior art
is unanimous on the cost and the traps (Lisp/Scheme → hygiene is mandatory; Rust proc-macros / C++ TMP →
debuggability & error spans are brutal; C# source generators / Java APT → the *safe* design is **additive-
only**, which cannot even express `@Memoize`-rewrites-the-body; Scala → deleted and rebuilt its macro system;
Nim → macro-heavy code becomes an unreadable dialect, the opposite of Polyglot's readable-output promise).
A user-programmable macro layer is the maximal form of the scope creep that killed JSIL/SharpKit/Bridge.NET.

**The reality check that makes this moot for the stated motivation.** The attributes that are "used
everywhere daily" — Angular `@Component`/`@Injectable`, `[JsonProperty]`/`[JsonIgnore]`, DataAnnotations
`[Required]`, `@app.route`/`[Route]` — are **passive metadata consumed by the target's *own* framework**
(Angular's compiler, System.Text.Json/Newtonsoft, the DI container, the router). For all of them Polyglot
**must** emit the native annotation verbatim; a transform can't replicate them without reimplementing each
framework (modern Angular Ivy and STJ even consume them at *build* time via their own compiler/source-gen,
making native pass-through *more* mandatory). So the portable behavior-injecting attribute is **orthogonal**
to the daily demand: the daily demand is Model 1 pass-through.

**Decision (closed 2026-07-23).** §D builds **Model 1** (pass-through, passive-metadata-only) as Tier 1 —
correct for the framework-interop use cases — and the follow-up investigation added **Tier 2** (§D.2) for
the portable-metadata demand Model 1 cannot serve. Behavior-transforming / "functional" attributes (Model
2 as a *user-facing* feature) are **refused**: this is exactly Haxe's macro tier (`@:build`), i.e. the
maximal form of the scope creep that killed JSIL/SharpKit/Bridge.NET. If a specific transform is ever
genuinely wanted, the scope-disciplined path remains a **fixed, first-party, compiler-authored lowering**
(i.e. "add one language feature," like Polyglot already synthesizes constructors and value-equality) —
**not** a user-programmable `.pg` macro system (see §G).

### D.2 Tier 2 — portable inert metadata (`attribute` + `std.meta`)

**Declaration — a pure data shape.** `attribute Range(min: i32, max: i32)` — a name + typed parameters.
**No body, no methods, no bases, no constructor logic** (a `{` after the param list is a diagnostic naming
the Tier 3 refusal). Parameter types are the **const envelope**: `bool`, the integer/float scalars,
`string`, enum types, and arrays of those — the intersection of the C#/PHP attribute-constant rules and
what every target holds as a plain literal. Const defaults allowed. Internally the declaration is a
record-shaped type flagged as an attribute — it reuses the record machinery end-to-end but is **emitted
only if materialized** (below). This is the typed shape Haxe's rejected "typed metadata" evolution
proposal pointed at (haxe-evolution #73): where Haxe metadata is stringly-typed with `Dynamic` returns,
a `.pg` attribute has a signature the checker enforces at every attachment and a typed query result.

**Attachment (as built).** Same `[Name(args)]` syntax and positions as D8 — classes/records, methods,
fields/properties, and **parameters** (stacked/grouped; positional + named args; no locals; enum cases
deferred). Tier-2-specific rules: **values must be compile-time constants** — literals, enum members,
non-empty arrays of those — always (there is no non-const gate for Tier 2; the value must be inlinable
at every query site). **At most one application of a given attribute type per declaration** (repeat →
diagnostic; `AllowMultiple` is deferred). **No capability gating** — nothing native is emitted, so
Python's lack of parameter decorators etc. is irrelevant here; the D10 `attributes:target.*`
intersection applies to Tier 1 only. (Free functions carry Tier 1 only: `Meta` takes a type, so
function-level Tier 2 metadata has no query surface — refused toward that rationale.)

**Query — compile-time-resolved, lowers to inline constants.** The `Meta` intrinsics:
`Meta.has<T, A>() -> bool`, `Meta.get<T, A>() -> A?`, `Meta.member<T, A>("name") -> A?`, and
`Meta.param<T, A>("method", "param") -> A?`. **Every query is resolved at transpile time**: the compiler looks up
the attribute list on the statically-named declaration and lowers the call site to plain code — `has` →
a `true`/`false` literal; `get`/`member` → a construction of the attribute record from the recorded
constant args (C#/PHP `new Range(1, 8)`, TS `new Range(1, 8)`, Python `Range(1, 8)`) or the target's
typed `null`/`None`. **No metadata file, no table, no reflection surface, no runtime type identity —
the output contains the *answers*, not the questions.** Consequences, all pinned:
- An attribute attached but never queried produces **zero runtime output** (golden-pinned); a queried
  attribute materializes exactly its record type + the inlined constants. Pay only for what you query —
  the discipline that separates this from the Bridge.NET/JSIL/SharpKit default-on metadata tables
  (`ATTRIBUTES-RESEARCH.md` §2), and from Haxe's own always-emitted `__meta__` (its documented DCE/bloat
  regret).
- **Fresh value per query evaluation** (incidentally C# `GetCustomAttributes` semantics); attribute
  records have structural equality, so identity is immaterial. Identical on all four targets →
  **differentially testable** — the conformance suite, not goldens, is the primary instrument (H6).
- **Exact-type lookup only**: `Meta.get<Derived, A>()` consults `Derived`'s own attribute list — no
  inherited walk (C#'s `Inherited=true` is a reflection-walk behavior we don't replicate; H7).
- `Meta.member`'s name argument must be a **string literal** naming a member of `T` — unknown member →
  diagnostic with the exact literal underlined (typed where Haxe is stringly).

### D.3 M6 — the static-resolvability invariant (non-negotiable, permanent)

**M6: every `Meta` query is resolved from names that are compile-time-concrete, forever.** `Meta` will
**never** accept a runtime-variant type — no `Meta.of(expr)` over a value's runtime type, no type
parameter as a type argument, no computed member name. This is not a v1 simplification; it is
structurally load-bearing twice over:
1. **There is nothing at runtime to ask.** Tier 2's entire value — zero tables, zero reflection runtime,
   zero output when unqueried, §3.C-faithful semantics — exists *because* the question is answered at
   transpile time. A runtime-variant query would require shipping per-type metadata registries in every
   output **plus** inventing a uniform cross-target runtime type-identity primitive that does not exist
   (TS/Python/PHP unions discriminate by string tag, C# by nominal type; TS interfaces have no runtime
   identity at all) — i.e. the exact default-on reflection-emulation path §3.B refuses and the dead
   C#→JS cohort died on.
2. **The compiler is generic-preserving, so a type-parameter query can never be resolved.** Polyglot
   never monomorphizes: generic decls lower 1:1 (`ir.hpp:427-538`, `lower.cpp:415`) and emit as native
   generics; TypeArg inference substitutes at call sites only, never specializing bodies
   (`sema.cpp:1504-1548`). Inside `fn f<T>() { Meta.get<T, Range>() }` there is no point in compilation
   at which `T` is one type — so this is refused **by architecture**, not by choice, and lifting it would
   require monomorphization (a rejected architecture change). Any future "dynamic metadata" request
   re-opens as its own PRD against both walls above, with the recorded expectation of decline.

**How the compiler *knows* an argument isn't statically determinable — a closed syntactic form, not an
analysis.** Polyglot has no constant-evaluation machinery and gains none here; resolvability is decided
by form, so the diagnostic set is total and predictable ("define errors out of existence" — no
sometimes-works constant propagation):
- **Type arguments**: a `TypeRef` is concrete iff every `Named` node in it, walked recursively like
  `resolveTypeRef` (`sema.cpp:852-870`), satisfies `!genericsInScope_.count(name) &&
  (isBuiltinType(name) || typeNames_.count(name))`. The `genericsInScope_` set (`sema.cpp:388`,
  maintained by `pushGenerics`/`popGenerics` around every decl walk) is checked **first** — it is
  exactly how sema distinguishes a type parameter from a concrete type, so `T` in the example above
  fails the test with a diagnostic naming M6, while `List<Foo>` composes fine.
- **Member-name argument**: the node must be `ExprKind::StringLit` (value directly on `Expr::text`,
  `ast.hpp:103-115`); a variable, concatenation, or interpolated string → diagnostic. No evaluator
  needed or wanted.
- **Sync trap found by validation (fix, don't inherit):** explicit type args on member calls currently
  **parse and are silently ignored** — `parser.cpp:1025-1040` fills `Expr::typeArgs` for
  `obj.m<T>(...)`/`Type.m<T>(...)`, but the static/instance/extension call paths in sema never read
  them (only construction does, `sema.cpp:1738`). `Meta` adds the first consumer; the same change adds
  a diagnostic for explicit type args any *other* member-call path would drop — an existing latent
  silent-drop closed under the anti-silent-drop rule, not widened.

**Where enforcement lives — and why the LSP diagnoses identically for free.** All M6/D.2 usage checks go
in **target-independent sema (`check`)** — NOT the capability pass: `polyglot build`, `polyglot check`,
and the LSP all funnel through the one shared front end (`runFrontEnd`, `compiler.cpp:460`; the LSP's
`analyze()` at `:765` runs the same sema on every keystroke), whereas `checkCapabilities` runs only
inside `compile()` and never reaches live squiggles. One `Diagnostic` type serves both consumers
(`diagnostics.hpp:21-35`); `Meta` diagnostics use the **two-arg span overload** (`bag.error(pos, end,
msg)`, `diagnostics.hpp:35` — machinery present, previously unused by sema) so the editor underlines
exactly the offending type argument or string literal (the LSP's point-widening covers identifiers only,
`main.cpp:1056-1060`). The live generated-output preview (`polyglot/emit`) runs the full `compile()` per
debounced edit, so query resolution changes are visible in the preview as you type. Hover on a `Meta`
call showing the resolved constants, and member-name completion inside the string literal, are recorded
LSP follow-ups (hover has direct precedent, `main.cpp:1154-1163`; in-string completion is net-new).

## E. Shared prerequisite — keyed capability vocabulary

C6 (operator sub-keys) and D10/D11 (attribute target/arg keys) both need capabilities the current **flat
16-member `Feature` enum** can't express. Generalize the capability lookup to a **string-keyed set with a
fixed `parent:child` form**, **load-validated against a closed enumerated vocabulary** (an unknown key is a
load error — never silently ignored, per the anti-silent-drop rule). The per-target intersection stays
set-AND; the closed-vocabulary discipline of PRD §3.E/§4.11 is explicitly reaffirmed to cover these keys
(sub-keying a fixed entry into a small set of grades is *refinement*, not per-feature growth). Build once;
both workstreams consume it.

## F. Faithfulness / determinism honesty notes (must land in SPEC §3.C/§3.D/§10)

- **H1** — reflection-never rationale + the three-way attribute split (decision 0): native runtime
  reflection refused forever; Tier 1 emit-without-read-back allowed; Tier 2 compile-time-resolved queries
  allowed *because nothing resolves at run time*.
- **H2** — **Tier 1** attributes are outside the §3.C faithfulness guarantee: Polyglot guarantees the
  spelling + attachment land per the binding, **not** identical cross-target runtime behavior (C#/PHP
  attributes are inert metadata; TS/Python decorators execute at definition time). Behavior-*transforming*
  decorators are out of scope; only bindings to a semantically-passive annotation on each target are
  permitted; no binding ⇒ D12 refusal.
- **H3** — `as` = checked conversion → `null` (runtime guard on TS/Python/PHP, never TS assertion); interface
  `is`/`as` refused when a configured target lacks runtime interface identity; `is`-binding narrows in-branch
  only.
- **H4** — user operators lower to static methods on the synthetic-dispatch target (TS) and stay native on
  C#/Python; comparison vs the `null` literal is a null check, not a user-`eq` call; explicit conversions
  only.
- **H5** — capability-vocabulary governance: operator sub-keys and attribute keys are additive to the closed,
  load-validated set via a fixed `parent:child` form — not a growth license.
- **H6** — **Tier 2** metadata is fully **inside §3.C**: semantics are identical on all four targets by
  construction (compiler-emitted constants), pinned **differentially** by the conformance suite — the
  first attribute surface that is, unlike Tier 1's golden-only coverage.
- **H7** — `Meta` queries are exact-type, definition-site, compile-time lookups yielding a fresh value per
  evaluation: no inherited-attribute walk (no C# `Inherited=true`), no runtime dispatch, no decorator
  mutation semantics. M6 (static-resolvability, §D.3) is permanent, not a v1 gap.

## G. Scope boundaries & deferred follow-ups

- **Refused (loud diagnostic, never silent):** native runtime reflection / attribute read-back; new operator
  *symbols*; implicit user conversion operators; attributes on local variables; a Tier 1 attribute used
  with no arm (or an explicit `refuse` arm) for a configured target; variable attribute-arg values (both
  tiers — deferred with the `attributes:arg.*` keys); **runtime-variant `Meta` queries — permanently**
  (M6, §D.3: instance-dispatched `Meta.of(expr)`, type-parameter type args, computed member/param names);
  repeated application of one attribute type; a Tier 2 `attribute` declared with a body; attributes on an
  attribute declaration's own parameters; Tier 2 metadata on free functions (no query surface); explicit
  member-call type args that no path consumes (latent silent-drop, closed by D.3); **Tier 3 behavior-
  transforming attributes** (Model 2 as a user feature — if a specific transform is ever wanted, a fixed
  first-party compiler-authored lowering, never a user-programmable `.pg` macro platform; rationale +
  empirical C# demonstration in §D.1).
- **Deferred (demand-gated follow-ups, recorded not built):** cross-package operator *resolution* (adopt the
  static *shape* now; no consumer + no resolver path today); `return:`-target, enum-case, and type-parameter
  attributes (new syntax / no grammar home); non-const attribute values (`attributes:arg.*`); the D13
  binding-only P30 package kind + an auto-included attribute stdlib package; `AllowMultiple` (query returns
  a list); `Meta.getInherited`; `Meta.all<T>()` (needs a heterogeneous-list story); a dual-tier single
  declaration (portable data *and* native emission from one attachment); LSP hover showing resolved `Meta`
  constants + member-name completion inside the string literal.

## H. Testing strategy (see PLAN.md matrix)

Operators / `is` / `as` / compound-assign / conversions are **differentially executable** — the conformance
suite is the safety net (critical for the C5 rework, which regenerates 9 existing operator/equality
goldens). **Tier 1** attributes hook target *frameworks* absent from the harness → **golden-only** (assert
emitted per-target text + required imports). **Tier 2** is differentially executable (H6): `meta_query.pg`
prints `Meta.has`/`get`/`member`/`param` results (present + absent + defaults + enum + array args +
parameter metadata) and all four targets must agree — PHP included; plus one golden pinning that an
attached-but-unqueried attribute emits **zero** runtime output. Capability gates (C6 sub-keys, D10/D11, B4, D12) and the M6/D.2 rules (type-parameter type
arg, non-literal member name, unknown member, repeat application, param attachment, body on an `attribute`)
get **refusal fixtures**. The A-rename adds a golden asserting emitted TS/JS never yields a bare
`.constructor` access.
