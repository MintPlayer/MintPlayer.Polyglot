// Polyglot VS Code extension — thin client over the `polyglot` CLI.
//
// Highlighting is contributed declaratively (see package.json + ../grammars). This module adds the two
// features that just shell out to the existing CLI:
//   • formatting  — `polyglot fmt` (the round-trip pretty-printer) on the current buffer.
//   • diagnostics — `polyglot check --json` on open/save, surfaced as squiggles.
//
// Deeper intelligence (go-to-definition, hover, completion) is deliberately NOT here: it belongs in the
// shared `polyglot lsp` server (Tier 2) so VS Code and Visual Studio are both thin clients over one C++
// core — not two reimplementations. See editors/README.md.

const vscode = require('vscode');
const { execFile } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

/** Resolve the configured CLI path (defaults to `polyglot` on PATH). */
function cliPath() {
  return vscode.workspace.getConfiguration('polyglot').get('cliPath', 'polyglot');
}

/** Run the CLI with args; resolve with {code, stdout, stderr} (never rejects on a non-zero exit). */
function runCli(args, cwd) {
  return new Promise((resolve) => {
    execFile(cliPath(), args, { cwd, maxBuffer: 8 * 1024 * 1024 }, (err, stdout, stderr) => {
      resolve({ code: err && typeof err.code === 'number' ? err.code : err ? 1 : 0, stdout, stderr, spawnError: err && err.code === 'ENOENT' ? err : null });
    });
  });
}

let cliMissingWarned = false;
function warnCliMissing() {
  if (cliMissingWarned) return;
  cliMissingWarned = true;
  vscode.window.showWarningMessage(
    `Polyglot: could not run the CLI ('${cliPath()}'). Set "polyglot.cliPath" to your built MintPlayer.Polyglot.Cli.exe.`
  );
}

// ---- formatting -----------------------------------------------------------------------------------------

/** Format the whole document by piping its (possibly unsaved) text through `polyglot fmt` via a temp file. */
async function formatDocument(document) {
  const tmp = path.join(os.tmpdir(), `polyglot-fmt-${process.pid}-${Date.now()}.pg`);
  try {
    fs.writeFileSync(tmp, document.getText());
    const { code, stdout, spawnError } = await runCli(['fmt', tmp]);
    if (spawnError) { warnCliMissing(); return []; }
    if (code !== 0 || !stdout) return []; // parse error — leave the buffer untouched (diagnostics show why)
    const fullRange = new vscode.Range(
      document.positionAt(0),
      document.positionAt(document.getText().length)
    );
    return [vscode.TextEdit.replace(fullRange, stdout)];
  } catch (_e) {
    return [];
  } finally {
    fs.rm(tmp, () => {});
  }
}

// ---- diagnostics ----------------------------------------------------------------------------------------

/** Turn a 1-based {line,col} from the CLI into a VS Code range covering the word (or one char) at that spot. */
function rangeAt(document, line, col) {
  const pos = new vscode.Position(Math.max(0, line - 1), Math.max(0, col - 1));
  const word = document.getWordRangeAtPosition(pos);
  return word || new vscode.Range(pos, pos.translate(0, 1));
}

/** Run `polyglot check --json` against the saved file and publish diagnostics for it. */
async function checkDocument(document, collection) {
  const cfg = vscode.workspace.getConfiguration('polyglot');
  if (!cfg.get('diagnostics.enabled', true)) { collection.delete(document.uri); return; }
  if (document.isUntitled) return; // check reads a path; unsaved buffers wait for the LSP (Tier 2)

  const file = document.uri.fsPath;
  const folder = vscode.workspace.getWorkspaceFolder(document.uri);
  const root = folder ? folder.uri.fsPath : path.dirname(file);
  const lib = cfg.get('lib', 'io,math');

  const args = ['check', file, '--json', '--root', root];
  if (lib) args.push('--lib', lib);

  const { stdout, spawnError } = await runCli(args, path.dirname(file));
  if (spawnError) { warnCliMissing(); return; }

  let items = [];
  try { items = JSON.parse(stdout || '[]'); } catch (_e) { return; }

  const diags = items.map((d) => {
    const diag = new vscode.Diagnostic(
      rangeAt(document, d.line, d.col),
      d.message,
      d.severity === 'warning' ? vscode.DiagnosticSeverity.Warning : vscode.DiagnosticSeverity.Error
    );
    diag.source = 'polyglot';
    return diag;
  });
  collection.set(document.uri, diags);
}

// ---- activation -----------------------------------------------------------------------------------------

function activate(context) {
  const collection = vscode.languages.createDiagnosticCollection('polyglot');
  context.subscriptions.push(collection);

  context.subscriptions.push(
    vscode.languages.registerDocumentFormattingEditProvider('polyglot', {
      provideDocumentFormattingEdits: (document) => formatDocument(document)
    })
  );

  const check = (doc) => { if (doc.languageId === 'polyglot') checkDocument(doc, collection); };
  context.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument(check),
    vscode.workspace.onDidSaveTextDocument(check),
    vscode.workspace.onDidCloseTextDocument((doc) => collection.delete(doc.uri))
  );
  // Check documents already open when the extension activates.
  vscode.workspace.textDocuments.forEach(check);
}

function deactivate() {}

module.exports = { activate, deactivate };
