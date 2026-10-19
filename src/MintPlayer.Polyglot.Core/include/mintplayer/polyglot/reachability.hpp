#pragma once

#include <map>
#include <set>
#include <string>

#include "mintplayer/polyglot/ir.hpp"

// P40 §0 — prelude reachability over the lowered IR.
//
// The prelude (embedded std/core/lib declarations, `originModule == ir::kPreludeOrigin`) used to be emitted
// whole into every file that carried it. This pass keeps only the prelude declarations the file's own code
// reaches, so a program that never touches `Option` or `std.io` emits neither (which is also what keeps an
// unused `process` reference out of an Angular bundle).
//
// Roots are every non-prelude declaration left in the module. Edges are every name the IR references —
// declared and resolved types (every expression's type included), callees and their mangled names, static
// receivers, constructions, union cases (a case name reaches its union), pattern constructor cases, globals —
// followed transitively through the prelude declarations reached. As a conservative BACKSTOP the opaque target
// text the IR carries (`extern(...)` code, std binding templates, extern-type spellings, attribute lines) is
// scanned for identifiers that name a prelude declaration: a third-party plugin template that spells a prelude
// name without the IR saying so still keeps it alive. A false positive only keeps a declaration; a false
// negative would fail at runtime, which is why the scan errs wide.
//
// Manifest preludes (the plugin's `preludes` text) are NOT modelled here: they are recorded by `require` while
// a kept declaration is emitted, so pruning a declaration also drops the helpers only it needed.

namespace mintplayer::polyglot {

// The prelude declaration names reachable from `m`'s non-prelude declarations, plus everything reachable from
// `forcedReach` (names reached elsewhere — the union across roots for a shared prelude file). Both a
// declaration's source name and its mangled name are reported, so the set round-trips through
// `pruneUnreachablePrelude` on a re-lowered module.
std::set<std::string> preludeReach(const ir::Module& m, const std::set<std::string>* forcedReach = nullptr);

// Erase every prelude declaration of `m` that is not reachable (as `preludeReach` computes it). Returns the
// reached set.
std::set<std::string> pruneUnreachablePrelude(ir::Module& m, const std::set<std::string>* forcedReach = nullptr);

// P40 review r6 #6 — the user declarations of OTHER modules that the code of `fileOrigin` (its declarations in the
// lowered, not yet partitioned, `m`) spells, keyed by declaring module, each under the name that module emits it
// as. Lowering can reference a name its source never imported (a record or function default filled in at a call
// site in another module), so a target's imports are completed from this set, not only from the source graph.
std::map<std::string, std::set<std::string>> crossModuleReferences(const ir::Module& m, const std::string& fileOrigin);

} // namespace mintplayer::polyglot
