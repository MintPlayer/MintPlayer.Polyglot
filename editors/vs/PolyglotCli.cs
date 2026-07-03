using System;
using System.IO;
using System.Reflection;

namespace MintPlayer.Polyglot.VisualStudio
{
    // Resolves the `polyglot` CLI (which hosts the language server via `polyglot lsp`). Mirrors the VS Code
    // client's cliPath idea, in precedence order:
    //   1. the POLYGLOT_CLI environment variable (an absolute path to the exe),
    //   2. a CLI bundled inside this extension (a `cli\MintPlayer.Polyglot.Cli.exe` next to the assembly),
    //   3. `polyglot` on PATH.
    // A Tools -> Options page (the VS analogue of the polyglot.cliPath setting) is a planned follow-up (PLAN §P16d).
    internal static class PolyglotCli
    {
        public static string Resolve()
        {
            var env = Environment.GetEnvironmentVariable("POLYGLOT_CLI");
            if (!string.IsNullOrEmpty(env) && File.Exists(env))
                return env;

            var dir = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
            if (dir != null)
            {
                var bundled = Path.Combine(dir, "cli", "MintPlayer.Polyglot.Cli.exe");
                if (File.Exists(bundled))
                    return bundled;
            }

            return "polyglot"; // rely on PATH
        }
    }
}
