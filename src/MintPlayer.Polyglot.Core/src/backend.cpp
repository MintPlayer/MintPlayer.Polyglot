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

const CSharpBackend kCSharp;
const TypeScriptBackend kTypeScript;
const Backend* const kRegistry[] = {&kCSharp, &kTypeScript};

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
