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
        case Feature::MutableRefClasses:   return "mutableRefClasses";
        case Feature::FixedWidthIntegers:  return "fixedWidthIntegers";
        case Feature::Utf16Strings:        return "utf16Strings";
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
                  std::unordered_map<std::string, std::string> overlays, bool crossDirImports = false)
        : name_(std::move(name)), ext_(std::move(fileExtension)), spec_(std::move(spec)),
          rules_(std::move(rules)), capabilities_(std::move(capabilities)), overlays_(std::move(overlays)),
          crossDirImports_(crossDirImports) {}

    std::string name() const override { return name_; }

    std::string emit(const ir::Module& m) const override {
        InterpretedEmitter emitter([this]() -> const BackendSpec& { return spec_; }, rules_);
        return emitter.emit(m);
    }

    // Tri-state capabilities (PRD §4.11): a feature absent from the map is `"native"` (supported); `"false"`
    // gates it at compile time (§3.E); `"emulated"` is supported but warns (call-site rewrite). `supports()`
    // is the coarse gate; `capabilityStance()` is the source of truth both read from.
    std::string capabilityStance(Feature f) const override {
        auto it = capabilities_.find(featureName(f));
        return it == capabilities_.end() ? "native" : it->second;
    }
    bool supports(Feature f) const override { return capabilityStance(f) != "false"; }

    const std::unordered_map<std::string, std::string>& stdOverlays() const override { return overlays_; }
    std::string fileExtension() const override { return ext_; }
    bool crossDirImports() const override { return crossDirImports_; }
    const std::vector<std::string>& reservedIdentifiers() const override { return spec_.reservedNames; }
    const std::vector<std::string>& globalIdentifiers() const override { return spec_.globalNames; }

private:
    bool crossDirImports_ = false;
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
        "generics", "where", "mangle", "access", // declaration-context catalog (`access` = the requested accessibility modifier)
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

namespace {

// Parse + validate an artifact into a ready-to-register backend (nullptr + error on any problem). The
// duplicate-name check is the caller's (registration) concern — `polyglot install` validates a manifest
// for a target that may well already be loaded in this process.
std::unique_ptr<LoadedBackend> buildBackend(const std::string& artifactJson, std::string& error) {
    json::Value doc = json::parse(artifactJson);
    if (doc.kind != json::Value::Kind::Object) { error = "plugin artifact must be a JSON object"; return nullptr; }
    const std::string name = doc["name"].asString();
    if (name.empty()) { error = "plugin artifact: missing 'name'"; return nullptr; }

    SpecLoadResult spec = loadBackendSpec(doc["spec"]);
    if (!spec.ok) {
        error = "plugin '" + name + "': " + (spec.errors.empty() ? "invalid spec" : spec.errors.front());
        return nullptr;
    }
    if (spec.spec.name != name) {
        error = "plugin '" + name + "': spec name '" + spec.spec.name + "' does not match";
        return nullptr;
    }

    const json::Value& rulesDoc = doc["rules"];
    if (rulesDoc.kind != json::Value::Kind::Object) { error = "plugin '" + name + "': missing 'rules'"; return nullptr; }
    engine::RuleTable rules;
    for (const auto& kv : rulesDoc.members) {
        bool ok = true;
        std::string err;
        engine::Rule r = engine::parseRule(kv.second, ok, err);
        if (!ok) { error = "plugin '" + name + "': rule '" + kv.first + "': " + err; return nullptr; }
        rules.emplace(kv.first, std::move(r));
    }
    for (const char* required : {"Program", "Type"})
        if (rules.find(required) == rules.end()) {
            error = "plugin '" + name + "': missing required rule '" + std::string(required) + "'";
            return nullptr;
        }

    std::unordered_map<std::string, std::string> caps;
    for (const auto& kv : doc["capabilities"].members) {
        // Tri-state, strictly validated (P26 slice 0): a JSON bool normalizes (true→"native", false→"false");
        // a string must be exactly one of the three stances. A typo like "emulted" used to pass silently and
        // then read as "supported" (a silent miscompile risk) — reject it at load instead.
        std::string stance;
        if (kv.second.kind == json::Value::Kind::Bool) stance = kv.second.boolean ? "native" : "false";
        else if (kv.second.kind == json::Value::Kind::String) stance = kv.second.asString();
        else { error = "plugin '" + name + "': capability '" + kv.first + "' must be a bool or a string"; return nullptr; }
        if (stance != "native" && stance != "emulated" && stance != "false") {
            error = "plugin '" + name + "': capability '" + kv.first +
                    "' must be \"native\", \"emulated\" or false (got '" + stance + "')";
            return nullptr;
        }
        caps[kv.first] = stance;
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
            return nullptr;
        }
        if (it->second != "false" && it->second != "emulated") {
            error = "plugin '" + name + "': capability '" + std::string(cap) + "' claims '" + it->second +
                    "' but no '" + c.rule + "' rule exists";
            return nullptr;
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
                return nullptr;
            }
        for (const auto& c : calls)
            if (rules.find(c) == rules.end()) {
                error = "plugin '" + name + "': rule reference '" + c + "' does not exist";
                return nullptr;
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

    // P30 slice 8: whether this target's emitted import specifiers may span directories (the
    // compiler then hands the import rule a full relative specifier instead of a bare basename).
    const bool crossDir = doc["crossDirImports"].asBool(false);

    return std::make_unique<LoadedBackend>(name, std::move(ext), std::move(spec.spec), std::move(rules),
                                           std::move(caps), std::move(overlays), crossDir);
}

} // namespace

bool loadBackend(const std::string& artifactJson, std::string& error, bool replaceExisting) {
    const std::string name = json::parse(artifactJson)["name"].asString();
    if (!name.empty() && findBackend(name) && !replaceExisting) { // checked first: the clearer error for a re-load
        error = "plugin '" + name + "' is already loaded";
        return false;
    }
    std::unique_ptr<LoadedBackend> b = buildBackend(artifactJson, error);
    if (!b) return false;
    if (replaceExisting && !name.empty()) {
        // Shadow, don't destroy: outstanding BackendHandles point into the displaced entry, so it
        // moves to a process-lifetime graveyard and only the lookups forget it.
        static std::vector<std::unique_ptr<LoadedBackend>> graveyard;
        auto& reg = registry();
        for (auto it = reg.begin(); it != reg.end(); ++it) {
            if ((*it)->name() == name) {
                graveyard.push_back(std::move(*it));
                reg.erase(it);
                break;
            }
        }
    }
    registry().push_back(std::move(b));
    return true;
}

bool validateBackend(const std::string& artifactJson, std::string& error) {
    return buildBackend(artifactJson, error) != nullptr;
}

} // namespace mintplayer::polyglot
