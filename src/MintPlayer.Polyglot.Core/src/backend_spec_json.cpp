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
    SpecLoadResult r;
    json::Value doc = json::parse(json);
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
    r.spec.trueLit      = strOr("trueLit", "true");
    r.spec.falseLit     = strOr("falseLit", "false");
    r.spec.nullLit      = strOr("nullLit", "null");

    for (const auto& kv : doc["escapes"].members)
        if (kv.second.kind == json::Value::Kind::Object) loadStringMap(kv.second, r.spec.escapes[kv.first]);

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
    if (!spec.keywords.empty() || !spec.escapeStrategy.empty() || !spec.mangleFrom.empty()) {
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
        ids += "}";
    }

    return "{\"name\":" + json::quote(spec.name) +
           ",\"scalarType\":" + mapJson(spec.scalarType) +
           ",\"intSuffix\":" + mapJson(spec.intSuffix) +
           ",\"binaryOp\":" + mapJson(spec.binaryOp) +
           ",\"delimited\":" + delim +
           ",\"blockStyle\":" + json::quote(bs) +
           ",\"stmtEnd\":" + json::quote(spec.stmtEnd) +
           ",\"throwKeyword\":" + json::quote(spec.throwKeyword) +
           ",\"trueLit\":" + json::quote(spec.trueLit) +
           ",\"falseLit\":" + json::quote(spec.falseLit) +
           ",\"nullLit\":" + json::quote(spec.nullLit) + escs + ids + "}";
}

} // namespace mintplayer::polyglot
