using System;
using System.Collections.Generic;
using System.ComponentModel.Composition;
using System.Diagnostics;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.VisualStudio.LanguageServer.Client;
using Microsoft.VisualStudio.Threading;
using Microsoft.VisualStudio.Utilities;
using Task = System.Threading.Tasks.Task;

namespace MintPlayer.Polyglot.VisualStudio
{
    // A thin Visual Studio LSP client over the same `polyglot lsp` server the VS Code extension drives — no
    // language analysis is reimplemented per editor. VS discovers this MEF component, sees a `.pg` buffer of the
    // `polyglot` content type, calls ActivateAsync, and drives the server over the returned stdio Connection.
    // Every standard capability the server advertises (diagnostics, definition, hover, documentSymbol,
    // semanticTokens, formatting, references, rename, `.`-completion) then works with no further code here.
    [ContentType(PolyglotContentDefinition.ContentTypeName)]
    [Export(typeof(ILanguageClient))]
    public sealed class PolyglotLanguageClient : ILanguageClient
    {
        public string Name => "Polyglot Language Server";

        // Mirror the VS Code client's initializationOptions. `root` is left null: the server locates each file's
        // nearest pgconfig.json itself (its contextFor walk-up), so an explicit workspace root isn't required.
        public object InitializationOptions => new { root = (string)null, lib = "io,math" };

        public IEnumerable<string> ConfigurationSections => null;
        public IEnumerable<string> FilesToWatch => null;
        public bool ShowNotificationOnInitializeFailed => true;

        public event AsyncEventHandler<EventArgs> StartAsync;
        public event AsyncEventHandler<EventArgs> StopAsync;

        public async Task<Connection> ActivateAsync(CancellationToken token)
        {
            await Task.Yield();

            var startInfo = new ProcessStartInfo
            {
                FileName = PolyglotCli.Resolve(),
                Arguments = "lsp",
                RedirectStandardInput = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
                CreateNoWindow = true,
            };

            var process = new Process { StartInfo = startInfo };
            if (!process.Start())
                return null;

            // Connection(reader, writer): the server's stdout is what we READ, its stdin is what we WRITE. Order
            // is load-bearing — swapping these silently breaks all communication.
            return new Connection(process.StandardOutput.BaseStream, process.StandardInput.BaseStream);
        }

        public async Task OnLoadedAsync()
        {
            var handler = StartAsync;
            if (handler != null)
                await handler.InvokeAsync(this, EventArgs.Empty);
        }

        public Task OnServerInitializedAsync() => Task.CompletedTask;

        public Task<InitializationFailureContext> OnServerInitializeFailedAsync(ILanguageClientInitializationInfo initializationState)
        {
            // Surface the server's own message; VS shows it (ShowNotificationOnInitializeFailed = true).
            return Task.FromResult(new InitializationFailureContext
            {
                FailureMessage = initializationState?.StatusMessage ??
                                 "The Polyglot language server ('polyglot lsp') failed to start. " +
                                 "Set POLYGLOT_CLI to your built MintPlayer.Polyglot.Cli.exe, or put 'polyglot' on PATH.",
            });
        }
    }
}
