#include "mintplayer/polyglot/backend.hpp"

#include "mintplayer/polyglot/emit.hpp"

// The first-party backend registry. Each backend is a thin Backend over the hand-written pretty-printer
// for its target (emit.hpp). New targets register here; P9 makes downloaded declarative specs register the
// same way.

namespace mintplayer::polyglot {

const char* featureName(Feature f) {
    switch (f) {
        case Feature::ExtensionMethods:    return "extensionMethods";
        case Feature::OperatorOverloading: return "operatorOverloading";
        case Feature::Properties:          return "properties";
        case Feature::Iterators:           return "iterators";
        case Feature::PatternMatching:     return "patternMatching";
        case Feature::Closures:            return "closures";
        case Feature::Exceptions:          return "exceptions";
        case Feature::Disposal:            return "disposal";
        case Feature::Inheritance:         return "inheritance";
    }
    return "?";
}

namespace {

// C# and TS both emit the entire §3.A surface, so each declares the full capability set. (When a third
// backend with a smaller set lands, the intersection — not this — is what gates source.)
class CSharpBackend : public Backend {
public:
    std::string name() const override { return "csharp"; }
    std::string emit(const ir::Module& m) const override { return emitCSharp(m); }
    bool supports(Feature) const override { return true; }
};

class TypeScriptBackend : public Backend {
public:
    std::string name() const override { return "typescript"; }
    std::string emit(const ir::Module& m) const override { return emitTypeScript(m); }
    bool supports(Feature) const override { return true; }
};

// The third backend (P9 validation / bring-up). Python is colon+indent — a non-brace target that stresses
// the shared engine. It currently emits the walking-skeleton subset (fn/arithmetic/let/if/while/print), so
// it declares NO §3.A advanced feature yet: capability-gating thus refuses any of them targeting Python
// (never a miscompile), and each flag flips to true as the emitter gains that feature.
class PythonBackend : public Backend {
public:
    std::string name() const override { return "python"; }
    std::string emit(const ir::Module& m) const override { return emitPython(m); }
    // Flips to true per feature as emit_python.cpp gains it; the rest stay refused (never miscompiled).
    bool supports(Feature f) const override {
        switch (f) {
            case Feature::Closures:           return true; // expression-bodied lambdas -> Python `lambda`
            case Feature::Iterators:          return true; // a `def` containing `yield` is already a generator
            case Feature::OperatorOverloading: return true; // `operator fn plus` -> a `__add__` dunder
            case Feature::Properties:         return true; // computed property -> `@property`
            case Feature::PatternMatching:    return true; // enum/union + match -> lambda-bound ternary chain
            default: return false;
        }
    }
};

const CSharpBackend kCSharp;
const TypeScriptBackend kTypeScript;
const PythonBackend kPython;
const Backend* const kRegistry[] = {&kCSharp, &kTypeScript, &kPython};

} // namespace

const Backend* findBackend(const std::string& name) {
    for (const Backend* b : kRegistry) if (b->name() == name) return b;
    return nullptr;
}

std::vector<std::string> backendNames() {
    std::vector<std::string> names;
    for (const Backend* b : kRegistry) names.push_back(b->name());
    return names;
}

} // namespace mintplayer::polyglot
