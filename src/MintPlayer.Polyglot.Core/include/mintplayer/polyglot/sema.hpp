#pragma once

#include "mintplayer/polyglot/ast.hpp"
#include "mintplayer/polyglot/diagnostics.hpp"

namespace mintplayer::polyglot {

// Name resolution + type checking for the MVP subset. Annotates every Expr's `type` in place (the
// annotated tree is the typed IR the backends consume). No implicit numeric conversions: an operator's
// operands must share a type. The `print` builtin accepts one i32/f64/bool/string and yields unit.
// Errors are reported into `diags`.
void check(CompilationUnit& unit, DiagnosticBag& diags);

} // namespace mintplayer::polyglot
