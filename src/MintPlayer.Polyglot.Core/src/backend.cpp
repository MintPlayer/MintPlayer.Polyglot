#include "mintplayer/polyglot/backend.hpp"

#include "mintplayer/polyglot/emit.hpp"

// The first-party backend registry. Each backend is a thin Backend over the hand-written pretty-printer
// for its target (emit.hpp). New targets register here; P9 makes downloaded declarative specs register the
// same way.

namespace mintplayer::polyglot {

namespace {

class CSharpBackend : public Backend {
public:
    std::string name() const override { return "csharp"; }
    std::string emit(const ir::Module& m) const override { return emitCSharp(m); }
};

class TypeScriptBackend : public Backend {
public:
    std::string name() const override { return "typescript"; }
    std::string emit(const ir::Module& m) const override { return emitTypeScript(m); }
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
