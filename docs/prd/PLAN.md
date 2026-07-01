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
10. Stamp each module's `fileId`/URI at its lex boundary (`loadImports`/`linkCoreModule`); embedded std → a
    `polyglot:<name>` virtual document the server serves from `STD_MODULES`. Cross-module go-to-def.
11. `references`/`rename`; `completion` (keywords + in-scope names first; member completion last — needs receiver type).

**P16d — Visual Studio LSP client.** An `ILanguageClient` VSIX pointing at `polyglot lsp` (after the VS Code
client proves the server); target the VS-2026/v145 SDK generation.

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
