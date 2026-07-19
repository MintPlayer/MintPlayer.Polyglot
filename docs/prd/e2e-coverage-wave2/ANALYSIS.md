> **Provenance:** synthesis output of a 13-agent coverage-analysis workflow (2026-07-19): six dimension
> analysts (feature interactions, std-lib surface, refusals/faithfulness, toolchain, per-target divergence,
> robustness/regression), each independently re-verified by a skeptic agent against the repo, then merged.
> Gap IDs G1-G48 and defect IDs D1-D8 are referenced by [PRD.md](./PRD.md) and [PLAN.md](./PLAN.md).

# Consolidated e2e coverage analysis — synthesis of six verified dimension reports

## Executive summary

Single-feature §3.A coverage is now strong (71 programs post-PR #45), the watch/registry protocol gates are exemplary, and refusals are thoroughly unit-tested in-process. The real risk concentrates in three places: **(1) live silent miscompiles the verification probes proved today** — PHP's loose `==` family (numeric strings, `!= null` on 0/`""`, class identity), TS/Python `List.clear/removeAll` receiver-rebinding through aliases, TS `BigInt(number)` precision loss on annotation-typed i64 literals, unmasked shifts and float `%` on Python/PHP, and four-way `Math.round` half-value modes — all invisible because the suite never touches the divergent inputs; **(2) feature-composition holes**, where probes found five *new* unfiled compiler defects (missing C# `virtual`/`override`, loop-closure capture divergence, TS member-generator missing `*`, `operator fn eq` breaking C# and mis-routing on TS, block-bodied match arms silently lowering to `return 0` on all four targets); and **(3) shipped toolchain surfaces with zero automated execution** — `tests/msbuild/run-nuget.ps1` is wired into no gate (verified: absent from `scripts/build-and-test.ps1` and all workflows), `polyglot lsp` has no protocol-level test, and `run-php.ps1` counts *any* capability refusal as a pass, so one plugin JSON edit silently converts coverage to green. The C#-oracle-only differential design also has a correlated-wrongness blind spot (no `.expected` golden for semantics-defining programs).

---

## 1. Confirmed gaps (ordered: silent-divergence risk first, then cheapness)

Leg key: **CP** = conformance program in `tests/conformance/programs/` (auto-discovered by all three runners) · **RX** = extension of an existing runner · **NL** = new runner leg wired into `scripts/build-and-test.ps1` · **UT** = unit test in `tests_main.cpp`.

### Tier 1 — live silent divergence today (program + in-wave fix, PR #45 pattern)

| ID | Pins | Leg | Size |
|---|---|---|---|
| G1 | `string_eq_numeric.pg` + `null_falsy.pg` + `class_identity_eq.pg` — PHP loose `==` cutover to `===`/`!==`: `"10"=="1e1"` false, `x != null` true for 0/`""`, distinct equal-field class instances unequal (probes: PHP wrong bool on all three today) | CP ×3 + PHP plugin fix | M |
| G2 | `list_rebind_alias.pg` — `clear()`/`removeAll()` through an alias and a fn parameter stay shared-visible (TS/Py `$this = []` rebinding diverges from C#/PHP mutation today; fix: in-place splice/`del[:]` templates) | CP + TS/Py plugin fix | S |
| G3 | `i64_literal_widen.pg` — annotation-typed i64 literal (`let a: i64 = 9007199254740993`) + mixed i32/i64 widening emit exact BigInt, not `BigInt(<rounded number>)` (TS diverges today) | CP + TS fix | S |
| G4 | `shift_wrap.pg` — i32 shift count masking (`1 << 33`), result wrap (`INT_MIN << 1`), i64 `<< 65`, u32 `>>`/`<<` past 2^31 (Py/PHP emit bare shifts, TS u32 `>>` reinterprets signed — all diverge today; fix: wrap-arm extension + `& 31/63` + `>>>`) | CP + Py/PHP/TS fix | M |
| G5 | `float_mod.pg` — `-7.5 % 2.0` sign semantics, results scaled to ints (Python floors, PHP int-casts + deprecation notice today; fix: `math.fmod`/`fmod()` arms) | CP + Py/PHP fix | S |
| G6 | `round_half.pg` — `Math.round(2.5/3.5/-2.5/-0.5)`: one SPEC'd mode (presumably ToEven, the oracle's) on all four targets (three distinct modes live today: C#/Py ToEven, TS half-up, PHP half-away) | CP + SPEC decision + TS/PHP shims | M |
| G7 | `codepoints_values.pg` — codePoints() element values printed/compared/concatenated (only the count is observed today; C# `EnumerateRunes()` yields Rune vs declared `Iterable<string>` → invalid C# on first value use; fix: Rune→string projection) | CP + C# fix | S |
| G8 | `i64_boundaries.pg` — wrap at ±2^63, u64 at 2^64, i64→i32 narrowing of 2^32/2^31 (TS `asIntN`/Py mask arms never fire on a wrapping input; PHP has **no i64 wrap arm at all** — float-promotes; needs a PHP shim-or-§3.B-refusal decision, either pinned by this program) | CP + PHP decision/fix | M |
| G9 | `int_boundaries.pg` — i32 wrap at INT_MAX+1 / INT_MIN−1 / unary −INT_MIN / INT_MAX×2; separately decide + pin `INT_MIN / -1` (live .NET-crash-vs-JS-value divergence today) | CP (+ decision for the /-1 case) | S |
| G10 | `equality_composite.pg` — record `==` with List/string/union-typed fields (TS generated equals routes non-records to `===`, C# uses per-field default equality — union fields plausibly diverge; agreement on list fields is unpinned coincidence), `==`/`!=` on class instances (identity per SPEC §427) | CP (fix-or-relaxation if divergent) | M |
| G11 | Uninitialized class fields — `class C { var x: i32 }` + read: today 0 / undefined / AttributeError / null across the four targets with a clean check; decide (checker "needs initializer" diagnostic — define the error out of existence — or per-target default emission) and pin | UT or CP + decision | M |
| G12 | Float→int cast out of range — SPEC §73-74 promises .NET-mirrored masking but issue #11 removed the TS `\|0`; decide the contract (restore masking or refuse) and pin `(i32)2147483648.0`, `(i32)-3e9`, `(i16)70000.5` | CP or UT + decision | M |

### Tier 2 — never-executed surface / composition pins (pass today; pure regression pinning)

| ID | Pins | Leg | Size |
|---|---|---|---|
| G13 | `parse_widths.pg` — all 7 never-run scalar parse arms (i8/i16/u8/u16/u32/u64/f32 = 28 never-executed templates) on in-range values; leading-zero `"007"` (intval octal guard); record the out-of-range/garbage spec decision as a follow-up (TS wraps, Py/PHP pass through, C# throws) | CP | S |
| G14 | `disposal_throw.pg` — `use` dispose-before-catch ordering + nested-use LIFO unwind on the throw path (SPEC §5.2's "even on throw" promise, never executed; probe passes 4-way today) | CP | S |
| G15 | `async_compose.pg` — async class method throwing, try/catch around await inside a for loop, await in an if arm (PHP refusal counts as expected pass) | CP | S |
| G16 | `union_tree.pg` — recursive union with `List<Tree>` payload + recursive match sum — **expression arms + helper fn only** (block-bodied arms silently lower to `return 0` on all four targets — see defect D5) | CP | S |
| G17 | `extension_generic.pg` — SPEC §6.3's own `extension fn List<T>.second(): T?` + `totalWith((T)=>i32)` on List<i32>/List<string> with `?? -1` (probe passes 4-way) | CP | S |
| G18 | `with_list_alias.pg` — record-`with` copy shares (not deep-copies) a List field, observed via post-copy mutation incl. PHP ArrayObject (#36 path); avoid the reserved field name `tag` | CP | S |
| G19 | `union_i64.pg` — i64 through a generic union into a match **binder**, printed bare + in `${...}` (call-return inference is pinned by typeargs.pg; the pattern-binder substitution path isn't); keep match in expression position (see defect D6) | CP | S |
| G20 | Extend `int64.pg` — i64 `==`/`<`/`>` on values differing only past 2^53, mixed i64==widened-i32, i64-driven branch (a BigInt→Number comparison coercion would pass every current program) | CP (extend) | S |
| G21 | Math edges — `Math.E` via the existing `q()` quantizer (271828; zero references to E anywhere today), negative floor/ceil/round (−9.8→−10 …), float-typed sign/clamp (never instantiated), unsigned min/max/clamp (u8/u16/u32/u64) | CP (extend math.pg / sibling) | S |
| G22 | `file_io_edges.pg` — appendText creates a missing file, writeText truncates an existing one, `"a\nb"` round-trip pinned via `.len()==3` | CP | S |
| G23 | `strings_edges.pg` — `"-7".toI32()`, `"".len()`, last-index charAt, mixed-alnum toUpper/toLower, `"007".toI32()` | CP | S |
| G24 | `f32.pg` — exactly-representable f32 values through `+ − × ÷` with quantized prints: the subset that MUST agree under the published §3.C relaxation (only f32 use today is one round() call); the SPEC'd `Math.fround` opt-in flag doesn't exist — file as follow-up | CP | S |
| G25 | `closure_this.pg` — returned this-reading lambda + this-mutating block lambda invoked after the method exits (PHP $this binding / Python self capture; currently pinned only by emission-string asserts) | CP | S |
| G26 | `scale.pg` — deterministically generated ~1000-line module (100 fns, 32-param fn, 256-element list literal) printing checksums; coarse plugin-interpreter perf canary | CP | S |

### Tier 3 — harness & toolchain

| ID | Pins | Leg | Size |
|---|---|---|---|
| G27 | Wire `tests/msbuild/run-nuget.ps1` into `build-and-test.ps1` (dotnet-guarded stage) — its ~25 existing checks (incremental skip, non-transitivity, prelude hoisting, twin routing) currently run only by hand; optionally a release.yml consumer-install step for non-win-x64 RIDs | NL (existing script, needs wiring) | S |
| G28 | Pin `run-php.ps1`'s expected-refusal set (`vec2, async_await, expect_actual, extern_ffi`) — any other refusal FAILs, an expected refuser that starts passing warns (today one capability flip silently converts coverage to green passes) | RX | S |
| G29 | `tests/lsp/run-lsp.ps1` — scripted Content-Length-framed stdio session: initialize/positionEncoding negotiation, didOpen→publishDiagnostics, fix→empty, hover/def/rename incl. a non-ASCII line for UTF-16 columns (whole JSON-RPC server has zero protocol tests; two shipped clients depend on it) | NL | L |
| G30 | Per-program timeout (WaitForExit + kill → `[FAIL] timed out`) in the three conformance runners + one self-test — today a single miscompiled `while(true)` wedges the gate indefinitely, locally and on 5 release legs | RX | S |
| G31 | Loud workdir-cleanup failure + per-run unique temp root — today `Remove-Item -EA SilentlyContinue` into fixed shared paths; one stale `.ts` flips run-diff's multi-file import-rewrite (false-PASS against stale code possible) | RX | S |
| G32 | `<name>.expected` golden-stdout support in all three runners (fallback: pairwise) for semantics-defining programs (neg_divmod, round_half, shift_wrap, i64/int boundaries) — closes the C#-oracle correlated-wrongness blind spot (C#+Py already agree on banker's rounding regardless of intent) | RX | M |
| G33 | `tests/refusals/run-refusals.ps1` — CLI over §3.B fixtures (decimal/Thread/lock/unsafe/dynamic/Expression, await-outside-async, un-annotated `[]`/`null`): nonzero exit, `path:line:col: error:` stderr shape, **no output file written**, stale pre-seeded twin untouched; plus `polyglot check` good/broken, `check --json` parsed field-by-field, `--json --watch` → exit 64, a 3-error fixture asserting all three diagnostics distinct (also as UT: nothing anywhere asserts `diagnostics.size() > 1`), and a BOM'd hello-world | NL | M |
| G34 | Bare `polyglot build` include-pattern discovery — multi-file/subdir match emits everything, no-match refuses with guidance (`discoverIncludeInputs` referenced only by main.cpp; every test passes an explicit entry) | RX (registry gate) | S |
| G35 | `polyglot install` — local-directory install into POLYGLOT_CACHE + corrupted-manifest refusal; registry-gate pre-warm then offline first-build (documented offline workflow, zero invocations anywhere) | RX (registry gate) | M |
| G36 | Watch cycles 9–11 — delete a watched `.pg` (fail cycle, last-good preserved, watcher alive), recreate (green recovery), delete pgconfig.json (documented behavior, no hang) — save-via-rename is a routine editor write | RX (watch gate) | S |
| G37 | Stale-lock re-resolve + `update=true` — pin outside an edited range re-resolves (and refuses offline, never silently keeps the old plugin); update advances a satisfied pin (all six existing resolver tests pass update=false) | UT (fake transport) | S |
| G38 | POSIX legs for watch + registry gates in ci.yml (pwsh/node preinstalled; run-python joins release.yml's linux job) — the watcher/HTTP/cache code is exactly the `#ifdef`-forked code that broke every P30 POSIX leg; validate once against MEMORY.md's loopback caveat | RX (ci.yml) | M |
| G39 | One four-target single-invocation build (in-box plugins) with byte-equality vs per-target runs — N=2 is heavily exercised (run-diff's pgconfig is csharp+typescript); Py/PHP never join the same process | RX | S |
| G40 | `%(RecursiveDir)`/`%(Directory)`/`%(TargetLanguage)` driven through a real nested-tree build to disk (unit tests compose the paths; every e2e rule uses only `%(Filename)`) | RX (registry gate) | S |
| G41 | CLI surface smoke — `--version` semver shape, `-h` exit 0, unknown command/flag exit 64, missing input "cannot open" (locally unpinned; asserted only in release.yml) | RX (build-and-test stage) | S |

### Tier 4 — front-end robustness (unit tests; guard-then-pin where noted)

| ID | Pins | Leg | Size |
|---|---|---|---|
| G42 | Guard `parser.cpp:396`'s bare `std::stoll` (enum explicit value `> 2^63` currently **aborts the process** — no `catch(` anywhere in main.cpp) → diagnostic, pinned by a negative UT | UT + small fix | S |
| G43 | BOM handling — `"\xEF\xBB\xBF"`-prefixed source compiles clean (or one clear diagnostic), UT + the G33 e2e fixture (raw binary readFile, zero BOM handling in lexer.cpp today; Notepad/VS default) | UT | S |
| G44 | Trivia pins — empty/comment-only source, unterminated `/*` at EOF **diagnosed** (today silently swallows all code after it — miscompile-adjacent), `//` at EOF, non-nesting `/* /* */` | UT (+ small lexer fix) | S |
| G45 | Parser/emitter recursion-depth guard ("too deeply nested" diagnostic, not stack overflow) pinned with a 20k-paren / 10k-if-chain generated source — worst in the long-lived LSP process | UT + guard | M |
| G46 | `Activator` refusal row in the P6 refuses() table (sema.cpp:147 exists, untested — the exact degradation P6 guards against); note `Type.GetType` needs a sema addition first | UT | S |
| G47 | Non-ASCII string literal → documented §3.C relaxation entry + diagnostic (or warning), UT on a `"café"` literal (today PHP silently prints byte-len 5 vs 4 elsewhere; no runtime leg viable — no local mbstring) | UT + decision | S |
| G48 | Non-ASCII identifier → one position-correct diagnostic, not per-byte spam; explicit ASCII range check replacing `std::isalpha` (locale risk is theoretical — no setlocale anywhere — the UX pin is the value) | UT | S |

---

## 2. Implementation slices (each ≈ one commit)

**Slice 1 — Live-divergence fixes + programs: comparisons & collections** (G1, G2, G7, G10): PHP strict-comparison cutover, TS/Py in-place list templates, C# Rune projection, composite equality — every program lands *with* its plugin/Core fix, PR #45 style. No harness changes.

**Slice 2 — Numeric semantics & boundaries** (G3, G4, G5, G6, G8, G9, G12, G13, G20, G21, G24): the i64/i32/shift/mod/round/parse/f32/math-edge wave. Carries three small spec decisions (round-half mode, PHP i64 wrap shim-vs-refusal, float→int range, INT_MIN/-1) — record each in SPEC.md §3.C/§3.D as decided, never silently. Pairs naturally with G32's `.expected` files (authored here, mechanism from slice 4).

**Slice 3 — Composition & stdlib-edge pins (pass-today programs)** (G14–G19, G22, G23, G25, G26): pure conformance programs, no compiler changes, all probe-verified passing. Cheapest slice; also the G11 field-initialization decision if it resolves to a checker diagnostic (else it moves here as a program).

**Slice 4 — Harness hardening + gate wiring** (G27, G28, G30, G31, G32, G39, G41): **needs `scripts/build-and-test.ps1` edits** — wire run-nuget as a guarded stage, add the cli-surface smoke stage; runner edits for timeouts, loud cleanup, expected-refusal set, `.expected` support, the four-target leg.

**Slice 5 — New CLI legs + gate extensions** (G29, G33, G34, G35, G36, G37, G38, G40): **two NEW runner legs wired into build-and-test.ps1** (`tests/refusals/run-refusals.ps1`, `tests/lsp/run-lsp.ps1`) plus registry/watch-gate extensions, resolver unit tests, and the ci.yml POSIX additions. The largest slice; the LSP leg (L) may split out on its own.

**Slice 6 — Front-end robustness & refusal units** (G42–G48): in-process unit tests with their small guarded fixes (stoll, BOM, unterminated comment, depth guard, Activator row, non-ASCII diagnostics). No harness changes.

---

## 3. Blocked — on open issues (acceptance programs, land with the fix)

- **#39** (+ **#43**): un-narrow `match_nested.pg` — ctor-arm guards + `Leaf(0)` literal sub-pattern ordering (the #43 class is silently-wrong arm selection). **#39 alone**: pre-agreed acceptance programs `dowhile.pg`, `destructure.pg`, `prop_set.pg`, `import_forms.pg` so each front-end slice lands with runtime differential coverage, not just unit tests.
- **#40 + #42**: un-narrow `indexer_grid.pg` — write indexer via `r[2] = 41` + 2-D `operator fn get/set(x, y)` with per-coordinate values (a dropped second arg changes stdout).
- **#41**: un-narrow `loop_control.pg` — rename `k` back to `i` so run-diff's C#-must-compile assertion pins the CS0136 scoping fix.
- **#44**: un-narrow `catch_when.pg` — false when-guard falls to the next same-type catch arm (Py/PHP re-raise escapes the try today); also restore sample 06's shape.

## 4. Blocked — on NEW defects found during verification (each gap is its acceptance test)

All eight were **re-verified with local repros and filed on 2026-07-19** (slice 0). Observations below
are the *maintainer-verified* numbers — two details from the original probe reports were corrected:
D1's PHP output is `1/11/21` (by-value `use` capture at creation, like TS — not late-bind), and D3's
TS output is `0/0` (both comparisons route to structural `equals`).

- **D1 → [#46](https://github.com/MintPlayer/MintPlayer.Polyglot/issues/46)** — loop-closure capture divergence (blocks `closure_loop_capture.pg`): live three-way silent miscompile — C# 31/31/31 (shared for-var) vs TS/PHP 1/11/21 (per-iteration) vs Python 21/21/21 (late bind).
- **D2 → [#47](https://github.com/MintPlayer/MintPlayer.Polyglot/issues/47)** — C# emits no `virtual`/`override` → method hiding (C# 9/0 vs TS/Py/PHP 9/90), and `let a: Animal = Dog()` drops the annotation (`var a = new Dog()`). Blocks `virtual_dispatch.pg`. Silent miscompile of a headline §3.A feature; the csharp manifest has no modifier template at all.
- **D2b → [#48](https://github.com/MintPlayer/MintPlayer.Polyglot/issues/48)** — PHP fatal (`Cannot call constructor`) on `super()` to an implicit base ctor; C#/TS/Py fine. Split from D2 (different target, different fix).
- **D3 → [#49](https://github.com/MintPlayer/MintPlayer.Polyglot/issues/49)** — `operator fn eq`: C# CS0111 (user `operator ==` collides with the record's synthesized one — same family as #40) + TS routes `==` to structural `equals` instead of the user `eq` (TS 0/0 vs Python 1/0 — live silent divergence; PHP refuses as expected) (blocks `operator_eq.pg`).
- **D4 → [#50](https://github.com/MintPlayer/MintPlayer.Polyglot/issues/50)** — TS member generator missing `*` → invalid TS, node crash; top-level `function*` works (blocks `iterator_compose.pg`; loud, hence medium).
- **D5 → [#51](https://github.com/MintPlayer/MintPlayer.Polyglot/issues/51)** — block-bodied match arms silently lower to a default `0` on ALL FOUR targets (grammar-legal; binder bound, block discarded, `Circle(var r) => 0`). Nastiest of the batch: because run-diff is pairwise-only, the shape **false-passes while miscompiling**. Same silent-drop family as #43; poster child for G32's `.expected` goldens.
- **D6 → [#52](https://github.com/MintPlayer/MintPlayer.Polyglot/issues/52)** — match-as-statement with void arms emits a C# switch *expression* (`s switch {...};` → CS0201/CS0029); TS/Py/PHP run fine.
- **D7 → [#53](https://github.com/MintPlayer/MintPlayer.Polyglot/issues/53)** — member (method) overloading unimplemented — checker registers only the first same-named member (call-site arity error, declaration silently shadowed); §3.A scope decision (implement, or document top-level-only + declaration-site refusal), then `method_overloads.pg`.
- **D8 → [#54](https://github.com/MintPlayer/MintPlayer.Polyglot/issues/54)** — finalizer refusal missing — §3.B's only row with no targeted diagnostic (`~Foo(){}` dies as two raw parse errors); needs the sema/parser entry before its test.

## 5. Rejected (appendix)

- **crossdir-refusal-real-plugins** — already covered: `tests_main.cpp:2434-2436` pins the real manifests' crossDirImports flags + refusal control at 2480-2482, and the registry gate's pyfixture *is* the real python plugin renamed (byte-equality asserted).
- **crlf-mixed-line-endings-e2e** — already covered incidentally: `core.autocrlf=true`, no `.gitattributes` → the entire conformance/fidelity corpus runs CRLF through every local gate and LF on the ubuntu CI leg; a CRLF regression reddens the whole gate. (Residual lone-`\r` line-numbering sliver: cosmetic, folded conceptually into G44's trivia territory if desired.)
- **decimal-refusal-promises-absent-type** — out of scope for a test wave: SPEC/PRD document the fixed-point type as a *planned* P7 module, so the forward-referencing message is deliberate; remedy is docs or new std surface, not a test.

## 6. Merged duplicates

- `nuget-gate-not-wired` (robustness) ≡ `msbuild-nuget-gate-unwired` (toolchain) → **G27**.
- `polyglot-check-untested` (toolchain) ≡ `check-json-diagnostics-untested` (refusals) → folded into **G33**.
- `sub32-parse-range-divergence` (in-range half) ≡ `scalar-parse-width-arms-never-run` → **G13** (out-of-range spec decision kept as its noted follow-up).
- `ts-u32-shift-signed` folded into `py-php-shift-unwrapped` → one **G4** `shift_wrap.pg` (both analysts proposed the fold).
- `php-loose-string-equality` + `php-null-compare-falsy` + `php-class-identity-equality` → **G1**: three programs, one strict-comparison cutover.
- `php-i64-overflow-unwrapped` + `i64-2pow63-wrap-and-narrowing` → **G8** (same proposed program).
- `numeric-literal-limits-and-stoll-crash` split: boundary-literal program half merged into **G9**; the stoll process-abort kept as **G42**.
- `multi-error-output-unpinned` and `build-failure-output-hygiene` folded into the **G33** refusals leg (each keeps its unit-test half).