#pragma once

#include <optional>
#include <string>
#include <vector>

#include "mintplayer/polyglot/diagnostics.hpp"

// MintPlayer.Polyglot core library — public surface.
//
// P2 (walking-skeleton MVP): the pipeline lexer -> parser -> typed tree IR -> C#/TS backends is wired
// end to end for a minimal language subset. `compile()` is the deep facade over all of it; the per-pass
// pieces (lexer/parser/sema/emit) are internal headers. See docs/prd/PLAN.md.

namespace mintplayer::polyglot {

// Semantic version of the Polyglot toolchain.
inline constexpr const char* kVersion = "0.0.1";

// The compiler facade. Kept for the version entry point; the pipeline is exposed via compile() below.
class Compiler {
public:
    static std::string version();
};

// A transpilation target. More targets become downloadable declarative backend plugins post-P8
// (see docs/design/plugins-and-targets.md); C# and TS are first-party native backends through P5.
enum class Target { CSharp, TypeScript, Python };

// Result of compiling one source to one target. On success `ok` is true and `code` holds the emitted
// source; on failure `ok` is false and `diagnostics` explains why (positions are 1-based).
struct EmitResult {
    bool ok = false;
    std::string code;
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
};

// Compile Polyglot source text to a single target. Runs lex -> parse -> (link imported + lib modules) ->
// check -> emit, stopping at the first pass that reports errors (no partial/garbage output on failure).
// `resolver` loads non-std (`import … from "./x"` / `"a.b"`) modules; nullptr = std modules only.
// `lib` auto-imports the named std modules (the prelude); empty = none.
EmitResult compile(const std::string& source, Target target, ModuleResolver* resolver = nullptr,
                   const LibConfig& lib = {});

// Re-print source as canonical Polyglot (lex -> parse -> pretty-print). On success `code` holds the
// formatted source. This is the parser-fidelity surface (P3): running it twice is idempotent.
EmitResult format(const std::string& source);

} // namespace mintplayer::polyglot
