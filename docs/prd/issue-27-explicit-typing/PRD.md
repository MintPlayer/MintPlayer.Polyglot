# Explicit element types, array/list spellings & union-in-collection composition (PRD)

> **GitHub:** grew out of [MintPlayer/MintPlayer.Polyglot#27](https://github.com/MintPlayer/MintPlayer.Polyglot/issues/27)
> — *"TypeScript emitter: local `List<T>` declarations lose their type annotation; `List<T?>` renders
> without parentheses"* · labels `bug`, `codegen`, `lang-design`. Per the maintainer's decision the whole
> change (root-cause language fix, not just the two symptoms) is resolved **under issue #27 in a single
> pull request**; `T[]` arrays are **in scope**.

- **Status:** Draft v1.0 · 2026-07-13 · ships as a lockstep **minor** (language-spec change + new type
  spelling) — the PR carries `release:minor`; versions are build-injected `0.0.0-dev` placeholders in-tree
  (P24), so no source version bump.
- **Author:** Pieterjan (with Claude Code).
- **Provenance:** a 3-agent read-only investigation (current-semantics · per-target-requirements ·
  blast-radius+precedent), verified against the source. Findings are folded in below with `file:line`.
- **Reporter context:** surfaced while single-sourcing an AlphaZero chess engine to the browser — the
  generated `chess_solver.ts` was the first program to use tuple-typed local lists (`List<(i32,i32)>`) and
  a nullable list (`List<Node?>`). Issue #27 fixed the two symptoms at the backend; this PRD fixes the
  **root cause at the source language**, per the *principled-fix-over-workaround* rule in `CLAUDE.md`.

---

## 1. Problem

Issue #27 was two TypeScript miscompiles that both trace to the same design gap: **the source language
lets a declaration have a type that no target can reconstruct, and each backend then has to *guess*.**

- **Bug 1 — un-inferable initializers lose their element type.** `var d: List<(i32,i32)> = List<(i32,i32)>()`
  emitted `let d = [];` in TS. A bare `[]` is an *evolving-any*: after `d.push([1,2])` it becomes
  `number[][]`, not the declared `[number, number][]` (TS2322 under `strict`; a silent `any[]` otherwise).
  The same class covers a bare `null` local. The C# side already dodges this only because
  `new List<…>()` / `new List<…>{}` bakes the element type into the *expression* — a backend workaround,
  not a language guarantee.

- **Bug 2 — a union element type mis-parenthesizes inside a postfix collection.** `var next: List<Node?>`
  emitted `next: Node | null[];`, which TS parses as `Node | (null[])` — "a `Node`, or an array of `null`"
  — not `(Node | null)[]`. A real miscompile of the type.

The investigation confirmed **the source language accepts an un-inferable declaration silently** — no
diagnostic — and hands emission an empty-name "unknown" type that each backend fills with its own fallback
(C# `object`, TS `unknown`) (`sema.cpp:983-988,944`; `plugins/csharp/…:2679-2683`,
`plugins/typescript/…:3077-3081`). That is a **§3.B-class silent imprecision**: the program type-checks,
but the emitted type is not the one the author declared.

The honest fix is not more backend guessing. It is to **require the annotation at the source whenever the
initializer conveys no type**, so every backend is *always* handed a concrete type — turning a silent
guess into a fail-fast diagnostic, and making "collections compose with union element types" a spec
invariant rather than a per-target patch.

This PRD covers three linked pieces:

- **A. The inference rule** — require an explicit type when (and only when) the initializer is un-inferable.
- **B. Array vs list spellings** — introduce a `T[]` array type alongside `List<T>`, with target mappings
  chosen so both are idiomatic (both → a JS array on TS; C# keeps `T[]` vs `List<T>`).
- **C. Union-in-collection composition** — a precedence/parenthesization invariant so a collection of a
  union element is always valid on every target (the general form of Bug 2; already fixed for TS nullable).

---

## 2. Current reality (investigation findings)

- **Declarations.** `local_decl = ("let"|"var"), pattern_binding, [":" , type], "=", expression`
  (`grammar.ebnf:224`). The annotation is **optional**; the initializer is **mandatory** (there is no
  uninitialized form). Module-scope `let` mirrors this. Fields, parameters, properties **already require**
  a type (`grammar.ebnf:64,85,103,116,145`). So the gap is exactly the two inferred contexts: local
  `let`/`var` and module `let`.
- **Inference.** `checkStmt` for `Let` is bottom-up-first: `checkExpr` yields a type; with an annotation,
  `checkConvert` runs the target-directed pass; without one, the variable takes the raw bottom-up type,
  **with no error even when that type is Unknown** (`sema.cpp:822-827`, lenient `tUnknown()` at `:12-17`).
- **The un-inferable set (today, all accepted silently):**
  | Initializer | Bottom-up type | Behavior |
  |---|---|---|
  | `[]` (empty list) | `List<unknown>` (empty-name elem) `sema.cpp:983-988` | emits C# `List<object>` / TS unknown-elem |
  | bare `null` | nullable empty-name `sema.cpp:944,19` | declares nullable-unknown |
  | bare lambda, un-annotated params, no target | `tUnknown()` `sema.cpp:1022-1032` | params typed Unknown |
  | empty map/set literal | — | **no such literal exists** (`grammar.ebnf:297-311`) |

  Every *unambiguous* initializer already infers precisely: `[1,2,3]` → `List<i32>` (first-element),
  `List<i32>()` → `List<i32>`, any typed expression. The bidirectional element-coercion
  (`sema.cpp:745-749`) rescues `[]`/`null` **only when a target type is present** — the asymmetry this
  rule removes.
- **Types & collections.** There is **no `T[]` array *type* spelling** today — the only collection type is
  `List<T>` (`grammar.ebnf:192-193`; SPEC §4.1). The type grammar has **no `|` alternation**: a type is
  `named | tuple | function`, optionally `?` (`grammar.ebnf:185-195`). So the only "unions" are (a) nominal
  `union` ADTs — lowered to C# record hierarchies / TS tagged unions — and (b) the concrete `T?` nullable
  sugar rendering `T?` (C#) / `T | null` (TS). A generic `T?` desugars to the core `Option<T>` union
  (`sema.cpp:450-459`); a concrete `T?` stays a native nullable.
- **Union-in-collection (the Bug 2 mechanism).** `List<X>` renders by substituting each type-arg through
  the full Type rule (`emitter_base.cpp:488-501`). TS `List.type` is the **postfix** `$0[]`
  (`plugins/typescript/…:3479`); C# is the **angle-bracket** `List<$0>` (`plugins/csharp/…:2739`). Postfix
  `[]` binds tighter than `|`, so a bare `Node | null` element becomes `Node | null[]` = `Node | (null[])`.
  **Fixed in this branch** by parenthesizing the TS nullable at its source (`(base | null)`), so
  composition yields `(Node | null)[]`. C# is inherently safe (angle brackets self-delimit; nullable is a
  tight postfix `T?`).
- **Blast radius = zero.** All 64 `.pg` files + inline test programs: every existing `[]`/`null`
  declaration **already carries an explicit annotation** (`empty_list.pg:8,11`; `null_local.pg:12,23`).
  Only *non-empty* literals rely on inference (`samples/03…:41`, `08…:24`, `06…:27`) — which the rule
  keeps. **No program needs migration.** The corpus already converged on this convention de facto.

---

## 3. The design

### 3.A — Require an explicit type on an un-inferable initializer

**Rule.** A `let`/`var` (local or module-scope) binding **must** carry a type annotation **iff** its
initializer is *un-inferable* — i.e. bottom-up type inference cannot produce a fully-known type. Otherwise
it is a **compile error** with a clear, fixit-shaped diagnostic. When the initializer *is* inferable
(non-empty literal, constructor, any typed expression), the annotation stays **optional** and inference is
unchanged.

*Un-inferable* is defined structurally as "the inferred type is, or transitively contains, the unknown
(empty-name) type": the empty list literal `[]` (→ `List<unknown>`), a bare `null` (→ nullable-unknown), a
bare lambda whose parameter types are unknown and which has no contextual target, and — should a `{}` map
or set literal ever be added — the empty forms of those. This is the general "if it can't be inferred,
annotate" principle (your steer), not an `[]`/`null` special-case.

**Diagnostic (example):**
```
error: cannot infer the type of 'd' from its initializer — an empty list conveys no element type.
       add an explicit type annotation, e.g.  var d: List<i32> = []
  --> chess_solver.pg:12:7
```

**Consequence for the backends — the guess disappears.** Because the source now guarantees a concrete
declared type wherever the initializer is un-inferable, every backend is *always* handed the element type.
The emitter therefore emits the **declared type on the declaration** for these cases (idiomatic
`let d: number[] = []` / `int[] d = …`), and the `type.nameEmpty → object|unknown` fallbacks
(`plugins/*/…`) become unreachable for locals (kept only as defensive belt-and-braces). This is the clean
re-statement of what issue #27's reverted `[] as $T` cast did — but via the author's own annotation, not a
synthesized `as`-cast.

**Design-principle note (define errors out of existence, `CLAUDE_steve.md`).** This rule *adds* an error
rather than removing one — the opposite of the usual bias. It is justified because an un-inferable
initializer has **no single correct answer**: the language was previously papering over it with a
target-specific fallback (`object` vs `unknown` vs evolving-`any`), which is exactly the silent divergence
§3.B exists to forbid. A fail-fast diagnostic with an obvious fix is the honest outcome; it is also what
Swift and Go do (see §7).

### 3.B — `T[]` array type alongside `List<T>` (recommended)

Per your insight that a JS/TS array already carries `push`/`pop`/`splice` (≈ C# `List` operations), a
Polyglot **array** and **list** should both erase to a **JS array** on the TypeScript target, while C#
keeps the two distinct. Recommendation: introduce a **fixed-size array** type `T[]` as a first-class
spelling, distinct from the growable `List<T>`.

| Source | C# | TypeScript | Python | PHP | Element ops in source |
|---|---|---|---|---|---|
| `T[]` (array) | `T[]` | `T[]` (JS array) | `list` | `array` | index get/set, `.length`/`.count`; **no** size mutation |
| `List<T>` (list) | `List<T>` | `T[]` (JS array) | `list` | `array` | index + `.add`/`.removeAt`/… (growable) |

- **Semantic contract.** `T[]` is a **fixed-size sequence** (element read/write + length); `List<T>` is
  **growable** (the existing std type + its FFI bindings). The distinction is enforced by the Polyglot type
  checker at author time. On the dynamically-typed / JS targets both share one runtime representation (a JS
  array / Python list / PHP array); the fixed-size guarantee is a *compile-time* property, which is fine —
  it is the same discipline C# arrays give.
- **Literals & coercion.** A list literal `[a, b, c]` continues to build a `List<T>` by default (SPEC
  §4.1, unchanged). An array is produced by annotating the target — `var a: i32[] = [1, 2, 3]` coerces the
  literal to a fixed array — or by an explicit array construction. This keeps one literal syntax and lets
  the *declared type* pick array-vs-list (consistent with §3.A, where the annotation is already the
  disambiguator).
- **Grammar.** Add a postfix `[]` to the type grammar: `union_free_type = (named_type | tuple_type |
  function_type) , { "[" , "]" }` (or a dedicated `array_type = union_free_type , "[" , "]"`), composing
  *before* the trailing `?`. This is the only new surface.
- **Scheduling.** §3.B is **independently schedulable** — it is an additive ergonomic spelling, not
  required by §3.A or §3.C. If you want to ship the inference rule + the union invariant first and defer
  the array type, the plan slices accordingly.

### 3.C — Collections compose with union element types (invariant)

**Invariant.** *When a target spells a collection with a **postfix** operator (`T[]`), an element type
whose spelling contains a top-level type-union operator MUST be delimited (parenthesized) before the
postfix is applied. Angle-bracket / subscript constructors (`List<…>`, `Array<…>`, `list[…]`) are
self-delimiting and need no wrapping.*

- **TypeScript** is the only current postfix target. Its nullable union renders `(base | null)` and
  composes with `$0[]` to `(Node | null)[]` — **fixed in this branch** (`plugins/typescript/…` nullable
  arm). Named discriminated unions are single identifiers (`Shape[]`) and carry no obligation.
- **Critically, this invariant must extend to the new `T[]` array type** (§3.B), whose TS spelling is
  *also* postfix `$0[]`: `Node?[]` must render `(Node | null)[]`, not `Node | null[]`.
- **C#** is angle-bracketed (`List<Node?>`) and its nullable is a tight postfix `T?` — inherently safe; no
  parens emitted or needed. A future C# `T[]` array of a nullable is `Node?[]` (also safe — `?` binds
  tightly).
- **Python / PHP** are type-erased (no element spelling emitted); any future type hint must adopt its
  host's discipline (a bracketed subscript `list[…]` self-delimits and is safe).
- **Rejected alternative:** emitting TS `Array<T | null>` (angle-bracket, precedence-free) instead of
  `(T | null)[]`. It works, but postfix `T[]` is the more idiomatic TS spelling and the parenthesization
  rule is simple and local; mixing the two spellings is the real hazard. Commit to postfix + the parens
  invariant.

---

## 4. Scope-contract alignment (PRD §3)

- **§3.A Supported.** Type inference (SPEC §4.1) and collections stay in the supported core. This change
  *narrows* inference (requires an annotation where none could be correct) and *adds* the `T[]` spelling —
  no §3.A feature is removed or refused.
- **§3.B "clear diagnostic — never a miscompile."** This is the heart of it: the change moves the
  un-inferable-declaration failure from a *silent* backend guess (C# `object` / TS evolving-`any`) to a
  *parse/sema-time diagnostic*. A scope-honoring move — it makes §3.B land earlier and more honestly.
- **§3.C Faithful-by-default.** Nullability (`T?` → C# `T?` / TS `T | null`) is unchanged; the union-in-
  collection invariant makes the *faithful* rendering provably valid on every target.
- **§3.D determinism / §3.E capability negotiation.** Untouched.

## 5. Acceptance criteria

1. A `let`/`var`/module-`let` with an un-inferable initializer and **no** annotation is a **compile error**
   with a fixit-shaped message naming the binding and suggesting an annotation. Covers `[]`, `null`, and a
   contextless bare lambda.
2. The same binding **with** an annotation compiles, and the emitted code carries the declared element type
   on **every** target — verified by a strict `tsc` (`--strict --noImplicitAny`) + `dotnet build` on the
   generated output, no `as`-cast synthesized.
3. An **inferable** initializer (`[1,2,3]`, `List<i32>()`, typed expr) still compiles with **no** annotation
   and infers the precise type (no regression) — the existing corpus stays byte-identical except where a
   program intentionally exercises the new rule.
4. `List<T?>` and (if §3.B ships) `T?[]` render as `(T | null)[]` on TS and `List<T?>` / `T?[]` on C#;
   `List<union>` is valid on every target. The `Node | null[]` mis-parenthesization can never recur
   (regression test).
5. (If §3.B ships) `T[]` and `List<T>` both emit a JS array on TS; C# emits `T[]` vs `List<T>`; the source
   type checker forbids size-mutation (`.add`/`removeAt`) on a `T[]`.
6. Differential C#/TS/Python conformance green; `scripts/build-and-test.ps1` green; SPEC + grammar updated.

## 6. Out of scope

- **TypeScript-style anonymous structural unions `A | B` as a type spelling.** Not added here — the type
  grammar stays `named | tuple | function` + `?`. (Only nominal `union` ADTs + `T?` nullable exist.) If
  ever added, they inherit the §3.C invariant automatically.
- **Map / set literals** (`{}` / `[:]`). None exist today; when added, their empty forms fall under §3.A's
  general un-inferable rule for free.
- **Mixed-element list inference precision** (today `[1, "x"]` takes only the first element's type,
  `sema.cpp:985`). A separate latent imprecision; not addressed here.
- **Whole-program flow inference** (Rust/TS-style "infer from later use"). Out of scope — Polyglot infers
  bottom-up from the initializer only; the annotation is the escape hatch.

## 7. Precedent (why this is mainstream)

| Language | Empty-collection literal at a `var` | Requires annotation? |
|---|---|---|
| **Swift** | `var x = []` → *"empty collection literal requires an explicit type"* | **Yes** (hard error — nearly this exact rule) |
| **Go** | no untyped empty slice — `x := []int{}` or `var x []int` | **Yes** (type always present) |
| **Rust** | `let v = vec![]` → `E0282` unless later use fixes it | **Yes**, absent later-use inference |
| **Kotlin** | `emptyList()` infers useless `List<Nothing>`; idiom annotates | Effectively yes |
| **TypeScript (strict)** | `let x = []` → evolving `any[]`; `noImplicitAny` penalizes | *Permits it* — the cautionary case whose hazard we've been patching |

The rule puts Polyglot on the **Swift/Go** side of the line, consistent with its "faithful, never
miscompile" contract.

## 8. Versioning

Versioning is **lockstep + tag-driven** (PRD §4.16 / P24): committed sources are `0.0.0-dev` placeholders
(`kVersion` in `polyglot.cpp`, `version` in each `plugins/*/package.json`), and the real version is injected
at build from the release tag. So this change bumps **no** version constant in the tree. It is a language
feature (new `T[]` spelling + a tightened inference rule), so the **PR carries the `release:minor` label**
(overriding the default patch bump); the merge→tag automation then cuts one lockstep minor across CLI +
NuGet + extension + all four plugins. SPEC.md + grammar.ebnf are revised in-tree. No compat shim — clean
cutover, per the repo's no-backward-compatibility stance.

---

*Implementation plan: see [PLAN.md](./PLAN.md).*
