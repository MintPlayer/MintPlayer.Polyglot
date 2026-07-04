# MintPlayer.Polyglot

> A **cross-SDK transpiler**: write logic once in a small, deliberately-scoped source language (`.pg`)
> and emit idiomatic, readable code for multiple target SDKs — **C# / .NET**, **TypeScript / JavaScript**,
> **Python**, and **PHP** today, with every target defined as a **pure-JSON plugin**.

Polyglot exists to solve the "same logic, two languages, kept in sync by hand" problem (the concrete
motivating case: a physics solver living as a hand-maintained twin in C# and TypeScript). Rather than a
general-purpose "compile any language to any language" tool — a known multi-decade trap — Polyglot is
**faithful-by-default with a published list of relaxations**, and it **refuses** the features that make
transpilers balloon (threads, runtime reflection, finalizers, `decimal`, `unsafe`, bit-exact cross-target
floats). A differential conformance suite keeps every target's *runtime output* identical, program by
program.

- **Why the name:** a *polyglot* writes in many languages — exactly what the emitter does.
- **Read first:** [`docs/prd/POLYGLOT_PRD.md`](docs/prd/POLYGLOT_PRD.md) (vision, scope contract,
  architecture) and [`docs/prd/PLAN.md`](docs/prd/PLAN.md) (the milestone log).
- **Coming from C# or TypeScript?** Side-by-side construct maps:
  [`docs/lang/for-csharp-devs.md`](docs/lang/for-csharp-devs.md) ·
  [`docs/lang/for-typescript-devs.md`](docs/lang/for-typescript-devs.md). The language spec is
  [`docs/lang/SPEC.md`](docs/lang/SPEC.md).

## A taste

```
// hello.pg
import { print } from "std.io"

record Point(x: f64, y: f64)

fn dist2(a: Point, b: Point): f64 {
  let dx = a.x - b.x
  let dy = a.y - b.y
  return dx * dx + dy * dy
}

fn main() {
  print("d2 = ${dist2(Point(3.0, 4.0), Point(0.0, 0.0))}")
}
```

```
polyglot build hello.pg                      # emits hello.cs + hello.ts (the default pair)
polyglot build hello.pg --target python     # emits hello.py
polyglot build hello.pg --target php        # emits hello.php
```

Each output is idiomatic, readable source for that language — and running all of them prints the same
bytes.

## Getting the CLI

There is no prebuilt binary distribution yet — the CLI is built from source (a GitHub-Releases channel
is on the roadmap):

1. You need **Visual Studio 2026** with the *Desktop development with C++* workload (the projects pin
   platform toolset **v145**; VS 2022/v143 is not sufficient).
2. Build the solution:

   ```
   msbuild MintPlayer.Polyglot.sln /p:Configuration=Debug /p:Platform=x64
   ```

3. The self-contained CLI lands at `x64\Debug\MintPlayer.Polyglot.Cli.exe` with its target plugins in
   the `plugins\` folder beside it (statically linked CRT — no runtime dependencies). Put it on your
   `PATH` as `polyglot` if you like.

One-shot verification (build → unit tests → differential conformance; needs `dotnet` + `node`):
`pwsh scripts/build-and-test.ps1`.

## Using the CLI

```
polyglot build <input.pg> [--target <name>] [--out <dir>] [--root <dir>] [--lib <a,b>] [--watch]
polyglot check <input.pg> [--json] [--watch]   # diagnostics only, no output files
polyglot fmt   <input.pg>                      # canonical re-print of the source
polyglot lsp                                   # the language server (spawned by editors, stdio JSON-RPC)
polyglot install <plugin-dir | npm-name>       # add a target plugin to the user cache
```

`--watch` keeps the emitted outputs fresh: it rebuilds whenever the input, any transitively imported
`.pg`, or `pgconfig.json` changes (all configured targets per rebuild). A failed rebuild prints the
diagnostics, keeps watching, and never touches the last good outputs.

A `pgconfig.json` next to (or above) your `.pg` files replaces the flags:

```json
{
  "root": ".",
  "lib": ["io", "math"],
  "targets": ["csharp", "typescript"],
  "forbiddenIdentifiers": { "*": ["temp"] }
}
```

`root` anchors logical imports (`import { a } from "geometry"` → `<root>/geometry.pg`), `lib` is the
ambient std prelude (so `print`/`Math` resolve without imports), `targets` is the project's target set —
a bare `polyglot build foo.pg` then emits **all** of them — and `forbiddenIdentifiers` bans names
project-wide or per target.

## Targets are plugins

No language is compiled into the CLI: **a target is one JSON file** (`polyglot-plugin.json` — spec
tables + emission rules + capabilities + std bindings), validated at load so a malformed or incomplete
plugin is refused rather than silently miscompiling. The four first-party targets live in
[`plugins/`](plugins/) and are published to npm:

| Target | Package |
|---|---|
| C# | [`@mintplayer/polyglot-target-csharp`](https://www.npmjs.com/package/@mintplayer/polyglot-target-csharp) |
| TypeScript | [`@mintplayer/polyglot-target-typescript`](https://www.npmjs.com/package/@mintplayer/polyglot-target-typescript) |
| Python | [`@mintplayer/polyglot-target-python`](https://www.npmjs.com/package/@mintplayer/polyglot-target-python) |
| PHP | [`@mintplayer/polyglot-target-php`](https://www.npmjs.com/package/@mintplayer/polyglot-target-php) |

Resolution order: the `plugins/` folder next to the exe → a pgconfig `dependencies` entry
(`{"mytarget": "file:<dir>"}`) → the user cache (`%LOCALAPPDATA%\polyglot\plugins\`) → a clean refusal
naming the known targets. `polyglot install <name>` fetches a plugin from npm (bare names map to
`@mintplayer/polyglot-target-<name>`), validates it, and caches it:

```
polyglot install php               # from npm
polyglot install path\to\plugin    # from a local directory
```

Writing a new language means writing a JSON file — the PHP backend was added without touching the
compiler. **[`docs/plugin-authoring.md`](docs/plugin-authoring.md)** is the guide (manifest anatomy,
the rule DSL, validation, testing, publishing), with
[`plugins/php/polyglot-plugin.json`](plugins/php/polyglot-plugin.json) as the worked example;
[`docs/design/json-plugins.md`](docs/design/json-plugins.md) records the underlying design.

## Editor support

The **[MintPlayer Polyglot](https://marketplace.visualstudio.com/items?itemName=mintplayer.polyglot-lang)**
VS Code extension ships syntax highlighting plus the full language server: live diagnostics,
go-to-definition (including into the std library), hover, completion, rename, formatting, per-target
reserved-name checks, a **live generated-output preview** ("Show Generated Output" opens the emitted
C#/TS/Python beside your `.pg` as you type), and **watch mode** (a status-bar toggle / `polyglot` task
type runs `build --watch` in the background with errors in the Problems panel). Highlighting works out
of the box; the language features need the CLI — set `polyglot.cliPath` or have `polyglot` on `PATH`.

## .NET build integration

`MintPlayer.Polyglot.MSBuild` (in [`src/MintPlayer.Polyglot.MSBuild/`](src/MintPlayer.Polyglot.MSBuild/),
not yet on nuget.org) makes `dotnet build` transpile `.pg` files straight into an ordinary C# project —
incremental, clean-aware, non-transitive, no extra SDK. The `.pg` sources are registered as `Watch`
items, so **`dotnet watch build|run` re-transpiles on every `.pg` edit** — which is also the watch story
inside Visual Studio for .NET hosts (opt out with `Watch="false"` metadata or `PolyglotWatch=false`).
Pack it locally with `dotnet pack` (it embeds the CLI you built); the end-to-end gate is
`tests/msbuild/run-nuget.ps1`.

## Layout

```
MintPlayer.Polyglot.sln
src/
  MintPlayer.Polyglot.Core/     # the compiler library (lexer → parser → sema → typed IR → plugin engine)
  MintPlayer.Polyglot.Cli/      # the `polyglot` CLI (build/check/fmt/lsp/install)
  MintPlayer.Polyglot.MSBuild/  # the .pg-aware NuGet (assets-only package)
plugins/                        # the four first-party targets — each one JSON file + npm packaging
editors/vscode/                 # the VS Code extension (TextMate grammar + LSP client + live preview)
editors/vs/                     # the Visual Studio VSIX (LSP client)
tests/
  MintPlayer.Polyglot.Tests/    # in-process unit tests
  conformance/                  # differential runtime gates (C#/TS, Python vs the C# oracle)
  samples/, fidelity/, msbuild/ # sample-compile, fmt round-trip, and NuGet gates
docs/
  prd/                          # PRD + milestone plan
  lang/                         # SPEC.md, grammar, samples, the Rosetta docs
  design/                       # subsystem designs (JSON plugins, backend engine, skins, …)
```
