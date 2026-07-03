#pragma once

#include <vector>

#include "mintplayer/polyglot/ast.hpp"
#include "mintplayer/polyglot/diagnostics.hpp"
#include "mintplayer/polyglot/token.hpp"

namespace mintplayer::polyglot {

// Recursive-descent parse of the MVP token stream into a CompilationUnit. Errors are reported into
// `diags`; the MVP uses minimal recovery (skip to the next likely boundary). Full error recovery is P3.
CompilationUnit parse(const std::vector<Token>& tokens, DiagnosticBag& diags);

} // namespace mintplayer::polyglot
