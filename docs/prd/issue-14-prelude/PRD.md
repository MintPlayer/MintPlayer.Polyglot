# PRD — Issue #14: multi-`.pg` C# projects duplicate the runtime prelude (CS0101/CS8863)

> **Status:** ✅ DONE · 2026-07-05 · released as **0.3.1** (all six slices built + gated)
> **Source:** [GitHub issue #14](https://github.com/MintPlayer/MintPlayer.Polyglot/issues/14) — blocking the
> MintPlayer.AI M33 Snake port (a **second** `.pg` alongside the existing FruitCake `.pg` in one project) on
> `MintPlayer.Polyglot.MSBuild 0.3.0`.
> **Companion:** `PLAN.md` (byte-gated slice plan). **Investigation:** 2-agent trace, 2026-07-05.

## 1. Problem statement
A C# project with **two or more independent `.pg` files** (that do not import each other) fails to compile:
each transpiled `.pg` emits its **own copy** of the shared runtime prelude — the `Option<T>` / `Some<T>` /
`None<T>` records **and** the `static class PolyglotProgram` wrapper — so compiling both generated `.cs` into
one assembly hits **CS0101** (duplicate `Option`/`Some`/`None`/`PolyglotProgram`) and **CS8863** (the
`sealed record Some<T>(T value)` param list can't be split across `partial` declarations). One `.pg` builds
fine; adding a second breaks the build.

Minimal repro (no shared user types, no imports — exactly the FruitCake + Snake shape):
```sh
printf 'fn fa(): i32 { return 1 }\n' > a.pg
printf 'fn fb(): i32 { return 2 }\n' > b.pg
polyglot build a.pg b.pg --target csharp --out out   # ONE invocation, both files (as the MSBuild target does)
# out/a.cs AND out/b.cs each contain: abstract record Option<T>; sealed record Some<T>(…); … static class PolyglotProgram
```

This is a **regression from 0.3.0 module linking** (issue #11) and contradicts the behavior the package's own
`.targets` advertises (*"emits one flat .cs per .pg module — so compiling them all into one assembly no longer
hits CS0101"*). The **cross-module import** linking works correctly (an imported user type is referenced, not
redefined); the **auto-emitted prelude** is what duplicates, independent of imports.

## 2. Root cause (from the 2026-07-05 investigation)
Two path decisions interact:

1. **A no-import root takes the single-file fast path** (`compiler.cpp` `compile()`, the `if (userOrigins.empty())`
   branch, ~585). `userOrigins` is the set of imported **user**-module origins (non-empty `originModule`,
   excluding the `"<prelude>"` sentinel). A `.pg` that imports nothing has an empty `userOrigins`, so `compile()`
   lowers the whole unit and emits it **inline, unpartitioned, with `linked=false`** — the full prelude
   (`Option`/`Some`/`None`, always linked via the core prelude at `STD_CORE`, and the `PolyglotProgram` wrapper)
   lands in that one file. The entry-only-prelude + `partial`-wrapper policy lives only in the **multi-module**
   branch (`emitFile`'s `keep` set + `m.linked=true`), which a no-import root never enters.
2. **The multi-root CLI build runs N independent `compile()`s with path-based dedup only** (`main.cpp` `runBuild`
   multi-input, ~559). For `build a.pg b.pg` (neither imports the other) **both are roots**, each compiled by a
   separate `emitOne`→`compile()`. `writeDedup` keys on the **output path**, so `a.cs` and `b.cs` are distinct
   keys and both are written — each with its own inlined prelude. Dedup only collapses the *same* basename with
   *identical* content (the mechanism P19 built for shared imported modules).

Consequences that shape the fix:
- The only `"<prelude>"` decl that emits as a C# **type** is the **`Option<T>` union** (Some/None). `Error`,
  `Iterable`, `INumber`, `List`, `Math`, and the scalar `parse`s are `extern` (not emitted). `print` (when
  `--lib io` is active) emits as a `PolyglotProgram` method; without it, only the empty-ish wrapper + `Option`
  collide. `Option` is emitted **even when unused** (lowering does no reachability pruning), so it is always
  present.
- `Some<T>`/`None<T>` are `sealed record`s **with positional parameter lists** → C# forbids them from being
  `partial` (**CS8863**). So the prelude cannot be de-duplicated by marking it `partial`; it must be emitted in
  **exactly one file**.
- **`run-nuget.ps1`'s shared-library fixture didn't catch this**: there `game.pg` *imports* `nn.pg`, so only
  `game.pg` is a root → one `compile()` → one prelude. The multi-**root** case (2+ files that don't import each
  other) was never built.

## 3. Design — hoist the C# prelude into one shared file (`__polyglot_prelude.cs`)
Chosen (both investigation agents concur): **Candidate A, scoped to C#**, reusing existing machinery.

**The model.** When a C# build comprises **multiple input files** (a "project build" — the CLI already knows
this), `compile()` emits the `"<prelude>"`-origin declarations into a single reserved module
**`__polyglot_prelude`** instead of inlining them, and emits every other module (including a no-import root's
own decls) with **`linked=true`** so the `PolyglotProgram`/`PolyglotExtensions` wrappers become `partial` and
merge across files. Because the prelude module's content depends only on the core/lib set + target — identical
for every root in one invocation — each root's `compile()` produces a **byte-identical** `__polyglot_prelude.cs`
at the same output path, and the existing `writeDedup` collapses it to **one** file. Result in one assembly:
`Option`/Some/None defined once (in the prelude file), each `.pg`'s free functions in its own `partial
PolyglotProgram` (they merge), `Main` still emitted once (gated on `module.hasEntry`). No CS0101/CS8863.

**Why this shape:**
- **Reuses, doesn't rebuild.** `emitFile`'s partition-by-`keep`, the `partial`-on-`linked` wrapper heads (in
  the csharp plugin), and the `writeDedup`/`seen` cross-root dedup all already exist. The fix is wiring.
- **No cross-file `using`/import for C#** — global-namespace types + `partial`, one assembly. `buildImports`
  already returns empty for C#. **No plugin change** (the wrappers already emit `partial` when `module.linked`).
- **Multi-root dedup falls out for free** from `writeDedup`(identical content → written once); a genuine
  content conflict (e.g. a hypothetical future per-root differing `lib`) errors loudly rather than clobbering.

**Scoping decision — gate on multi-input, and C#-only:**
- **C#-only.** TS/Python/PHP emit each file as its own module scope, so a per-file `Option`/`print` copy is
  harmless (no cross-file type merging). The current `preludeEverywhere = target != "csharp"` policy already
  reflects this; a shared prelude file for those targets would force synthetic prelude `import`/`export`
  plumbing for zero correctness gain. Leave them per-file-inline.
- **Multi-input only.** A **single** `.pg` built alone keeps the current inline emission (one self-contained
  `.cs`, **byte-identical** to 0.3.0) — the split activates only when the CLI is given 2+ inputs (which is
  exactly when the collision is possible). This preserves the single-file golden invariant and leaves the
  run-nuget single-`.pg` App fixture untouched. The MSBuild NuGet passes all `**/*.pg` in one invocation, so a
  1-file project is single-input (unchanged) and a 2+-file project is multi-input (shared prelude).

**Plumbing.** A `LibConfig::sharedPrelude` flag (set by the CLI's multi-input branch) tells `compile()` to
peel the prelude into `__polyglot_prelude` for C#. The reserved basename is excluded from user-basename
collision checking and is not an importable module.

**MSBuild.** `_PolyglotAddGenerated` currently adds one `.cs` per `@(PolyglotFile)`; a 2+-file project now also
produces `__polyglot_prelude.cs`, which has no source `.pg`. Switch the generated-file set to **glob
`$(PolyglotOutDir)*.cs`** (for `@(Compile)` + `FileWrites`) so the prelude file is compiled and cleaned. This
is robust for any file count and keeps the single-file project unchanged.

## 4. Scope guard (PRD §3 check)
- **§3.B refuse-not-miscompile:** the collision was a hard compile error (CS0101/CS8863), never a silent
  miscompile — this makes the intended "each definition once per assembly" (§4.5) actually hold. No new surface.
- **Determinism / faithfulness:** unaffected (prelude relocation is layout-only; behavior identical).
- **Byte-identical invariant:** preserved for single-file programs and all non-C# targets; multi-`.pg` C#
  output changes by design (prelude relocates to the shared file; wrappers gain `partial`).

## 5. Acceptance criteria
1. `polyglot build a.pg b.pg --target csharp --out out` (independent files, no imports) emits `a.cs`, `b.cs`,
   and one `__polyglot_prelude.cs`; compiling all three into one assembly succeeds — **no CS0101/CS8863**; the
   program runs.
2. The MintPlayer.AI shape works through the NuGet: a project with **two independent `.pg`** builds with
   `dotnet build` into one assembly and runs (a new `run-nuget.ps1` fixture proves it).
3. Single-`.pg` C# output is **byte-identical** to 0.3.0 (the App fixture + single-file goldens unchanged).
4. The `library/` (entry imports nn) + `modular/` programs still produce identical stdout (now with a
   `__polyglot_prelude.cs` in the set); TS/Python/PHP output byte-identical.
5. `scripts/build-and-test.ps1` + `run-nuget.ps1` green end-to-end.

## 6. Versioning
- **CLI / Core (`kVersion`) → 0.3.1**, **NuGet `MintPlayer.Polyglot.MSBuild` → 0.3.1** (patch — regression fix).
- **Plugins unchanged (stay 0.3.0)** — the fix is Core (`compile()` partition) + CLI (flag) + MSBuild (glob);
  the csharp plugin's `partial`-on-`linked` wrappers already support it. No manifest change.
- Publishing: a `v0.3.1` tag fires `release.yml` (CLI RIDs + GitHub Release + NuGet). Plugins are not
  republished (no plugin path changed, so `publish-plugins.yml` does not fire).
