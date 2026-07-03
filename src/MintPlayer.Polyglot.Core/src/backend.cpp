#include "mintplayer/polyglot/backend.hpp"

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
    LoadedBackend(std::string name, std::string fileExtension, BackendSpec spec, engine::RuleTable rules,
                  std::unordered_map<std::string, std::string> capabilities,
                  std::unordered_map<std::string, std::string> overlays)
        : name_(std::move(name)), ext_(std::move(fileExtension)), spec_(std::move(spec)),
          rules_(std::move(rules)), capabilities_(std::move(capabilities)), overlays_(std::move(overlays)) {}

    std::string name() const override { return name_; }

    std::string emit(const ir::Module& m) const override {
        InterpretedEmitter emitter([this]() -> const BackendSpec& { return spec_; }, rules_);
        return emitter.emit(m);
    }

    // Tri-state capabilities (PRD §4.11): a feature absent from the map is supported; `"false"` gates it
    // at compile time (§3.E — Python's `"blockLambdas": false`); `"native"`/`"emulated"` document HOW a
    // covered feature emits (validation reads them; the gate only refuses on `"false"`).
    bool supports(Feature f) const override {
        auto it = capabilities_.find(featureName(f));
        return it == capabilities_.end() || it->second != "false";
    }

    const std::unordered_map<std::string, std::string>& stdOverlays() const override { return overlays_; }
    std::string fileExtension() const override { return ext_; }

private:
    std::string name_;
    std::string ext_;
    BackendSpec spec_;
    engine::RuleTable rules_;
    std::unordered_map<std::string, std::string> capabilities_;
    std::unordered_map<std::string, std::string> overlays_; // flattened std FFI templates
};

// ---- Load-time validation (P19 slice 8) ------------------------------------------------------------------

// The fixed builtin catalog — every `{"fn":…}` a rule may name. A name outside this set evaluated to ""
// SILENTLY before this check existed (the slice-6d gate caught exactly that); now it fails the load.
const std::unordered_set<std::string>& fnCatalog() {
    static const std::unordered_set<std::string> names = {
        "intSuffix", "escapeString", "opSpelling", "ident", "mangleName", "escape", "wrap", "table",
        "subst", "require", "inlineBlock", // shared expression catalog
        "generics", "where", "mangle",     // declaration-context catalog
        "substExtern",                     // type-context catalog (extern-class template substitution)
    };
    return names;
}

// Recursively collect every fn name, call target, and mapMembers rule reference in a Rule tree.
void collectRefs(const engine::Rule& r, std::vector<std::string>& fns, std::vector<std::string>& calls) {
    using K = engine::Rule::Kind;
    if (r.kind == K::Fn) fns.push_back(r.s);
    if (r.kind == K::Call) calls.push_back(r.s);
    if (r.kind == K::MapMembers) calls.push_back(r.s2);
    for (const auto& p : r.parts) collectRefs(p, fns, calls);
    for (const auto& arm : r.arms) collectRefs(arm.second, fns, calls);
    for (const auto& e : r.elseBody) collectRefs(e, fns, calls);
}

// The anti-silent-drop coverage contract: every IR construct the compiler can produce must have a rule,
// or the plugin must DECLARE its stance via the named capability — "false" (refused at compile time) or
// "emulated" (handled by other rules — Python's duck-typed interfaces). A missing rule with no declared
// stance — or a "native" claim with no rule — is a load error, because at emit time it would silently
// drop output (the P9-V lesson made structural).
struct Coverage {
    const char* rule;
    const char* capability; // nullptr = core, no capability can excuse a missing rule
};
constexpr Coverage kCoverage[] = {
    {"Int", nullptr},      {"Float", nullptr},    {"Bool", nullptr},   {"Null", nullptr},
    {"Str", nullptr},      {"Char", nullptr},     {"Var", nullptr},    {"This", nullptr},
    {"Extern", nullptr},   {"Call", nullptr},     {"Member", nullptr}, {"Index", nullptr},
    {"Cond", nullptr},     {"ListLit", nullptr},  {"Tuple", nullptr},  {"New", nullptr},
    {"Unary", nullptr},    {"Cast", nullptr},     {"Interp", nullptr},
    {"MethodCall", nullptr}, {"Binary", nullptr},
    {"MakeCase", "patternMatching"}, // a union-constructor node — only reachable when unions/match are
    {"Match", "patternMatching"}, {"With", "withExpressions"}, {"Await", "async"}, {"Lambda", "closures"},
    {"ForStmt", nullptr}, {"TryStmt", "exceptions"},
    {"Program", nullptr}, {"Type", nullptr}, {"EnumDecl", nullptr}, {"RecordDecl", nullptr},
    {"ClassDecl", nullptr}, {"MethodDecl", nullptr}, {"FunctionDecl", nullptr},
    {"UnionDecl", "patternMatching"}, {"InterfaceDecl", "interfaces"},
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

    std::unordered_map<std::string, std::string> caps;
    for (const auto& kv : doc["capabilities"].members) {
        if (kv.second.kind == json::Value::Kind::Bool) caps[kv.first] = kv.second.boolean ? "native" : "false";
        else if (kv.second.kind == json::Value::Kind::String) caps[kv.first] = kv.second.asString();
    }

    // Anti-silent-drop coverage: every construct has a rule, or a declared "false"/"emulated" stance.
    for (const Coverage& c : kCoverage) {
        if (rules.find(c.rule) != rules.end()) continue;
        const char* cap = c.capability;
        auto it = cap ? caps.find(cap) : caps.end();
        if (!cap || it == caps.end()) {
            error = "plugin '" + name + "': no rule for '" + c.rule + "'" +
                    (cap ? std::string(" and no '") + cap + "' capability entry" : std::string()) +
                    " — a construct with no rule must declare its stance (anti-silent-drop)";
            return false;
        }
        if (it->second != "false" && it->second != "emulated") {
            error = "plugin '" + name + "': capability '" + std::string(cap) + "' claims '" + it->second +
                    "' but no '" + c.rule + "' rule exists";
            return false;
        }
    }

    // Reference validation: every {"fn":…} names a catalog builtin; every {"call":…}/mapMembers target
    // exists in the table. An unknown fn evaluated to "" SILENTLY before this check (the 6d gate catch).
    {
        std::vector<std::string> fns, calls;
        for (const auto& kv : rules) collectRefs(kv.second, fns, calls);
        for (const auto& f : fns)
            if (fnCatalog().count(f) == 0) {
                error = "plugin '" + name + "': unknown builtin '" + f + "' (not in the fixed catalog)";
                return false;
            }
        for (const auto& c : calls)
            if (rules.find(c) == rules.end()) {
                error = "plugin '" + name + "': rule reference '" + c + "' does not exist";
                return false;
            }
    }

    // The std overlay arms, organized by module in the manifest for readability, flattened here (member
    // keys — "List.add", "print" — are unique across the std surface).
    std::unordered_map<std::string, std::string> overlays;
    for (const auto& mod : doc["std"].members)
        for (const auto& kv : mod.second.members)
            if (kv.second.kind == json::Value::Kind::String) overlays[kv.first] = kv.second.asString();

    std::string ext = doc["fileExtension"].asString();
    if (ext.empty()) ext = "." + name; // a reasonable default; first-party plugins declare theirs

    registry().push_back(std::make_unique<LoadedBackend>(name, std::move(ext), std::move(spec.spec),
                                                         std::move(rules), std::move(caps),
                                                         std::move(overlays)));
    return true;
}

} // namespace mintplayer::polyglot
