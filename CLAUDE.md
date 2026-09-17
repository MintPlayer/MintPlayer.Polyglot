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
x64\Debug\MintPlayer.Polyglot.Cli.exe --version      # -> 0.0.0-dev in-tree; the real number is stamped at release
x64\Debug\MintPlayer.Polyglot.Tests.exe              # -> all tests pass
```
**One-shot gate** (build → unit tests → all gate legs → differential C#/TS/Python/PHP conformance):
`pwsh scripts/build-and-test.ps1` — or invoke the **`/build-and-test`** skill
(`.claude/skills/build-and-test/`). Needs `dotnet` + `node` (+ `python`/`php` for those legs).
**Tiered (P35):** `-Tier fast` = parity/build/unit/cli-smoke/refusals/lsp (~20 s, the mid-slice sanity
ceiling); `-Tier full` (default) is the only pre-merge bar (~3–4 min — the conformance leg is ONE
merged parallel runner over a shared `csc /shared` oracle, not three legs). The registry leg fails the
gate unless `POLYGLOT_ALLOW_REGISTRY_SKIP=1` (the documented local loopback opt-out; CI never sets it).

**NX-cached path (P35 slice 6):** `npx nx run polyglot:<leg>` or `npx nx run-many -t <legs>` runs the
same runners with per-leg caching (local + the remote at nx-cache.mintplayer.com — credentials are
machine env vars). Keys are `compilerSources` (src/plugins/sln) + the runner's own files + toolchain
`--version`s, NOT the built exe (stable across non-reproducible relinks; `dependsOn build` refreshes
the exe before any cache-miss runs). The runners stay directly invocable without NX — that is the
contract; release.yml never touches NX. CI adoption of the org RO/RW token convention is an opt-in
follow-up (the source-keying unblocked it; wiring it changes the CI contract, so it needs sign-off).

**Code coverage** — **three** instruments answering three different questions. None substitutes for
another: a 100% C++ number with dead template arms is a lie, and both can be green while the targets
diverge at runtime.

| Question | Instrument |
|---|---|
| Which **C++ Core/CLI** lines ran? | `ci.yml` `coverage` job (g++ `--coverage` + gcovr) · locally `pwsh scripts/coverage.ps1` (OpenCppCoverage, `choco install opencppcoverage`, HTML at `x64/coverage/`) |
| Which **plugin template arms** ran? | the arm tracer — `polyglot --emit-arm-trace <file>` + `scripts/arm-trace-to-lcov.ps1`, one lcov per `plugins/<t>/polyglot-plugin.json` |
| Do the four targets **agree at runtime**? | the differential conformance suite (`tests/conformance/`) |
| Which **`.pg` lines ran in a CONSUMER's** coverage run? | `--origin-info` (P38, issue #69) — C# `#line` → PDB sequence points; TS → a v3 sidecar. Opt-in; off = byte-identical output |

The arm tracer exists because the backends **are** the JSON manifests (zero compiled in), so gcov is
structurally blind to them, and the load-time anti-silent-drop contract proves a rule *exists*, never
that it *ran*. Its denominator comes from the compiler's own parse, so a never-fired arm reports as
uncovered rather than missing. It is a debug flag, off by default — one pointer test per evaluation.

Both surfaces publish to **coverage.mintplayer.com** from the `coverage` job via the org's shared
action (OIDC, no token — the repo is public). Report-only: no floor yet, and turning one on is a
committed `coverage.yml` plus branch protection, not a code change. `scripts/verify-coverage-paths.ps1`
is the tripwire for the server's one silent failure mode — a report path that doesn't suffix-match
`git ls-files` is dropped without an error. Design: `docs/prd/code-coverage-upload/`.
**Known bias:** coverage is measured on Linux only, so `#ifdef _WIN32` branches read as permanently
uncovered. Don't "fix" the number by deleting a POSIX branch.

**Do NOT run intermediary builds/tests between phases/slices.** The full gate takes ~3–4 min (P35;
was ~15); running it per slice still multiplies into a lot of waiting. Implement EVERYTHING first, then
build + run the full gate **once** at the end. (If a slice really needs a mid-flight sanity check,
`-Tier fast` or the cheap unit-test exe run is the ceiling — never the full gate, and never speculative
extra legs like WSL/CMake builds.)
**Exception:** when a change touches platform-forked code (`#ifdef _WIN32`/POSIX branches, dlopen/popen,
chrono/filesystem edges), one POSIX compile+unit run (WSL `cmake`, or the PR's Linux check) is part of
the required end gate, not an extra leg — MSVC accepting the code proves nothing about g++/clang (the
P30 release run broke on every POSIX leg post-merge; PRs now run `ci.yml` = one ubuntu build as the
pre-merge floor).
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
The full pipeline is built and shipping — this is a maturing project, not a skeleton.

**Versions ship in LOCKSTEP** — the CLI, the NuGet package, the four target plugins
(**csharp / typescript / python / php**) and the VS Code extension all carry the same number, enforced in
`main.cpp` / `pluginresolve.hpp`. Nothing in-tree states it: every manifest says `0.0.0-dev` and the real
number is stamped at release by `release.yml` / `publish-plugins.yml` / `publish-vscode.yml`. So the source
of truth is the tags — `git tag --sort=-v:refname | head -1`. (This paragraph used to name a number; it
duplicated a fact it does not own and drifted six minor versions out of date, which is why it no longer does.)

What exists end-to-end today (per-milestone history + slice logs live in `docs/prd/PLAN.md`; roadmap
summary in PRD §6 — this file does **not** track milestones):
- **Language → four targets.** `.pg` → idiomatic **C#, TypeScript, Python, PHP** through the one typed IR.
  Backends are **100% JSON plugins** (P18–P19): zero backends compiled in — a language is a
  `plugins/<target>/polyglot-plugin.json` the Core loads + validates (anti-silent-drop coverage contract).
  The §3.A surface is complete, with §3.B refusals, §3.C faithfulness, and single-threaded async/await (P15).
  Collections come in two spellings — growable `List<T>` and fixed-size `T[]` arrays (both erase to a JS
  array on TS; C# keeps `T[]` vs `List<T>`); an un-inferable initializer (`[]`/`null`) must be annotated,
  and a union element parenthesizes inside a postfix array — `(Node | null)[]` (P29, issue #27).
  Interfaces are checker-enforced (implements-conformance, C#-convention `override`, nominal
  assignability) and emit on all four targets incl. Python ABC; `std.strings.codePointAt` is the
  char→ordinal path (hex literals lex correctly); PHP lists/arrays are reference-semantic
  `\ArrayObject` and module globals reach methods/getters (P31, issues #29 + #33–#36).
- **Editor tooling.** A zero-dep `polyglot lsp` (diagnostics / go-to-def / hover / symbols / semantic tokens /
  rename / completion), live generated-output preview, and watch mode (P16 / P17 / P21). The VS Code
  extension is on the marketplace (ID `mintplayer.polyglot-lang`, frozen).
- **Distribution.** Build-time **plugin auto-download** (P30, issue #30): `pgconfig.json` `dependencies`
  resolve inside the exe — in-box → lockfile-pinned verified cache (offline, zero network) → the npm
  registry HTTP API (SRI-verified, data-only, no npm/tar processes) — pinned in a committed
  `pgconfig.lock.json`; `polyglot install` is an optional cache pre-warmer. Output routing is
  config-sourced too: pgconfig **`include` rules** (`{ pattern, target, output-template }`,
  `%(Filename)`/`%(Directory)`/`%(RecursiveDir)`/`%(TargetLanguage)`, extension auto-appended from the
  plugin manifest) route each emitted file — TS twins into an Angular app while C# stays in obj — and a
  bare `polyglot build` discovers inputs from the same patterns; a closure split across dirs emits
  real relative specifiers on a `crossDirImports` target (TS declares it) and refuses loudly on the
  rest (Python/PHP/C#). Plus the npm target plugins, the `.pg`-aware **NuGet**
  (auto-transpiles before `dotnet build`, per-RID, **no language flag — the consumer's pgconfig decides;
  minimum `"targets": ["csharp"]`**), a provenance-attested prebuilt-CLI release channel, and a
  cross-platform CLI (Windows + Linux x64/arm64 + macOS x64/arm64). A no-config, no-`--target` build
  refuses (the plugin set is fully config-sourced).

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
