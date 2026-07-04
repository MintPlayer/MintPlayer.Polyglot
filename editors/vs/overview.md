# MintPlayer Polyglot for Visual Studio

Language support for **Polyglot** (`.pg`) — one small source language that transpiles to idiomatic,
readable **C#**, **TypeScript**, **Python**, and **PHP**.

## Features

- **Syntax highlighting** for `.pg` files.
- **Language server** (the `polyglot lsp` CLI): live diagnostics, go-to-definition (same-file and
  cross-module), hover, document symbols, find references, rename, Format Document, completion
  (including `obj.` members), and semantic-token coloring.
- **Per-target reserved-name checks** — identifiers that would collide with a configured target's
  generated code squiggle live, named per target.

## Requirements

The language features need the **polyglot CLI** on your `PATH` (it hosts the language server).
There is no prebuilt CLI download yet — build it from the
[MintPlayer.Polyglot repository](https://github.com/MintPlayer/MintPlayer.Polyglot) (see the README),
or point the extension at a built `MintPlayer.Polyglot.Cli.exe`.

## Project configuration

An optional `pgconfig.json` next to (or above) your `.pg` files drives module resolution, the ambient
std prelude, and the project's target set:

```json
{
  "root": ".",
  "lib": ["io", "math"],
  "targets": ["csharp", "typescript"]
}
```

## Learn the language

- [Language specification](https://github.com/MintPlayer/MintPlayer.Polyglot/blob/master/docs/lang/SPEC.md)
- [Polyglot for C# developers](https://github.com/MintPlayer/MintPlayer.Polyglot/blob/master/docs/lang/for-csharp-devs.md)
- [Polyglot for TypeScript developers](https://github.com/MintPlayer/MintPlayer.Polyglot/blob/master/docs/lang/for-typescript-devs.md)
