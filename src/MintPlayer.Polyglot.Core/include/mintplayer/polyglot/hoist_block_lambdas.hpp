#pragma once

#include "mintplayer/polyglot/ir.hpp"

// Block-lambda hoisting (P25 §4.18) — the local-tier lowering for **expression-only-lambda targets** (Python).
// Python's `lambda` can't hold statements, so a block lambda `(a) => { stmts }` is rewritten to a hoisted
// nested `def` (`ir::LocalFunc`) inserted before the enclosing statement, and the lambda expression is
// replaced by a reference to it. Mutated captures (from the capture-analysis pass) become `nonlocal`
// declarations. Runs after `analyzeCaptures` and only for targets that can't emit statement-bodied lambdas;
// every other target keeps block lambdas inline and never invokes this.

namespace mintplayer::polyglot {

void hoistBlockLambdas(ir::Module& m);

} // namespace mintplayer::polyglot
