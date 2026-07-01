#pragma once

#include <string>
#include <vector>

#include "mintplayer/polyglot/backend_spec.hpp"

// JSON (de)serialization for the tabular half of a backend (BackendSpec). This is the first brick of P18
// (PRD §4.10): the ~70% of a backend that is already declarative data becomes loadable from a JSON document,
// so a target's type/literal/operator/bracket tables can ship as plugin data rather than compiled-in C++.
// The imperative emitter Hooks (the ~30%) are migrated to a JSON emission DSL in later P18 slices; for now a
// backend still supplies C++ hooks but reads its Spec data through here. Core stays IO-free — the host hands
// this function the spec bytes; it parses + validates them.

namespace mintplayer::polyglot {

struct SpecLoadResult {
    bool ok = false;
    BackendSpec spec;
    std::vector<std::string> errors; // non-empty iff !ok — a malformed/incomplete spec fails loudly, never silently
};

// Parse a backend-spec JSON document into a BackendSpec. Validates the required `name` and the `blockStyle`
// enum; every other field defaults (matching the BackendSpec struct defaults) when absent.
SpecLoadResult loadBackendSpec(const std::string& json);

// Serialize a BackendSpec back to a JSON document (the inverse of loadBackendSpec, for tests + tooling).
// Map fields serialize in unspecified order (round-trips to an equal struct, not necessarily equal bytes).
std::string backendSpecToJson(const BackendSpec& spec);

} // namespace mintplayer::polyglot
