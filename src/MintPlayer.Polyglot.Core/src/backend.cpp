#include "mintplayer/polyglot/backend.hpp"

#include <memory>
#include <unordered_map>

#include "mintplayer/polyglot/backend_spec_json.hpp"
#include "mintplayer/polyglot/emitter_base.hpp"
#include "mintplayer/polyglot/json.hpp"

// The backend registry (P19 slice 7e). There are NO compiled-in backends: every target — including the
// first-party three — registers by loading its `polyglot-plugin.json` artifact through loadBackend(). The
// CLI is a pure engine; the language data is not welded into it (PRD §4.11 scope decision).

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
        case Feature::Async:               return "async";
        case Feature::BlockLambdas:        return "blockLambdas";
        case Feature::WithExpressions:     return "withExpressions";
    }
    return "?";
}

namespace {

// A backend loaded from a plugin artifact: the spec + rule tables the shared InterpretedEmitter walks,
// the capability map, and (until slice 9's std overlays collapse the per-target IR fields) which
// ExternType/Bound members carry this target's templates.
class LoadedBackend : public Backend {
public:
    LoadedBackend(std::string name, BackendSpec spec, engine::RuleTable rules,
                  std::unordered_map<std::string, bool> capabilities,
                  std::string ir::ExternType::* externField, std::string ir::Bound::* boundField)
        : name_(std::move(name)), spec_(std::move(spec)), rules_(std::move(rules)),
          capabilities_(std::move(capabilities)), externField_(externField), boundField_(boundField) {}

    std::string name() const override { return name_; }

    std::string emit(const ir::Module& m) const override {
        InterpretedEmitter emitter([this]() -> const BackendSpec& { return spec_; }, rules_,
                                   externField_, boundField_);
        return emitter.emit(m);
    }

    // A feature absent from the capability map is supported; a `false` entry gates it (§3.E). Python's
    // artifact declares `"blockLambdas": false` — a Python lambda is expression-only.
    bool supports(Feature f) const override {
        auto it = capabilities_.find(featureName(f));
        return it == capabilities_.end() ? true : it->second;
    }

private:
    std::string name_;
    BackendSpec spec_;
    engine::RuleTable rules_;
    std::unordered_map<std::string, bool> capabilities_;
    std::string ir::ExternType::* externField_;
    std::string ir::Bound::* boundField_;
};

// The registry: populated exclusively by loadBackend(); entries are stable for the process lifetime
// (BackendHandle holds raw pointers into it).
std::vector<std::unique_ptr<LoadedBackend>>& registry() {
    static std::vector<std::unique_ptr<LoadedBackend>> r;
    return r;
}

} // namespace

const Backend* findBackend(const std::string& name) {
    for (const auto& b : registry()) if (b->name() == name) return b.get();
    return nullptr;
}

std::vector<std::string> backendNames() {
    std::vector<std::string> names;
    for (const auto& b : registry()) names.push_back(b->name());
    return names;
}

bool loadBackend(const std::string& artifactJson, std::string& error) {
    json::Value doc = json::parse(artifactJson);
    if (doc.kind != json::Value::Kind::Object) { error = "plugin artifact must be a JSON object"; return false; }
    const std::string name = doc["name"].asString();
    if (name.empty()) { error = "plugin artifact: missing 'name'"; return false; }
    if (findBackend(name)) { error = "plugin '" + name + "' is already loaded"; return false; }

    SpecLoadResult spec = loadBackendSpec(doc["spec"]);
    if (!spec.ok) {
        error = "plugin '" + name + "': " + (spec.errors.empty() ? "invalid spec" : spec.errors.front());
        return false;
    }
    if (spec.spec.name != name) {
        error = "plugin '" + name + "': spec name '" + spec.spec.name + "' does not match";
        return false;
    }

    const json::Value& rulesDoc = doc["rules"];
    if (rulesDoc.kind != json::Value::Kind::Object) { error = "plugin '" + name + "': missing 'rules'"; return false; }
    engine::RuleTable rules;
    for (const auto& kv : rulesDoc.members) {
        bool ok = true;
        std::string err;
        engine::Rule r = engine::parseRule(kv.second, ok, err);
        if (!ok) { error = "plugin '" + name + "': rule '" + kv.first + "': " + err; return false; }
        rules.emplace(kv.first, std::move(r));
    }
    for (const char* required : {"Program", "Type"})
        if (rules.find(required) == rules.end()) {
            error = "plugin '" + name + "': missing required rule '" + std::string(required) + "'";
            return false;
        }

    std::unordered_map<std::string, bool> caps;
    for (const auto& kv : doc["capabilities"].members)
        if (kv.second.kind == json::Value::Kind::Bool) caps[kv.first] = kv.second.boolean;

    // The per-target IR template fields — slice-9 death row (std overlays collapse them to one field).
    const std::string ir = doc["irTemplates"].asString();
    std::string ir::ExternType::* extF;
    std::string ir::Bound::* bndF;
    if (ir == "cs") { extF = &ir::ExternType::csType; bndF = &ir::Bound::csTemplate; }
    else if (ir == "ts") { extF = &ir::ExternType::tsType; bndF = &ir::Bound::tsTemplate; }
    else if (ir == "py") { extF = &ir::ExternType::pyType; bndF = &ir::Bound::pyTemplate; }
    else {
        error = "plugin '" + name + "': 'irTemplates' must be cs|ts|py (until std overlays land)";
        return false;
    }

    registry().push_back(std::make_unique<LoadedBackend>(name, std::move(spec.spec), std::move(rules),
                                                         std::move(caps), extF, bndF));
    return true;
}

} // namespace mintplayer::polyglot
