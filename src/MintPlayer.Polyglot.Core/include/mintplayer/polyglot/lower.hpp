#pragma once

#include "mintplayer/polyglot/ast.hpp"
#include "mintplayer/polyglot/ir.hpp"

// Lowering: AST -> typed IR. Run AFTER sema (it reads the resolved types sema annotated onto Expr nodes).
// This is where syntactic sugar is desugared into the IR's small core and resolved decisions are baked in
// (the `print` intrinsic, the `main` entry point). Currently lowers the MVP subset (functions + scalar
// expressions/statements); it widens to the full §3.A surface alongside the backends in P5.

namespace mintplayer::polyglot {

// Lower a checked AST to the typed IR FOR ONE TARGET (P19 slice 9): std-binding and extern-class arms are
// resolved to the active target's template here, so the IR carries exactly one template per site.
ir::Module lower(const CompilationUnit& unit, const std::string& target);

} // namespace mintplayer::polyglot
