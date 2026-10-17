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
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
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

// A path as UTF-8 bytes, whatever the process code page — for the files other tools read as UTF-8 (MSBuild's
// ReadLinesFromFile: the depfile and the outputs list). The executables also run with UTF-8 as their ANSI code
// page (utf8.manifest), so `.string()` agrees; this keeps those two files right even where the manifest is
// not honoured (Windows before 10 1903). P40 review r3, gating 1.
inline std::string utf8(const std::filesystem::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

// Read an entire file into a string. Returns false if it could not be opened.
inline bool readFile(const std::filesystem::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// P40 review r3: a temp file a crashed (or killed) run left behind — `<name>.pgtmp.<…>` from writeFile, or
// `<name>.pgcase.tmp` from a case-only rename. Swept once per directory per process, and only when older than
// ten minutes: a parallel build (net8;net9 sharing a routed twin) may have its own in-flight temp there.
inline bool isPolyglotTempName(const std::string& name) {
    return name.find(".pgtmp.") != std::string::npos ||
           (name.size() > 11 && name.compare(name.size() - 11, 11, ".pgcase.tmp") == 0);
}
inline void sweepStaleTemps(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    static std::mutex mu;
    static std::set<fs::path> swept;
    {
        std::lock_guard<std::mutex> lock(mu);
        if (!swept.insert(dir).second) return;
    }
    std::error_code ec;
    const auto cutoff = fs::file_time_type::clock::now() - std::chrono::minutes(10);
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fec;
        if (!it->is_regular_file(fec) || !isPolyglotTempName(it->path().filename().string())) continue;
        if (const auto t = fs::last_write_time(it->path(), fec); !fec && t < cutoff) fs::remove(it->path(), fec);
    }
}

// Atomic replace (P40 review of #75): write a uniquely named temp file next to `path`, then rename it over
// `path`. Truncate-then-write let a concurrent reader (parallel TFM builds sharing a routed twin) see a half
// written file and refuse it as not Polyglot's. Readers now see the old bytes or the new ones, never a mix.
// P40 review r3: when the rename cannot replace the target — on Windows, exactly while a reader holds it open
// without FILE_SHARE_DELETE, as MSVC's ifstream does — it is RETRIED with a short backoff (~3 s in all), then
// the write FAILS. It used to fall back to truncate-and-write, which is the torn read the rename exists to
// prevent, and the run exited 0.
inline bool writeFile(const std::filesystem::path& path, const std::string& content) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = path.has_parent_path() ? path.parent_path() : fs::path(".");
    if (path.has_parent_path()) fs::create_directories(dir, ec); // `--out newdir` just works
    sweepStaleTemps(dir);
    static std::atomic<unsigned> counter{0};
    const auto stamp = static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::path tmp = path;
    tmp += ".pgtmp." + std::to_string(std::random_device{}()) + "." + std::to_string(stamp % 1000000) + "." +
           std::to_string(counter++);
    {
        std::ofstream os(tmp, std::ios::binary);
        if (!os) return false;
        os << content;
        os.close();
        if (os.fail()) { fs::remove(tmp, ec); return false; }
    }
    for (int attempt = 0, delayMs = 10;; ++attempt, delayMs = std::min(delayMs * 2, 500)) {
        ec.clear();
        fs::rename(tmp, path, ec);
        if (!ec) return true;
        if (attempt == 10) break; // 10+20+40+…+500 ms ≈ 3 s
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
    }
    fs::remove(tmp, ec);
    return false;
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
    bool owned = false;  // P40 B5: the literal output folder is Polyglot's (stale-twin scan; discovery mode only)
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

// P40 A5: the walk owns the FORM of its input. A relative start ("." for `a.pg`, "LibA" for `LibA/a.pg`)
// has no lexical parent to walk to, so it used to stop early and silently ignore the config. Absolute +
// lexical normalisation (not weakly_canonical: the watch precedent, and symlinked checkouts keep working)
// makes `pc.dir` and `pc.root` always absolute, whatever the caller passed.
inline std::filesystem::path absoluteDir(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::path d = std::filesystem::absolute(p.empty() ? std::filesystem::path(".") : p, ec).lexically_normal();
    if (!d.has_filename() && d.has_relative_path()) d = d.parent_path(); // "C:\x\" -> "C:\x" (one identity)
    return d;
}

// P40 review (#75): ONE normalization for an include rule's output template, shared by routing
// (routeIncludeOutput) and the owned scan (ownedScope), so the folder a rule writes to and the folder its
// owned scan deletes in can never disagree. `\` is a separator on every platform (a pgconfig.json is
// cross-platform; on Windows `<config> / "\gen"` used to mean `<drive>:\gen`), and a leading separator does
// not root the template: it stays config-relative (an empty %(Directory) leaves a leading '/').
inline std::string normalizeOutputTemplate(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    while (!s.empty() && s.front() == '/') s.erase(0, 1);
    return s;
}

// P40 B5 + review (#75): the scan folder of one `"owned": true` rule — the template's literal folder prefix
// (everything before the first `%(`, cut back to the last separator), resolved exactly as routing resolves
// it. Recursive when what follows can introduce directories (`%(RecursiveDir)`, `%(Directory)`, or any
// later separator, e.g. `gen/%(TargetLanguage)/%(Filename)`).
// An owned folder's stale marked files are DELETED, so the scope must be a dedicated folder: refused (false,
// `err` = the reason) when the template is rooted (a leading `/` or `\`) or absolute, when the literal prefix
// is empty (`%(Filename)`, `gen-%(TargetLanguage)/…`), or when it resolves to a filesystem root, the config
// folder itself (`./%(Filename)`) or one of its ancestors (`../%(Filename)`). `..` into a SIBLING subtree
// (`../../web/src/gen/%(Filename)`) stays allowed.
inline bool ownedScope(const std::filesystem::path& configDir, const std::string& tmpl, std::filesystem::path& dir,
                       bool& recursive, std::string& err) {
    namespace fs = std::filesystem;
    const std::size_t ph = tmpl.find("%(");
    std::string lit = tmpl.substr(0, ph);
    std::replace(lit.begin(), lit.end(), '\\', '/');
    std::string rest = ph == std::string::npos ? std::string() : tmpl.substr(ph);
    std::replace(rest.begin(), rest.end(), '\\', '/');
    recursive = rest.find("%(RecursiveDir)") != std::string::npos || rest.find("%(Directory)") != std::string::npos ||
                rest.find('/') != std::string::npos;
    dir.clear();
    if (!lit.empty() && lit.front() == '/') { err = "starts with a path separator"; return false; }
    if (fs::path(lit).has_root_name() || fs::path(lit).is_absolute()) { err = "is an absolute path"; return false; }
    const std::size_t slash = lit.rfind('/');
    const std::string prefix = slash == std::string::npos ? std::string() : normalizeOutputTemplate(lit.substr(0, slash));
    if (prefix.empty()) { err = "has no literal output folder before its first placeholder"; return false; }
    const fs::path base = absoluteDir(configDir);
    dir = absoluteDir(base / prefix);
    if (dir == dir.root_path()) { err = "resolves to a filesystem root"; return false; }
    const fs::path up = base.lexically_relative(dir); // "." = the config folder; no leading ".." = an ancestor
    if (dir == base || (!up.empty() && *up.begin() != "..")) {
        err = "resolves to the config folder or one of its ancestors ('" + dir.string() + "')";
        return false;
    }
    // P40 review (#75, pre-push): the lexical test above is case-sensitive and blind to aliases, so on a
    // case-insensitive filesystem `../PROJ/%(Filename)` from `t\Proj` (or `../../T/…`, an 8.3 short name, a
    // junction) named the config folder or an ancestor and passed. The config folder and its ancestors always
    // exist, so ask the filesystem: the scope must not BE any of them (fs::equivalent: same file identity).
    std::error_code ec;
    if (fs::exists(dir, ec)) {
        for (fs::path a = base;; a = a.parent_path()) {
            if (fs::equivalent(dir, a, ec) && !ec) {
                err = "resolves to the config folder or one of its ancestors ('" + dir.string() + "' is '" + a.string() + "')";
                return false;
            }
            if (!a.has_relative_path() || a.parent_path() == a) break;
        }
    }
    return true;
}

// P40 review (A5): the SHAPE of a pgconfig.json, checked before anything reads it. Every reader below is
// lenient by design (a wrong-kind value reads as empty), so without this a typo'd key (`"target"`), a string
// where a list belongs (`"targets": "csharp"`) or a non-object document was silently ignored — and a build
// with the config ignored exited 0. Each problem is one error naming the key; the caller refuses with 64.
inline void validatePgConfigShape(const json::Value& v, std::vector<std::string>& errors) {
    using K = json::Value::Kind;
    if (v.kind != K::Object) {
        errors.push_back("pgconfig.json: the document must be a JSON object");
        return;
    }
    // P40 review r3: blank means empty — `" "` is no name and no pattern (a blank pattern used to be accepted
    // and its output landed next to the .pg).
    auto blank = [](const std::string& s) {
        return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c) != 0; });
    };
    auto stringList = [&](const json::Value& x, const std::string& where) {
        if (x.kind != K::Array) { errors.push_back("pgconfig.json: " + where + " must be an array of strings"); return; }
        for (const auto& e : x.items())
            if (e.kind != K::String || blank(e.asString())) {
                errors.push_back("pgconfig.json: " + where + " must be an array of non-empty strings");
                return;
            }
    };
    static const char* const kKeys[] = {"$schema", "root", "lib", "targets", "targetOptions", "originInfo",
                                        "dependencies", "forbiddenIdentifiers", "include", "access"};
    for (const auto& [key, val] : v.members) {
        if (std::find_if(std::begin(kKeys), std::end(kKeys), [&](const char* k) { return key == k; }) == std::end(kKeys)) {
            errors.push_back("pgconfig.json: unknown key \"" + key + "\" (known keys: root, lib, targets, targetOptions, "
                             "originInfo, dependencies, forbiddenIdentifiers, include)");
            continue;
        }
        if (key == "root" && val.kind != K::String) errors.push_back("pgconfig.json: \"root\" must be a string");
        else if (key == "lib" || key == "targets") {
            stringList(val, "\"" + key + "\"");
            // P40 review r3: `"targets": []` read as ABSENT for `build` (exit 64) but as a valid config for
            // `--target` and `check` (exit 0). An empty set builds nothing; it is refused everywhere.
            if (key == "targets" && val.kind == K::Array && val.items().empty())
                errors.push_back("pgconfig.json: \"targets\" must name at least one target (e.g. [\"csharp\"]); "
                                 "omit the key to pass --target instead");
        }
        else if (key == "originInfo" && val.kind != K::Bool) errors.push_back("pgconfig.json: \"originInfo\" must be true or false");
        else if (key == "dependencies") {
            if (val.kind != K::Object) errors.push_back("pgconfig.json: \"dependencies\" must be an object of package -> version spec");
            else for (const auto& [pkg, spec] : val.members)
                if (spec.kind != K::String) errors.push_back("pgconfig.json: dependencies." + pkg + " must be a string version spec");
        } else if (key == "forbiddenIdentifiers") {
            if (val.kind == K::Array) stringList(val, "\"forbiddenIdentifiers\"");
            else if (val.kind == K::Object) { for (const auto& [t, names] : val.members) stringList(names, "forbiddenIdentifiers." + t); }
            else errors.push_back("pgconfig.json: \"forbiddenIdentifiers\" must be an array of names or an object of target -> names");
        } else if (key == "include") {
            if (val.kind != K::Array) { errors.push_back("pgconfig.json: \"include\" must be an array of rules"); continue; }
            for (const auto& rule : val.items()) {
                if (rule.kind != K::Object) { errors.push_back("pgconfig.json: every \"include\" rule must be an object"); continue; }
                for (const auto& [rk, rv] : rule.members) {
                    if (rk == "pattern" || rk == "output") {
                        if (rv.kind != K::String) errors.push_back("pgconfig.json: an include rule's \"" + rk + "\" must be a string");
                        else if (!rv.asString().empty() && blank(rv.asString()))
                            errors.push_back("pgconfig.json: an include rule's \"" + rk + "\" is blank");
                    } else if (rk == "target") {
                        if (rv.kind != K::String) stringList(rv, "an include rule's \"target\" (a name or a list of names)");
                    } else if (rk != "targetOptions" && rk != "owned") { // both checked where they are read
                        errors.push_back("pgconfig.json: an include rule has an unknown key \"" + rk +
                                         "\" (known keys: pattern, output, target, targetOptions, owned)");
                    }
                }
            }
        }
    }
}

inline PgConfig loadPgConfig(const std::filesystem::path& startDir) {
    for (std::filesystem::path d = absoluteDir(startDir);; d = d.parent_path()) {
        std::string src;
        if (readFile(d / "pgconfig.json", src)) {
            std::string jsonErr;
            json::Value v = json::parse(src, jsonErr);
            PgConfig pc;
            pc.found = true;
            pc.dir = d;
            if (!jsonErr.empty()) pc.errors.push_back((d / "pgconfig.json").string() + ": malformed JSON: " + jsonErr);
            else validatePgConfigShape(v, pc.errors); // P40 review (A5): a well-formed but invalid config refuses too
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
                // P40 B5: `"owned": true` — the rule's literal output folder belongs to Polyglot, so a stale
                // file there that carries Polyglot's own `Generated by Polyglot.` line is drift (--check) or
                // deleted (build). Anything else found there is an error and is never deleted (D5).
                const json::Value& ow = e["owned"];
                if (ow.kind == json::Value::Kind::Bool) rule.owned = ow.boolean;
                else if (!ow.isNull())
                    pc.errors.push_back("pgconfig.json: an include rule's \"owned\" must be true or false");
                std::filesystem::path scope;
                bool scopeRecursive = false;
                std::string scopeErr;
                if (rule.pattern.empty() || rule.output.empty())
                    pc.errors.push_back("pgconfig.json: an include rule needs both \"pattern\" and \"output\"");
                else if (rule.owned && !ownedScope(d, rule.output, scope, scopeRecursive, scopeErr))
                    pc.errors.push_back("pgconfig.json: include rule '" + rule.pattern + "': \"owned\": true is refused "
                                        "because its output '" + rule.output + "' " + scopeErr + ". Stale generated "
                                        "files in an owned folder are deleted, so it must be a dedicated subfolder "
                                        "(e.g. \"gen/%(Filename)\", or \"../web/src/gen/%(Filename)\" in a sibling tree)");
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
// `unverified` (offline `check`): targets whose plugin could not be loaded without the network — their options
// are skipped, not reported as naming an unknown target.
inline void validateTargetOptions(const PgConfig& pc, const TargetOptionsByTarget& flags,
                                  std::vector<std::string>& errors,
                                  const std::set<std::string>* unverified = nullptr) {
    auto check = [&](const TargetOptionsByTarget& layer, const std::string& where) {
        for (const auto& [target, opts] : layer) {
            const Backend* b = findBackend(target);
            if (!b && unverified && unverified->count(target)) continue;
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
        // The one normalization ownedScope shares: `\` is a separator, a leading one stays config-relative.
        return (pc.dir / normalizeOutputTemplate(expanded)).lexically_normal();
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
