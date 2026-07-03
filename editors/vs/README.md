# Polyglot — Visual Studio extension (P16d)

A Visual Studio LSP-client VSIX for Polyglot (`.pg`). It drives the **same** `polyglot lsp` language server
the VS Code extension uses (`src/MintPlayer.Polyglot.Cli`), so no language analysis is duplicated per editor.

## What it provides (v1)

Everything the server advertises, through Visual Studio's `ILanguageClient` with no VS-specific code:

- Live diagnostics (on-type)
- Go-to-definition (same-file + cross-module)
- Hover, document symbols (nav bar), find references, rename
- Formatting (Format Document)
- Completion (incl. member `obj.`)
- Semantic-token coloring (refines the TextMate grammar)

Coloring floor is the **shared TextMate grammar** (`editors/vscode/syntaxes/polyglot.tmLanguage.json` +
`language-configuration.json`), copied into the VSIX at build so it stays single-sourced in `editors/vscode`.

**Deferred** (VS-Code-specific glue, no turnkey VS analogue — future slices via `ILanguageClientCustomMessage2`):
the `polyglot:` std virtual-doc click-through (`polyglot/moduleSource`) and the generated-output preview
(`polyglot/emit`). Go-to-def into std resolves to a `polyglot:…` URI VS can't open yet, and no-ops gracefully.

## Requirements

- **Visual Studio 2026 ("18" generation)** with the **Visual Studio extension development** workload (VSSDK).
- A built `polyglot` CLI. The extension finds it, in order:
  1. the `POLYGLOT_CLI` environment variable (absolute path to `MintPlayer.Polyglot.Cli.exe`),
  2. a CLI bundled in the extension (`cli\MintPlayer.Polyglot.Cli.exe` beside the assembly — not bundled by default yet),
  3. `polyglot` on `PATH`.

  For a source checkout, set `POLYGLOT_CLI` to `…\x64\Debug\MintPlayer.Polyglot.Cli.exe`.

## Build (headless)

```
"C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\MSBuild.exe" ^
  editors\vs\MintPlayer.Polyglot.VisualStudio.csproj ^
  /p:Configuration=Release /p:Platform=AnyCPU /p:VisualStudioVersion=18.0 /t:Restore,Rebuild
```

Produces `editors\vs\bin\Release\MintPlayer.Polyglot.VisualStudio.vsix`.

## Test (interactive — required; cannot be done headlessly)

Testing a VS extension needs the **experimental instance** (a GUI Visual Studio session):

1. Open `editors\vs\MintPlayer.Polyglot.VisualStudio.csproj` in VS 2026 and press **F5** (it launches
   `devenv /rootsuffix Exp` with the freshly-built VSIX deployed), **or** double-click the built `.vsix` to install
   it into your main VS.
2. Set `POLYGLOT_CLI` (see above) so the server can start.
3. Open a `.pg` file — you should get coloring immediately (TextMate), then diagnostics/hover/go-to-def/etc. once
   the server initializes.

To install into the main hive from the command line: `"…\Common7\IDE\VSIXInstaller.exe" <path-to>.vsix`.

## Layout

- `MintPlayer.Polyglot.VisualStudio.csproj` — legacy VSSDK project, `net472`, `InstallationTarget [17.0,)`.
- `source.extension.vsixmanifest` — VSIX manifest (single MEF-component asset).
- `PolyglotContentType.cs` — the `polyglot` content type (`BaseDefinition = code.remote`) + `.pg` association.
- `PolyglotLanguageClient.cs` — the `ILanguageClient` that launches `polyglot lsp` over stdio.
- `PolyglotCli.cs` — CLI-path resolution.
- `Grammars/polyglot/` — the grammar bundle (`package.json` is committed; the grammar + language-config are copied
  from `editors/vscode` at build and git-ignored).
