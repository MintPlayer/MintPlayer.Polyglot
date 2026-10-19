# P40 — Library-grade output (issue #73) — investigation evidence

> Companion to `PRD.md` (the design contract) and `PLAN.md` (slices, spikes, decision log).
> Source: a **6-agent investigation** on 2026-10-17 against HEAD `a603dba`, with probes run on the
> 2026-09-19 Debug CLI plus dotnet 11 rc2, Node 24, Python 3.13, PHP 8.5 and the global `tsc`.
> The investigators split the work as: A1+A2 · A3+A6 · A4 · A5+B5+B6+B7 · B1+B2 · B3+B4.
> The probe sources were throwaway scratch files. Everything load-bearing is restated here, so this
> document stands on its own.

The issue's **line numbers held up almost everywhere**. Its **scope did not**: every item turned out to be
one instance of a wider class of defect, and three of its proposals would have introduced new defects.
These are the headline corrections:

| # | The issue says | The investigation found |
|---|---|---|
| 1 | `"\u{1F600}".len() == 2` on C#, TS **and Python** | Python counts code points (1) and PHP counts bytes (4). SPEC.md:529 is wrong too: it claims code points "on C#/TS/Python". |
| 2 | Escape fixes are a lexer change | There is also a **PHP `$` miscompile** (`"cost $x"` interpolates on PHP). Raw NUL bytes make the Python output unloadable while the build exits 0. Char literals are never validated, and `polyglot fmt` turns `\${` into a live interpolation. |
| 3 | A2 is a grammar precedence bug | The root cause is **sema** (`sema.cpp:1816` types a range as its lower bound). `super.f()` is a second source of the poison identifier. Range bounds are evaluated once on Python but on every iteration elsewhere. |
| 4 | Parse with `NumberStyles.AllowLeadingSign` | That is a **regression**, because it rejects `" 5"`, which every target accepts today. Use `NumberStyles.Integer` with the invariant culture. |
| 5 | Culture is the f64 divergence | Float text diverges on **three of four targets even with the guard on**. PHP `echo` uses `precision=14`, so `print(0.1+0.2)` prints `0.3` today. |
| 6 | `!` is broken on C# reference types | `!` throws on **no** target for any type, except C# value types. And `string + T?` is one case of a hole where every nullable operand is typed `Unknown` and accepted. |
| 7 | Patch the six `loadPgConfig` call sites | Only two of them are buggy, and the silent failures (exit 0 with the config ignored) are worse than the exit 64 the issue shows. Fix it inside `loadPgConfig`. |
| 8 | MSBuild passes `--target csharp` | `--target` with `--out` bypasses include rules entirely (P30 D7). It is single-valued and last-wins. It is the wrong tool. |
| 9 | Make `#line` paths relative | Absolute paths were a **deliberate decision** (issue-69 PRD §4.F, D2). A spike shows a relative path still works, so the decision is amended rather than reversed silently. |
| 10 | Dedupe `Option` into a sibling TS file | The whole prelude (`print`, `readText`, …) is pasted and **exported** into every file, so a barrel collides on more than `Option`. "Emit only when referenced" needs a reachability pass that does not exist. |
| 11 | `Map<K,V>` is a std addition | It is **blocked by an existing silent miscompile**: methods of a generic class are not typed with the instance's type arguments. `Cell<string>.first(): V` stays `V`, and `polyglot build` exits 0. |
| 12 | Pgconfig key `"csharp": {...}` | The C++ Core must not compare target names. It becomes a manifest-declared `targetOptions` bag instead. |

---

## A1 — String and char escapes

**Verified.**
- `lexer.cpp:79-89` (`scanChunk`) and `:105-116` (`scanQuoted`): the `default: value += esc;` arms at `:88` and
  `:115` drop the backslash.
- `"\u{1D}"` emits `"u{1D}"` (length 5) on **all four** targets.

**Root cause.**
- There are two independent escape decoders, and they already disagree: `\'` is decoded only in chars.
- Neither decoder handles code points, and both fall through silently.
- `SPEC.md:41`'s `'\u{1F600}'` cannot exist: SPEC §8 (`:504`) defines a char as one UTF-16 code unit.

**Missed by the issue (all reproduced):**
1. **PHP `$` miscompile.** String literals go through the shared `renderString` (`backend_spec.hpp:313-330`),
   which never escapes `$`. PHP double-quoted strings interpolate, so `"cost $x and ${y}"` prints
   `cost  and ` plus an "Undefined variable" warning. The PHP and Python interpolation chunks
   (`php…json:954`, `python…json:2784`) have the same gap.
2. **Raw control characters.**
   - `"nul\0end"` writes a raw 0x00 into all four outputs. Python refuses the file ("source code cannot
     contain null bytes"), yet the build exits 0.
   - A raw U+2028, U+2029 or U+0085 would be CS1010 in C#.
3. **Char literals are never validated.**
   - `'ab'`, `''`, `'😀'` and `'\u{41}'` emit C# that does not compile (CS1012, CS1011).
   - TS and Python silently get strings.
   - The C# `escapes.char` map (`csharp…json:102-108`) has no NUL entry.
4. **`polyglot fmt` changes meaning** (`pg_printer.cpp:342-359`). It reprints `"lit \${x}"` as `"lit ${x}"`
   and writes NUL as a raw byte.
5. **The VS Code grammar** (`editors/vscode/syntaxes/polyglot.tmLanguage.json:97-99`) highlights `\uXXXX`,
   which the lexer rejects. It has no char pattern.
6. **The PHP non-ASCII warning is invisible.**
   - `reportDiagnostics` (`Cli/main.cpp:176-181`) runs only on failure (`:301`, `:910`).
   - It hardcodes `error:` and prints once per target.
7. **The octal trap.** NUL emitted as `\0` followed by a digit is an octal escape on Python and PHP, and a
   strict-mode SyntaxError on TS. NUL must be `\u0000` (C#, TS) or `\x00` (Python, PHP).
8. Invalid UTF-8 inside a literal passes through unchecked.

**Per-target string length** (this decides A1's acceptance criterion and B3's guarantee tier). For `"a😀b"`:

| | `.len()` | `charAt(2)` |
|---|---|---|
| C# / TS | 4 (UTF-16 units) | lone surrogate |
| Python | 3 (code points) | `b` |
| PHP | 6 (bytes) | — |

## A2 — Range precedence and the poison identifier

**Verified.**
- `grammar.ebnf:316-317`. The parser is `parser.cpp:1126-1138`, called from `parseUnary` (`:1124`).
- Lowering is `lower.cpp:905-909`. Ranges are lowered only at the `for` site (`:1002-1006`).

**Root cause is sema.** `sema.cpp:1816` types a range as its lower bound (or `i32`). It never checks the upper
bound, never requires integers, and ignores where the range appears, so `(0..n) - 1` type-checks as
`i32 - i32`.
- C# fails at compile time (CS0103).
- TS, Python (NameError) and PHP (undefined constant) fail at **runtime**.
- The build exits 0.

**Missed by the issue:**
- **More range forms reach the poison:** `-2..2` parses as `-(2..2)`; `0..n * 2` and `let r = 0..5` do too.
- **`super.f()` / bare `super` reach the poison** on every target.
  - Sema returns Unknown silently (`sema.cpp:1790`).
  - Only the base-constructor `super(...)` is lowered (`lower.cpp:576-584`).
  - So the poison has exactly two sources: Range and Super. The comment naming CharLit is stale; it is lowered
    at `:692`.
- **Float ranges** `1.5..3.5`: C-style loops on C#, TS and PHP, but `range(1.5, 3.5)` on Python, which is a
  runtime TypeError.
- **Bound evaluation diverges.** A body that mutates the bound prints 6 on TS and PHP (`i < n` each iteration)
  and 3 on Python (`range(0, n)` once). SPEC is silent on which is right.
- **Mixed-width bounds:** the loop variable takes the lower bound's type (`sema.cpp:1296`), so an `i64` upper
  bound overflows C# past 2³¹.
- **Workaround already in the corpus:** `fruitcake.pg:162` writes `(i + 1)..this.bodies.count`.
- **No diagnostic codes exist.** `Diagnostic` (`diagnostics.hpp:22-27`) has no code field, and the refusal gate
  (`tests/refusals/run-refusals.ps1:64-80`) matches message text.

## A3 — Culture-dependent C# bindings, and stringification across all targets

**Verified line references** (`plugins/csharp/polyglot-plugin.json`):

| Lines | What |
|---|---|
| `:3708-3709` | toUpper / toLower |
| `:3713` | toI32 (`global::System.Int32.Parse($this)`) |
| `:3723-3730` | integer `parse` |
| `:3731-3732` | f32/f64 parse with InvariantCulture |
| `:2849`, `:2854` | the invariant guard, inside the `Program` rule (`:2572`) |

All the issue's repros reproduced under .NET 11 with ICU.

**Missed by the issue:**
1. **ar-SA** also throws on `int.Parse("-5")`.
2. **Integer `print` is culture-sensitive.** The print binding (`:3665`) is `Console.WriteLine((object?)x)`, so
   `print(-5)` prints `−5` under sv-SE in library mode.
3. **Float text diverges with the guard on.** TS is the reference:

| value | TS | C# (invariant) | Python | PHP |
|---|---|---|---|---|
| 0.1+0.2 | 0.30000000000000004 | same | same | **0.3** |
| 1/3 | 0.3333333333333333 | same | same | **0.33333333333333** |
| 1e20 | 100000000000000000000 | 1E+20 | 1e+20 | 1.0E+20 |
| 1e16 | 10000000000000000 | same | 1e+16 | 1.0E+16 |
| 1e-6 | 0.000001 | 1E-06 | 1e-06 | 1.0E-6 |
| 1e-7 | 1e-7 | 1E-07 | 1e-07 | 1.0E-7 |
| -0.0 | 0 | -0 | 0 | -0 |
| 5e-324 | 5e-324 | 5E-324 | 5e-324 | 4.9406564584125E-324 |

   The suite only prints round floats (`float_print.pg`, `interp_print.pg`), so this has never been caught.
4. **Interpolation holes have no printable guard.** `print` rejects non-scalars (`sema.cpp:2256-2261`), but
   holes are only `checkExpr`'d (`:1786`).
   - `"${record}"`: C# `P { x = 1.5 … }`, TS `[object Object]`, Python `<__main__.P …>`, PHP fatal.
   - `"${enum}"`: C# `Green` against `1` on the others.
   - `"${list}"`: C# ``List`1[…]`` against TS `1,2`.
   - `print(enum)` passes the guard and diverges the same way.
5. **The issue's `AllowLeadingSign` proposal is a regression.** It makes `int.Parse(" 5")` throw.
   `NumberStyles.Integer` with InvariantCulture fixes fa-IR and ar-SA without that regression.
   `f64.parse` includes `AllowThousands`, so `"1,000"` is 1000 on C# and NaN on TS; use `NumberStyles.Float`.
6. **TS `toI32` is `parseInt($this, 10)` with no `|0`** (`typescript:4545`), so `"3000000000".toI32()` is an `i32`
   holding 3e9.
7. **The prelude mechanism cannot host a C# or PHP helper.**
   - `emitter_base.cpp:1606-1622` prepends at the top of the file. In C# a type before `using` is CS1529; in PHP,
     text before `<?php` is echoed.
   - Std bindings are plain strings and cannot `require` a prelude, which is why Python's print binding
     (`python:3775`) inlines its float shim.
8. **Case mapping has three behaviours:**
   - Python matches JS full Unicode (`İ`→`i̇`, final sigma, `ß`→`SS`).
   - C# invariant uses simple mapping.
   - PHP `strtolower` is ASCII-only from 8.2 and locale-dependent before. No PHP minimum is stated anywhere.

**Spikes run by the investigator:**

| Spike | Result |
|---|---|
| **S1 — ECMAScript `Number::toString` clone** | **0 mismatches over 258,164 doubles** on C#, Python and PHP. Corpus: 200k random bit patterns, every power of two, 1eN/5eN for N in −330..310, subnormals, ±0, NaN, ±Inf. Run under nl-BE, tr-TR, fa-IR, sv-SE and PHP `precision=14`/`serialize_precision=17`. Method: take the platform's own shortest digits (.NET Core 3.0+ `"R"` invariant, Python `repr`, PHP `var_export` with `serialize_precision=-1`) and redo only the layout, about 25 lines per target. |
| **S2 — culture leg** | **Pass.** A ~10-line `PgCultureHook.dll` loaded via `DOTNET_STARTUP_HOOKS` reads `PG_CULTURE` and sets Current, UI and DefaultThread cultures before `Main`; the culture holds across `await`. Without the guard, library and program code are identical, so re-running each already-compiled conformance dll under the hook covers about 167 programs with no extra transpile. Cost is about 15–20 s on the full gate. |
| **S3 — Roslyn hole forms** | Only a **top-level `:`** breaks a hole. These compile unparenthesized: string literals containing `:`, nested `$"{$"{n}"}"`, braces, and generic commas. |

## A4 — Null-assert `!` and nullable operands

**Mechanism.**
- `lower.cpp:736` lowers `!` to an `ir::Cast`.
- The C# cast rule is at `csharp…json:623`.
- `node.castSame` (`emitter_base.cpp:81-84`) ignores nullability, so TS, Python and PHP emit the bare operand.
- **No target throws for any type, except C# value types** (`(int)(i)` raises InvalidOperationException).

**Wider hole.** `scalarTyOf()` (`ast.hpp:49-50`) maps every nullable type and every type parameter to `Unknown`,
and `checkBinary` (`sema.cpp:1944-2010`) accepts the result. Measured, with no diagnostic:

| Expression (operand null) | C# | TS | Python | PHP |
|---|---|---|---|---|
| `"a" + n` | `a` | `anull` | TypeError | fatal |
| `ni < 1` | false | true | TypeError | true |
| `ni + 1` | null | 1 | TypeError | 1 |
| `"${n}"` / `print(n)` | `""` | `null` | `None` | `""` |

On PHP the unknown type makes the concat rule (`php:1139-1165`, keyed on `node.type == "string"`) emit `+`
instead of `.`.

**Also found:**
- PHP `catch (e: Error)` maps to `catch (\Exception)`. PHP engine errors (`TypeError`, `DivisionByZeroError`)
  are `\Error`, so a `.pg` program can never catch them.
- Reading a property on null is only a PHP **warning**.
- The current TS output fails `tsc --strict` (TS2322 on `return s;` from `!`; TS18047 on `box.next!.v`). The
  conformance runner only strips types.
- The nullable gate (`tests/nullable/run-nullable.ps1`) passes only because its fixture has no `!`. The current C#
  output produces CS8600, CS8602, CS8603 and CS8629.
- The only unit test touching `!` (`tests_main.cpp:243`) parses it.
- `fruitcake.pg:238` writes through `!` (`b!.vx += …`).
- The doc references are `docs/lang/SPEC.md:188` and `docs/lang/for-csharp-devs.md:217` (repeated at `:246-247`).

**Spike S0 — pass.** The throwing forms (PRD §A4) compile and run. They throw a catchable error with an
identical message on all four targets, evaluate the operand once, and work as an assignment target
(`box.next!.v += 1`). They are warning-clean under `/nullable:enable /warnaserror` and `tsc --strict`. The one
failure was PHP `$x ?? throw …` in write context ("temporary expression in write context"), which is why PHP
uses a helper.

## A5 — `pgconfig.json` discovery for relative inputs

`pgconfig.hpp:93-137` walks `parent_path()` lexically. `"a.pg"` passes `"."`, `"LibA/a.pg"` passes `"LibA"`
(whose parent is `""`), and both stop early. The call sites:

| Line | Caller | Status |
|---|---|---|
| 428 | watch | Already absolutised at 426-427 |
| 742 | bare build | `current_path()`, fine |
| **771-772** | **build grouping** | **bug** (the issue says 770) |
| **870** | **`check`** | **bug** |
| 1137 | LSP | absolute from the URI, fine |
| 1616 | install | `current_path()`, fine |

**Silent failures, worse than exit 64:**
- `build --target csharp a.pg` exits 0 with the whole config ignored: include routing, `forbiddenIdentifiers`,
  `root`, `lib`, `access`, `originInfo` and `dependencies`.
- `check a.pg` exits 0 where the absolute path gives exit 1 (a forbidden identifier is missed).
- `../LibA/a.pg` plus an absolute sibling: the group key is the relative `pc.dir.string()` (`:773`), so one
  config forms two groups. That emits two standalone non-partial `PolyglotProgram`s and duplicate `Option<T>`
  (CS0101 when compiled together), and the build exits 0.

## B5 — `--check`, owned directories, generated header

**Current state.**
- Three write paths, all via `cli::writeFile`:
  - `writeDedup` (`main.cpp:196-231`)
  - `writeSourceMap` (`:263-292`, which always writes)
  - watch's `writeOut` (`:500-506`)
- No header is emitted. The first line comes from each `Program` rule: C# `#nullable enable` (`csharp:2575`),
  PHP `<?php` (`php:3321`).

**Bugs found.**
- Watch mode under `--origin-info` never writes the TS/Python `.map` or footer (`:454` vs `:500-511`).
- `runBuild` merges group exit codes with `if (rc > worst)` (`:783`), so a drift 3 would mask a compile failure 1.
- `resolvePluginDependencies` can save `pgconfig.lock.json` (`pluginresolve.hpp:271,280`), which a dry run
  must suppress.

**Exit codes today:** 0, 1, 64, 66, 69. Code 3 is unused.

**Spike S-B5 — pass.** With a `// <auto-generated/>` header (after `#line hidden`), Roslyn treats the file as
generated: CA1050 and CA1515 are suppressed. `#nullable enable` still keeps nullable warnings, and becomes
load-bearing, because Roslyn disables nullable in generated files by default.

## B6 — Build-time target filter for MSBuild

- `MintPlayer.Polyglot.MSBuild.targets:85-91` is accurate. It passes files, `--out obj`, `--root`, and optionally
  `--lib`, `--access`, `--origin-info`, but no language (a P30 decision).
- `tests/msbuild/run-nuget.ps1:108-150` **asserts** that `dotnet build` writes the TS twin through an include
  rule.
- `--target` is single-valued and last-wins (`main.cpp:713`), and with `--out` it sets `flagRouted`, bypassing
  include rules (`:654`, P30 D7).
- `.targets:67` `Inputs` omit `pgconfig.json`, so editing it does not retrigger the transpile (B1 investigator).

## B7 — Relative `#line` paths

- `emitter_base.cpp:1664-1695`: `originPathFor` returns the canonical absolute, forward-slashed path. The CLI
  writes TS/Python maps relative to the map (`main.cpp:258-292`).
- Canonicalisation differs in case (`c--Repos` canonical vs `C--Repos` from `fs::absolute`), so relativisation
  must use `weakly_canonical` on both sides.
- `coverage remap` resolves directive paths against the **working directory** (`coverage_cmd.hpp:196`).

**Spike S-B7a — pass.** Roslyn resolves `#line 3 "../src/m.pg"` against the .cs file's folder. The PDB records
the same absolute document as today (`/_/src/m.pg` under Deterministic+PathMap), so the coverlet evidence
behind issue-69 D2 still holds. A PathMap-style option **cannot** make committed output identical, because
Roslyn's PathMap rewrites only the PDB.

## B1 — C# namespace and wrapper class

- Both repros reproduce: two net10.0 classlibs plus an app give **CS0433** on `PolyglotProgram`, `Option<T>` and
  `None<T>`.
- **Further C# gaps:**
  - An empty `static class PolyglotProgram {}` is always emitted.
  - Prelude helpers such as `print` become public API under `access: public`.
- **PHP has the same collision** for user code and the prelude ("Cannot redeclare"). Python has none.
- The P38 "`sharesPreludeFile`" follow-up is **already done** (P39: `sharedPreludeFile`/`preludePerFile`,
  `backend.hpp:124-139`, `compiler.cpp:683`). The Core has no target-name literals today.
- The LSP preview ignores `access` (`main.cpp:1194`).

**Spikes run by the investigator:**

| # | Question | Result |
|---|---|---|
| S1 | Emitted C# inside a namespace | 98 programs, each in its own namespace, compiled into one assembly with **0 errors** |
| S2 | Emitted PHP inside a namespace | **102/102** identical output |
| S3 | Non-exported TS `Option` under declaration emit | passes, including `isolatedDeclarations` |
| S4 | Two same-named `PolyglotExtensions` classes | extension calls still compile |

## B2 — TS prelude dedupe

- TS, Python and PHP paste the **whole** prelude, exported, into every file. With `lib: io` a barrel also gets
  TS2308 on `print`, `readText`, `writeText`, `appendText`, `fileExists` and `deleteFile`.
- Unused `std.io` pulls `process` into an Angular bundle.
- **Nothing tracks references.** The prelude is tagged only by `originModule == "<prelude>"`.
- Pruning per root breaks the shared-prelude contract: every root must emit an identical
  `__polyglot_prelude.cs` (`main.cpp:196-215`).
- **The acceptance test as worded cannot pass.** 112 of 114 conformance programs export `main`, so a barrel
  over all of them collides on user names. `run-library.ps1:92-97` also fails any non-exported top-level
  declaration, so it must check only user declarations.

## B3 — Ordinal string std

- `compiler.cpp:162-179` holds only empty `extension fn string.*` declarations.
- The bindings live in the manifests:

| Target | Lines |
|---|---|
| C# | `:3705-3714` |
| TS | `:4537-4546` |
| Python | `:3815-3824` |
| PHP | `:205-214` |

- `injectStdOverlays` (`compiler.cpp:489-551`) attaches them after sema.

**Out-of-range behaviour diverges on all four targets.** On `"abc"`:

| call | C# | TS | Python | PHP 8.5 |
|---|---|---|---|---|
| `charAt(3)` | throws | `""` | throws | `""` |
| `charAt(-1)` | throws | `""` | `"c"` | `"c"` |
| `codePointAt(3)` | throws | `undefined` (the template's `!` hides it) | throws | `0` + a "Deprecated: ord()" line on **stdout** |

`codePointAt` on a low surrogate: C# throws, TS returns the unit.

**C# culture-sensitive overloads confirmed** (en-US):
- `"Å".StartsWith("Å")` is true.
- `"abc".IndexOf("\0")` is 0.
- `IndexOf`, `StartsWith` and `EndsWith` need `StringComparison.Ordinal`.
- `Contains`, `Replace(string,string)` and `Split(string)` are already ordinal.

**Native edge behaviour:**

| Operation | C# | JS | Python | PHP |
|---|---|---|---|---|
| `indexOf(n, from > len)` | throws | clamps | -1 | ValueError |
| `replaceAll("", r)` | throws | `xaxbxcx` | `xaxbxcx` | unchanged |
| `replaceAll(n, "$$")` | literal | **`$`** | literal | literal |
| `split("")` | `["abc"]` | `[a,b,c]` | ValueError | ValueError |
| `trim()` | Unicode ws | + U+FEFF | + **U+001C–U+001F** (strips GS1's FNC1 separator GS) | includes NUL, not `\f` |
| substring out of range | throws | swaps/clamps | clamps | clamps |
| `fromCodePoint(0xD800)` | throws | allowed | allowed | `mb_chr` absent locally |

PHP `<=>` compares numeric strings numerically (`"1e1" <=> "10"` is 0), so `compareOrdinal` must use `strcmp`.

**Limits of the binding templates:**
- `$this`/`$N` re-emit the argument expression each time (`emitter_base.cpp:1780-1792`), so inline guards
  would double-evaluate.
- Bindings cannot `require` a helper. `require` exists only in the rule DSL (`emitter_base.cpp:465`).

**Spikes run by the investigator:**
- **S1 — pass.** A guard written once in `.pg` over unchecked per-target primitives gave identical output
  **and exception messages** on all four targets.
- **S2 — pass.** `extern class string { static fn fromCodePoint }` already works.
- **S4 — fail.** Extension default parameters are accepted at declaration and ignored at the call
  ("expects 2 argument(s), got 1").

## B4 — `Map<K,V>`, and the generic-member-typing blocker

**Blocker (spike S5 failed on all four targets).** Methods on a generic class are not typed with the
instance's type arguments, for user and extern classes alike.
- `Cell<string>.first(): V` stays `V`: TS emits `V.plus(f, "!")` (ReferenceError) and PHP emits `$f + "!"`
  (TypeError).
- `Box<i32>.get(): T?` becomes `Option<T>`, which fails C# with CS0246.
- **`polyglot build` exits 0 in every case.** This is a pre-existing silent miscompile.
- The fix is to substitute class type parameters from the receiver type in sema's member typing, the way
  `inferResult` already does for generic functions.

**Spike S3 — fail.** Bounds on a generic **class** are never checked (`class Box<T: INumber>` accepts
`Box<string>`). They are checked only on functions (`sema.cpp:217-222`, `:2061`).

**Native semantics:**
- **Insertion order** after `set a,b; remove a; set c; set a; set b` is `b,c,a` on `OrderedDictionary`, JS
  `Map`, Python `dict` and PHP arrays. Plain C# `Dictionary` gives `c,b,a` (it reuses the freed slot).
- **Mutation during iteration:** C# throws, JS sees the new key, Python raises RuntimeError, PHP iterates a
  copy.
- **PHP keys are a silent miscompile.**
  - `$a["1"]` and `$a[1]` are the same slot, and `"10"` comes back from `array_keys` as the int 10.
  - `\ArrayObject` coerces the same way.
  - With no `strict_types`, `k == "10"` (emitted `===`) is silently false.
- **TS:** `bigint` keys compare by value in a `Map`.
- **C#:** two `get` calls in one statement both declare `out var __pgv`, giving CS0136.
- **C# runtime floor:** `OrderedDictionary<K,V>` needs .NET 9+. The oracle compiles against the active SDK
  major (`scripts/lib/OracleCompile.ps1:33-43`): net10 in CI, net11 locally. No consumer minimum is documented.

## Cross-cutting finding — one helper and reachability mechanism, needed by six items

A3 (`__PgRt` stringify), A4 (null-assert helpers), B1 (prelude inside a namespace, emitted only when
referenced), B2 (non-exported per-file prelude), B3 (PHP `fromCodePoint` encoder, private primitives) and B4
(PHP key encoding) each independently asked for the same three missing capabilities:
1. a binding-level `requires` (bindings can pull in a helper, not only rules);
2. **prelude placement** that is not "top of file" (C# needs end of file or after the usings; PHP needs after
   `<?php`);
3. a **reachability pass**, so only referenced prelude declarations are emitted, with the shared C# prelude
   built from the union of the roots.

`PRD.md` §0 designs it once. It is the critical path of the plan.
