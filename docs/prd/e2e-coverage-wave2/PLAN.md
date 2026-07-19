# E2E coverage wave 2 — implementation plan

Companion to [PRD.md](./PRD.md); gap/defect IDs (G*, D*) resolve in [ANALYSIS.md](./ANALYSIS.md).

**PR shaping (maintainer decision 2026-07-19): ONE branch, ONE PR** — `e2e-coverage-wave2` carries
all slices 1–7 (programs + fixes, harness + legs + robustness, code coverage). Slice 0 (issue filing)
precedes the branch work and is not itself part of the PR diff.

Discipline per CLAUDE.md: implement all slices first, cheap unit-exe/single-program sanity while
authoring, the **full `scripts/build-and-test.ps1` gate ONCE at the end**. The wave touches runner
scripts / CI / (possibly) platform-forked code — the end gate includes **one POSIX compile+unit run**
(WSL cmake or the PR's ci.yml leg). Registry-leg caveat: known-flaky loopback on
this machine (MEMORY.md) — a local registry-gate failure on an untouched leg is environmental; confirm
via ci.yml, don't chase it.

House rules for every program (unchanged from wave 1): auto-discovered, integer/string output only,
ASCII strings, one scenario per file, header comment naming the pinned divergence. Where a scenario
surfaces a divergence: smallest principled fix in-PR with `issue#`-style unit asserts, else file +
narrow with `TODO(#N)` — never a silent xfail.

---

## Slice 0 — re-verify + file the new defects (D1–D8)

For each of D1–D8: reproduce the probe locally (transpile + run the minimal program on the affected
targets), then file the GitHub issue with the repro `.pg`, observed-vs-expected per target, and the
acceptance-program name from ANALYSIS.md §4. D7 (member overloading) is filed as a §3.A **scope
decision** question, not a bug; D8 (finalizer refusal) as a missing-§3.B-diagnostic. If a probe fails
to reproduce, record that in ANALYSIS.md and drop the filing — skeptic-verified is not
maintainer-verified. Update the wave-1 narrowed-program TODOs if any new issue supersedes an old one.

## Slice 1 — live-divergence tier: comparisons & collections (G1, G2, G7, G10)

| Program | Fix riding along | Watch for |
|---|---|---|
| `string_eq_numeric.pg`, `null_falsy.pg`, `class_identity_eq.pg` | PHP plugin: `==`/`!=` → `===`/`!==` cutover | `"10"=="1e1"`, `x != null` with `x`=0/`""`, distinct equal-field instances — PHP prints wrong bools today |
| `list_rebind_alias.pg` | TS/Py `clear`/`removeAll` → in-place (`.length=0`/splice, `del xs[:]`) | mutation must stay visible through aliases + fn params (C#/PHP already in-place) |
| `codepoints_values.pg` | C# `EnumerateRunes()` → Rune→string projection | element *values* (print/compare/concat) — only the count is observed today; C# emits invalid code on first value use |
| `equality_composite.pg` | fix-or-§3.C-relaxation if divergent | record `==` with List/string/union fields; class-instance `==` is identity (SPEC §427) |

## Slice 2 — numeric semantics & boundaries (G3–G6, G8, G9, G12, G13, G20, G21, G24)

Programs: `i64_literal_widen.pg`, `shift_wrap.pg`, `float_mod.pg`, `round_half.pg`,
`i64_boundaries.pg`, `int_boundaries.pg`, `parse_widths.pg`, `f32.pg`; extend `int64.pg` (comparisons
past 2^53) and `math.pg` (`Math.E` via `q()`, negative floor/ceil/round, float sign/clamp, unsigned
min/max/clamp). Fixes riding along: TS exact-BigInt literal emission, Py/PHP/TS shift mask/wrap arms,
Py/PHP `fmod` arms, TS/PHP round-half shims.

**Spec decisions this slice carries** (each recorded in SPEC.md §3.C/§3.D, never silent):
1. `Math.round` half mode (presumably ToEven — the C#/Py oracle behavior).
2. PHP i64 wrap: GMP-free shim vs §3.B refusal (PHP float-promotes today).
3. Float→int out-of-range cast: restore .NET-mirrored masking (SPEC §73–74) or refuse (issue #11
   removed the TS `|0`).
4. `INT_MIN / -1`: .NET crashes, JS yields a value — pick and pin.
5. Scalar-parse out-of-range/garbage semantics — *recorded as a follow-up decision*, `parse_widths.pg`
   pins in-range values only.

Author the `.expected` golden files for the semantics-defining programs here (mechanism lands in
slice 4; the files are inert until then).

## Slice 3 — composition & stdlib pins, pass-today (G11, G14–G19, G22, G23, G25, G26)

Pure programs, no compiler changes expected: `disposal_throw.pg`, `async_compose.pg`, `union_tree.pg`
(expression arms only — D5 blocks block-bodied arms), `extension_generic.pg`, `with_list_alias.pg`
(avoid reserved field name `tag`), `union_i64.pg` (expression-position match — D6), `file_io_edges.pg`,
`strings_edges.pg`, `closure_this.pg`, `scale.pg` (~1000-line generated module printing checksums;
commit the generator script next to it). G11 (uninitialized fields) starts as a decision: checker
"needs initializer" diagnostic (define the error out of existence — preferred) → unit test in slice 6;
per-target defaults → program here.

## Slice 4 — harness hardening + gate wiring (G27, G28, G30, G31, G32, G39, G41)

- `build-and-test.ps1`: wire `tests/msbuild/run-nuget.ps1` as a dotnet-guarded stage; add a
  CLI-surface smoke stage (`--version` shape, `-h` exit 0, unknown flag exit 64, missing input).
- All three conformance runners: per-program timeout (WaitForExit + kill → `[FAIL] timed out`) with
  one self-test; loud cleanup failures + per-run unique temp roots; `<name>.expected` golden-stdout
  support (fallback: pairwise as today).
- `run-php.ps1`: pin the expected-refusal set (`vec2, async_await, expect_actual, extern_ffi`) —
  any other refusal fails, an expected refuser that starts passing warns.
- One four-target single-invocation build asserting byte-equality vs per-target runs.

## Slice 5 — new CLI legs + gate extensions (G29, G33–G38, G40)

- **`tests/refusals/run-refusals.ps1`** (new leg): §3.B fixtures (decimal/Thread/lock/unsafe/dynamic/
  Expression, await-outside-async, un-annotated `[]`/`null`) → nonzero exit, `path:line:col: error:`
  stderr shape, **no output file written**, stale pre-seeded twin untouched; `polyglot check`
  good/broken; `check --json` parsed field-by-field; `--json --watch` → exit 64; a 3-error fixture
  asserting all three diagnostics (plus the matching `diagnostics.size() > 1` unit assert); a BOM'd
  hello-world (pairs with G43).
- **`tests/lsp/run-lsp.ps1`** (new leg, the L-sized item — may split into its own commit):
  Content-Length-framed stdio session — initialize/positionEncoding, didOpen→publishDiagnostics,
  fix→empty, hover/def/rename incl. a non-ASCII line for UTF-16 columns.
- Registry-gate extensions: bare `polyglot build` include discovery (match + no-match refusal),
  `polyglot install` local-dir + corrupted-manifest + pre-warm-then-offline-build,
  `%(RecursiveDir)`/`%(Directory)`/`%(TargetLanguage)` through a real nested tree to disk.
- Watch-gate cycles 9–11: delete watched file / recreate / delete pgconfig.json.
- Resolver unit tests (fake transport): stale-lock re-resolve, offline refusal, `update=true`.
- ci.yml: add watch + registry legs (pwsh/node preinstalled on ubuntu runners) — this is the
  `#ifdef`-forked code that broke every P30 POSIX leg.

## Slice 6 — front-end robustness units + small fixes (G42–G48)

`std::stoll` guard (huge enum value currently **aborts the process**) + negative UT; BOM tolerance;
unterminated `/*` at EOF → diagnostic (today silently swallows the rest of the file); parser/emitter
recursion-depth guard pinned with generated 20k-paren / 10k-if-chain sources; `Activator` refusal row
in the P6 refuses() table; non-ASCII literal → documented §3.C entry + diagnostic; non-ASCII
identifier → one position-correct diagnostic (explicit ASCII range check, not `std::isalpha`).

## Slice 7 — code coverage (PR-C)

1. **ci.yml `coverage` job** (parallel to `linux-build`, same path filters): Debug CMake build with
   `--coverage`; run `polyglot-tests`; run the smoke transpile + a handful of conformance programs
   via the instrumented CLI so emitter paths accumulate `.gcda`; `gcovr --filter src/`
   → `$GITHUB_STEP_SUMMARY` table + html-details artifact. Report-only; no threshold until a
   baseline exists (then add `--fail-under-line` as a follow-up).
2. **`scripts/coverage.ps1`**: OpenCppCoverage (`--cover_children --sources src\`) over
   `MintPlayer.Polyglot.Tests.exe` (+ optionally one conformance leg) → HTML report under
   `x64/coverage/`; degrades to a clear install hint (`choco install opencppcoverage`) when absent.
3. **Template-arm tracer (demand-gated):** `--emit-arm-trace` debug flag recording fired manifest
   arms; a script aggregates a conformance run into per-plugin fired/total. If plumbing proves
   invasive, file the follow-up issue and stop — no hacks.
4. README/CLAUDE.md note: what each instrument measures — gcov/OpenCppCoverage = Core C++ paths;
   conformance suite + arm tracer = plugin JSON behavior. Neither substitutes for the other.

## Gate & log

Per PR: full `scripts/build-and-test.ps1` once at the end (+ the POSIX leg for PR-B/PR-C). Append the
milestone log to `docs/prd/PLAN.md` (house style) per PR: programs added, divergences caught, spec
decisions recorded, issues filed. Log divergences found while authoring in the section below.

---

## Log

**2026-07-19 — Slice 0 done.** All eight defects re-verified with local repros (C#/TS/Python, plus
PHP where relevant) and filed: D1→#46, D2→#47, D2b→#48 (split out of D2 — PHP-only, different fix),
D3→#49, D4→#50, D5→#51, D6→#52, D7→#53, D8→#54. Two probe claims corrected during verification
(recorded in ANALYSIS.md §4): D1's PHP leg is per-creation capture (`1/11/21`, like TS), and D3's TS
prints `0/0`. Baseline health re-confirmed the same day: unit suite green, all 71 conformance
programs agree C#↔TS.

**2026-07-19 — Slices 1–7 implemented (one branch, one PR).** Authoring the programs surfaced
**five MORE defects beyond the analysis** — each fixed in-wave or filed:

- **f32.pg caught a general paren-dropping miscompile**: a textless identity cast (f32→f64 on the
  dynamic targets) swallowed its operand's precedence parens — `(a + b) * 100` emitted `a + b * 100`
  on TS/Python/PHP (26 vs C#'s 175). Fixed in `emitChild` (precedence judged through Cast nodes).
- **scale.pg caught a compiler stack overflow**: `p1 + … + p32` (32-term sum!) killed the Debug
  Python/PHP emit with 0xC00000FD and empty stderr. Fixed three ways (G45 pulled forward): parser
  nesting guard (256), emitter depth guard (400, clean error), 16 MB stack reserve (vcxproj + CMake
  MSVC/macOS), plus a CLI-wide exception boundary (also de-fangs G42's stoll abort).
- **i64_boundaries.pg caught Python's missing narrowing casts**: `(i32)` of an i64 emitted nothing
  (printed the unwrapped value). Fixed: Python Cast int→smaller-int / 64↔64 wrap arms.
- **shift authoring caught the C# shift-count rule**: operand reconciliation widened counts
  (`long << long` is invalid C#). Fixed in sema: shift counts stay i32, result takes the shiftee's
  type (the C# rule, all targets).
- **union probe confirmed TS `==` on unions is reference equality** (0/0 vs C#/Py 1/0) → filed #57;
  plus #55 (PHP built-in name collisions: `fn explode`), #56 (unbound member calls emit verbatim —
  found twice: missing import, and generic-instantiated lambda params).
- **null_falsy.pg caught the C# dropped-annotation family live** (`var n: i32? = 0` → `var n = 0`,
  CS0037 on `n = null`). Fixed generally: a declared type that differs from the initializer's own
  type always emits (also the principled fix for #47's second half).

Spec decisions recorded in SPEC.md §9: round half-to-even everywhere; shift count masking + i32
counts; float `%` = fmod; record-field equality (strict leaves, reference lists); PHP 64-bit-wrap
caveat; non-ASCII string caveat (+ lexer warning); `INT_MIN / -1`, out-of-range float→int, and
out-of-range parse documented as §3.D. G11 resolved as the checker diagnostic ("field never
initialized"), G8's ±2⁶³ half as the PHP caveat. Program count 71 → **87** (+16), plus 9 `.expected`
goldens closing the correlated-wrongness blind spot. New gate legs: cli-smoke, refusals, LSP;
run-nuget wired in; runners hardened (timeouts, unique temp roots, loud cleanup, pinned PHP refuser
set: async_await, async_compose, expect_actual, extern_ffi, indexer_grid, operators_full, vec2).
Coverage: ci.yml gcovr job (report-only) + scripts/coverage.ps1 (OpenCppCoverage); the plugin
template-arm tracer stays demand-gated (follow-up candidate).
