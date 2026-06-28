#pragma once

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
enum class Target { CSharp, TypeScript };

// Result of compiling one source to one target. On success `ok` is true and `code` holds the emitted
// source; on failure `ok` is false and `diagnostics` explains why (positions are 1-based).
struct EmitResult {
    bool ok = false;
    std::string code;
    std::vector<Diagnostic> diagnostics;
};

// Compile Polyglot source text to a single target. Runs lex -> parse -> check -> emit, stopping at the
// first pass that reports errors (no partial/garbage output on failure).
EmitResult compile(const std::string& source, Target target);

} // namespace mintplayer::polyglot
