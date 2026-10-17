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

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
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

#include "mintplayer/polyglot/backend.hpp"
#include "mintplayer/polyglot/json.hpp"
#include "mintplayer/polyglot/polyglot.hpp"

#include "glob.hpp"

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

// One pgconfig `include` file-mapping rule (PRD issue-30 D7): route the files `pattern` matches
// (per target) to the path the `output` template names. `targets` empty = every target.
struct IncludeRule {
    std::string pattern; // glob, relative to the config's directory
    std::vector<std::string> targets;
    std::string output;  // template for the emitted file's path STEM (config-relative); the
                         // target's manifest fileExtension() is always appended by the CLI
    // P40 B1: per-rule target options (target -> option -> value), layered over the project-level
    // `targetOptions` for the files this rule matches (first-match-wins, like the routing itself).
    std::map<std::string, std::map<std::string, std::string>> targetOptions;
};

// P40 B1: target options keyed target -> option -> value. Which map applies is a lookup by target NAME
// (data, like forbiddenIdentifiers); what an option MEANS is the target plugin's business.
using TargetOptionsByTarget = std::map<std::string, std::map<std::string, std::string>>;

// Parse a `targetOptions` object (`{ "<target>": { "<option>": "<value>" } }`). A malformed shape is an
// ERROR, never a silent no-op — a mistyped block would leave every default in force with no hint why.
inline void parseTargetOptions(const json::Value& v, TargetOptionsByTarget& out, std::vector<std::string>& errors,
                               const std::string& where) {
    if (v.kind == json::Value::Kind::Null) return;
    if (v.kind != json::Value::Kind::Object) {
        errors.push_back(where + ": \"targetOptions\" must be an object keyed by target name");
        return;
    }
    for (const auto& t : v.members) {
        if (t.second.kind != json::Value::Kind::Object) {
            errors.push_back(where + ": targetOptions." + t.first + " must be an object of option values");
            continue;
        }
        auto& opts = out[t.first];
        for (const auto& o : t.second.members) {
            if (o.second.kind != json::Value::Kind::String) {
                errors.push_back(where + ": targetOptions." + t.first + "." + o.first + " must be a string");
                continue;
            }
            opts[o.first] = o.second.asString();
        }
    }
}

// P40 B1: `--target-option <target>.<name>=<value>` into `out` (a later flag for the same option wins).
inline bool parseTargetOptionFlag(const std::string& arg, TargetOptionsByTarget& out, std::string& err) {
    const std::size_t dot = arg.find('.');
    const std::size_t eq = arg.find('=');
    if (dot == std::string::npos || eq == std::string::npos || dot == 0 || eq < dot + 2) {
        err = "--target-option expects <target>.<name>=<value> (got '" + arg + "')";
        return false;
    }
    out[arg.substr(0, dot)][arg.substr(dot + 1, eq - dot - 1)] = arg.substr(eq + 1);
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
    TargetOptionsByTarget targetOptions; // P40 B1: project-level `targetOptions` (include rules override per file)
    // P38/issue #69: emit source-origin info (C# `#line` directives, TS v3 source map) so generated code
    // attributes back to the `.pg`. `--origin-info` overrides this; false = emit exactly as before.
    bool originInfo = false;
    std::vector<std::string> targets; // the project's target set (drives the default build; P19 slice 10)
    std::vector<std::pair<std::string, std::string>> dependencies; // package -> spec ("file:<dir>", "1.2.3", "^1.2.3", …)
    // project-policy identifier bans (P19 slices 13-15): (target-or-"*", name) fed to checkReservedNames.
    // JSON shape: `"forbiddenIdentifiers": {"*": ["temp"], "python": ["data"]}` — or a bare array = all targets.
    std::vector<std::pair<std::string, std::string>> forbiddenIdentifiers;
    std::vector<IncludeRule> include; // P30 slice 7: per-file output routing (+ discovery when no inputs)
    std::vector<std::string> errors;  // malformed-manifest diagnostics (a broken rule must not silently no-op)
    std::filesystem::path dir; // where the config was found (file: deps + the lockfile resolve against it)
};

inline PgConfig loadPgConfig(const std::filesystem::path& startDir) {
    for (std::filesystem::path d = startDir;; d = d.parent_path()) {
        std::string src;
        if (readFile(d / "pgconfig.json", src)) {
            std::string jsonErr;
            json::Value v = json::parse(src, jsonErr);
            PgConfig pc;
            pc.found = true;
            pc.dir = d;
            if (!jsonErr.empty()) pc.errors.push_back((d / "pgconfig.json").string() + ": " + jsonErr);
            std::string r = v["root"].asString();
            pc.root = (r.empty() ? d : (d / r)).lexically_normal().string();
            for (const auto& e : v["lib"].items()) { if (!pc.lib.empty()) pc.lib += ","; pc.lib += e.asString(); }
            parseTargetOptions(v["targetOptions"], pc.targetOptions, pc.errors, "pgconfig.json");
            // P40 B1 (D9): the top-level C# `access` key moved under targetOptions.csharp — a clean
            // cutover, so the old spelling is refused with the new one named rather than silently ignored.
            if (v["access"].kind != json::Value::Kind::Null)
                pc.errors.push_back("pgconfig.json: top-level \"access\" was removed; set it per target instead: "
                                    "\"targetOptions\": { \"csharp\": { \"access\": \"" + v["access"].asString() + "\" } }");
            pc.originInfo = v["originInfo"].asBool(false); // P38/issue #69
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
            // P30 slice 7: `include` file-mapping rules. A malformed rule is an ERROR, never a
            // silent no-op — a typo'd rule silently falling back would misplace outputs.
            for (const auto& e : v["include"].items()) {
                IncludeRule rule;
                rule.pattern = e["pattern"].asString();
                rule.output = e["output"].asString();
                const json::Value& t = e["target"];
                if (t.kind == json::Value::Kind::String) rule.targets.push_back(t.asString());
                else for (const auto& te : t.items())
                    if (te.kind == json::Value::Kind::String) rule.targets.push_back(te.asString());
                parseTargetOptions(e["targetOptions"], rule.targetOptions, pc.errors,
                                   "pgconfig.json include rule '" + rule.pattern + "'");
                if (rule.pattern.empty() || rule.output.empty())
                    pc.errors.push_back("pgconfig.json: an include rule needs both \"pattern\" and \"output\"");
                else
                    pc.include.push_back(std::move(rule));
            }
            return pc;
        }
        if (!d.has_parent_path() || d.parent_path() == d) return {};
    }
}

// Route one source file for one target through the config's `include` rules (PRD D7):
// first-match-wins in rule order. Returns the ABSOLUTE emitted-file path STEM — the caller appends
// the target's manifest fileExtension() — or nullopt when no rule matches (the caller falls back to
// --out / the input's directory). A template error sets `err` (distinct from a clean no-match; the
// caller must check it) — a typo'd placeholder is a refusal, never a silent fallback.
// The first `include` rule (in rule order) that applies to `sourceAbs` for `targetName` — the one rule that
// both routes the file and contributes its per-rule targetOptions. nullptr = no rule applies.
inline const IncludeRule* matchIncludeRule(const PgConfig& pc, const std::filesystem::path& sourceAbs,
                                           const std::string& targetName, std::filesystem::path* relOut = nullptr,
                                           GlobResult* globOut = nullptr) {
    if (!pc.found || pc.include.empty() || sourceAbs.empty()) return nullptr;
    std::error_code ec;
    const std::filesystem::path rel = std::filesystem::relative(sourceAbs, pc.dir, ec);
    if (ec || rel.empty() || *rel.begin() == "..") return nullptr; // outside the config's tree
    const std::string relStr = rel.generic_string();
    for (const auto& rule : pc.include) {
        if (!rule.targets.empty() &&
            std::find(rule.targets.begin(), rule.targets.end(), targetName) == rule.targets.end())
            continue;
        const GlobResult g = globMatch(rule.pattern, relStr);
        if (!g.matched) continue;
        if (relOut) *relOut = rel;
        if (globOut) *globOut = g;
        return &rule;
    }
    return nullptr;
}

// P40 B1: the target options one file sees for one target: the project-level `targetOptions.<target>`,
// overlaid by its first matching include rule's, overlaid by `--target-option` (the most explicit wins).
inline std::map<std::string, std::string> fileTargetOptions(const PgConfig& pc, const TargetOptionsByTarget& flags,
                                                            const std::filesystem::path& sourceAbs,
                                                            const std::string& targetName) {
    std::map<std::string, std::string> out;
    auto overlay = [&](const TargetOptionsByTarget& layer) {
        auto it = layer.find(targetName);
        if (it == layer.end()) return;
        for (const auto& kv : it->second) out[kv.first] = kv.second;
    };
    if (pc.found) {
        overlay(pc.targetOptions);
        if (const IncludeRule* r = matchIncludeRule(pc, sourceAbs, targetName)) overlay(r->targetOptions);
    }
    overlay(flags);
    return out;
}

// P40 B1: the LibConfig::targetOptions callback for one compiled closure: origin "" is `entryAbs`, any other
// origin is a module's canonical path. Captures by value — the callback may outlive the caller's locals.
inline std::function<std::map<std::string, std::string>(const std::string&, const std::string&)>
targetOptionsFn(const PgConfig& pc, const TargetOptionsByTarget& flags, const std::filesystem::path& entryAbs) {
    return [pc, flags, entryAbs](const std::string& target, const std::string& origin) {
        return fileTargetOptions(pc, flags, origin.empty() ? entryAbs : std::filesystem::path(origin), target);
    };
}

// P40 B1: config-time validation of every targetOptions layer (project, include rules, flags): each key must
// name a loaded target and each option must be one that target declares, with a value matching its pattern.
// Done up front so a typo refuses even for a target this run does not build, and before anything is written.
inline void validateTargetOptions(const PgConfig& pc, const TargetOptionsByTarget& flags,
                                  std::vector<std::string>& errors) {
    auto check = [&](const TargetOptionsByTarget& layer, const std::string& where) {
        for (const auto& [target, opts] : layer) {
            const Backend* b = findBackend(target);
            if (!b) {
                errors.push_back(where + ": targetOptions names unknown target '" + target + "' (loaded targets: " +
                                 [] { std::string s; for (const auto& n : backendNames()) s += (s.empty() ? "" : ", ") + n; return s; }() + ")");
                continue;
            }
            std::map<std::string, std::string> resolved;
            std::string err;
            if (!resolveTargetOptions(*b, opts, resolved, err)) errors.push_back(where + ": " + err);
        }
    };
    if (pc.found) {
        check(pc.targetOptions, "pgconfig.json");
        for (const auto& r : pc.include) check(r.targetOptions, "pgconfig.json include rule '" + r.pattern + "'");
    }
    check(flags, "--target-option");
}

inline std::optional<std::filesystem::path> routeIncludeOutput(const PgConfig& pc,
                                                               const std::filesystem::path& sourceAbs,
                                                               const std::string& targetName,
                                                               std::string& err) {
    std::filesystem::path rel;
    GlobResult g;
    if (const IncludeRule* r = matchIncludeRule(pc, sourceAbs, targetName, &rel, &g)) {
        const IncludeRule& rule = *r;
        TemplateValues vals;
        vals.filename = sourceAbs.stem().string();
        vals.directory = rel.parent_path().generic_string(); // config-relative, no trailing '/'
        vals.recursiveDir = g.recursiveDir;                  // trailing '/' (MSBuild semantics)
        vals.targetLanguage = targetName;
        std::string expanded;
        if (!expandOutputTemplate(rule.output, vals, expanded, err)) return std::nullopt; // err set
        // An empty %(Directory) can leave a leading '/'; the template stays config-relative.
        while (!expanded.empty() && expanded.front() == '/') expanded.erase(0, 1);
        return (pc.dir / expanded).lexically_normal();
    }
    return std::nullopt;
}

// Trailing-separator-insensitive directory identity ("obj/x/." == "obj/x/" == "obj/x") — MSBuild's
// quoting guard passes `--out "<dir>\."`, which must not read as a different directory. Also the
// normalized '/'-separated key handed to Core's relative-specifier arithmetic (P30 slice 8).
inline std::string normalizedDirKey(const std::filesystem::path& p) {
    std::string s = p.lexically_normal().generic_string();
    while (s.size() > 1 && (s.back() == '/' || s.back() == '.')) {
        if (s.back() == '.' && (s.size() < 2 || s[s.size() - 2] != '/')) break; // keep "dir/name."
        s.pop_back();
    }
    return s;
}

// Resolve the OUTPUT PATHS for one compiled closure (entry + linked modules) for one target
// (PRD D7). `outs` is filled parallel to [entry, result.modules...]:
//  * include-rule routing per file (skipped entirely under `flagRouted` — explicit --target+--out),
//  * the fallback dir for unmatched files (--out, or the input's directory),
//  * the synthesized prelude (empty sourcePath) follows the ENTRY's directory,
//  * the closure rule: every file must land in ONE directory — emitted flat "./<name>" imports would
//    miscompile across a split, so refuse with the split named — UNLESS the target declares
//    `crossDirImports` (`allowSplit`, P30 slice 8: the compiler then emits real relative specifiers).
inline bool resolveClosureOutputs(const PgConfig& pc, bool flagRouted, const std::filesystem::path& input,
                                  const EmitResult& result, const std::string& targetName, const char* ext,
                                  const std::filesystem::path& fallbackDir,
                                  std::vector<std::filesystem::path>& outs, std::string& err,
                                  bool allowSplit = false) {
    namespace fs = std::filesystem;
    outs.clear();
    auto route = [&](const fs::path& sourceAbs, const std::string& stem, bool preludeLike,
                     const fs::path& preludeDir, fs::path& out) -> bool {
        if (preludeLike) { // no source: follows the entry's directory
            out = preludeDir / stem;
            out += ext;
            return true;
        }
        if (!flagRouted) {
            std::string terr;
            if (auto r = routeIncludeOutput(pc, sourceAbs, targetName, terr)) {
                out = *r;
                out += ext;
                return true;
            }
            if (!terr.empty()) { err = terr; return false; }
        }
        out = fallbackDir / stem;
        out += ext;
        return true;
    };

    fs::path entryOut;
    std::error_code ec;
    if (!route(fs::absolute(input, ec), input.stem().string(), false, {}, entryOut)) return false;
    outs.push_back(entryOut);
    const fs::path entryDir = entryOut.parent_path();
    const std::string entryDirKey = normalizedDirKey(entryDir);

    for (const auto& mf : result.modules) {
        fs::path mout;
        if (!route(fs::path(mf.sourcePath), mf.basename, mf.sourcePath.empty(), entryDir, mout)) return false;
        if (!allowSplit && normalizedDirKey(mout.parent_path()) != entryDirKey) {
            err = "include rules split the import closure of '" + input.string() + "' for target '" +
                  targetName + "': module '" + mf.sourcePath + "' would emit to '" +
                  mout.parent_path().string() + "' but its entry emits to '" + entryDir.string() +
                  "' (emitted imports are \"./<name>\"; route the whole closure to one directory)";
            return false;
        }
        outs.push_back(mout);
    }
    return true;
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
