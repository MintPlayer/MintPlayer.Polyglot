# PLAN — Issue #14 (release 0.3.1)

Byte-gated slices. **Byte gate** = single-file + all non-C# output must stay byte-identical to 0.3.0 (the
split is C#-multi-input-only). **Regression gate** = a 2+-independent-`.pg` C# build compiles into one
assembly (no CS0101/CS8863) and runs. Build with VS 18 Insiders MSBuild; one-shot gate
`pwsh scripts/build-and-test.ps1`; NuGet gate `pwsh tests/msbuild/run-nuget.ps1`.

Snapshot the baseline first: emit every `tests/conformance/programs/**` to cs/ts/py; each slice diffs against it.

---

## Slice 1 — the failing regression test (red first)
Lock the bug before fixing it.
- **`tests/msbuild/run-nuget.ps1`** — add a fixture: a project with **two independent `.pg`** (no import
  between them), e.g. `phys.pg` (`fn step(): i32 { return 1 }`) + `snake.pg` (`fn tick(): i32 { return 2 }`)
  + a `Program.cs` calling both via `PolyglotProgram`. Assert it builds into one assembly with **no CS0101**
  and runs. (Mirrors FruitCake + Snake.)
- Optionally a direct CLI check: `polyglot build a.pg b.pg --target csharp --out …` then compile all `.cs`.
**Gate now:** this fixture **FAILS** on current `master` (CS0101/CS8863) — confirming it reproduces #14.

---

## Slice 2 — Core: hoist the C# prelude into a shared `__polyglot_prelude` module
**Files:** `src/MintPlayer.Polyglot.Core/include/mintplayer/polyglot/polyglot.hpp` (`LibConfig`);
`src/MintPlayer.Polyglot.Core/src/compiler.cpp` (`compile()` partition, ~544-633).
- Add `bool sharedPrelude = false;` to `LibConfig` — "this compile is one unit of a multi-file C# project;
  emit the shared prelude to its own file so it's defined once per assembly."
- In `compile()`, when `target.name() == "csharp" && lib.sharedPrelude`:
  - **Both paths split the prelude.** Reuse the `emitFile(keep)` lambda. Emit the **entry/own** file with
    `keep = {"", <each userOrigin>?}` — no wait: keep own decls only, `linked = true`, prelude EXCLUDED. Emit
    each imported user module as today (`keep = {canon}`, `linked=true`). Emit **one prelude module** via
    `emitFile` with `keep = {"<prelude>"}`, `linked = true`, appended to `result.modules` as
    `{"__polyglot_prelude", …}`.
  - Applies **even when `userOrigins.empty()`** (the no-import-root case — the actual bug): that root must now
    take a partitioned path (own decls + a prelude module) instead of the inline fast path.
  - Guard: if the prelude module has **no emittable decls** (shouldn't happen — `Option` is always linked —
    but be safe), skip the file.
- Exclude the reserved basename `__polyglot_prelude` from the user-basename collision check (~598-607) and
  ensure it's never treated as an importable module.
- **Everything else unchanged:** non-C# targets, and C# **single-input** builds (`sharedPrelude=false`), keep
  the exact current emission → byte-identical.
**Gate:** full-corpus byte diff = **zero** for single-file + non-C#; the Slice-1 CLI check now emits
`a.cs`/`b.cs`/`__polyglot_prelude.cs` and compiles clean.

---

## Slice 3 — CLI: set `sharedPrelude` for multi-input C# builds
**File:** `src/MintPlayer.Polyglot.Cli/src/main.cpp` (`runBuild`).
- In the **multi-input** branch (~559), set `lib.sharedPrelude = true` before the per-root `emitOne` loop
  (single-input branch leaves it false → unchanged). The identical `__polyglot_prelude.cs` each root emits is
  collapsed to one by the existing `writeDedup`/`seen` map — no new CLI logic.
- (`--access` composes: the prelude file carries the same `access`, so `public` propagates to `Option`.)
**Gate:** `polyglot build a.pg b.pg` → three files, one `__polyglot_prelude.cs`; compile all → no CS0101.

---

## Slice 4 — MSBuild: compile every generated `.cs`, not one-per-`.pg`
**File:** `src/MintPlayer.Polyglot.MSBuild/build/MintPlayer.Polyglot.MSBuild.targets`.
- A 2+-`.pg` project now also emits `__polyglot_prelude.cs` (no source `.pg`). Change `_PolyglotAddGenerated`
  to add the **glob `$(PolyglotOutDir)*.cs`** to `@(Compile)` + `FileWrites` (instead of the per-`PolyglotFile`
  map), so the prelude file is compiled and removed by `dotnet clean`. Robust for any file count; a
  single-`.pg` project still yields just its one `.cs`.
- Leave the incremental `Inputs/Outputs` on `PolyglotTranspile` per-`.pg` (drives regen); the prelude rides the
  same invocation.
**Gate:** `run-nuget.ps1` App fixture (1 `.pg`) unchanged; the new 2-`.pg` fixture builds + runs.

---

## Slice 5 — tests + gate updates
- **`run-nuget.ps1`:** the Slice-1 fixture now **passes**; update the existing **Shared** fixture (game imports
  nn) to tolerate the extra `__polyglot_prelude.cs` in its generated-file assertions; confirm `dotnet clean`
  removes the prelude file (FileWrites).
- **Conformance:** `library/` + `modular/` still agree cross-target (an extra `__polyglot_prelude.cs` is
  compiled/run with the set; stdout unchanged). run-diff/run-python already compile the whole dir.
- **Unit test** (`tests/MintPlayer.Polyglot.Tests/src/tests_main.cpp`): with `LibConfig{... sharedPrelude=true}`
  for C#, a program emits a `__polyglot_prelude` module in `result.modules`, `result.code` contains **no**
  `record Option`, and the wrappers are `partial`; with `sharedPrelude=false` the output is unchanged (Option
  inline). Confirms the flag gates cleanly.
**Gate:** `build-and-test.ps1` + `run-nuget.ps1` fully green.

---

## Slice 6 — version bump + docs
- `kVersion` → `0.3.1` (`polyglot.hpp`); NuGet `<Version>0.3.1</Version>`. **Plugins unchanged (0.3.0).**
- `CLAUDE.md`: `--version` → 0.3.1 + a Hotfix 0.3.1 status paragraph.
- `docs/prd/PLAN.md`: a `## Release 0.3.1` retrospective; mark issue-14 done.
- PRD §4.5 note: record the shared `__polyglot_prelude` file + the "multiple independent C# `.pg` roots" case
  now handled (removes the v1 "one C# entry per assembly" limitation the 0.3.0 notes flagged).
**Gate:** `scripts/build-and-test.ps1` green; then a `v0.3.1` tag → `release.yml` (CLI + NuGet).

## Risks / watch-items
- **Empty prelude file** — `Option` is always linked+emitted (no reachability pruning), so `__polyglot_prelude.cs`
  is always non-empty; still guard the skip-if-empty path.
- **`writeDedup` conflict** — if two roots ever produced differing prelude content (not possible with one
  `LibConfig` per invocation), `writeDedup` errors loudly rather than clobbering. Acceptable / correct.
- **MSBuild incremental** — deleting `__polyglot_prelude.cs` by hand without touching a `.pg` could skip regen;
  minor (obj is disposable). Optionally add it to `Outputs`.
- **`--access public` × prelude** — verify `Option`/Some/None in the shared file honor `public` (they render
  through the same `access` builtin).
