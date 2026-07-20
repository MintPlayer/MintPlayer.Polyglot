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
    return loadBackendSpec(json::parse(json));
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

    for (const auto& kv : doc["escapes"].members)
        if (kv.second.kind == json::Value::Kind::Object) loadStringMap(kv.second, r.spec.escapes[kv.first]);
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
        for (const json::Value& k : wa["recv"].items())
            if (k.kind == json::Value::Kind::String) r.spec.wrapAtomRecv.push_back(k.asString());
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
    if (!spec.wrapAtomRecv.empty() || !spec.wrapAtomUnary.empty()) {
        auto arr = [](const std::vector<std::string>& v) {
            std::string o = "[";
            for (std::size_t i = 0; i < v.size(); ++i) { if (i) o += ","; o += json::quote(v[i]); }
            return o + "]";
        };
        wa = ",\"wrapAtom\":{\"recv\":" + arr(spec.wrapAtomRecv) + ",\"unary\":" + arr(spec.wrapAtomUnary) + "}";
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
    if (!spec.escapes.empty()) {
        escs = ",\"escapes\":{";
        bool firstMap = true;
        for (const auto& kv : spec.escapes) {
            if (!firstMap) escs += ",";
            firstMap = false;
            escs += json::quote(kv.first) + ":" + mapJson(kv.second);
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
           ",\"nullLit\":" + json::quote(spec.nullLit) + gen + wa + tbls + escs + ids + "}";
}

} // namespace mintplayer::polyglot
