#include "mintplayer/polyglot/backend.hpp"

#include <map>
#include <memory>
#include <regex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mintplayer/polyglot/arm_trace.hpp"
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
        case Feature::PropertySetters:     return "propertySetters";
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
                  std::unordered_map<std::string, StdBinding> overlays, bool crossDirImports = false,
                  OriginMapping originMapping = {}, bool sharedPreludeFile = false,
                  bool preludePerFile = false, std::vector<TargetOptionDecl> options = {})
        // Initialiser order follows DECLARATION order below, or g++ warns (-Wreorder).
        : crossDirImports_(crossDirImports), originMapping_(std::move(originMapping)),
          sharedPreludeFile_(sharedPreludeFile), preludePerFile_(preludePerFile),
          name_(std::move(name)), ext_(std::move(fileExtension)), spec_(std::move(spec)),
          rules_(std::move(rules)), capabilities_(std::move(capabilities)),
          overlays_(std::move(overlays)), options_(std::move(options)) {}

    std::string name() const override { return name_; }

    std::string emit(const ir::Module& m) const override {
        InterpretedEmitter emitter([this]() -> const BackendSpec& { return spec_; }, rules_, &originMapping_);
        return emitter.emit(m);
    }
    std::string emitWithOrigins(const ir::Module& m, std::vector<std::array<int, 3>>& origins) const override {
        InterpretedEmitter emitter([this]() -> const BackendSpec& { return spec_; }, rules_, &originMapping_);
        std::string code = emitter.emit(m);
        origins.clear();
        for (const auto& r : emitter.originRecords())
            origins.push_back({r.first, r.second.fileId, r.second.line});
        return code;
    }

    // Tri-state capabilities (PRD §4.11 / P37 slice 0): keyed lookup with the umbrella rule — exact key,
    // else the bare parent of a `parent:child` key, else `"native"` (supported). `"false"` gates at compile
    // time (§3.E); `"emulated"` is supported but warns (call-site rewrite). `supports()` is the coarse gate;
    // `capabilityStance()` is the source of truth both read from.
    std::string capabilityStance(const std::string& key) const override {
        auto it = capabilities_.find(key);
        if (it != capabilities_.end()) return it->second;
        const auto colon = key.find(':');
        if (colon != std::string::npos) {
            it = capabilities_.find(key.substr(0, colon));
            if (it != capabilities_.end()) return it->second;
        }
        return "native";
    }
    using Backend::capabilityStance;

    ConstSpelling constSpelling() const override {
        return {spec_.trueLit, spec_.falseLit, spec_.enumMemberOp, spec_.constArrayOpen, spec_.constArrayClose};
    }
    bool linksWithoutImports() const override { return spec_.linksWithoutImports; }
    bool forbidsShadowedLocals() const override { return spec_.forbidsShadowedLocals; }
    bool expressionOnlyLambdas() const override { return spec_.expressionOnlyLambdas; }
    const std::unordered_map<std::string, StdBinding>& stdOverlays() const override { return overlays_; }
    std::string fileExtension() const override { return ext_; }
    bool crossDirImports() const override { return crossDirImports_; }
    bool sharedPreludeFile() const override { return sharedPreludeFile_; }
    bool preludePerFile() const override { return preludePerFile_; }
    const OriginMapping& originMapping() const override { return originMapping_; }
    const std::vector<std::string>& reservedIdentifiers() const override { return spec_.reservedNames; }
    const std::vector<std::string>& globalIdentifiers() const override { return spec_.globalNames; }
    const std::vector<TargetOptionDecl>& options() const override { return options_; }

private:
    bool crossDirImports_ = false;
    OriginMapping originMapping_;
    bool sharedPreludeFile_ = false;
    bool preludePerFile_ = false;
    std::string name_;
    std::string ext_;
    BackendSpec spec_;
    engine::RuleTable rules_;
    std::unordered_map<std::string, std::string> capabilities_;
    std::unordered_map<std::string, StdBinding> overlays_; // flattened std FFI templates (+ their requires)
    std::vector<TargetOptionDecl> options_;                // P40 B1: the manifest `options` block
};

// ---- Load-time validation (P19 slice 8) ------------------------------------------------------------------

// The closed capability vocabulary (P37 slice 0, PRD §3.E / H5): the 16 Feature names, the coverage-only
// entries, and the enumerated `parent:child` refinements later slices consume. A manifest key outside this
// set is a LOAD ERROR (never silently ignored) — the anti-silent-drop rule applied to capability spelling:
// a typo'd key used to read as an unrelated, defaulted-to-native capability.
const std::unordered_set<std::string>& capabilityVocabulary() {
    static const std::unordered_set<std::string> keys = [] {
        std::unordered_set<std::string> s;
        for (const Feature f : kAllFeatures) s.insert(featureName(f));
        s.insert("interfaces"); // coverage-only today (InterfaceDecl stance); checker-enforced since P31
        // P37 B4: whether interfaces exist as testable runtime types (`is`/`as`/typed-match against an
        // interface). False on TS (interfaces erase); native elsewhere (C# `is`, Python ABC isinstance,
        // PHP instanceof).
        s.insert("interfaces:runtimeIdentity");
        // P37 C6: graded operator support — PHP supports :eq/:indexers while refusing the rest; the bare
        // `operatorOverloading` stance umbrella-covers undeclared sub-keys.
        s.insert("operatorOverloading:arithmetic");
        s.insert("operatorOverloading:comparison");
        s.insert("operatorOverloading:eq");
        s.insert("operatorOverloading:indexers");
        s.insert("operatorOverloading:conversion");
        // P37 D10: Tier 1 pass-through attribute ATTACHMENT POINTS (Tier 2 needs no capability — the
        // compiler both writes and reads its data). `attributes` is the umbrella parent.
        s.insert("attributes");
        s.insert("attributes:target.type");
        s.insert("attributes:target.method");
        s.insert("attributes:target.function");
        s.insert("attributes:target.field");  // false on Python (no field annotations)
        s.insert("attributes:target.param");  // false on TS (TC39 has none) + Python
        return s;
    }();
    return keys;
}

} // namespace

bool isKnownCapabilityKey(const std::string& key) { return capabilityVocabulary().count(key) != 0; }

namespace {

// The fixed builtin catalog — every `{"fn":…}` a rule may name. A name outside this set evaluated to ""
// SILENTLY before this check existed (the slice-6d gate caught exactly that); now it fails the load.
const std::unordered_set<std::string>& fnCatalog() {
    static const std::unordered_set<std::string> names = {
        "intSuffix", "literal", "quoted", "opSpelling", "ident", "identFn", "mangleName", "wrap", "table",
        "subst", "require", "inlineBlock", // shared expression catalog
        "generics", "where", "mangle",     // declaration-context catalog
        "substExtern",                     // type-context catalog (extern-class template substitution)
        "preludeSlot",                     // P40 §0: the Program rule's prelude placement marker (decl position)
    };
    return names;
}

// P40 §0: walk a rule tree, calling `f` on every node (the shapes collectRefs walks).
template <typename F>
void forEachRule(const engine::Rule& r, const F& f) {
    f(r);
    for (const auto& p : r.parts) forEachRule(p, f);
    for (const auto& arm : r.arms) forEachRule(arm.second, f);
    for (const auto& e : r.elseBody) forEachRule(e, f);
}

// P40 §0: does `r` place the prelude slot UNCONDITIONALLY at declaration position — reached through
// `seq`/`indent` only, never inside a `case` arm, a `block` or an expression template? An unconditional slot
// is what lets the emitter rely on the marker always having fired.
bool unconditionalPreludeSlot(const engine::Rule& r) {
    using K = engine::Rule::Kind;
    if (r.kind == K::Fn) return r.s == "preludeSlot";
    if (r.kind == K::Seq || r.kind == K::Indent)
        for (const auto& p : r.parts)
            if (unconditionalPreludeSlot(p)) return true;
    return false;
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

// P40 B1: every `option.<name>` a rule tree reads — through `{"get":…}` or an `eq`/`has` test — so the loader
// can refuse a reference to an option the manifest never declares (it would read "" forever, silently).
void collectOptionRefs(const engine::Test& t, std::vector<std::string>& out) {
    if ((t.kind == engine::Test::Kind::Eq || t.kind == engine::Test::Kind::Has) &&
        t.path.rfind(engine::kOptionPathPrefix, 0) == 0)
        out.push_back(t.path.substr(std::char_traits<char>::length(engine::kOptionPathPrefix)));
    for (const auto& s : t.subs) collectOptionRefs(s, out);
}
void collectOptionRefs(const engine::Rule& r, std::vector<std::string>& out) {
    if (r.kind == engine::Rule::Kind::Get && r.s.rfind(engine::kOptionPathPrefix, 0) == 0)
        out.push_back(r.s.substr(std::char_traits<char>::length(engine::kOptionPathPrefix)));
    for (const auto& p : r.parts) collectOptionRefs(p, out);
    for (const auto& arm : r.arms) {
        collectOptionRefs(arm.first, out);
        collectOptionRefs(arm.second, out);
    }
    for (const auto& e : r.elseBody) collectOptionRefs(e, out);
}

// P40 B1: parse + validate the manifest `options` block. Each entry is an object with only the known keys;
// `default` is a string, `pattern` a compilable ECMAScript regex the default itself must match, `scope` is
// absent or "closure", `reserve` a bool. A malformed block fails the load (never a silently ignored key).
bool parseOptionDecls(const std::string& plugin, const json::Value& block, std::vector<TargetOptionDecl>& out,
                      std::string& error) {
    if (block.kind == json::Value::Kind::Null) return true;
    if (block.kind != json::Value::Kind::Object) {
        error = "plugin '" + plugin + "': 'options' must be an object of option declarations";
        return false;
    }
    static const std::regex kName("[A-Za-z][A-Za-z0-9_]*");
    for (const auto& kv : block.members) {
        const std::string where = "plugin '" + plugin + "': option '" + kv.first + "'";
        if (!std::regex_match(kv.first, kName)) { error = where + ": the name must be an identifier"; return false; }
        const json::Value& o = kv.second;
        if (o.kind != json::Value::Kind::Object) { error = where + " must be an object"; return false; }
        TargetOptionDecl d;
        d.name = kv.first;
        for (const auto& m : o.members) {
            if (m.first == "default") {
                if (m.second.kind != json::Value::Kind::String) { error = where + ": 'default' must be a string"; return false; }
                d.defaultValue = m.second.asString();
            } else if (m.first == "pattern") {
                if (m.second.kind != json::Value::Kind::String) { error = where + ": 'pattern' must be a string"; return false; }
                d.pattern = m.second.asString();
            } else if (m.first == "scope") {
                if (m.second.kind != json::Value::Kind::String || m.second.asString() != "closure") {
                    error = where + ": 'scope' must be \"closure\" (or absent)";
                    return false;
                }
                d.closureScope = true;
            } else if (m.first == "reserve") {
                if (m.second.kind != json::Value::Kind::Bool) { error = where + ": 'reserve' must be a bool"; return false; }
                d.reserve = m.second.boolean;
            } else {
                error = where + ": unknown key '" + m.first + "' (expected default / pattern / scope / reserve)";
                return false;
            }
        }
        if (!d.pattern.empty()) {
            try {
                if (!std::regex_match(d.defaultValue, std::regex(d.pattern, std::regex::ECMAScript))) {
                    error = where + ": the default '" + d.defaultValue + "' does not match its own pattern '" +
                            d.pattern + "'";
                    return false;
                }
            } catch (const std::regex_error&) {
                error = where + ": 'pattern' is not a valid regular expression: " + d.pattern;
                return false;
            }
        }
        out.push_back(std::move(d));
    }
    return true;
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
    {"IsTest", nullptr},   {"AsCast", nullptr},   // P37 B: `is` / `as` (instanceof/isinstance everywhere)
    {"MethodCall", nullptr}, {"Binary", nullptr},
    {"MakeCase", "patternMatching"}, // a union-constructor node — only reachable when unions/match are
    {"Match", "patternMatching"}, {"With", "withExpressions"}, {"Await", "async"}, {"Lambda", "closures"},
    {"ForStmt", nullptr}, {"TryStmt", "exceptions"}, {"IndexAssign", nullptr}, {"TupleLet", nullptr},
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
    std::string jsonErr;
    json::Value doc = json::parse(artifactJson, jsonErr);
    if (doc.kind != json::Value::Kind::Object) { error = "plugin artifact must be a JSON object"; return nullptr; }
    if (!jsonErr.empty()) { error = "plugin artifact: " + jsonErr; return nullptr; }
    const std::string name = doc["name"].asString();
    if (name.empty()) { error = "plugin artifact: missing 'name'"; return nullptr; }

    // P40: the manifest schema version is validated before anything else is read. A schema-1 manifest
    // (no `preludeSlot`, no binding `requires`) or one from a newer engine would otherwise load with
    // features silently misread — refuse it, naming the version this compiler expects.
    {
        const json::Value& schema = doc["schema"];
        const bool isNumber = schema.kind == json::Value::Kind::Number;
        const double v = isNumber ? schema.asNumber(0) : 0;
        if (!isNumber || v != static_cast<double>(kPluginSchemaVersion)) {
            std::string got = "missing";
            if (isNumber) {
                got = (v == static_cast<double>(static_cast<long long>(v))) ? std::to_string(static_cast<long long>(v))
                                                                             : std::to_string(v);
            } else if (schema.kind != json::Value::Kind::Null) {
                got = "not a number";
            }
            error = "plugin '" + name + "': unsupported manifest schema (" + got + "); this compiler reads schema " +
                    std::to_string(kPluginSchemaVersion) + " — use a plugin built for this engine version";
            return nullptr;
        }
    }

    SpecLoadResult spec = loadBackendSpec(doc["spec"]);
    if (!spec.ok) {
        error = "plugin '" + name + "': " + (spec.errors.empty() ? "invalid spec" : spec.errors.front());
        return nullptr;
    }
    if (spec.spec.name != name) {
        error = "plugin '" + name + "': spec name '" + spec.spec.name + "' does not match";
        return nullptr;
    }

    // P38/issue #69: origin mapping, as data. Absent = Style::None = emit exactly as before (Python/PHP
    // declare nothing). An unknown `style` is a LOAD ERROR, following `blockStyle`: it is the one place a
    // manifest's unknown VALUE refuses instead of defaulting, which is what keeps version skew loud.
    // Validated here, before the rule tables, because it is independent of them — a manifest that misspells
    // its origin style should hear about THAT, not about whichever construct rule it also happens to lack.
    OriginMapping origin;
    const json::Value& om = doc["originMapping"];
    if (om.kind == json::Value::Kind::Object) {
        const std::string style = om["style"].asString();
        if (style == "directive")         origin.style = OriginMapping::Style::Directive;
        else if (style == "sourceMapV3")  origin.style = OriginMapping::Style::SourceMapV3;
        else {
            error = "plugin '" + name + "': unknown originMapping style '" + style + "'";
            return nullptr;
        }
        origin.line             = om["line"].asString();
        origin.hidden           = om["hidden"].asString();
        origin.column           = static_cast<int>(om["column"].asNumber(0));
        origin.sidecarExtension = om["sidecarExtension"].asString();
        origin.footer           = om["footer"].asString();
        // A declared sink that cannot actually emit is a manifest bug, not a silent no-op.
        if (origin.style == OriginMapping::Style::Directive && (origin.line.empty() || origin.hidden.empty())) {
            error = "plugin '" + name + "': originMapping style 'directive' needs both 'line' and 'hidden'";
            return nullptr;
        }
        if (origin.style == OriginMapping::Style::SourceMapV3 && origin.sidecarExtension.empty()) {
            error = "plugin '" + name + "': originMapping style 'sourceMapV3' needs 'sidecarExtension'";
            return nullptr;
        }
    } else if (om.kind != json::Value::Kind::Null) {
        error = "plugin '" + name + "': 'originMapping' must be an object";
        return nullptr;
    }

    const json::Value& rulesDoc = doc["rules"];
    if (rulesDoc.kind != json::Value::Kind::Object) { error = "plugin '" + name + "': missing 'rules'"; return nullptr; }
    engine::RuleTable rules;
    // Arm coverage: register the manifest text so hits can be attributed back to source lines, and
    // stamp every parsed rule with this manifest's opaque srcId. Both are no-ops (srcId 0) unless
    // --emit-arm-trace enabled tracing before load. The offsets collected here are the coverage
    // DENOMINATOR, so they come from this same parse rather than a separate scan of the file.
    const int srcId = armtrace::registerManifest(name, artifactJson);
    std::vector<std::size_t> armOffsets;
    for (const auto& kv : rulesDoc.members) {
        bool ok = true;
        std::string err;
        engine::Rule r = engine::parseRule(kv.second, ok, err);
        if (!ok) { error = "plugin '" + name + "': rule '" + kv.first + "': " + err; return nullptr; }
        if (srcId > 0) engine::indexRule(r, srcId, armOffsets);
        rules.emplace(kv.first, std::move(r));
    }
    if (srcId > 0) armtrace::addStaticOffsets(srcId, armOffsets);
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
        // P37 slice 0: the vocabulary is closed and load-validated — an unknown key (typo, or a key from a
        // newer vocabulary than this compiler) fails the load instead of silently defaulting to "native".
        if (!isKnownCapabilityKey(kv.first)) {
            error = "plugin '" + name + "': unknown capability key '" + kv.first +
                    "' (the capability vocabulary is closed; see PRD §3.E)";
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

    // P40 §0: prelude placement is data. The `Program` rule must carry exactly one `{"fn":"preludeSlot"}`
    // marker at an unconditional declaration position, and no other rule may carry one — each language
    // puts it where its helpers are legal (PHP after the namespace/use lines, C# after the usings).
    // A literal `{"fn":"require"}` key must name a declared prelude, like a binding's `requires`.
    {
        int slots = 0;
        std::string badRequire;
        for (const auto& kv : rules)
            forEachRule(kv.second, [&](const engine::Rule& r) {
                if (r.kind != engine::Rule::Kind::Fn) return;
                if (r.s == "preludeSlot") ++slots;
                if (r.s == "require" && !r.parts.empty() && r.parts[0].kind == engine::Rule::Kind::Lit &&
                    !spec.spec.preludes.count(r.parts[0].s) && badRequire.empty())
                    badRequire = r.parts[0].s;
            });
        // P40 A1: a literal context is required data, and every {"fn":"literal"/"quoted"} names a declared one
        // (a typo would silently fall back to the string context's quoting).
        if (!spec.spec.literals.count("string")) {
            error = "plugin '" + name + "': spec.literals must declare a 'string' context (P40)";
            return nullptr;
        }
        std::string badContext;
        for (const auto& kv : rules)
            forEachRule(kv.second, [&](const engine::Rule& r) {
                if (r.kind != engine::Rule::Kind::Fn || (r.s != "literal" && r.s != "quoted")) return;
                const bool ok = r.parts.size() == 2 && r.parts[0].kind == engine::Rule::Kind::Lit &&
                                spec.spec.literals.count(r.parts[0].s);
                if (!ok && badContext.empty())
                    badContext = r.parts.empty() || r.parts[0].kind != engine::Rule::Kind::Lit ? std::string("?")
                                                                                              : r.parts[0].s;
            });
        if (!badContext.empty()) {
            error = "plugin '" + name + "': a {\"fn\":\"literal\"/\"quoted\"} names literal context '" + badContext +
                    "', which spec.literals does not declare (args: [\"<context>\", <text>])";
            return nullptr;
        }
        if (!badRequire.empty()) {
            error = "plugin '" + name + "': {\"fn\":\"require\"} names '" + badRequire +
                    "', which is not a declared prelude (spec.preludes)";
            return nullptr;
        }
        if (slots != 1 || !unconditionalPreludeSlot(rules.at("Program"))) {
            error = "plugin '" + name + "': the 'Program' rule must contain exactly one {\"fn\":\"preludeSlot\"} "
                    "marker at an unconditional declaration position (inside 'seq'/'indent' only), and no "
                    "other rule may contain one (found " + std::to_string(slots) + " in total)";
            return nullptr;
        }
    }

    // P40 B1: the target options this plugin accepts, and every `option.<name>` its rules read must be one of
    // them (an undeclared reference would read "" forever — the anti-silent-drop contract, applied to options).
    std::vector<TargetOptionDecl> optionDecls;
    if (!parseOptionDecls(name, doc["options"], optionDecls, error)) return nullptr;
    {
        std::vector<std::string> refs;
        for (const auto& kv : rules) collectOptionRefs(kv.second, refs);
        for (const auto& ref : refs) {
            bool declared = false;
            for (const auto& d : optionDecls) declared = declared || d.name == ref;
            if (!declared) {
                error = "plugin '" + name + "': a rule reads 'option." + ref +
                        "', which the manifest's 'options' block does not declare";
                return nullptr;
            }
        }
    }

    // The std overlay arms, organized by module in the manifest for readability, flattened here (member
    // keys — "List.add", "print" — are unique across the std surface).
    // P40 §0: a binding is a plain template string, or `{ "template": "...", "requires": ["key", ...] }`.
    // Every required key must be a declared prelude — a typo would otherwise emit a call to a helper that
    // is never defined (the anti-silent-drop contract, applied to helper references).
    const auto& preludes = spec.spec.preludes;
    std::unordered_map<std::string, StdBinding> overlays;
    for (const auto& mod : doc["std"].members)
        for (const auto& kv : mod.second.members) {
            const json::Value& v = kv.second;
            if (v.kind == json::Value::Kind::String) {
                overlays[kv.first] = StdBinding{v.asString(), {}};
                continue;
            }
            if (v.kind != json::Value::Kind::Object || v["template"].kind != json::Value::Kind::String) {
                error = "plugin '" + name + "': std binding '" + kv.first +
                        "' must be a template string or an object with a string 'template'";
                return nullptr;
            }
            StdBinding b;
            b.tmpl = v["template"].asString();
            const json::Value& req = v["requires"];
            if (req.kind != json::Value::Kind::Null && req.kind != json::Value::Kind::Array) {
                error = "plugin '" + name + "': std binding '" + kv.first + "': 'requires' must be an array";
                return nullptr;
            }
            for (const json::Value& r : req.items()) {
                if (r.kind != json::Value::Kind::String || !preludes.count(r.asString())) {
                    error = "plugin '" + name + "': std binding '" + kv.first + "' requires '" +
                            (r.kind == json::Value::Kind::String ? r.asString() : std::string("<non-string>")) +
                            "', which is not a declared prelude (spec.preludes)";
                    return nullptr;
                }
                b.preludeKeys.push_back(r.asString());
            }
            for (const auto& m : v.members)
                if (m.first != "template" && m.first != "requires") {
                    error = "plugin '" + name + "': std binding '" + kv.first + "': unknown key '" + m.first + "'";
                    return nullptr;
                }
            // A type spelling is rendered through the Type rule, not emitted as an expression, so nothing
            // would record its requirements — refuse rather than silently drop them.
            const std::string typeSuffix = ".type";
            if (!b.preludeKeys.empty() && kv.first.size() > typeSuffix.size() &&
                kv.first.compare(kv.first.size() - typeSuffix.size(), typeSuffix.size(), typeSuffix) == 0) {
                error = "plugin '" + name + "': std binding '" + kv.first +
                        "': 'requires' is not supported on a type spelling";
                return nullptr;
            }
            overlays[kv.first] = std::move(b);
        }

    std::string ext = doc["fileExtension"].asString();
    if (ext.empty()) ext = "." + name; // a reasonable default; first-party plugins declare theirs

    // P30 slice 8: whether this target's emitted import specifiers may span directories (the
    // compiler then hands the import rule a full relative specifier instead of a bare basename).
    const bool crossDir = doc["crossDirImports"].asBool(false);

    // P39/issue #71: prelude placement, as data. Replaces two `target.name() == "csharp"` comparisons in
    // the compiler — see Backend::sharedPreludeFile / preludePerFile for why this is load-bearing rather
    // than cosmetic.
    const bool sharedPreludeFile = doc["sharedPreludeFile"].asBool(false);
    const bool preludePerFile    = doc["preludePerFile"].asBool(false);

    return std::make_unique<LoadedBackend>(name, std::move(ext), std::move(spec.spec), std::move(rules),
                                           std::move(caps), std::move(overlays), crossDir, std::move(origin),
                                           sharedPreludeFile, preludePerFile, std::move(optionDecls));
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

bool resolveTargetOptions(const Backend& backend, const std::map<std::string, std::string>& given,
                          std::map<std::string, std::string>& resolved, std::string& error) {
    resolved.clear();
    const auto& decls = backend.options();
    for (const auto& [key, value] : given) {
        const TargetOptionDecl* d = nullptr;
        for (const auto& c : decls) if (c.name == key) d = &c;
        if (!d) {
            std::string known;
            for (const auto& c : decls) known += (known.empty() ? "" : ", ") + c.name;
            error = "unknown target option '" + backend.name() + "." + key + "' (target '" + backend.name() +
                    "' declares " + (known.empty() ? std::string("no options") : "only: " + known) + ")";
            return false;
        }
        // The manifest pattern compiled at load (parseOptionDecls), so it cannot throw here.
        if (!d->pattern.empty() && !std::regex_match(value, std::regex(d->pattern, std::regex::ECMAScript))) {
            error = "target option '" + backend.name() + "." + key + "' has the invalid value '" + value +
                    "' (it must match " + d->pattern + ")";
            return false;
        }
    }
    for (const auto& d : decls) {
        auto it = given.find(d.name);
        resolved[d.name] = it != given.end() ? it->second : d.defaultValue;
    }
    return true;
}

} // namespace mintplayer::polyglot
