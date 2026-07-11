// Polyglot VS Code extension — a thin client over the `polyglot lsp` language server.
//
// Highlighting is contributed declaratively (package.json + ../grammars). Everything else — diagnostics,
// go-to-definition, hover, document symbols, and formatting — comes from the server, which VS Code drives
// over stdio via vscode-languageclient. The server is the C++ frontend (`polyglot lsp`), so VS Code and (a
// future) Visual Studio share one implementation. See editors/README.md and PRD §4.8.

const vscode = require('vscode');
const path = require('path');
const fs = require('fs');
const { execFileSync } = require('child_process');
const { LanguageClient, TransportKind } = require('vscode-languageclient/node');

let client;

// The released CLI binary's name (native C++, not .NET despite the source-project name), and where a user
// installs it from when the discovery ladder comes up empty.
const CLI_BASENAME = process.platform === 'win32' ? 'polyglot.exe' : 'polyglot';
const RELEASES_URL = 'https://github.com/MintPlayer/MintPlayer.Polyglot/releases';

// The three emit targets: display name, file extension (drives built-in language detection + tab title),
// language id (belt-and-suspenders coloring), and line-comment prefix (for the stale banner).
// FIXME(P10): this must not stay hardcoded — the real target set is the CLI's backend registry (plus any
// downloadable P10 backends). Derive it from a server-advertised target list (`polyglot/targets`) instead.
// See PRD §4.9 (deferred) / PLAN §P17 deferred tail + §P10.
const TARGETS = {
  csharp:     { name: 'C#',         ext: 'cs', langId: 'csharp',     comment: '//' },
  typescript: { name: 'TypeScript', ext: 'ts', langId: 'typescript', comment: '//' },
  python:     { name: 'Python',     ext: 'py', langId: 'python',     comment: '#'  }
};

// First existing regular file among `candidates`, or null.
function firstExisting(candidates) {
  for (const p of candidates) {
    try { if (fs.statSync(p).isFile()) return p; } catch (_e) { /* try next */ }
  }
  return null;
}

// A `which`-style PATH probe for the bare `polyglot` command, so PATH becomes a *verifiable* rung (we can
// tell "found" from "not found" before spawning, which is what lets the ladder fail into an actionable modal
// instead of a dead-end ENOENT). Returns the resolved absolute path or null.
function polyglotOnPath() {
  const isWin = process.platform === 'win32';
  const exts = isWin ? (process.env.PATHEXT || '.EXE;.CMD;.BAT;.COM').split(';') : [''];
  for (const dir of (process.env.PATH || '').split(path.delimiter)) {
    if (!dir) continue;
    for (const ext of exts) {
      const full = path.join(dir, 'polyglot' + ext);
      try { if (fs.statSync(full).isFile()) return full; } catch (_e) { /* try next */ }
    }
  }
  return null;
}

/**
 * Find the polyglot CLI via an explicit discovery ladder (PRD §4.15 / PLAN §P23) — define the ENOENT dead end
 * out of existence by locating the binary before we ever spawn it:
 *   1. `polyglot.cliPath` if non-empty — the explicit override. Bare name → PATH lookup; a relative path is
 *      resolved against the workspace root; an absolute path is used as-is. (Semantics unchanged; not probed
 *      for existence — it's the user's deliberate choice, and a wrong value still surfaces the modal on start.)
 *   2. the bundled binary `<extensionPath>/bin/polyglot(.exe)` — the happy path for the platform vsixes
 *      (chmod +x on Unix first, since the vsix is a plain zip that can drop the executable bit).
 *   3. `polyglot` on PATH — a self-installed CLI / universal-fallback users.
 *   4. the source checkout — contributors on this very repo, per platform's build output: on Windows the
 *      MSBuild `<workspace>/x64/{Release,Debug}/MintPlayer.Polyglot.Cli.exe`, on Unix the CMake
 *      `<workspace>/build/polyglot`.
 * Returns the command to spawn, or null when nothing is found (→ the caller shows the actionable modal).
 */
function resolveCli(context) {
  // Rung 1 — explicit override.
  const raw = vscode.workspace.getConfiguration('polyglot').get('cliPath', '');
  const p = raw && raw.trim();
  if (p) {
    if (path.isAbsolute(p)) return p;
    if (p.includes('/') || p.includes('\\')) {
      const ws = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0];
      if (ws) return path.resolve(ws.uri.fsPath, p);
    }
    return p; // bare command → let PATH resolution handle it
  }

  // Rung 2 — bundled binary (the platform-specific vsix payload).
  if (context && context.extensionPath) {
    const bundled = path.join(context.extensionPath, 'bin', CLI_BASENAME);
    try {
      if (fs.statSync(bundled).isFile()) {
        // The vsix is a zip; extraction can strip +x, so restore it before use (rust-analyzer does the same).
        if (process.platform !== 'win32') { try { fs.chmodSync(bundled, 0o755); } catch (_e) { /* best effort */ } }
        // macOS: the bundled CLI is ad-hoc signed (not notarized), so Gatekeeper quarantines a downloaded
        // vsix's binary and refuses to exec it. Strip the quarantine xattr so the server can launch. Best
        // effort — errors if the attr isn't present (common) or xattr is unavailable; neither is fatal.
        if (process.platform === 'darwin') {
          try { execFileSync('xattr', ['-d', 'com.apple.quarantine', bundled], { stdio: 'ignore' }); } catch (_e) { /* not quarantined */ }
        }
        return bundled;
      }
    } catch (_e) { /* not a bundled vsix — fall through */ }
  }

  // Rung 3 — polyglot on PATH.
  const onPath = polyglotOnPath();
  if (onPath) return onPath;

  // Rung 4 — source checkout of this repo (per platform's build output: MSBuild x64\ on Windows, CMake
  // build/ on Unix — the .vcxproj stays the Windows source of truth, CMake mirrors it on Linux).
  const ws = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0];
  if (ws) {
    const root = ws.uri.fsPath;
    const candidates = process.platform === 'win32'
      ? [path.join(root, 'x64', 'Release', 'MintPlayer.Polyglot.Cli.exe'),
         path.join(root, 'x64', 'Debug', 'MintPlayer.Polyglot.Cli.exe')]
      : [path.join(root, 'build', 'polyglot')];
    const checkout = firstExisting(candidates);
    if (checkout) return checkout;
  }

  // Rung 5 — nothing found; caller surfaces the actionable modal.
  return null;
}

// The rung-5 dead end, made actionable: instead of a plain warning the user can't act on, offer to install
// the CLI, point cliPath at an existing binary, or open settings — then (on locate) start the server for real.
async function promptCliMissing(context, detail) {
  const INSTALL = 'Install the CLI';
  const LOCATE = 'Locate polyglot.exe…';
  const SETTINGS = 'Open Settings';
  const choice = await vscode.window.showErrorMessage(
    `Polyglot: could not start the language server ('polyglot lsp'). The polyglot CLI ('${CLI_BASENAME}') was ` +
    `not found. Install it, or set "polyglot.cliPath" to your ${CLI_BASENAME}.` + (detail ? ` (${detail})` : ''),
    { modal: true },
    INSTALL, LOCATE, SETTINGS
  );
  if (choice === INSTALL) {
    vscode.env.openExternal(vscode.Uri.parse(RELEASES_URL));
  } else if (choice === LOCATE) {
    const picked = await vscode.window.showOpenDialog({
      canSelectMany: false,
      openLabel: 'Select polyglot executable',
      title: 'Locate the polyglot CLI executable',
      filters: process.platform === 'win32' ? { Executable: ['exe'] } : undefined
    });
    if (picked && picked[0]) {
      await vscode.workspace.getConfiguration('polyglot')
        .update('cliPath', picked[0].fsPath, vscode.ConfigurationTarget.Global);
      try { if (client) await client.stop(); } catch (_e) { /* was never started */ }
      client = undefined;
      await startClient(context);
    }
  } else if (choice === SETTINGS) {
    vscode.commands.executeCommand('workbench.action.openSettings', 'polyglot.cliPath');
  }
}

// Create and start the language client against the resolved CLI. Extracted from activate() so it can be
// re-run after the user locates the binary (promptCliMissing) without reloading the window.
async function startClient(context) {
  const command = resolveCli(context);
  if (!command) { await promptCliMissing(context, 'spawn polyglot ENOENT'); return; }

  const cfg = vscode.workspace.getConfiguration('polyglot');
  const ws = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0];
  const serverOptions = {
    run:   { command, args: ['lsp'], transport: TransportKind.stdio },
    debug: { command, args: ['lsp'], transport: TransportKind.stdio }
  };
  const clientOptions = {
    // `file:` = real .pg files; `polyglot:` = the read-only embedded std virtual docs (so they get semantic
    // tokens / hover / go-to-def too, not just TextMate coloring). The server suppresses diagnostics for the
    // latter (analyzing std standalone would raise link-context noise on code the user can't edit).
    documentSelector: [{ scheme: 'file', language: 'polyglot' }, { scheme: 'polyglot', language: 'polyglot' }],
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
  try {
    await client.start();
  } catch (err) {
    await promptCliMissing(context, err && err.message ? err.message : String(err));
  }
}

function activate(context) {
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
  setupWatch(context);

  startClient(context);
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

// Watch mode (PRD §4.13 / PLAN §P21): keep the REAL emitted output files on disk fresh as .pg sources
// change — the disk-file sibling of the in-memory preview above (preview = unsaved on-type emit to a
// virtual doc; watch = saved-file on-change emit to disk). The work is one `polyglot build <file> --watch`
// process wrapped in a VS Code background task, so the Problems panel comes free via the $polyglot-watch
// problemMatcher (package.json — its regexes are the CLI's frozen watch protocol, golden-tested by
// tests/watch/run-watch.ps1). The status-bar toggle and the commands RUN THE TASK — one code path, and
// terminate/restart is VS Code's job, not ours.
function setupWatch(context) {
  const TASK_TYPE = 'polyglot';

  // Build the background Task for one entry file. `file` may be workspace-relative (tasks.json) or
  // absolute (the startWatch command passes the active editor's path).
  const makeWatchTask = (definition, scope) => {
    const folder = scope && scope.uri ? scope.uri.fsPath : undefined;
    const file = path.isAbsolute(definition.file)
      ? definition.file
      : folder ? path.resolve(folder, definition.file) : definition.file;
    const args = ['build', file, '--watch'];
    if (definition.target) args.push('--target', definition.target);
    const task = new vscode.Task(
      definition,
      scope || vscode.TaskScope.Workspace,
      `watch ${path.basename(file)}${definition.target ? ` (${definition.target})` : ''}`,
      TASK_TYPE,
      new vscode.ProcessExecution(resolveCli(context) || 'polyglot', args),
      '$polyglot-watch'
    );
    task.isBackground = true;
    task.presentationOptions = { reveal: vscode.TaskRevealKind.Silent, panel: vscode.TaskPanelKind.Dedicated };
    return task;
  };

  context.subscriptions.push(vscode.tasks.registerTaskProvider(TASK_TYPE, {
    // Offer a ready-made "polyglot: watch" task for the active .pg (discoverable via Run Task).
    provideTasks: () => {
      const e = vscode.window.activeTextEditor;
      if (!e || e.document.languageId !== 'polyglot' || e.document.uri.scheme !== 'file') return [];
      const folder = vscode.workspace.getWorkspaceFolder(e.document.uri);
      return [makeWatchTask({ type: TASK_TYPE, task: 'watch', file: e.document.uri.fsPath }, folder)];
    },
    // Give tasks.json-defined polyglot tasks their execution (VS Code hands us the bare definition).
    resolveTask: (task) => {
      const def = task.definition;
      if (def.type !== TASK_TYPE || def.task !== 'watch' || !def.file) return undefined;
      return makeWatchTask(def, task.scope && task.scope.uri ? task.scope : undefined);
    }
  }));

  // Status-bar toggle: $(eye) idle -> click starts watching the active .pg; $(sync~spin) running ->
  // click stops. State tracks task lifecycle events so tasks started any other way (Run Task,
  // tasks.json) drive the same indicator.
  const status = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 90);
  context.subscriptions.push(status);
  let running = null; // the active TaskExecution, if any

  const renderStatus = () => {
    if (running) {
      status.text = '$(sync~spin) polyglot watch';
      status.tooltip = `Watching ${running.task.name.replace(/^watch /, '')} — click to stop`;
      status.command = 'polyglot.stopWatch';
    } else {
      status.text = '$(eye) polyglot watch';
      status.tooltip = 'Start watching the active .pg (emit output files on change)';
      status.command = 'polyglot.startWatch';
    }
    const e = vscode.window.activeTextEditor;
    const relevant = running || (e && e.document.languageId === 'polyglot' && e.document.uri.scheme === 'file');
    if (relevant) status.show(); else status.hide();
  };

  context.subscriptions.push(
    vscode.commands.registerCommand('polyglot.startWatch', async () => {
      if (running) { vscode.window.showInformationMessage('Polyglot: a watch task is already running.'); return; }
      const e = vscode.window.activeTextEditor;
      if (!e || e.document.languageId !== 'polyglot' || e.document.uri.scheme !== 'file') {
        vscode.window.showInformationMessage('Polyglot: open a .pg file to start watching it.');
        return;
      }
      const folder = vscode.workspace.getWorkspaceFolder(e.document.uri);
      await vscode.tasks.executeTask(
        makeWatchTask({ type: TASK_TYPE, task: 'watch', file: e.document.uri.fsPath }, folder));
    }),
    vscode.commands.registerCommand('polyglot.stopWatch', () => {
      if (running) running.terminate();
    }),
    vscode.tasks.onDidStartTask((ev) => {
      if (ev.execution.task.definition.type === TASK_TYPE) { running = ev.execution; renderStatus(); }
    }),
    vscode.tasks.onDidEndTask((ev) => {
      if (running && ev.execution === running) { running = null; renderStatus(); }
    }),
    vscode.window.onDidChangeActiveTextEditor(renderStatus)
  );
  renderStatus();
}

function deactivate() {
  try { return client ? client.stop() : undefined; } catch (_e) { return undefined; }
}

module.exports = { activate, deactivate };
