# MintPlayer.Polyglot — Milestone Plan

Companion to [POLYGLOT_PRD.md](POLYGLOT_PRD.md). Each milestone names a concrete deliverable and a **gate**
(the observable thing that says it's done). This is a long-haul craft project — milestones are ordered but
unhurried, and the §3 support/refuse contract in the PRD is the law every milestone is checked against.

---

## P0 — Solution skeleton ✅ done (built green 2026-06-28)
The Visual Studio solution and sources are committed: `MintPlayer.Polyglot.Core` (static lib),
`MintPlayer.Polyglot.Cli` (the `polyglot` exe answering `--version`/`--help`, `build` stubbed), and
`MintPlayer.Polyglot.Tests` (a tiny zero-dependency assert harness). C++20, x64 Debug/Release.
*Gate (closed):* the hand-authored `.sln`/`.vcxproj` build with **0 warnings / 0 errors** via the
VS 18 "Insiders" MSBuild. The projects target toolset **v145** (→ MSVC 14.51), so they require VS 2026 —
a deliberate VS-2026-only pin (see CLAUDE.md for paths). `MintPlayer.Polyglot.Cli.exe --version` prints
`0.0.1`; `...Tests.exe` reports all-pass.

## P1 — Language design v0.1 ✅ locked (2026-06-28)
Design the source language on paper before any compiler code. Write a grammar (EBNF) + a short language
spec + 5–10 sample `.pg` programs spanning the §3.A supported surface (a function, a struct/record, an
enum + pattern match, a generic, an iterator, exception handling, a `using`). Deliberately exclude the
§3.B refused features from the grammar entirely.
*Delivered:* `docs/lang/grammar.ebnf` (admits only §3.A; §3.B unspeakable), `docs/lang/SPEC.md` (spec with
per-feature C#/TS lowering tables + the §3.C relaxation list + §3.B refusals), and `docs/lang/samples/` —
9 focused samples (`01_functions` … `09_strings`) covering every §3.A feature + `fruitcake_sketch.pg`,
the surface test modeled 1:1 on `MintPlayer.AI`'s `FruitCakeWorld.cs`. Key design call: only `class`
(mutable, reference identity) and `record` (immutable, structural equality) — **mutable value types
(`struct`) are refused for v0.1** because they are the one construct whose value/reference identity
diverges between C# and TS (SPEC §4.2).
*Gate (closed):* samples reviewed across the design sessions; v0.1 locked and used as the basis for the
P2 MVP. (Semicolons clarified as an optional separator during P2 — grammar/SPEC updated to match.)

## P2 — Walking skeleton (MVP) ✅ done (2026-06-28) ★ thinnest end-to-end slice
Took a *minimal* language subset — `fn`, `i32`/`f64` + arithmetic, `let`/`var`, `if`/`while`, function
calls, `print` — **all the way through** the pipeline: lexer → recursive-descent parser → typer (name
resolution + types, annotated tree = the typed IR) → **hand-written C# *and* TS pretty-printers** (no
Roslyn/ts-morph — §4.3). `polyglot build foo.pg` emits `foo.cs` + `foo.ts`. This front-loaded the
project's biggest architectural bet — that **one high-level IR serves both targets** — and stood up the
crown-jewel **differential conformance test** at P2 instead of P5. Later milestones *widen* each pass.
*Delivered:* `src/MintPlayer.Polyglot.Core` (diagnostics/token/ast/lexer/parser/sema/emit_csharp/
emit_typescript/compiler); `polyglot build` in the CLI; 20 in-process unit/golden tests; and
`tests/conformance/` (`run-diff.ps1` + `programs/arithmetic.pg`).
*Gate (closed):* `arithmetic.pg` → emitted C# compiles+runs under `dotnet` and TS runs under `node`
(type-stripping), with **identical stdout** (`128 / 28 / 50`); `run-diff.ps1` is green. Semicolons are an
optional separator (statements are newline-terminated) — grammar/SPEC aligned to the samples.

## P3 — Full front-end (lexer + parser) ✅ done (2026-06-29)
Widened the MVP front-end to the entire P1 grammar: full token set + a recursive-descent parser over the
whole surface — expressions (member/index/call, `?.`/`??`/`!`, ranges, lambdas, list/tuple/`with`, if-expr,
`match` + patterns), the `TypeRef` type grammar (named/generic/tuple/function/nullable), all statements
(`for`/`while`/`do`-less, `try`/`catch`/`when`/`finally`, `throw`, `use`, `yield`, break/continue,
compound/lvalue assignment), declarations (`fn`/`record`/`class`/`interface`/`extension`/`enum`/`union`,
members, generic params/bounds, default params), and `import`. A canonical `.pg` pretty-printer
(`pg_printer`, exposed as `polyglot fmt`) is the fidelity surface.
*Gate (closed):* all 10 `docs/lang/samples/*.pg` (incl. `fruitcake_sketch`) round-trip source → AST →
source idempotently — `tests/fidelity/run-roundtrip.ps1`, wired into `/build-and-test`. Malformed input
yields `file:line:col` diagnostics. Built incrementally (P3 1 → 3e-3), green at every step.
String interpolation is fully parsed (chunks + hole expressions, re-entrant lexer). *Deferred (small, not
the gate):* the `{ get/set }` property-accessor form (samples use read-only `=> expr` properties only),
nested strings inside an interpolation hole, and nested-generic edge cases beyond the samples. The
trivia-bearing lexer keeps comments/whitespace for *later* readable output; the P3 printer is canonical
(re-formats), not trivia-preserving.

## P4 — Full semantics + typed IR ✅ done (2026-06-29)
Built the static type system + the **separate typed IR** (per the design decision to make the IR its own
tree, not just the typed AST): name/type resolution across the whole declaration surface (unknown-type,
duplicate, missing-member, wrong-arity diagnostics); nominal expression typing (members, construction,
method calls, operator-overload lookup, `this`/`super`, `EnumName.Case`, nullability) — lenient on
unknown/generic/std types so there are no false positives; pattern-match exhaustiveness (union/enum/bool +
catch-all required for non-enumerable scalars); a dedicated IR (`ir.hpp`: typed tagged hierarchy carrying
resolved decisions — the `print` intrinsic, the `main` entry) produced by a lowering pass (`lower.hpp`,
AST→IR after sema); and the **backends rerouted to emit from the IR**, so the pipeline is now
lexer→parser→sema→lower→IR→backend (PRD §4.1/§4.2).
*Gate (closed):* type errors reported with `file:line:col`; a deterministic **typed IR dump** (`<expr>:<type>`)
verified by unit tests; resolution/typing/exhaustiveness covered by ~25 sema/IR unit tests. Conformance
unchanged (arithmetic.pg → IR → identical C#/TS).
*Notes:* full semantic checking runs on self-contained programs; the std-using P1 samples get end-to-end
type-checking at P7 (when `List`/`Error`/`sqrt`/… exist). The IR/lowering cover the MVP subset today and
widen to the full §3.A surface in P5. Expr nodes now carry a resolved `TypeRef`. Generic *instantiation*
substitution and full overload *mangling* are best-effort/lenient for now (refined as P5/P6 need them).

## P5 — Backends to the full §3.A surface ✅ done (2026-06-29)
Widen both hand-written pretty-printers from the MVP subset to the **entire supported surface**: records,
enums, unions + pattern matching, iterators, exceptions, `using`/disposal, extension methods, operators,
properties/indexers, closures — idiomatic in each target. Golden-output baselines checked in for **both**
targets; the differential conformance suite (stood up at P2) grows to cover the surface.
Also introduce the **backend-interface seam**: a small `Backend` abstraction (`name()` + `emit(unit)`)
selected via a registry, replacing the `if/else` on `Target` in `compile()`. Backends stay compiled-in,
but this is the shape the P9 declarative-plugin API grows from — the natural moment, with two *complete*
native backends to generalize across (the design note's "extracted, not guessed").
*Gate:* P1 samples emit C# that compiles under `dotnet build` and TS that type-checks under `tsc` + runs
under Node, both with expected output; golden baselines green; the differential suite passes.
*Delivered (2026-06-29):* the **backend-interface seam** is in (a `Backend` abstraction +
registry; `compile()` selects via `findBackend`). The IR/lowering/both backends now cover **records**
(fields, methods, operators, properties), **enums**, **unions + pattern matching** (exhaustiveness, ctor
patterns/binders), **operators & properties** (C# `operator +` / expr-bodied property vs TS `.plus()` /
getter), **classes** (mutable reference types with `init`, **inheritance + `super(...)`**), **`for…in`**
over ranges and iterables, **iterators (`yield`** → C# `IEnumerable`+`yield return` / TS `function*`),
**exceptions** (`throw` + `try`/`catch`/`when`/`finally`; TS gets an `instanceof`/guard dispatch chain),
and **`use`/disposal** (→ `try/finally` + `.dispose()`). `Iterable` and `Error` are registered as core
builtin types. The differential suite grew from 1 → 11 self-contained programs (arithmetic, records,
vec2, enums, unions, counter, forrange, iterator, exceptions, inheritance, disposal), all agreeing.
*Lambda syntax* also admits the bare single-parameter form `x => …`. **Closures/lambdas** emit (native
arrow functions both sides; function types → `Func`/`Action` / `(a:T)=>R`). **Extension methods** emit:
C# `static class Extensions` of `this`-methods (call site `x.m()` survives), TS free functions (call site
becomes `m(x)`) — the §3.E call-site divergence, exercised by `extensions.pg`.
**§3.E per-target capability gating is implemented early and active**, not just designed: `backend.hpp`
has a closed `Feature` enum + `Backend::supports(Feature)`; `capability.cpp` walks the AST for used
features and `compile()` refuses (per target, intersection-wise) any a target can't emit — a clear error,
never a miscompile. C#/TS declare the full set, so nothing is gated yet; a `StubBackend` unit test proves
the gate bites and names the feature + target. Finally **generics** emit: `<T>` parameter lists + bounds
(C# `where T : …`, TS `<T extends …>`) on functions/records/classes/methods/extensions, and explicit
construction type args (`Box<i32>(7)` → `new Box<int>(7)` / `new Box<number>(7)`).
*Gate (closed):* full §3.A surface lowers + emits; **14 differential programs** agree across C#/TS; 10/10
round-trip; capability gate proven by unit test. Golden `tsc`/`dotnet build` baselines deferred to P7
(std-using samples) — the differential run already compiles+runs both targets per program.

## P6 — Faithfulness pass ✅ done (2026-06-29)
Implement the §3.C relaxations *as documented behaviour*: int32/uint masking (`|0`/`>>>0`/`Math.imul`),
opt-in `Math.fround` strict floats, `BigInt` for int64, structural equality/hashing, null/undefined
normalization. Enforce the §3.B **refusals** with clear, actionable compiler errors (not miscompiles).
*Gate:* a numeric conformance suite passes within tolerance across both targets; every refused feature has
a refusal test asserting the diagnostic; the relaxation list is written up in the spec.
*Delivered:* §3.B **refusal diagnostics** (threads/locks, `decimal`, pointers/unsafe, `dynamic`, expression
trees, reflection → "Polyglot refuses X" with a unit test each); **all integer widths faithful** — i8/i16/
u8/u16/u32 narrowing (TS bit-ops; C# casts), i32 `|0`/`Math.imul`, **i64/u64 → BigInt** with 64-bit wrap;
**structural value equality** for records (C# native, TS generated `equals`); **explicit casts `(T)x`** +
**implicit lossless widening** (replacing conversion methods); and **function overloading** (compile-time
resolution; C# native name / TS param-mangled). 8 new differential programs (int_overflow, int64,
int_widths, equality, casts, widening, overloading) + refusal/overload unit tests. *Deferred to its own
step (low value):* opt-in strict-f32 `Math.fround`, null/undefined normalization (better with std).
**Statement-level `lock`/`unsafe` refusals ✅ done** (2026-07-01): parseStmt catches the C#-habit form and
emits a targeted §3.B "Polyglot refuses" message + brace-balanced-skips the construct (no cascade). Numeric
conversion design recorded in PRD §3.A / SPEC §3.

## P7 — Std core + expect/actual + FFI (the three plugin mechanisms, as first-party code) ✅ done (2026-06-29)
A minimal portable standard library in `.pg` (math, basic collections, iterators) compiled to both
targets. The **target-gated `expect`/`actual`** mechanism (portable core forbidden from touching platform
APIs; per-target `actual` impls) and an `extern`/inline-target **FFI hatch**. This is where the three
mechanisms of the plugin architecture — *binding*, *replacement*, *capability* — are proven **as
first-party code** behind a backend interface designed to become the plugin API (see the plugin design
note, below). This milestone also introduces **static methods on types** and the first use of them:
**string↔number parsing as static type methods** — `i32.parse(s)`/`i64.parse(s)`/`f64.parse(s)` (throwing)
and `tryParse(s): T?` (nullable) — realized per target (C# `int.Parse`/`TryParse`, JS `Number`/parse +
range checks). Parsing is *not* a cast (PRD §3.A/§4.4): a cast `(T)x` converts between numeric types, parse
reads text. (This unblocks the `.pg` samples that currently sketch `s.toI32()` — now `i32.parse(s)`.)
*Gate:* a program using a portable std API + one `expect`/`actual` capability (e.g. current time) builds
and runs identically on both targets; `i32.parse("42")` round-trips on both; the portable core cannot
reference `document`/`System.*` (compiler-enforced, with a test proving the rejection).
*Delivered:* **static methods** on types (`Type.method()`, native on both); **`i32.parse`/`i64.parse`/
`f64.parse`** (string→number, per-target idiom); the **`Math`** namespace (`sqrt`/`ln`/`abs`/`min`/`max`/
`floor`/`ceil` → `System.Math.*` / `Math.*`) — the **replacement** mechanism; **`expect`/`actual`** target-
gated capabilities (each backend emits only its matching `actual`) — the **capability** mechanism;
**`extern("…")`** raw-code FFI — the **binding** mechanism; and the **portable-core guard** (`extern`
refused outside a target-gated `actual`). 6 new differential programs (static_methods, parse, math,
expect_actual, extern_ffi) + extern-guard unit tests. *Deviation from the sketch:* the std is currently
**compiler-builtin intrinsics** (Math/parse), not yet a portable std *written in `.pg`* with an import
mechanism — that (and `tryParse`, collections) is a further step; the three plugin mechanisms are proven,
which is the milestone's point. At P9 these intrinsics become declarative plugin data.

## P8 — Dogfood: the FruitCake physics ★ north star — ✅ DONE
Expressed the MintPlayer.AI FruitCake circle-physics solver in `.pg`
(`docs/lang/samples/fruitcake_sketch.pg`): the full sequential-impulse circle solver with rigid-body
rotation, walls, deferred same-tier merges, settle-to-rest. Generates `FruitCakeWorld`-equivalent C# and
`fruit-cake-physics`-equivalent TS from one source.

**Delivered (beyond the original plan):**
- Written in **`f64`** so the solver's `+ − × ÷ √` are **bit-exact across .NET and JS** (§3.D). The two
  emitted twins therefore agree *to the bit* — a stronger result than the hand-ported `float`/`number`
  twins, which diverge from each other. So the conformance gate asserts **byte-identical** stdout
  (`bodies=1 scored=1` on a scripted two-fruit drop+merge), not just behavioural tolerance.
- **`List<T>` as a first-party `.pg` std type** (the principled path, not a compiler intrinsic) via a new
  **binding mechanism**: a method/property body of `actual(target) extern("…template…")` arms with
  `$this` (receiver) / `$0…` (args) substituted at each call site; a `$this = …` arm emits a receiver
  *assignment* (`list.clear()` → C# `xs.Clear()` / TS `xs = []`; `list.removeAll(p)` → C# `xs.RemoveAll(p)`
  / TS `xs = xs.filter(e => !((p)(e)))`). `extern class` = native-backed type (not emitted). Embedded std
  module linked on `import std.collections.{ List }` (no filesystem resolver yet). List literal / `lst[i]` /
  `for x in` / `for (a,b) in` element typing is compiler-level (syntax the bindings can't express).
- Lowered + emitted for the first time (all previously fell through to a silent `0`): `null` literal,
  `x!` null-assert (→ cast to the non-null type, unwrapping C# `Nullable<T>`), `??`, string interpolation,
  index, tuple literal, top-level `const`/`let` globals, tuple destructuring in `for`.

*Gate (met):* `fruitcake` is conformance program #27; generated C# and TS produce identical stdout. The
27-program differential suite + 10-sample round-trip + in-process unit tests are all green. Polyglot has
earned the right to own that physics; the hand-ports can retire while the conformance test guards the
generator. *Not yet done here:* wiring the generated output back into the live MintPlayer.AI repo (out of
scope — that repo's build is not run from here) and a real module-resolution system (std is embedded source).

## P9 — Declarative backend engine + DSL — ✅ done (to the principled two-backend extent)
**Status: ✅ complete — the declarative DSL is extracted, validated across three backends.** `EmitterBase`
owns the statement walk, the output buffer/indentation, and the block abstraction, and reads all per-target
data through one `spec()` accessor; **`BackendSpec` is the extracted DSL — every per-target datum**: scalar/
suffix/operator/bracket tables + block style + statement terminator + throw keyword + bool/null literal
spellings (string escaping is the shared `renderString` primitive; `renderDelimited`/`renderArgs`/`renderCond`
capture the target-identical expression structure). Every backend — including the non-sibling Python — is now
a `{Spec + Hooks}` instance. The residual **hook surface** is genuine per-target behavior: `emitExpr`/
`emitStmtTarget`/`localDecl`/`yieldStmt`/`rethrowStmt` + the declaration emitters. Extraction proved (and the
third backend confirmed) the expression walk and declaration shapes are **irreducibly per-target** — they
can't be flattened to data without an embedded DSL the zero-dep core forbids (§4's "full-power local tier").
The slice log (1 → 4f, then the DSL-consolidation slices: renderString, spec-data, bool/null) and the realized
C++ shape are in `design/backend-spec.md` §3–4.
**Concrete design + extraction map: [`../design/backend-spec.md`](../design/backend-spec.md).** A structural
catalog of both emitters found the split is ~70% tabular / ~30% imperative, so a backend = **Spec (declarative
data)** + **Hooks (C++ for the imperative 30%: `tsConvert`, TS `try` lowering, numeric narrowing, operator-
method dispatch, record equality, `Match`, interpolation)** + capability set, over **one shared emit engine**.
Migration is incremental, each slice a byte-for-byte no-op enforced by the golden/differential/round-trip
gates. **Slice 1 ✅:** the scalar type-leaf table extracted into `BackendSpec` (`backend_spec.hpp`); both
emitters consult their spec instead of hardcoded type ladders. **Slice 2 ✅:** the remaining tabular operator/
literal data is now in the spec — `intSuffix` (literal suffixes, since slice 1) plus the new `binaryOp`
spelling table (TS `==`→`===`/`!=`→`!==`; C# identity) consulted via `BackendSpec::binOp()` at every binary
emission; precedence stays a shared free function (`operatorPrecedence`, identical across targets, so not
per-backend data). Numeric-overflow wrapping and operator-overload dispatch stay imperative Hooks.
**Slice 3a ✅ (engine seam seeded):** the first per-node templates + the shared render primitives that are
the seed of the `SpecEmitter` — `renderDelimited(DelimitedTemplate, children)` (open/sep/close affix table
on the spec; e.g. tuple `(a, b)` vs `[a, b]`) and `renderCond(c,t,e)` (identical across targets, so a shared
rule not per-backend data), both in `backend_spec.hpp`. `Cond` and `Tuple` in both emitters now route through
them; children are emitted into explicit left-to-right locals first (C++ leaves `operator+`/argument order
unspecified, and a child may bump a per-emitter counter). Byte-for-byte no-op: unit + 37/37 conformance +
10/10 emit + 10/10 round-trip. **Slice 3b ✅:** the call-argument family now routes through the shared
`renderArgs(children)` primitive (the `(a, b, c)` affix is identical across targets, so an engine constant,
not spec data) — C# `Call`/`MethodCall`/`New`/`MakeCase` and TS `Call`/`MethodCall` (plain + extension form)/
`New`. Args are emitted into a left-to-right vector first, then wrapped; byte-for-byte no-op (all four gates
green). **Slice 3c ✅:** `ListLit` now renders via `renderDelimited` too — TS's static `[…]` affix lives in
the spec `delimited` table (`"list"`), while C#'s open affix is *computed* (`new …List<elem> { `, carrying
the rendered element type) and passed to the same primitive, proving `renderDelimited` handles dynamic
affixes, not just spec constants. All delimited/list-shaped expression nodes now route through the shared
primitives (byte-for-byte no-op, all gates green). (The slices below then lifted the *statement* walk into an
engine class incrementally — keeping the shared surface growing rather than a risky big-bang — and found, per
the top Status, that the *expression* walk could not be lifted the same way: it is per-target by shape.)
**Slice 4a ✅ (the engine class is born):** a new `EmitterBase` (`include/.../emitter_base.hpp`) owns the
byte-identical walk machinery — the `out_`/`indent_` buffer, `line()`, `inlineBlock()` — and the statement
dispatch; the leaf statements whose spelling is identical across targets (`Assign`/`ExprStmt`/`Return`) are
rendered there, with `emitStmt`'s `default` routing every other kind to a pure-virtual `emitStmtTarget` the
concrete backends override (alongside the pure-virtual `emitExpr`). `CSharpEmitter`/`TypeScriptEmitter` now
`: public EmitterBase`, with their duplicated state/helpers/leaf-cases deleted. The leaf-statement code was
copied verbatim (same `+`-chains), so no evaluation-order change. Byte-for-byte no-op: all four gates green.
**Slice 4b ✅ (brace-style abstraction):** the one real divergence in block control flow — C# Allman (`{` on
its own line) vs TS K&R (`{` on the head line) — is now a single `bracesOnHeadLine()` hook + a shared
`headBlock(head, body)` / `blockBody(body)` pair on `EmitterBase`. `While` (head content `while (cond)` is
identical across targets) moved fully into the base; `For` keeps its target-specific head-building
(`foreach…in` vs `for…of`, range/tuple forms) in the concrete emitter but wraps the block via `headBlock`.
Byte-for-byte no-op: all four gates green. **Slice 4c ✅:** `If` joins `While` in the base — its `if (cond)`
head is identical across targets, and the `else` arm's only divergence (K&R merges the close+else+open onto
one `} else {` line; Allman puts `}`/`else`/`{` on separate lines) is captured by the same
`bracesOnHeadLine()` hook. Removed from both concrete emitters; all four gates green. The whole `if/while/for`
trio now lives in `EmitterBase`. **Slice 4d ✅ (statement tail):** `Let`, `Yield`, `Throw`, and `Use` all
move into the base; only their *spellings* diverge, captured by three tiny hooks — `localDecl(name,
isMutable)` (`var <csIdent>` / `let|const <name>`; shared by `Let` **and** `Use`), `yieldStmt(value,
hasValue)` (`yield return v;`/`yield break;` vs `yield v;`/`return;`), and `rethrowStmt()` (`throw;` vs
`throw __e;`; the value-bearing `throw v;` is identical and stays in the base). `Use` reuses `localDecl` +
the brace hook for its try/finally+dispose shape. After this, **each concrete emitter's `emitStmtTarget`
switch is down to just `For` + `Try`** — `For` (target-specific head) and `Try` (C# native `catch…when` vs
TS instanceof-dispatch) are the only statements still per-target. Byte-for-byte no-op: all four gates green.
**Slice 4e ✅ (block emission unified; declaration shapes assessed as per-target):** a structural survey of
both declaration emitters found the *shapes* are fundamentally per-target — C# `record`/`abstract record`
unions/real operators/indexers/properties/`this`-extensions vs TS classes/tagged-union type-aliases/getters/
free-functions/structural-`equals`. Forcing these into shared templates would mean many shallow hooks hiding
little (the leaky abstraction the §4.3 principles warn against), so the declaration bodies legitimately *are*
the per-target "imperative 30%" and stay in the concrete emitters. The one genuine consolidation taken: the
two `emitBlock` methods (a same-name/different-contract footgun — C# Allman-braced vs TS brace-less) are
**deleted**; every block now routes through the base's single `headBlock`/`blockBody` pair (function, method,
operator, extension, class-init bodies, and `Try`'s try/catch/finally). Byte-for-byte no-op across every
function/method body: all four gates green. **Slice 4f ✅ (Hook tier formalized — P9 closed):** the
backend↔engine **hook surface** is documented in `emitter_base.hpp` as the contract it is — two granularities,
wholesale per-target walks (`emitExpr`, `emitStmtTarget`) and fine-grained spellings (`bracesOnHeadLine`/
`localDecl`/`yieldStmt`/`rethrowStmt`); `design/backend-spec.md` §3 was rewritten to describe the *realized*
architecture (correcting the original speculative `SpecEmitter`/per-node-`BackendHooks` sketch). The residual
imperative codegen (`Cast`/`tsConvert`, `Match`, `Try`, interpolation, numeric narrowing, operator-method
dispatch, the declaration emitters) is the backend's private imperative tier behind those walks. The full
declarative-scaffold DSL for declarations is deferred until a third backend exists to extract it from (the
"never guess the format" discipline).

The backends, generalized. *Extract* a declarative backend format from the two **native** C#/TS backends
(P4/P5) — a rule/template per IR node (context-aware: precedence, expr-vs-stmt position), the std-type
mapping, operator/keyword tables, naming/import rules, and the build-project scaffold. Build the core's
**declarative emit engine** that interprets a spec, and **re-express C# and TS as declarative specs**.
Critically, the DSL is extracted from working backends, never guessed (the §4.3 "design it twice"
discipline applied to the format itself); a **local full-power plugin tier** covers what the DSL can't yet
express. See **[`../design/plugins-and-targets.md`](../design/plugins-and-targets.md)** §4/§7.
Each backend spec also declares its **capability set** — the named §3.E flags (`extensionMethods`,
`operatorOverloading`, `properties`, `iterators`, `patternMatching`, …) it can emit. C# and TS both declare
the full §3.A set, so the format carries capabilities from the start even though nothing is gated yet.
*Gate:* C#/TS emitted via the declarative specs match the native backends' golden output byte-for-byte;
each spec carries a capability set (both = full §3.A).

### P9-V — Third backend (Python): engine-validation spike — ✅ done (36/36 conformance programs)
P9 deferred the data-only declarative DSL until a **third backend** exists to extract it from (never guess —
§4.3). Rather than wait for full P10 distribution, a **native Python backend** was brought up purely to
*validate* that the P9 engine generalizes beyond the two brace-family backends it was extracted from — and to
be the artifact the DSL is eventually extracted from. Python is a non-sibling target (colon+indent, no
statement terminators, no `var`, builtin `print`), so it stresses the engine hard. It now covers the **full
§3.A surface — all 36 conformance programs agree with the C# oracle** (`tests/conformance/run-python.ps1`,
which globs every program like run-diff). `--target python` is opt-in (kept out of the default cs+ts build).

**Findings (the payoff of a non-sibling backend):**
- The engine was **not** fully target-neutral — it was brace-family-specific. Python forced a real
  generalization (**P-1a**: `bracesOnHeadLine()` bool → a 3-way `BlockStyle` {BracesAllman, BracesKnR,
  ColonIndent} + a `stmtEnd()` terminator hook; later a `throwKeyword()` hook + a block-style-agnostic `Use`),
  each a verified C#/TS no-op. *After* that, the shared statement layer served Python unchanged — the
  abstraction holds, once honestly generalized. Declarations stayed per-target (hand-written in
  `emit_python.cpp`), as P9 predicted; Python's emission is often *cleaner* (structural `==` via `__eq__`; no
  `new`; arbitrary-precision `int` keeps i64 exact past 2⁵³; native dunders/generators; `except Type as e`).
- **Three latent bugs a sibling backend would never surface, all fixed at the root:** (1) Python's builtin
  `print` collides (`__builtins__.print` + lowercase bools); (2) the always-linked `extern class Error`'s
  `message` tripped `Properties` for *every* program (`capability.cpp`: the Collector skips extern-class
  members — they're bindings); (3) **`break`/`continue` were SILENTLY DROPPED** in lowering (`default:
  return nullptr`) for *all* targets — a §3.B miscompile the C#/TS diff gate couldn't catch (both dropped them
  identically, so they still agreed). Fixed by adding `ir::Break`/`Continue` + emitting them in the shared
  engine (identical spelling on all three targets; diff gate stays green).

**Slices (all ✅):** engine generalization (no-op) → walking skeleton → records/classes/casts → closures +
parse → iterators + operators(dunders) + properties → enums/unions/match → 5 already-supported programs →
static methods → std bindings infra (ir::Bound pyTemplate) + Math → List/collections → extensions + strings →
exceptions + inheritance (Error→Exception) → expect/actual + extern arms → overloading (name mangling) →
integer faithfulness (width-mask overflow + truncating div/rem) → disposal → **fruitcake** (the north star:
keyword escaping, class consts/statics, field initializers, default params, string interpolation, tuples,
`?.`/`??`/`!`, + the break/continue fix). `PythonBackend::supports` now returns `true` for everything (the
StubBackend test still proves gating bites).

**The declarative DSL (P9 endpoint) was extracted from three backends instead of guessed ✅ (2026-07-01):**
all per-target data consolidated into `BackendSpec` (blockStyle/stmtEnd/throwKeyword lifted from
constant-hooks; bool/null spellings; string escaping → shared `renderString`), each backend reduced to
`{Spec + Hooks}`, Python given its own Spec. See `design/backend-spec.md` header + §3.

**Follow-up ✅ (2026-07-01): target-gated portability refusal.** Closed the §3.B gap surfaced by the spike —
a call to a portable function (one with `actual` impls) on a target that has no `actual` for it now **refuses
with a call-site diagnostic** ("portable function 'f' has no 'actual' implementation for target 'python'")
instead of silently emitting a call to an undefined function. The check lives in `checkCapabilities`
(target-aware) and is **keyed on call sites**, so an *unused* portable fn missing this target's arm is fine
(e.g. `std.io.readText` has no python arm, but a python program that never calls it is unaffected — which is
why all 36 python programs still pass). Unit-tested (refuse on python / compile on C# / unused-is-fine).

**Audit follow-up ✅ (2026-07-01): tuple patterns in `match` refused (were a silent miscompile).** The same
audit that found the break/continue drop found another: a **tuple pattern in `match`** (`match p { (a, b) => … }`)
binds + type-checks in sema but has no lowering — it collapsed to a wildcard and emitted a switch arm
referencing undefined binders (invalid code). Now refused cleanly in sema (`declarePattern`) with a
diagnostic pointing at destructuring-after-match or a `for (a, b) in …` binding (which *is* supported — a
separate path). Full tuple-pattern support (positional binders → `Item1`/`[0]` per target + nested-literal
exhaustiveness) is a deliberate future slice, not an audit side-fix. With this, **every `ir` StmtKind is
lowered** (the break/continue fix closed the statement default) and the remaining silent fallbacks are gone.

## P10 — Plugin distribution + ecosystem (the endpoint of §4.4)
The downloadable, declarative plugin system: a **workspace config (`pgconfig.json`)** declaring target
*environments* (desktop/web/mobile/…) + plugins+versions; **download → shared cache → verify → lockfile**
(declarative data only — no executable code fetched; integrity-pinned, zip-slip-safe); **availability
resolution** by target + environment (off-target use is a compile error, never a miscompile); and
**build-dependency threading** (a plugin declares the NuGet `PackageReference`s/SDK or npm deps its output
needs; the core emits a buildable project including them). Trust model + open decisions in the design note.
This is also where **§3.E capability-gating** first *bites*: the usable §3.A surface for a project becomes
the **intersection** of the configured backends' declared capability sets, and using a feature outside it
is refused at compile time naming the capability + the lacking target (distinct from a §3.B global refusal).
A natural exercise: the downloaded Python backend declares a capability set that omits, say, call-site-
preserving `extensionMethods`, and a program using extension methods while targeting Python is refused.
*Gate:* adding a **downloaded declarative Python backend** *and* a target-scoped binding plugin (e.g.
WinForms, with its PackageReferences) requires **no core change** — only `pgconfig.json` + downloads; a program
using them emits a buildable project, and wrong-target/-environment use **and use of a feature outside the
target intersection** are each rejected with a clear, distinct diagnostic.

**Prerequisite mechanism gap — ✅ closed 2026-06-30 (the type-mapping/construction slice; full P10 plugin
distribution still pending).** An `extern class` now binds **its own type-name → target type** (a
`type { actual(target) extern("…$0…") }` block; `$0,$1` = rendered type args, so `List<T>`→C# `…List<$0>` /
TS `$0[]`) **and its construction** (binding arms on `init`; `$T` = the mapped type, `$0,…` = ctor args;
`Type(args)` lowers to `ir::Bound`). Carried on the IR as an `ir::ExternType` registry that `csType`/`tsType`
consult per-emit. **`List` is dogfooded** onto this (its old `csType`/`tsType`/`New` hardcoding is gone).
Remaining hardcoded core type: **`Error`** (`csType`→`System.Exception`, the `ir::New` Error branch, and the
`Error.message` binding in `lower`) and **`Iterable`** (type-only) — modelling them as `extern class`es in an
always-linked **core module** is the last dogfood step (backlog below). This is the "Binding" mechanism
(plugins-and-targets.md §2) at its complete form. See `design/backend-spec.md` §4a.

**Distribution model — resolved 2026-07-01 (design note §6.1):** no per-plugin executables; a plugin is a
declarative artifact fetched from a feed by name+version, and **`polyglot install <plugin>[@version]`** is the
single trusted writer of a **global per-user registry** (`%APPDATA%\polyglot\registry.json` / XDG), distinct from
the per-project `pgconfig.json` that pins which installed plugins a workspace uses. Feed candidate is npm (reuse
versioning/integrity/CDN, **consumed data-only — no lifecycle scripts**), with a generic URL/file-hosting fallback
and a feed-agnostic `{source, integrity}` registry entry. Rejected: the "ship an .exe per plugin that self-registers"
idea (reintroduces fetch-and-run; every plugin writing the shared registry invites corruption/trust problems).

**Editor tie-in:** once backends are downloadable, the **editor's target list must come from the registry**, not a
hardcode. Add a server-advertised `polyglot/targets` list (`{ id, displayName, fileExtension }` per registered
backend) so the P17 preview ("Show Generated Output" + the Outputs tree) picks up plugin backends with no client
change. Currently hardcoded in `extension.js` `TARGETS` (`FIXME(P10)`); see PLAN §P17 deferred tail.

## P11 — Build integration: the `.pg`-aware NuGet (and npm) on-ramp
Make adoption frictionless: a developer adds a package to an ordinary C# project, drops in `.pg` files,
runs `dotnet build`, and the `.pg` is transpiled to C# and compiled into the assembly with **no manual
step and no extra SDK/runtime** — the payoff of the §4.3 self-contained native CLI (the consuming dev
already has the .NET SDK; the transpiler ships in the package). This milestone depends only on a stable
CLI (the `.pg → .cs` path works from P5), **not** on the P9/P10 plugin engine — it can ship independently;
it sits here because it's an ecosystem/distribution concern. A sibling **npm + build-script** story does
the same for TypeScript projects.

**Design (modeled on `Grpc.Tools` — the closest prior art: a *native, non-.NET* compiler invoked at build
to turn a custom source into `.cs` that joins `@(Compile)`; a Roslyn source generator can't be used because
the input is a foreign language and the transpiler is a native binary):**
- **Package** `MintPlayer.Polyglot.MSBuild` ships the native `polyglot` CLI **per host RID** (win/linux/osx
  × x64/arm64) plus `build/<id>.props` + `build/<id>.targets` (auto-imported: props near the top, targets
  near the bottom of the consuming project). `.props` resolves the host RID and tool path from
  `$(MSBuildThisFileDirectory)` (via `$(NETCoreSdkPortableRuntimeIdentifier)`), with an env-var/property
  **override hook** for CI; on unix it `chmod +x`'s the binary (NuGet drops the exec bit).
- **Non-transitive by construction:** assets go in **`build/`** (not `buildTransitive/`), and the package is
  marked **`DevelopmentDependency=true`** so it never flows to downstream consumers or into published
  output; consumers may reinforce with `PrivateAssets="all"`. Installing it in project A does **not** impose
  it on a project that references A.
- **Hook + incrementality:** a target `BeforeTargets="CoreCompile"` transpiles each `.pg` → `*.g.cs` into
  `$(IntermediateOutputPath)` (`obj/`, so no source-tree noise and no double-compile against the SDK's
  default `**/*.cs` glob) and adds them to `@(Compile)` + `@(FileWrites)` (for clean/rebuild). `Inputs`/
  `Outputs` (including the tool path, so a CLI upgrade forces regen) skip unchanged files; a companion
  always-run target re-adds the existing generated files when the transpile target is skipped (else the
  compiler loses them). Uses the `Exec` task (no managed MSBuild task — keeps the zero-dep ethos).
- **Design-time builds run the transpile** (do not skip on `$(DesignTimeBuild)`) — Grpc.Tools' hard-won
  lesson: skipping makes generated types vanish from IntelliSense.
- **Fail loudly** if no native binary matches the host RID (name the RID + the override property) — never
  silently skip, which would cascade into misleading "type not found" errors. Don't fetch a runtime at
  build (the Antlr-JRE / TypeScript-needs-node trap) — the self-contained binary is exactly the advantage.
*Gate:* a fresh C# project + the package + a `.pg` file builds with **`dotnet build` alone** (the `.pg`
types are usable from C# in the same project, incl. the IDE); a second build with no `.pg` change skips
transpilation; `dotnet clean` removes the generated files; and a project referencing the first project's
output does **not** inherit the transpile behavior or the package dependency.

## P12 — Modules, imports & name resolution — ✅ DONE (phase-1)
Turn the P8 embedded-std foothold into a real module system (full design in PRD §4.5). Independently
sequenceable — frontend-only, depends just on the current pipeline — and a natural companion to P10's
`pgconfig.json` workspace. **Delivered:** the import syntax, the `ModuleResolver` seam + transitive
cross-`.pg` loading, and collision *detection*. **Phase-2 (deferred):** selective-import visibility
restriction and `as` rebinding — both need a per-file import-scope table (the current merge-into-one-unit
model has no place to hang per-file visibility); the *safety* property (never silently shadow) is already
met. Three tracks:

- **New import syntax:** `import { a, b as c } from "spec"`, `import * as ns from "spec"`, bare
  `import "spec"`. `from` is a contextual identifier (no new keyword); the specifier is a quoted `StringLit`
  (a bare `"std.io"` = logical name, a `"./x"` = importer-relative). Rewrite `ImportDecl` (per-name aliases
  + namespace), `parseImport`, and `importStr`; migrate the ~14 existing `.pg` import lines + grammar.ebnf
  + SPEC §11 in the same change (round-trip fidelity is atomic). No lexer/token change.
- **`ModuleResolver` seam for cross-`.pg` imports:** add `compile(source, target, ModuleResolver* = nullptr)`
  (default = std-only, so all existing callers/tests are unchanged). Core generalizes `linkStdModules` into
  a transitive load + `visited` dedup + `inProgress` cycle-detection (clear `a → b → a` error) + post-order
  merge; the resolver only maps specifier→source. CLI ships a `FileModuleResolver` (std registry first, then
  `<root>/a/b/c.pg` for logical names, importer-relative for `./`), with `--root` (later `pgconfig.json`);
  tests ship an in-memory `MapModuleResolver` (no disk). Core stays IO-free.
- **Collision policy — refuse loudly:** ✅ any name collision is a hard error; builtins can't be shadowed;
  functions merge as overloads unless the signature is identical; the silent last-wins holes for top-level
  values / union ctors / extensions are closed. Exports = all top-level public for now (`private` marker
  deferred). ⏳ *phase-2:* selective imports *restricting* visibility (today they validate names but still
  merge the module's whole public surface) and `as` *rebinding* (parsed + recorded, not yet applied) —
  both await the per-file import-scope table.

*Gate (met):* a two-file program (`entry.pg` importing `"./util"` + a logical module) builds via `--root` to
identical C#/TS stdout; a transitive chain `a → b → c` resolves (with shared-dependency dedup); an import
cycle, an unknown module, an unknown imported name, and a cross-module collision (type / value / union case /
extension) are each a clear diagnostic; the in-process tests cover all of this with the in-memory resolver
(no filesystem); existing single-file programs are unaffected. (Phase-2 visibility/`as` is the remaining
gate.)

## P13 ✅ done — Std as real modules + the `lib` prelude
Close the "builtins that bypass imports" gap (full design in PRD §4.6). `print` and the `Math` namespace
*were* hardcoded builtins; the samples that `import`ed them were actually **broken** (the import validation
rejected them — `std.io` had no `print`, `std.math` didn't exist) and only survived because the fidelity
gate `fmt`s rather than compiles. The std is now honestly import-based; `i32.parse` stays global and
`Error`/`Iterable` stay core. All three tracks landed, sequenced (the `lib` track was a prerequisite for
low-churn rollout):

- **`lib` prelude (do first):** add a `LibConfig` (list of specifiers) param to `compile()`; synthesize one
  whole-module `ImportDecl` per entry, tagged lib-origin; reuse `linkModules`. A bare word (`"io"`) is sugar
  for `std.io`; a qualified entry (`"acme.physics"`) is a full specifier resolved like any import — so
  third-party plugins auto-import by their own namespace, no per-publisher special-casing. Add a
  `dropShadowedLibDecls` pre-link pass so a lib decl **loses silently** to any user/explicit decl of the same
  name (explicit-vs-explicit and lib-vs-lib still hard-error). Provenance via an `isLibAuto` flag on
  `ImportDecl` + an `originLib` tag threaded through `mergeDecls`. CLI `--lib io,math`; `pgconfig.json`
  `"lib"` deferred to P10. Core stays IO-free; the defaulted param keeps every existing `compile`/test call
  working.
- **`Math` → `std.math`:** an `extern class Math` with bound static members + `PI`/`E` constants (call
  surface `Math.sqrt(x)` unchanged). Generic `min<T>/max<T>/abs<T>` get type-preservation from normal
  generic typing; IIFE-form operator-ternary templates are `number`+`bigint`-safe and evaluate-once. Delete
  `mathArity`, the `Math` sema/lower/emit special-casing, `mathRename`. *Minor* parser work: accept binding
  arms on a `const` member (or model `PI`/`E` as bound static properties).
- **`print` → `std.io`:** generic `expect fn print<T>(x: T)` + per-target `actual` `extern`; i64/u64 TS
  `actual` overloads carry the `String(…)` wrap (pure data); retain a one-line sema guard rejecting
  non-printable args (no `Printable` bound needed yet). Delete the `isPrint` flag + `printFn`.

*Gate (all green):* `print`/`Math` are unresolved without an import or a matching `lib` entry; `--lib io math`
(or `import`s) makes them available; a user `print`/`Math` or explicit import **silently shadows** the lib one
(no error) while two explicit imports of a name still error; the (now-fixed) `docs/lang/samples/*.pg` join a
**compile** check (`tests/samples/run-compile.ps1`), not just `fmt`; and the `math` + all `print`-using
conformance programs stay byte-identical across C#/TS. A full `Printable` bound and per-name `lib` config
remain out of scope. *Delivered (2026-06-30):* TypeArg inference (generic-return substitution) as the
principled fix for generic-call return types; `List.removeAt` added to the `std.collections` binding; the
in-process suite gained a `compileStd` prelude helper and an IR-level TypeArg-inference canary (the old
String-wrap canary died once `print`'s TS body wrapped *universally*).

**Follow-up gaps surfaced by the sample compile gate — both ✅ fixed (2026-06-30), xfail list now empty:**
- **`06_exceptions.pg` — `Error.message`:** ✅ `findMember` now walks base classes (`TypeInfo.bases`), and
  `Error.message` is a per-target bound property (lower's synthetic `Error.message` arm → C# `$this.Message`
  / JS `$this.message`), reached on a `: Error` subclass via the base walk in *both* sema and lower.
- **`08_extensions.pg` — extension on a generic receiver:** ✅ sema's `liftExtensionGenerics` lifts free
  type-variable leaves of the receiver (`List<T>` → `T`) into the extension's `generics` before tables/
  resolution/body-check; lower + both emitters already thread `generics`, so the signature/return `T?` and
  the emitted `<T>` all follow.

**Backlog (recorded 2026-06-30):**
- ~~**`Error`/`Iterable` as real `extern class`es**~~ ✅ **done 2026-06-30.** An always-linked **core prelude**
  (`compiler.cpp` `STD_CORE`, merged into every compilation via `linkCoreModule` — distinct from the opt-in
  `lib` prelude since these need no import) declares `extern class Error` (type → `System.Exception`/`Error`,
  ctor, `message` property binding) and `extern class Iterable<T>` (type-only → `IEnumerable<$0>`/`Iterable<$0>`).
  Removed: `Error`/`Iterable` from `isBuiltinType`, the `findMember` Error.message + Error-construction
  special-cases, lower's synthetic `Error.message` binding + `typeNames_.insert("Error")`, and the `csType`/
  `tsType`/`ir::New` Error/Iterable branches. The emitters now carry **no** hardcoded type mapping.
- **`extern record` / `@plain` for TS-plugin interop** (idea, 2026-06-30): a record emits as a TS *class*
  (prototype + methods + `equals`), so a JS library that hands back a plain `{x,y}` object (e.g. `JSON.parse`)
  isn't a record instance — the binding template must reconstruct via `new R(...)`. An opt-in form that emits
  a record as a plain `interface`+object-literal in TS (no class) would make JS-library interop friction-free,
  trading away methods/value-`equals`. Records→JS is already easy; only JS→record needs the rebuild today.
- **Idiomatic per-target member casing** (C# `public double X` / `obj.X`, TS `x` / `obj.x`): an *emitter*-only
  style feature (NOT sema/IR — that layer is target-neutral). Would make output more idiomatic and make
  `message`→`Message` fall out for the common case, but touches every field/property/method/access in both
  backends + all goldens and needs real casing rules (acronyms, already-cased names) + a faithfulness call on
  silently renaming user identifiers. A blanket "uppercase-first" is **not** a substitute for native-member
  bindings (a native name isn't always a capitalization away — it would silently miscompile, violating §3).

## P14 — Emitted-output correctness (compile-run the output) + `Option<T>` — ✅ done (2026-06-30)
**Why:** the samples gate (`tests/samples/run-compile.ps1`) only checks the *transpiler* succeeds, not that
the emitted code **compiles and runs**. Manually compiling every sample's C# (`dotnet`) + running its TS
(`node`) surfaced a cluster of output-only miscompiles the transpile-only gate was green over — the §3
"never miscompile" law was being broken silently. This milestone hardens that.

**P14a — the gate. ✅ done.** `tests/samples/run-emit.ps1`: for each sample, build the emitted C# and run the
emitted TS, asserting **each compiles + runs without error** (NOT a stdout cross-compare — samples emit
floats, non-deterministic across targets per §3.D; reuses the conformance csproj/dotnet/node harness). xfail
map for cases blocked on a documented gap (now empty — see the P14a/b summary below: **10 compile+run, 0 xfail**).

**P14b — the bugs it surfaced (2026-06-30):**
- ✅ **`02_records_operators` — `__polyglot_unlowered_expr__`**: the `with`-copy expression was never lowered.
  Fixed (`ir::With`; C# native `with`; TS rebuilds via the ctor). *Was the scariest.*
- ✅ **empty list `[]` → C# `List<object>`** (bidirectional gap, same family as `None`): `[]` now takes its
  element type from the target slot. Plus precise match-binding types + local `T?` normalization.
- ✅ **`print` of a bool diverged** (C# `True` vs JS `true`): std.io C# `print` lowercases bools (conformance
  `bool_print`). *Surfaced while fixing 02.*
- ✅ **`03_enums_unions_match` — C# CS1001**: C# keyword-escaped identifiers (`@base`) + interp-string `\n`
  escaping + float InvariantCulture + self-contained list literals.
- ✅ **`04_generics` — C# CS1020 / TS `compareTo is not a function`**: emit **interfaces** (were never emitted)
  + record `implements`/base clauses + real **indexers** (`operator get` → C# `this[]` / TS `get()`).
- ✅ **`09_strings` — C# CS1039 "unterminated string"**: the interp-string escaping fix above.
- ✅ **`07_using_disposal` — `Disposable` not found**: interface emission + a TS class `implements` interfaces.
- ✅ **Aspirational std methods** (`string.isEmpty`/`toI32`/`toUpper`/`codePoints`): added as **`std.strings`**,
  a module of bound extension methods (the new "extension methods with binding arms" capability).

**P14c — `Option<T>` (the faithful nullable-generic fix).** `T?` over an **unconstrained** type parameter
can't be faithfully compiled to C#: `… : null` is CS0403 (null can't convert to unconstrained `T`); `T?` +
`default` *compiles* but `default(T)` is `0`/`false` for value types, **not** an absent marker — so
`List<i32>.secondOrNull()` would return `0` in C# while TS returns `null` (silent divergence + `0 ?? -1`
doesn't even type-check); and `Nullable<T>` is `where T : struct` (CS0453), illegal for an unconstrained `T`.
All three verified empirically (2026-06-30).

**Design chosen (2026-06-30, "solid over quick"):** a real **`Option<T>` generic discriminated union** in the
core prelude — `union Option<T> { Some(value: T), None }` — emitted via the *existing* union machinery
(C# `abstract record`+`sealed record`s, TS tagged union). `T?` whose base is a **bare generic type
parameter** is sugar that **desugars in the front-end** to `Option<T>`; everything concrete (`string?`,
`i32?`, `Record?`) keeps the idiomatic **native** nullable (C# `int?`/ref, TS `T | null`) since that's
faithful there. Rationale: the tagged union distinguishes `Some(null)` from `None` on both targets (the
asymmetric "TS `T | null` + C# wrapper" alternative cannot — a latent §3 divergence), and desugaring to
existing union+`match` primitives means **no per-target optional special-casing in the emitters**. The
prerequisite — generic unions — is independently valuable (`Result<T,E>`, …).

*Slices:* **(1) ✅** generic unions through parser/AST/IR/lower/both emitters (conformance `generic_union`).
**(2) ✅** `Option<T>` in the core prelude + `Some`/`None`/`match`, with bare-case contextual typing
(`instantiateBareCases`) so `None` takes its `<T>` from context (conformance `option`). **(3) ✅** desugar
`T?`-over-a-generic → `Option<T>` (sema `normalizeOptionalGenerics` + `T`→`Some`/`null`→`None` via
`coerceToOptional` + `??`→`match` lowering + extension-receiver inference), conformance `optional_sugar`;
sample 08's `secondOrNull(): T? … ?? -1` compiles+runs byte-identical. **`x!` on an optional generic is
refused** with a diagnostic (faithful force-unwrap = follow-up). *Deferred:* local `let x: T?` declared-type
normalization inside bodies; nested/already-nullable `T` (`Option<Option<T>>`); `?.` on a generic-nullable.

**P14a ✅ + P14b ✅ done (2026-06-30).** The compile-run gate (`tests/samples/run-emit.ps1`) builds the C# +
runs the TS for every sample; **all 10 now compile+run (0 xfail).** Fixed, worst-first: the `with`-copy
lowering hole (`ir::With`); empty-list `[]`→`List<object>` + precise match-binding types + local `T?`
(expected-type propagation); `print` of bool (`True`→`true`) and of floats (InvariantCulture); C# keyword
identifiers (`@base`), interp-string `\n` escaping, self-contained list literals; **interfaces** (were never
emitted), record `implements`/base clauses, real **indexers** (`operator get`→C# `this[]`/TS `get()`); a TS
class `implements` interfaces vs `extends` a class; the `Type<Args>()` construction-result type now carries
its args; **char literals** (`ir::CharLit`); and **`std.strings`** (a new module of bound extension methods —
`isEmpty`/`len`/`toUpper`/`toLower`/`charAt`/`codePoints`/`toI32`) built on a new general capability:
**extension methods with binding arms** (`extension fn string.toUpper() { actual(target) extern(…) }`), plus
fixing `mergeDecls` to merge an imported module's extensions. New conformance programs: `empty_list`,
`bool_print`, `float_print`, `strings`. (06 trimmed a non-numeric input that can't parse faithfully cross-target.)

## P15 — Single-threaded async/await — ✅ done (2026-07-01)
**Full design: PRD §4.7** (produced by a 4-agent investigation of surface / sema+IR+lower / backends /
capability). Built exactly to the 5-slice plan below; the shared engine (`emitter_base`/`backend_spec`)
needed **zero** changes as predicted. Conformance program #38 (`async_await.pg`) agrees byte-for-byte across
C#/TS **and** Python (`14 | 20`); all gates green (unit + C#/TS 38/38 + Python 37/37 + samples 10 + fidelity 10).
Delivered: `bool isAsync` on `FunctionDecl`/`Member`/`ir::Function`/`ir::Method` (method `async` promoted from
`Member.modifiers` to a typed flag, re-emitted by `printMember`); `ExprKind::Await` (AST + `ir::Await`) parsed
at unary precedence; top-level `async fn`/`async expect`/`async actual` via `parseAsyncFunction`; sema `inAsync_`
gates `await` placement + refuses `async`+`yield`; `Feature::Async` (all 3 backends `supports`, StubBackend
proves it bites). Backends synthesize the wrapper (Option B): C# `async Task<T>`/`Task` + `main().GetAwaiter().GetResult()`;
TS `async…Promise<T>`/`Promise<void>` + floating `main();`; Python `async def` + `asyncio.run(main())` with a
`needsAsyncio_`-gated `import asyncio` prepend. `await e` → `await atom(e)` in each `emitExpr`.

**Follow-up ✅ (2026-07-01) — real `Awaitable<T>` unwrap** (replaced the v1 identity typing): a call to an
async fn/method now types as the compile-time-only `Awaitable<T>` (an `isAsync` bit on sema's `FnSig`/`MemberInfo`
wraps the inferred result); `await` unwraps `Awaitable<T>` → `T`. This makes sema catch the two mistakes the
identity model missed — **forgot-to-await** (`return f()` / `let x: i32 = f()` / `print(f())` all refuse, naming
the fix) and **awaited-a-non-async-value** (`await plain()` refuses) — and mirrors C#/TS, where `return f()`
from an async fn requires `return await f()`. `Awaitable` is never written by the author and never reaches
emission (locals infer via `var`/`const`; backends synthesize the real `Task`/`Promise` from `isAsync`), so all
gates stay byte-identical.

A "colored function" like iterators, with two deliberate divergences from the iterator precedent:
async is **declared** (no `sawAwait_` inference needed) and the return-type wrapper is **backend-synthesized**,
not user-written (Option B — keeps `.pg` source portable: author writes the unwrapped `T`).

**Key decisions (locked):** (1) author writes `async fn f(): T`, backends wrap (`Task<T>` / `Promise<T>` /
`async def`) — NOT an `extern class Task` (Option A rejected: un-idiomatic + non-portable). (2) `await` is a
distinct `ExprKind::Await` at **unary precedence** (not a `Unary` with `text`, not `parsePrimary`). (3) sema
validates `await` only inside `async fn` (else a native compile error = a §3.B miscompile) and **refuses
`async`+`yield`** (async iterators out of scope v1). (4) `await e` typing is **identity in v1** (an async call
already yields the unwrapped `T`); a real `Awaitable<T>` unwrap is the principled follow-up. (5) `Feature::Async`
gates it; all 3 backends support it, so it bites only for a future PHP-like target; multi-target "all must
support it" already emerges from the per-target CLI loop (no `pgconfig.json` pass needed — that's P10).

**Slice plan (each gate-green; the shared engine `emitter_base`/`backend_spec` needs ZERO changes):**
1. **AST + parser + printer.** `bool isAsync` on `FunctionDecl`; promote method-`async` from `Member.modifiers`
   to a typed flag (strip from modifiers **and** re-emit in `printMember` together — else the roundtrip gate
   regresses); `ExprKind::Await`. Parser: consume leading `async` in `parseFunction` + route top-level `async fn`;
   parse `await` in `parseUnary`; add `KwAwait` to `beginsExpr`. Printer: `async ` prefix + `await ` expr.
   Gate: fidelity roundtrip on new async samples.
2. **IR + lower.** `isAsync` on `ir::Function`+`ir::Method` (carry from the AST flag, like `isEntry`); `ir::Await`
   node (mirrors `ir::Unary`); lower `Await` straight through.
3. **Sema.** `inAsync_` flag; validate `await` placement; refuse `async`+`yield`; identity typing for `await e`.
4. **Backends** (per-target, from PRD §4.7): C# `async `+`Task<T>`/`Task` + `main().GetAwaiter().GetResult()`;
   TS `async function`+`Promise<T>`/`Promise<void>` + floating `main();`; Python `async def` + `asyncio.run(main())`
   with an `import asyncio` prepend via a `needsAsyncio_` flag (mirrors `needsIdiv_`). `await e` → `await atom(e)`
   in each `emitExpr`.
5. **Capability + tests.** `Feature::Async` in the enum + `kAllFeatures[]` + `featureName()` case; Collector marks
   it on an async decl or an `Await` expr; `StubBackend(Feature::Async)` refusal test. New conformance program(s)
   exercising `async fn` + `await` across all three targets (the output must be deterministic — e.g. await an
   already-resolved value / a simple async chain, no wall-clock).

## Editor tooling — 🚧 Tier 1 in progress (2026-07-01; brought forward from Stretch)
Both editors (VS Code + Visual Studio), sequenced Tier 1 → Tier 2. Guiding principle: **language intelligence
is written once in the C++ frontend; every editor is a thin client.** Layout + full plan: `editors/README.md`.
- **Tier 1a — highlighting ✅ (VS Code).** A single declarative TextMate grammar
  (`editors/grammars/polyglot.tmLanguage.json`) built from the real lexer keyword set + string-interpolation
  /number-suffix rules; consumed natively by VS Code *and* Visual Studio 2022+. VS Code extension scaffold in
  `editors/vscode/` (package.json + language-configuration.json + extension.js). Keep the grammar in sync with
  `src/.../lexer.cpp` + `docs/lang/grammar.ebnf`.
- **Tier 1b — formatting + diagnostics ✅ (engine + VS Code).** New **`polyglot check <file> --json`** CLI
  command runs the frontend (lex/parse/sema + C#-reference capability gating) and prints
  `[{line,col,severity,message}]`; the extension surfaces those as squiggles on open/save and wires
  `polyglot fmt` as the document formatter. (Live-on-type diagnostics wait for the LSP, which holds the buffer.)
- **Tier 1c — Visual Studio highlighting (next).** Package the same grammar as a VSIX-consumable TextMate
  grammar; register the `.pg` file type.
- **Tier 1c — Visual Studio highlighting.** Package the same grammar as a coloring-only VSIX; register `.pg`.
  Can land anytime (trivial, reuses the shared grammar).

## P16 (Tier 2) — `polyglot lsp` language server — 🚧 designed (2026-07-01; §4.8, 4-agent investigation)
A zero-dep JSON-RPC server over the frontend-as-a-library; VS Code + Visual Studio are thin LSP clients.
**Full design + per-pass map: PRD §4.8.** Four load-bearing changes, then the server, then clients. Each
phase is gate-green (unit + conformance stay untouched — all additions are behind defaulted/optional seams).

**P16a — Frontend foundation + semantic model — ✅ done (2026-07-01).** Shipped across 4 commits: `SourcePos.fileId`
+ `analyze()` seam + same-file `SemanticModel` (functions/params/locals), then `Diagnostic` ranges+severity, then
type/member/value references. Go-to-def resolves functions, types, members, locals, and values in the file; unit-tested;
all conformance/samples/fidelity gates green throughout (additions sit behind defaulted/optional seams).
1. **`SourcePos { …; int fileId = 0; }`** (`diagnostics.hpp`) + thread `fileId` into `lex(source, diags, fileId=0)`.
   Defaulted → every existing site + test compiles unchanged (single-file = fileId 0). *(§4.8 change 1)*
2. **Parser name-token positions:** capture `expect(Identifier).pos` into a `namePos`/`nameSpan` on decls +
   `Member` exprs (today they point at the keyword/dot; `Name`/`Param`/`Pattern` are already precise). Mechanical.
   *(§4.8 change 2)*
3. **Extend `Diagnostic`** with an end position + a severity (today: a bare point + hardcoded "error"), so LSP
   diagnostics carry real ranges. Update `check --json` to emit them.
4. **`analyze(source, resolver, lib) → { CompilationUnit, diagnostics }`** — split the front half out of
   `compile()` (stops before lower/emit); `compile()` calls it then lowers. *(§4.8 change 3)*
5. **`SemanticModel` via a sema hook:** optional `SemanticModel* out = nullptr` on `check()`; emit `SymbolDef`
   at `declare`/`buildTables`, `SymbolRef` at `checkName`/`checkCall`/`checkMember`; mark merged std/import decls
   `external`. Query API: `definitionAt`, `documentSymbols`, `hoverAt` (post-check `Expr.type`). Structures +
   correctness notes (shadowing, overloads, unresolved sentinel) in §4.8 change 4.

**P16b — The server + VS Code client — ✅ done (2026-07-01; same-file intelligence shipped).** Zero-dep JSON reader
(`json.hpp/.cpp`) + a `polyglot lsp` stdio JSON-RPC server (Full sync, utf-8 position encoding, cached per-uri model)
serving publishDiagnostics / definition / hover / documentSymbol / formatting. The VS Code extension is now a
`vscode-languageclient` client spawning `polyglot lsp`; the old shell-out `fmt`/`check` providers are removed
(diagnostics are live on-type). **semanticTokens ✅ (2026-07-01)** — delta-encoded `semanticTokens/full`
classified from the model (function/type/method/property/variable/enumMember + `declaration`), layered over the
grammar; required finishing name-token positions (`namePos` on all type/member/value/let decls, matching
`FunctionDecl`) — which also makes go-to-def anchor at the name, not the keyword. Also implemented: `pgconfig.json`
(step 9). *Deferred:* completion; cross-module (P16c); the Visual Studio client (P16d).
6. **JSON reader** in Core (`json.hpp/.cpp`) — hand-written, ~300 lines, unit-tested (incl. `\uXXXX` + surrogate pairs).
7. **`polyglot lsp`** CLI subcommand: binary-stdio `Content-Length` framing + JSON-RPC dispatch; lifecycle;
   negotiate `positionEncoding:"utf-8"`; open-document store + **buffer-aware `ModuleResolver`** (Full sync);
   cache `(uri,version) → {unit, model}`. Capabilities in cost order: `publishDiagnostics` → `documentSymbol`
   → `definition` → `hover` → `semanticTokens/full`.
8. **VS Code client:** add `vscode-languageclient` + an **esbuild** bundle (source stays JS); reuse `cliPath()`
   as the server command (`args:["lsp"]`); pass `{root,lib}` as `initializationOptions`. **Remove** the
   `check`/`fmt` shell-out providers (superseded by `publishDiagnostics`/`formatting`); **keep** the grammar
   (semantic tokens layer on top) + `language-configuration.json`. Add a bundle `preLaunchTask`.
9. **Minimal `pgconfig.json`** ✅ (2026-07-01) — `{root, lib}` parsed in the CLI/LSP layer (Core JSON reader) →
   the resolver + `LibConfig` the core already consumes (core stays IO-free). Found by walking up from the file;
   the LSP re-reads it per analysis and it wins over `initializationOptions`; explicit `check`/`build` flags win
   over it. Testbench demo: `editors/vscode/testbench/pgconfig.json` + a `"geometry"` import. `paths` deferred to P10.

**P16c — Cross-module + richer features.**
10. **Cross-module go-to-definition ✅ (2026-07-01).** `SourcePos` carries a `fileId`; `analyze()` stamps the
    entry (id 1) and each loaded module at its `loadImports` lex boundary, returning a `SourceMap` (fileId →
    canonical origin). Sema registers imported symbols as `external` defs carrying their module's fileId, so a
    ref to an imported symbol resolves to it; the LSP maps the def's fileId → a `file://` Location (F12 on
    `Vec2`/`length` jumps into `geometry.pg`). **std click-through ✅** — core/lib/imported std modules are
    stamped too; a std def maps to a `polyglot:<name>` URI the server serves via a `polyglot/moduleSource`
    request, and the VS Code extension registers a content provider so clicking `print`/`Math`/`Error` opens
    the embedded std source read-only. The extension marks those `polyglot:` docs as the `polyglot` language,
    so they get the same TextMate grammar highlighting as `.pg` files (grammar only — their scheme isn't in the
    LSP selector, so the server never analyzes the read-only std source).
11. **`references`/`rename` ✅** (from the model; rename is file-local-only) and **`completion` ✅** (keywords +
    bare-callable symbols, context-insensitive). **Member completion (`obj.`) ✅ (2026-07-01)** — see the tail
    note below. *Deferred:* in-scope-only local filtering.

**P16d — Visual Studio LSP client — 🚧 in progress (2026-07-01; 2-agent investigation).** A single VSIX at
`editors/vs/` that reuses the *same* `polyglot lsp` server VS Code drives, so no analysis logic is reimplemented.
**Build/test reality (probed on this machine):** the VSSDK **is installed** on VS 18 Insiders and a VSIX **builds
headlessly** with the repo's VS 18 MSBuild (`$(VSToolsPath)\VSSDK\Microsoft.VsSDK.targets` imports cleanly under
`VisualStudioVersion=18.0`, verified); **testing the running extension needs an interactive `devenv /rootsuffix
Exp`** — a GUI step that is the user's, not automatable here.

**Architecture (from reading the server + VS Code client):**
- **`[Export(typeof(ILanguageClient))] [ContentType("polyglot")]`** MEF component whose `ActivateAsync` launches
  `polyglot lsp` and returns `new Connection(process.StandardOutput.BaseStream, process.StandardInput.BaseStream)`
  (server→client, client→server — order is load-bearing). Sends `{root, lib}` as `initializationOptions`, mirroring
  the VS Code client. The full standard set the server advertises (diagnostics, definition, hover, documentSymbol,
  semanticTokens, formatting, references, rename, `.`-completion) then flows **with zero VS-specific code**.
- **Content type** `polyglot` with `BaseDefinition = code.remote` (`CodeRemoteContentDefinition` — required for
  LSP-backed buffers) + a `FileExtensionToContentTypeDefinition` mapping `.pg`. This is what routes `.pg` buffers to
  the client.
- **Position encoding:** do **not** force utf-8 — VS's client sends utf-16, and the server already does the correct
  per-line utf-16 conversion (the P16-tail work). Nothing to do.
- **Coloring:** bundle the **shared TextMate grammar** (`editors/vscode/syntaxes/polyglot.tmLanguage.json` +
  `language-configuration.json`, copied at build so they stay canonical in `editors/vscode/`) in the VS-Code
  contribution shape VS's TextMate engine reads — the offline/instant floor — with the server's `semanticTokens`
  refining on top for free. Same posture as VS Code; ship both.
- **CLI path:** resolve like the VS Code `cliPath` — a Tools→Options page, else a bundled per-RID CLI, else
  `polyglot` on PATH.
- **Project shape:** a **legacy VSSDK `.csproj`** (VSIX project-type GUID; SDK-style isn't fully supported for VSIX
  on the 18 SDK), `net472`, `InstallationTarget [17.0,)` (VS 18 loads 17.0+ extensions), VS SDK referenced via
  stable **17.x** NuGet (`Microsoft.VisualStudio.SDK` + `Microsoft.VSSDK.BuildTools`).

**v1 scope (ships): the full standard LSP set above + TextMate coloring + CLI-path resolution.** **Deferred** (the
VS-Code-specific glue with no turnkey VS analogue, each a later slice via `ILanguageClientCustomMessage2`): the
`polyglot:` **std virtual-doc click-through** (`polyglot/moduleSource` → a read-only buffer) and the **generated-output
preview** (`polyglot/emit` → a `ToolWindowPane`, the "P17-for-VS" follow-up). Go-to-def into std resolves to a
`polyglot:…` URI VS can't open → it no-ops gracefully until then.

**Slices (commit each):** (1) VSSDK project + manifest + deps (empty MEF) builds & deploys to Exp; (2) content-type +
`.pg` association; (3) bundle the TextMate grammar — **coloring-only milestone, shippable alone**; (4)
`PolyglotLanguageClient` + CLI resolution — server starts, all standard features light up; (5) Tools→Options CLI-path
page (+ optional bundled CLI); (6, deferred) custom-message std docs + emit tool window.

**As-built (2026-07-01) — slices 1–4 done, headless build green; interactive test pending.** The VSIX at
`editors/vs/` (`MintPlayer.Polyglot.VisualStudio.csproj`) **builds headlessly** with the VS 18 MSBuild and packages
correctly — verified the `.vsix` contains the MEF assembly, the manifest, and the grammar bundle
(`Grammars/polyglot/{package.json, syntaxes/polyglot.tmLanguage.json, language-configuration.json}`, copied from
`editors/vscode` at build via the `CopyPolyglotGrammar` target, git-ignored). Delivered: `PolyglotContentType.cs`
(content type `code.remote` base + `.pg`), `PolyglotLanguageClient.cs` (`ILanguageClient` launching `polyglot lsp`,
`Connection(stdout, stdin)`, `{root:null, lib:"io,math"}` init options), `PolyglotCli.cs` (`POLYGLOT_CLI` env →
bundled → PATH). Build gotchas solved: set `VSToolsPath` from `MSBuildExtensionsPath` (the x86 fallback is empty);
add framework refs `System` + `System.ComponentModel.Composition` (MEF, not auto-referenced in a legacy net472
project); the manifest needs `<ProductArchitecture>amd64</ProductArchitecture>` on the install target for VS 18.
NuGet: `Microsoft.VisualStudio.SDK` 17.0.32112.339 + `Microsoft.VSSDK.BuildTools` 17.0.5234. **Not yet done:**
interactive verification in `devenv /rootsuffix Exp` (a GUI step — the user must open a `.pg` file and confirm
coloring + the LSP features light up; and confirm VS's TextMate engine actually picks up the bundled grammar, the
one piece not verifiable headlessly), slice 5 (Options page / bundled CLI), and slice 6 (deferred custom-message
features).

**As-built notes (deltas from the plan above — 2026-07-01):**
- **VS Code client uses NO bundler.** The plan said esbuild; in practice the extension stays plain CommonJS
  (`main: ./extension.js`) with a single runtime dep (`vscode-languageclient`). F5's `prepare-extension`
  preLaunchTask runs `build-cli` (VS 18 MSBuild) + `npm install` in `editors/vscode`. `node_modules` is gitignored.
- **Buffer-aware `ModuleResolver` ✅ (2026-07-01) — live cross-file editing.** A `BufferResolver` wraps
  `FileModuleResolver` and serves an **open editor buffer's unsaved text** for any imported module the editor has
  open (matched by real path via `fs::equivalent`, robust to uri-encoding/drive-case; only the source is swapped,
  the disk `canonicalPath` stays the dedup/cycle identity). Used in both `analyzeDoc` and the P17 `generatedSource`
  preview. To *trigger* dependents, `didChange` now re-analyzes **every open doc** (not just the edited one), so
  editing an imported `.pg` refreshes its dependents' diagnostics + preview immediately, before save. (Naive
  re-analyze-all is cheap for the few files an editor holds open; dependency-tracked re-analysis is a later
  optimization.) Spawn-tested: A importing `helper` from B goes red the instant B's *unsaved* buffer drops it.
- **Position encoding is negotiated:** the server advertises `utf-8` only if the client offered it in
  `capabilities.general.positionEncodings`, else falls back to `utf-16` (correct for ASCII either way; the
  UTF-8↔UTF-16 column walk for non-ASCII lines is still a follow-up).
- **The CLI statically links the CRT** (`/MTd`,`/MT` across Core/Cli/Tests) so it depends only on `KERNEL32.dll`
  — required for VS Code to spawn it (its extension host lacks the CRT DLLs on PATH) and load-bearing for the P11
  NuGet (runs on a consumer machine with no CRT prereqs). See PRD §4.3.
- **pgconfig.json is live:** the VS Code client watches `**/pgconfig.json`; on change the server re-analyzes
  every open document (`workspace/didChangeWatchedFiles`), so root/lib edits refresh diagnostics immediately.
  `publishDiagnostics` emits only the entry file's own diagnostics (fileId 1) — an imported module's errors
  show in that module, not leaked into the importer.
- **Implemented LSP capabilities:** `publishDiagnostics` (live on-type, point ranges widened to the identifier),
  `definition` (same-file + cross-module + std virtual docs), `hover`, `documentSymbol`, `semanticTokens/full`,
  `documentFormatting`, `references`, `rename` (file-local only), `completion` (bare names + keywords), and the
  custom `polyglot/moduleSource` for std virtual documents.
- **Semantic tokens (+ hover/go-to-def) inside std virtual docs ✅ (2026-07-01).** The `polyglot:` scheme is now
  in the client's document selector, so the read-only embedded std docs are analyzed (from their synced text —
  `std.*` imports resolve via the Core's embedded registry with no file resolver) and get accurate identifier
  coloring + hover + go-to-def, not just TextMate grammar. Their diagnostics are **not** published (analyzing std
  standalone would raise link-context noise on code the user can't edit). Spawn-tested: `std.math` returns
  non-empty semantic tokens and zero published diagnostics.
- **Member completion (`obj.`) ✅ (2026-07-01).** Typing `.` after a receiver lists that type's members. The
  model now carries an `owner` (the owning type name) on Field/Method defs (set in sema's `registerType` + the
  positional-record-fields loop). The LSP detects the member context by scanning left from the cursor over an
  identifier prefix to a `.`, then **analyzes a repaired buffer** (the trailing `.member` dropped so it parses even
  mid-edit) via a new `analyzeText` helper, resolves the receiver identifier's type from that model
  (`definitionAt` → `type.name`), and emits the defs whose `owner` matches (Field=5/Method=2 kinds; bare
  completion still excludes members via `completionKind`→0). `.` is advertised as a completion trigger character.
  Works for value receivers **and** type names (`Math.` lists statics for free, since a type reference's def type
  is the type itself). Spawn-tested: `v.` on a `Vec2` local lists `x`/`y`/`length`, no keyword leakage. *v1
  limits (follow-ups):* only the receiver type's **direct** members (no inherited base-class members yet), and
  **`this.`** isn't resolved (needs the enclosing type, which the model doesn't expose).
- **In-scope-only local filtering ✅ (2026-07-01).** Bare completion no longer offers locals/params from other
  functions. The parser now records each fn/method body's end (`bodyEnd`, from the closing `}` — a new
  `lastBlockEnd_` captured in `parseBlock`); sema stamps every Local/Parameter def with its enclosing
  `[scopeStart, scopeEnd]` (fn/method name → body end); the LSP offers a local only when the cursor falls in that
  range (defs with no recorded scope — top-level, lambdas — stay always-offered, as before). Function-level
  granularity (a local in an inner block is in scope for the whole enclosing fn — fine for completion). Spawn-tested:
  inside `main`, its own local shows but a sibling function's local doesn't, while functions stay offered.
- **Non-ASCII UTF-16 position walk ✅ (2026-07-01) — the deferred tail is now empty.** When a client negotiates
  `utf-16` (VS Code negotiates `utf-8`, so that path is unchanged — every conversion is guarded by a `utf16_` flag
  and is pure identity under utf-8), the server converts columns per line between its internal byte offsets and the
  client's utf-16 code units: **incoming** positions via `inCol` (used by `modelFor` + completion), **outgoing**
  ranges via `encRange` (diagnostics, definition, references, rename, documentSymbol — using the target doc's text,
  falling back to byte columns for a cross-file doc we don't have open), and **semantic tokens** convert each
  token's start column *and* length. Helpers `utf16Units`/`byteColFromUtf16`/`protoColFromByte`/`lineOf` (a 4-byte
  astral char = 2 utf-16 units). Spawn-tested with a `"é"`-containing line under forced utf-16: references reports
  the utf-16 column and go-to-def from a utf-16 cursor resolves correctly. **All P16 tail items are done; only
  P16d (the Visual Studio client) remains of P16.**

## P17 — Live generated-output preview — ✅ done (2026-07-01; §4.9, 2-agent investigation)
See a `.pg` file's emitted C#/TS/Python **live as you type**, produced in memory (never written to disk) and
rendered into a **read-only virtual editor opened beside** the source — reusing P16's virtual-doc + custom-LSP-
request plumbing almost verbatim. **Full design + rationale: PRD §4.9.** The file tree is *not* where code renders
(a `TreeView` item is label+icon only — it can't show colored code); a virtual document is, and it gets built-in
target-language coloring for free. Each slice keeps every gate green (all additions are new handlers / new client
code behind the existing seams — `compile()`, emission, and `analyze()` are untouched).

**P17a — Server: the `polyglot/emit` request (CLI only, zero Core change).**
1. **`compile()` is already suitable** — pure, in-memory, returns `EmitResult { ok, code, diagnostics }`; the CLI's
   `build` verb writes to disk, `compile()` does not. So the whole feature is one new handler.
2. **Factor `contextFor(uri)`** out of `LspServer::analyzeDoc` (the `entryDir`/`loadPgConfig`/`root`/`libStr`/
   `FileModuleResolver` block) so preview and diagnostics resolve modules *identically* (same `pgconfig.json`
   `{root,lib}`). Add free helpers `targetFromString(str) → optional<Target>` (also tidy `runBuild`'s inline
   string compares) and `diagnosticsToJson(diags)` (shared with `check --json`).
3. **`LspServer::generatedSource(params)`** beside `moduleSource`: guard `text_.count(uri)` (reply
   `{ok:false,code:"",diagnostics:[]}` if the doc isn't open — never insert an empty `text_[uri]`); build the
   resolver from `contextFor(uri)`; `compile(text_[uri], targetFromString(target), &resolver, parseLibList(lib))`;
   serialize `{ target, code, ok, diagnostics }`. **One target per request** (not a bundled three-target map).
4. **Dispatch arm** `else if (method == "polyglot/emit")` in `runLsp`, beside `polyglot/moduleSource`. No
   `initialize` capability change (custom `polyglot/*` requests aren't in the standard capability set).
   *Verify:* a spawn-based test (like the pgconfig watch test) — didOpen a doc, request `polyglot/emit` for each
   target, assert `ok` + non-empty `code` for C#/TS; assert a broken doc returns `{ok:false,code:""}` + diagnostics.

**P17b — Client: the live preview surface (`extension.js` + `package.json`).**
5. **`polyglot-gen:` `TextDocumentContentProvider`** (clone the existing `polyglot:` provider): URI encodes
   `(sourcePath, target)` with a `.cs`/`.ts`/`.py` extension so language detection colors it automatically;
   `provideTextDocumentContent` calls `client.sendRequest('polyglot/emit', {uri, target})` and returns
   `res.code` (or the last-good text + a stale banner when `!res.ok`). Back its `onDidChange` with a
   `vscode.EventEmitter<Uri>`.
6. **Command `polyglot.showOutput`** (+ an editor-title menu icon on `polyglot` files) opens the gen doc
   `ViewColumn.Beside`, `preview:true`, `preserveFocus:true` (reuse one tab, keep the cursor in the `.pg`).
   Coloring belt-and-suspenders: `setTextDocumentLanguage(doc, 'csharp'|'typescript'|'python')` on open (same
   hook that marks `polyglot:` docs).
7. **Liveness:** debounce (~150–250 ms) `onDidChangeTextDocument` for the previewed `.pg` → `emitter.fire(genUri)`;
   VS Code re-pulls and diff-patches the visible editor in place (scroll/cursor preserved). Guard stale responses
   with a per-URI request sequence. Emit reflects the **in-memory buffer** (unsaved), not disk.
8. **Follow the active editor:** `onDidChangeActiveTextEditor` retargets the preview to the focused `.pg` —
   **guard the feedback loop** (ignore activations of `polyglot-gen:`/`polyglot:` docs; only follow
   `file:`+`polyglot`). **Target switch** via a `StatusBarItem` (`Output: C#`) → `QuickPick` of the three,
   persisted in workspace state; a `polyglot.preview.defaultTarget` setting seeds it.
9. **Lifecycle + multi-root:** push provider/emitter/command/status-bar into `context.subscriptions`; on
   `onDidCloseTextDocument` drop that doc's last-good cache. Resolve the source's owning folder via
   `getWorkspaceFolder(sourceUri)` and pass **that** folder's `{root,lib}` into `polyglot/emit`, not
   `workspaceFolders[0]` (so imports in a second root resolve correctly).

**As-built (P17a+P17b — 2026-07-01):** server slice shipped exactly as planned (spawn-tested: C#/TS/Python emit
`ok` for a valid doc; broken/unknown-target/unopened all `ok:false`). Client delta from step 9: the client sends
**only `{uri, target}`** and the **server** derives `{root,lib}` from the file's nearest `pgconfig.json` (the
`contextFor` walk-up, re-read per request) — so multi-root resolution is correct *without* the client computing an
owning folder, and preview output always matches the diagnostics. Client UX delta from steps 6/8: **"Show Generated
Output" opens *all three* targets at once** (each its own permanent tab beside the source), not one selected target
— a `.pg` genuinely has C#/TS/Python outputs, so surfacing all is the least-surprising behavior. Consequently the
single-target status-bar switcher / `selectTarget` / `preview.defaultTarget` (built first, then reworked on user
feedback) were **dropped**; opening a *single* target on demand is the Explorer tree's job (`openGenerated`). To let
multiple tabs follow one source without per-source tab churn, `polyglot-gen:` URIs are keyed by **target only**
(`{target}` in the query) and render whatever `previewSourceUri` currently points at; following the active `.pg`
(guarding against gen/std virtual docs) and the 200 ms debounce both just re-fire every open preview. Coloring via
the `.cs/.ts/.py` extension + an explicit `setTextDocumentLanguage`; last-good cache keyed `target::src`, pruned on
source close.

**P17c — the "Polyglot Outputs" TreeView (discovery) — ✅ done.**
10. A `TreeDataProvider` **in the Explorer** (`contributes.views.explorer`, gated by a `polyglot.hasOutputs`
    context key so it's hidden when no `.pg` is open — cheaper than an activity-bar `viewsContainer`, which would
    need a shipped SVG icon): open `.pg` files as roots, each expanding to C#/TypeScript/Python leaves whose
    `command` runs `polyglot.openGenerated(src, target)` (a specific-target opener added beside `showOutput`).
    Renders *no code* — it's a navigator over the same virtual docs. Refreshes on `.pg` open/close.

**Error-behavior invariant (§3.B-adjacent):** `compile()` never returns partial/garbage — on failure it's
`{ok:false, code:""}`. The client keeps the **last-good** output visible with a `// Polyglot: N errors — showing
last successful output` banner; it must **never** blank the pane on a transient parse error mid-typing, nor render
half-emitted code as valid. Real errors already squiggle in the `.pg` editor. Per-target honesty: a Python preview
may legitimately fail (walking-skeleton subset) where C#/TS succeed — say so in the banner, don't hide it.

**Deferred (post-P17):**
- **Target list from the backend registry, not hardcoded (belongs with P10).** `extension.js` `TARGETS` hardcodes
  the three targets + their `{name, ext, langId, comment}`. The real set is the CLI's backend registry — and, with
  P10, downloadable backends too. Fix: a server-advertised list (a `polyglot/targets` request, or an `initialize`
  field) of `{ id, displayName, fileExtension }` per registered backend; the client derives `TARGETS` from it
  (langId via VS Code's extension detection; comment prefix defaults to `//` with an override), so a plugin backend
  shows up in "Show Generated Output" + the Outputs tree with **no client change**. Blocked on the registry being
  queryable across the LSP seam → do it in P10. Flagged at the code site (`extension.js` `FIXME(P10)`).
- A Webview with source-map gutter lines linking a `.pg` line to its output line (the one job a real editor can't
  do); pre-warming targets on idle.

## P18 — Data-driven backends: languages as pure-JSON plugins — 🚧 designed (2026-07-01; §4.10, 4-agent investigation)
Turn target languages into **installable, pure-data JSON plugins** — no Core change, no plugin code (RCE-safe). This
is the completion of PLAN P9 (the declarative-DSL endpoint) + the backend half of P10 distribution, and the
prerequisite the user set before publishing the editor extensions. **Full design: PRD §4.10; detailed DSL +
interpreter in `docs/design/backend-spec.md`; packaging/safety in `docs/design/plugins-and-targets.md` §6.1/§4.**

**The reframe:** P9's "irreducible 30% imperative" was overstated *for the pure-data question* — almost all of it is
a fixed decision tree over IR fields rendering strings. Verdict from the investigation: **≈85%** flattens to data
with a ~10-primitive interpreter; **≈95%+** by adding a small fixed set of bounded, audited Core primitives; the
remaining **<5%** is genuine target limits (Python expression-only `lambda`) the §3.E capability gate already refuses.
No per-plugin escape hatch exists (that would reintroduce RCE) — completeness comes from **Core primitive growth**,
bounded by "expressible as selection-among + substitution-into plugin templates using only fixed primitives?"

**The DSL (Design A, chosen over a "strategy vocabulary"):** one JSON `Rule` per `ir::ExprKind`/`StmtKind`/decl kind,
interpreted by a fixed non-Turing-complete vocabulary — `tmpl`/`get`/`emit`/`emitChild` (interpreter-computed
precedence parenthesization from a declared `precedence` table — plugins never author paren logic), `type`,
`map`/`fold`/`interleave` (bounded list iteration), `case`+weak `Test` (`eq`/`in`/`has`/`isKind`/`typeIs`/`any`/`all`,
no arithmetic), `call` (depth-capped helpers), `let`/`fresh`, `require` (dedup'd import/preamble buckets placed by the
`program` rule — the one controlled side effect), + a fixed builtin set (`ident`/keyword-escape, casing,
`escapeString`/`escapeInterp`, `opSpelling`, `wrap`). Worked JSON was designed for every hard case (precedence,
`Match` in 3 idioms, `Try`, records/unions, iterators, async, interpolation, numeric-wrap matrices). Design B (a
closed `MatchOp{style}` strategy vocabulary) rejected — it makes the Core *guess* future targets' idioms.

**Slices (each proven byte-identical before it counts — the P9/P9-V discipline):**
1. **Interpreter engine** (`backend_engine.hpp/.cpp`) reusing `EmitterBase`'s buffer/block machinery; a **dual-run
   test harness** (emit via C++ backend AND via the interpreted spec; assert byte-equal) gates every later slice.
   `Target` enum → a string name + a `BackendHandle` (parsed, schema-validated-at-load, immutable spec bytes from the
   host; Core stays IO-free). `compile()` takes a handle; **`analyze()` unchanged**. `loadBackend()` validates
   exhaustively at load (every claimed feature has a rule; unknown primitive/slot → hard error; "no rule" is never a
   silent drop — the P9-V lesson).
2. **Migrate the already-tabular ≈70%** (scalar/operator/literal/precedence/block tables) of C# (the differential
   *oracle* — must be byte-perfect first), then TS, then Python, into JSON specs behind the dual-run gate.
3. **Migrate the hard nodes** — `Match`/`Try`/`Cast`/interpolation/**numeric faithfulness** (`intWrap`/`binaryWrap`
   tables + `wrap` builtin + `require`d prelude fragments like `_pg_idiv`) — plus the small fixed primitives the
   ceiling needs (`fold`/`interleave`/`any`/`all`/`fresh`/`require`).
4. **Push module-semantic queries into lowering** as precomputed IR bits (`typeIsRecord` for TS structural `.equals`,
   `hasCatchAll` for C# exhaustiveness) so the interpreter context stays node-local.
5. **Migrate declarations** (record/class/union/enum/interface/extension/`program` wrapper — most template volume,
   least logic).
6. **Flip default to interpreted** (C++ backends stay one release behind `--legacy-backend`), then **delete
   `emit_csharp/typescript/python.cpp` + `kRegistry[]`**; the three targets are now three in-box JSON specs.
7. **Std de-hardcoding:** the ~90 `actual(target)` arms in `compiler.cpp` become per-plugin data (a new target ships
   its own std arms), riding the P12 `ModuleResolver` + `lib` prelude seams — the hidden per-`(module×member×target)`
   scaling cost the inventory surfaced.
8. **Backend distribution (with P10):** the npm-wrapped `polyglot-plugin.json` package (spec + capabilities +
   `externTypes` + `builddeps` + `std/*.pg`), `polyglot install` fetching via the npm HTTP API **data-only** (verify
   SHA-512, zip-slip-safe extract, never run scripts), `--target <name>`→registry→cache→`BackendHandle`, and the
   editor `polyglot/targets` picking up installed languages (closes the `extension.js` `FIXME(P10)`). The
   **`pgconfig.json` schema** this rides is resolved in design note §6.3: `dependencies` (umbrella plugin set —
   language + library), `targets` (emit selection), `lib` ⊆ dependencies (ambient), + `pgconfig.lock.json` integrity
   pinning. A plugin is data the Core interprets — never executed — which is what keeps install+transpile RCE-safe.

**Honest residual (unchanged from §6.1):** data-only kills *transpile-time* RCE, not *runtime* trust — a malicious
declarative binding can still emit hostile *target* code; plugin output needs the same trust as any dependency.
Signing/trust is the deferred mitigation. **Highest risks:** the DSL expressing `Match`/`Try`/type-spelling/numeric
wrap (mitigated by the fixed-primitive library, defined *from* the current emitters, not guessed); C# spec byte-perfect
as the oracle; and "no rule" must be a loud error (load-time exhaustiveness validation is the defense).

**As-built (2026-07-01, in progress):** ordering the *lowest-risk data migration first* (the already-tabular Spec,
before the interpreter). **Slice-2-start ✅:** `loadBackendSpec`/`backendSpecToJson` (`backend_spec_json.hpp/.cpp`,
over the existing `json.hpp`) parse+serialize a `BackendSpec`, validating `name` + the `blockStyle` enum and failing
loudly otherwise. The **C# backend's Spec now loads from an embedded JSON document** (`CSHARP_SPEC_JSON` in
`emit_csharp.cpp`) instead of a compiled-in struct — the imperative Hooks are unchanged, only the tabular data's
source moved to JSON; output is byte-identical (run-diff 38/38 + unit tests green, incl. 4 new P18 tests).
**Slice-2 ✅ (all three):** the TypeScript (`TS_SPEC_JSON`) and Python (`PY_SPEC_JSON`) Specs also load from
embedded JSON now — all three backends' tabular data is data-from-JSON, byte-identical (run-diff 38/38, run-python
37/37, unit tests green).
**Slice-1 (interpreter spine) ✅:** `backend_engine.hpp/.cpp` — the JSON emission-DSL interpreter's scalar core
(`Rule`: literal/`tmpl`/`get`/`case`+`Test`/`fn`; `Test`: eq/has/and/or/not) parsed from JSON and evaluated against
an `EvalContext` seam (`get`/`has`/`builtin`). Non-Turing-complete, no plugin code (RCE-safe by construction);
malformed rules fail loudly. 11 new unit tests over a mock context. **Deliberately deferred to the IR-wiring slice**
(don't guess the shape): the recursive child-emission primitives `emit`/`emitChild`/`map`/`fold`/`interleave` — they
plug into `ir::Expr` where their shape is grounded.
**Slice-4 (first hooks → data) ✅:** the C# emitter's **leaf-literal hooks** (Int/Float/Bool/Null/Str) are now JSON
`Rule`s interpreted at emit time, not C++ — driven by `CSHARP_EXPR_RULES_JSON` + a `CsExprCtx` (EvalContext over
`ir::Expr`: exposes `node.text`/`node.value`/`node.type` + `spec.*` literals + the `intSuffix`/`escapeString`
builtins). `emitExpr` looks up the rule for the node kind (via `csExprRuleKey`) and interprets it; unmigrated kinds
still run the C++ switch. **Byte-identical** — the existing differential (run-diff 38/38, run-python 37/37) + golden
unit tests are the byte-identity oracle, so a separate toggle-harness is redundant for an in-place migration (noted
as an engineering call). First real proof the imperative ~30% flattens to data.
**Slice-5 (first recursive family) ✅:** `Binary` migrated to a JSON Rule — introduces the child-recursion
primitives grounded in the real IR: `{"emit":path}` / `{"emitChild":path,"side":"l|r|recv"}`, where the **context
computes parenthesization** from the precedence table (plugins never author paren logic — the `child()`/`atom()`
algorithms moved into `CsExprCtx::emitChild` as fixed C++). Numeric faithfulness = a `subWordWrap(type, inner)`
builtin (C# sub-32 cast-back), operator spelling = an `opSpelling` builtin. The dead C++ `child()`/`subWordCast`
were removed. Byte-identical incl. the FruitCake north-star's arithmetic (run-diff 38/38, unit tests +1). Next:
more recursive families (Call/Member/Cond) via a `map` primitive, then `Target`→`BackendHandle`, then TS/Python.

**Slice-6 (child lists — the `map` primitive) ✅:** C# `Call` migrated to a JSON Rule, introducing the
`{"map":path,"sep":...,"side":...}` primitive for child *lists*. `map` stays pure interpreter logic (loop +
join) and reuses `emitChild`: it reads the list length from `get("<path>.count")`, then emits each element via
`emitChild("<path>.<i>", side)` — so an **indexed child path** (`node.args.0`) is first-class, needing *no* new
`EvalContext` method. The free-vs-bare callee split (`Program.f(...)` vs bare closure call) is a `case` on
`node.isFree`. `CsExprCtx` grew `node.callee`/`node.isFree`/`node.args.count` reads + indexed-arg `childExpr`;
the dead C++ `Call` switch case was removed. Byte-identical (run-diff 38/38, run-python 37/37, unit tests +2).
Next: `Member`/`Cond`/`Index` (scalar-ish child recursion, no new primitive), then `Target`→`BackendHandle`,
then TS/Python.

**Slice-7 (scalar-child family) ✅:** C# `Member`/`Index`/`Cond` migrated to JSON Rules — **no new primitive**,
just `emit`/`emitChild`/`get`/`case` over the IR: `Member` = receiver (`emitChild recv` or the `node.staticType`
qualifier) + `.`/`?.` + `ident(field)`; `Index` = `emitChild recv` + `[` + `emit index` + `]`; `Cond` =
`(cond ? then : els)` (plain `emit`). Added an **`ident` builtin** (per-target keyword escaping — C# `@base`),
forward-declared so `CsExprCtx` can reach it. `CsExprCtx` grew the Member/Index/Cond scalar reads + `childExpr`
arms; the three dead C++ switch cases were removed. Byte-identical (run-diff 38/38, run-python 37/37, unit tests
unchanged — the conformance programs exercise all three). Next: the delimited-list family (ListLit/Tuple/New/
MakeCase — reuses `map`) or `Target`→`BackendHandle`, then TS/Python.

**Slice-8 (delimited-list family, part 1) ✅:** C# `Tuple`/`ListLit` migrated — both are "delimited list of
child exprs", so they **reuse `map`** (no new primitive). `Tuple` reads its brackets from the spec's `delimited`
table (`{"get":"spec.delimited.tuple.open"}` — the affix stays *data*), joined by `map`. `ListLit`'s container
is the inherent BCL `List<T>` (list literals are built-in syntax, container fixed regardless of imports), so its
affix is a literal splicing an **`elemType` builtin** (renders the element type via `csType`). `CsExprCtx` grew
`spec.delimited.<key>.<field>` + `node.elements.count` reads, indexed `node.elements.<i>` `childExpr`, and the
`elemType` builtin; `csType` was forward-declared for it. Two dead C++ switch cases removed. Byte-identical
(run-diff 38/38, run-python 37/37, unit tests pass). Next: `New`/`MakeCase` (delimited args + a type-arg-suffix
builtin), then `Target`→`BackendHandle`, then TS/Python.

**Slice-9 (delimited-list family, part 2) ✅:** C# `New`/`MakeCase` migrated — `new Name<T,U>(args)`. Both reuse
`map` for the args (New over `node.args`, MakeCase over `node.fields`→each field's value) + a new
`typeArgsSuffix` builtin ("" or `<T, U>`: New's own `typeArgs`, or a MakeCase's *result-type* args, since a
generic union's case record is itself generic). `CsExprCtx` grew New/MakeCase scalar+childExpr arms (the
`node.args.<i>` handler now serves both Call and New). Two dead C++ switch cases removed. With this, **the whole
expression walk that has a stable per-node shape is data**; the residue still in the C++ `emitExpr` switch is
the genuinely-structural kinds (Interp/Char/Unary/Await/Cast/Extern/MethodCall/With/Bound/Lambda/Match — string
building, operator-method dispatch, pattern spelling). Byte-identical (run-diff 38/38, run-python 37/37, unit
tests pass). Next: `Target`→`BackendHandle`, then port the C# rule set + interpreter seam to TS/Python.

**Slice-10 (easy structural leftovers) ✅:** C# `Var`/`Extern`/`Cast`/`Unary` migrated. `Var` = `ident(name)`;
`Extern` = `get node.code` (raw verbatim); `Cast` = `(castType)(emit operand)` via a `castType` builtin (the
target type); `Unary` = `op` + `emitChild operand side:"unary"` — a new `"unary"` precedence side that wraps
**only** a binary operand (`-(a+b)`, but `-x`/`-(-x)` stay bare), distinct from `"recv"` which also wraps
unary/cast. `CsExprCtx` grew `node.name`/`node.code` reads, `node.op` now serves Unary too, `node.operand`
childExpr for Cast/Unary. Dead C++ cases removed; **only `This`** (stateful — an operator body rebinds `this`
to `lhs`) stays as a one-line leaf hook. Remaining imperative: Interp/Char/This/Await/MethodCall/With/Bound/
Lambda/Match — the genuinely per-target-shape or stateful kinds. Byte-identical (run-diff 38/38, run-python
37/37, unit tests pass). Next: `Target`→`BackendHandle`, then port the C# rule set + interpreter seam to
TS/Python.

**Slice-11 (`Target`→`BackendHandle`) ✅:** the compiled-in `enum class Target` is **gone** — a target is a
*name*. `BackendHandle` (polyglot.hpp) is a validated, immutable reference to a loaded backend: built-ins
resolve via **`findTarget(name)`** today; a future `loadBackend(specBytes)` for installed data plugins returns
the *same handle type*, so `compile()`'s signature never changes again. Validation at resolve, not emit: an
unknown name yields a `!ok()` handle carrying `error()` ("unknown target 'rust' (known targets: csharp,
typescript, python)"), and `compile()` with it refuses with that diagnostic. `compile(source, const
BackendHandle&, …)`; `analyze()` unchanged (as designed). CLI: `emitOne` takes the handle, the unknown-target
message now comes from `findTarget`, the LSP `polyglot/emit` handler resolves per-request, `targetFromString`
deleted. Tests: ~60 call sites mechanically `Target::X`→`findTarget("x")`, +3 new tests (resolve, unknown-name
error, compile-with-invalid-handle refusal). All gates green (unit, run-diff 38/38, run-python 37/37, samples
10/10; CLI smoke: unknown target exit 64 with the registry-derived list). Next: port the C# rule set +
interpreter seam to TS/Python.

**Slice-12 (hoist the shared seam — `IrExprCtx`) ✅:** pure refactor, zero behavior change, prep for the
TS/Python ports. `CsExprCtx`'s target-INDEPENDENT plumbing moved into a shared **`IrExprCtx`**
(emitter_base.hpp/.cpp): the whole path→IR mapping (`node.lhs`, indexed `node.args.<i>`, `node.fields.<i>.name`,
`spec.delimited.…`), the `side:"l"/"r"` precedence policy, and the spec-driven builtins
(intSuffix/escapeString/opSpelling). A backend subclass supplies only: `targetGet` (extra scalar reads its
rules test), `targetBuiltin` (keyword escaping / type rendering / numeric faithfulness), and `wrapAtom` (its
`recv`/`unary` paren policy). `CsExprCtx` shrank from ~180 lines to ~35 (the C# builtins + policy). Also
pre-added the shared reads TS needs (`node.mangledCallee`, `node.fields.<i>.name/.value`, Char's
`node.value`) — inert for C#. Byte-identical (unit, run-diff 38/38, run-python 37/37).

**Slice-13 (TS port, simple families) ✅ — the DSL's first non-C# consumer.** `TS_EXPR_RULES_JSON` +
`TsExprCtx` land in emit_typescript.cpp; 17 kinds route through the *same interpreter/primitives* as C#:
Int/Float/Bool/Null/Str/**Char** (a string — TS has no char), **Var** (verbatim — no keyword escaping),
**This** (a plain literal — only C# rebinds `this`), Extern, **Await**, **Call** (the overload-mangled
callee), Member, **Index** (a `case` on the new `node.receiverHasIndexer` predicate — TS has no `[]`
overload, `operator get` → `.get(i)`), Cond, Tuple/ListLit (spec `delimited` brackets), New. `TsExprCtx`
supplies the TS `wrapAtom` policy (recv wraps Unary + *scalar* Binary only — a user-type binary becomes a
high-binding method call), `node.receiverHasIndexer` (over the emitter's per-module indexer set, passed by
ref), and a tsType-based `typeArgsSuffix`. C# gains the identical `Await` rule for parity (its `atom()` ==
its recv policy). Proven with the strongest gate so far: **an old-vs-new emitted-source byte-diff across
all 38 programs × 3 targets — 114 files, zero differences** (plus unit/run-diff/run-python green). Still
imperative in TS: Unary/Cast/Binary (numeric faithfulness — next slice), MakeCase (needs the `map` item
template), Interp/MethodCall/With/Bound/Lambda/Match.

**Slice-14 (TS hard families + the `map` item template) ✅:** TS `Binary`/`Unary`/`Cast`/`MakeCase`
migrated — the numeric-faithfulness heart of the TS backend is now rule-selected data over fixed Core
builtins. **`map` grew an optional `item` template**: `{"map":path,"sep":…,"item":rule}` renders the rule
once per element with `item.…` paths rewritten onto the element's indexed path by an interpreter-internal
`ItemCtx` wrapper (no new EvalContext method — indexed child paths were already first-class); TS `MakeCase`
= `{ tag: "Name", f: v, … }` via `{"get":"item.name"}: {"emit":"item.value"}`. **`Binary`** is a 6-arm
ordered `case` mirroring the old C++ branch order exactly (record `==`→`.equals`; `==`/`!=`→strict `===`;
user-type op→`opMethod` call; i64→`i64Wrap` BigInt masking; small-int `*`→`imul`; small-int `+−/%`→
`narrowWrap`) — op-set tests are `or`-of-`eq` data, type-set tests are ctx predicates
(`node.typeIsSmallInt`/`node.lhsIsRecord`/`node.hasOpMethod`). **`Cast`** = one `convert` builtin (the
BigInt-boundary conversion algorithm stays fixed C++, like all §3.C machinery). `TsExprCtx` gained
`recordNames` (by ref) + the faithfulness builtins (`narrowWrap`/`i64Wrap`/`imul`/`opMethod`/`convert`);
dead C++ `child()` deleted. **TS expression residue now equals C#'s**: Interp/MethodCall/With/Bound/
Lambda/Match (+ per-target This/Char asymmetries already data). Verified byte-identical against the
slice-13 baseline (114 emitted files, zero diffs — transitively equal to pre-port) + unit (+1 item-template
test)/run-diff 38/38/run-python 37/37.

**Slice-15 (Python port — the non-sibling stress test) ✅. All three backends now share one interpreter.**
`PY_EXPR_RULES_JSON` + `PyExprCtx`: 20 kinds migrated in one slice (the shapes were all proven on C#/TS).
Python's per-target shape is pure DATA: `This`=`self`, `Cond`=`then if cond else els`, a 1-`Tuple` gets the
trailing comma (a `case` on `node.elements.count`=="1" + the indexed path `node.elements.0`), `MakeCase` is
a tagged dict with *quoted* keys (`escapeString(item.name)`), `New` has no keyword/type-args, `!`→`not `,
`&&`/`||`→`and`/`or` **moved into the spec's `binaryOp` table** (pyOp() deleted). The stateful machinery
became fixed builtins reaching the emitter's counters **by reference**: `nullSafeMember`/`nullCoalesce`
(walrus single-eval temporaries, `tmp_`), `idiv`/`irem` (set `needsIdiv_` so the `_pg_idiv` prelude
prepends), plus `wrapInt` (§3.C width masking), `ident` (keyword suffix `_`), `mangleName` (`$`→`_`),
`convert`. `Binary` nests `case` inside `wrapInt`'s arg (`/`→idiv, `%`→irem, else spelled infix) — rule
composition working as designed. pyId/pyName/wrapInt hoisted to free fns; dead `child()`/`pyOp()` deleted.
Python's residue: MethodCall/Lambda/Bound/Interp/Match (matches C#/TS modulo Python's lambda-chain Match).
Byte-identical vs the slice-14 baseline (114 files, zero diffs); unit/run-diff 38/38/run-python 37/37/
samples 10/10 green. **The TS/Python port milestone the P18 plan called for is complete** — next: the
loader-validation slice (every claimed feature has a rule; "no rule"→hard error), then flip-default +
delete-the-C++-switch residue per family as the remaining kinds migrate, then std de-hardcoding /
distribution.

**→ P18's remaining tail (expression residue, declarations, loader, std arms, distribution) is superseded
by §P19 below** (2026-07-02, from the second 4-agent investigation). P18 stands as ✅ slices 1–15: the
interpreter + all three backends' specs/expression rules over one shared seam + `Target`→`BackendHandle`.

## P19 — 100% JSON plugins: the complete artifact (designed 2026-07-02; PRD §4.11, design note
`docs/design/json-plugins.md`; from a 4-agent investigation: declarations / hard exprs + types / builtin
catalog / artifact + loader)

**Goal.** Close everything P18 left imperative so a language plugin is *entirely* JSON (npm-wrapped, no
plugin code, steady-state zero Core changes for new languages), ending with a downloaded 4th backend
emitting with no Core change. Verdicts: declarations ~90→98% data (<2% = §3.E refusals); expr residue +
type renderers ~95%; builtins collapse to a ~10-entry generic catalog (pioneer-pays-once via
`requiresCore`); the artifact = manifest + rules + tri-state capabilities + std overlays, validated
fail-loud at load ("every IR node kind has a rule OR its capability is `false`").

**Discipline (unchanged):** extract from working backends, never guess; every slice byte-identical
(emitted-source diff across all conformance programs × 3 targets — the harness built in P18 slices 13–15);
**each lowering absorption is its own gated slice** (a lowering bug is invisible to a two-backend diff that
consumes the same wrong fact); refusals loud, never sentinels.

**Slices:**
1. **Latent-miscompile fixes (independent, do first):** Python statement-bodied lambda emits
   `__py_unsupported_block_lambda__` and Python `With` emits `__py_unsupported_expr__` into "valid" output
   (the P9-V failure mode, live today) → real refusals (or, for `With`, slice 2's lowering rebuild). Plus
   `i32.parse`/`f64.parse` → std `Bound` bindings (deletes `MethodCall`'s per-target parse special case —
   the P13 "std, not compiler" move).
2. **Lowering absorption** (one fact per sub-slice, each byte-gated): `lhsIsRecord`/`hasOpMethod`/
   `receiverHasIndexer`/`base.isInterface` bits; `With`→ordered `ctorArgs`+`baseIsSimple`+lowered
   `tempName` (also gives Python `With` for free); `This`→`Var("lhs")` in operator bodies; match
   `hasCatchAll`/`genArgs`/binder accessors; walrus temp names.
3. **Expression residue → rules**, per family across all three targets: `Interp` (**`interleave`**
   primitive + `interpEscape` builtins), `MethodCall` (post-slice-1 it's a 2-arm case + `map`), `Lambda`
   (**`emitBlock`** primitive for statement bodies), `Char` (a `charLit` builtin) + `This` (a literal after
   slice 2), `With` (a `New`-shaped template after slice 2), `Match` last (**`fold`** primitive + arm/
   binder/pattern paths — the largest rules in the DSL).
4. **Type-rule tables:** the **`type`** primitive + `TypeEvalContext` (`type.kind/.nullable/.isValueType/
   .scalar/.externType/.args.<i>/.ret`); per-target tables replacing `csType`/`tsType`/`pyTypeName`;
   `substTypeTmpl` becomes one shared fixed builtin. Generic bounds move to the decl tables (slice 5).
5. **Declaration rules:** the emitter-rule flavor (**`line`/`block`/`mapDecl`** reusing `EmitterBase`'s
   block machinery) + per-target decl tables (enum/interface/union/record/class/method/function/extension/
   global, incl. synthesized members as templates + `any`/`all` Tests) + the **`program`** scaffold rule +
   **`require`** preamble buckets (replaces `needsAsyncio_`/`needsIdiv_`). Largest volume; per-decl-kind
   sub-slices, each byte-gated.
6. **Generic builtin catalog:** parameterize `escapeString`'s escape set (spec data — PHP's `$`); add
   `typeIs`/`isKind` Tests; consolidate per-target builtins into catalog entries (`table`, `ident`
   w/ keyword-set+strategy, `mangle`, `wrap` w/ `intRepr` enum, `convert` as a per-plugin
   (fromClass,toClass)→Rule matrix, `wrapAtom` kind-sets as spec data). Byte-identical consolidation, no
   behavior change.
7. **Delete the C++ backends — the CLI becomes a pure engine (user decision 2026-07-02: no compat
   fallback, no embedded specs):** `loadBackend(bytes)`→`BackendHandle`; one `InterpretedBackend`; the
   three first-party backends move to **real plugin packages in this repo** (`plugins/csharp|typescript|
   python/` — manifest + spec/rules files on disk), loaded via pgconfig local `file:` dependencies; delete
   `emit_csharp/typescript/python.cpp` + `kRegistry[]` + every embedded spec/rule string. Conformance/test
   harnesses point at the repo's `plugins/` (no registry needed in CI). No pgconfig/targets → actionable
   error, never a fallback.
8. **Loader validation + tri-state capabilities:** the full fail-loud obligation catalog (node-kind
   coverage OR capability `false`; builtin/path references against a Core-published catalog; `requiresCore`
   + `schema` axes; depth caps; unknown-primitive refusal / unknown-manifest-key warn) +
   `native|emulated|false` capabilities (StubBackend gating still bites; `emulated` warns).
9. **Std skeleton/overlay split:** extract the ~97 `actual(...)` arms into **each first-party plugin's**
   `std/*.overlay.json`; `compiler.cpp` keeps only the target-neutral skeletons; collapse
   `ir::Bound{cs/ts/py}`→`{template}` + `ExternType` 6→2 fields; per-target selection moves into linking;
   missing-arm call-site refusal rides `checkCapabilities`. **Highest-risk gate** (every conformance
   program uses List/Math).
10. **Package format + config:** `polyglot-plugin.json` (+ sibling files per `json-plugins.md` §5);
    `pgconfig.json` gains `dependencies` (incl. local `file:` paths for in-repo/plugin-dev use) +
    `targets`; resolution = dependencies → lockfile-pinned cache → registry → "run `polyglot install`"
    (no compiled-in tier).
11. **`polyglot install` + registry:** npm HTTP data-only fetch, SHA-512 verify, zip-slip-safe extract, no
    lifecycle scripts, install-time `loadBackend` validation, `registry.json` + `pgconfig.lock.json`,
    warn-only sink lint.
12. **The proof (P10's gate):** a **downloaded 4th backend** (candidate: Kotlin or PHP — PHP also exercises
    the parameterized escape set) emits a working program with **zero Core change**; LSP `polyglot/targets`
    lists it with no client change (closes `extension.js`'s `FIXME(P10)`); off-intersection features refuse
    with distinct §3.B-vs-§3.E diagnostics.

**Slice-1 ✅ (2026-07-02):** the two latent §3.B silent-broken-output bugs are now **capability refusals**:
`Feature::BlockLambdas` + `Feature::WithExpressions` added (capability.cpp detects a Lambda with `flag`
set / a `With` expr; `PythonBackend::supports` returns false for both; C#/TS unaffected) — a Python compile
of either now refuses with a named diagnostic instead of emitting `__py_unsupported_block_lambda__`/
`__py_unsupported_expr__` sentinels into "valid" output. (P19 slice 2's `With` ctor-rebuild lowering flips
`WithExpressions` back on for Python.) **`i32.parse`…`f64.parse` are std bindings now**: ten
`extern class <scalar> { static fn parse(s: string): <scalar> { actual(…) extern("…") } }` declarations
appended to std.core; lower's existing static-binding path (`bindings_["i32.parse"]`) produces `ir::Bound`,
so all three backends' `MethodCall` parse special cases (+ both `isPrimNumeric` helpers) are **deleted** —
sema still types the call intrinsically. One sema change: `declareType` treats an *extern* class naming a
builtin scalar as a **member carrier**, not a shadow (the scalar type stays authoritative; the name is not
registered) — non-extern user types still get "'i32' shadows a builtin type". Gates: byte-identical vs the
P18-final baseline (114 files, zero diffs), unit +4 (refusal tests), run-diff 38/38, run-python 37/37,
samples 10/10, fidelity 10/10.

**Slice-2 ✅ (2026-07-02) — lowering absorbs the expression-layer module facts; `With` is data on all three
targets.** IR gains lowering-precomputed facts: `Binary.lhsIsRecord`/`lhsIsUserType`, `Index.
receiverHasIndexer`, and **`With.ctorArgs`/`tempName`/`baseIsSimple`** (the record's fields in decl order,
each the override or a `<base>.field` read; non-simple bases get a `__w<n>` temp for single eval —
`Lowerer` grew `recordNames_`/`records_`/`indexerTypes_` tables). The shared `IrExprCtx` reads them, so:
`TsExprCtx` lost its `indexerTypes`/`recordNames` by-ref params (its `targetGet` keeps only
`typeIsSmallInt` + `hasOpMethod` = the lhsIsUserType bit × the TS opMethod table; `wrapAtom` reads the
bit); the TS emitter dropped `recordFields_`/`indexerTypes_`/`tmp_` + its `noteIndexer` scan. **`With` is
now a rule in all three tables**: C# native (`base with { f = v }` via a `map` item over `node.fields`),
TS/Python ctor rebuild (`case` on `baseIsSimple`; TS IIFE / Python lambda for the non-simple temp) —
**`WithExpressions` flips back on for Python** (only `BlockLambdas` still gates). Deviations from the
plan, both principled: `This`→`Var("lhs")` was NOT moved to lowering — the rebind is a *C# declaration
shape* consequence (TS keeps `this`, Python `self`), so it stays a C#-ctx concern for slice 3; Match/walrus
facts land in slice 3 *with their consumers* (a fact nobody reads can't be meaningfully byte-gated). New
conformance program **with_update.pg** (simple + non-simple bases, all targets) — gates now **39/39 C#-TS,
38/38 Python**; pre-existing 114 emitted files byte-identical; unit tests updated (+With-rebuild goldens).

**Slice-3a ✅ (2026-07-02) — `Interp` is data on all three targets via the `interleave` primitive; C#
`Char`/`This` migrated.** New Rule kind **`interleave`** (`{"interleave":{"lits":path,"holes":path,
"lit":rule,"hole":rule}}`): zips the chunk list with the hole list (`lit0 hole0 lit1 … litN`), each element
scoped through the existing `ItemCtx` (`item` = the chunk text via `get` / the hole expr via `emit`).
Shared `IrExprCtx` gained `node.chunks.count`/`node.chunks.<i>` (scalar text) + `node.holes.count`/
`node.holes.<i>` (child). Per-target Interp DATA: C# `$"…"` + `interpEscape` builtin (brace doubling), TS
backtick + its `interpEscape` (`` ` ``/`\`/`\$`-before-`{`), Python `escapeString` chunks joined by
`" + str(hole) + "` — no outer delimiter. C# **`Char`** = a `charLit` builtin; C# **`This`** = a `case` on
the new `ctx.thisAlias` context scalar (the operator-body `this`→`lhs` rebind stays emitter state — a C#
declaration-shape consequence — but the *rule* is data; `CsExprCtx` takes the alias by ref). Dead C++
cases deleted: C# Interp/Char/This, TS Interp, Python Interp. **Expression residue everywhere is now just
MethodCall/Bound/Lambda/Match.** Byte-identical (117 files), unit green, 39/39 + 38/38.

## Stretch (unordered, post-P10)
- **Further targets** as downloadable declarative backends (the IR is target-neutral by design).
- **Source maps:** thread positions through every pass for debuggable JS output; decide the C# debug story.
- **Editor tooling — full detail (now tracked above):**
  - **Syntax highlighting** — a TextMate grammar (`.tmLanguage`/`.json`) for `.pg`. Independent of the
    compiler (no frontend reuse needed) and **can land early/cheaply**: it gives VS Code (and most editors)
    coloring immediately, and Visual Studio 2022+ consumes TextMate grammars too. Keep it in sync with
    `docs/lang/grammar.ebnf`. This is the lowest-effort, highest-visibility tooling win.
  - **LSP server** — `polyglot lsp`, built on the **frontend-as-a-reusable-library** (lexer/parser/sema),
    so the CLI and the language server are thin clients over the same core (diagnostics, hover, go-to-def,
    completion from the symbol tables). Editor-agnostic protocol.
  - **VS Code extension** — bundles the TextMate grammar (ships first, standalone) and later an LSP client
    pointing at `polyglot lsp`.
  - **Visual Studio extension (VSIX)** — an LSP client (VS's LSP support) for the same server; can reuse
    the TextMate grammar for coloring. (NB: the build toolchain is already VS-2026/v145 — see top of this
    file; a VSIX targets that generation.)
  *Gate:* `.pg` files are colorized in both VS Code and Visual Studio from the shared grammar; the LSP
  surfaces compiler diagnostics live in both, with no logic duplicated outside the frontend library.
- **Binding auto-generation:** shape-only bindings from `.d.ts` / .NET metadata / WebIDL + hand overrides
  (feeds the plugin ecosystem; produces declarative data at authoring time).
- **Plugin registry & signing:** distribution/versioning infrastructure + signature trust for downloads.
