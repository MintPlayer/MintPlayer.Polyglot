#pragma once

#include "mintplayer/polyglot/ast.hpp"
#include "mintplayer/polyglot/ir.hpp"

// Lowering: AST -> typed IR. Run AFTER sema (it reads the resolved types sema annotated onto Expr nodes).
// This is where syntactic sugar is desugared into the IR's small core and resolved decisions are baked in
// (the `print` intrinsic, the `main` entry point). Currently lowers the MVP subset (functions + scalar
// expressions/statements); it widens to the full §3.A surface alongside the backends in P5.

namespace mintplayer::polyglot {

ir::Module lower(const CompilationUnit& unit);

} // namespace mintplayer::polyglot
