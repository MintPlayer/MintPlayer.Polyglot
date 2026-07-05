# Issue #9 — v0.1.4 codegen bugs (PRD)

> **GitHub:** [MintPlayer/MintPlayer.Polyglot#9](https://github.com/MintPlayer/MintPlayer.Polyglot/issues/9)
> — *"v0.1.4 codegen bugs: primitive casts `i32()`/`f64()`, null-initialized typed locals, and 2+
> nested-generic params"* · labels `bug`, `codegen`.

- **Status:** Draft v1.0 · 2026-07-05 · ships as **release 0.2.0** (three codegen fixes + a systemic
  gate-hardening warrant a minor bump, not a patch).
- **Author:** Pieterjan (with Claude Code).
- **Provenance:** a 4-agent read-only investigation of the three reported bugs + the conformance-gate
  gap (one agent each; root causes below are their findings, verified against the source).
- **Reporter context:** found live while porting the MintPlayer.AI **FruitCake inference path**
  (observation + dueling-Q forward pass) into `fruitcake_solver.pg` on
  `MintPlayer.Polyglot.MSBuild 0.1.4`. All three have `.pg`-side workarounds already applied in the pilot,
  so the reporter is **unblocked** — this is a correctness/robustness hotfix, not a blocker. Consuming-repo
  handoff: `C:\Repos\MintPlayer.AI\docs\prd\polyglot-pilot\POLYGLOT_BUG_HANDOFF_M32.md`.

---

## 1. Problem

Three constructs **type-check cleanly** (`polyglot check` reports no problems) but emit code that either
does not compile (C#) or crashes at runtime (TS). Each is a **§3.B-class silent miscompile** — the exact
failure mode the scope contract exists to prevent (*"a clear diagnostic — never a miscompile"*). Two of
the three lie inside the **§3.A supported surface** (explicit casts, generics), so the answer is **fix the
emission**, not refuse the feature.

A fourth, systemic finding: **the crown-jewel differential gate (`run-diff.ps1`) is structurally blind to
all three.** It builds the generated C# and runs the generated TS, then *discards both outcomes* and
compares only stdout — so when both targets fail identically (empty stdout == empty stdout) it **false-
passes**. This is the same lesson as the `??`-precedence (0.1.1) and TS-export (0.1.4) hotfixes: a
stdout-only gate cannot see a codegen bug that breaks both targets the same way.

## 2. The three bugs (root causes)

### Bug 1 — primitive cast `T(expr)` emits `new T(expr)` (invalid in both targets)

```
fn toInt(x: f64): i32 { return i32(x) }   // emits: return new i32(x);   (C# CS0815 / TS runtime crash)
fn toF64(n: i32): f64 { return f64(n) }   // emits: return new f64(n);
```

**Root cause:** the numeric scalars (`i8…u64`, `f32`, `f64`) are declared as `extern class` in the
always-linked core prelude (`compiler.cpp:151-169`) so `i32.parse(s)` resolves. Side effect: `i32`/`f64`
register as ordinary types, so a call `i32(x)` is misclassified as **construction** — in sema (`checkCall`
hits `types_.find("i32")`, `sema.cpp:1280`) and in lowering (`typeNames_.count("i32")` →
`ir::New`, `lower.cpp:487-495`). With no ctor, arg-count is never checked either, so `i32()`/`i32(a,b)`
also pass silently.

**Expected:** identical to the already-correct `(T)x` cast — `i32(x)` ≡ `(i32)x`, `f64(n)` ≡ `(f64)n`.
C# `(int)x` / `(double)n`; TS `Math.trunc(x)` (**not** `x | 0`) / identity `x`; Python `int(x)` /
`float(x)`. The `Cast` JSON rules already emit exactly this in all four backends.

### Bug 2 — `var x: T? = null` emits `var x = null` in C# (CS0815)

```
fn pick(): Box? { var best: Box? = null; return best }   // emits: var best = null;   (CS0815)
```

**Root cause:** the shared engine's `Let` statement (`emitter_base.cpp:1393-1396`) spells every local via
the single-hole `localDecl` table, and C#'s row is `{"mutable":"var $x","const":"var $x"}`
(`plugins/csharp/…:46-49`) — the declared type is always discarded. Fine everywhere a `var` can infer;
CS0815 when the initializer is the bare `null` literal (no inferable type). TS/Python/PHP are untyped
locals → unaffected. The IR **does** carry the declared type (`ir::Let.type`, nullability preserved post-
0.1.3), and null-init is detectable (`init->kind == ir::ExprKind::Null`).

**Expected (C#):** emit the declared type when the initializer is `null` — `Box? best = null;`.

### Bug 3 — 2+ nested-generic (`List<List<f64>>`) params in one signature fail to parse

```
init(a: List<List<f64>>, b: List<List<f64>>) { … }   // error: expected ')' at `b`
init(a: List<List<f64>>) { … }                        // OK — a single one parses
```

**Root cause (not the reporter's `>>`-state hypothesis):** the `>>` lexing + `pendingAngles_` borrow
mechanism is correct and leaves no dirty state. The bug is in the generic-arg loop of `parseTypeCore`
(`parser.cpp:471-474`): `do { args.push_back(parseType()); } while (accept(Comma))` tests for the comma
**before honoring the pending outer-generic close**. After the inner `List<f64>` consumes `>>` and lends
one `>` to the outer close (`pendingAngles_ = 1`), the next token is the *parameter-list* comma — which the
loop wrongly accepts, parsing `b` as a third type-arg of the outer `List`. A single param works because
`)` follows instead of `,`; fields work because they are `;`/newline-separated, never comma.

**Expected:** two or more nested-generic params parse identically to one.

## 3. Scope-contract alignment (PRD §3)

- **Bug 1** — explicit casts are **§3.A supported**; the `(T)x` form already emits correctly. Fix, don't
  refuse. Bonus: the fix turns the currently-silent `i32()`/`i32(a,b)`/`i32(true)` into diagnostics.
- **Bug 2** — nullable annotations are **§3.C faithful-by-default** (sibling to the 0.1.3 nullable-
  reference fix). Dropping the type is an infidelity, not a refusal.
- **Bug 3** — generics are **§3.A supported**; a parser bug, purely mechanical.

None of the three touches the §3.B refusal set; all are corrections inside the supported surface.

## 4. Acceptance criteria

1. `i32(x)`/`f64(n)` (and every `T(expr)` where `T` is numeric) emit the **same** code as `(T)x` on all
   four targets; `casts_call.pg` prints identical stdout across C#/TS/Python and its C# compiles + TS runs.
2. Zero-arg / multi-arg / non-numeric-source primitive "casts" (`i32()`, `i32(a,b)`, `i32(true)`) now
   produce a **diagnostic**, not silent construction.
3. `var x: T? = null` emits `T? x = null;` in C# (value **and** reference `T`); TS/Python/PHP output
   byte-unchanged; `null_local.pg` compiles clean under the nullable gate.
4. Two or more nested-generic params in one signature parse; `nested_generics.pg` transpiles and runs.
5. **Gate hardening:** `run-diff.ps1` and `run-python.ps1` **fail** the run if the generated C# build
   returns non-zero (or the `.dll` is missing) or if `node`/`python`/`dotnet <dll>` exits non-zero — so a
   symmetric double-failure can no longer false-pass. The three new regression programs would each go
   **red before** their fix and **green after**.
6. Byte-gate discipline: every emitted-file diff across the existing 41-program corpus is limited to the
   lines the fix is expected to change (Bug 2 touches only null-initialized annotated C# locals; Bugs 1 & 3
   add new programs and change nothing existing).
7. Full `scripts/build-and-test.ps1` green.

## 5. Out of scope (flagged, not fixed)

- Other `var`-can't-infer C# cases beyond `null` (bare-lambda locals; empty `[]` already emits
  `new List<T>()` correctly). The `localDeclTyped` seam generalizes later if wanted.
- `string(x)`/`bool(x)`/`char(x)` call-casts — those are **not** extern classes, so they already error
  ("undeclared function") rather than miscompiling; a separate non-numeric conversion story, untouched.
- The PHP runtime differential (no php toolchain here) — PHP output for the new programs is inspected, not
  executed, consistent with the rest of the corpus.

## 6. Versioning

- **Core / CLI** (`polyglot.hpp kVersion`, `--version`): `0.1.4` → **`0.2.0`** (Bugs 1 & 3 + Bug 2's Core
  `Let`-case change + gate hardening).
- **NuGet** (`MintPlayer.Polyglot.MSBuild.csproj <Version>`): `0.1.4` → **`0.2.0`**.
- **Plugins** — only Bug 2 touches plugin data, and only C# needs it (the new `localDeclTyped` row).
  TS/Python/PHP get `""` back from `specSubstTX` and fall through to the unchanged untyped `localDecl`, so
  their output is byte-identical and they do **not** bump: **csharp 0.2.1 → 0.2.2** only. (Bugs 1 & 3 are
  Core-only, no plugin change.)

See [PLAN.md](./PLAN.md) for the sliced implementation.
