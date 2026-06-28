---
name: build-and-test
description: Build the MintPlayer.Polyglot solution (VS 2026 / toolset v145) and run the full test gate — in-process unit/golden tests plus the differential C#/TS conformance suite. Use whenever asked to build, test, verify, or confirm the project compiles and passes (e.g. "build and test", "run the tests", "is it green?", "verify the build").
---

# Build and test MintPlayer.Polyglot

Run the repo's one-shot build+test gate and report the outcome. Do **not** re-derive the build steps by
hand — call the script.

## Run

From the repo root:

```
pwsh scripts/build-and-test.ps1
```

It runs three stages in order and stops at the first failure:
1. **Build** the solution (VS 2026 MSBuild, toolset v145, `Debug|x64`).
2. **Unit / golden tests** — `x64\Debug\MintPlayer.Polyglot.Tests.exe`.
3. **Differential conformance** — `tests/conformance/run-diff.ps1`: emits each `tests/conformance/programs/*.pg` to C# *and* TS, runs both, and asserts identical stdout.

Useful flags: `-Configuration Release` to gate the release build; `-SkipConformance` to skip the
`dotnet`/`node` differential stage (faster inner loop).

## Report

State clearly: build pass/fail, the unit-test result, and the conformance result. On failure, surface the
**first** error with its `file:line` and stop — quote the actual output. Do not loop build retries: a
`dotnet run` left alive during conformance can lock a DLL and cause `MSB3027`/`MSB3021` on the next build;
if that happens, kill the `dotnet`/app process tree first (see the global CLAUDE.md note), don't retry.

## Requirements
- **VS 2026** (toolset v145) — the project does not build on VS 2022/2019 (see `CLAUDE.md`).
- **`dotnet`** and **`node`** on PATH for the conformance stage (Node 22+/24 for `.ts` type-stripping).
