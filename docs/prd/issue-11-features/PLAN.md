# PLAN — Issue #11 (release 0.3.0)

Byte-gated slices, ordered easiest→hardest so the codebase stays green after every slice. **Byte gate** =
emit every corpus program to every target and diff against the pre-change output; the only permitted changes
are the intended ones (single-module programs must be byte-identical). **Stdout gate** = `run-diff.ps1` /
`run-python.ps1` agree cross-target. Build with the VS 18 Insiders MSBuild (see CLAUDE.md); one-shot gate is
`pwsh scripts/build-and-test.ps1`.

Snapshot the baseline before starting: emit all `tests/conformance/programs/**` to cs/ts/py into a
`baseline/` dir; each slice's byte gate diffs against it.

---

## Slice 1 — `i32()` TS/PHP float→int cast emits plain truncation (issue 1.C)
**Files:** `plugins/typescript/polyglot-plugin.json` (Cast `fromIsFloat` branch, ~685-709);
`plugins/php/polyglot-plugin.json` (Cast `fromIsFloat` branch, ~546-580).
- TS: replace the `{"fn":"wrap", args:[node.type, {"tmpl":["Math.trunc(", {emit:operand}, ")"]}]}` with the
  bare `{"tmpl":["Math.trunc(", {"emit":"node.operand"}, ")"]}`.
- PHP: replace the wrapped `(int)(operand)` with the bare `{"tmpl":["(int)(", {"emit":"node.operand"}, ")"]}`.
- Leave the `wrapInt` tables, the Cast *else* (int→int) branch, unary, and arithmetic-overflow sites alone.
- New conformance program `tests/conformance/programs/cast_float_int.pg`: casts small in-range floats to
  every int width (`i8..u32`) and prints them; agrees C#/TS/Python (values chosen in-range so all agree).
**Gate:** byte diff shows the change *only* on float→int cast lines; `run-diff` + `run-python` green.

---

## Slice 2 — transcendental `std.math` tier (issue 1.A)
**Files:** `src/MintPlayer.Polyglot.Core/src/compiler.cpp` (`STD_MATH` skeleton, 68-96, + the header
comment 62-67); `plugins/{csharp,typescript,python}/polyglot-plugin.json` (`"std.math"` overlay sub-block);
`plugins/php/polyglot-plugin.json` (net-new `"std.math"` sub-block under `std`).
- Skeleton: add `static fn` decls for `sin cos tan asin acos atan atan2 sinh cosh tanh exp log log2 log10
  pow cbrt trunc` (all `f64`→`f64`; `atan2`/`pow` two-arg), plus `log` as the conventional natural-log name
  alongside the existing `ln`. Extend the header comment to state the new members are the best-effort
  (≤1-ULP, tolerance-only) tier per §3.D.
- Overlays (plain bound statics, `$0`/`$1` = args): C# `global::System.Math.Sin($0)` …
  `Atan2($0,$1)`/`Pow($0,$1)`/`Log2`/`Log10`/`Cbrt`/`Truncate`; TS `Math.sin($0)` … `atan2($0,$1)`/
  `pow($0,$1)`/`log2`/`log10`/`cbrt`/`trunc`; Python `__import__('math').sin($0)` … `atan2($0,$1)`/
  `pow($0,$1)`/`log2`/`log10`/`cbrt`/`float(__import__('math').trunc($0))`; PHP `sin($0)` … `atan2($0,$1)`/
  `pow($0,$1)`/`log($0,2)`/`log10($0)`/`... cbrt` `(float)(int)$0` for `trunc`.
- `sign`: attempt clean 1:1 arms (see PRD §3.A note). If any target needs emulation beyond a trivial
  expression, **drop `sign` from v1** and note it — do not emulate.
- New conformance program `tests/conformance/programs/math_transcendental.pg`: computes `cos(3·x)`, `tanh`,
  `exp`, `log`, `pow`, etc., and prints each **quantized** (e.g. `Math.round(v * 1000000.0) / 1000000.0`, or
  format to N decimals) so C#/TS/Python stdout is byte-identical despite ≤1-ULP drift. This is the tolerance
  gate expressed as quantized equality.
**Gate:** existing programs byte-identical (additive-only); new program green on `run-diff` + `run-python`;
PHP emission spot-checked (no php runtime here — inspection, per the existing PHP-differential TODO).

---

## Slice 3 — module linking, plumbing (Core; **byte-identical output**)
No emission change in this slice — it only threads the data Slice 4 consumes. Output stays inlined, byte-identical.
- **3a — origin tag.** Add `std::string originModule` to each top-level AST decl (empty = entry-own).
  `mergeDecls` (`compiler.cpp:227`) stamps merged user-module decls with the source module's canonical id;
  `linkCoreModule` (298) / `linkLibModules` (336) / std-module merges in `loadImports` stamp `"<prelude>"`.
  (This is the "per-file import-scope table" the deferred P12-phase-2 work always needed.)
- **3b — import capture.** During `loadImports`, record per importer (`""` = entry) the list of resolved
  **user** imports `{ importedCanon, names:[{name,alias}] }` (std/lib specifiers filtered — prelude). Surface
  this map out of `runFrontEnd`. The entry's own imports are already on `root.imports`.
- **No IR per-decl field.** Emission partitions by a **name→origin map** built from the tagged AST (ir decls
  carry `.name`; functions share a name+origin; extensions key on receiver+name). `ir::Module` gains only
  `std::vector<ModuleImport> imports` + `bool linked` (set per-partition in Slice 4).
**Gate:** full-corpus byte diff = **zero** changes.

---

## Slice 4 — module linking, split-for-emit (Core + engine + 4 plugins; the behavior change)
Lower the whole merged unit **once** (the `Lowerer` needs the full type universe). Then in `compile()`
partition the one `ir::Module` by name→origin into per-module files. **Linked mode is active only when the
closure has >1 module** — single-module programs take the exact current path and stay byte-identical.
- **4a — `EmitResult.modules`.** `struct ModuleFile { std::string basename; std::string code; }` +
  `std::vector<ModuleFile> modules` on `EmitResult` (`polyglot.hpp:57`). For single-module programs, one entry
  == `code`. Byte-identical.
- **4b — partition + per-target prelude policy.** For each origin O (entry + each user-module canon), build a
  filtered `ir::Module` = decls whose name→origin is O, **plus the full `externTypes` registry** (type
  spelling must still resolve), plus O's `imports` (from Slice 3b, mapped to basenames), `linked=true`. Prelude
  policy:
  - **C#:** prelude-origin decls go into the **entry** partition only; `PolyglotProgram`/`PolyglotExtensions`
    emit `partial` when `linked`; types are global-namespace (no `using`). No import statements.
  - **TS/Python/PHP:** prelude-origin decls are included in **every** partition (per-file inline; distinct
    module scopes, no collision).
  Add an engine primitive to iterate `module.imports`; add each plugin's `Program`-rule preamble:
  - typescript: `import { $name (as $alias)?, … } from "./$basename";`
  - python: `from $basename import $name (as $alias)?, …`
  - php: `require_once __DIR__ . '/$basename.php';`
  - csharp: none. C# `Program` rule emits `static partial class` (vs `static class`) gated on `module.linked`.
  `EmitResult.code` = entry partition; `modules` = every partition ({basename, code}).
- **4c — CLI writes the set + multi-input roots/dedup.** `emitOne` (`main.cpp:236`) writes each
  `result.modules` entry to `<outDir>/<basename><ext>`. For **multiple** inputs, the CLI builds the input
  set's import graph, compiles only **roots** (inputs not imported by another input), and dedups emitted
  modules by basename (identical content; conflict = error) — so an imported-only library `.pg` is emitted as
  part of its importer's closure, never also as its own entry.
- **v1 limitations (documented):** flat output (`<basename><ext>`) → **unique basenames required**; **one C#
  entry (with `main`) per assembly** (prelude lives there). Subdir mirroring + multi-entry C# are follow-ups.
**Gate:** **single-module** programs byte-identical; `modular` stdout byte-identical cross-target via multiple
files; the new `library` program compiles into one C# assembly (no CS0101) and imports cleanly in TS/Python.

---

## Slice 5 — conformance harness + MSBuild rework, and the CS0101 regression witness
- **New program** `tests/conformance/programs/library/` = the issue-#11 shape: `nn.pg` (`class PgThing` +
  `fn twice`) + `entry.pg` (`import { twice, PgThing } from "./nn"`), prints a value.
- **`run-diff.ps1` / `run-python.ps1`:** for a multi-file program dir, emit the whole module set (the CLI writes
  all closure files from the entry), compile **all** generated `.cs` into one assembly and run it (proves no
  CS0101), and run the entry `.ts`/`.py` with siblings present. Keep the issue-#9 hardening (C# compiled + both
  runtimes exit 0). Stdout stays byte-identical to 0.2.0.
- **MSBuild** (`src/MintPlayer.Polyglot.MSBuild/build/MintPlayer.Polyglot.MSBuild.targets`): invoke the CLI
  **once** over all `@(PolyglotFile)` (roots/dedup handled CLI-side) so each module emits once; add the deduped
  generated set to `@(Compile)`. Extend `tests/msbuild/run-nuget.ps1` with a two-`.pg`-sharing-a-library
  fixture that must build into one assembly with no CS0101.
**Gate:** `run-diff` + `run-python` green (incl. `modular` + `library`); `run-nuget` green incl. the fixture.

---

## Slice 6 — versioning, docs, relaxation list
- `kVersion` → `0.3.0` (`polyglot.hpp:20`); NuGet `<Version>0.3.0</Version>`
  (`MintPlayer.Polyglot.MSBuild.csproj`); plugins → csharp `0.3.0`, typescript `0.3.0`, python `0.3.0`,
  php `0.3.0` (`plugins/*/package.json` + each manifest's version field if present).
- PRD §3.D relaxation list: add the transcendental best-effort (≤1-ULP, tolerance-only) tier line.
- PRD §4.5 / PLAN P12: mark "cross-module reference emission (proper linking)" **done** in 0.3.0; note the
  merge-for-sema/split-for-emit model and that selective-import visibility + `as` rebinding are now unblocked
  (the origin/import-scope table exists).
- `CLAUDE.md`: bump the `--version` comment to 0.3.0; add a "Release 0.3.0" status paragraph
  (transcendental std.math tier, proper module linking, faithful i32() TS/PHP cast).
- `docs/prd/PLAN.md`: append a `## Release 0.3.0` retrospective.
**Gate:** `scripts/build-and-test.ps1` fully green.

---

## Test-coverage additions (unit + conformance)
- Unit (`tests/MintPlayer.Polyglot.Tests/src/tests_main.cpp`): `Math.cos`/`tanh`/`pow` type-check &
  emit on cs/ts/py; a cross-module `import` produces a reference (no re-definition) in the importer's output
  and populates `EmitResult.modules` with >1 entry; a duplicate type across modules still **refuses**
  (collision error, not a miscompile); `i32(floatExpr)` TS output contains `Math.trunc` and **not** `| 0`.
- Conformance: `cast_float_int.pg`, `math_transcendental.pg`, `library/` (added above); `modular` reused as
  the transitive-linking witness.

## Risks / watch-items
- **TS ESM cross-file run in the harness.** `node <entry>.ts` must resolve the emitted relative import
  specifier (extension/module-resolution). Pin the emitted specifier form in Slice 4b and verify the harness
  runs the entry with siblings on disk. Fallback: emit an explicit extension if bare specifiers don't resolve.
- **Relative-path basenames** (`geom/vec`) — preserve subdir structure under `--out` and in the import path.
- **`sign` cleanliness** — drop from v1 if it can't stay a 1:1 binding (PRD §3.A).
- **Prelude top-level defs** — Slice 4d is the correctness backstop; verify empirically before assuming empty.
