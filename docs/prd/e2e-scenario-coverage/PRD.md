# E2E codegen-scenario coverage expansion (PRD)

> **GitHub:** no originating issue — maintainer request (2026-07-18): *"add more e2e tests to see for
> several scenarios if the correct code is being generated"*, as its own pull request following the
> issue-batch PR (`docs/prd/issue-29-36-batch/`).

- **Status:** Draft v1.0 · 2026-07-18 · test-only change — no compiler/plugin behavior deltas intended;
  the PR carries the default **patch** release label (or none, if the release automation skips test-only
  changes).
- **Author:** Pieterjan (with Claude Code).
- **Provenance:** a read-only coverage investigation mapping the 58 existing
  `tests/conformance/programs/*.pg` against the SPEC §3.A surface, feature by feature.

---

## 1. Problem

The differential conformance suite is the project's strongest correctness instrument — every
`programs/*.pg` runs on **all four targets** (C# oracle ↔ TS / Python / PHP) and stdout must match byte
for byte. But its 58 programs grew bottom-up out of milestones and hotfixes, so coverage follows *where
bugs already happened*, not the language surface. The gap analysis found **~20 §3.A features with no (or
only incidental) runtime-differential scenario** — including at least one class of *genuinely divergent
per-target defaults* that nothing pins today:

- **Negative integer division/modulo** — C# truncates, Python floors (`//`/`%`), JS truncates with its
  own `%` sign rule, PHP `intdiv` truncates. `int_overflow.pg` tests only positive operands. If the
  emitters get this wrong, it is a silent cross-target divergence of exactly the kind the PRD §3
  contract forbids — and the suite would stay green. *(The Python plugin already carries `_pg_idiv`
  helpers for this — untested at runtime.)*
- **Logical `&&`/`||` short-circuiting** — zero programs; Python emits `and`/`or`, PHP/JS native.
  Side-effect-observing short-circuit order is untested.
- **`catch` with a `when` guard, rethrow-on-unmatched-type** — spec'd, round-trip-tested, never run.
- **Indexers (`operator fn get`/`set`), property setters, the full operator-overload set**
  (comparisons, bitwise) — only `plus` and read-only computed properties execute today.
- Plus: `do…while`, isolated `break`/`continue`, char literals + escapes, explicit enum values, nested
  match patterns, user generic bounds (`<T: Comparable<T>>`), tuples-in-isolation, `List.removeAt`,
  `tryParse`, std.io file round-trip, import aliasing, compound assignment.

## 2. The change

Add **runtime-differential scenario programs** (auto-discovered by all three runner legs — no manifest)
covering every gap, ordered by divergence risk. Each program:

- prints **integers/strings only** (§3.D float-determinism trap avoided; any float-adjacent scenario
  reuses `math_transcendental.pg`'s scale-and-truncate quantization),
- stays **ASCII** in string content (PHP's string std is byte-oriented — the documented caveat),
- carries a header comment naming the scenario and the per-target divergence it pins,
- is small and single-purpose (one feature per program, house style).

Where a scenario surfaces a real divergence (a target printing different bytes), the fix belongs to that
target's plugin/Core **in this same PR** if small and obvious, else it becomes a filed issue with the
program temporarily narrowed to the passing subset (never a silent xfail — the narrowing is a `// TODO(#N)`
naming the issue).

## 3. Scenario list (priority order)

| # | Program | Pins |
|---|---|---|
| 1 | `neg_divmod.pg` | `-7/2`, `-7%2`, `7/-2`, `7%-2`, i64 variants — trunc-toward-zero semantics on every target (Python `_pg_idiv`/`_pg_irem`, TS BigInt leg) |
| 2 | `logical_shortcircuit.pg` | `&&`/`||` evaluation order + short-circuit (side-effecting fn calls counted) |
| 3 | `catch_when.pg` | `catch (e: T) when (cond)` guard falls through; unmatched type rethrows to outer catch |
| 4 | `indexer_grid.pg` | `operator fn get`/`set` two-arg indexer read/write on every target |
| 5 | `property_setter.pg` | `var` property with get/set accessor block |
| 6 | `operators_full.pg` | overloaded `==`, `<`, `<=`, `>`, `>=`, `-`, `*`, bitwise on a record |
| 7 | `char_escapes.pg` | char literals, `\n`/`\t`/quote escapes, `codePoints()` iteration, string `==` |
| 8 | `do_while.pg` + `loop_control.pg` | do…while executes-at-least-once; `break`/`continue` in for/while |
| 9 | `generic_bound.pg` | `<T: Comparable<T>>` user bound + dispatch through the bound's method |
| 10 | `enum_values.pg` | explicit enum values (`Ok = 200`), value gaps, match on them |
| 11 | `match_nested.pg` | multi-arg + nested ctor patterns, literal patterns, guards |
| 12 | `tuples.pg` | tuple construction, member access, `for (a, b) in` destructuring |
| 13 | ~~`tryparse.pg`~~ | dropped — `tryParse` is documented (SPEC §7) but not implemented in any plugin; that is a *std gap*, not a test gap (out of scope §4; candidate follow-up issue) |
| 14 | `file_io.pg` | std.io write → append → read → exists → delete round-trip (temp-relative path) — authoring it exposed that **Python had no std.io file arms at all** (C#/TS/PHP did); the five arms are added in this PR as an existing-surface parity fill |
| 15 | `import_alias.pg` (module dir) | namespace/aliased imports across files |
| 16 | `compound_assign.pg` | `+=`/`-=`/`*=`/`/=` on locals, fields, indexed slots |
| 17 | `collections.pg` (extend) | add the missing `removeAt` calls |

## 4. Non-goals

- No new language/std surface — a gap in the *language* (e.g. missing string methods) is out of scope;
  this PR only exercises what SPEC already promises.
- No golden-file snapshots — the project's instrument is runtime output equality plus targeted
  `has(code, …)` unit asserts; this PR adds programs, not a snapshot mechanism.
- No float-parity assertions beyond the existing quantization pattern (§3.D).

## 5. Acceptance criteria

1. Every scenario program passes on all three differential legs (C#↔TS, C#↔Python, C#↔PHP) — or its
   divergence is fixed in-PR / filed + narrowed with a `TODO(#N)`.
2. The suite grows from 58 to ~75 programs; runtimes stay tolerable (each program is O(ms)).
3. Any compiler/plugin fixes made along the way carry their own `issue#`-style unit asserts.
4. `scripts/build-and-test.ps1` fully green, run once at the end.

---

*Implementation plan: see [PLAN.md](./PLAN.md).*
