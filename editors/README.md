# Polyglot editor tooling

Editor integration for the `.pg` language. The guiding principle: **language intelligence is written once,
in the C++ frontend, and every editor is a thin client over it.** Concretely:

- **Syntax highlighting** is a single declarative **TextMate grammar** (`grammars/polyglot.tmLanguage.json`).
  It is consumed *natively* by both VS Code and Visual Studio 2022+ — no compiler dependency, no duplication.
- **Diagnostics, go-to-definition, hover, document symbols, and formatting** come from the **`polyglot lsp`**
  language server — the C++ frontend spoken over stdio (JSON-RPC). VS Code (and a future Visual Studio) are
  thin LSP clients over the *same* server, so the intelligence is written once.

## Layout

```
editors/
  grammars/polyglot.tmLanguage.json   # THE grammar — single source of truth, shared by every editor
  vscode/                             # VS Code extension (thin LSP client)
    package.json                      #   language + grammar contribution, LSP dep, settings
    language-configuration.json       #   comments / brackets / auto-close
    extension.js                      #   starts the LanguageClient (spawns `polyglot lsp`)
  vs/                                 # Visual Studio integration (planned — same grammar + LSP client)
```

## Tiers

**Tier 1 — highlighting (done).** The TextMate grammar. Keep it in sync with `src/.../lexer.cpp` (keywords)
and `docs/lang/grammar.ebnf`. Consumed by VS Code and Visual Studio 2022+ alike.

**Tier 2 — `polyglot lsp` (done for same-file; VS Code client wired).** A zero-dependency Language Server
built on the frontend `analyze()` facade + a position-indexed **semantic model** (`SemanticModel`, built as a
by-product of sema). It serves `publishDiagnostics`, `textDocument/definition`, `hover`, `documentSymbol`, and
`textDocument/formatting`. The VS Code extension is a `vscode-languageclient` client that spawns `polyglot lsp`
over stdio; the shell-out `fmt`/`check` providers it used to have are gone (superseded). Grammar highlighting
stays; semantic tokens (accurate function/variable coloring) will *layer on top* as a follow-up.
*Deferred:* semantic tokens, completion, cross-module go-to-definition (needs file-tracked positions),
find-references/rename, and the Visual Studio client. See PRD §4.8 / PLAN §P16c–d.

## Running the VS Code extension (dev)

**Recommended — open the repo root and press F5.** The repo-root `.vscode/launch.json` *Run Polyglot VS Code
extension* profile has a `preLaunchTask` (`prepare-extension` in `.vscode/tasks.json`) that (1) builds the
solution with the **VS 18 Insiders MSBuild** — this is a C++/v145 project, *not* `dotnet build` — and (2) runs
`npm install` in `editors/vscode` (the extension depends on `vscode-languageclient`). It then starts an
Extension Development Host with the extension loaded and opens `editors/vscode/testbench/`. One keypress = build
CLI + install deps + launch + a ready `.pg` workspace.

The dev host opens `editors/vscode/testbench/` — a `hello.pg` with a pre-wired `polyglot.cliPath` pointing at
the Debug CLI (`x64/Debug/…`, resolved relative to the workspace, so it's portable across checkouts). The
extension starts the language server by spawning that CLI as `polyglot lsp`.

In the dev host:
1. `hello.pg` opens colorized (grammar).
2. **Format Document** (Shift+Alt+F) reformats (server `textDocument/formatting`).
3. Introduce an error (e.g. `let n: i32 = "oops"`) — a squiggle appears **as you type** (the server holds the
   buffer, so no save needed).
4. **Go to Definition** (F12) / hover on a function, type, or local jumps to / describes its declaration.

If you open your own workspace instead, run `npm install` in `editors/vscode` once, set **`polyglot.cliPath`**
(bare `polyglot` uses PATH; a relative path resolves against the workspace root; absolute is used as-is) and
**`polyglot.lib`** (default `io,math`). `polyglot.trace.server` toggles JSON-RPC tracing for debugging.

Manual alternative (opens an arbitrary folder):
```
code --extensionDevelopmentPath=editors/vscode <a folder with .pg files>
```

> Note: `package.json` references the canonical grammar at `../grammars/…` so there is a single source of
> truth during development. Packaging with `vsce` (which bundles only files under the extension root) will
> add a pre-package step that copies the grammar into `vscode/syntaxes/`.

## Visual Studio (planned)

VS 2022+ loads TextMate grammars, so `grammars/polyglot.tmLanguage.json` gives coloring with no rework. A
VSIX will register the `.pg` file type and, in Tier 2, host an LSP client pointing at `polyglot lsp`. The
repo toolchain is already VS-2026/v145 (see the root `CLAUDE.md`), which the VSIX will target.
