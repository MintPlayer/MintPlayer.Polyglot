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

// Live generated-output preview (PRD §4.9 / PLAN §P17). "Show Generated Output" opens all three emitted targets
// (C#/TS/Python) for the focused .pg, each in its own read-only tab (scheme `polyglot-gen:`) beside the source,
// refreshed on-type. The server produces the code in memory via the custom `polyglot/emit` request — no file is
// written. Coloring is free: each gen URI carries a .cs/.ts/.py extension (and we set the languageId), so the
// built-in target-language grammars apply. Read-only is by construction (content-provider docs can't be saved).
// Gen URIs are keyed by target only; content follows the active .pg via previewSourceUri, so the tabs re-render
// in place instead of churning per source. The Explorer "Polyglot Outputs" tree opens a single target on demand.
function setupGeneratedPreview(context) {
  let previewSourceUri = null; // toString() of the .pg the previews currently follow
  let debounce;
  const lastGood = new Map();  // `${target}::${src}` -> last successful emitted code

  // Gen URIs are keyed by TARGET only; the source they render is read from previewSourceUri at render time. So
  // the (up to) three preview tabs follow the active .pg by re-rendering in place — no per-source tab churn.
  const genUriFor = (target) =>
    vscode.Uri.parse(`polyglot-gen:/output.${TARGETS[target].ext}`).with({ query: JSON.stringify({ target }) });

  const emitter = new vscode.EventEmitter();
  const provider = {
    onDidChange: emitter.event,
    provideTextDocumentContent: async (uri) => {
      let target;
      try { ({ target } = JSON.parse(uri.query)); } catch (_e) { target = 'csharp'; }
      const t = TARGETS[target] || TARGETS.csharp;
      const src = previewSourceUri;
      if (!src) return `${t.comment} Polyglot: open a .pg file to preview its ${t.name} output`;
      const key = `${target}::${src}`;
      try {
        const res = await client.sendRequest('polyglot/emit', { uri: src, target });
        if (res && res.ok) { lastGood.set(key, res.code); return res.code; }
        const n = (res && res.diagnostics && res.diagnostics.length) || 0;
        const banner = `${t.comment} Polyglot: ${n} error${n === 1 ? '' : 's'} — showing last successful output (stale)\n`;
        const prev = lastGood.get(key);
        return prev !== undefined ? banner + prev
          : `${t.comment} Polyglot: ${n} error${n === 1 ? '' : 's'} — no successful output yet`;
      } catch (_e) {
        return `${t.comment} Polyglot: language server unavailable`;
      }
    }
  };
  context.subscriptions.push(vscode.workspace.registerTextDocumentContentProvider('polyglot-gen', provider));

  // Re-render every open preview tab (they all follow previewSourceUri).
  const refreshOpen = () => {
    for (const d of vscode.workspace.textDocuments) if (d.uri.scheme === 'polyglot-gen') emitter.fire(d.uri);
  };

  // Open one target's preview beside the source as a permanent tab (preview:false), so multiple targets coexist.
  const openTarget = async (target) => {
    const genUri = genUriFor(target);
    const doc = await vscode.workspace.openTextDocument(genUri);
    await vscode.languages.setTextDocumentLanguage(doc, TARGETS[target].langId);
    await vscode.window.showTextDocument(doc, { viewColumn: vscode.ViewColumn.Beside, preview: false, preserveFocus: true });
    emitter.fire(genUri);
  };

  const activePgUri = () => {
    const e = vscode.window.activeTextEditor;
    return e && e.document.languageId === 'polyglot' && e.document.uri.scheme === 'file' ? e.document.uri.toString() : null;
  };

  context.subscriptions.push(
    // Show ALL generated outputs (C#/TS/Python) for the active .pg — each in its own tab beside the source.
    vscode.commands.registerCommand('polyglot.showOutput', async () => {
      const src = activePgUri();
      if (!src) { vscode.window.showInformationMessage('Polyglot: open a .pg file to preview its generated output.'); return; }
      previewSourceUri = src;
      for (const t of Object.keys(TARGETS)) await openTarget(t);
    }),
    // Open a single (source, target) preview — the command a "Polyglot Outputs" tree leaf runs.
    vscode.commands.registerCommand('polyglot.openGenerated', async (srcUri, target) => {
      if (!TARGETS[target]) return;
      previewSourceUri = srcUri;
      await openTarget(target);
    })
  );

  // Follow the focused .pg: re-render open previews for the newly-active source (never follow a gen/std doc).
  context.subscriptions.push(vscode.window.onDidChangeActiveTextEditor((editor) => {
    if (!editor) return;
    const d = editor.document;
    if (d.uri.scheme !== 'file' || d.languageId !== 'polyglot') return;
    if (d.uri.toString() === previewSourceUri) return;
    previewSourceUri = d.uri.toString();
    refreshOpen();
  }));

  // Debounced on-type refresh of the followed source.
  context.subscriptions.push(vscode.workspace.onDidChangeTextDocument((e) => {
    if (!previewSourceUri || e.document.uri.toString() !== previewSourceUri) return;
    clearTimeout(debounce);
    debounce = setTimeout(refreshOpen, 200);
  }));

  // Drop a closed .pg source's cached output.
  context.subscriptions.push(vscode.workspace.onDidCloseTextDocument((doc) => {
    if (doc.uri.scheme === 'file' && doc.languageId === 'polyglot') {
      const suffix = `::${doc.uri.toString()}`;
      for (const k of [...lastGood.keys()]) if (k.endsWith(suffix)) lastGood.delete(k);
    }
  }));

  // "Polyglot Outputs" tree (discovery): each open .pg expands to C#/TypeScript/Python leaves; clicking a leaf
  // opens that (source, target) preview via polyglot.openGenerated. The tree renders no code — it navigates to
  // the same virtual docs. A `polyglot.hasOutputs` context key hides the Explorer section when no .pg is open.
  const openPg = () => vscode.workspace.textDocuments.filter((d) => d.uri.scheme === 'file' && d.languageId === 'polyglot');
  const treeChanged = new vscode.EventEmitter();
  const treeProvider = {
    onDidChangeTreeData: treeChanged.event,
    getChildren: (el) => {
      if (!el) return openPg().map((d) => ({ kind: 'source', uri: d.uri.toString(), fsPath: d.uri.fsPath }));
      if (el.kind === 'source') return Object.keys(TARGETS).map((t) => ({ kind: 'target', src: el.uri, target: t }));
      return [];
    },
    getTreeItem: (el) => {
      if (el.kind === 'source') {
        const item = new vscode.TreeItem(path.basename(el.fsPath), vscode.TreeItemCollapsibleState.Expanded);
        item.iconPath = new vscode.ThemeIcon('file-code');
        item.resourceUri = vscode.Uri.parse(el.uri);
        return item;
      }
      const item = new vscode.TreeItem(TARGETS[el.target].name, vscode.TreeItemCollapsibleState.None);
      item.description = `.${TARGETS[el.target].ext}`;
      item.iconPath = new vscode.ThemeIcon('symbol-file');
      item.command = { command: 'polyglot.openGenerated', title: 'Open Generated Output', arguments: [el.src, el.target] };
      return item;
    }
  };
  context.subscriptions.push(vscode.window.registerTreeDataProvider('polyglotOutputs', treeProvider));
  const refreshTree = () => {
    vscode.commands.executeCommand('setContext', 'polyglot.hasOutputs', openPg().length > 0);
    treeChanged.fire();
  };
  context.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument(refreshTree),
    vscode.workspace.onDidCloseTextDocument(refreshTree)
  );
  refreshTree();
}

function deactivate() {
  try { return client ? client.stop() : undefined; } catch (_e) { return undefined; }
}

module.exports = { activate, deactivate };
