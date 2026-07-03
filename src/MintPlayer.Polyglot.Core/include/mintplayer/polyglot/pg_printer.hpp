#pragma once

#include <string>

#include "mintplayer/polyglot/ast.hpp"

// Re-print a parsed CompilationUnit as canonical Polyglot (.pg) source. This is the P3 parser-fidelity
// gate: the printer is idempotent on its own output, so for every valid program
//   printSource(parse(printSource(parse(src)))) == printSource(parse(src))
// i.e. nothing the parser needs survives only in the original formatting. The printer grows alongside
// the parser as each grammar feature group is widened.

namespace mintplayer::polyglot {

std::string printSource(const CompilationUnit& unit);

} // namespace mintplayer::polyglot
