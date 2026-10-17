# P40 — Library-grade output for a .NET library + Angular entry point (issue #73) — PRD

> **Status:** ✅ built (2026-10-17), slices 1–13; the end gate (slice 14) is pending. **The maintainer accepted
> all of D1–D17 on #73**, with changes to D3, D5, D8, D10 and D17. Three follow-up investigations folded those
> changes in below, adding D18–D20.
> **Issue:** [#73](https://github.com/MintPlayer/MintPlayer.Polyglot/issues/73).
> **Evidence:** `ANALYSIS.md` (6-agent investigation, verified against HEAD `a603dba`).
> **Plan, spikes and decision log:** `PLAN.md`.
> **Delivery:** one PR, one minor release under the lockstep scheme.

## 1. Problem

MintPlayer.Spark evaluated Polyglot for its GS1 core: element-string and Digital Link parsing, AI linters and
check digits. The same algorithm must run in a .NET 11 server library and in the Angular secondary entry point
`@mintplayer/ng-spark/scanning`, and both are tested against GS1's Syntax Engine vectors. This is the PRD's
stated sweet spot (`POLYGLOT_PRD.md:27-30`).

A probe on v0.10.1 found the **logic** faithful: check digits, element-string parse and a code-point fold
matched in every culture. Two classes of problem remain:
- **Correctness defects that the differential suite cannot see.** Either both targets agree on the wrong
  answer (A1), or the divergence only appears in **library** mode under a host culture (A3), or it fails only
  at runtime while the build exits 0 (A2, A4, B4's blocker).
- **Packaging gaps.** Two Polyglot libraries cannot coexist in one .NET app (B1) or one TS barrel (B2). There
  is no drift check for committed output (B5), and MSBuild writes TS twins into another project behind a
  build cache's back (B6).

The investigation widened nearly every item from "this case" to "this class of defect" (`ANALYSIS.md`
headline table). This PRD designs for the class.

## 2. Goals and non-goals

**Goals:**
- G1. **No silent output.** Every construct the checker accepts either lowers or is refused with a diagnostic.
  The `__polyglot_unlowered_expr__` poison becomes an internal-compiler-error backstop that fails the build.
- G2. **Library output is culture-independent.** No C# binding consults `CurrentCulture`, and `"${x}"` and
  `print(x)` are byte-identical on all four targets for integers and f64.
- G3. **Null is explicit.** `x!` throws catchably on every target, and a nullable operand cannot silently
  stringify or do arithmetic.
- G4. **Libraries compose.** Several Polyglot libraries coexist in one C# app, one PHP app and one TS barrel,
  and only referenced runtime helpers are emitted.
- G5. **A std that library code can use**: ordinal strings with identical edge semantics, and an
  insertion-ordered `Map<K,V>`.
- G6. **CI-friendly tooling**: relative inputs find their config, `build --check` detects drift, MSBuild can be
  limited to a subset of targets, and committed origin-info output is checkout-independent.

**Non-goals** (the issue's "out of scope" list, which stands):
- A DTO / API-contract mode.
- `decimal` or fixed-point (`POLYGLOT_PRD.md:123`).
- Unicode normalization.

**Also not in this PRD:**
- `super.m()` method calls. They are refused here; implementing them is a separate language feature (D6).
- `Map` iteration with `for k in m`. `keys()` is a snapshot.
- netstandard2.0, .NET Framework, Mono or Unity compilability of the C# output (see §6 floors). Reaching it
  would mean removing records from the prelude and rebinding several std functions. It is genuinely not being
  done, not deferred to avoid diff size.

## 3. Scope contract check (PRD §3)

| Item | §3 bucket | Notes |
|---|---|---|
| Escapes, ranges, `!`, nullable operands | §3.A, made faithful | Turns silent miscompiles into diagnostics (§3 "never a miscompile") |
| Integer and f64 stringification | §3.A faithful | §3.D already guarantees identical bits for `+ − × ÷ √`, so identical *text* follows |
| `toUpperAscii`/`toLowerAscii` | faithful | Identical on every target |
| `toUpper`/`toLower` | **published best-effort tier** (§3.C list) | Three behaviours: JS/Python full Unicode, C# invariant simple mapping, PHP ASCII-only |
| f32 to string | faithful (exact value widened to f64, S13) | f32 *arithmetic* stays §3.C, because TS, Python and PHP hold f32 as a double |
| String index units | **published caveat** (or faithful, if SP-S1 says so) | C#/TS UTF-16 units, Python code points, PHP bytes |
| `compareOrdinal` | faithful on BMP; caveat for astral vs U+E000–FFFF | |
| `Map<K,V>` | §3.A std collection | Keys limited to `string` and integer scalars; refusals for the rest |

Nothing here touches §3.B. No target-name comparison enters the Core: every distinction below is manifest
data (`core-language-agnostic` rule).

---

## 4. Design

### §0 — Shared foundation: runtime helpers, placement, reachability

Six items need the same three missing capabilities (`ANALYSIS.md` cross-cutting finding). They are built once,
first.

1. **`requires` on bindings.**
   - Today `{"fn":"require"}` exists only in the rule DSL (`emitter_base.cpp:465`).
   - A std binding gains an object form: `{ "template": "...", "requires": ["pgStr"] }`.
   - The loader validates that every required name is a declared prelude, as part of the anti-silent-drop
     contract.
2. **Placement is a `{"fn":"preludeSlot"}` marker in each `Program` rule**, required by the loader. *(Amended
   after S12: a line-based vocabulary fails on PHP. Right after `<?php` is fatal before a `namespace`
   statement, and end of file is fatal when top-level code calls the helper first.)* Each manifest puts the
   slot where its language needs it: after `namespace`/`use` on PHP, after the usings on C#, at the top on TS
   and Python.
3. **A reachability pass** over the lowered IR, run after lowering and before emission.
   - It computes the set of prelude declarations reachable from user code, plus the `requires` of every rule
     and binding that fired.
   - Every declaration exposes `decl.isPrelude`.
   - Only reachable prelude declarations are emitted.
   - **Shared C# prelude** (`sharedPreludeFile`): compile all roots first, then emit **one**
     `__polyglot_prelude.cs` from the **union** of what they reach, or no file at all if nothing is reached.
     This keeps the identical-file contract in `main.cpp:196-215`. The union covers **which std modules each
     root links** as well as reached names (S15).
   - **Two categories of helper, named explicitly** (S14/S15):
     - `.pg` prelude *declarations* (`Option`/`Some`/`None`, std io functions) are tracked structurally,
       pruned by reachability, and shared on C#.
     - *Manifest* helpers (pulled in through `requires`) are emitted per file. On C# they live in the
       file-local `__PgRt`.
   - **S14 measured** identical conformance results on all four targets, with emitted size down 21–38%. No
     manifest template names a prelude declaration without a declared reference. A conservative identifier
     scan of opaque `extern(...)` text stays as a backstop for third-party plugins.
   - **Per-file preludes** (TS, Python, PHP): each file gets only what it reaches.
4. **Visibility.**
   - Prelude helpers are never public API.
   - C# runtime helpers live in a **`file static class __PgRt`** (C# 11 file-local). It cannot collide across
     partial files or libraries.
   - Prelude *types* that appear in user signatures (`Option<T>`, `Some<T>`, `None<T>`) live in the namespace
     (§B1) and follow `access`.
   - TS prelude declarations are module-local and **not exported** (SP-S3 of B1: declaration emit is fine).
   - PHP helpers sit behind `function_exists(__NAMESPACE__.'\\name')`, namespace-qualified so a same-named
     global function cannot shadow them.

Recommended over the issue's sibling `__polyglot_prelude.ts` (**D10**). A sibling file needs generated imports
and cross-directory routing (`crossDirImports`), and it would export the helpers anyway. A non-exported
per-file copy of only what is reached costs a few lines per file and has no failure mode.

### A1 — Escapes

- **One `decodeEscape`** shared by string and char literals.
  - Accepted: `\n \t \r \0 \\ \" \' \$ \u{H…}`. The `\u{}` form takes 1–6 hex digits, must be ≤ U+10FFFF and
    must not be a surrogate. Values are encoded as UTF-8 internally.
  - **Every other escape is a lexer error at the backslash**, for example: *unknown escape sequence `\q` —
    valid escapes are \n \t \r \0 \\ \" \' \$ and \u{…}*.
  - `\uXXXX` and `\xHH` are **not** accepted: one spelling per thing, and C#'s variable-length `\x` is a known
    trap.
  - Invalid UTF-8 inside a literal is an error.
- **Char literals are validated:**
  - empty → error;
  - more than one character → error;
  - an astral code point → error (*a char is one UTF-16 code unit (SPEC §8) — use a string*).
- **Emission:** retire the shared `renderString` and the manifests' `escapes` maps. Each manifest gains a
  `literals` object with named contexts `string`, `interp` and `char`. Each context has:
  - a `map` of special characters;
  - a `codepoint` template (`\u$H4`, `\u{$H}`, `\x$H2`);
  - an `astral` template (`\U$H8`, or `"surrogates"`).
- **What gets escaped.** The Core decides only *which* characters, from a fixed language-agnostic set: C0
  controls, DEL, C1 controls, U+2028, U+2029 and U+FEFF. Printable non-ASCII passes through as UTF-8, so
  ASCII output is byte-identical to today.
- **PHP** maps `$` to `\$`, which fixes the `"cost $x"` miscompile.
- **NUL is never emitted as `\0`** (the octal trap).
- **The loader requires:**
  - a `string` context;
  - each context maps `\` and its own quote character;
  - each context has a `codepoint` template.
- **Siblings fixed in the same change:**
  - `polyglot fmt` becomes the exact inverse of the lexer.
  - The TextMate grammar gets the new escape set, an invalid-escape catch-all and a char pattern.
  - The CLI prints warnings with their real severity, **on successful builds too**, and once rather than once
    per target.
  - SPEC §2, §8 and §9 (`:529` rewritten with the true per-target counting).

### A2 — Ranges and the poison backstop

- **Precedence: Rust-style.**
  - `range_expr = coalesce_expr [ (".."|"..=") coalesce_expr ]` is the loosest binary form, and ranges do not
    chain.
  - `0..n - 1`, `-2..2`, `i + 1..n` and `0..a ?? 5` all mean what they read as.
  - A range can never be an operand, so binding looser only ever turns errors into valid programs.
- **Sema rules:**
  - A range outside a `for` iterable is an error: *a range ('a..b') is only valid as a for-loop iterable*.
  - Range bounds must be integers.
  - The two bound types widen to one, which becomes the loop variable's type; an incompatible pair is an error.
  - `super` other than a base-constructor `super(…)` is an error (**D6**).
- **Bounds are evaluated once**, before the first iteration, as in Rust, Kotlin, Swift and Python (**D7**).
  - A non-simple end bound is hoisted into a temporary.
  - An `IR` flag `rangeEndSimple`, like `With::baseIsSimple`, keeps `0..n` readable.
  - The C# spelling is the multi-declarator `for (int i = 0, __end0 = xs.Count; i < __end0; i++)` (S6). TS
    `let` and PHP comma init take the same shape.
- **The match-arm parser loop gets a progress guard.** `Fail => throw …` currently hangs the parser forever
  (S8).
- **Backstop:**
  - `ir::Module.loweringFailures` collects failures from the `default` arm (`lower.cpp:909`).
  - `compile()` turns them into *internal compiler error: no lowering for <kind> expression — this is a
    Polyglot bug* and writes nothing.
  - The switch becomes exhaustive: explicit Range and Super arms, g++ `-Wall`, MSVC `/we4062` (SP-S5).
  - A **grep gate** fails if any emitted output contains `__polyglot_unlowered_expr__`.

### A3 + A6 — One stringification rule, culture-free C#

**The rule:**
- Integers render as invariant decimal.
- f64 renders as ECMAScript `Number::toString`:
  - shortest round-trip digits;
  - plain notation for 1e-7 ≤ |x| < 1e21, `e±N` otherwise;
  - `-0` → `0`;
  - `NaN`, `Infinity`, `-Infinity`.
- This applies to `print`, interpolation and `string` concatenation of numbers (where permitted) on **all four**
  targets.

**Helpers**, using §0 placement and reachability. The SP-A1 corpus already passed: 0 mismatches over
258,164 doubles.

| Target | Helper | How |
|---|---|---|
| C# | `__PgRt.Str(int…ulong)`, `Str(double)`, `Str(float)`, `Show(object?)` | **Exact shortest digits (Burger–Dybvig over `System.Numerics.BigInteger`, ties to even)**, then the ECMAScript layout. Never `"R"`; see below. `Str(float)` widens to double first (S13) |
| Python | the `fstr` prelude is replaced | Digits from `repr` |
| PHP | `__pg_str` | `var_export` with `serialize_precision=-1`, set and then restored |
| TS | none | Native |

**Why C# must not use `"R"`** (follow-up spike, 2026-10-17; source in `spikes/F64Exact/`). Over 237,124
doubles checked against Node 24 `String(x)`:

| Formulation | net48 | net8.0.32 | net10.0.13 | net11 rc2 |
|---|---|---|---|---|
| `"R"` + layout | 77,615 mismatches | 0 | **4** | 0 |
| exact Burger–Dybvig | **0** | **0** | **0** | **0** |

- **The .NET 10.0.13 runtime regressed** at power-of-two boundaries: `Math.Pow(2,-25).ToString("R")` is
  `2.980232238769531E-08`, which does not round-trip. So `"R"` is unsafe even on a modern runtime, and the Core
  cannot know the consumer's runtime or patch level.
- E14–E17 round-trip searches fail by design: the power-of-two rounding interval is asymmetric, and
  subnormals have fewer than 15 significant digits.
- **Cost:** about 5–8 µs per call against 0.8 µs for `"R"`, which is acceptable for print and interpolation.
  If speed ever matters, the principled upgrade is a Ryu port, never a runtime switch.
- Python `repr` and PHP `serialize_precision=-1` are correctly-rounded shortest (Gay's dtoa) and passed the
  SP-A1 corpus. They keep the platform digits.

**C# rebinds:**
- int and float interpolation holes become `{__PgRt.Str(x)}` (via the existing `typeIsInt`/`typeIsFloat`);
- `print` becomes `Console.WriteLine(__PgRt.Show(x))`;
- every integer `parse` and `toI32` binds to `NumberStyles.Integer, CultureInfo.InvariantCulture` (**not**
  `AllowLeadingSign`, which is a regression); f32/f64 use `NumberStyles.Float`;
- `toUpper`/`toLower` become `ToUpperInvariant`/`ToLowerInvariant`;
- **the invariant guard is removed from `Main`**, so library and program code are identical.

**TS:** `toI32` gains the i32 truncation and is unified with `i32.parse`.

**f32 text is the f32's exact value widened to f64, printed under the f64 rule** (S13). For example, `0.1f`
prints `0.10000000149011612`. That is faithful on all four targets: over 65,021 literals, widened-double vs
JS `Math.fround` showed 0 mismatches, whereas C# float `"R"` digits mismatched on 74.9%. f32 *arithmetic*
stays a §3.C relaxation, because TS, Python and PHP compute f32 as f64. Same change: the C# f32 literal keeps
its `F` suffix (today `0.1f` emits `float a = 0.1;`, which is CS0664).

**Python `print`** stops calling `__builtins__.print`, which raises AttributeError in any **imported** module.

**Case:**
- New `toUpperAscii`/`toLowerAscii`, faithful on every target: TS a regex replace, Python `translate`, PHP
  `strtr`, C# a loop in `__PgRt`. `System.Text.Ascii` stops at the first non-ASCII character, so it can't be
  used.
- `toUpper`/`toLower` are listed in SPEC §9 as best-effort, with the three behaviours.

**The printable guard reaches interpolation holes.**
- A hole accepts exactly what `print` accepts.
- Records, lists and maps in a hole are an error.
- Enums in a hole or `print` are an error too, by **D1**: write the explicit mapping.

**A6, C# holes.** One generic engine builtin, `{"fn":"parenIfContains","args":[":", <text>]}`, is used in the
C# hole template.
- The character set is manifest data.
- Per SP-A3, only a top-level `:` breaks a hole.
- A false positive costs only harmless parentheses; there are no false negatives.

**Culture conformance leg** (SP-A2 passed):
- A `PgCultureHook.dll` startup hook re-runs every compiled conformance dll under
  `-Cultures tr-TR,nl-BE,sv-SE,fa-IR` and diffs against the reference output.
- The hook **asserts the culture took effect** (fa-IR `NegativeSign != "-"`, tr-TR lowercases `I` to `ı`), so a
  missing ICU cannot pass vacuously.
- One synthetic hostile culture goes into `-Tier fast`.
- PHP gets a hostile `-d precision=17 -d serialize_precision=17` run.
- A grep asserts no emitted `.cs` contains `CultureInfo.CurrentCulture =`.

### A4 — Null-assert and nullable operands

- **New IR node** `ir::NullAssert { operand, where }`.
  - `where` is the source `file:line:col`, owned by the Core, so the message is identical on every target.
  - `!` on a non-nullable operand lowers to the bare operand.
  - The anti-silent-drop load check forces all four plugins to add the rule.
- **Emission.** Every form evaluates `x` once and is valid as an assignment target (SP-A4-S0 passed):

| Target | Emitted form | Throws |
|---|---|---|
| C# | `(x ?? throw new global::System.NullReferenceException("null-assert failed (m.pg:L:C)"))` | NullReferenceException |
| TS | `(x ?? __pgNullAssert("m.pg:L:C"))` | TypeError (helper, non-exported) |
| Python | inline `(x if x is not None else _pg_fail("…"))`; walrus form for non-name operands (S9: the helper costs +16% in the FruitCake hot loop, inline +3.9%) | TypeError |
| PHP | `__pg_nn($x, "…")` | `\UnexpectedValueException` |

   PHP needs a helper because `?? throw` is illegal in write context.
- **Sema rules:**
  - **N1.** A nullable operand to any operator except `==`, `!=` and `??` is an error: *'+' operand of type
    'string?' may be null; supply a fallback with '??' or assert with '!'*.
  - **N2.** A nullable value in an interpolation hole, or passed to `print`, is an error.
  - **N3.** `string + <type parameter>` is an error ("interpolate instead"), consistent with `string + i32`
    already being refused.
  - **Fallout:** S7 prototyped N1–N3 over 156 files, including MintPlayer.AI, and found **zero hits**.
  - **Four paths the prototype showed N1–N3 must also cover:**
    - compound assignment (`n += 1` on `i32?`);
    - `bool?` inside `&&` conditions and in `requireBool`;
    - an if-expression with a `null` branch, which is typed non-nullable today;
    - member access on a nullable receiver.
  - **Null flow-narrowing after `if x != null` (D14).** Without it N1 rejects the most natural shape, so the
    recommendation is to add narrowing for locals and parameters.
- **Runtime backstop** for null reaching code through a type parameter: the generic stringifier (§A3 `Show`
  and its siblings) renders null as `null` on every target.
- **PHP `catch (e: Error)` maps to `catch (\Throwable)`**, so engine errors are catchable (**D13**).
- **`opt!` on the `Option<T>` generic (D12)**, currently refused at `sema.cpp:1822`. It lowers directly as
  `NullAssert` per target: the C# switch arm, the TS closure throw, the PHP match-arm throw, and the Python
  inline check. S8 showed the *language* cannot hold `throw` in an arm, so no throw-expression is added.
- **C# keeps explicit type arguments at call sites.** S8 found `failT<T>(..)` emitted as `failT(..)`, which is
  CS0411.
- **PHP `function_exists` guards are namespace-qualified.** Today's unqualified guard silently calls a
  same-named global function (S12).

### A5 — Config discovery

- `loadPgConfig` owns the form of its input: `d = fs::absolute(startDir.empty() ? "." :
  startDir).lexically_normal()`.
  - Absolute plus lexical normalisation, not `weakly_canonical`, matches the watch precedent and keeps symlinked
    checkouts working.
  - `pc.dir` and `pc.root` are always absolute.
- `inDir`, `entryDir` and the default `outDir` are made absolute too.
- The group key is `normalizedDirKey(pc.dir)`.
- Diagnostics keep the user's spelling of the input path.

### B1 — Namespaces and wrapper-class names, as language-agnostic data

- **`targetOptions`**, not `"csharp": {…}`.
  - pgconfig gains `"targetOptions": { "<target>": { … } }` at project level **and** on `include` rules.
  - Each plugin manifest declares its options in an `options` block (`default`, `pattern`, `scope: "closure"`,
    `reserve`).
  - Templates read them as `option.<name>`.
  - Selecting the map by target name is a data lookup, as `forbiddenIdentifiers` already does.
  - Unknown options and values that fail `pattern` are refused at load or config time.
  - Also: a CLI `--target-option <target>.<name>=<value>`, and MSBuild `PolyglotNamespace`/`PolyglotClass`.
- **C# options:**
  - `namespace` (emits file-scoped `namespace X;` before the usings; `using static` follows it);
  - `class` (default `PolyglotProgram`, reserved as an identifier);
  - `access` moves here from the top level, a clean cutover per the no-backward-compat rule (**D9**).
- **The wrapper class is emitted only when it has members.**
- **PHP:** a `namespace` option in the same change (SP-B1-S2: 102/102 programs pass namespaced).
- **Refusal:** files in one import closure that resolve to different namespaces or classes (`scope: "closure"`).
- **Same change:**
  - The LSP preview honours `targetOptions` (it ignores `access` today, `main.cpp:1194`).
  - MSBuild regenerates after a config edit. S20 confirmed it does not today. The CLI writes a **depfile**
    naming the config and lockfile it used, which the targets read as `Inputs` (**D17**); a fixed
    `$(PolyglotRoot)\pgconfig.json` would guess wrong under A5's upward discovery.

### B2 — TS prelude

This is covered by §0: per-file, reached-only and non-exported (D10, accepted). Per-file copies of `Option`
stay assignable across files, because the TS `UnionDecl` rule emits structural `{ tag: … }` types and matches
test `_m.tag`; nothing relies on `instanceof`.

**The library gate compiles under the Angular CLI library flag set** (accepted with D10): `strict`,
`noImplicitOverride`, `noPropertyAccessFromIndexSignature`, `noImplicitReturns`,
`noFallthroughCasesInSwitch`, `isolatedModules` and `declaration` (with `emitDeclarationOnly`), on an ES2022
target and lib. The follow-up measurement:
- Today's 116 conformance programs give **0 diagnostics** under that set; the four added flags cost nothing.
- **Three latent defects** fire only in probe programs, so they are fixed and gated by new regression
  programs:
  - **Bare union-case construction widens the tag.** `MakeCase` (`typescript:577`) emits `{ tag: "Circle", … }`,
    which widens to `string` in an unannotated `const` or array, giving TS2345. One sample already hits it.
    Fix: emit `tag: "Circle" as const`.
  - **`!` erasure** (TS2531/TS2322 wherever narrowing does not apply). This is fixed by §A4's
    `(x ?? __pgNullAssert(…))`, whose `never` return narrows the result.
  - **No `override` on a field that shadows a base member** (TS4114, e.g. `cause` on an `Error` subclass). Fix:
    manifest data listing the base-root members, so the plugin emits `override` on such fields. No target-name
    check enters the Core.
- **A second gate leg** uses `lib: ["ES2022","dom"]` with **no `process` shim** for programs that do not use io.
  Today every file fails 5× with TS2591, because the unreached std-io functions are emitted. This leg proves
  §0's reachability instead of hiding it behind the hermetic `env.d.ts`.
- The gate also covers `docs/lang/samples`.

On top of that:
- The library gate's barrel case is built over a **dedicated multi-module project**, not over all conformance
  programs, since 112 of 114 export `main` and would collide on user names regardless.
- `run-library.ps1:92-97` checks export status of **user** declarations only.

### B3 — Ordinal string std

- **Guards are written once in `.pg`**, over unchecked per-target primitives (SP-B3-S1 passed with identical
  output and messages).
  - There is one guard and one message, so the four manifests cannot drift.
  - Primitives are only called with plain identifiers, so there is no double evaluation.
  - The guards work on Python, whose lambdas cannot `raise`.
- **Primitives** are `expect fn`s with a `__pg` prefix, kept out of the user namespace by
  `forbiddenIdentifiers`.
- **API.** All functions are ordinal, and all throw `Error` out of range on every target.

| Function | Notes |
|---|---|
| `charAt(i)` | changed: throws out of range |
| `codePointAt(i)` | changed: throws out of range; a lone surrogate returns its unit |
| `substring(start, end)` | |
| `indexOf(needle, from = 0)` | needs extension default parameters, fixed in this change |
| `startsWith`, `endsWith`, `contains` | |
| `replaceAll(needle, replacement)` | empty needle throws; TS uses `split(n).join(r)` because of `$$` |
| `split(sep): List<string>` | empty separator throws |
| `trimAscii()` | exactly U+0009–U+000D and U+0020, spelled per target, because Python `strip()` eats GS |
| `string.fromCodePoint(cp)` | throws for surrogates, < 0 and > 0x10FFFF; PHP uses a UTF-8 encoder helper via `requires` |
| `compareOrdinal(a, b)` | returns −1/0/1; PHP uses `strcmp`, never `<=>` |

- **C# bindings pass `StringComparison.Ordinal`** to `IndexOf`, `StartsWith` and `EndsWith`. Golden tests pin
  this, so the culture-sensitive overloads cannot come back.
- **Index-unit tier (D2, D15, S1):**
  - **Python is UTF-16-faithful** through helpers covering `len`, `charAt`, `codePointAt`, `substring` and
    `indexOf`. The fast path is `isascii()` (O(1)); other strings use a cached surrogate-expanded view. ASCII
    cost is 0.98–1.25× the guarded call.
  - Faithful Python needs a **surrogate→U+FFFD stdout handler**, because CPython crashes printing a lone
    surrogate where C# and Node print U+FFFD.
  - The test harness must pass `-X utf8`, because `python -I` ignores `PYTHONIOENCODING`.
  - **PHP is faithful only if iconv becomes a declared requirement** (D15). The helpers use an ASCII scan,
    then a cached UTF-16LE copy, at 1.4–1.8× on ASCII. Otherwise PHP index functions carry a published
    byte-unit caveat.
  - A new `utf16Strings:index` capability child, marked at string index calls, makes the choice explicit.
    Today `utf16Strings` is marked only on `char`, and Python/PHP wrongly default to "native".
- **`std.strings` imports `std.collections` itself**, so `split` can return `List<string>` (S18). The resulting
  flat-namespace leak of `List` is addressed by module scoping (D16).
- **A bare `import` of an extension-only module emits its names** on Python and TS. Today it is a NameError
  and TS2304 (found by the S16–S20 spikes).

### B4 — `Map<K, V>`, and its prerequisites

**Prerequisites** (pre-existing defects, fixed at the root):
- **Generic member typing.** Sema substitutes a generic class's type parameters from the receiver type when
  typing members, as `inferResult` does for generic functions. After substitution, `V?` with `V = i32` is a
  native `int?`; `Option<T>` remains only for an unresolved type parameter. This fixes the whole class, not
  just `Map`.
- **Class type-parameter bounds are enforced**, as on functions.

**API:**

```pg
extern class Map<K: IMapKey, V> {
  constructor()
  let count: i32
  fn get(key: K): V?
  fn set(key: K, value: V)
  fn has(key: K): bool
  fn remove(key: K): bool
  fn keys(): List<K>      // snapshot, insertion order
  fn values(): List<V>    // snapshot, insertion order
}
```

- **Keys** are `string` and the integer scalars, via an `IMapKey` marker. Refused: floats, `bool`, `char`,
  records and classes, each with a diagnostic naming the reason.
- **`V` must not be nullable.** Otherwise `get` cannot distinguish an absent key from a stored null; `has` is the
  escape hatch.
- **`keys()` and `values()` return snapshots**, because mutating a map while iterating it differs on every
  target. `Map` is not directly `for`-iterable in this version.

**Bindings:**

| Target | Type | Notes |
|---|---|---|
| C# | `OrderedDictionary<K,V>` | .NET 9+ (**D8**). `get` is `(m.TryGetValue(k, out var __pg_tN) ? __pg_tN : (V?)null)`. The typed else-arm avoids CS0173; a per-expansion unique temporary (a small Core token) avoids CS0136/CS0128 (S16) |
| TS | `Map` | `get` returns `?? null`, because `==` is emitted as `===` |
| Python | `dict` | |
| PHP | `\ArrayObject`, never a bare array | String keys are prefixed (`is_int($k) ? $k : "s".$k`) and stripped on the way out, so `"10"` never becomes `10` |

### B5 + D17 — the outputs record: stale-file cleanup for every invocation

*(Added after the maintainer accepted D17 with "clean up for MSBuild's explicit inputs too": "previous outputs
minus current outputs".)*

**Bug reproduced** (follow-up, 2026-10-17, against the real `.props`/`.targets`):
- After renaming `b.pg` to `c.pg`, `obj/…/polyglot/b.cs` survives and `c.cs` is never written.
- After deleting `a.pg`, `Program.cs` calling `PolyglotProgram.alpha()` **still builds with 0 errors**,
  compiling the stale `a.cs`.
- Cause: `Inputs` and `Outputs` cannot see a removed item, and a rename keeps the file's mtime. The P28
  `RemoveDir` never runs, and `_PolyglotAddGenerated` globs `*.cs` (`.targets:104`).

**Two more bugs found by reading:**
- The dedup `seen` set is per group, so two config groups routing to one path overwrite each other with no
  error.
- Discovery takes its patterns from the cwd config but groups files by their nearest config, so a nested
  config's group can be incomplete.

**Design:**
- **Two files, one writer.**
  - The **record** (JSON, for the CLI):
    `{version:1, sections:{"<configDir>|<target>": {inputs:[…], outputs:[{path, map?}]}}}`. Paths are relative
    to the record's directory, so a copied repo never deletes in the original. It is written via temp file and
    rename; just before writing, the CLI re-reads it and replaces only its own sections.
  - The **depfile** (`--depfile <path>`): one absolute path per line, listing the config, the lockfile, every
    `.pg` in each closure, and `file:` plugin manifests. MSBuild reads it with `ReadLinesFromFile`.
- **Only an authoritative run keeps a record (D18).**
  - Discovery mode writes `<configDir>/.polyglot/outputs.json`, for the discovery config's group. The folder
    has its own `.gitignore` of `*`.
  - `--record <path>`: the caller vouches its input set is complete. MSBuild passes
    `$(IntermediateOutputPath)polyglot.outputs.json` and `polyglot.deps`, **outside** `PolyglotOutDir`. That is
    one record per intermediate directory, so parallel TFM builds never cross-delete.
  - Explicit inputs without `--record`, `--target`+`--out`, and watch mode keep **no record and delete
    nothing**, with a notice.
- **The record is not committed (D19).** CI's `--check` catches a rename as `would create <new>`, and the owned
  scan covers the rest. The one documented gap: a route moved outside every owned prefix, rebuilt on a machine
  with no record.
- **The deletion invariant** (stated once; confirmed by Spark on #73):
  1. There are **exactly two deletion paths**: the record's doomed set, and the owned-prefix extras.
  2. **Both** require the file to carry the `Generated by Polyglot.` line — as a header **line** in the
     target's comment syntax (`<lineComment> Generated by Polyglot.` from column 0, one of the first 5
     lines; the token is the plugin manifest's load-validated `lineComment`). A line that merely mentions
     the phrase does not count (review of #75, 2a).
  3. **A file without that line is never deleted**, by either path. A `.map` goes only with its owner, at
     the path derived as `owner + sidecarExtension` (the record's `map` field is never trusted), and only
     when it is a v3 map whose `file` names that owner (review 2b).
  4. Any failed group in a run means **nothing** is deleted.

  **The overwrite guard (D21, review 3a)** is the invariant's other half: `put` never *replaces* an existing
  file whose bytes differ unless it is Polyglot's — it carries the header line (a sidecar: names its
  owner), the outputs record lists it, it sits under a directory the caller declared build-owned
  (`--build-owned <dir>`; MSBuild passes its `$(PolyglotOutDir)`), or the run passes `--adopt`. Otherwise:
  exit 1, the file untouched, and under `--check` an error rather than `would update`. **Upgrade from
  0.10.x** (headerless outputs, no record): `polyglot build --adopt` once; MSBuild needs nothing.
  Neither option widens what may be deleted.

  So the owned scan deletes stale twins on a fresh clone with no record, during `build` as well as reporting
  them under `--check`. Nobody ever has to delete a file by hand after CI reports `would delete`.
- **`build`:** after all groups succeed, for each section built this run:
  - `doomed = previous − current`, then minus **every** current output of the run.
  - A doomed file is deleted only if it still carries `Generated by Polyglot.` in its first 5 lines (**D20**:
    the record path of the invariant above). A recorded file without the line is kept, with a notice.
  - A `.map` goes with its owner.
  - **Any failed group means no deletion and no record rewrite.**
- **`--only` / `PolyglotTargets`:** only the sections for the targets built are read or replaced; the others
  carry over byte for byte. A csharp-only MSBuild run never touches the TS twins.
- **`--check`:** reports the same `doomed` set plus owned extras as `would delete`, writes nothing, and exits 3.
- **watch:** never deletes and never writes the record.
- **A missing, corrupt or newer record:** warn once, treat it as empty, and rewrite it after success. Never fail.
- **The owned scan (D5) stays alongside**, for what the record never saw: a fresh CI checkout, a lost record, a
  manual copy. The record covers what the scan cannot reach: explicit inputs, `obj/`, and routes outside any
  owned prefix.
- **MSBuild:**
  - The transpile and the `@(Compile)` injection run **before `BeforeCompile`** (`_PolyglotGenerate`), so the
    SDK's `_GenerateCompileDependencyCache` hashes the complete item list: a deleted generated file changes
    `CoreCompileInputs.cache` and CoreCompile re-runs even when every surviving `.cs` is byte-identical
    (review of #75, item 1 — hooked before `CoreCompile`, a 3-file delete kept linking the stale DLL).
  - `_PolyglotInputsCache` (`WriteLinesToFile WriteOnlyWhenDifferent` over the `.pg` set plus
    `PolyglotTargets`/`Lib`/`Access`/`OriginInfo`) **replaces the stamp-name tag**.
  - `Inputs = @(PolyglotFile);$(PolyglotTool);<cache>;@(_PolyglotDep)`.
  - The record, depfile and cache go into `FileWrites`.
- **`OutputSink` owns one produced-path set for the whole run**, which turns cross-group collisions into an
  error. Discovery groups by the nearest config consistently.

### B5 — `build --check`, owned directories, generated header

- **One `OutputSink`** (`Cli/src/outputsink.hpp`) with Write and Check modes.
  - It owns the dedup map and the produced-file set.
  - All three write paths use it, which also fixes watch mode's missing source map.
- **`build --check`:**
  - writes nothing (no directories, no maps, no lockfile; `resolvePluginDependencies` gains a dry-run mode);
  - prints sorted `polyglot: would create|update|delete <path>` lines;
  - exits **3** on drift.
- **Exit-code rank** replaces numeric max: 64/66/69 > 1 > 3 > 0. `--check --watch` → 64.
- **`"owned": true` on an include rule.** The scope is the rule's literal output-folder prefix, recursive when
  the template uses `%(RecursiveDir)`/`%(Directory)`, limited to files of the targets built in this run.
  It is resolved by the same normalization as routing (`\` is a separator; a leading separator stays
  config-relative), and refused at config load (review 2c) when the prefix is empty, is the config folder or
  one of its ancestors, is a filesystem root, or the template is absolute or starts with `/` or `\`. `..`
  into a sibling subtree (Spark's `../../…/scanning/src/gs1/`) stays allowed.
  - An extra file **carrying Polyglot's own line** (`Generated by Polyglot.`, in the target's comment syntax)
    → drift (`--check`) or deletion (`build`).
  - Every other extra file → exit 1 and never deleted (**D5**, as accepted on #73). That includes files
    carrying only the bare `<auto-generated/>` marker, which Roslyn generators, NSwag, OpenAPI Generator and
    protoc plugins all emit. Spark commits its own generated `contracts.ts` into the same Angular entry
    points, and hand-written `.ts` in `src/app/` is equally safe.
  - This applies only when the build discovered its own inputs. With explicit inputs (as MSBuild passes them)
    it is skipped with a notice.
- **Header**, as plugin data in each `Program` rule (Python uses `#`, PHP puts it after `<?php`):

  ```
  // <auto-generated/>
  // Generated by Polyglot. Do not edit: regenerate with `polyglot build`.
  ```

  There is no version and no timestamp. It is emitted via `line()`, so origin accounting stays correct.
  `<auto-generated/>` is for analyzers; **the `Generated by Polyglot.` line is the ownership key** for every
  deletion decision (D5). A unit test asserts every in-box plugin emits both lines.

### B6 — Build-time target filter

- **`--only <a,b>`** (repeatable; name pending **D4**) intersects with pgconfig `targets`.
  - A name outside the project's set → 64, naming both lists.
  - Combined with `--target` → 64.
  - Include routing is unchanged.
- **A repeated `--target` becomes a usage error** instead of last-wins.
- **MSBuild:** `PolyglotTargets` passes ` --only "$(PolyglotTargets.Replace(';', ','))"`. It is part of the
  `_PolyglotInputsCache` (§B5 + D17), so switching back to all targets regenerates.
- **Default (D3, accepted):** empty means the full pgconfig set. This keeps the P30 story and the existing
  NuGet fixture. The documented recipe for Nx and other cached monorepos is Spark's own setup:
  `PolyglotTargets=csharp` in the library's `.csproj`, plus **`polyglot build --check --only typescript`** in CI
  for the TS twin. **That exact combination is CLI-tested.** It must exit 0 when clean and 3 on a modified,
  new or stale TS twin, and must neither report nor touch the C# outputs.

### B7 — Checkout-independent origin info

- `originPathFor` emits the `.pg` path **relative to the generated file's directory**, forward-slashed and
  `weakly_canonical` on both sides.
  - A different drive falls back to absolute, which is documented.
  - The new `IrModule.originBaseDir` is set per file.
- `coverage remap` resolves a relative directive path against the generated file's folder; absolute paths keep
  working.
- **issue-69 D2 is amended**, dated, in that PRD: SP-B7a shows the PDB is unchanged.

## 5. Acceptance

The issue's criteria hold as amended here:
- A1's `"\u{1F600}".len()` is asserted per the published table, not `== 2` on Python.
- B2's barrel uses a dedicated project.

Then:
- Conformance programs:
  - `escapes.pg`
  - `range_precedence.pg`
  - `null_assert.pg`
  - `interp_qualified.pg`
  - `numbers_text.pg` (the f64 edge values)
  - `ascii_case.pg`
  - `strings_std.pg`
  - `strings_oor.pg`
  - `map.pg`
  - `generic_members.pg`
- The **culture leg**, asserting the culture took effect.
- The **grep gates**: no poison identifier, and no `CurrentCulture =`.
- **Refusal fixtures** for every new diagnostic.
- The **two-library C#** and **two-library PHP** consumer cases, and the **TS barrel** in the library gate.
- **CLI smoke tests:** relative discovery (3 forms + `./a.pg` + mixed), `--check` exit codes (unchanged /
  modified / new / extra-owned / extra-unmarked / compile-error-plus-drift), `--only`, and origin-info
  byte-identity across two checkout roots.
- **NuGet gate:** `PolyglotTargets=csharp` writes no `.ts`; flipping it back writes the twin; editing
  `pgconfig.json` regenerates.
- The **nullable gate** gains `!` cases and is warning-clean.
- The full gate once, plus the **POSIX leg** (required: path and `#ifdef` code changes).

## 6. Compatibility

There are **no shims** (`no-backward-compat`). Every behaviour change is observable only where the targets
already disagreed or the output was already wrong:
- Unknown escapes, misplaced ranges, nullable operands, unprintable holes and enum printing become
  diagnostics.
- C# number text, PHP float text, `charAt`/`codePointAt` out of range and `!` change behaviour.
- `access` moves under `targetOptions`.
- `escapes` becomes `literals`, and plugins gain `options`, `preludeSlot` and binding `requires`. **The plugin
  manifest `"schema"` goes from 1 to 2** in all four manifests (maintainer decision, 2026-10-17). The Core
  starts **validating** the field, which it never read before: a schema-1 or unknown-schema manifest is refused
  at load, naming the expected version. Package versions stay `0.0.0-dev` in-tree and are stamped by CI/CD
  from the tag.
- The header changes every generated file once.

**Floors (D8, accepted with corrected wording):**
- **C# 11 and a .NET 8+ BCL.** `Map` additionally needs **.NET 9+**. netstandard2.0, .NET Framework, Mono and
  Unity are **not supported**.
- What rules netstandard2.0 out is the existing output and the BCL, **not** the `file` keyword. The maintainer
  measured `file static class` compiling on netstandard2.0 with `LangVersion 11`. But the follow-up spike
  found 0/116 conformance programs compile there today:
  - The prelude's `record` needs `IsExternalInit`; a per-file polyfill collides, and a `file`-scoped one does
    not satisfy the compiler.
  - The `codePoints` and `Log2`/hyperbolic bindings need netcoreapp3.0+.
  - Several planned P40 overloads are missing (`Contains(string, StringComparison)`, `Split(string)`,
    `OrderedDictionary`).
- netstandard2.0 compilability is **not** a P40 goal (§2 non-goals).
- .NET 9 for programs that use `Map`.
- PHP 8.2.

The release ships as one minor version under lockstep.
