# Polyglot for VS Code

Language support for **Polyglot** (`.pg`) — one small source language that transpiles to idiomatic,
readable **C#**, **TypeScript**, **Python**, and **PHP**.

## Features

- **Syntax highlighting** for `.pg` files (TextMate grammar).
- **Language server** (spawned as `polyglot lsp`): live diagnostics, go-to-definition (including into
  the std library), hover, document symbols, semantic tokens, formatting, references, rename, and
  completion (including `obj.` members).
- **Live generated-output preview** — *Show Generated Output* opens the emitted C#, TypeScript, and
  Python beside your `.pg` file and updates as you type. A *Polyglot Outputs* explorer view opens a
  single target on demand.
- **Per-target reserved-name checks** — identifiers that would collide with a configured target's
  generated code or runtime globals squiggle live, named per target.
- **Watch mode** — keep the real emitted output files on disk fresh as you save. The status-bar
  **eye toggle** (or *Polyglot: Start Watch*) runs `polyglot build <file> --watch` as a background
  task for the active `.pg`; errors land in the **Problems panel** (`$polyglot-watch` matcher), and a
  failed rebuild never touches the last good outputs. Also available as a task type for `tasks.json`:

  ```json
  { "type": "polyglot", "task": "watch", "file": "hello.pg", "problemMatcher": "$polyglot-watch" }
  ```

  (Preview shows *unsaved* code in a virtual tab as you type; watch emits *saved* files to disk —
  they complement each other.)

## Requirements

Everything except syntax highlighting needs the **polyglot CLI**, which hosts the language server:

- If `polyglot` is on your `PATH`, it is picked up automatically.
- Otherwise set `polyglot.cliPath` to the executable (a relative path resolves against the
  workspace root).

## Project configuration

An optional `pgconfig.json` next to (or above) your `.pg` files drives module resolution and the
target set:

```json
{
  "root": ".",
  "lib": ["io", "math"],
  "targets": ["csharp", "typescript"]
}
```

`root` is the base for logical imports (`import { a } from "geometry"`), `lib` is the ambient std
prelude, `targets` is the project's target set (drives `polyglot build` and the per-target
reserved-name squiggles). `forbiddenIdentifiers` can ban names project-wide or per target.

## Settings

| Setting | Default | Description |
|---|---|---|
| `polyglot.cliPath` | `polyglot` | Path to the polyglot CLI (which hosts the language server). |
| `polyglot.lib` | `io,math` | Fallback std prelude when no `pgconfig.json` is found. |
| `polyglot.trace.server` | `off` | Trace the JSON-RPC traffic (for debugging the server). |

## Learn the language

- [Language specification](https://github.com/MintPlayer/MintPlayer.Polyglot/blob/main/docs/lang/SPEC.md)
- [Polyglot for C# developers](https://github.com/MintPlayer/MintPlayer.Polyglot/blob/main/docs/lang/for-csharp-devs.md)
- [Polyglot for TypeScript developers](https://github.com/MintPlayer/MintPlayer.Polyglot/blob/main/docs/lang/for-typescript-devs.md)
