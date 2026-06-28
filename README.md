# MintPlayer.Polyglot

> A **cross-SDK transpiler**: write logic once in a small, deliberately-scoped source language and
> emit idiomatic, readable code for multiple target SDKs — **C# / .NET** and **TypeScript / JavaScript**
> first.

Polyglot exists to solve the "same logic, two languages, kept in sync by hand" problem (the concrete
motivating case: the [MintPlayer.AI](https://github.com/) FruitCake physics solver, which lives as a
hand-maintained twin in C# and TypeScript). Rather than a general-purpose "compile any language to any
language" tool — a known multi-decade trap — Polyglot is **faithful-by-default with a published list of
relaxations**, and it **refuses** the features that make transpilers balloon (threads, runtime
reflection, finalizers, `decimal`, `unsafe`, bit-exact cross-target floats).

- **Why the name:** a *polyglot* writes in many languages — exactly what the emitter does.
- **Status:** v0 skeleton. The compiler pipeline is not implemented yet.
- **Read first:** [`docs/prd/POLYGLOT_PRD.md`](docs/prd/POLYGLOT_PRD.md) (vision, scope, architecture,
  feature spec) and [`docs/prd/PLAN.md`](docs/prd/PLAN.md) (milestone roadmap).

## Layout

```
MintPlayer.Polyglot.sln
src/
  MintPlayer.Polyglot.Core/    # the compiler library (lexer → parser → typed IR → backends)
  MintPlayer.Polyglot.Cli/     # the `polyglot` command-line front-end
tests/
  MintPlayer.Polyglot.Tests/   # unit + (eventually) differential conformance tests
docs/prd/                      # PRD + plan
```

## Build

Open `MintPlayer.Polyglot.sln` in Visual Studio 2022 (Desktop development with C++ workload) and build,
or from a Developer command prompt:

```
msbuild MintPlayer.Polyglot.sln /p:Configuration=Debug /p:Platform=x64
```

The CLI currently only answers `--version` / `--help`; `build` is a stub until the pipeline lands (see
the plan).
