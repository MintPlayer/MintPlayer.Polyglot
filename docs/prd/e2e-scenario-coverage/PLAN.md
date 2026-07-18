# E2E codegen-scenario coverage — implementation plan

Companion to [PRD.md](./PRD.md). One branch, **one PR** (test-only unless a scenario surfaces a real
divergence — then the smallest principled fix rides along with its own `issue#`-style unit asserts).
Discipline: programs are added in the priority order below; **no intermediary full gates** — write all
programs, run each once through a quick local three-target sanity (`polyglot build … --target X` + run)
while authoring, then the full `scripts/build-and-test.ps1` gate ONCE at the end.

House rules for every program (from the infra survey + PRD §2): auto-discovered (no manifest), integer/
string output only, ASCII strings, one feature per file, header comment naming the pinned divergence.

---

## Slice 1 — Divergence-risk tier (the ones most likely to catch a real bug)

| Program | Contents | Watch for |
|---|---|---|
| `neg_divmod.pg` | `-7/2 · -7%2 · 7/-2 · 7%-2 · -8/2 · i64` variants, printed | Python `//`/`%` floor vs C# trunc (the `_pg_idiv`/`_pg_irem` preludes exist but are runtime-untested); TS BigInt ops for i64; PHP `intdiv` |
| `logical_shortcircuit.pg` | side-effect counter fns; `f() && g()` / `f() \|\| g()` matrices; print call counts + results | Python `and`/`or` value semantics; evaluation order |
| `catch_when.pg` | typed catch with `when` guard true/false; unmatched type rethrown to outer try; finally ordering | PHP/Python guard emulation (guard falls through to next catch) |
| `indexer_grid.pg` | class with 2-arg `operator fn get`/`set`; read+write via `g[x, y]` | targets without native indexers (TS `.get()`/`.set()` methods, PHP/Python) |
| `property_setter.pg` | `var` property with accessor block; set then get | PHP property emulation (`properties: emulated`) |

## Slice 2 — Operator/OO tier

- `operators_full.pg` — record overloading `==`, `<`, `<=`, `>`, `>=`, binary `-`, `*`; print comparison
  results. (PHP declares `operatorOverloading: false` — the run-php leg counts the refusal as an
  expected pass; assert the diagnostic path stays clean.)
- `generic_bound.pg` — `interface Comparable<T>` + `fn maxOf<T: Comparable<T>>` (SPEC §4.4's own example,
  never executed end-to-end) — now also exercises the P31 conformance checker on all four targets.
- `enum_values.pg` — `enum HttpCode { Ok = 200, NotFound = 404 }`, implicit-after-explicit continuation,
  match on cases, print numeric values via cast-free comparisons.

## Slice 3 — Control-flow & pattern tier

- `do_while.pg` — executes-at-least-once semantics, condition-false first iteration.
- `loop_control.pg` — `break`/`continue` in `for`/`while`, nested-loop break scope.
- `match_nested.pg` — multi-arg ctor patterns, nested `Some(Pair(a, b))`, literal patterns, guards.
- `tuples.pg` — construction, `.0`-style member access (per SPEC spelling), `for (a, b) in` destructure.
- `compound_assign.pg` — `+= -= *= /=` on locals, fields, and indexed slots (`xs[i] += 1`).
- `char_escapes.pg` — char literals, `\n`/`\t`/`\"` escapes in strings + interpolation, `codePoints()`
  iteration counts, string `==`. ASCII only.

## Slice 4 — Std-surface tier

- `collections.pg` — extend with the missing `removeAt` mid-list call (index shift asserted).
- ~~`tryparse.pg`~~ — dropped: `tryParse` is SPEC-documented but unimplemented in every plugin (a std
  gap, out of scope here; candidate follow-up issue).
- `file_io.pg` — writeText → appendText → readText → fileExists → deleteFile → fileExists round-trip in
  the working dir (`conformance` runners execute per-temp-dir, so a relative filename is isolated).
  Exposed a parity hole: the Python plugin had NO std.io file arms — the five are added in this PR
  (`pathlib.Path.read_text/write_text`, `open(…, 'a').write`, `os.path.exists`, `os.remove`).
- `import_alias.pg/` (module dir) — namespace import `import * as geo` (or SPEC's alias spelling) across
  two files; call through the alias.

## Slice 5 — Docs + gate + PR

- If any scenario caught a divergence: fix (or file + `TODO(#N)`-narrow), with unit asserts, and record
  each in this PLAN's log section below.
- `docs/prd/PLAN.md`: append the milestone log (house style) — programs added, divergences caught.
- **One full gate:** `pwsh scripts/build-and-test.ps1`.
- PR: test-only → default patch label; body lists each program and what it pins.

## Log

Authoring the 16 scenarios surfaced **seven real defects/gaps** (2026-07-18) — exactly the point of
this PR. Filed and cross-referenced from the narrowed programs:

- **#39 — five grammar/SPEC-documented features the front-end doesn't implement:** `do…while`
  ("expected an expression"), `let (a, b) = t` tuple destructuring ("expected a binding name"),
  property accessor blocks with `set(v)` ("expected a member" — SPEC's own diameter example doesn't
  compile), guards on ctor patterns (`Pair(a, b) if …` — "expected '=>'"), and namespace/renaming
  imports (`* as geo`, `{ area as flat }` — parse, never resolve). `do_while.pg`,
  `property_setter.pg`, and `import_alias/` were REMOVED (nothing implementable to pin);
  `tuples.pg`/`match_nested.pg` narrowed.
- **#40 — `operator fn set` emits invalid C#** (`static void operator set(...)`; the get/set pair is
  never merged into a C# indexer declaration). `indexer_grid.pg` narrowed to the read half.
- **#41 — C# CS0136:** reusing a for-binding's name for a later method-scope local emits invalid C#
  (valid .pg). `loop_control.pg` sidesteps with a distinct name and points at the issue.
- **#42 — multi-arg index READ drops every argument after the first on ALL targets**
  (`g[1, 1]` → C# `g[1]`, TS `g.get(1)` — silent on the dynamic targets). `indexer_grid.pg`
  narrowed to one index argument.
- **#43 — literal sub-patterns inside ctor patterns silently dropped** (`Leaf(0)` lowers to
  match-any-`Leaf`, stealing the next arm on TS/Python/PHP; loud arity errors on C#).
  `match_nested.pg` narrowed to binder-only ctor arms.
- **Python std.io parity hole** (fixed in-PR): the plugin had no `readText`/`writeText`/`appendText`/
  `fileExists`/`deleteFile` arms at all — added; `file_io.pg` runs them on all four targets.
- **`tryParse`** is SPEC-documented but unimplemented in every plugin — dropped from the scenario
  list as a std gap (candidate future issue), not a test gap.

Net program count: 58 → **71** (13 added: `neg_divmod`, `logical_shortcircuit`, `catch_when`,
`indexer_grid`, `operators_full`, `char_escapes`, `loop_control`, `generic_bound`, `enum_values`,
`match_nested`, `tuples`, `file_io`, `compound_assign`) + `collections.pg` extended with `removeAt`.
