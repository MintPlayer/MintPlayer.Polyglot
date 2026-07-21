# P37 — Feature batch: constructor rename · `is`/`as` · operators-complete · attributes

**Status:** designed (2026-07-20), pre-implementation. One cohesive PR (`p37-feature-batch`), ordered
commits, no back-compat (clean cutovers). Investigated by an 8-agent scope investigation + a 4-agent
adversarial validation team; validation verdict **GO-WITH-CONDITIONS** (conditions folded in below).

This PRD is the design contract. `PLAN.md` is the dependency-ordered slice plan + acceptance matrix.
`ANALYSIS.md` is the code-grounded validation findings (file:line) the implementation follows.

## 0. Prime directive check (PRD §3)

Every item here was weighed against the support/refuse/faithfulness/determinism contract. The batch adds
language surface **without** opening a lowest-common-denominator seam:

- **Runtime reflection stays refused, forever.** `.pg` is fully transpiled away — nothing exists at runtime
  to reflect against. Attributes (workstream D) are therefore strictly **emit-only pass-through**: Polyglot
  parses them, type-checks them against a binding, and emits each target's native annotation for *that
  target's* ecosystem to consume. Polyglot never reads an attribute back. (SPEC "Refused" already names
  "attribute introspection at run time"; §D below and SPEC §10 must state the emit-only-is-allowed /
  read-back-is-refused distinction explicitly so the two don't read as a contradiction.)

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

Two new expression operators over **concrete class/union types** (never interfaces — see the refusal):

- **`is`** — type test. `x is T` → `bool`. `x is T name` binds a narrowed local `name` **scoped to the
  guarded branch only, permanently** (B3 — no flow-typing out, ever). Emits the P36 #38 shapes: C# decl
  pattern, TS `instanceof`, Python `isinstance`, PHP `instanceof`. Lowering prepends a narrowed `Let` to
  `ir::If.thenBody` (no `ir::If` change needed — makes branch-scoping structural).
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

- **Explicit conversion operators** — new: `operator fn explicit T(...)`. C# native `explicit operator T`;
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

## D. Attributes — emit-only pass-through

A `.pg` attribute is parsed, type-checked against a binding, and emitted as each target's native annotation.
Polyglot never executes or reads it. This is an **FFI-class** feature (like `extern`), governed by the same
per-target binding + capability-intersection machinery.

- **Syntax (D8):** C#-style `[Name(args)]` in **declaration-prefix position** — before class, method,
  field/property, param, and (deferred) return. `[` is illegal at all those positions today, so parsing is
  unambiguous with one-token lookahead. **No attributes on local variables in v1** (the one position where
  `[` already begins a list literal — excluding it removes all ambiguity; frameworks don't target locals).
  Stacked `[A] [B]` and grouped `[A, B]` are both accepted (equivalent).
- **Arguments:** positional and **named** (`Name = value`, a *new* `[ident '='] expr` production — maps to
  C# named attribute properties, per-target options elsewhere). Value grammar: literals, enum-member
  references, and arrays of those. **Non-const / variable values (D11) are supported from day one**, gated
  by capability `attributes.arg.nonConst`: allowed only when *all* configured targets declare support
  (C#/PHP require compile-time constants → their flag is false → the intersection refuses variable values
  whenever C#/PHP is a target; TS/Python-only target sets get them). Loud refusal, never a silent drop.
- **Binding (D9):** reuse the existing binding-arm syntax (do NOT invent a parallel grammar), extended with
  an import/using clause and a `refuse` arm:
  ```
  extern attribute JsonProperty(name: string) {
    actual(csharp)     extern("Newtonsoft.Json.JsonProperty($name)") using "Newtonsoft.Json"
    actual(typescript) extern("JsonProperty($name)")                 import { JsonProperty } from "class-transformer"
    actual(php)        extern("JsonProperty($name)")
    actual(python)     refuse
  }
  ```
  The declared `(params)` signature type-checks each usage via the existing `checkArgs`/`checkConvert` path
  (same as extern-class methods). `$name`/`$0`… placeholders reuse `substBoundTemplate`. **New machinery:**
  a per-target import/using field on `TargetBinding` (it has none today — C# std bindings sidestep it with
  `global::` FQN, but package annotations need real `import`s) + a per-file import accumulator feeding the
  existing `module.imports` emission site + the `refuse` arm.
- **Attachment points & arg-kinds gated by intersection (D10):** attribute targets
  (`attributes.target.{class,method,field,param,return}`) and arg kinds
  (`attributes.arg.{const,nonConst,enum,array}`) are capability-gated per plugin; the supported set is the
  set-AND across configured targets — the *same* mechanism as language features today. **Per-attachment-point
  granularity is mandatory** (Python has class decorators but no *parameter* decorators — a coarse
  `attributes` flag would silently miscompile; the intersection must exclude param attributes when Python is
  targeted). `return`-target attributes (C# `[return: X]`) have no grammar home yet → **deferred**.
- **D12 (non-negotiable):** a used attribute with no binding for a configured target → **loud compile-time
  refusal** naming the attribute and the lacking target(s). This is the backstop that makes emit-only honest.
- **D13 — mechanism-only distribution:** the Core ships only the attribute mechanism; *all* bindings ship as
  packages via P30. This needs a **binding-only package kind** (a partial plugin contributing `extern
  attribute` declarations + their imports, *not* a full target backend — today every plugin must be a
  complete backend with full IR coverage). To avoid N-dependency friction for the common case, bless a
  curated first-party `polyglot-attributes-*` package (one dependency, not N). No auto-included stdlib.

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

**Decision & open item.** P37 §D builds **Model 1** (pass-through, passive-metadata-only) — correct for the
motivating use cases. Behavior-transforming / "functional" attributes remain **out of scope and the way
forward is OPEN** (maintainer undecided as of 2026-07-21). If a specific transform is ever genuinely wanted,
the scope-disciplined path is a **fixed, first-party, compiler-authored lowering** (i.e. "add one language
feature," like Polyglot already synthesizes constructors and value-equality) — **not** a user-programmable
`.pg` macro system. Recorded as a demand-gated deferral (see §G).

## E. Shared prerequisite — keyed capability vocabulary

C6 (operator sub-keys) and D10/D11 (attribute target/arg keys) both need capabilities the current **flat
16-member `Feature` enum** can't express. Generalize the capability lookup to a **string-keyed set with a
fixed `parent:child` form**, **load-validated against a closed enumerated vocabulary** (an unknown key is a
load error — never silently ignored, per the anti-silent-drop rule). The per-target intersection stays
set-AND; the closed-vocabulary discipline of PRD §3.E/§4.11 is explicitly reaffirmed to cover these keys
(sub-keying a fixed entry into a small set of grades is *refinement*, not per-feature growth). Build once;
both workstreams consume it.

## F. Faithfulness / determinism honesty notes (must land in SPEC §3.C/§3.D/§10)

- **H1** — reflection-never rationale + attributes emit-only-only (decision 0).
- **H2** — attributes are outside the §3.C faithfulness guarantee: Polyglot guarantees the spelling +
  attachment land per the binding, **not** identical cross-target runtime behavior (C#/PHP attributes are
  inert metadata; TS/Python decorators execute at definition time). Behavior-*transforming* decorators are
  out of scope; only bindings to a semantically-passive annotation on each target are permitted; no binding
  ⇒ D12 refusal.
- **H3** — `as` = checked conversion → `null` (runtime guard on TS/Python/PHP, never TS assertion); interface
  `is`/`as` refused when a configured target lacks runtime interface identity; `is`-binding narrows in-branch
  only.
- **H4** — user operators lower to static methods on the synthetic-dispatch target (TS) and stay native on
  C#/Python; comparison vs the `null` literal is a null check, not a user-`eq` call; explicit conversions
  only.
- **H5** — capability-vocabulary governance: operator sub-keys and attribute keys are additive to the closed,
  load-validated set via a fixed `parent:child` form — not a growth license.

## G. Scope boundaries & deferred follow-ups

- **Refused (loud diagnostic, never silent):** runtime reflection / attribute read-back; new operator
  *symbols*; implicit user conversion operators; attributes on local variables (v1); an attribute used with
  no binding for a configured target; variable attribute-arg values when C#/PHP is targeted.
- **Deferred (demand-gated follow-ups, recorded not built):** cross-package operator *resolution* (adopt the
  static *shape* now; no consumer + no resolver path today); `return:`-target and type-parameter attributes
  (new syntax, no grammar home); an auto-included attribute stdlib package; **"functional" / behavior-
  transforming attributes (Model 2 — a compile-time transform/macro subsystem)** — the way forward is OPEN
  (maintainer undecided 2026-07-21); if ever pursued, a fixed first-party compiler-authored lowering, not a
  user-programmable `.pg` macro platform (full rationale + empirical C# demonstration in §D.1).

## H. Testing strategy (see PLAN.md matrix)

Operators / `is` / `as` / compound-assign / conversions are **differentially executable** — the conformance
suite is the safety net (critical for the C5 rework, which regenerates 9 existing operator/equality
goldens). Attributes hook target *frameworks* absent from the harness → **golden-only** (assert emitted
per-target text + required imports). Capability gates (C6 sub-keys, D10/D11, B4, D12) get **refusal
fixtures**. The A-rename adds a golden asserting emitted TS/JS never yields a bare `.constructor` access.
