// Polyglot VS Code extension — a thin client over the `polyglot lsp` language server.
//
// Highlighting is contributed declaratively (package.json + ../grammars). Everything else — diagnostics,
// go-to-definition, hover, document symbols, and formatting — comes from the server, which VS Code drives
// over stdio via vscode-languageclient. The server is the C++ frontend (`polyglot lsp`), so VS Code and (a
// future) Visual Studio share one implementation. See editors/README.md and PRD §4.8.

const vscode = require('vscode');
const path = require('path');
const { LanguageClient, TransportKind } = require('vscode-languageclient/node');

let client;

/**
 * Resolve the CLI path. A bare command name (`polyglot`) is left for a PATH lookup; a relative path is
 * resolved against the workspace root (so a checkout can point at its built x64/Debug exe portably); an
 * absolute path is used as-is.
 */
function resolveCli() {
  const raw = vscode.workspace.getConfiguration('polyglot').get('cliPath', 'polyglot');
  if (path.isAbsolute(raw)) return raw;
  if (raw.includes('/') || raw.includes('\\')) {
    const ws = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0];
    if (ws) return path.resolve(ws.uri.fsPath, raw);
  }
  return raw;
}

function activate(context) {
  const cfg = vscode.workspace.getConfiguration('polyglot');
  const command = resolveCli();
  const ws = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0];

  const serverOptions = {
    run:   { command, args: ['lsp'], transport: TransportKind.stdio },
    debug: { command, args: ['lsp'], transport: TransportKind.stdio }
  };
  const clientOptions = {
    documentSelector: [{ scheme: 'file', language: 'polyglot' }],
    synchronize: { configurationSection: 'polyglot' },
    initializationOptions: {
      root: ws ? ws.uri.fsPath : undefined,
      lib: cfg.get('lib', 'io,math')
    }
  };

  client = new LanguageClient('polyglot', 'Polyglot Language Server', serverOptions, clientOptions);

  // Serve `polyglot:<module>` virtual documents (embedded std sources) so go-to-definition can open a std
  // symbol's declaration read-only. The server returns the source via a custom `polyglot/moduleSource` request.
  context.subscriptions.push(
    vscode.workspace.registerTextDocumentContentProvider('polyglot', {
      provideTextDocumentContent: async (uri) => {
        try {
          const res = await client.sendRequest('polyglot/moduleSource', { uri: uri.toString() });
          return (res && res.source) || `// no embedded source for ${uri.toString()}`;
        } catch (_e) {
          return `// language server unavailable for ${uri.toString()}`;
        }
      }
    })
  );

  client.start().catch((err) => {
    vscode.window.showWarningMessage(
      `Polyglot: could not start the language server ('${command} lsp'). ` +
      `Set "polyglot.cliPath" to your built MintPlayer.Polyglot.Cli.exe. (${err && err.message ? err.message : err})`
    );
  });
  // Stop defensively on dispose: if start() failed the client may be mid-'starting', where stop() throws.
  context.subscriptions.push({ dispose: () => { try { if (client) client.stop(); } catch (_e) { /* ignore */ } } });
}

function deactivate() {
  try { return client ? client.stop() : undefined; } catch (_e) { return undefined; }
}

module.exports = { activate, deactivate };
