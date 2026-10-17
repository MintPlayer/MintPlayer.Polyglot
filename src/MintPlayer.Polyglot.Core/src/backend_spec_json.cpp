#include "mintplayer/polyglot/backend_spec_json.hpp"

#include <algorithm>

#include "mintplayer/polyglot/json.hpp"

namespace mintplayer::polyglot {

namespace {
// constexpr char arrays: zero dynamic init, so loadBackendSpec is safe to call from any TU's static
// initialization (std::string globals here once made a static-init-order abort in the CLI).
constexpr const char* kBlockAllman = "bracesAllman";
constexpr const char* kBlockKnR    = "bracesKnR";
constexpr const char* kBlockColon  = "colonIndent";

void loadStringMap(const json::Value& obj, std::unordered_map<std::string, std::string>& out) {
    for (const auto& kv : obj.members)
        if (kv.second.kind == json::Value::Kind::String) out[kv.first] = kv.second.asString();
}
} // namespace

SpecLoadResult loadBackendSpec(const std::string& json) {
    std::string err;
    json::Value doc = json::parse(json, err);
    if (!err.empty()) {
        SpecLoadResult r;
        r.errors.push_back("backend spec: " + err);
        return r;
    }
    return loadBackendSpec(doc);
}

SpecLoadResult loadBackendSpec(const json::Value& doc) {
    SpecLoadResult r;
    if (doc.kind != json::Value::Kind::Object) {
        r.errors.push_back("backend spec must be a JSON object");
        return r;
    }
    const json::Value& name = doc["name"];
    if (name.kind != json::Value::Kind::String || name.asString().empty()) {
        r.errors.push_back("backend spec: missing or empty 'name'");
        return r;
    }
    r.spec.name = name.asString();

    loadStringMap(doc["scalarType"], r.spec.scalarType);
    loadStringMap(doc["intSuffix"], r.spec.intSuffix);
    loadStringMap(doc["binaryOp"], r.spec.binaryOp);
    loadStringMap(doc["wrapInt"], r.spec.wrapInt);
    loadStringMap(doc["preludes"], r.spec.preludes);

    for (const auto& kv : doc["delimited"].members) {
        const json::Value& d = kv.second;
        r.spec.delimited[kv.first] = {d["open"].asString(), d["sep"].asString(), d["close"].asString()};
    }

    const std::string bs = doc["blockStyle"].asString();
    if (bs.empty() || bs == kBlockKnR) r.spec.blockStyle = BlockStyle::BracesKnR;
    else if (bs == kBlockAllman)       r.spec.blockStyle = BlockStyle::BracesAllman;
    else if (bs == kBlockColon)        r.spec.blockStyle = BlockStyle::ColonIndent;
    else {
        r.errors.push_back("backend spec: unknown blockStyle '" + bs + "'");
        return r;
    }

    auto strOr = [&](const char* key, const std::string& dflt) {
        const json::Value& v = doc[key];
        return v.kind == json::Value::Kind::String ? v.asString() : dflt;
    };
    r.spec.stmtEnd      = strOr("stmtEnd", ";");
    r.spec.throwKeyword = strOr("throwKeyword", "throw");
    r.spec.rethrow      = strOr("rethrow", "throw;");
    r.spec.memberOp     = strOr("memberOp", ".");
    r.spec.trueLit      = strOr("trueLit", "true");
    r.spec.falseLit     = strOr("falseLit", "false");
    r.spec.nullLit      = strOr("nullLit", "null");
    r.spec.enumMemberOp   = strOr("enumMemberOp", ".");
    r.spec.constArrayOpen  = strOr("constArrayOpen", "[");
    r.spec.constArrayClose = strOr("constArrayClose", "]");
    r.spec.linksWithoutImports   = doc["linksWithoutImports"].asBool(false);
    r.spec.forbidsShadowedLocals = doc["forbidsShadowedLocals"].asBool(false);
    r.spec.expressionOnlyLambdas = doc["expressionOnlyLambdas"].asBool(false);

    // P40 A1: per-context literal spelling. Shape-checked here so a broken context never emits a literal the
    // target misreads: every context names its quote, maps the backslash and that quote, and carries a
    // code-point template able to spell any BMP character (the `\x$H2` form cannot carry U+2028).
    if (doc.has("escapes")) {
        r.errors.push_back("backend spec: 'escapes' was replaced by 'literals' (P40, manifest schema 2)");
        return r;
    }
    const json::Value& lits = doc["literals"];
    if (lits.kind != json::Value::Kind::Null && lits.kind != json::Value::Kind::Object) {
        r.errors.push_back("backend spec: 'literals' must be an object of named contexts");
        return r;
    }
    for (const auto& kv : lits.members) {
        const std::string where = "backend spec: literals." + kv.first;
        const json::Value& c = kv.second;
        if (c.kind != json::Value::Kind::Object) { r.errors.push_back(where + " must be an object"); return r; }
        for (const auto& m : c.members)
            if (m.first != "quote" && m.first != "map" && m.first != "codepoint" && m.first != "astral") {
                r.errors.push_back(where + ": unknown key '" + m.first + "' (quote, map, codepoint, astral)");
                return r;
            }
        BackendSpec::LiteralContext lc;
        lc.quote = c["quote"].asString();
        loadStringMap(c["map"], lc.map);
        lc.codepoint = c["codepoint"].asString();
        lc.astral = c["astral"].asString();
        if (lc.quote.empty()) { r.errors.push_back(where + " needs a 'quote'"); return r; }
        if (!lc.map.count("\\") || !lc.map.count(lc.quote)) {
            r.errors.push_back(where + ": 'map' must escape the backslash and the context's quote (" + lc.quote + ")");
            return r;
        }
        auto holes = [](const std::string& t) {
            int n = 0;
            for (std::size_t p = 0; (p = t.find("$H", p)) != std::string::npos; p += 2) ++n;
            return n;
        };
        const bool bmpOk = holes(lc.codepoint) == 1 && lc.codepoint.find("$H2") == std::string::npos;
        if (!bmpOk) {
            r.errors.push_back(where + " needs a 'codepoint' template with one $H4, $H8 or $H hole (one that can "
                                       "spell any BMP character)");
            return r;
        }
        if (!lc.astral.empty() && lc.astral != "surrogates" &&
            (holes(lc.astral) != 1 || lc.astral.find("$H4") != std::string::npos ||
             lc.astral.find("$H2") != std::string::npos)) {
            r.errors.push_back(where + ": 'astral' is \"surrogates\" or a template with one $H8 or $H hole");
            return r;
        }
        r.spec.literals[kv.first] = std::move(lc);
    }
    for (const auto& kv : doc["tables"].members)
        if (kv.second.kind == json::Value::Kind::Object) loadStringMap(kv.second, r.spec.tables[kv.first]);

    const json::Value& gen = doc["generics"];
    if (gen.kind == json::Value::Kind::Object) {
        r.spec.genericsStyle       = gen["style"].asString();
        r.spec.genericsBoundsIntro = gen["boundsIntro"].asString();
        r.spec.genericsBoundsSep   = gen["boundsSep"].asString();
        for (const json::Value& k : gen["erase"].items())
            if (k.kind == json::Value::Kind::String) r.spec.genericsErase.insert(k.asString());
    }

    const json::Value& wa = doc["wrapAtom"];
    if (wa.kind == json::Value::Kind::Object) {
        if (wa["recv"].kind != json::Value::Kind::Null) // P40 review A6: the opt-in wrap list became a primary list
            r.errors.push_back("wrapAtom.recv was replaced by wrapAtom.recvPrimary (the kinds emitted as primary "
                               "expressions; every other receiver is parenthesized)");
        for (const json::Value& k : wa["recvPrimary"].items())
            if (k.kind == json::Value::Kind::String) r.spec.wrapAtomRecvPrimary.push_back(k.asString());
        for (const json::Value& k : wa["unary"].items())
            if (k.kind == json::Value::Kind::String) r.spec.wrapAtomUnary.push_back(k.asString());
    }

    const json::Value& ids = doc["identifiers"];
    if (ids.kind == json::Value::Kind::Object) {
        for (const json::Value& k : ids["keywords"].items())
            if (k.kind == json::Value::Kind::String) r.spec.keywords.insert(k.asString());
        const json::Value& esc = ids["escape"];
        if (esc.kind == json::Value::Kind::Object) {
            r.spec.escapeStrategy = esc["strategy"].asString();
            r.spec.escapeWith     = esc["with"].asString();
        }
        const json::Value& man = ids["mangle"];
        if (man.kind == json::Value::Kind::Object) {
            r.spec.mangleFrom = man["replace"].asString();
            r.spec.mangleTo   = man["with"].asString();
        }
        for (const json::Value& k : ids["reserved"].items())
            if (k.kind == json::Value::Kind::String) r.spec.reservedNames.push_back(k.asString());
        for (const json::Value& k : ids["globals"].items())
            if (k.kind == json::Value::Kind::String) r.spec.globalNames.push_back(k.asString());
        const json::Value& fb = ids["functionBuiltins"];
        if (fb.kind == json::Value::Kind::Object) {
            r.spec.fnBuiltinSuffix = fb["suffix"].asString();
            for (const json::Value& k : fb["names"].items())
                if (k.kind == json::Value::Kind::String) r.spec.fnBuiltins.insert(k.asString());
        }
    }

    r.ok = true;
    return r;
}

std::string backendSpecToJson(const BackendSpec& spec) {
    auto mapJson = [](const std::unordered_map<std::string, std::string>& m) {
        std::string o = "{";
        bool first = true;
        for (const auto& kv : m) {
            if (!first) o += ",";
            first = false;
            o += json::quote(kv.first) + ":" + json::quote(kv.second);
        }
        return o + "}";
    };

    std::string delim = "{";
    bool first = true;
    for (const auto& kv : spec.delimited) {
        if (!first) delim += ",";
        first = false;
        delim += json::quote(kv.first) + ":{\"open\":" + json::quote(kv.second.open) +
                 ",\"sep\":" + json::quote(kv.second.sep) + ",\"close\":" + json::quote(kv.second.close) + "}";
    }
    delim += "}";

    const std::string bs = spec.blockStyle == BlockStyle::BracesAllman ? kBlockAllman
                         : spec.blockStyle == BlockStyle::ColonIndent  ? kBlockColon
                                                                       : kBlockKnR;

    std::string gen;
    if (!spec.genericsStyle.empty()) {
        std::vector<std::string> er(spec.genericsErase.begin(), spec.genericsErase.end());
        std::sort(er.begin(), er.end()); // deterministic serialization
        gen = ",\"generics\":{\"style\":" + json::quote(spec.genericsStyle) +
              ",\"boundsIntro\":" + json::quote(spec.genericsBoundsIntro) +
              ",\"boundsSep\":" + json::quote(spec.genericsBoundsSep) + ",\"erase\":[";
        for (std::size_t i = 0; i < er.size(); ++i) { if (i) gen += ","; gen += json::quote(er[i]); }
        gen += "]}";
    }

    std::string wa;
    if (!spec.wrapAtomRecvPrimary.empty() || !spec.wrapAtomUnary.empty()) {
        auto arr = [](const std::vector<std::string>& v) {
            std::string o = "[";
            for (std::size_t i = 0; i < v.size(); ++i) { if (i) o += ","; o += json::quote(v[i]); }
            return o + "]";
        };
        wa = ",\"wrapAtom\":{\"recvPrimary\":" + arr(spec.wrapAtomRecvPrimary) +",\"unary\":" + arr(spec.wrapAtomUnary) + "}";
    }

    std::string tbls;
    if (!spec.tables.empty()) {
        tbls = ",\"tables\":{";
        bool firstMap = true;
        for (const auto& kv : spec.tables) {
            if (!firstMap) tbls += ",";
            firstMap = false;
            tbls += json::quote(kv.first) + ":" + mapJson(kv.second);
        }
        tbls += "}";
    }

    std::string escs;
    if (!spec.literals.empty()) {
        std::vector<std::string> names;
        for (const auto& kv : spec.literals) names.push_back(kv.first);
        std::sort(names.begin(), names.end()); // deterministic serialization
        escs = ",\"literals\":{";
        for (std::size_t i = 0; i < names.size(); ++i) {
            const auto& c = spec.literals.at(names[i]);
            if (i) escs += ",";
            escs += json::quote(names[i]) + ":{\"quote\":" + json::quote(c.quote) + ",\"map\":" + mapJson(c.map) +
                    ",\"codepoint\":" + json::quote(c.codepoint);
            if (!c.astral.empty()) escs += ",\"astral\":" + json::quote(c.astral);
            escs += "}";
        }
        escs += "}";
    }

    std::string ids;
    if (!spec.keywords.empty() || !spec.escapeStrategy.empty() || !spec.mangleFrom.empty() ||
        !spec.reservedNames.empty() || !spec.globalNames.empty() || !spec.fnBuiltins.empty()) {
        std::vector<std::string> kws(spec.keywords.begin(), spec.keywords.end());
        std::sort(kws.begin(), kws.end()); // deterministic serialization
        ids = ",\"identifiers\":{\"keywords\":[";
        for (std::size_t i = 0; i < kws.size(); ++i) { if (i) ids += ","; ids += json::quote(kws[i]); }
        ids += "]";
        if (!spec.escapeStrategy.empty())
            ids += ",\"escape\":{\"strategy\":" + json::quote(spec.escapeStrategy) +
                   ",\"with\":" + json::quote(spec.escapeWith) + "}";
        if (!spec.mangleFrom.empty())
            ids += ",\"mangle\":{\"replace\":" + json::quote(spec.mangleFrom) +
                   ",\"with\":" + json::quote(spec.mangleTo) + "}";
        auto arr = [](const std::vector<std::string>& v) {
            std::string o = "[";
            for (std::size_t i = 0; i < v.size(); ++i) { if (i) o += ","; o += json::quote(v[i]); }
            return o + "]";
        };
        if (!spec.reservedNames.empty()) ids += ",\"reserved\":" + arr(spec.reservedNames);
        if (!spec.globalNames.empty()) ids += ",\"globals\":" + arr(spec.globalNames);
        if (!spec.fnBuiltins.empty()) {
            std::vector<std::string> fbs(spec.fnBuiltins.begin(), spec.fnBuiltins.end());
            std::sort(fbs.begin(), fbs.end()); // deterministic serialization
            ids += ",\"functionBuiltins\":{\"suffix\":" + json::quote(spec.fnBuiltinSuffix) +
                   ",\"names\":" + arr(fbs) + "}";
        }
        ids += "}";
    }

    return "{\"name\":" + json::quote(spec.name) +
           ",\"scalarType\":" + mapJson(spec.scalarType) +
           ",\"intSuffix\":" + mapJson(spec.intSuffix) +
           ",\"binaryOp\":" + mapJson(spec.binaryOp) +
           ",\"wrapInt\":" + mapJson(spec.wrapInt) +
           ",\"preludes\":" + mapJson(spec.preludes) +
           ",\"delimited\":" + delim +
           ",\"blockStyle\":" + json::quote(bs) +
           ",\"stmtEnd\":" + json::quote(spec.stmtEnd) +
           ",\"throwKeyword\":" + json::quote(spec.throwKeyword) +
           ",\"rethrow\":" + json::quote(spec.rethrow) +
           ",\"memberOp\":" + json::quote(spec.memberOp) +
           ",\"trueLit\":" + json::quote(spec.trueLit) +
           ",\"falseLit\":" + json::quote(spec.falseLit) +
           ",\"nullLit\":" + json::quote(spec.nullLit) +
           ",\"enumMemberOp\":" + json::quote(spec.enumMemberOp) +
           ",\"constArrayOpen\":" + json::quote(spec.constArrayOpen) +
           ",\"constArrayClose\":" + json::quote(spec.constArrayClose) +
           std::string(",\"linksWithoutImports\":") + (spec.linksWithoutImports ? "true" : "false") +
           std::string(",\"forbidsShadowedLocals\":") + (spec.forbidsShadowedLocals ? "true" : "false") +
           std::string(",\"expressionOnlyLambdas\":") + (spec.expressionOnlyLambdas ? "true" : "false") + gen + wa + tbls + escs + ids + "}";
}

} // namespace mintplayer::polyglot
