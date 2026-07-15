#pragma once

// The workspace-config + plugin-cache glue extracted out of main.cpp's anonymous namespace
// (PLAN §P30 slice 0) so the resolution pipeline is unit-testable — the tests add Cli/src to
// their include path (the watch.hpp precedent). Core stays IO-free; everything here is CLI-layer.
//
//  * PgConfig / loadPgConfig — the minimal project manifest (PRD §4.8), found by walking up from
//    a start directory to the first pgconfig.json.
//  * pluginCacheDir — the durable per-user plugin cache `polyglot install` populates and the
//    auto-download pipeline (P30) fills at build time. POLYGLOT_CACHE overrides it wholesale
//    (test harnesses point it at a scratch dir; also an escape hatch for odd machines).
//  * loadPluginFile — one manifest file into the Core registry, reporting (not throwing) failures.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef Yield // winbase.h macro; ir::StmtKind::Yield must survive
#endif

#include "mintplayer/polyglot/json.hpp"
#include "mintplayer/polyglot/polyglot.hpp"

namespace mintplayer::polyglot::cli {

// Read an entire file into a string. Returns false if it could not be opened.
inline bool readFile(const std::filesystem::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

inline bool writeFile(const std::filesystem::path& path, const std::string& content) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec); // `--out newdir` just works
    std::ofstream os(path, std::ios::binary);
    if (!os) return false;
    os << content;
    return true;
}

// The minimal project manifest (PRD §4.8): `{ "root": <dir>, "lib": ["io","math"] }`. `root` is resolved
// against the pgconfig.json that declares it; `lib` is the ambient prelude. Found by walking up from a start
// directory to the first pgconfig.json. Parsed with the Core JSON reader — Core stays IO-free; this is
// CLI/LSP glue that produces the same `root`/`lib` the compiler already accepts.
struct PgConfig {
    bool found = false;
    std::string root; // absolute, resolved against the config's directory
    std::string lib;  // comma-joined lib names
    std::string access; // emitted C# accessibility ("public"/"internal"); empty = target default
    std::vector<std::string> targets; // the project's target set (drives the default build; P19 slice 10)
    std::vector<std::pair<std::string, std::string>> dependencies; // package -> spec ("file:<dir>", "1.2.3", "^1.2.3", …)
    // project-policy identifier bans (P19 slices 13-15): (target-or-"*", name) fed to checkReservedNames.
    // JSON shape: `"forbiddenIdentifiers": {"*": ["temp"], "python": ["data"]}` — or a bare array = all targets.
    std::vector<std::pair<std::string, std::string>> forbiddenIdentifiers;
    std::filesystem::path dir; // where the config was found (file: deps + the lockfile resolve against it)
};

inline PgConfig loadPgConfig(const std::filesystem::path& startDir) {
    for (std::filesystem::path d = startDir;; d = d.parent_path()) {
        std::string src;
        if (readFile(d / "pgconfig.json", src)) {
            json::Value v = json::parse(src);
            PgConfig pc;
            pc.found = true;
            pc.dir = d;
            std::string r = v["root"].asString();
            pc.root = (r.empty() ? d : (d / r)).lexically_normal().string();
            for (const auto& e : v["lib"].items()) { if (!pc.lib.empty()) pc.lib += ","; pc.lib += e.asString(); }
            pc.access = v["access"].asString();
            for (const auto& e : v["targets"].items())
                if (e.kind == json::Value::Kind::String) pc.targets.push_back(e.asString());
            for (const auto& kv : v["dependencies"].members)
                if (kv.second.kind == json::Value::Kind::String) pc.dependencies.push_back({kv.first, kv.second.asString()});
            const json::Value& fi = v["forbiddenIdentifiers"];
            if (fi.kind == json::Value::Kind::Array) { // bare array = every target
                for (const auto& e : fi.items())
                    if (e.kind == json::Value::Kind::String) pc.forbiddenIdentifiers.push_back({"*", e.asString()});
            } else if (fi.kind == json::Value::Kind::Object) {
                for (const auto& kv : fi.members)
                    for (const auto& e : kv.second.items())
                        if (e.kind == json::Value::Kind::String) pc.forbiddenIdentifiers.push_back({kv.first, e.asString()});
            }
            return pc;
        }
        if (!d.has_parent_path() || d.parent_path() == d) return {};
    }
}

// The user-level plugin cache: %LOCALAPPDATA%\polyglot\plugins on Windows, the XDG data dir on
// Linux, ~/Library/Application Support on macOS — a DURABLE per-user location so installed
// plugins survive a reboot. POLYGLOT_CACHE (when set) replaces the whole path — the fake-registry
// test gate builds into a scratch cache without touching the user's real one.
// temp_directory_path() is only the last resort (never fail here).
inline std::filesystem::path pluginCacheDir() {
#ifdef _WIN32
    char buf[4096];
    unsigned long n = GetEnvironmentVariableA("POLYGLOT_CACHE", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) return std::filesystem::path(std::string(buf, n));
    n = GetEnvironmentVariableA("LOCALAPPDATA", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) return std::filesystem::path(std::string(buf, n)) / "polyglot" / "plugins";
#elif defined(__APPLE__)
    if (const char* env = std::getenv("POLYGLOT_CACHE"); env && *env) return std::filesystem::path(env);
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / "Library" / "Application Support" / "polyglot" / "plugins";
#else
    if (const char* env = std::getenv("POLYGLOT_CACHE"); env && *env) return std::filesystem::path(env);
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / "polyglot" / "plugins";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".local" / "share" / "polyglot" / "plugins";
#endif
    return std::filesystem::temp_directory_path() / "polyglot" / "plugins";
}

// Load one plugin manifest file into the registry; reports (but does not throw on) failures.
inline bool loadPluginFile(const std::filesystem::path& manifest) {
    std::string src;
    if (!readFile(manifest, src)) return false;
    std::string err;
    if (!loadBackend(src, err)) {
        std::cerr << "polyglot: " << manifest.string() << ": " << err << "\n";
        return false;
    }
    return true;
}

} // namespace mintplayer::polyglot::cli
