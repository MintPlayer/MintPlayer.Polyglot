#pragma once

#include "mintplayer/polyglot/ir.hpp"

// Capture analysis (P25 §4.18) — the target-neutral closure-capture pass. For every `ir::Lambda` it computes
// the classified capture list (free variables bound in an enclosing local scope), the authoritative
// `needsCell` decision per capture (the variable is an assignment target somewhere its closures can observe,
// or is self-referential → it must be a shared reference/cell; otherwise a by-value SNAPSHOT is faithful),
// and `capturesThis`. It stamps the decision onto the declaration sites (`ir::Let`/`For`/`Param`.needsCell)
// and the access sites (`ir::Var.throughCell` / `ir::Assign.targetThroughCell`) so each backend's cell
// get/set is a node-local rewrite — the P19 precompute pattern (`lhsIsRecord`/`receiverHasIndexer`).
//
// It runs over the lowered IR (which preserves every name/binding), so a `ir::Var` whose name is bound in no
// enclosing *local* scope is a module global and simply not a capture — no separate symbol table is needed.
// The result is a pure deterministic function of the IR structure, so running it per-target (once per
// `lower()`) yields identical decisions on every target — no cross-target divergence.

namespace mintplayer::polyglot {

// Analyze and stamp capture facts across a lowered module in place. Called by `lower()` before the module
// is returned; also usable from the LSP `analyze()` path.
void analyzeCaptures(ir::Module& m);

} // namespace mintplayer::polyglot
