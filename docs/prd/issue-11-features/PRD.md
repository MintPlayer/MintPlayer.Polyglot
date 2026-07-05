# PRD — Issue #11: transcendental std.math, real module linking, faithful `i32()` TS cast

> **Status:** Draft · 2026-07-05 · targets release **0.3.0**
> **Source:** [GitHub issue #11](https://github.com/MintPlayer/MintPlayer.Polyglot/issues/11) — feature gaps
> found while scoping MintPlayer.AI's M33 client-side-AI ports (Snake + MountainCar) on the
> `MintPlayer.Polyglot.MSBuild 0.2.0` NuGet.
> **Companion:** `PLAN.md` (byte-gated slice plan).

## 1. Problem statement

Three gaps block single-sourcing real physics/ML programs to C#/TS/Python. In priority order:

### 1.A — `std.math` has no transcendental functions (primary)
`std.math` exposes only `PI`, `E`, `sqrt`, `ln`, `floor`, `ceil`, `min`, `max`, `abs`, `round`. **`cos`, `sin`,
`tan`, `exp`, `log`, `pow`, `tanh` are all absent** — `Math.cos(x)` fails with *"type 'Math' has no static
method 'cos'"*. This blocks a whole class of programs: MountainCar's env dynamics use `cos(3·position)` and its
PPO policy net uses `tanh` activations — neither can even be *written* in a `.pg` today.

### 1.B — module imports **inline** imported symbols → duplicate definitions (the big one)
User `.pg`→`.pg` imports parse and type-check, but the emitter **inlines a full copy** of the imported
declarations into the importer's output rather than emitting a cross-module reference. `main.pg` importing
`PgThing`/`twice` from `./nn` produces a `main.cs` that *re-defines* `class PgThing` + `twice`. When the
`MintPlayer.Polyglot.MSBuild` package globs **every** `**/*.pg`, transpiles each, and compiles all generated
`.cs` **into one assembly**, `nn.cs` (standalone) and `main.cs` (inlined copy) both define `PgThing` →
**CS0101 duplicate type**. A shared library `.pg` (a reusable net-forward imported by several games) is
therefore unusable without hand-excluding it from the glob.

### 1.C — `i32(x)` TS codegen uses `(… | 0)` (32-bit wrap), diverging from C# `(int)` (minor)
Casting a float to `i32` emits TS `(Math.trunc(x) | 0)`; the `| 0` forces a 32-bit wrap, so for `|x| ≥ 2³¹`
the TS result wraps while C# `(int)x` does not. In-range it's fine; out-of-range it's a silent target
divergence. The **issue-9 PRD already documented the intended TS emission as plain `Math.trunc(x)`** —
so this is a rule/spec divergence, not a design change.

## 2. Root causes (from the 2026-07-05 three-agent investigation)

### 2.A — math: skeleton + overlays, no C++ logic
`std.math` is a two-part mechanism: an embedded `.pg` **skeleton** (`compiler.cpp` `STD_MATH`,
`src/MintPlayer.Polyglot.Core/src/compiler.cpp:68-96`) declaring shapes/signatures with empty bodies, plus
per-target **`std` overlay blocks** in each plugin manifest (`plugins/<t>/polyglot-plugin.json`, the
`"std.math"` sub-block) supplying each member's emission template. `injectStdOverlays`
(`compiler.cpp:378-443`, run after sema, before the capability gate) attaches the active target's templates.
Adding a function = add a `static fn` to the skeleton + one overlay row per target. **No emitter/C++ logic
changes.** PHP currently has *no* `std.math` block at all (net-new sub-block; today any `Math.*` on PHP
refuses at the capability gate).

The requested functions are all plain bound statics like `sqrt` (no generics, no `$T` cast): C#
`global::System.Math.Cos($0)`, TS `Math.cos($0)`, Python `__import__('math').cos($0)`, PHP `cos($0)`; `pow`
takes two args (`$0, $1`).

### 2.B — module inlining: `mergeDecls` deep-copies into one flat unit (deliberate single-file design)
`mergeDecls` (`compiler.cpp:189-198`) appends **every** imported module's top-level declarations into the
importer's single `CompilationUnit`, which lowers to one flat `ir::Module` (`ir.hpp:436`, no module identity
/ no per-decl origin) and emits as one file. The comment is explicit: *"Output stays single-file per target:
everything lands in one unit."* This was a **deliberate** design (PRD §4.5, `POLYGLOT_PRD.md:303-307`) — it
avoided a conformance-harness redesign — but it is exactly what produces the duplicate. No backend emits any
cross-`.pg` `import`/`using`; the only import emission that exists is for runtime/std needs. Cross-module
*reference emission* was explicitly deferred ("P12 phase-2, needs a per-file import-scope table").

### 2.C — i32() TS wrap: float→int branch routes through the shared `wrapInt` table
The TS `Cast` rule's `fromIsFloat` branch (`plugins/typescript/polyglot-plugin.json:685-709`) wraps
`Math.trunc(operand)` through the shared `wrap` fn, whose `i32` template is `($x | 0)` (`wrapInt` table,
lines 117-126). That table is the **legitimate** int→int narrowing / arithmetic-overflow machinery (unary
negation, binary overflow, the Cast *else* branch) — it must not be disturbed. C# float→int is a plain
truncate (out-of-range **unspecified**, *not* modular — ECMA explicit numeric conversions); Python already
emits plain `int(x)`; PHP's float→int branch (`plugins/php/…:546-580`) also wraps and is likewise wrong.
The safe fix touches **only the float→int cast branch** of TS and PHP.

## 3. Design

### 3.A — math tier: a documented **best-effort (tolerance) tier**
Per PRD §3.D (the determinism-honesty clause, `POLYGLOT_PRD.md:115-121`), only `+ − × ÷ √` are bit-exact
cross-target; **transcendentals are explicitly named as the non-reproducible class**. Adding them does **not**
break the scope contract — it *exercises* §3.D. We ship them as an explicitly documented **best-effort tier**:
"available on all targets, but results may differ by ≤1 ULP across runtimes — code that needs cross-target
identity must use `+ − × ÷ √` (or a future fixed-point std type), not transcendentals." The skeleton comment
(which already flags `ln`/`round` as not-bit-exact) is extended to the new members, and the PRD §3.D / PLAN
relaxation list gets a one-line entry.

**Function set — comprehensive, not just the seven requested.** The tier covers the common surface *both*
.NET `Math` and JS `Math` expose (so every function maps 1:1 with no emulation), which is also what Python
`math` and PHP provide:

| Group | Functions (new) |
|---|---|
| Trig | `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2(y,x)` |
| Hyperbolic | `sinh`, `cosh`, `tanh` |
| Exp / log | `exp`, `log` (natural), `log2`, `log10`, `pow(x,y)`, `cbrt` |
| Rounding / sign | `trunc`, `sign` |

Notes on the set:
- `log` = **natural** log (matches JS `Math.log`, C# `Math.Log(x)`, Python `math.log(x)`, PHP `log($0)`).
  The existing skeleton already has `ln`; `log` is added as the conventional name and `ln` is kept as an
  alias (both emit the same target call). `log2`/`log10` map to the targets' dedicated functions.
- `sign` returns `f64` (`-1.0/0.0/1.0`) to stay a plain bound static across targets — C# `Math.Sign` returns
  `int` and only has integer/decimal/double overloads returning `int`, JS `Math.sign` returns a number,
  Python has no `math.sign`. To keep it a **uniform, no-emulation** binding, `sign(x: f64): f64` emits:
  C# `global::System.Math.Sign($0)` (widens to f64 via the return type), JS `Math.sign($0)`, Python
  `__import__('math').copysign(1.0, $0)` is *not* correct for 0 — instead Python uses a small inline
  `(0.0 if $0==0.0 else __import__('math').copysign(1.0,$0))`; PHP `($0 <=> 0)`. *(Decision recorded in PLAN:
  if `sign`'s per-target arms can't stay clean 1:1 bindings, it is dropped from v1 rather than emulated —
  the primary ask is trig/exp/log/tanh.)*
- `trunc` maps to C# `Math.Truncate`, JS `Math.trunc`, Python `math.trunc` (returns int → wrap `float(...)`),
  PHP `(float)(int)$0` — a plain bound static.
- **Constants:** `PI`, `E` already exist. No `TAU` (JS `Math` has no `Math.TAU`; adding it would force
  emulation — out of scope for a 1:1 tier). Authors write `2.0 * Math.PI`.

`min`/`max`/`abs`/`round`/`floor`/`ceil`/`sqrt`/`ln` are unchanged.

### 3.B — module linking: **merge-for-sema, split-for-emit** (chosen: proper linking)
The user chose proper module linking over the smaller MSBuild-only library-module workaround: *"Pick the best
option… as long as the code is correct and resilient."* We keep semantic analysis working on the merged unit
(so **cross-module resolution, collision detection, match-exhaustiveness, and capability gating are entirely
unchanged** — the delicate resolver is not rewritten), and change only what happens **after** sema: emission
splits back out by declaration origin, and cross-module references emit as target-native imports instead of
being inlined.

**The model:**
1. **Origin tagging.** Each top-level declaration carries an `originModule` (a stable module id). The entry
   file's own decls are the entry origin; `mergeDecls` stamps merged user-module decls with their source
   module's canonical id; `linkCoreModule`/`linkLibModules` stamp std/core/lib decls with a **`prelude`**
   sentinel. (This is the "per-file import-scope table" the deferred P12-phase-2 work always needed.)
2. **Split for emit (partition by name→origin).** `compile()` runs the front-end once over the whole closure
   and lowers the whole merged unit once (the `Lowerer` needs the full type universe — it builds cross-module
   lookup tables — so per-module lowering in isolation is *not* safe). It then **partitions the one lowered
   `ir::Module` by a name→origin map** (built from the tagged AST; no per-decl IR field needed) into one
   emitted file per user module, and emits each. `EmitResult` gains `std::vector<ModuleFile> modules`
   (`{ basename, code }`); `code` stays the **entry** module's source for back-compat.
   **Single-module programs (no user imports) emit byte-identically to today** — the whole existing corpus is
   unaffected; only the one multi-module program (`modular`) and the new `library` program change. Linked
   emission (imports / `partial`) is switched on **only when the closure has >1 module**.
3. **Prelude is a real problem, handled per target.** `print` is *not* a call-site binding — it is an
   `expect/actual` function emitted as a **top-level `PolyglotProgram.print<T>` method**, and every user free
   function likewise lives inside `PolyglotProgram`. So a naive split duplicates the **whole wrapper**, not
   just user types (verified: both files emit `class PgThing` **and** `static class PolyglotProgram { twice;
   print }`). The prelude (core/lib/std-origin) decls therefore need a target-appropriate home:
   - **C#** (one assembly): `PolyglotProgram`/`PolyglotExtensions` become **`partial`** (only when linked), so
     every module's file contributes to the one merged class; **prelude decls are emitted into the entry file
     only** and are visible assembly-wide. Types emit into the **global namespace** (verified — only the
     wrapper is a class), so cross-file type references resolve with **no `using`**. Requires exactly one
     entry (the module with `main`) per assembly — a documented v1 boundary (a pure-library project with no
     `main` that calls `print` from a type method is out of scope).
   - **TS / Python / PHP** (each file is its own module scope — no cross-file collision): prelude decls are
     **inlined into every module file**. A `print` defined+exported in both `main.ts` and `nn.ts` is harmless
     (distinct module scopes); tsc under `isolatedModules` accepts it. No prelude import is needed.
4. **Import list is the source's own `import` decls.** A module's cross-module imports are exactly its parsed
   `import { a, b as c } from "spec"` statements, with `std.*`/lib specifiers filtered out (prelude). No IR
   reference-scan. `ir::Module` gains `imports: [{ path, names:[{name, alias}] }]` (+ a `linked` flag), where
   `path` is the imported module's **basename** (its canonical path's last component, sans `.pg`). **v1 output
   is flat** — every module emits to `<basename>.<ext>` and imports reference `./<basename>`; this keeps Core
   path-free (pure string op on canonical paths, no `<filesystem>`). **v1 requires unique basenames** across
   the closure (documented); subdir-mirroring + relative-path specifiers are a follow-up.

**Per-target import emission** (Program-scaffold rule data + one engine primitive to iterate `module.imports`):
- **C#** — no import statement (global-namespace types + `partial` wrappers, one assembly).
- **TypeScript** — `import { a, b as c } from "./nn";` at file top (0.1.4 already `export`s every top-level decl).
  The emitted-path spelling for the harness's `node <file>.ts` ESM run must resolve — pinned in PLAN 4b.
- **Python** — `from nn import a, b as c` (module = file stem; siblings importable when run from the dir).
- **PHP** — `require_once __DIR__ . '/nn.php';` (process-global once required).

**CLI / build-system contract.** `compile()` (one root) returns that root's whole closure as `modules`; the CLI
writes them all. Given **several** input files (MSBuild globs all `**/*.pg`), the CLI first builds the input
set's import graph, compiles only the **roots** (files not imported by another input) and **dedups** shared
modules across roots by basename (identical content; a content conflict is an error) — so an imported-only
library `.pg` is emitted **as part of its importer's closure**, never also as its own entry. This is what
removes the double-emit: `main.pg` → `{main.cs, nn.cs}` and `nn.pg` (imported) is *not* compiled separately.
The `MintPlayer.Polyglot.MSBuild` target invokes the CLI **once** over all `@(PolyglotFile)` and adds the
deduped generated set to `@(Compile)` — each type/function defined once, CS0101 gone, **no** hand-exclusion.

**Conformance-harness rework.** `run-diff.ps1` / `run-python.ps1` currently emit one file for a multi-file
program and compile/run only that. They are reworked to emit the whole module set and compile/run it together
(compile all `.cs` into one assembly; run the entry `.ts`/`.py` with siblings importable). Stdout must stay
byte-identical to today — the `modular` conformance program (transitive `geom.vec` + `./scale` imports) is the
primary regression witness, plus a new `library` two-file program that reproduces the exact issue-#11 shape.

**Why this is resilient, not a workaround.** Sema is untouched (no new cross-module symbol-table code path to
get subtly wrong). The emit split is a pure post-sema partition by a tag that resolution already implies. The
import list is the source's own declarations (no heuristic reference-scan). Every slice is byte-gated against
the current output for single-module programs (which must not change at all) and stdout-gated for multi-module
programs. Duplicate/again-defined types remain a **compile-time collision error** (never a miscompile), per
the §3.B law.

### 3.C — i32() TS/PHP cast: plain truncation in the float→int branch
Change **only** the `fromIsFloat` branch of the TS `Cast` rule (and PHP's) to emit plain truncation —
TS `Math.trunc($operand)`, PHP `(int)($operand)` — instead of routing through `wrap`. The `wrapInt` table and
the int→int narrowing (Cast *else*) branch, unary, and arithmetic-overflow sites are **untouched**, so genuine
32-bit masking of oversized *integer* values keeps wrapping. This matches C# in-range across the full range;
out-of-range is unspecified in C# (§3.D territory), so no target owes a specific value. Python is already
correct. The branch is width-generic (i8/i16/u8/u16/u32/i32) — for float→int this is *correct*, because C#'s
float→integral conversion is truncate-toward-zero for all widths (not modular), so plain truncation matches C#
for every width in-range.

## 4. Scope guard (PRD §3 check)
- **§3.A supported:** casts (1.C) and module imports (1.B) are already in-surface; this makes their *emission*
  faithful. No new surface.
- **§3.B refuse-not-miscompile:** duplicate types stay a collision error; a `Math.*` a target can't bind still
  refuses at the capability gate. Nothing new is silently miscompiled.
- **§3.C faithful-by-default:** 1.C removes a silent int32-wrap divergence.
- **§3.D determinism honesty:** transcendentals are shipped as an explicitly documented best-effort tier;
  the relaxation list gains one line. No promise of bit-exact transcendentals.
- **Prime directive (hold the scope line):** the math set is the 1:1 both-targets-expose surface (no emulation,
  no `TAU`); module linking implements a *deferred-but-planned* capability (P12 phase-2), not new scope.

## 5. Acceptance criteria
1. `import { Math } from "std.math"` then `Math.cos/sin/tan/asin/acos/atan/atan2/sinh/cosh/tanh/exp/log/log2/
   log10/pow/cbrt/trunc(/sign)` type-check and emit on **all four** targets (PHP gains a `std.math` block).
2. A MountainCar-shaped `.pg` using `cos(3·x)` and `tanh` transpiles to C#/TS/Python; a **tolerance-gated**
   conformance program (prints values quantized so equality holds despite ≤1-ULP drift) is green.
3. The issue-#11 two-file repro (`main.pg` importing `PgThing`/`twice` from `./nn`) transpiles so that the
   generated `main.*` **references** the symbols and does **not** re-define them; building both files and
   compiling into one assembly (C#) / importing across files (TS/Python) succeeds — **no CS0101**.
4. The `modular` conformance program still produces byte-identical stdout on C#/TS/Python, now via multiple
   emitted files compiled/run together.
5. Every **single-module** program in the corpus emits **byte-identically** to 0.2.0 (the byte gate).
6. `i32(x)` on a float emits TS `Math.trunc(x)` (no `| 0`) and PHP `(int)(x)` (no mask); a full-range cast
   conformance program agrees C#/TS/Python; int→int narrowing wraps are unchanged (byte-gated).
7. `scripts/build-and-test.ps1` is green end-to-end (build + unit + run-diff + run-python + run-emit +
   library + nullable + watch + msbuild gates).

## 6. Versioning
- **CLI / Core (`kVersion`) → 0.3.0**, **NuGet `MintPlayer.Polyglot.MSBuild` → 0.3.0** (module linking is a
  substantial additive feature; multi-file emit is new behavior).
- **Plugins** (each gains features → minor bump to a coherent line):
  - `@mintplayer/polyglot-target-csharp` 0.2.2 → **0.3.0** (std.math block, global-namespace linking = no change / verify).
  - `@mintplayer/polyglot-target-typescript` 0.2.0 → **0.3.0** (std.math, import emission, i32 cast fix).
  - `@mintplayer/polyglot-target-python` 0.2.0 → **0.3.0** (std.math, import emission).
  - `@mintplayer/polyglot-target-php` 0.2.0 → **0.3.0** (std.math block net-new, import emission, i32 cast fix).
- Publishing rides the existing `publish-plugins.yml` (push to master → npm + GitHub Packages) and
  `release.yml` (`v0.3.0` tag → CLI RIDs + GitHub Release + NuGet). No workflow changes expected.
