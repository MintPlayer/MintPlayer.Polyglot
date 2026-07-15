#pragma once

// The plugin auto-download pipeline (PRD issue-30 §D1/§D4 / PLAN §P30 slice 3) — what
// `resolveConfiguredTargets` runs for every `pgconfig.json` dependency, and what `polyglot
// install` is a thin wrapper over. Per dependency, in order:
//
//   1. `file:<dir>`      — load in place (local development; never locked).
//   2. in-box satisfied  — the bundled plugin's version IS the CLI's version (the P24 lockstep
//                          invariant), so if that satisfies the range — or the CLI is a dev build
//                          (`0.0.0-dev`) — the in-box copy wins and nothing is fetched.
//   3. lock-first        — a lock entry satisfying the range resolves from the verified cache
//                          with ZERO network I/O; a missing/tampered cache entry re-fetches the
//                          lock's exact pinned URL and must match the pinned integrity.
//   4. registry          — abbreviated packument → maxSatisfying → tarball → SRI verify →
//                          in-exe extract → validateBackend → versioned cache → register →
//                          lock entry written. Data-only, never an npm process (§6 trust model).
//
// A fetched plugin REPLACES a same-named in-box registration (config wins over the bundle).
// Failures are collected as human messages, never thrown, and never partially cached; the
// ResolveState memoizes both successes and failures per (package, spec, config stamp) so a
// long-lived host (LSP, watch) retries on config change instead of hammering the network.

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "mintplayer/polyglot/backend.hpp"
#include "mintplayer/polyglot/polyglot.hpp"

#include "inflate.hpp"
#include "lockfile.hpp"
#include "pgconfig.hpp"
#include "plugincache.hpp"
#include "registry.hpp"
#include "semver.hpp"
#include "sha.hpp"
#include "tar.hpp"

namespace mintplayer::polyglot::cli {

// A bare dependency key ("python") means the first-party plugin package; anything scoped or
// pathed passes through as an npm package name.
inline std::string normalizePluginPackageName(const std::string& key) {
    if (key.find('@') != std::string::npos || key.find('/') != std::string::npos) return key;
    return "@mintplayer/polyglot-target-" + key;
}

// The target a first-party package provides, derivable without downloading; empty for
// third-party packages (their manifest speaks when it arrives).
inline std::string targetNameForPackage(const std::string& npmName) {
    const std::string prefix = "@mintplayer/polyglot-target-";
    return npmName.rfind(prefix, 0) == 0 ? npmName.substr(prefix.size()) : std::string();
}

inline bool cliIsDevBuild() { return Compiler::version().rfind("0.0.0-dev", 0) == 0; }

namespace detail {

inline constexpr std::size_t kMaxInflatedTarball = 64u << 20; // a plugin manifest is KB-scale

// Download one tarball, verify it against `integrity` (SRI; empty = refuse — an unverifiable
// package never installs), extract `package/polyglot-plugin.json`, validate, cache, register.
inline bool installTarball(const HttpGet& http, const std::string& npmName, const std::string& version,
                           const std::string& url, const std::string& integrity, std::string& err) {
    if (integrity.empty()) {
        err = npmName + "@" + version + ": the registry offers no integrity for this tarball — refusing it";
        return false;
    }
    std::string tgz;
    if (!http(url, {}, tgz, err)) return false;
    if (!verifySri(integrity, tgz, err)) {
        err = npmName + "@" + version + ": " + err;
        return false;
    }
    std::string tarBytes;
    if (!gunzip(tgz, tarBytes, kMaxInflatedTarball, err)) return false;
    std::vector<TarEntry> entries;
    if (!tarRead(tarBytes, entries, err)) return false;
    const std::string* manifest = nullptr;
    for (const auto& e : entries)
        if (e.path == "package/polyglot-plugin.json") manifest = &e.data;
    if (!manifest) {
        err = npmName + "@" + version + ": the package carries no package/polyglot-plugin.json — "
              "not a Polyglot target plugin";
        return false;
    }
    if (!validateBackend(*manifest, err)) {
        err = npmName + "@" + version + ": invalid plugin manifest: " + err;
        return false;
    }
    if (!cacheStore(npmName, version, url, integrity, *manifest, err)) return false;
    // Config wins over the bundle: a same-named in-box registration is shadowed, not an error.
    return loadBackend(*manifest, err, /*replaceExisting=*/true);
}

} // namespace detail

struct ResolvedPlugin {
    std::string targetName; // the manifest's name, as registered
    std::string version;
    std::string resolvedUrl;
    std::string integrity;
    bool fetched = false; // false = served by in-box / file: / cache
};

// Resolve ONE versioned dependency (spec already known not to be `file:`). On success the plugin
// is registered; `lock` gains/keeps its pin (caller persists it). `update` skips a satisfied lock
// entry and re-resolves against the registry.
inline bool resolvePluginDependency(const HttpGet& http, const std::string& npmName, const std::string& spec,
                                    const std::filesystem::path& configDir, Lockfile& lock, bool update,
                                    ResolvedPlugin& out, std::string& err) {
    const auto range = parseRange(spec, err);
    if (!range) {
        err = "dependency '" + npmName + "': " + err;
        return false;
    }

    // In-box rule: the bundle IS version <CLI version> (lockstep); dev builds always prefer it.
    // `update` bypasses it — an explicit `install --update` means "ask the registry, period".
    const std::string inboxTarget = targetNameForPackage(npmName);
    if (!update && !inboxTarget.empty() && findBackend(inboxTarget)) {
        const auto cliVer = parseSemVer(Compiler::version());
        if (cliIsDevBuild() || (cliVer && satisfies(*cliVer, *range))) {
            out.targetName = inboxTarget;
            out.version = Compiler::version();
            return true;
        }
    }

    // Lock-first: a satisfying pin resolves from the verified cache with zero network I/O.
    if (!update) {
        if (const auto it = lock.packages.find(npmName); it != lock.packages.end()) {
            const LockEntry& pin = it->second;
            if (const auto pinned = parseSemVer(pin.version); pinned && satisfies(*pinned, *range)) {
                std::string manifest, cacheErr;
                if (cacheLoad(npmName, pin.version, pin.integrity, manifest, cacheErr)) {
                    if (!loadBackend(manifest, err, /*replaceExisting=*/true)) return false;
                    out = {json::parse(manifest)["name"].asString(), pin.version, pin.resolved, pin.integrity, false};
                    return true;
                }
                // Cache miss (or refused-with-reason): re-fetch the EXACT pinned artifact.
                if (detail::installTarball(http, npmName, pin.version, pin.resolved, pin.integrity, err)) {
                    std::string reload;
                    cacheLoad(npmName, pin.version, pin.integrity, reload, cacheErr);
                    out = {json::parse(reload)["name"].asString(), pin.version, pin.resolved, pin.integrity, true};
                    return true;
                }
                err = "dependency '" + npmName + "' is locked to " + pin.version + " but unavailable: " + err +
                      "\n  (" + cacheErr + ")";
                return false;
            }
        }
    }

    // Registry resolve: packument → maxSatisfying → pinned download.
    Packument pk;
    if (!fetchPackument(http, registryBaseFor(npmName, configDir), npmName, pk, err)) {
        err = "dependency '" + npmName + "': " + err;
        return false;
    }
    std::vector<std::string> versions;
    for (const auto& v : pk.versions) versions.push_back(v.version);
    const auto latest = pk.distTags.count("latest") ? pk.distTags.at("latest") : std::string();
    const auto win = maxSatisfying(versions, *range, latest);
    if (!win) {
        err = "dependency '" + npmName + "': no published version satisfies '" + spec + "' (" +
              std::to_string(versions.size()) + " version(s) available, latest: " +
              (latest.empty() ? "?" : latest) + ")";
        return false;
    }
    const PackumentVersion* pv = nullptr;
    for (const auto& v : pk.versions)
        if (v.version == *win) pv = &v;
    // Ancient packages may carry only the sha1 shasum; normalize what we pin to SRI.
    std::string integrity = pv->integrity;
    if (integrity.empty() && !pv->shasum.empty()) integrity = "sha1?" + pv->shasum; // resolved below
    if (integrity.rfind("sha1?", 0) == 0) {
        std::string tgzProbe; // fetch once here so the shasum can be lifted to SRI before install
        if (!http(pv->tarball, {}, tgzProbe, err)) return false;
        if (toHex(sha1(tgzProbe)) != pv->shasum) {
            err = "dependency '" + npmName + "': shasum mismatch for " + *win;
            return false;
        }
        integrity = "sha1-" + base64Encode(sha1(tgzProbe));
    }
    if (!detail::installTarball(http, npmName, *win, pv->tarball, integrity, err)) return false;

    std::string manifest, cacheErr;
    cacheLoad(npmName, *win, integrity, manifest, cacheErr);
    out = {json::parse(manifest)["name"].asString(), *win, pv->tarball, integrity, true};
    lock.packages[npmName] = {*win, pv->tarball, integrity};
    return true;
}

// Success/failure memoization for long-lived hosts (LSP, watch): a key is retried only when the
// config file's stamp changes, so a dead network costs one attempt per config generation.
struct ResolveState {
    std::set<std::string> succeeded;
    std::map<std::string, std::string> failed; // key -> message (repeated verbatim, no re-network)
};

struct ResolveResult {
    bool ok = true;
    bool lockChanged = false;
    std::vector<std::string> messages;
};

inline std::string configStamp(const PgConfig& pc) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(pc.dir / "pgconfig.json", ec);
    return ec ? std::string("?") : std::to_string(t.time_since_epoch().count());
}

// Resolve every dependency in a pgconfig (the `resolveConfiguredTargets` engine). `state` may be
// null (one-shot CLI runs); pass a process-lifetime instance from the LSP/watch loops.
inline ResolveResult resolvePluginDependencies(const PgConfig& pc, const HttpGet& http, bool update,
                                               ResolveState* state) {
    ResolveResult res;
    if (!pc.found || pc.dependencies.empty()) return res;
    Lockfile lock = loadLockfile(pc.dir);
    const std::string stamp = configStamp(pc);

    for (const auto& [key, spec] : pc.dependencies) {
        if (spec.rfind("file:", 0) == 0) { // local plugin dir, resolved in place (never locked)
            const auto manifest = (pc.dir / spec.substr(5) / "polyglot-plugin.json").lexically_normal();
            std::string src, err;
            if (!readFile(manifest, src)) {
                res.ok = false;
                res.messages.push_back("dependency '" + key + "': cannot read " + manifest.string());
            } else if (!findBackend(json::parse(src)["name"].asString()) && !loadBackend(src, err)) {
                res.ok = false;
                res.messages.push_back(manifest.string() + ": " + err);
            }
            continue;
        }

        const std::string npmName = normalizePluginPackageName(key);
        const std::string memoKey = npmName + "|" + spec + "|" + stamp;
        if (state) {
            if (state->succeeded.count(memoKey)) continue;
            if (const auto it = state->failed.find(memoKey); it != state->failed.end()) {
                res.ok = false;
                res.messages.push_back(it->second);
                continue;
            }
        }

        ResolvedPlugin rp;
        std::string err;
        if (resolvePluginDependency(http, npmName, spec, pc.dir, lock, update, rp, err)) {
            if (rp.fetched) res.lockChanged = true;
            if (state) state->succeeded.insert(memoKey);
        } else {
            res.ok = false;
            res.messages.push_back(err);
            if (state) state->failed[memoKey] = err;
        }
    }

    if (res.lockChanged && !saveLockfile(pc.dir, lock)) {
        res.messages.push_back("warning: could not write " + lockfilePath(pc.dir).string() +
                               " (resolution succeeded; the build is not pinned)");
    }
    return res;
}

} // namespace mintplayer::polyglot::cli
