#pragma once

#include <vector>

#include "mintplayer/polyglot/ast.hpp"       // CompilationUnit, SourcePos
#include "mintplayer/polyglot/backend.hpp"    // Feature, Backend
#include "mintplayer/polyglot/diagnostics.hpp"

// Per-target capability gating (PRD §3.E). A backend declares which §3.A features it can emit; the usable
// surface for a build is the *intersection* of the configured backends' supported features. This module
// finds which features a program actually uses and refuses — at compile time, never a miscompile — any a
// configured target can't emit. Distinct from a §3.B global refusal: this depends on the chosen target set.

namespace mintplayer::polyglot {

struct FeatureUse {
    Feature feature;
    SourcePos pos; // a representative source position (first occurrence) for the diagnostic
};

// Every §3.A capability-flagged feature the program uses, each with its first source position.
std::vector<FeatureUse> collectFeatureUses(const CompilationUnit& unit);

// Report (into `diags`) every used feature that `backend` cannot emit. Run once per configured target: a
// feature unsupported by ANY configured backend is thereby rejected, so the net rule is the intersection.
// C#/TS support all features today, so for them this is a no-op.
void checkCapabilities(const CompilationUnit& unit, const Backend& backend, DiagnosticBag& diags);

} // namespace mintplayer::polyglot
