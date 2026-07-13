# Explicit typing, arrays & union composition — implementation plan

Companion to [PRD.md](./PRD.md). Slices are self-contained and independently gated. Discipline (per
project convention): **byte-gate** every slice — diff the emitted C#/TS/Python/PHP for the full existing
conformance + samples corpus before vs after; the only allowed output changes are the ones the slice
intends. Run `scripts/build-and-test.ps1` (or the `/build-and-test` skill) at the end of every slice.

Build: **VS 18 Insiders MSBuild (v145)** — see root `CLAUDE.md`. Differential stages need `dotnet` + `node`
(Node 22+/24 for `.ts` type-stripping); TS acceptance additionally runs `tsc --strict --noImplicitAny`.

Status: **Slices 0–3 built + gated** on branch `fix/issue-27-...` (unit suite green; full differential
C#/TS/Python conformance green, incl. the new `arrays` program; strict-`tsc` clean). `T[]` arrays are in
scope (maintainer decision) and the whole change resolves under **issue #27 in one PR**. Slice 4 (docs +
release) below is the remaining wrap-up.

---

## Slice 0 — Union element parenthesization on TS (issue #27 Bug 2) — ✅ DONE

**Status:** implemented on this branch (`fix/issue-27-...`).

**Change:** `plugins/typescript/polyglot-plugin.json` — the `Type` rule's `type.nullable` arm now emits
`("(", base, " | null)")` instead of `(base, " | null")`, so a nullable/union element composes correctly
inside the postfix `$0[]`: `List<Node?>` → `(Node | null)[]` (was `Node | null[]` = `Node | (null[])`).

**Regression:** `tests/MintPlayer.Polyglot.Tests/src/tests_main.cpp` — the `issue#27 Bug2` block asserts
`next: (Node | null)[];`, absence of `Node | null[]`, no stray parens on non-nullable arrays
(`[number, number][]`), and C# `List<Node?>` self-delimiting.

**Gate:** unit suite green; byte-gate shows the only output delta is `… | null` → `(… | null)` inside
nullable collection/element positions. ✅

**Version-visible:** none yet (bundled into the 0.4.0 release, Slice 4).

---

## Slice 1 — Require an explicit type on an un-inferable initializer (sema, Core-only) — ✅ DONE

**Change:** `src/MintPlayer.Polyglot.Core/src/sema.cpp` — in `checkStmt` for `StmtKind::Let`
(`~822-827`) and the module-`let` path (`~293-296,551-553`): when `!s.hasDeclType`, after computing the
bottom-up `init` type, test `isUnknownOrContainsUnknown(init)` and, if true, emit an error diagnostic
(binding name + fixit suggestion) instead of silently declaring the unknown type.
- Add a small predicate `bool typeIsFullyKnown(const TypeRef&)` (recursive: name non-empty for Named, all
  `args` known, `ret`/tuple members known; a `nullable` empty-name is *not* known) next to `tUnknown()`
  (`sema.cpp:12-19`). This is the single definition of "inferable".
- The un-inferable forms it catches: empty `[]` (`List<unknown>`), bare `null` (nullable-unknown), a bare
  lambda with unknown params + no target. Inferable forms (`[1,2,3]`, `List<i32>()`, typed exprs) are
  `fullyKnown` → unchanged.
- Message shape: `cannot infer the type of '<name>' from its initializer; add an explicit type annotation`
  with a context-specific hint (`e.g. var <name>: List<T> = []` for an empty list; `… : T? = null` for
  null). Diagnostic only — no lowering/emit change here.

**Regression:** new conformance/negative programs —
- `tests/conformance/programs/…` cannot host a *rejected* program (they must compile); add **unit
  `rejects(...)`** cases in `tests_main.cpp` (mirroring the existing `issue#9 Bug1` `rejects` block):
  `var d = []` rejected; `var x = null` rejected; a contextless bare-lambda local rejected; and **accepts**
  `var d: List<i32> = []`, `let e = [1,2,3]`, `var d = List<i32>()`.

**Gate:** the existing corpus stays green (blast-radius = 0 — every existing `[]`/`null` decl is already
annotated; PRD §2). If any corpus program newly errors, it was relying on the silent fallback — record and
annotate it. Full `build-and-test.ps1` green.

**Version-visible:** `polyglot check` now reports the new diagnostic.

---

## Slice 2 — Emit the declared type for un-inferable initializers (backends) — ✅ DONE

**Why after Slice 1:** the source now *guarantees* an annotation wherever the init is un-inferable, so the
emitter can rely on `l.type` being concrete for these cases and print it — the idiomatic replacement for
issue #27's reverted `[] as $T`.

**Change:**
- `src/MintPlayer.Polyglot.Core/src/emitter_base.cpp` — broaden the `StmtKind::Let` gate (`~1453-1469`):
  fire `localDeclTyped` not only for `ir::ExprKind::Null` but whenever the initializer is un-inferable
  (bare null **or** an empty collection literal / empty-collection construction). Keep the "row absent →
  fall back to `localDecl`" behavior so untyped targets are byte-unchanged.
- `plugins/typescript/polyglot-plugin.json` — re-add the `localDeclTyped` row (`let $x: $T` /
  `const $x: $T`). This time it is *driven by the source-guaranteed annotation*, producing idiomatic
  `let d: number[] = []` and `let x: (Node | null) = null` — **no `as`-cast**.
- C# already has the row; verify its output is unchanged for empty-list locals (it infers from
  `new List<…>{}` today, so the added explicit type is redundant-but-harmless — byte-gate will show the
  delta; decide keep-or-suppress to minimize churn).
- The `type.nameEmpty → object|unknown` fallbacks in the plugins become unreachable for locals; leave them
  as defensive (add a comment pointing at this rule).

**Regression:** a conformance program `explicit_empty_local.pg` — `var d: List<(i32,i32)> = []` +
`d.add((1,2))` + a `Node?` list — asserted to (a) build under `dotnet` (C#) and (b) type-check under
`tsc --strict --noImplicitAny` (extend `run-diff.ps1` / add a strict-tsc assertion), reproducing the
original issue #27 program with a clean result on both targets.

**Gate:** byte-gate — the only TS deltas are un-inferable locals gaining `: T` (and the `List<T?>` parens
from Slice 0). Strict-`tsc` clean. `build-and-test.ps1` green.

**Version-visible:** TS output for annotated un-inferable locals now carries the type.

---

## Slice 3 — `T[]` fixed-size array type alongside `List<T>` — ✅ DONE

**Independently schedulable** (PRD §3.B). Defer if shipping the rule + invariant first.

**Change:**
- `docs/lang/grammar.ebnf` — add postfix array to the type grammar:
  `union_free_type` gains `{ "[" , "]" }` (composes before the trailing `?`), so `i32[]`, `Node?[]`,
  `(i32,i32)[]` parse.
- `src/MintPlayer.Polyglot.Core/src/parser.cpp` (`parseType`/`parseTypeCore`, `~460-495`) — parse the
  postfix `[]` into a `TypeRef` for a fixed-size array (a distinct kind or a `Named "Array"<T>` marker —
  choose to keep the IR uniform with `List`).
- `src/MintPlayer.Polyglot.Core/src/sema.cpp` — type an array literal/coercion; **forbid size-mutation**
  (`.add`/`.removeAt`/`clear`/`removeAll`) on an array (bindings exist only on `List`); index get/set and
  `.count`/`.length` allowed. A list literal annotated `: T[]` coerces to the array type.
- Plugins — add the array type/init mappings so **both array and list erase to a JS array on TS** and C#
  keeps them distinct:
  | | `Array.type` | `Array.init` |
  |---|---|---|
  | csharp | `$0[]` | `new $0[] { … }` / `System.Array.Empty<$0>()` for empty |
  | typescript | `$0[]` | `[]` (JS array — same runtime as `List`) |
  | python | `list` | `[]` |
  | php | `array` | `[]` |
  The **§3.C invariant applies to the TS array too** (postfix `$0[]`): `Node?[]` → `(Node | null)[]`.

**Regression:** `array_vs_list.pg` — an `i32[]` and a `List<i32>` side by side; assert C# emits `int[]`
vs `List<int>`, TS emits `number[]` for both, and a `.add` on the array is **rejected** (unit `rejects`).
Plus a `Node?[]` array asserting `(Node | null)[]` on TS.

**Gate:** byte-gate (existing programs unaffected — no program uses `T[]` yet); build-and-test green;
strict-tsc clean.

**Version-visible:** new `T[]` spelling in the language.

---

## Slice 4 — Docs + release wrap-up

**Change:**
- `docs/lang/SPEC.md` — ✅ §4.1 now states the un-inferable-requires-annotation rule (with the
  `var d: List<i32> = []` / `var x: Node? = null` examples) and documents the `T[]` array type, the
  array-vs-list target-mapping table, and the union-in-collection rendering guarantee.
- `docs/lang/grammar.ebnf` — ✅ `type` production carries the postfix `{ "?" | "[" "]" }` modifiers.
- **No version bump in-tree.** Versioning is lockstep + tag-driven (P24): committed sources are
  `0.0.0-dev` placeholders. The **PR carries the `release:minor` label** (language feature); the merge→tag
  automation cuts the lockstep minor across CLI + NuGet + extension + 4 plugins. Root `CLAUDE.md` status
  line gets a light one-line touch (features/rules only, per P24 D6).
- `docs/prd/PLAN.md` — append a `## P<next> — explicit typing, arrays & union composition` milestone
  retrospective (root cause, systemic fix, +1 conformance program `arrays`), house style.
- Folder is `docs/prd/issue-27-explicit-typing/`; the pointer in `tests_main.cpp` matches.

**Gate:** ✅ full `scripts/build-and-test.ps1` (build → unit → differential C#/TS/Python) green incl. the
new `arrays` program; strict-`tsc` clean on the array + repro outputs.

**Close-out:** branch → single PR to `master` (folds design + all slices, per the maintainer's one-PR
preference) with `release:minor` → comment on issue #27 linking the root-cause fix and the new spec section.

---

## Summary of touch-points

| Slice | Area | Files | Version-visible |
|---|---|---|---|
| 0 ✅ | Union parens (TS) | `plugins/typescript/polyglot-plugin.json`; `tests_main.cpp` | (in 0.4.0) |
| 1 ✅ | Source rule (diagnostic) | `sema.cpp`; `tests_main.cpp` (`rejects`) | new `check` diagnostic |
| 2 ✅ | Emit declared type | `emitter_base.cpp`; `plugins/typescript/…`; new conformance prog | TS locals carry type |
| 3 ✅ | `T[]` array type | `grammar.ebnf`; `parser.cpp`; `sema.cpp`; all 4 plugins; new progs | `T[]` spelling |
| 4 | Docs + release | `SPEC.md`; `grammar.ebnf`; version files; `docs/prd/PLAN.md`; `CLAUDE.md` | 0.4.0 |
