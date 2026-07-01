# Design note — the declarative backend spec (P9)

> Concrete realization of [`plugins-and-targets.md`](plugins-and-targets.md) §4 ("backends are declarative
> plugins") for the zero-dependency C++ core. This is *extracted* from the hand-written backends
> (`emit_csharp.cpp`, `emit_typescript.cpp`, `emit_python.cpp`), per the §4 discipline — never guessed.
> **Status: DSL extracted and validated across three backends (✅ 2026-07-01).** The shared engine
> (`EmitterBase`) + the `BackendSpec` declarative data + the documented hook surface are extracted; all
> per-target *data* now lives in `BackendSpec` (§3), and all three backends are `{Spec + Hooks}` instances
> over the one engine. The expression walk and declaration shapes proved **irreducibly per-target** — a
> conclusion now backed by a third, non-sibling backend (Python), not a two-backend guess (§6).

## 1. What a backend actually is, after extraction

A structural catalog of the two emitters (every emission decision, June 2026) found the split is roughly
**70% tabular / 30% imperative**. So a backend is **not** a pure template file; it is:

```
Backend  =  Spec (declarative data)            // the 70%: tables + per-node templates
          +  Hooks (C++ implementations)        // the 30%: target-specific imperative codegen
          +  capability set (§3.E Feature flags)
```

The **core owns a single emit engine** that walks the IR, manages precedence/parenthesization and
expr-vs-stmt context, and emits each node by consulting the active backend's Spec — calling a Hook only for
the node kinds the Spec marks as imperative. C# and TS become *two instances* of (Spec + Hooks); adding a
target is (eventually) a new Spec + the few Hooks it needs, with **no engine change**.

Why hybrid and not pure-declarative: a flat template-per-node format produces wrong output for the gnarly
cases (numeric faithfulness, BigInt boundaries, `tsConvert`, TS `try` lowering, pattern matching). §4
predicted this ("the format inevitably becomes a small DSL … the full-power-local tier covers what the DSL
can't"). The Hook tier *is* that full-power tier, expressed as C++ here because the core is native and
zero-dep (no embedded scripting runtime). A downloaded backend (P10) gets the declarative Spec only — never
Hooks — so the §4 "downloaded = data, never code" safety rule still holds; bundled backends (C#/TS) may use
Hooks because they ship in the trusted core.

## 2. The declarative / imperative boundary (from the catalog)

**Declarative (lives in the Spec — data only):**
- **Type leaf table** — scalar `.pg` type → target string (`i8`→`sbyte`/`number`, `i64`→`long`/`bigint`, …),
  plus the structural rules for `List`/`Iterable`/tuple/function/nullable/generic-bounds (per-target affixes).
- **Literal rules** — int-width suffix table (`i64`→`L`/`n`, `u64`→`UL`/`n`), the (identical) string-escape
  table, bool/null spellings.
- **Operator table** — symbol per op + the precedence table (identical across targets) + parenthesization
  rules (by node kind).
- **Per-node templates** — for the ~50% of nodes that are a fixed shape with child slots: `New`, `Index`,
  `ListLit`, `Tuple`, `Bound`, `MakeCase`, `Cond`, `Member`, plain `Call`, and most statements (`Let`,
  `Assign`, `Return`, `If`, `While`, the three `For` shapes, `Use`). Templates differ per target by affix
  (`var`/`let`, `foreach…in`/`for…of`, `(A,B)`/`[A,B]`).
- **Declaration scaffolds** — enum/union/record/class/function/extension shapes, the program wrapper
  (`static class Program`+`Main` vs bare top-level + entry call), generic-bounds syntax (`where` vs
  `extends`), `print`→`Console.WriteLine`/`console.log`, the `Math` qualifier.
- **Naming rules** — free-fn qualification (`Program.` vs bare), the `Math.<fn>` rename table
  (`ln`→`Log`/`log`, `ceil`→`Ceiling`).

**Imperative (lives in a Hook — C++):**
- **`Cast`** — C# is a template `(T)(x)`; TS `tsConvert` is a multi-branch BigInt/narrowing matrix.
- **Numeric faithfulness** — C# sub-word cast-back (`(byte)(a+b)`); TS small-int `narrowTs` + i64/u64
  `BigInt.asIntN/asUintN` boundary wrapping on each op.
- **Binary dispatch** — TS user-type operator→method (`a.plus(b)`), record `==`→`.equals()`.
- **`MethodCall` specials** — `i32.parse` per-type conversion; `Math.min/max/abs` on BigInt → comparison IIFE.
- **`Match`** — C# `switch` expression vs TS IIFE/if-chain.
- **`Try`** — C# native `catch…when` vs TS `__handled`/`instanceof` dispatch chain.
- **Interpolation** — different literal syntax + escape (`$"…{e}…"` vs `` `…${e}…` ``).
- **Record `.equals()`** (TS) — recursive per-field comparison.
- **`Call` BigInt-print** (TS) — wrap an i64 arg in `String()`.

Pre-computed-in-lowering (neither Spec nor Hook — already IR input): overload mangling (`mangledCallee`),
entry detection (`isEntry`), extension-call flag (`isExtension`), the `Bound` `$this/$0` FFI templates.

## 3. C++ shape (as realized — corrected from the original sketch)

The original sketch guessed a `SpecEmitter` that rendered *every* node from `spec` and dispatched fine-grained
per-node `BackendHooks` (`emitCast`, `emitTry`, `narrowBinary`, `emitMatch`, …). Extraction proved that wrong:
the cleanly-shared layer is the **statement walk**, not the expression walk, so the realized split is:

```cpp
// 1. Spec — ALL per-target data, in one struct (backend_spec.hpp). Every backend (incl. Python) has one:
struct BackendSpec {
    std::string name;                                                  // "csharp"/"typescript"/"python"
    std::unordered_map<std::string, std::string> scalarType;           // "i8" -> "sbyte" / "number"
    std::unordered_map<std::string, std::string> intSuffix;            // "i64" -> "L" / "n"
    std::unordered_map<std::string, std::string> binaryOp;             // "==" -> "===" (TS); C# verbatim
    std::unordered_map<std::string, DelimitedTemplate> delimited;      // "tuple"/"list" bracket affixes
    BlockStyle blockStyle;                                             // Allman / K&R / colon+indent
    std::string stmtEnd;                                              // ";" / "" (Python)
    std::string throwKeyword;                                         // "throw" / "raise"
    std::string trueLit, falseLit, nullLit;                          // "true"/"false"/"null" vs Python's
    std::string binOp(const std::string&) const;
};
// plus shared, target-identical render primitives (renderDelimited/renderArgs/renderCond/renderString) and
// the operatorPrecedence() table — engine concerns, not per-backend data.

// 2. Engine — EmitterBase (emitter_base.hpp/.cpp) owns the IR *statement* walk + output buffer/indentation:
//    line/blockBody/headBlock/inlineBlock, every leaf statement, the if/while/for trio, Let/Yield/Throw/Use/
//    Break/Continue. It reads all per-target data through the one `spec()` accessor.

// 3. Hooks — the pure virtuals EmitterBase calls (the backend↔engine contract), now cleanly split:
//    (a) data:      spec() — returns the BackendSpec (replaced the old blockStyle()/stmtEnd()/throwKeyword()
//                   constant-hooks; those were data, so they moved into the Spec);
//    (b) wholesale per-target walks: emitExpr (the entire expression walk) and emitStmtTarget (For + Try);
//    (c) fine-grained behavior (real per-target logic, not constants): localDecl, yieldStmt, rethrowStmt.
```

The DSL-extraction endpoint is thus reached to the extent the architecture allows: **a backend's declarative
description is exactly `BackendSpec`** (types, literals, operators, brackets, block style, terminators), and
the residual C++ is the irreducible imperative tier — the expression walk and declaration emitters — which
three backends confirm cannot be flattened to data without a full embedded DSL the zero-dep core forbids
(§4's "full-power local tier"). A hypothetical fourth *sibling* target could be a new `BackendSpec` + the few
hooks; a non-sibling still needs its own imperative tier, exactly as Python did.

Why expressions stayed wholesale-per-target: almost every expression node spells differently per target
(numeric narrowing/BigInt boundaries, `Cast`/`tsConvert`, `Match`, interpolation, operator-method dispatch),
so a base expr-walk would handle a handful of nodes and delegate ~20 — inverting the statement situation.
The cleanly-shared expr structure (`Cond`/`Tuple`/`ListLit`/arg-lists) is captured by the shared *render
primitives* instead, called from within each backend's `emitExpr`. Likewise the declaration emitters
(enum/union/record/class/function/extension) are per-target *by shape* and stay in the concrete backends.
This is the design's "full-power local tier"; the data-only declarative-DSL endpoint is deferred to the
third backend (P10) that would justify extracting it — never guessed (the §4.3 discipline).

## 4. Incremental migration (keep the gate green at every step)

> **This was the original plan; the realized path diverged — see §3 and the PLAN P9 slice log (1 → 4f).**
> In particular, steps 3–4 below guessed a node-rendering `SpecEmitter` + per-node `BackendHooks`; extraction
> instead lifted the *statement* walk into `EmitterBase` and left the *expression* walk and declaration shapes
> as each backend's per-target imperative tier. Steps 1–2 and 5 landed substantially as written.

The P9 gate is **byte-for-byte parity with the current native output** — enforced continuously by the
existing golden unit tests + the 37-program differential conformance + the 10-sample round-trip. So the
extraction proceeds in slices, each a no-op on output:

1. **Type-leaf + literal-suffix tables → `BackendSpec`** (this commit). Both emitters consult the spec for
   scalar leaves and int suffixes instead of hardcoded `if`-ladders. Smallest safe proof that "spec is data
   the emitter reads." *(structural type cases, operators, and node templates stay in code for now.)*
2. Operator symbol + precedence + parenthesization tables → spec.
3. Per-node **templates** for the fixed-shape nodes; introduce the `SpecEmitter` engine that renders them,
   delegating unsupported kinds back to the native emitter (hybrid bridge).
4. Declaration scaffolds + program wrapper. **Revised after extraction (done):** the statement layer lifted
   cleanly into `EmitterBase` (leaf statements + the `if`/`while`/`for` trio + `Let`/`Yield`/`Throw`/`Use`,
   with brace style and a few spellings behind hooks), and all block emission unified on `headBlock`/
   `blockBody` (the per-target `emitBlock`s deleted). But the *declaration* shapes proved fundamentally
   per-target (C# `record`/`abstract record` unions/operators/indexers/properties/`this`-extensions vs TS
   classes/tagged-union aliases/getters/free-functions/structural `equals`) — sharing them would be shallow
   hooks hiding nothing, so they stay as concrete-emitter code (this *is* the imperative 30%). The full
   declarative-scaffold DSL for declarations is deferred until a third backend exists to extract it from
   (the "never guess the format" discipline), not invented speculatively now.
5. Formalize the **Hook interface**; the residual imperative code (incl. the per-target declaration emitters)
   becomes the documented hook surface.
6. Re-express C# and TS as `{Spec, Hooks}` pairs over the one engine; delete the bespoke per-file walking.

At the end, the two `.cpp` files are *data + a handful of hooks*, and P10 can add a downloaded **Spec-only**
backend (Python) with no engine change — the §4 endpoint.

## 4a. Plugin classes (`extern class`) — type-mapping + construction (P10, ✅ 2026-06-30)

The `extern class` + per-target `actual(target) extern("…")` binding arms (the "binding" plugin mechanism)
work for **any** type, std or user-authored. Beyond **member/property access** (`w.poke(3)` → `w.Poke(3)`),
a plugin class now also declares:
- **Type-name mapping** — a `type { actual(csharp) extern("global::Acme.Widget") actual(typescript) extern("Widget") }`
  block. `$0,$1,…` in the template are the rendered type args, so a generic structural type maps too
  (`List<T>` → C# `…List<$0>`, TS `$0[]`). `csType`/`tsType` consult this (an `ir::ExternType` registry on the
  `ir::Module`, set per-emit) instead of a hardcoded name.
- **Construction** — binding arms on `init`: `init(n) { actual(csharp) extern("new $T($0)") … }`, where `$T`
  is the mapped type spelling and `$0,…` are the ctor args. `Type(args)` lowers to an `ir::Bound` (not
  `ir::New`) when the type has a ctor binding.

**All std/core types are dogfooded onto this — the emitters carry zero hardcoded type mappings:** **`List`**
declares its type + ctor in `std.collections`; **`Error`** (type → `System.Exception`, ctor, and the
`message` property) and **`Iterable`** (type-only → `IEnumerable<$0>`) are `extern class`es in the
always-linked **core prelude** (`compiler.cpp` `STD_CORE`, merged into every compilation since `throw`/
`catch`/`yield` are core surface needing no import). `csType`/`tsType` resolve every nominal type through the
`ir::ExternType` registry or the user's own declaration — there is no longer a `List`/`Error`/`Iterable`
branch anywhere in `emit_csharp.cpp`/`emit_typescript.cpp`.

## 5. Capability sets

Each Spec carries its §3.E `Feature` set. C# and TS both declare the **full §3.A surface**. Python's set
*grew* feature-by-feature during the §6 spike — proving `capability.cpp` refuses out-of-intersection use for
real (not just the StubBackend test) — and, now that the spike is complete, declares the full surface too
(`supports` → `true`). The StubBackend test still proves gating bites.

## 6. Third-backend validation (Python) — what the spike proved (✅ 36/36)

P9 deferred the data-only declarative DSL until a third backend exists to extract it from (never guess). A
native **Python** backend (`emit_python.cpp`, a `PythonEmitter : EmitterBase`) was brought up to *validate*
this architecture against a non-sibling target — colon+indent, no statement terminators, no `var`, builtin
`print`. It now covers the **full §3.A surface: all 36 conformance programs (incl. the FruitCake north star)
transpile to Python byte-identical to the C# oracle** (`run-python.ps1`). The findings sharpen the design:
- **The engine was brace-family-specific, not fully target-neutral.** Python forced `EmitterBase` to
  generalize from a 2-way brace bool to a 3-way `BlockStyle` {BracesAllman, BracesKnR, ColonIndent} + a
  `stmtEnd()` terminator hook, later a `throwKeyword()` hook + a block-style-agnostic `Use` (all verified
  C#/TS no-ops). *After* that, the shared statement walk served Python unchanged — so the §1 split is sound,
  and the engine's "shared" surface is precisely the statement walk, not anything brace/semicolon-specific.
- **Declarations stayed per-target**, confirming §3/§4's call: `emit_python.cpp` hand-writes `class`/`__init__`/
  `__eq__`/`def`, dunders, generators, `except T as e`, match-as-ternary, etc. Python's versions are often
  *cleaner* (structural `==` via `__eq__` dispatch; no `new`; arbitrary-precision `int`).
- **The per-target FFI binding mechanism generalized to a third arm:** `ir::Bound` gained a `pyTemplate`
  (+ `ExternType` `pyType`/`pyCtor`); std `actual(python)` arms brought Math/List/strings to Python with the
  emitter carrying zero hardcoded mappings — the same data path P10 dogfooded for C#/TS.
- **It surfaced three latent bugs, all fixed at the root** (a non-sibling backend earns its keep): (1) the
  always-linked `extern class Error`'s `message` tripped `Properties` for every program once a backend
  declared a non-full set — the Collector now skips extern-class members (bindings); (2) Python's builtin
  `print` collides (→ `__builtins__.print`); (3) **`break`/`continue` were silently dropped in lowering for
  *every* target** (`default: return nullptr`) — a §3.B miscompile the C#/TS diff gate structurally could not
  catch (both dropped them identically, so they agreed). Now `ir::Break`/`Continue` emit in the shared engine.

The declarative DSL (the P9 endpoint) was then extracted from these **three** working backends instead of
two-plus-a-guess — see the header + §3: all per-target data consolidated into `BackendSpec` (blockStyle/
stmtEnd/throwKeyword lifted from constant-hooks, bool/null spellings, string escaping → the shared
`renderString`), each backend reduced to `{Spec + Hooks}`. Full slice log: `../prd/PLAN.md` §P9 / §P9-V.
