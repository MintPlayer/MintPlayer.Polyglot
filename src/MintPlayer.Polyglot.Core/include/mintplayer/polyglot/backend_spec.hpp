#pragma once

#include <string>
#include <unordered_map>

// Declarative, per-target emission data — the "70% tabular" half of a backend, extracted from the
// hand-written emitters (see docs/design/backend-spec.md, P9). This grows slice by slice; the shared emit
// engine + the imperative "Hook" tier land in later P9 slices. For now each emitter consults its own spec
// for the data already extracted, with output identical to before (the differential/golden gates enforce it).

namespace mintplayer::polyglot {

struct BackendSpec {
    std::string name; // "csharp" / "typescript"

    // A leaf/scalar `.pg` type name -> its target-language spelling. Captures the per-target leaf mapping
    // (e.g. "i64" -> "long" / "bigint"; TS maps "char" -> "string", C# does not). Structural types
    // (List / Iterable / Error / tuple / function / nullable / generics) are still built in the emitter.
    std::unordered_map<std::string, std::string> scalarType;
};

} // namespace mintplayer::polyglot
