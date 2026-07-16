#pragma once

// The versioned plugin cache (PRD issue-30 §D3 / PLAN §P30 slice 2). Layout:
//
//     <pluginCacheDir()>/<npm-name>/<version>/polyglot-plugin.json   (scoped names nest: @scope/pkg)
//                                            /meta.json              { name, version, resolved,
//                                                                      integrity, manifestSha512 }
//
// Trust properties (§6.3 "re-verified on load", made concrete):
//  * meta.json records the SRI integrity of the tarball the entry came from AND a sha512 of the
//    extracted manifest; cacheLoad re-hashes the manifest and cross-checks both — a tampered or
//    torn entry reads as ABSENT (refused, re-fetchable), never as trusted content.
//  * Writes are atomic: stage into a sibling temp dir, then rename into place. A concurrent
//    build can never observe a half-written entry; losing the rename race to an identical entry
//    is success.
//
// This versioned layout SUPERSEDES both the old flat `<cache>/<target>/polyglot-plugin.json` keying
// (unversioned — a second install silently overwrote the first) and the design doc's separate
// global registry.json index (the per-project lockfile + this directory ARE the installed set).

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "mintplayer/polyglot/json.hpp"

#include "pgconfig.hpp" // readFile/writeFile, pluginCacheDir
#include "sha.hpp"

namespace mintplayer::polyglot::cli {

// "@scope/pkg" nests naturally as two path levels; refuse anything path-hostile outright (npm
// names are lowercase URL-safe — a name that isn't is an attack, not a package).
inline bool cacheSafeName(const std::string& name) {
    if (name.empty() || name.size() > 214) return false; // npm's own name-length cap
    for (char ch : name)
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '@' || ch == '/' || ch == '.' ||
              ch == '-' || ch == '_'))
            return false;
    return name.find("..") == std::string::npos && name[0] != '.' && name[0] != '/' &&
           name.find('/') != 0 && std::count(name.begin(), name.end(), '/') <= 1;
}

inline std::filesystem::path cacheEntryDir(const std::string& name, const std::string& version) {
    std::filesystem::path p = pluginCacheDir();
    std::size_t b = 0; // split "@scope/pkg" into path levels explicitly (no separator surprises)
    while (b < name.size()) {
        std::size_t e = name.find('/', b);
        if (e == std::string::npos) e = name.size();
        p /= name.substr(b, e - b);
        b = e + 1;
    }
    return p / version;
}

// Load + verify a cache entry. `expectedIntegrity` (from the lockfile) may be empty when no lock
// pins it yet — the manifest hash still must verify. Any mismatch reads as absent-with-reason.
inline bool cacheLoad(const std::string& name, const std::string& version,
                      const std::string& expectedIntegrity, std::string& manifestJson, std::string& err) {
    const std::filesystem::path dir = cacheEntryDir(name, version);
    std::string metaSrc;
    if (!readFile(dir / "meta.json", metaSrc) || !readFile(dir / "polyglot-plugin.json", manifestJson)) {
        err = "cache: no entry for " + name + "@" + version;
        return false;
    }
    const json::Value meta = json::parse(metaSrc);
    if (meta["name"].asString() != name || meta["version"].asString() != version) {
        err = "cache: meta.json does not match its directory for " + name + "@" + version;
        return false;
    }
    if (toHex(sha512(manifestJson)) != meta["manifestSha512"].asString()) {
        err = "cache: manifest hash mismatch for " + name + "@" + version + " (tampered or torn entry) — "
              "refusing it; it will be re-fetched";
        return false;
    }
    if (!expectedIntegrity.empty() && meta["integrity"].asString() != expectedIntegrity) {
        err = "cache: entry for " + name + "@" + version + " came from a tarball with integrity " +
              meta["integrity"].asString() + " but the lockfile pins " + expectedIntegrity +
              " — refusing it";
        return false;
    }
    return true;
}

// Store a validated manifest. `integrity` is the (verified) SRI of the source tarball.
inline bool cacheStore(const std::string& name, const std::string& version, const std::string& resolved,
                       const std::string& integrity, const std::string& manifestJson, std::string& err) {
    namespace fs = std::filesystem;
    if (!cacheSafeName(name) || version.find('/') != std::string::npos ||
        version.find("..") != std::string::npos) {
        err = "cache: refusing unsafe cache key '" + name + "@" + version + "'";
        return false;
    }
    const fs::path dest = cacheEntryDir(name, version);
    const fs::path tmp = dest.parent_path() /
                         (".tmp-" + version + "-" + std::to_string(
#ifdef _WIN32
                             GetCurrentProcessId()
#else
                             static_cast<unsigned long>(getpid())
#endif
                             ));
    std::error_code ec;
    fs::remove_all(tmp, ec);

    std::string meta = "{\n";
    meta += "  \"name\": " + json::quote(name) + ",\n";
    meta += "  \"version\": " + json::quote(version) + ",\n";
    meta += "  \"resolved\": " + json::quote(resolved) + ",\n";
    meta += "  \"integrity\": " + json::quote(integrity) + ",\n";
    meta += "  \"manifestSha512\": " + json::quote(toHex(sha512(manifestJson))) + "\n";
    meta += "}\n";

    if (!writeFile(tmp / "polyglot-plugin.json", manifestJson) || !writeFile(tmp / "meta.json", meta)) {
        err = "cache: cannot write to " + tmp.string();
        fs::remove_all(tmp, ec);
        return false;
    }
    fs::remove_all(dest, ec); // replacing an entry (e.g. after a refused/tampered load) is fine
    fs::rename(tmp, dest, ec);
    if (ec) {
        fs::remove_all(tmp, ec);
        // Lost the race to a concurrent identical install? That entry must still verify.
        std::string ignored;
        if (cacheLoad(name, version, integrity, ignored, err)) return true;
        err = "cache: cannot move entry into place at " + dest.string();
        return false;
    }
    return true;
}

} // namespace mintplayer::polyglot::cli
