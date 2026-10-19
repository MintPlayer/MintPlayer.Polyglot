#pragma once

// pgconfig.lock.json (PRD issue-30 §D2 / PLAN §P30 slice 2) — the committed pin that makes plugin
// resolution deterministic and offline-capable. Flat by design (plugins have no transitive deps):
//
//     {
//       "lockfileVersion": 1,
//       "packages": {
//         "@mintplayer/polyglot-target-python": {
//           "version": "0.3.1",
//           "resolved": "https://registry.npmjs.org/.../polyglot-target-python-0.3.1.tgz",
//           "integrity": "sha512-…"
//         }
//       }
//     }
//
// Lock-first resolution: when the locked version satisfies the requested range and the cache
// verifies, no network I/O happens at all. The lockfile lives next to pgconfig.json and is meant
// to be committed; keys emit in sorted order so diffs stay stable.

#include <filesystem>
#include <map>
#include <string>

#include "mintplayer/polyglot/json.hpp"

#include "pgconfig.hpp" // readFile/writeFile

namespace mintplayer::polyglot::cli {

struct LockEntry {
    std::string version;
    std::string resolved;
    std::string integrity;
};

struct Lockfile {
    bool found = false;
    std::map<std::string, LockEntry> packages; // npm package name -> pin (sorted = stable diffs)
    std::string error; // non-empty: the file exists but is malformed (P40 review r4) — refuse, never re-resolve
};

inline std::filesystem::path lockfilePath(const std::filesystem::path& configDir) {
    return configDir / "pgconfig.lock.json";
}

// P40 review r4: parsed STRICTLY (json::parse with an error: a duplicate key, invalid UTF-8, a bad escape, a
// truncated document) and its shape validated. The lockfile is a trust root — it pins the integrity every
// download is checked against — so a lenient read (a duplicate key took the FIRST value, unlike npm/JS) must
// not silently change what is pinned. A malformed lockfile sets `error`; the caller refuses (exit 64) rather than
// re-resolving over it and rewriting it. An unknown future `lockfileVersion` is still ignored (re-resolve).
inline Lockfile loadLockfile(const std::filesystem::path& configDir) {
    using K = json::Value::Kind;
    Lockfile lf;
    std::string src;
    if (!readFile(lockfilePath(configDir), src)) return lf;
    std::string jsonErr;
    const json::Value v = json::parse(src, jsonErr);
    if (!jsonErr.empty()) { lf.error = "malformed JSON: " + jsonErr; return lf; }
    if (v.kind != K::Object || v["lockfileVersion"].kind != K::Number) {
        lf.error = "not a pgconfig lockfile (expected an object with a numeric \"lockfileVersion\")";
        return lf;
    }
    if (v["lockfileVersion"].asInt() != 1) return lf; // unknown future version: ignore, re-resolve
    if (v["packages"].kind != K::Object) { lf.error = "\"packages\" must be an object"; return lf; }
    for (const auto& kv : v["packages"].members) {
        const json::Value& p = kv.second;
        if (p.kind != K::Object || p["version"].kind != K::String || p["resolved"].kind != K::String ||
            p["integrity"].kind != K::String || p["version"].asString().empty()) {
            lf.error = "packages." + kv.first + " must be an object with string \"version\", \"resolved\" and \"integrity\"";
            return {false, {}, lf.error};
        }
        lf.packages[kv.first] = {p["version"].asString(), p["resolved"].asString(), p["integrity"].asString()};
    }
    lf.found = true;
    return lf;
}

inline bool saveLockfile(const std::filesystem::path& configDir, const Lockfile& lf) {
    std::string out = "{\n  \"lockfileVersion\": 1,\n  \"packages\": {";
    bool first = true;
    for (const auto& [name, e] : lf.packages) {
        out += first ? "\n" : ",\n";
        first = false;
        out += "    " + json::quote(name) + ": {\n";
        out += "      \"version\": " + json::quote(e.version) + ",\n";
        out += "      \"resolved\": " + json::quote(e.resolved) + ",\n";
        out += "      \"integrity\": " + json::quote(e.integrity) + "\n";
        out += "    }";
    }
    out += first ? "}\n}\n" : "\n  }\n}\n";
    return writeFile(lockfilePath(configDir), out);
}

} // namespace mintplayer::polyglot::cli
