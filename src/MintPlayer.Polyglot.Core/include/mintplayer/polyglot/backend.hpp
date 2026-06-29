#pragma once

#include <string>
#include <vector>

#include "mintplayer/polyglot/ir.hpp"

// A code-generation backend turns the typed IR (ir::Module) into target source. This is the seam the P9
// declarative-plugin API grows from: today the C# and TS backends are first-party implementations selected
// through a registry by name, instead of an `if/else` on the target. Adding a target becomes "register a
// Backend"; at P9 a downloaded declarative spec becomes another Backend over this same interface.
//
// (Build-dependency declaration — NuGet PackageReferences, npm deps — attaches here at P10; for now a
// backend just turns IR into a source string.)

namespace mintplayer::polyglot {

class Backend {
public:
    virtual ~Backend() = default;
    virtual std::string name() const = 0;                 // stable id, e.g. "csharp" / "typescript"
    virtual std::string emit(const ir::Module& module) const = 0;
};

// The registered backend with this name, or nullptr if none.
const Backend* findBackend(const std::string& name);

// Names of all registered backends (for diagnostics / a future `--list-targets`).
std::vector<std::string> backendNames();

} // namespace mintplayer::polyglot
