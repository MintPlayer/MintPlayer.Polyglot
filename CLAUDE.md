# MintPlayer.Polyglot — agent guide

A **cross-SDK transpiler**: one small source language → idiomatic, readable **C#/.NET** and
**TypeScript/JS**. A long-haul personal craft project, not on any delivery deadline.

**Read first:** `docs/prd/POLYGLOT_PRD.md` (vision, scope contract, architecture) and
`docs/prd/PLAN.md` (the full P0–P23 roadmap + slice logs). This file is just the always-on rules + a
thin current-status pointer — it deliberately does **not** carry the milestone history (that's PLAN.md's job).

## Prime directive — hold the scope line
The PRD §3 **support / refuse contract** is the law. Every multi-target transpiler that died (JSIL,
SharpKit, Bridge.NET) died of scope creep. Before adding any feature, check it against §3:
- **Supported (§3.A):** operators, properties/indexers, extension methods, exceptions, `using`,
  iterators, pattern matching/ADTs, enums, closures, overloading, strings (both targets are UTF-16).
- **Refused (§3.B), with a clear diagnostic — never a miscompile:** threads/locks, runtime reflection,
  finalizers/GC hooks, `decimal`, `unsafe`/pointers, `dynamic`/runtime code-gen, bit-exact cross-target
  floats.
- **Faithful-by-default with a *published* relaxation list (§3.C):** int overflow masking, int64→BigInt,
  opt-in `Math.fround` strict floats, structural equality. Never relax silently — document it.
- **Determinism honesty (§3.D):** only `+ − × ÷ √` are reproducible across .NET and JS; transcendentals
  are not. Don't promise bit-exact float parity; offer a fixed-point std type instead.

## Principled fix over workaround
This is a long-haul craft project — prefer the **root-cause fix** over an expedient patch, even when the
patch is smaller. When a problem traces to a missing language/compiler capability, build (or plan) that
capability rather than papering over the symptom. Example (2026-06-29): a generic call's return type wasn't
substituted, so `Math.max(i64,i64)` would print `20n` vs `20`; the workaround was to wrap every `print` arg
in `String()`, but the principled fix was **real TypeArg inference** (bind type params from args, substitute
the return) — which fixes the whole class of generic-call bugs, not just `print`. Take that path. If a
workaround is genuinely warranted (time-boxed, the real fix is out of scope), say so explicitly and leave a
note pointing at the principled follow-up — never let a silent shortcut masquerade as the design.

## Key decisions (don't relitigate without reason)
- **C++20**, single self-contained native CLI, zero runtime deps. Consequence: **no Roslyn / no ts-morph**
  — the C# and TS backends **hand-write** their pretty-printers over the IR (the Haxe path). The C#/Roslyn
  alternative is recorded in PRD §4.3 as the fork to revisit only if hand-emitters become painful.
- **One high-level, typed, tree-shaped IR. NOT SSA, NOT a common denominator.** Specialize per target.
- Targets: **C# and TS first.** More targets are post-P8 stretch.

## Build / run (Windows)
Open `MintPlayer.Polyglot.sln` in a C++-capable VS (*Desktop development with C++* workload), or build
from MSBuild:
```
msbuild MintPlayer.Polyglot.sln /p:Configuration=Debug /p:Platform=x64
x64\Debug\MintPlayer.Polyglot.Cli.exe --version      # -> 0.3.2
x64\Debug\MintPlayer.Polyglot.Tests.exe              # -> all tests pass
```
**One-shot gate** (build → unit tests → differential C#/TS conformance): `pwsh scripts/build-and-test.ps1`
— or invoke the **`/build-and-test`** skill (`.claude/skills/build-and-test/`). Needs `dotnet` + `node`
for the conformance stage.

**Do NOT run intermediary builds/tests between phases/slices.** The full gate takes ~15 minutes; running
it per slice multiplies that into hours of waiting. Implement EVERYTHING first, then build + run the full
gate **once** at the end. (If a slice really needs a mid-flight sanity check, the cheap unit-test exe run
is the ceiling — never the full gate, and never speculative extra legs like WSL/CMake builds.)
TOOLCHAIN: the projects target PlatformToolset **v145** / VCProjectVersion **18.0**, so they **require
VS 2026 (the "18" generation)** — by design; this is a VS-2026-only project. The build is **VS 18
"Insiders"** (v145 → MSVC 14.51):
- IDE:     `C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\IDE\devenv.exe`
- MSBuild: `C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\MSBuild.exe`

VS 2019 BuildTools (v142) and VS 2022 (v143) are both **insufficient** for v145 — don't build with them.

**POSIX build (Linux + macOS)** — P22 slice 2, the `.vcxproj` stays the Windows source of truth: a root
`CMakeLists.txt` mirrors the three projects (verified on WSL Ubuntu with g++/cmake; the macOS legs build via
clang on `macos-13`/`macos-14` in `release.yml`). `cmake -S . -B build
-DCMAKE_BUILD_TYPE=Release && cmake --build build` → `build/polyglot` (the CLI; static-linked libstdc++ on
Linux, ad-hoc-signed on macOS) + `build/polyglot-tests`. `scripts/check-buildfile-parity.ps1` guards
`.vcxproj`↔CMake source-list drift (first stage of `build-and-test.ps1`).

## Layout
```
src/MintPlayer.Polyglot.Core/   # compiler library (lexer→parser→typed IR→backends); public headers in include/
src/MintPlayer.Polyglot.Cli/    # the `polyglot` CLI
tests/MintPlayer.Polyglot.Tests/# unit + (later) differential-conformance tests
docs/prd/                       # PRD + plan
docs/lang/                      # SPEC.md + grammar.ebnf + samples/*.pg  (P1 design)
```

## Status & next step
The full pipeline is built and shipping — this is a maturing project, not a skeleton. Current versions:
**CLI + NuGet 0.3.2**, the four target plugins (**csharp / typescript / python / php**) at **0.3.0**, the
VS Code extension at **0.4.1**.

What exists end-to-end today (per-milestone history + slice logs live in `docs/prd/PLAN.md`; roadmap
summary in PRD §6 — this file does **not** track milestones):
- **Language → four targets.** `.pg` → idiomatic **C#, TypeScript, Python, PHP** through the one typed IR.
  Backends are **100% JSON plugins** (P18–P19): zero backends compiled in — a language is a
  `plugins/<target>/polyglot-plugin.json` the Core loads + validates (anti-silent-drop coverage contract).
  The §3.A surface is complete, with §3.B refusals, §3.C faithfulness, and single-threaded async/await (P15).
  Collections come in two spellings — growable `List<T>` and fixed-size `T[]` arrays (both erase to a JS
  array on TS; C# keeps `T[]` vs `List<T>`); an un-inferable initializer (`[]`/`null`) must be annotated,
  and a union element parenthesizes inside a postfix array — `(Node | null)[]` (P29, issue #27).
- **Editor tooling.** A zero-dep `polyglot lsp` (diagnostics / go-to-def / hover / symbols / semantic tokens /
  rename / completion), live generated-output preview, and watch mode (P16 / P17 / P21). The VS Code
  extension is on the marketplace (ID `mintplayer.polyglot-lang`, frozen).
- **Distribution.** Build-time **plugin auto-download** (P30, issue #30): `pgconfig.json` `dependencies`
  resolve inside the exe — in-box → lockfile-pinned verified cache (offline, zero network) → the npm
  registry HTTP API (SRI-verified, data-only, no npm/tar processes) — pinned in a committed
  `pgconfig.lock.json`; `polyglot install` is an optional cache pre-warmer. Plus the npm target plugins,
  the `.pg`-aware **NuGet** (auto-transpiles before `dotnet build`, per-RID), a provenance-attested
  prebuilt-CLI release channel, and a cross-platform CLI (Windows + Linux x64/arm64 + macOS x64/arm64).
  A no-config, no-`--target` build now refuses (the plugin set is fully config-sourced).

In flight / gated: **P23** (bundle the CLI in the VS Code extension for zero-setup install — built, pending
an interactive vsix install + the first marketplace publish; PR #16), **P22** tail (PHP runtime differential +
the esbuild-pattern npm CLI sibling), **P16d** (Visual Studio LSP client — built, interactive verify pending),
**P20** (alternative input "skins" — designed, demand-gated).

**Next step:** finish P23 verification (interactive install + publish), or pick up the P22 tail.

To verify a build: `pwsh scripts/build-and-test.ps1` (build → unit tests → differential C#/TS/Python
conformance), or the **`/build-and-test`** skill.

## Sibling repo
The P8 dogfood target (FruitCake physics twins) lives in `C:\Repos\MintPlayer.AI` — see PRD §8 for paths.
That repo is a .NET + Angular app; this one is unrelated C++ tooling. Don't run its build/tests from here.
