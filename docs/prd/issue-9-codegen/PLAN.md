# Issue #9 codegen hotfix — implementation plan (release 0.2.0)

Companion to [PRD.md](./PRD.md). Five slices, each self-contained and independently gated. Discipline
(per project convention): **byte-gate** every slice — diff the emitted C#/TS/Python for the full existing
41-program corpus before vs after; the only allowed changes are the ones the slice intends. Run
`scripts/build-and-test.ps1` at the end of every slice.

Build: VS 18 Insiders MSBuild (v145) — see root `CLAUDE.md`. The one-shot gate is `/build-and-test`.

Suggested order rationale: **gate hardening first** (slice 1) so that the regression programs added in the
bug slices actually *bite* (a symmetric double-failure would otherwise still false-pass). Bugs then land
parser → sema → emitter (independent; any order works). Release/version bump last.

---

## Slice 1 — Harden the differential gates (infra, no compiler change)

**Why first:** without this, the new regression programs in slices 2–4 could pass even while broken (Bug 1
breaks *both* targets → empty == empty → false pass). This is the systemic finding and the highest-leverage
edit.

**Changes:**
- `tests/conformance/run-diff.ps1` (the discard-outcome hole ≈ lines 46–63): after `dotnet build`, **fail**
  the program if `$LASTEXITCODE -ne 0` or the output `.dll` is absent; after `node <ts>` and
  `dotnet <dll>`, **fail** if the exit code is non-zero. Stop redirecting the build/run outcome to `$null`
  for the purpose of the check (keep quiet logging, but assert success).
- `tests/conformance/run-python.ps1`: same treatment — assert the C# build and the `python`/`dotnet` runs
  exit 0, not just that stdouts match.
- `scripts/build-and-test.ps1`: wire in `run-python.ps1` and `tests/samples/run-emit.ps1` (both currently
  not orchestrated), so Python parity and per-sample compile+run are part of the one-shot gate.

**Gate:** the *existing* corpus must stay green under the stricter checks (proves no program was silently
relying on a discarded failure). If any program newly fails, that is a pre-existing latent bug to record and
fix — investigate before proceeding. Full `build-and-test.ps1` green.

**Version-visible:** none (scripts only).

---

## Slice 2 — Bug 3: 2+ nested-generic params parse (parser, Core-only)

**Change:** `src/MintPlayer.Polyglot.Core/src/parser.cpp` — guard the generic-arg comma loop so it cannot
consume a comma while an outer-generic close is pending:
- line 472 (`parseTypeCore`): `do { t.args.push_back(parseType()); } while (pendingAngles_ == 0 &&
  accept(TokKind::Comma));`
- line 884 (generic call/construction `Name<…>(…)`, same latent bug in `foo<List<List<i32>>, X>()`):
  `do { call->typeArgs.push_back(parseType()); } while (pendingAngles_ == 0 && accept(TokKind::Comma));`

The guard is semantically exact: `pendingAngles_ > 0` means "a borrowed `>` must close this level now",
mutually exclusive with "another comma-separated arg follows". No lexer change, no counter reset.

**Regression:** `tests/conformance/programs/nested_generics.pg` — a class/ctor with **two** `List<List<f64>>`
params (plus fields), populated and summed so it prints a stable scalar all three targets agree on.

**Gate:** byte-gate (no existing emission changes — pure parser acceptance); the new program transpiles,
compiles (C#), and runs (TS/Python). Full `build-and-test.ps1` green.

---

## Slice 3 — Bug 1: `T(expr)` primitive cast (sema, Core-only)

**Change:** `src/MintPlayer.Polyglot.Core/src/sema.cpp` `checkCall`, in the `Name`-callee branch
(after ~line 1264, **before** the `types_` construction branch at 1280): when
`isNumericTypeName(tNamed(name))` (`sema.cpp:26`):
- require exactly one argument — else a clear diagnostic (arg-count);
- reuse `checkCast`'s source-type validation (reject bool/string source) — else a cast-appropriate
  diagnostic;
- rewrite the node in place to a cast (the established `coerce`/`wrapSome` in-place-rewrite pattern):
  `e.kind = ExprKind::Cast; e.castType = tNamed(name); e.lhs = std::move(e.args[0]); e.args.clear();`
  and return the target type.

Args are already checked at the top of `checkCall`, so the moved operand keeps its resolved type. Lowering's
existing `ExprKind::Cast` case (`lower.cpp:419`) and the plugins' `Cast` rules then handle everything —
**no lower.cpp change, no plugin change**. Edge cases (i64/u64 BigInt, i8..u32 narrowing masks, same-type
identity) are handled for free by the existing Cast rules.

**Regression:** `tests/conformance/programs/casts_call.pg` — `i32(f64)`, `f64(i32)`, a narrowing width, an
i64 boundary; mirrors `casts.pg` but in call syntax; prints stdout the three targets agree on. (Optionally
extend an existing unit test to assert `i32()`/`i32(a,b)`/`i32(true)` now diagnose.)

**Gate:** byte-gate (no existing program uses call-casts, so no existing emission changes); the new program
compiles (C#) + runs (TS/Python) + matches C# oracle. Full `build-and-test.ps1` green.

---

## Slice 4 — Bug 2: `var x: T? = null` keeps its C# type (Core + all 4 plugins)

**Core change:** `src/MintPlayer.Polyglot.Core/src/emitter_base.cpp` `Let` case (≈1393-1396): when
`l.init->kind == ir::ExprKind::Null` **and** `renderType(l.type)` is non-empty, route through a new
`localDeclTyped(name, isMutable, renderedType)` helper (a two-hole `$T`/`$x` substitution — add the
two-hole variant alongside the existing single-hole `specSubst`, `backend_spec.hpp:149-172`). Otherwise
unchanged. The non-empty-type guard keeps degenerate/annotation-less cases on current behavior.

**Plugin data** — add a `localDeclTyped` table row to the **csharp** plugin only:
`{"mutable":"$T $x","const":"$T $x"}` → `Box? best`. TS/Python/PHP declare no such row; `specSubstTX`
returns `""` for them and the Core `Let` case falls back to the unchanged untyped `localDecl`, so their
output is byte-identical (verified). This also fixes `var x: i32? = null` → `int? x = null`.

**Regression:** `tests/conformance/programs/null_local.pg` — `var x: T? = null` for a reference type **and**
a value type (`i32?`), later assigned and read, printing a stable scalar. It must pass the **hardened**
`run-diff` (C# build succeeds) and ideally be added to the `run-nullable.ps1` fixture set (compiles clean
under `<Nullable>enable</>;<TreatWarningsAsErrors>`).

**Gate:** byte-gate — the **only** allowed emission changes corpus-wide are null-initialized annotated C#
locals; TS/Python/PHP byte-identical. Full `build-and-test.ps1` green.

---

## Slice 5 — Release: version bump + docs + final gate

**Versions** (per PRD §6):
- `src/MintPlayer.Polyglot.Core/include/mintplayer/polyglot/polyglot.hpp` `kVersion`: `0.1.4` → `0.2.0`.
- `src/MintPlayer.Polyglot.MSBuild/MintPlayer.Polyglot.MSBuild.csproj` `<Version>`: `0.1.4` → `0.2.0`.
- `plugins/csharp/package.json` `0.2.1` → `0.2.2` only. (Bug 2's `localDeclTyped` row is C#-only;
  ts/py/php output is byte-unchanged, so they don't bump.)

**Docs:**
- Append a `## Release 0.2.0` retrospective to `docs/prd/PLAN.md` (matching the 0.1.1/0.1.3/0.1.4 style):
  the three root causes, the gate-hardening systemic fix, the conformance-program count delta (+3), and the
  version table.
- Update `CLAUDE.md` status: bump the `--version` expectation from `0.1.4` to `0.1.5`, and add a one-line
  Hotfix 0.1.5 note to the roadmap tail.
- Update the conformance program count wherever it's asserted (`--version`/status lines say "38"/"41"
  historically; re-count after adding the 3 programs and adjust any hardcoded totals in the gate scripts).

**Gate:** full `scripts/build-and-test.ps1` green, including the hardened `run-diff`/`run-python`,
`run-nullable`, `run-library`, and (newly wired) `run-emit`. Confirm `--version` prints `0.1.5`.

**Close-out:** commit on a branch, PR to `master`, comment on issue #9 with the fixes + the new regression
programs, referencing that all three are now covered by a gate that compiles the C# and runs the TS/Python.

---

## Summary of touch-points

| Slice | Bug | Files | Version-visible |
|---|---|---|---|
| 1 | gate | `run-diff.ps1`, `run-python.ps1`, `build-and-test.ps1` | — |
| 2 | Bug 3 | `parser.cpp` (2 lines) + `nested_generics.pg` | Core |
| 3 | Bug 1 | `sema.cpp` (`checkCall`) + `casts_call.pg` | Core |
| 4 | Bug 2 | `emitter_base.cpp` + `emitter_base.hpp` + `backend_spec.hpp` + csharp `polyglot-plugin.json` + `null_local.pg` | Core + csharp plugin |
| 5 | release | `polyglot.hpp`, `.csproj`, csharp `package.json`, `PLAN.md`, `CLAUDE.md` | Core + NuGet + csharp plugin |
