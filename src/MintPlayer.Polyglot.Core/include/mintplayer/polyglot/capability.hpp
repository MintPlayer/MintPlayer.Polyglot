#pragma once

#include <string>
#include <vector>

#include "mintplayer/polyglot/ast.hpp"       // CompilationUnit, SourcePos
#include "mintplayer/polyglot/backend.hpp"    // Feature, Backend
#include "mintplayer/polyglot/diagnostics.hpp"

// Per-target capability gating (PRD §3.E). A backend declares which §3.A features it can emit; the usable
// surface for a build is the *intersection* of the configured backends' supported features. This module
// finds which features a program actually uses and refuses — at compile time, never a miscompile — any a
// configured target can't emit. Distinct from a §3.B global refusal: this depends on the chosen target set.

namespace mintplayer::polyglot {

// A use of a capability, addressed by its string KEY (P37 slice 0): a bare parent (`"iterators"`) or a
// `parent:child` refinement (`"operatorOverloading:eq"`). Keys come from the closed, load-validated
// vocabulary (isKnownCapabilityKey); stance lookup applies the umbrella rule (backend.hpp).
struct FeatureUse {
    std::string key;
    SourcePos pos; // a representative source position (first occurrence) for the diagnostic
};

// Every capability-keyed feature the program uses, each with its first source position.
std::vector<FeatureUse> collectFeatureUses(const CompilationUnit& unit);

// Report (into `diags`) every used feature that `backend` cannot emit. Run once per configured target: a
// feature unsupported by ANY configured backend is thereby rejected, so the net rule is the intersection.
// C#/TS support all features today, so for them this is a no-op.
void checkCapabilities(const CompilationUnit& unit, const Backend& backend, DiagnosticBag& diags);

// Refuse user identifiers colliding with the target's declared reserved names / runtime globals (its
// plugin's `identifiers.reserved` / `identifiers.globals`) or with pgconfig `forbiddenIdentifiers`
// (`forbidden` pairs: target-or-"*" -> name). Identifiers ONLY — declaration sites are walked; string
// literals, comments, and extern("…") templates can never trip it, by construction (P19 §7). This turns
// the silent generated-name collision miscompiles into loud per-target refusals.
void checkReservedNames(const CompilationUnit& unit, const Backend& backend,
                        const std::vector<std::pair<std::string, std::string>>& forbidden,
                        DiagnosticBag& diags);

} // namespace mintplayer::polyglot
