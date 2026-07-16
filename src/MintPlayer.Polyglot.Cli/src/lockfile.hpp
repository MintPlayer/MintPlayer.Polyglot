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
};

inline std::filesystem::path lockfilePath(const std::filesystem::path& configDir) {
    return configDir / "pgconfig.lock.json";
}

inline Lockfile loadLockfile(const std::filesystem::path& configDir) {
    Lockfile lf;
    std::string src;
    if (!readFile(lockfilePath(configDir), src)) return lf;
    const json::Value v = json::parse(src);
    if (v["lockfileVersion"].asInt() != 1) return lf; // unknown future version: ignore, re-resolve
    lf.found = true;
    for (const auto& kv : v["packages"].members) {
        LockEntry e;
        e.version = kv.second["version"].asString();
        e.resolved = kv.second["resolved"].asString();
        e.integrity = kv.second["integrity"].asString();
        if (!e.version.empty()) lf.packages[kv.first] = std::move(e);
    }
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
