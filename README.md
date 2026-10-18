# MintPlayer.Polyglot

[![Coverage](https://coverage.mintplayer.com/badge/MintPlayer/MintPlayer.Polyglot.svg)](https://coverage.mintplayer.com/r/MintPlayer/MintPlayer.Polyglot)

> A **cross-SDK transpiler**: write logic once in a small, deliberately-scoped source language (`.pg`)
> and emit idiomatic, readable code for multiple target SDKs — **C# / .NET**, **TypeScript / JavaScript**,
> and **Python** (full §3.A surface), plus **PHP** (a **partial** target today — see
> [Targets are plugins](#targets-are-plugins)), with every target defined as a **pure-JSON plugin**.

Polyglot exists to solve the "same logic, two languages, kept in sync by hand" problem (the concrete
motivating case: a physics solver living as a hand-maintained twin in C# and TypeScript). Rather than a
general-purpose "compile any language to any language" tool — a known multi-decade trap — Polyglot is
**faithful-by-default with a published list of relaxations**, and it **refuses** the features that make
transpilers balloon (threads, runtime reflection, finalizers, `decimal`, `unsafe`, bit-exact cross-target
floats). A differential conformance suite keeps every target's *runtime output* identical, program by
program.

- **Why the name:** a *polyglot* writes in many languages — exactly what the emitter does.
- **See it in action:** [qrcode-demo](https://github.com/PieterjanDeClippel/qrcode-demo) — a QR-code
  encoder written once in `.pg` and emitted to all four targets (the real-world dogfood project behind
  several fixed issues).
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

**Prebuilt (Windows x64):** download `polyglot-win-x64.zip` from
[GitHub Releases](https://github.com/MintPlayer/MintPlayer.Polyglot/releases) and unzip — it contains
`polyglot.exe` with its `plugins/` folder beside it (statically linked, zero runtime dependencies). Put
the folder on your `PATH`.

Every release binary ships with a **signed build-provenance attestation** (GitHub artifact
attestations, SLSA + Sigstore): you can prove your download was built by this repository's release
workflow from a specific source commit — not tampered with, not built elsewhere:

```
gh attestation verify polyglot-win-x64.zip --repo MintPlayer/MintPlayer.Polyglot
```

The extracted `polyglot.exe` is attested too, so the same command works on the bare exe. The
verification output names the exact commit and workflow run that produced the binary.

**From source:**

1. You need **Visual Studio 2026** with the *Desktop development with C++* workload (the projects pin
   platform toolset **v145**; VS 2022/v143 is not sufficient).
2. Build the solution:

   ```
   msbuild MintPlayer.Polyglot.sln /p:Configuration=Debug /p:Platform=x64
   ```

3. The self-contained CLI lands at `x64\Debug\MintPlayer.Polyglot.Cli.exe` with its target plugins in
   the `plugins\` folder beside it. Put it on your `PATH` as `polyglot` if you like.

One-shot verification (build → unit tests → watch gate → differential conformance): `pwsh
scripts/build-and-test.ps1`. This runs each target's generated output against the C# oracle, so it needs
the SDKs of the targets it exercises (at minimum `dotnet` + `node` + `python3` — see below).

## Toolchains & SDKs

The `polyglot` CLI itself is a **self-contained native binary with zero runtime dependencies** — you do
not need any of the toolchains below just to *transpile*. You only need a target's SDK to **compile/run
the code emitted for that target** (and to run that target's leg of the differential conformance suite).

| For | Toolchain / SDK | Official download |
|---|---|---|
| **Building the CLI from source** | Visual Studio 2026 — *Desktop development with C++* workload (platform toolset v145) | <https://visualstudio.microsoft.com/> |
| **C# / .NET** output | Emitted code needs **C# 11 and a .NET 8+ BCL** (a consumer that uses `std.collections`' `Map<K, V>` needs **.NET 9+**: it binds `OrderedDictionary<K, V>`); netstandard2.0, .NET Framework, Mono and Unity are not supported. The conformance suite runs on the .NET SDK 10 | <https://dotnet.microsoft.com/download> |
| **TypeScript / JS** output | Node.js (LTS) | <https://nodejs.org/en/download> |
| **Python** output | Python 3 | <https://www.python.org/downloads/> |
| **PHP** output | PHP **8.2+** with the **iconv** extension (`ext-iconv`, on by default; the string std indexes non-ASCII text in UTF-16 units through it) (Windows builds: the NTS x64 zip) | <https://www.php.net/downloads> · Windows: <https://windows.php.net/download/> |
| **Kotlin** output *(P26, in progress)* | Kotlin command-line compiler `kotlinc` + a JDK 17+ | <https://kotlinlang.org/docs/command-line.html> · JDK: <https://adoptium.net/> |
| **Swift** output *(P26, in progress)* | Swift toolchain (Windows/Linux/macOS) | <https://www.swift.org/install/> |
| **Dart** output *(P26, in progress)* | Dart SDK | <https://dart.dev/get-dart> |

Targets marked *(P26, in progress)* are being added on the `p26-second-wave-targets` branch and are not
in a shipped release yet; C# / TypeScript / Python are full targets, PHP is a partial target (see
[Targets are plugins](#targets-are-plugins)).

## Using the CLI

```
polyglot build [<input.pg>...] [--target <name> | --only <a,b>] [--out <dir>] [--root <dir>] [--lib <a,b>]
               [--target-option <target>.<name>=<value>] [--origin-info]
               [--check] [--watch] [--record <file>] [--depfile <file>]
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
  "originInfo": false,
  "forbiddenIdentifiers": { "*": ["temp"] }
}
```

`root` anchors logical imports (`import { a } from "geometry"` → `<root>/geometry.pg`), `lib` is the
ambient std prelude (so `print`/`Math` resolve without imports), `targets` is the project's target set —
a bare `polyglot build foo.pg` then emits **all** of them — and `forbiddenIdentifiers` bans names
project-wide or per target.

`originInfo` (or `--origin-info`, which wins) records where each emitted line came from, so a
coverage run over the **generated** code attributes hits back to the `.pg` — the path *and* the line
numbers. C# gets `#line` pragmas, which Roslyn writes into the PDB, so .NET coverage tools need no
post-processing whatsoever; TypeScript gets a Source Map v3 sidecar (`solver.ts.map`) with the `.pg`
text embedded, so it survives bundling and relocation. How a target records origins is declared in its
own plugin manifest (`originMapping`), so a build whose targets *all* lack one refuses rather than
silently doing nothing. Off by default: output is byte-identical without it. Origin paths are relative
to the generated file, so committed output is identical in every checkout.

**Committed output in CI.** Every generated file starts with
`<auto-generated/>` and ``Generated by Polyglot. Do not edit: regenerate with `polyglot build`.`` in the
target's comment syntax. `polyglot build --check` writes nothing, prints sorted
`polyglot: would create|update|delete <path>` lines and exits **3** on drift (exit codes rank
64/66/69 > 1 > 3 > 0). `--only typescript` builds a subset of the pgconfig `targets` with routing
unchanged (`--target` instead names one override target).

**Stale outputs are cleaned up.** A bare `polyglot build` (which discovers its inputs from the `include`
patterns) keeps an outputs record in `<configDir>/.polyglot/outputs.json` (the folder ignores itself)
and deletes what an earlier build produced and this one did not — a renamed `.pg` leaves no stale twin.
With explicit inputs, `--record <file>` does the same (MSBuild passes one). An include rule with
`"owned": true` also removes stale twins from its output folder on a fresh clone with no record; that
folder must be a dedicated subfolder (an owned rule whose literal folder is empty, the config folder, one
of its ancestors, a filesystem root, or an absolute/rooted path is a config error; `../web/src/gen/…` into
a sibling tree is fine). The deletion rule is strict: only a file whose header **line** is
`// Generated by Polyglot.` (in the target's comment syntax, e.g. `#` for Python) is ever deleted — a
file that merely mentions the phrase, or an `<auto-generated/>` file from NSwag or a Roslyn generator,
never is — a `.map` goes only with its owner, and a build with any failed group deletes nothing.
`--target` + `--out` and `--watch` never delete. The one exception to "never delete" there is the same rule
applied to Polyglot's own leftovers: an output's temp file from an interrupted run (exactly
`<output>.pgtmp.<n>.<n>.<n>` or `<output>.pgcase.tmp`, next to an output this run writes, older than ten
minutes, and carrying the header line itself) is removed. Nothing else in the folder is looked at.

**Hand-written files are never overwritten.** If an output path already holds a file that has neither
Polyglot's header line nor an entry in the outputs record, the build refuses (exit 1, file untouched)
and `--check` reports it as an error rather than `would update`. **Upgrading from 0.10.x:** its outputs
carried no header and it kept no record, so run `polyglot build --adopt` once — it takes over unmarked
files at this build's output paths (they then carry the header and are recorded). Under the MSBuild
package, the `obj/…/polyglot/` folder belongs to the build outright (it passes `--build-owned`), and it
compiles only the `.cs` files the transpile reported, so a headerless stale `.cs` that 0.10.x left there is
ignored. A 0.10.x twin routed **outside** `obj/` (a TypeScript twin in a web app) still needs taking over
once: `dotnet build -p:PolyglotAdopt=true` (the `PolyglotAdopt` property passes `--adopt`). `--adopt` names
every file it takes over, and it is honoured only while the project has **no outputs record yet**: once a
build has written one, the record decides what is Polyglot's, so a flag left set is ignored with a warning
(and the MSBuild targets warn for as long as `PolyglotAdopt` is set). That holds for every invocation — explicit
inputs without `--record` and `--watch` also respect the project's `.polyglot/outputs.json` — and the refusal
then names the real remedy: move the file away, or delete the record and adopt once more. Neither option ever
widens what may be *deleted*.

**Writes are atomic.** Every file is written to a temp file and renamed over the old one. A reader that holds
the old file open with delete sharing (a dev server, .NET's `FileShare.Delete`) keeps reading the old bytes
while the new file takes its place; one that holds it without delete sharing makes the write retry for about
three seconds and then fail (exit 1) — the file is never truncated under a reader.

**Build systems.** `--depfile <file>` writes every input the outputs depend on (each `.pg`, every
`pgconfig.json` and lockfile consulted, `file:` plugins), one path per line, so MSBuild, Nx or make can
key an incremental or cached build on exactly those files. `--outputs-list <file>` writes every file the
run produced, one absolute path per line, so a build system compiles exactly those instead of globbing a
folder. Both files are UTF-8, whatever the system code page. The MSBuild package passes `--record`,
`--depfile`, `--outputs-list` and `--build-owned`: `@(Compile)` takes the `.cs` files of the outputs list
(a stale headerless `.cs` in `obj/` is never compiled), a listed file deleted by hand re-runs the
transpile, and it transpiles before the SDK hashes `@(Compile)`, so deleting a `.pg` recompiles even when
every other generated file is unchanged.

### Target options — namespaces, wrapper class, accessibility

Each target plugin declares the options it accepts; you set them per target under `targetOptions`, at the
project level and/or on an `include` rule (the rule's values override the project's for the files it
matches), or with `--target-option <target>.<name>=<value>` (which wins over both):

```json
{
  "targets": ["csharp", "php"],
  "targetOptions": {
    "csharp": { "namespace": "MyCompany.Strings", "class": "StringsApi", "access": "public" },
    "php":    { "namespace": "MyCompany\\Strings" }
  },
  "include": [
    { "pattern": "geometry/*.pg", "output": "gen/%(Filename)",
      "targetOptions": { "csharp": { "namespace": "MyCompany.Geometry" } } }
  ]
}
```

| Option | Default | Effect |
|---|---|---|
| `csharp.namespace` | *(none)* | wraps each file in a file-scoped `namespace X;` — two libraries built from `.pg` no longer collide on `PolyglotProgram` / `Option<T>` (CS0433) |
| `csharp.class` | `PolyglotProgram` | the static class holding top-level functions and globals; emitted only when it has members, and its name is reserved as an identifier |
| `csharp.access` | *(C# default, `internal`)* | `public` exposes the generated types and wrapper across assemblies (runtime helpers such as `print` stay `internal`) |
| `csharp.entryPoint` | `true` | `false` drops the `static void Main()` an entry file (one declaring `fn main`) otherwise gets — for an app that owns its entry point: its own `Main` (CS0017) or top-level statements (CS7022) calls `main()` itself |
| `php.namespace` | *(none)* | `namespace X;` right after `<?php`, so two libraries may define the same function names |

An unknown option, an unknown target, or a value that fails the option's pattern is refused up front. A
namespace or class must agree across files that import each other (and across the roots of one C# project
that share the runtime prelude file) — a mismatch is refused rather than emitted unlinkable. The former
top-level `"access"` key and `--access` flag are gone: use `targetOptions.csharp.access`.

## Targets are plugins

No language is compiled into the CLI: **a target is one JSON file** (`polyglot-plugin.json` — spec
tables + emission rules + capabilities + std bindings), validated at load so a malformed or incomplete
plugin is refused rather than silently miscompiling. The four first-party targets live in
[`plugins/`](plugins/) and are published to npm:

| Target | Package | Coverage |
|---|---|---|
| C# | [`@mintplayer/polyglot-target-csharp`](https://www.npmjs.com/package/@mintplayer/polyglot-target-csharp) | full §3.A |
| TypeScript | [`@mintplayer/polyglot-target-typescript`](https://www.npmjs.com/package/@mintplayer/polyglot-target-typescript) | full §3.A |
| Python | [`@mintplayer/polyglot-target-python`](https://www.npmjs.com/package/@mintplayer/polyglot-target-python) | full §3.A |
| PHP | [`@mintplayer/polyglot-target-php`](https://www.npmjs.com/package/@mintplayer/polyglot-target-php) | **partial** |

> **PHP is a partial target.** C#, TypeScript, and Python each pass the entire differential conformance
> suite; PHP runs its own `run-php.ps1` leg of it. PHP today covers nearly the whole §3.A surface —
> exceptions, pattern matching, interfaces, closures, with-expressions, extension methods, reference-
> semantics collections (`\ArrayObject`) — but its plugin still declares **operator overloading** and
> **async/await** unsupported, so a program using those is **refused with a clear diagnostic** (never
> miscompiled). Generated PHP **requires `ext-iconv`** (enabled in default PHP builds): the string std
> indexes non-ASCII strings in UTF-16 code units through it, so `len`/`charAt`/`substring` agree with
> the other targets (SPEC §8); it throws a named `Error` if the extension is missing. The remaining parity work is milestone **P26 ("PHP uplift")** — see
> [`docs/prd/PLAN.md`](docs/prd/PLAN.md) §P26.

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

[`MintPlayer.Polyglot.MSBuild`](https://www.nuget.org/packages/MintPlayer.Polyglot.MSBuild) (sources in
[`src/MintPlayer.Polyglot.MSBuild/`](src/MintPlayer.Polyglot.MSBuild/)) makes `dotnet build` transpile
`.pg` files straight into an ordinary C# project — incremental, clean-aware, non-transitive, no extra
SDK (the package embeds the native CLI):

```
dotnet add package MintPlayer.Polyglot.MSBuild
```

The `.pg` sources are registered as `Watch` items, so **`dotnet watch build|run` re-transpiles on every
`.pg` edit** — which is also the watch story inside Visual Studio for .NET hosts (opt out with
`Watch="false"` metadata or `PolyglotWatch=false`). The end-to-end gate is `tests/msbuild/run-nuget.ps1`.

The C# [target options](#target-options--namespaces-wrapper-class-accessibility) are also MSBuild
properties, which win over the project's `pgconfig.json`:

```xml
<PropertyGroup>
  <PolyglotNamespace>MyCompany.Strings</PolyglotNamespace> <!-- csharp.namespace -->
  <PolyglotClass>StringsApi</PolyglotClass>                <!-- csharp.class -->
  <PolyglotAccess>public</PolyglotAccess>                  <!-- csharp.access -->
  <PolyglotEntryPoint>false</PolyglotEntryPoint>           <!-- csharp.entryPoint -->
</PropertyGroup>
```
`PolyglotTargets=csharp` limits a build to a subset of the pgconfig `targets` (empty = all). Renaming or
deleting a `.pg`, or editing `pgconfig.json` or the lockfile, re-runs the transpile, and the stale `.cs`
is removed, so a deleted function no longer links. The recipe for Nx and other cached monorepos, where a
TS twin is committed into an Angular project: set `PolyglotTargets=csharp` in the library's `.csproj`,
and run `polyglot build --check --only typescript` in CI to catch a stale or missing twin.

**Refresh the twin with `polyglot build --only typescript`, never a bare `polyglot build`.** In this layout
the pgconfig declares `"targets": ["csharp", "typescript"]` but only the TypeScript twin has an `include`
rule: under MSBuild the C# goes to `obj/` (the package passes the output folder), while a bare CLI build has
no rule for it and writes each `.cs` **next to its `.pg`**. The SDK's default `**/*.cs` glob then compiles
those copies *and* the `obj/` ones, and the next `dotnet build` fails with CS0111 (CS0101/CS8863 with a
record). If that happened, delete the stray `.cs` files beside the `.pg` sources. CI's check and the refresh
are the same pair of commands, run from the pgconfig's folder:

```
polyglot build --check --only typescript    # CI: exit 3 when the committed twin is stale, missing or orphaned
polyglot build --only typescript            # refresh the twin (writes no C#)
```

**A case-only rename needs `git mv`.** Renaming `Foo.pg` to `foo.pg` renames the twin (and its source
map) to the exact case, and `--check` reports it as `would rename`. On a case-insensitive checkout
(`core.ignorecase=true`, the Windows and macOS default) `git add -A` does not stage a case-only change, so
the commit would keep `gen/Foo.ts` and a case-sensitive clone's check would fail. Polyglot prints the
remedy with the rename; run it before committing, e.g. `git mv -f gen/Foo.ts gen/foo.ts`. A **directory**
the route creates is compared too (`gen/Foo/a.ts` after `src/Foo` became `src/foo` under
`%(RecursiveDir)`, or an output template changed from `Gen/` to `gen/`): the build renames the directory
when everything in it is Polyglot output, and otherwise refuses with the remedy. git cannot `git mv` a
case-only directory rename on Windows, so that remedy restages the index instead:
`git rm -r -q --cached "gen/Foo" && git add "gen/foo"`. Outputs under the build-owned `obj/` folder are
renamed without a hint (they are never tracked).

The emitted C# needs **C# 11 and a .NET 8 BCL** (`std.collections` `Map<K, V>` needs .NET 9; it is
`OrderedDictionary<K, V>`); the `tests/floor` gate compiles the whole conformance corpus at that floor with
`Nullable` and `TreatWarningsAsErrors`.

`csharp.class` renames only the top-level wrapper. Your own `extension` functions live in a separate static
class, `PolyglotExtensions`, in the same namespace, with the same `csharp.access`. The `std` guards
(`_pg_charAt`, `_pg_split`, …) are plain `internal static` methods of the wrapper class, called as
`PolyglotProgram._pg_charAt(s, 1)`; they are not `this string` extensions, so an assembly you grant
`InternalsVisibleTo` sees nothing new on `string`. Give each library its own `csharp.namespace` and these
helpers never meet.

## Testing & code coverage

`pwsh scripts/build-and-test.ps1` is the one-shot gate (build → unit suite → every gate leg →
differential C#/TS/Python/PHP conformance). Coverage is reported by **three instruments that answer
three different questions** — none substitutes for another, because a 100% C++ number with dead
template arms is a lie, and both can be green while the four targets disagree at runtime:

| Question | Instrument |
|---|---|
| Which **C++ Core/CLI** lines ran? | the `coverage` job in `ci.yml` (g++ `--coverage` + gcovr) · locally `pwsh scripts/coverage.ps1` (OpenCppCoverage) |
| Which **plugin template arms** ran? | the arm tracer — `polyglot --emit-arm-trace <file>`, folded into one lcov per manifest by `scripts/arm-trace-to-lcov.ps1` |
| Do the four targets **agree at runtime**? | the differential conformance suite ([`tests/conformance/`](tests/conformance/)) |
| Which **`.pg` lines ran downstream**, in a consumer's own coverage run? | `--origin-info` (P38 / issue #69) — C# `#line` pragmas into the PDB, a v3 source map for TS |

The middle one exists because **the backends *are* their JSON manifests** — zero are compiled in — so
gcov and OpenCppCoverage are structurally blind to them, and the load-time anti-silent-drop contract
proves a rule *exists*, never that it ever *ran*. The tracer marks each rule as the interpreter
evaluates it and takes its denominator from the compiler's own parse, so an arm that never fires is
reported **uncovered** rather than quietly missing from the total. It is a debug flag, off by default.
Both surfaces publish to [coverage.mintplayer.com](https://coverage.mintplayer.com/r/MintPlayer/MintPlayer.Polyglot),
where each `polyglot-plugin.json` is browsable with per-line hit counts.

Two honest caveats: coverage is measured on Linux only, so `#ifdef _WIN32` branches read as
permanently uncovered (don't "improve" the number by deleting a POSIX branch); and reporting is
advisory — there is no coverage floor gating merges today. Design notes:
[`docs/prd/code-coverage-upload/`](docs/prd/code-coverage-upload/).

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
