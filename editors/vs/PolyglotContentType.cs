using System.ComponentModel.Composition;
using Microsoft.VisualStudio.LanguageServer.Client;
using Microsoft.VisualStudio.Utilities;

namespace MintPlayer.Polyglot.VisualStudio
{
    // Defines the `polyglot` content type and maps `.pg` files to it. Exporting these (as MEF) is what makes
    // Visual Studio open `.pg` buffers as `polyglot` content and route them to the exported ILanguageClient.
    //
    // The BaseDefinition MUST be `code.remote` (CodeRemoteContentDefinition) — LSP-backed content types derive
    // from it, and without it VS won't wire the language-server plumbing to the editor buffer.
    internal static class PolyglotContentDefinition
    {
        public const string ContentTypeName = "polyglot";

        [Export]
        [Name(ContentTypeName)]
        [BaseDefinition(CodeRemoteContentDefinition.CodeRemoteContentTypeName)]
        internal static ContentTypeDefinition PolyglotContentType;

        [Export]
        [FileExtension(".pg")]
        [ContentType(ContentTypeName)]
        internal static FileExtensionToContentTypeDefinition PolyglotFileExtension;
    }
}
