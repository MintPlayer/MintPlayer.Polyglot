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

// A §3.A language feature that a backend may or may not be able to emit on its target (PRD §3.E). The set
// is deliberately finite and closed — one flag per supported-surface feature whose availability genuinely
// varies across SDKs. The usable surface for a build is the *intersection* of the configured backends'
// supported features; using a feature a target lacks is refused at compile time (capability.hpp), distinct
// from a §3.B global refusal. C# and TS both support all of these today, so nothing is gated yet.
enum class Feature {
    ExtensionMethods,    // `extension fn T.m()` keeping `x.m()` call syntax (impossible on Java/Go/C++/PHP)
    OperatorOverloading, // `operator fn plus(...)`
    Properties,          // computed properties / indexers
    Iterators,           // `yield` sequences
    PatternMatching,     // `match` + discriminated unions
    Closures,            // lambdas
    Exceptions,          // throw / try / catch / finally
    Disposal,            // `use` deterministic disposal
    Inheritance,         // class `: Base` + `super`
    Async,               // `async fn` + `await` (single-threaded coroutines); a future PHP-like target may lack it
    BlockLambdas,        // statement-bodied lambdas `x => { … }` (Python lambdas are expression-only)
    WithExpressions,     // record update `expr with { f = v }` (until a target has a ctor-rebuild emission)
};
const char* featureName(Feature f); // stable lowerCamel id for diagnostics, e.g. "extensionMethods"

inline constexpr Feature kAllFeatures[] = {
    Feature::ExtensionMethods, Feature::OperatorOverloading, Feature::Properties, Feature::Iterators,
    Feature::PatternMatching, Feature::Closures, Feature::Exceptions, Feature::Disposal, Feature::Inheritance,
    Feature::Async, Feature::BlockLambdas, Feature::WithExpressions,
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual std::string name() const = 0;                 // stable id, e.g. "csharp" / "typescript"
    virtual std::string emit(const ir::Module& module) const = 0;
    // Whether this backend can emit the given §3.A feature on its target (PRD §3.E capability flag).
    virtual bool supports(Feature f) const = 0;
};

// The registered backend with this name, or nullptr if none.
const Backend* findBackend(const std::string& name);

// Names of all registered backends (for diagnostics / a future `--list-targets`).
std::vector<std::string> backendNames();

} // namespace mintplayer::polyglot
