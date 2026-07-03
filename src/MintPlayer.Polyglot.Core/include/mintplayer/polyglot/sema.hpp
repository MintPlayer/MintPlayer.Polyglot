#pragma once

#include "mintplayer/polyglot/ast.hpp"
#include "mintplayer/polyglot/diagnostics.hpp"
#include "mintplayer/polyglot/semantic_model.hpp"

namespace mintplayer::polyglot {

// Name resolution + type checking for the MVP subset. Annotates every Expr's `type` in place (the
// annotated tree is the typed IR the backends consume). No implicit numeric conversions: an operator's
// operands must share a type. The `print` builtin accepts one i32/f64/bool/string and yields unit.
// Errors are reported into `diags`.
//
// When `model` is non-null, sema records a position-indexed semantic model as a by-product (PRD §4.8):
// definitions and references for the file-local symbols identified by `req`. `compile()` passes nullptr
// and pays nothing; the LSP passes a model to answer go-to-definition / document-symbol queries.
void check(CompilationUnit& unit, DiagnosticBag& diags,
           SemanticModel* model = nullptr, const SemanticRequest* req = nullptr);

} // namespace mintplayer::polyglot
