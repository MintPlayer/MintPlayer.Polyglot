# Polyglot editor tooling

Editor integration for the `.pg` language. The guiding principle: **language intelligence is written once,
in the C++ frontend, and every editor is a thin client over it.** Concretely:

- **Syntax highlighting** is a single declarative **TextMate grammar** (`grammars/polyglot.tmLanguage.json`).
  It is consumed *natively* by both VS Code and Visual Studio 2022+ — no compiler dependency, no duplication.
- **Formatting** and **diagnostics** shell out to the existing CLI (`polyglot fmt`, `polyglot check --json`).
- **Go-to-definition / hover / completion** will come from a shared **`polyglot lsp`** server (Tier 2, below),
  so VS Code and Visual Studio share one implementation instead of two.

## Layout

```
editors/
  grammars/polyglot.tmLanguage.json   # THE grammar — single source of truth, shared by every editor
  vscode/                             # VS Code extension (thin client)
    package.json                      #   language + grammar contribution, formatter, diagnostics, settings
    language-configuration.json       #   comments / brackets / auto-close
    extension.js                      #   formatter + diagnostics providers (call the CLI)
  vs/                                 # Visual Studio integration (planned — consumes the same grammar)
```

## Tiers

**Tier 1 — highlighting + formatting + diagnostics (no new language-analysis code).**
- Highlighting: the TextMate grammar. Keep it in sync with `src/.../lexer.cpp` (keywords) and
  `docs/lang/grammar.ebnf`. Done for VS Code; Visual Studio consumes the same file (see below).
- Formatting: `polyglot fmt <file>` (the round-trip printer) — the extension pipes the buffer through it.
- Diagnostics: `polyglot check <file> --json` runs the frontend (lex/parse/sema + capability gating for the
  C# reference target) and prints `[{line,col,severity,message}]`; the extension turns those into squiggles
  on open and save. (Live-on-type waits for the LSP, which holds the buffer in memory.)

**Tier 2 — `polyglot lsp` (the intelligence layer).** A Language Server built on the frontend-as-a-library,
adding go-to-definition, hover, completion, and document symbols. This needs a new capability in the
frontend: a **position-indexed semantic model** (retain the resolved symbol table and map a source position
→ symbol → definition position). Both VS Code and Visual Studio then become thin `vscode-languageclient` /
VS-LSP clients over the same server. Not started yet.

## Running the VS Code extension (dev)

The extension is plain JS — no build step. Point a VS Code *Extension Development Host* at it:

```
code --extensionDevelopmentPath=editors/vscode <a folder with .pg files>
```

or open `editors/vscode` in VS Code and press **F5**. Then:

1. Open a `.pg` file — it colorizes immediately (grammar).
2. Set **`polyglot.cliPath`** to your built CLI (a source checkout builds it at
   `x64/Debug/MintPlayer.Polyglot.Cli.exe`); leave it as `polyglot` if the CLI is on `PATH`.
3. **Format Document** (Shift+Alt+F) reformats via `polyglot fmt`.
4. Errors appear as you open/save (via `polyglot check --json`).

> Note: `package.json` references the canonical grammar at `../grammars/…` so there is a single source of
> truth during development. Packaging with `vsce` (which bundles only files under the extension root) will
> add a pre-package step that copies the grammar into `vscode/syntaxes/`.

## Visual Studio (planned)

VS 2022+ loads TextMate grammars, so `grammars/polyglot.tmLanguage.json` gives coloring with no rework. A
VSIX will register the `.pg` file type and, in Tier 2, host an LSP client pointing at `polyglot lsp`. The
repo toolchain is already VS-2026/v145 (see the root `CLAUDE.md`), which the VSIX will target.
