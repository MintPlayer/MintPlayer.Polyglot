#pragma once

#include <string>

// MintPlayer.Polyglot core library.
//
// v0 is a skeleton: this exposes only a version facade. The pipeline it will grow into
// (lexer -> parser -> typed tree IR -> C#/TS backends) is described in docs/prd/POLYGLOT_PRD.md.

namespace mintplayer::polyglot {

// Semantic version of the Polyglot toolchain.
inline constexpr const char* kVersion = "0.0.1";

// The compiler facade. Intentionally tiny for now; real entry points (parse/check/emit) arrive in P2+.
class Compiler {
public:
    // Returns the toolchain version string.
    static std::string version();
};

} // namespace mintplayer::polyglot
