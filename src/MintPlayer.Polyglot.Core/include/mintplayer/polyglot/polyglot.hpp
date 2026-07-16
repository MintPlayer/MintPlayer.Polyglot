#pragma once

#include <optional>
#include <string>
#include <vector>

#include "mintplayer/polyglot/ast.hpp"
#include "mintplayer/polyglot/diagnostics.hpp"
#include "mintplayer/polyglot/semantic_model.hpp"

// MintPlayer.Polyglot core library — public surface.
//
// P2 (walking-skeleton MVP): the pipeline lexer -> parser -> typed tree IR -> C#/TS backends is wired
// end to end for a minimal language subset. `compile()` is the deep facade over all of it; the per-pass
// pieces (lexer/parser/sema/emit) are internal headers. See docs/prd/PLAN.md.

namespace mintplayer::polyglot {

// The compiler facade. `version()` returns the toolchain's semantic version — injected at build time by the
// release pipeline (see polyglot.cpp / -DPOLYGLOT_VERSION), NOT a committed constant. This is the single
// definition point (PRD §4.16): only Core's polyglot.cpp compiles the version in; everything else calls
// version(). The pipeline is exposed via compile() below.
class Compiler {
public:
    static std::string version();
};

class Backend; // internal emit interface (backend.hpp); opaque on the public surface

// A validated, immutable reference to a loaded backend — what compile() emits with (P18, PRD §4.10).
// The compiled-in `Target` enum is gone: a target is a *name*, resolved once via findTarget() for the
// built-in backends today, and by a future loadBackend(specBytes) for installed data plugins — both
// yield this same handle type, so compile()'s signature never changes again. Validation happens at
// resolve time, not at emit: an unknown name yields a !ok() handle carrying the error, and compile()
// with it refuses with that diagnostic (never crashes, never guesses a default).
class BackendHandle {
public:
    BackendHandle() = default;
    bool ok() const { return backend_ != nullptr; }
    const std::string& name() const { return name_; }     // the requested target name (set even when !ok)
    const std::string& error() const { return error_; }   // why resolution failed ("" when ok)
    const Backend* backend() const { return backend_; }

private:
    friend BackendHandle findTarget(const std::string&);
    const Backend* backend_ = nullptr;
    std::string name_;
    std::string error_;
};

// Resolve a target name ("csharp" / "typescript" / "python") to its registered backend. An unknown
// name returns a !ok() handle whose error() lists the known targets.
BackendHandle findTarget(const std::string& name);

// One emitted output file of a multi-module build (§4.5): `basename` is the file's stem (its imported
// module's canonical basename), `code` its emitted source. `basename` is empty for the entry file (the CLI
// names it after the input). `sourcePath` is the module's canonical origin (an absolute file path for
// disk-resolved modules) — the host routes per-file outputs by it (P30 slice 7); empty for the synthesized
// `__polyglot_prelude` file, which has no source.
struct ModuleFile {
    std::string basename;
    std::string code;
    std::string sourcePath;
};

// Result of compiling one source to one target. On success `ok` is true and `code` holds the emitted
// source; on failure `ok` is false and `diagnostics` explains why (positions are 1-based).
// Module linking (§4.5): `code` is always the entry file. For a multi-module program `modules` additionally
// lists every imported user module's emitted file (each with its own `basename`); it is empty for a
// single-file program (then `code` is the whole output).
struct EmitResult {
    bool ok = false;
    std::string code;
    std::vector<ModuleFile> modules;
    std::vector<Diagnostic> diagnostics;
};

// A module's source, as answered by a ModuleResolver. `canonicalPath` is the stable identity used for
// dedup + cycle detection (e.g. an absolute file path, or the logical module name).
struct ResolvedModule {
    std::string canonicalPath;
    std::string source;
};

// Answers "import specifier (+ importer) -> module source text" so the Core can load cross-file `.pg`
// modules WITHOUT doing any IO itself: the CLI implements this over the filesystem, tests over an
// in-memory map. `std.*` modules are served by the Core's embedded registry before a resolver is consulted,
// so an in-memory resolver still gets the std library for free. Return std::nullopt for "not found" — the
// Core turns that into an "unknown module" diagnostic (resolvers never throw).
class ModuleResolver {
public:
    virtual ~ModuleResolver() = default;
    virtual std::optional<ResolvedModule> resolve(const std::string& specifier,
                                                  const std::string& importerCanonicalPath) = 0;
};

// The workspace "prelude": std modules auto-imported into every file (à la tsconfig `lib`). Each name maps
// to a std module (`"io"` -> `std.io`) and is imported whole-module, but is AMBIENT and lowest-priority — a
// lib-provided declaration loses silently to any user declaration or explicit import of the same name.
// Sourced by the CLI (a `--lib` flag now, `pgconfig.json` "lib" later); the Core just receives the names.
struct LibConfig {
    std::vector<std::string> libs;
    // pgconfig `forbiddenIdentifiers` (P19 design note 7): (target-or-"*", name) pairs refused by checkReservedNames
    // when compiling for a matching target. Project policy, carried with the lib config for plumbing economy.
    std::vector<std::pair<std::string, std::string>> forbiddenIdentifiers;
    // Requested accessibility of emitted top-level C# declarations ("public" / "internal"). Empty = the target
    // default (C# modifier-less top-level types are `internal`), which keeps output byte-identical. Lets a
    // consumer expose the generated types across assemblies without a hand-written public facade. C#-only
    // (TS already `export`s everything; Python/PHP have no equivalent), carried with the lib config.
    std::string access;
    // §4.5 / issue #14: this compile is one unit of a multi-file C# PROJECT build (the CLI sets it when given
    // 2+ inputs). When set, the C# backend hoists the shared runtime prelude (Option/Some/None + the
    // PolyglotProgram wrapper) into a single reserved `__polyglot_prelude` file instead of inlining it, so N
    // independent link roots don't each emit it (which collides as CS0101/CS8863 in one assembly). Off = the
    // single-file behavior (prelude inlined) — byte-identical. C#-only; TS/Python/PHP inline it per file safely.
    bool sharedPrelude = false;
};

// Compile Polyglot source text to a single target. Runs lex -> parse -> (link imported + lib modules) ->
// check -> emit, stopping at the first pass that reports errors (no partial/garbage output on failure).
// A !ok() handle (unknown target) refuses immediately with its resolution error as the diagnostic.
// `resolver` loads non-std (`import … from "./x"` / `"a.b"`) modules; nullptr = std modules only.
// `lib` auto-imports the named std modules (the prelude); empty = none.
EmitResult compile(const std::string& source, const BackendHandle& target, ModuleResolver* resolver = nullptr,
                   const LibConfig& lib = {});

// Re-print source as canonical Polyglot (lex -> parse -> pretty-print). On success `code` holds the
// formatted source. This is the parser-fidelity surface (P3): running it twice is idempotent.
EmitResult format(const std::string& source);

// The non-std `import … from "spec"` specifiers a source declares (lex + parse only — no sema, no resolver).
// The CLI uses this to build a multi-file project's import graph (which inputs are imported by another) so
// it emits each linked module exactly once (§4.5). `std.*` specifiers are excluded (the inlined prelude).
std::vector<std::string> importSpecifiers(const std::string& source);

// Maps a SourcePos.fileId to the source it came from, so cross-module positions stay unambiguous after the
// linker merges every module into one unit (§4.8). Index 0 is unknown/synthetic (the always-linked core
// prelude + anything unstamped). analyze() assigns the entry file id 1 and each transitively loaded module
// the next id. The stored identity is the module's canonical name: an on-disk path for resolver-loaded
// modules (which the LSP turns into a file:// location for cross-module go-to-definition), or a logical
// "std.x" for embedded std modules.
struct SourceMap {
    std::vector<std::string> files{std::string()}; // index 0 reserved for "unknown"
    int add(const std::string& canon) { files.push_back(canon); return static_cast<int>(files.size()) - 1; }
    const std::string& canon(int fileId) const {
        return (fileId > 0 && fileId < static_cast<int>(files.size())) ? files[fileId] : files[0];
    }
};

// The front-end-only result for editor tooling (PRD §4.8): runs lex -> parse -> link -> check and returns
// the checked AST, its diagnostics, a position-indexed `SemanticModel`, and the `SourceMap` naming each
// fileId. This is what `polyglot lsp` queries for diagnostics, go-to-definition, and document symbols.
// Diagnostics are returned even when non-empty; the model is populated when the front end reaches `check`
// (so a file with only type errors still answers go-to-def), and is empty when lex/parse/link fails first.
struct AnalysisResult {
    CompilationUnit unit;                  // the checked (merged) AST — move-only
    std::vector<Diagnostic> diagnostics;
    SemanticModel model;
    SourceMap sources;
};
// `entryPath` names the entry file in the SourceMap (its own definitions get fileId 1); pass the document
// path from the editor. Empty is fine for one-off checks that don't need cross-module locations.
AnalysisResult analyze(const std::string& source, ModuleResolver* resolver = nullptr,
                       const LibConfig& lib = {}, const std::string& entryPath = "");

// The embedded source of a first-party module by logical name ("std.io", "std.math", "std.core", …) — the
// language server serves these as read-only virtual documents so go-to-definition can open a std symbol's
// declaration. Empty when `name` isn't a known embedded module.
std::string embeddedModuleSource(const std::string& name);

} // namespace mintplayer::polyglot
