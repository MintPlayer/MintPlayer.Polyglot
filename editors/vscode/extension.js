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

// The three emit targets: display name, file extension (drives built-in language detection + tab title),
// language id (belt-and-suspenders coloring), and line-comment prefix (for the stale banner).
const TARGETS = {
  csharp:     { name: 'C#',         ext: 'cs', langId: 'csharp',     comment: '//' },
  typescript: { name: 'TypeScript', ext: 'ts', langId: 'typescript', comment: '//' },
  python:     { name: 'Python',     ext: 'py', langId: 'python',     comment: '#'  }
};

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
    synchronize: {
      configurationSection: 'polyglot',
      // Watch pgconfig.json so editing root/lib re-analyzes open .pg files immediately (the server handles
      // workspace/didChangeWatchedFiles by re-analyzing every open document).
      fileEvents: vscode.workspace.createFileSystemWatcher('**/pgconfig.json')
    },
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

  // Colorize those virtual std documents like ordinary .pg files: mark them as the `polyglot` language so
  // the same TextMate grammar (and theme colors) applies. Their scheme isn't in the LSP document selector,
  // so they get grammar highlighting only — the server never analyzes the read-only std source.
  const asPolyglot = (doc) => {
    if (doc.uri.scheme === 'polyglot' && doc.languageId !== 'polyglot') {
      vscode.languages.setTextDocumentLanguage(doc, 'polyglot');
    }
  };
  context.subscriptions.push(vscode.workspace.onDidOpenTextDocument(asPolyglot));
  vscode.workspace.textDocuments.forEach(asPolyglot);

  setupGeneratedPreview(context);

  client.start().catch((err) => {
    vscode.window.showWarningMessage(
      `Polyglot: could not start the language server ('${command} lsp'). ` +
      `Set "polyglot.cliPath" to your built MintPlayer.Polyglot.Cli.exe. (${err && err.message ? err.message : err})`
    );
  });
  // Stop defensively on dispose: if start() failed the client may be mid-'starting', where stop() throws.
  context.subscriptions.push({ dispose: () => { try { if (client) client.stop(); } catch (_e) { /* ignore */ } } });
}

// Live generated-output preview (PRD §4.9 / PLAN §P17). Render the emitted C#/TS/Python for the focused .pg
// into a read-only virtual document (scheme `polyglot-gen:`) opened beside the source, refreshed on-type. The
// server produces the code in memory via the custom `polyglot/emit` request — no file is written. Coloring is
// free: the gen URI carries a .cs/.ts/.py extension (and we set the languageId explicitly), so the built-in
// target-language grammars apply. Read-only is by construction (content-provider docs can't be edited/saved).
function setupGeneratedPreview(context) {
  const cfg = vscode.workspace.getConfiguration('polyglot');
  let currentTarget = context.workspaceState.get('polyglot.previewTarget') || cfg.get('preview.defaultTarget', 'csharp');
  if (!TARGETS[currentTarget]) currentTarget = 'csharp';

  let previewSourceUri = null; // toString() of the .pg the preview currently follows
  let previewOpen = false;     // is a polyglot-gen: tab open?
  let debounce;
  const lastGood = new Map();  // genUri.toString() -> last successful emitted code

  // A deterministic gen URI for (source, target): same inputs -> identical URI, so on-type refresh fires the
  // exact URI of the open preview doc. The source uri + target ride in the query as JSON (VS Code decodes it
  // on `.query` access — the standard content-provider pattern).
  const genUriFor = (srcUri, target) => {
    const base = path.basename(vscode.Uri.parse(srcUri).fsPath).replace(/\.pg$/i, '');
    return vscode.Uri.parse(`polyglot-gen:/${base}.${TARGETS[target].ext}`)
      .with({ query: JSON.stringify({ src: srcUri, target }) });
  };

  const emitter = new vscode.EventEmitter();
  const provider = {
    onDidChange: emitter.event,
    provideTextDocumentContent: async (uri) => {
      let src, target;
      try { ({ src, target } = JSON.parse(uri.query)); } catch (_e) { return '// Polyglot: bad preview URI'; }
      const prefix = (TARGETS[target] || TARGETS.csharp).comment;
      try {
        const res = await client.sendRequest('polyglot/emit', { uri: src, target });
        if (res && res.ok) { lastGood.set(uri.toString(), res.code); return res.code; }
        const n = (res && res.diagnostics && res.diagnostics.length) || 0;
        const banner = `${prefix} Polyglot: ${n} error${n === 1 ? '' : 's'} — showing last successful output (stale)\n`;
        const prev = lastGood.get(uri.toString());
        return prev !== undefined ? banner + prev
          : `${prefix} Polyglot: ${n} error${n === 1 ? '' : 's'} — no successful output yet`;
      } catch (_e) {
        return `${prefix} Polyglot: language server unavailable`;
      }
    }
  };
  context.subscriptions.push(vscode.workspace.registerTextDocumentContentProvider('polyglot-gen', provider));

  const statusItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Right, 100);
  statusItem.command = 'polyglot.selectTarget';
  statusItem.tooltip = 'Polyglot: switch the generated-output preview target';
  context.subscriptions.push(statusItem);
  const updateStatus = (editor) => {
    statusItem.text = `$(file-code) Output: ${TARGETS[currentTarget].name}`;
    if (editor && editor.document.languageId === 'polyglot' && editor.document.uri.scheme === 'file') statusItem.show();
    else statusItem.hide();
  };
  updateStatus(vscode.window.activeTextEditor);

  const refreshPreview = async () => {
    if (!previewOpen || !previewSourceUri) return;
    const genUri = genUriFor(previewSourceUri, currentTarget);
    const doc = await vscode.workspace.openTextDocument(genUri);
    await vscode.languages.setTextDocumentLanguage(doc, TARGETS[currentTarget].langId);
    await vscode.window.showTextDocument(doc, { viewColumn: vscode.ViewColumn.Beside, preview: true, preserveFocus: true });
    emitter.fire(genUri); // re-pull in case the doc was cached from a prior edit
  };

  context.subscriptions.push(
    vscode.commands.registerCommand('polyglot.showOutput', async () => {
      const editor = vscode.window.activeTextEditor;
      if (!editor || editor.document.languageId !== 'polyglot' || editor.document.uri.scheme !== 'file') {
        vscode.window.showInformationMessage('Polyglot: open a .pg file to preview its generated output.');
        return;
      }
      previewSourceUri = editor.document.uri.toString();
      previewOpen = true;
      await refreshPreview();
    }),
    vscode.commands.registerCommand('polyglot.selectTarget', async () => {
      const pick = await vscode.window.showQuickPick(
        Object.keys(TARGETS).map((k) => ({ label: TARGETS[k].name, target: k })),
        { placeHolder: 'Select the Polyglot generated-output target' });
      if (!pick) return;
      currentTarget = pick.target;
      context.workspaceState.update('polyglot.previewTarget', currentTarget);
      updateStatus(vscode.window.activeTextEditor);
      await refreshPreview();
    })
  );

  // Follow the focused .pg — but never follow a gen/std virtual doc (that would thrash the preview).
  context.subscriptions.push(vscode.window.onDidChangeActiveTextEditor((editor) => {
    updateStatus(editor);
    if (!editor || !previewOpen) return;
    const d = editor.document;
    if (d.uri.scheme !== 'file' || d.languageId !== 'polyglot') return;
    if (d.uri.toString() === previewSourceUri) return;
    previewSourceUri = d.uri.toString();
    refreshPreview();
  }));

  // Debounced on-type refresh of the followed source.
  context.subscriptions.push(vscode.workspace.onDidChangeTextDocument((e) => {
    if (!previewOpen || !previewSourceUri) return;
    if (e.document.uri.toString() !== previewSourceUri) return;
    clearTimeout(debounce);
    debounce = setTimeout(() => emitter.fire(genUriFor(previewSourceUri, currentTarget)), 200);
  }));

  // Lifecycle: when the last gen tab closes, stop following; drop a closed source's cached output.
  context.subscriptions.push(vscode.workspace.onDidCloseTextDocument((doc) => {
    if (doc.uri.scheme === 'polyglot-gen') {
      if (!vscode.workspace.textDocuments.some((d) => d.uri.scheme === 'polyglot-gen')) previewOpen = false;
      return;
    }
    if (doc.uri.scheme === 'file' && doc.languageId === 'polyglot') {
      const s = doc.uri.toString();
      for (const k of [...lastGood.keys()]) {
        try { if (JSON.parse(vscode.Uri.parse(k).query).src === s) lastGood.delete(k); } catch (_e) { /* ignore */ }
      }
    }
  }));
}

function deactivate() {
  try { return client ? client.stop() : undefined; } catch (_e) { return undefined; }
}

module.exports = { activate, deactivate };
