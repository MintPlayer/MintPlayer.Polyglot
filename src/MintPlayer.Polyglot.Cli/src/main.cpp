#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#undef Yield // winbase.h macro; ir::StmtKind::Yield must survive

#include <fcntl.h>
#include <io.h>
#endif

#include "mintplayer/polyglot/backend.hpp"
#include "mintplayer/polyglot/capability.hpp"
#include "mintplayer/polyglot/json.hpp"
#include "mintplayer/polyglot/polyglot.hpp"

#include "exe_path.hpp"
#include "pgconfig.hpp"
#include "pluginresolve.hpp"
#include "watch.hpp"

using namespace mintplayer::polyglot;

namespace fs = std::filesystem;

// The workspace-config/cache glue lives in pgconfig.hpp (P30 slice 0) so tests can link it; the
// unqualified names below keep the call sites unchanged.
using cli::PgConfig;
using cli::loadPgConfig;
using cli::loadPluginFile;
using cli::pluginCacheDir;
using cli::readFile;
using cli::writeFile;

namespace {

void printUsage() {
    std::cout
        << "polyglot " << Compiler::version() << " - cross-SDK transpiler (P2 walking skeleton)\n"
        << "\n"
        << "Usage:\n"
        << "  polyglot --version\n"
        << "  polyglot build <input.pg> [--target <name>] [--out <dir>] [--root <dir>] [--lib <a,b>] [--watch]\n"
        << "  polyglot fmt <input.pg>\n"
        << "  polyglot check <input.pg> [--json] [--root <dir>] [--lib <a,b>] [--watch]\n"
        << "  polyglot lsp\n"
        << "  polyglot install <plugin-dir | npm-package>\n"
        << "\n"
        << "  build  Transpiles <input.pg> for every language in pgconfig.json `targets` (--target\n"
        << "         overrides; missing plugin dependencies download from the npm registry into the\n"
        << "         user cache, pinned by pgconfig.lock.json). pgconfig `include` rules route each\n"
        << "         emitted file (glob -> output template; the target extension is appended); with\n"
        << "         no input args, build discovers its inputs from those patterns.\n"
        << "         --out writes outputs to <dir> (default: alongside the input).\n"
        << "         --watch rebuilds whenever the input, an imported .pg, or pgconfig.json changes\n"
        << "         (a failed rebuild keeps watching and never touches the last good outputs).\n"
        << "  fmt    Re-prints <input.pg> as canonical Polyglot to stdout (the round-trip printer).\n"
        << "  check  Reports parse/type diagnostics without emitting. --json prints a machine-readable\n"
        << "         array (line/col/severity/message) for editor tooling.\n"
        << "  lsp    Runs the Language Server over stdio (JSON-RPC): diagnostics, go-to-definition,\n"
        << "         document symbols, hover. Spawned by the editor extensions; not for interactive use.\n";
}

// Comma-join a target-name list for "loaded targets: ..." diagnostics.
std::string joinNames(const std::vector<std::string>& names) {
    std::string out;
    for (const auto& n : names) { if (!out.empty()) out += ", "; out += n; }
    return out.empty() ? "none" : out;
}

// Split a comma-separated lib list ("io,math") into a LibConfig, trimming blank entries.
LibConfig parseLibList(const std::string& libArg) {
    LibConfig lib;
    for (std::size_t b = 0, e; b <= libArg.size(); b = e + 1) {
        e = libArg.find(',', b);
        if (e == std::string::npos) e = libArg.size();
        std::string name = libArg.substr(b, e - b);
        if (!name.empty()) lib.libs.push_back(name);
    }
    return lib;
}

// Resolve every pgconfig dependency (P19 slices 10-11, download half built at P30 slice 3):
// `file:` in place; in-box when the CLI's lockstep version satisfies the range; the verified
// versioned cache when the lockfile pins; else fetched from the npm registry inside the exe
// (packument -> maxSatisfying -> SRI verify -> extract -> validate -> cache -> lock). Shared by
// build, watch, and the LSP — the static ResolveState memoizes successes AND failures per config
// generation, so a long-lived host retries on pgconfig change instead of hammering the network.
void resolveConfiguredTargets(const PgConfig& pc) {
    static cli::ResolveState state;
    const cli::ResolveResult res =
        cli::resolvePluginDependencies(pc, cli::defaultHttpGet(), /*update=*/false, &state);
    for (const auto& m : res.messages) std::cerr << "polyglot: " << m << "\n";
    for (const auto& t : pc.targets)
        if (!findTarget(t).ok())
            std::cerr << "polyglot: target '" << t << "' is neither bundled nor provided by a pgconfig "
                      << "dependency; add one, e.g. \"dependencies\": { \"" << t << "\": \"latest\" }\n";
}

// Resolves a cross-`.pg` import to a file. A bare specifier ("a.b.c") is a logical module name resolved
// under the workspace root (a.b.c -> <root>/a/b/c.pg); a "./x"/"../x" specifier is resolved relative to
// the importing file. (std.* never reaches here — the Core serves it from its embedded registry first.)
class FileModuleResolver : public ModuleResolver {
public:
    FileModuleResolver(fs::path root, fs::path entryDir)
        : root_(std::move(root)), entryDir_(std::move(entryDir)) {}

    std::optional<ResolvedModule> resolve(const std::string& spec, const std::string& importer) override {
        fs::path file = candidate(spec, importer);
        std::string src;
        if (!readFile(file, src)) return std::nullopt;
        return ResolvedModule{fs::weakly_canonical(file).string(), std::move(src)};
    }

    // The file this resolver WOULD load for a specifier — exposed so watch mode can also poll the paths
    // of unresolved imports (creating the missing file then triggers the rebuild users expect).
    fs::path candidate(const std::string& spec, const std::string& importer) const {
        if (spec.rfind("./", 0) == 0 || spec.rfind("../", 0) == 0) {
            fs::path base = importer.empty() ? entryDir_ : fs::path(importer).parent_path();
            return (base / (spec + ".pg")).lexically_normal();
        }
        std::string rel = spec;
        for (char& c : rel) if (c == '.') c = '/';
        return (root_ / (rel + ".pg")).lexically_normal();
    }

private:
    fs::path root_;
    fs::path entryDir_;
};

void reportDiagnostics(const fs::path& input, const EmitResult& result) {
    for (const auto& d : result.diagnostics) {
        std::cerr << input.string() << ":" << d.pos.line << ":" << d.pos.col
                  << ": error: " << d.message << "\n";
    }
}

const char* severityName(Severity s) {
    switch (s) {
        case Severity::Warning: return "warning";
        case Severity::Info:    return "info";
        case Severity::Hint:    return "hint";
        default:                return "error";
    }
}

// Write one emitted file, deduping across a multi-root project build (§4.5): the same imported module can
// appear in several roots' closures — identical content is written once; a genuine content conflict (two
// distinct modules resolving to the same output path) is a hard error, never a silent clobber.
bool writeDedup(const fs::path& out, const std::string& content,
                std::map<std::string, std::string>* seen) {
    if (seen) {
        auto key = fs::weakly_canonical(out).string();
        auto it = seen->find(key);
        if (it != seen->end()) {
            if (it->second == content) return true; // already written, identical — skip
            std::cerr << "polyglot: conflicting output for '" << out.string()
                      << "' (two modules emit the same file with different content)\n";
            return false;
        }
        (*seen)[key] = content;
    }
    // Write-if-changed (P30 slice 7): an unchanged output keeps its mtime and never churns a file
    // watcher — routed twins may live inside a live dev server's src tree.
    std::string existing;
    if (readFile(out, existing) && existing == content) {
        std::cout << "  -> " << out.string() << "\n";
        return true;
    }
    if (!writeFile(out, content)) {
        std::cerr << "polyglot: cannot write '" << out.string() << "'\n";
        return false;
    }
    std::cout << "  -> " << out.string() << "\n";
    return true;
}

// Emit one target for one closure. Output paths route through the config's `include` rules
// (P30 slice 7, PRD D7) with `fallbackDir` (--out / the input's dir) for unmatched files;
// `flagRouted` (explicit --target + --out) bypasses the rules wholesale. `seen` (optional) dedups
// shared modules across a multi-root project build. Returns false on any failure.
bool emitOne(const std::string& source, const fs::path& input, const fs::path& fallbackDir,
             const BackendHandle& target, const char* ext, ModuleResolver* resolver, const LibConfig& lib,
             const PgConfig& pc, bool flagRouted,
             std::map<std::string, std::string>* seen = nullptr) {
    EmitResult result = compile(source, target, resolver, lib);
    if (!result.ok) {
        reportDiagnostics(input, result);
        return false;
    }
    std::vector<fs::path> outs;
    std::string rerr;
    if (!cli::resolveClosureOutputs(pc, flagRouted, input, result, target.backend()->name(), ext,
                                    fallbackDir, outs, rerr)) {
        std::cerr << "polyglot: " << rerr << "\n";
        return false;
    }
    if (!writeDedup(outs[0], result.code, seen)) return false;
    // §4.5 module linking: a multi-module program emits one file per imported user module alongside the entry.
    for (std::size_t i = 0; i < result.modules.size(); ++i)
        if (!writeDedup(outs[i + 1], result.modules[i].code, seen)) return false;
    return true;
}

// ============================ watch mode (`--watch`) — PRD §4.13 / PLAN §P21 ============================
// The frozen watch console protocol (golden-tested; the VS Code `$polyglot-watch` background problemMatcher
// anchors on these exact shapes — drift breaks tests/watch/run-watch.ps1 before it breaks the editor):
//   [HH:MM:SS] polyglot watch: building <abs entry>       (first cycle; later cycles say "rebuilding")
//   <ABSPATH>(<line>,<col>): error: <message>              (MSBuild-canonical; watch stream only)
//   [HH:MM:SS] polyglot watch: N error(s) - watching for changes
// Deliberately ASCII-only and un-localized (a Windows console codepage must not be able to mangle the
// matcher anchors), 24h clock, everything on stdout (one predictable stream for task runners).

// [HH:MM:SS] wall-clock stamp for the sentinel lines.
std::string clockStamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "[%02d:%02d:%02d]", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

#ifdef _WIN32
// The console-control handler runs on an OS-injected thread; it may only poke the watcher's atomic stop
// flag. Returning TRUE claims the event so the process exits via the loop (cleanly, after the current
// cycle) instead of being killed mid-write.
cli::FileWatcher* g_watchStopTarget = nullptr;
BOOL WINAPI watchCtrlHandler(DWORD type) {
    if (g_watchStopTarget &&
        (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT)) {
        g_watchStopTarget->stop();
        return TRUE;
    }
    return FALSE;
}
#endif

// One watch rebuild cycle: compile (+ write, unless checkOnly) every configured target, printing
// MSBuild-canonical diagnostics, and report the exact input closure to re-arm the watcher with.
// Identical frontend diagnostics repeat per target, so lines are deduped within the cycle (the count
// the end sentinel reports is unique error lines, not lines-times-targets).
struct WatchCycle {
    int errors = 0;
    std::vector<fs::path> watched;
};

WatchCycle watchBuildOnce(const fs::path& input, const fs::path& outDirArg, const fs::path& rootArg,
                          const std::string& targetArg, const std::string& libArgIn, bool checkOnly,
                          bool flagRouted) {
    WatchCycle c;
    const fs::path absInput = fs::absolute(input).lexically_normal();
    c.watched.push_back(absInput);

    std::set<std::string> printed; // per-cycle dedup of diagnostic lines
    auto emitDiag = [&](const std::string& line, Severity sev) {
        if (!printed.insert(line).second) return;
        std::cout << line << "\n";
        if (sev == Severity::Error) ++c.errors;
    };
    auto emitDiagAt = [&](const Diagnostic& d) {
        emitDiag(absInput.string() + "(" + std::to_string(d.pos.line) + "," + std::to_string(d.pos.col) +
                     "): " + severityName(d.severity) + ": " + d.message,
                 d.severity);
    };
    auto emitTopLevel = [&](const std::string& message) {
        emitDiag(absInput.string() + "(1,1): error: " + message, Severity::Error);
    };

    std::string source;
    if (!readFile(input, source)) {
        emitTopLevel("cannot open input file");
        return c;
    }

    // Absolute so the pgconfig walk-up actually walks for a relative input (a relative "." has no
    // parent to walk to) — the chain below and the closure paths must be pollable absolute paths anyway.
    const fs::path entryDir =
        fs::absolute(input.has_parent_path() ? input.parent_path() : fs::path(".")).lexically_normal();
    PgConfig pc = loadPgConfig(entryDir);
    // Watch every pgconfig.json CANDIDATE between the entry and the config that answered (or the
    // filesystem root when none did): editing the active config re-resolves the whole context next
    // cycle (targets/lib/root/forbiddenIdentifiers), and creating a NEARER one takes over.
    for (fs::path d = entryDir;; d = d.parent_path()) {
        c.watched.push_back(d / "pgconfig.json");
        if (pc.found && d == pc.dir) break;
        if (!d.has_parent_path() || d.parent_path() == d) break;
    }
    if (!pc.errors.empty()) { // a broken manifest refuses the cycle; the config is watched, so a fix recovers
        for (const auto& m : pc.errors) emitTopLevel(m);
        return c;
    }

    fs::path root = rootArg;
    std::string libArg = libArgIn;
    if (root.empty() && pc.found && !pc.root.empty()) root = pc.root;
    if (libArg.empty() && pc.found) libArg = pc.lib;
    if (root.empty()) root = entryDir;
    FileModuleResolver fileResolver(root, entryDir);
    cli::RecordingResolver resolver(fileResolver);

    LibConfig lib = parseLibList(libArg);
    lib.forbiddenIdentifiers = pc.forbiddenIdentifiers;

    resolveConfiguredTargets(pc); // safe per-cycle: already-registered names are skipped

    // The target set mirrors runBuild's resolution; `check --watch` uses the derived reference
    // target only. No config + no --target refuses per cycle (P30 slice 4) — adding a
    // pgconfig.json is watched, so the next save recovers.
    std::vector<std::string> targets;
    if (checkOnly) {
        if (const Backend* ref = cli::referenceBackend()) targets.push_back(ref->name());
        else { emitTopLevel("no full-coverage reference target is loaded (no plugins found?)"); return c; }
    } else if (!targetArg.empty()) {
        targets.push_back(targetArg);
    } else if (!pc.targets.empty()) {
        targets = pc.targets;
    } else {
        emitTopLevel("no --target given and no pgconfig.json declares `targets` (loaded targets: " +
                     joinNames(backendNames()) + ")");
        return c;
    }

    for (const auto& t : targets) {
        BackendHandle h = findTarget(t); // resolveConfiguredTargets above already ran the P30 pipeline
        if (!h.ok()) {
            emitTopLevel(h.error());
            continue;
        }
        EmitResult result = compile(source, h, &resolver, lib);
        if (!result.ok) {
            for (const auto& d : result.diagnostics) emitDiagAt(d);
            if (result.diagnostics.empty()) emitTopLevel("compilation failed for target '" + t + "'");
            continue; // last-good outputs stay in place
        }
        if (checkOnly) continue;
        const std::string ext = h.backend()->fileExtension();
        // P30 slice 7: outputs route through the config's include rules — same resolution as build.
        std::vector<fs::path> outs;
        std::string rerr;
        if (!cli::resolveClosureOutputs(pc, flagRouted, input, result, t, ext.c_str(), outDirArg, outs, rerr)) {
            emitTopLevel(rerr);
            continue;
        }
        // Write-if-changed: an untouched twin keeps its mtime (twins may live in a watched src tree).
        auto writeOut = [&](const fs::path& out, const std::string& content) {
            std::string existing;
            if (readFile(out, existing) && existing == content) { std::cout << "  -> " << out.string() << "\n"; return true; }
            if (!writeFile(out, content)) { emitTopLevel("cannot write '" + out.string() + "'"); return false; }
            std::cout << "  -> " << out.string() << "\n";
            return true;
        };
        bool wrote = writeOut(outs[0], result.code);
        // §4.5 module linking: also (re)write each imported user module's file, so editing an imported .pg
        // refreshes its own output — the entry file no longer inlines it.
        for (std::size_t i = 0; wrote && i < result.modules.size(); ++i)
            wrote = writeOut(outs[i + 1], result.modules[i].code);
    }

    for (const auto& p : resolver.loaded()) c.watched.push_back(p);
    // Unresolved imports: poll the file each one WOULD load, so creating it triggers the rebuild.
    for (const auto& [spec, importer] : resolver.unresolved())
        c.watched.push_back(fileResolver.candidate(spec, importer));
    return c;
}

int runWatch(const fs::path& input, const fs::path& outDir, const fs::path& root,
             const std::string& target, const std::string& libArg, bool checkOnly, bool flagRouted) {
    cli::PollingFileWatcher watcher;
#ifdef _WIN32
    g_watchStopTarget = &watcher;
    SetConsoleCtrlHandler(watchCtrlHandler, TRUE);
#endif
    const fs::path absInput = fs::absolute(input).lexically_normal();
    const char* verb = "building";
    for (;;) {
        std::cout << clockStamp() << " polyglot watch: " << verb << " " << absInput.string() << "\n";
        WatchCycle c = watchBuildOnce(input, outDir, root, target, libArg, checkOnly, flagRouted);
        std::cout << clockStamp() << " polyglot watch: " << c.errors
                  << " error(s) - watching for changes\n";
        std::cout.flush();
        verb = "rebuilding";

        watcher.watch(c.watched);
        cli::FileWatcher::Event e;
        do {
            e = watcher.waitNext(std::chrono::hours(1));
        } while (e == cli::FileWatcher::Event::TimedOut);
        if (e == cli::FileWatcher::Event::Stopped) break;
        // Debounce: a save burst (multi-file save, atomic-rename double edge) is one rebuild — drain
        // until 250 ms of quiet.
        do {
            e = watcher.waitNext(std::chrono::milliseconds(250));
        } while (e == cli::FileWatcher::Event::Changed);
        if (e == cli::FileWatcher::Event::Stopped) break;
    }
    std::cout << clockStamp() << " polyglot watch: stopped\n";
    return 0;
}

// P30 slice 7 discovery: a bare `polyglot build` builds every .pg the config's `include` patterns
// match (deterministic order). Dot-dirs and the usual dependency/output trees are pruned — a
// `**/*.pg` pattern should not wander into node_modules or bin/obj.
std::vector<fs::path> discoverIncludeInputs(const PgConfig& pc) {
    std::vector<fs::path> found;
    if (!pc.found || pc.include.empty()) return found;
    std::error_code ec;
    fs::recursive_directory_iterator it(pc.dir, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        const std::string name = p.filename().string();
        std::error_code tec;
        if (it->is_directory(tec)) {
            if (!name.empty() && (name[0] == '.' || name == "node_modules" || name == "bin" || name == "obj"))
                it.disable_recursion_pending();
            continue;
        }
        if (p.extension() != ".pg") continue;
        const std::string rel = fs::relative(p, pc.dir, tec).generic_string();
        for (const auto& rule : pc.include)
            if (cli::globMatch(rule.pattern, rel).matched) { found.push_back(p); break; }
    }
    std::sort(found.begin(), found.end());
    return found;
}

// Build one nearest-config group (P30 slice 7 / PRD D8): the whole per-config pipeline — flag/config
// merging, dependency resolution, target-set selection, and the single- or multi-file (§4.5) emit,
// with outputs routed through the group's `include` rules.
int buildGroup(PgConfig& pc, const std::vector<fs::path>& inputs, const std::string& target,
               const fs::path& outDir, bool outDirGiven, fs::path root, std::string libArg,
               const std::string& accessArg) {
    for (const auto& m : pc.errors) std::cerr << "polyglot: " << m << "\n";
    if (!pc.errors.empty()) return 64;

    const fs::path firstInput = inputs.front();
    const fs::path entryDir = firstInput.has_parent_path() ? firstInput.parent_path() : fs::path(".");
    // The pgconfig fills in root/lib the user didn't pass explicitly (flags win).
    if (root.empty() && pc.found && !pc.root.empty()) root = pc.root;
    if (libArg.empty() && pc.found) libArg = pc.lib;
    if (root.empty()) root = entryDir;

    LibConfig lib = parseLibList(libArg);
    lib.forbiddenIdentifiers = pc.forbiddenIdentifiers;
    lib.access = !accessArg.empty() ? accessArg : pc.access; // --access wins over pgconfig "access"
    if (!lib.access.empty() && lib.access != "public" && lib.access != "internal") {
        std::cerr << "polyglot: --access must be 'public' or 'internal' (got '" << lib.access << "')\n";
        return 64;
    }

    resolveConfiguredTargets(pc); // pgconfig `dependencies` + lock-first cache + registry (P30)

    // The target set to emit, each paired with its output extension.
    std::vector<std::pair<BackendHandle, std::string>> targets;
    if (target.empty() && !pc.targets.empty()) { // the project declares its target set — build all of it
        for (const auto& t : pc.targets) {
            BackendHandle h = findTarget(t);
            if (!h.ok()) { std::cerr << "polyglot: " << h.error() << "\n"; return 64; }
            targets.emplace_back(h, h.backend()->fileExtension());
        }
    } else if (target.empty()) { // no config + no --target: refuse — the plugin set is config-sourced (P30)
        std::cerr << "polyglot: no --target given and no pgconfig.json declares `targets` (loaded targets: "
                  << joinNames(backendNames())
                  << "); pass --target <name> or add \"targets\": [...] to pgconfig.json\n";
        return 64;
    } else { // ANY loaded plugin is a valid --target; its manifest names the output extension (P19)
        BackendHandle h = findTarget(target); // in-box, file:, or P30-resolved — never a bare-name probe
        if (!h.ok()) { std::cerr << "polyglot: " << h.error() << "\n"; return 64; }
        targets.emplace_back(h, h.backend()->fileExtension());
    }

    const bool flagRouted = !target.empty() && outDirGiven; // D7: explicit --target+--out bypasses rules

    bool ok = true;
    if (inputs.size() == 1) { // single input: emit its closure (entry + any imported user modules)
        std::string source;
        if (!readFile(firstInput, source)) { std::cerr << "polyglot: cannot open '" << firstInput.string() << "'\n"; return 66; }
        FileModuleResolver resolver(root, entryDir);
        for (const auto& t : targets)
            ok &= emitOne(source, firstInput, outDir, t.first, t.second.c_str(), &resolver, lib, pc, flagRouted);
        return ok ? 0 : 1;
    }

    // §4.5 multi-file project (e.g. the MSBuild NuGet globs every .pg): emit each linked module exactly once.
    // Read all inputs, canonicalize, and find the ROOTS — inputs not imported by another input. Compile only
    // roots (an imported-only library .pg is emitted within its importer's closure, never also as its own
    // entry), deduping modules shared across roots by output path.
    // §4.5 / issue #14: a multi-file build is a C# PROJECT — hoist the shared prelude into one
    // __polyglot_prelude.cs. Every root emits identical content there, so the writeDedup below collapses it
    // to one file. (No-op for TS/Python/PHP, whose per-file prelude inline is collision-free.)
    lib.sharedPrelude = true;

    struct In { fs::path path; std::string canon; std::string source; };
    std::vector<In> ins;
    for (const auto& p : inputs) {
        In e; e.path = p; e.canon = fs::weakly_canonical(p).string();
        if (!readFile(p, e.source)) { std::cerr << "polyglot: cannot open '" << p.string() << "'\n"; return 66; }
        ins.push_back(std::move(e));
    }
    std::set<std::string> importedCanons;
    for (const auto& e : ins) {
        fs::path edir = e.path.has_parent_path() ? e.path.parent_path() : fs::path(".");
        FileModuleResolver r(root, edir);
        for (const auto& spec : importSpecifiers(e.source))
            if (auto rm = r.resolve(spec, e.canon)) importedCanons.insert(rm->canonicalPath);
    }
    for (const auto& t : targets) {
        std::map<std::string, std::string> seen; // output path -> content, for cross-root dedup
        for (const auto& e : ins) {
            if (importedCanons.count(e.canon)) continue; // imported by another input — emitted in its closure
            fs::path edir = e.path.has_parent_path() ? e.path.parent_path() : fs::path(".");
            FileModuleResolver r(root, edir);
            ok &= emitOne(e.source, e.path, outDir, t.first, t.second.c_str(), &r, lib, pc, flagRouted, &seen);
        }
    }
    return ok ? 0 : 1;
}

int runBuild(const std::vector<std::string>& args) {
    std::vector<fs::path> inputs; // one or more .pg files (a multi-file project passes them all — §4.5)
    fs::path outDir;
    fs::path root;      // workspace root for logical-name imports; empty => input's parent dir
    std::string target; // empty => the pgconfig `targets` set
    std::string libArg; // comma-separated `lib` prelude entries (e.g. "io,math")
    std::string accessArg; // --access public|internal (C# emitted-type accessibility)
    bool watch = false;

    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--target" && i + 1 < args.size()) {
            target = args[++i];
        } else if (a == "--out" && i + 1 < args.size()) {
            outDir = args[++i];
        } else if (a == "--root" && i + 1 < args.size()) {
            root = args[++i];
        } else if (a == "--lib" && i + 1 < args.size()) {
            libArg = args[++i];
        } else if (a == "--access" && i + 1 < args.size()) {
            accessArg = args[++i];
        } else if (a == "--watch") {
            watch = true;
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "polyglot: unknown option '" << a << "'\n";
            return 64;
        } else {
            inputs.push_back(a);
        }
    }

    // P30 slice 7: a bare `polyglot build` discovers its inputs from the config's `include`
    // patterns (the tsc model, as a fallback — passed files always win as the input set).
    if (inputs.empty()) {
        const PgConfig pc0 = loadPgConfig(fs::current_path());
        for (const auto& m : pc0.errors) std::cerr << "polyglot: " << m << "\n";
        if (!pc0.errors.empty()) return 64;
        inputs = discoverIncludeInputs(pc0);
        if (inputs.empty()) {
            std::cerr << "polyglot: 'build' needs an input file (or a pgconfig.json whose `include` "
                         "patterns match some .pg sources)\n";
            return 64;
        }
    }
    const fs::path firstInput = inputs.front();
    const bool outDirGiven = !outDir.empty(); // D7: explicit --target + --out bypasses include rules
    if (outDir.empty()) outDir = firstInput.has_parent_path() ? firstInput.parent_path() : fs::path(".");

    if (watch) {
        if (inputs.size() > 1) { std::cerr << "polyglot: --watch takes a single input file\n"; return 64; }
        return runWatch(firstInput, outDir, root, target, libArg, /*checkOnly=*/false,
                        !target.empty() && outDirGiven);
    }

    std::cout << "polyglot build";
    for (const auto& in : inputs) std::cout << " " << in.string();
    std::cout << "\n";

    // Group inputs by their NEAREST pgconfig.json (P30 slice 7, PRD D8): nested configs must not
    // silently inherit the wrong targets/lib/routing. One root config (the recommended shape)
    // degenerates to a single group.
    std::map<std::string, std::pair<PgConfig, std::vector<fs::path>>> groups; // key: pc.dir ("" = none)
    for (const auto& in : inputs) {
        const fs::path inDir = in.has_parent_path() ? in.parent_path() : fs::path(".");
        PgConfig pcIn = loadPgConfig(inDir);
        const std::string key = pcIn.found ? pcIn.dir.string() : std::string();
        auto& g = groups[key];
        if (g.second.empty()) g.first = std::move(pcIn);
        g.second.push_back(in);
    }

    int worst = 0;
    for (auto& [key, group] : groups) {
        const int rc = buildGroup(group.first, group.second, target, outDir, outDirGiven, root, libArg,
                                  accessArg);
        if (rc > worst) worst = rc;
    }
    return worst;
}

// Escape a string for embedding in a JSON double-quoted value.
std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[(c >> 4) & 0xF];
                    out += hex[c & 0xF];
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Serialize diagnostics as a JSON array of {line,col,endLine,endCol,severity,message}. Shared by
// `check --json` and the LSP `polyglot/emit` preview response (whole-file list, no identifier-widening —
// that widening is a squiggle-only concern of publishDiagnostics).
std::string diagnosticsToJson(const std::vector<Diagnostic>& diags) {
    std::string out = "[";
    for (std::size_t i = 0; i < diags.size(); ++i) {
        const auto& d = diags[i];
        if (i) out += ",";
        out += "{\"line\":" + std::to_string(d.pos.line) + ",\"col\":" + std::to_string(d.pos.col) +
               ",\"endLine\":" + std::to_string(d.end.line) + ",\"endCol\":" + std::to_string(d.end.col) +
               ",\"severity\":\"" + severityName(d.severity) + "\",\"message\":\"" + jsonEscape(d.message) + "\"}";
    }
    out += "]";
    return out;
}

// `polyglot check <input.pg> [--json] [--root <dir>] [--lib <a,b>]` — runs the frontend (lex/parse/sema +
// capability gating for the reference target) and reports diagnostics, without emitting any output. This is
// the editor-tooling entry point: `--json` prints a machine-readable array editors turn into squiggles.
// (The reference target is C#, whose capability set is the full §3.A surface, so nothing gates spuriously.)
int runCheck(const std::vector<std::string>& args) {
    fs::path input;
    fs::path root;
    std::string libArg;
    bool json = false;
    bool watch = false;

    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--json") json = true;
        else if (a == "--watch") watch = true;
        else if (a == "--root" && i + 1 < args.size()) root = args[++i];
        else if (a == "--lib" && i + 1 < args.size()) libArg = args[++i];
        else if (!a.empty() && a[0] == '-') { std::cerr << "polyglot: unknown option '" << a << "'\n"; return 64; }
        else if (input.empty()) input = a;
        else { std::cerr << "polyglot: unexpected argument '" << a << "'\n"; return 64; }
    }
    if (input.empty()) { std::cerr << "polyglot: 'check' needs an input file\n"; return 64; }
    if (watch && json) { std::cerr << "polyglot: --json and --watch cannot be combined\n"; return 64; }
    if (watch) return runWatch(input, /*outDir=*/{}, root, /*target=*/{}, libArg, /*checkOnly=*/true,
                               /*flagRouted=*/false);

    std::string source;
    if (!readFile(input, source)) {
        std::cerr << "polyglot: cannot open '" << input.string() << "'\n";
        return 66;
    }

    fs::path entryDir = input.has_parent_path() ? input.parent_path() : fs::path(".");
    PgConfig pc = loadPgConfig(entryDir); // fills root/lib the user didn't pass explicitly
    if (root.empty() && pc.found && !pc.root.empty()) root = pc.root;
    if (libArg.empty() && pc.found) libArg = pc.lib;
    if (root.empty()) root = entryDir;
    FileModuleResolver resolver(root, entryDir);

    LibConfig lib = parseLibList(libArg);
    lib.forbiddenIdentifiers = pc.forbiddenIdentifiers;

    // The reference target carries the full §3.A surface so a pure frontend gate never refuses
    // spuriously — derived from capabilities (P30 slice 4), not the name "csharp".
    const Backend* ref = cli::referenceBackend();
    if (!ref) {
        std::cerr << "polyglot: no full-coverage reference target is loaded (no plugins found?)\n";
        return 69;
    }
    EmitResult result = compile(source, findTarget(ref->name()), &resolver, lib);

    if (json) {
        std::cout << diagnosticsToJson(result.diagnostics) << "\n";
    } else {
        reportDiagnostics(input, result);
        if (result.ok) std::cout << "polyglot: no problems in " << input.string() << "\n";
    }
    return result.ok ? 0 : 1;
}

int runFmt(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::cerr << "polyglot: 'fmt' needs an input file\n";
        return 64;
    }
    fs::path input = args[1];
    std::string source;
    if (!readFile(input, source)) {
        std::cerr << "polyglot: cannot open '" << input.string() << "'\n";
        return 66;
    }
    EmitResult result = format(source);
    if (!result.ok) {
        reportDiagnostics(input, result);
        return 1;
    }
    std::cout << result.code;
    return 0;
}

// ============================ LSP server (`polyglot lsp`) — PRD §4.8 ============================
// A zero-dependency Language Server over the frontend `analyze()` facade. Transport is Content-Length-framed
// JSON-RPC 2.0 on stdio; the JSON reader lives in Core. We negotiate the `utf-8` position encoding so LSP
// columns are byte offsets that map to our 1-based line/col with just a ±1 shift (no UTF-16 walk yet).

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// `file:///c%3A/x/y.pg` -> a native path. Percent-decodes and drops the leading slash before a Windows drive.
std::string uriToPath(const std::string& uri) {
    std::string u = uri;
    if (u.rfind("file://", 0) == 0) u = u.substr(7);
    std::string out;
    for (std::size_t k = 0; k < u.size(); ++k) {
        if (u[k] == '%' && k + 2 < u.size()) {
            int hi = hexVal(u[k + 1]), lo = hexVal(u[k + 2]);
            if (hi >= 0 && lo >= 0) { out += static_cast<char>(hi * 16 + lo); k += 2; continue; }
        }
        out += u[k];
    }
#ifdef _WIN32
    if (out.size() >= 3 && out[0] == '/' && out[2] == ':') out.erase(0, 1);
#endif
    return out;
}

// A native path -> a file:// URI (inverse of uriToPath), for cross-module go-to-definition Locations.
std::string pathToUri(const std::string& p) {
    std::string s = p;
    for (char& c : s) if (c == '\\') c = '/';
    if (s.size() >= 2 && s[1] == ':') return "file:///" + s; // Windows drive: file:///c:/...
    if (!s.empty() && s[0] == '/') return "file://" + s;     // POSIX absolute
    return "file:///" + s;
}

std::size_t byteOffset(const std::string& text, int line, int col) {
    std::size_t off = 0;
    for (int ln = 1; ln < line && off < text.size(); ++off) if (text[off] == '\n') ++ln;
    off += static_cast<std::size_t>(col - 1);
    return off < text.size() ? off : text.size();
}
bool identPart(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// UTF-8 lead-byte -> its sequence length (1..4); a stray continuation/invalid byte counts as 1.
int utf8Seq(unsigned char c) {
    return c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
}
// UTF-16 code units spanned by the first `nbytes` bytes of UTF-8 string `s` (a 4-byte/astral char = 2 units).
int utf16Units(const std::string& s, std::size_t nbytes) {
    int u = 0;
    for (std::size_t i = 0; i < nbytes && i < s.size();) {
        int seq = utf8Seq(static_cast<unsigned char>(s[i]));
        u += seq == 4 ? 2 : 1;
        i += seq;
    }
    return u;
}
// The line's text (1-based line, newline excluded) — for per-line UTF-16<->byte column conversion.
std::string lineOf(const std::string& text, int line) {
    std::size_t start = byteOffset(text, line, 1), end = start;
    while (end < text.size() && text[end] != '\n') ++end;
    return text.substr(start, end - start);
}
// 1-based byte column -> 1-based UTF-16 column on `lineText`.
int protoColFromByte(const std::string& lineText, int byteCol) { return 1 + utf16Units(lineText, static_cast<std::size_t>(byteCol - 1)); }
// 1-based UTF-16 column -> 1-based byte column on `lineText`.
int byteColFromUtf16(const std::string& lineText, int protoCol) {
    int target = protoCol - 1, u = 0;
    std::size_t i = 0;
    while (i < lineText.size() && u < target) { int seq = utf8Seq(static_cast<unsigned char>(lineText[i])); u += seq == 4 ? 2 : 1; i += seq; }
    return static_cast<int>(i) + 1;
}

// Map our SymbolKind -> the LSP SymbolKind enum (numeric).
int lspSymbolKind(SymbolKind k) {
    switch (k) {
        case SymbolKind::Function:  return 12; // Function
        case SymbolKind::Type:      return 5;  // Class
        case SymbolKind::Method:    return 6;  // Method
        case SymbolKind::Field:     return 8;  // Field
        case SymbolKind::Value:     return 14; // Constant
        case SymbolKind::UnionCase:
        case SymbolKind::EnumCase:  return 22; // EnumMember
        default:                    return 13; // Variable
    }
}

// The semantic-token legend index for a symbol (must match the tokenTypes array declared in `initialize`).
int semanticTokenType(SymbolKind k) {
    switch (k) {
        case SymbolKind::Function:  return 0; // function
        case SymbolKind::Type:      return 1; // type
        case SymbolKind::Method:    return 2; // method
        case SymbolKind::Field:     return 3; // property
        case SymbolKind::Value:     return 4; // variable
        case SymbolKind::Parameter: return 5; // parameter
        case SymbolKind::UnionCase:
        case SymbolKind::EnumCase:  return 6; // enumMember
        default:                    return 4; // Local -> variable
    }
}

// The LSP CompletionItemKind for a symbol offered as a bare (non-member) name. Returns 0 for kinds that are
// only reachable via member access (methods/fields) — the caller skips those.
int completionKind(SymbolKind k) {
    switch (k) {
        case SymbolKind::Function:  return 3;  // Function
        case SymbolKind::Type:      return 7;  // Class
        case SymbolKind::Value:     return 21; // Constant
        case SymbolKind::Parameter:
        case SymbolKind::Local:     return 6;  // Variable
        case SymbolKind::UnionCase:
        case SymbolKind::EnumCase:  return 20; // EnumMember
        default:                    return 0;  // Method/Field — member-access only
    }
}

// Read one Content-Length-framed message body from stdin. False on EOF.
bool lspRead(std::string& body) {
    std::size_t len = 0;
    std::string line;
    int c;
    for (;;) {
        line.clear();
        while ((c = std::getchar()) != EOF && c != '\n') if (c != '\r') line += static_cast<char>(c);
        if (c == EOF && line.empty()) return false;
        if (line.empty()) break; // blank line ends the headers
        static const char* kCL = "Content-Length:";
        if (line.rfind(kCL, 0) == 0) len = std::strtoul(line.c_str() + std::strlen(kCL), nullptr, 10);
    }
    body.assign(len, '\0');
    std::size_t got = 0;
    while (got < len) { std::size_t n = std::fread(&body[got], 1, len - got, stdin); if (!n) break; got += n; }
    body.resize(got);
    return true;
}
void lspSend(const std::string& body) {
    std::string h = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    std::fwrite(h.data(), 1, h.size(), stdout);
    std::fwrite(body.data(), 1, body.size(), stdout);
    std::fflush(stdout);
}

// A ModuleResolver that serves an OPEN editor buffer's (possibly unsaved) text for any imported module the
// editor currently has open, delegating to the on-disk resolver otherwise. So editing an imported .pg updates
// its dependents live, before the import is saved. Matching is by real path (`fs::equivalent`, robust to
// uri-encoding/drive-case), and only the source text is swapped — the `canonicalPath` (dedup/cycle identity)
// stays the disk path, so transitive loading is unaffected.
class BufferResolver : public ModuleResolver {
public:
    BufferResolver(ModuleResolver& disk, const std::map<std::string, std::string>& buffers)
        : disk_(disk), buffers_(buffers) {}
    std::optional<ResolvedModule> resolve(const std::string& spec, const std::string& importer) override {
        auto r = disk_.resolve(spec, importer);
        if (r) {
            for (const auto& kv : buffers_) {
                std::error_code ec;
                if (fs::equivalent(uriToPath(kv.first), r->canonicalPath, ec) && !ec) { r->source = kv.second; break; }
            }
        }
        return r;
    }
private:
    ModuleResolver& disk_;
    const std::map<std::string, std::string>& buffers_;
};

struct LspServer {
    std::map<std::string, std::string> text_;    // uri -> current source (Full-sync buffer)
    std::map<std::string, SemanticModel> model_; // uri -> latest analyzed model
    std::map<std::string, SourceMap> sources_;   // uri -> its fileId->origin map (for cross-module locations)
    std::string root_;
    std::string lib_ = "io,math";
    bool utf16_ = false;  // negotiated position encoding: false = utf-8 (byte columns, no conversion) / true = utf-16.

    // A range from 1-based (line,col) positions, emitted as 0-based LSP positions (byte columns).
    static std::string rangeJson(int sl, int sc, int el, int ec) {
        return "{\"start\":{\"line\":" + std::to_string(sl - 1) + ",\"character\":" + std::to_string(sc - 1) +
               "},\"end\":{\"line\":" + std::to_string(el - 1) + ",\"character\":" + std::to_string(ec - 1) + "}}";
    }

    // Position-encoding conversion (only bites when utf-16 was negotiated; utf-8 = identity, so the VS Code
    // path is byte-for-byte unchanged). Internal columns are 1-based byte offsets; the client speaks utf-16
    // code units. Conversion needs the line's text, looked up in `text_` by uri — for a cross-file range in a
    // doc we don't have open, we fall back to byte columns (correct for ASCII; the documented residual).

    // Client position -> internal 1-based byte column. `protoChar0` is the 0-based character from the request.
    int inCol(const std::string& uri, int line, int protoChar0) {
        if (!utf16_) return protoChar0 + 1;
        auto it = text_.find(uri);
        if (it == text_.end()) return protoChar0 + 1;
        return byteColFromUtf16(lineOf(it->second, line), protoChar0 + 1);
    }
    // Emit a range in `docUri`, converting byte columns to utf-16 when negotiated.
    std::string encRange(const std::string& docUri, int sl, int sc, int el, int ec) {
        if (!utf16_) return rangeJson(sl, sc, el, ec);
        auto it = text_.find(docUri);
        if (it == text_.end()) return rangeJson(sl, sc, el, ec);
        const std::string& t = it->second;
        return rangeJson(sl, protoColFromByte(lineOf(t, sl), sc), el, protoColFromByte(lineOf(t, el), ec));
    }

    // Module-resolution context for an open doc, derived from its nearest pgconfig.json (falling back to the
    // client's initializationOptions). Shared by analyzeDoc (diagnostics) and generatedSource (preview) so a
    // live preview resolves imports + lib exactly as the squiggles do. Re-read each call, so editing
    // pgconfig.json takes effect on the next analysis.
    struct DocContext {
        fs::path root, entryDir;
        std::string libStr;
        std::vector<std::string> targets; // the project's target set (reserved-name squiggles run per target)
        std::vector<std::pair<std::string, std::string>> forbidden; // pgconfig forbiddenIdentifiers
    };
    DocContext contextFor(const std::string& uri) const {
        fs::path p(uriToPath(uri));
        fs::path entryDir = p.has_parent_path() ? p.parent_path() : fs::path(".");
        PgConfig pc = loadPgConfig(entryDir);
        fs::path root = pc.found && !pc.root.empty() ? fs::path(pc.root)
                      : (root_.empty() ? entryDir : fs::path(root_));
        resolveConfiguredTargets(pc); // plugin targets (file: deps / cache / registry) resolve for squiggles too
        // No config: reserved-name squiggles run against EVERY loaded target (a superset squiggle
        // is safe; an invented default pair is not — P30 slice 4).
        std::vector<std::string> targets = pc.targets.empty() ? backendNames() : pc.targets;
        return { root, entryDir, pc.found ? pc.lib : lib_, std::move(targets), pc.forbiddenIdentifiers };
    }

    void analyzeDoc(const std::string& uri) {
        DocContext ctx = contextFor(uri);
        FileModuleResolver disk(ctx.root, ctx.entryDir);
        BufferResolver resolver(disk, text_); // see unsaved edits in open imported modules
        AnalysisResult a = analyze(text_[uri], &resolver, parseLibList(ctx.libStr), uriToPath(uri));
        // Reserved/forbidden identifiers are per-target, which analyze() has no notion of — run the check
        // here for each configured target so a name a build would refuse squiggles live (P19 slice 14).
        DiagnosticBag reserved;
        for (const auto& t : ctx.targets) {
            BackendHandle h = findTarget(t);
            if (h.ok()) checkReservedNames(a.unit, *h.backend(), ctx.forbidden, reserved);
        }
        for (const auto& d : reserved.items()) a.diagnostics.push_back(d);
        model_[uri] = std::move(a.model);
        sources_[uri] = std::move(a.sources);
        // Only real files get squiggles. A `polyglot:<std>` virtual doc is read-only embedded std source we
        // analyze solely to power its semantic tokens / hover / go-to-def — analyzing it standalone may raise
        // link-context diagnostics that would be noise on code the user can't edit, so we don't publish them.
        if (uri.rfind("file:", 0) == 0) publishDiagnostics(uri, a.diagnostics);
    }

    // Analyze arbitrary text as if it were `uri` (same resolver/lib context), returning just the model —
    // used by member completion, which analyzes a repaired buffer (the trailing `.member` dropped so it
    // parses) to resolve the receiver's type. Does not store/publish anything.
    SemanticModel analyzeText(const std::string& uri, const std::string& text) {
        DocContext ctx = contextFor(uri);
        FileModuleResolver disk(ctx.root, ctx.entryDir);
        BufferResolver resolver(disk, text_);
        return std::move(analyze(text, &resolver, parseLibList(ctx.libStr), uriToPath(uri)).model);
    }

    // Custom request `polyglot/emit`: params { uri, target } -> { target, code, ok, diagnostics }. Runs the
    // full compile() in memory (no disk write) for a live preview of the emitted target source. One target per
    // request; the client debounces on edit and re-requests on target switch. compile() never returns partial
    // output — on failure it's { ok:false, code:"" } and the client keeps its last-good text with a stale
    // banner (never a miscompile shown as valid).
    std::string generatedSource(const json::Value& params) {
        std::string uri = params["uri"].asString();
        std::string targetName = params["target"].asString();
        auto empty = [&]() {
            return "{\"target\":" + json::quote(targetName) + ",\"code\":\"\",\"ok\":false,\"diagnostics\":[]}";
        };
        if (!text_.count(uri)) return empty();          // doc not open — never insert an empty text_[uri]
        BackendHandle tgt = findTarget(targetName);
        if (!tgt.ok()) return empty();
        DocContext ctx = contextFor(uri);
        FileModuleResolver disk(ctx.root, ctx.entryDir);
        BufferResolver resolver(disk, text_); // preview reflects unsaved edits in open imported modules
        EmitResult r = compile(text_[uri], tgt, &resolver, parseLibList(ctx.libStr));
        return "{\"target\":" + json::quote(targetName) + ",\"code\":" + json::quote(r.code) +
               ",\"ok\":" + (r.ok ? "true" : "false") +
               ",\"diagnostics\":" + diagnosticsToJson(r.diagnostics) + "}";
    }

    void publishDiagnostics(const std::string& uri, const std::vector<Diagnostic>& diags) {
        const std::string& text = text_.count(uri) ? text_[uri] : uri; // uri unused when text absent
        std::string arr;
        bool first = true;
        for (const auto& d : diags) {
            if (d.pos.fileId != 1) continue; // only this file's own diagnostics (fileId 1 = the entry); an
                                             // imported module's errors belong to (and show in) that module.
            int sl = d.pos.line, sc = d.pos.col, el = d.end.line, ec = d.end.col;
            if (el == sl && ec == sc) { // widen a point to the identifier at that spot (else a 1-char range)
                std::size_t off = byteOffset(text, sl, sc), end = off;
                while (end < text.size() && identPart(text[end])) ++end;
                ec = sc + static_cast<int>(end - off);
                if (ec == sc) ec = sc + 1;
            }
            int sev = d.severity == Severity::Warning ? 2 : d.severity == Severity::Info ? 3
                    : d.severity == Severity::Hint ? 4 : 1;
            if (!first) arr += ",";
            first = false;
            arr += "{\"range\":" + encRange(uri, sl, sc, el, ec) + ",\"severity\":" + std::to_string(sev) +
                   ",\"source\":\"polyglot\",\"message\":" + json::quote(d.message) + "}";
        }
        lspSend("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":" +
                json::quote(uri) + ",\"diagnostics\":[" + arr + "]}}");
    }

    const SemanticModel* modelFor(const json::Value& params, int& line, int& col, std::string& uri) {
        uri = params["textDocument"]["uri"].asString();
        line = static_cast<int>(params["position"]["line"].asInt()) + 1;
        col = inCol(uri, line, static_cast<int>(params["position"]["character"].asInt()));
        auto it = model_.find(uri);
        return it == model_.end() ? nullptr : &it->second;
    }

    // The URI a definition's fileId lives in: fileId 1 = the entry (the request doc); a higher id is another
    // resolved module, mapped through the SourceMap to a file:// URI. Returns "" for embedded std / unknown
    // (no on-disk file) — those aren't navigable as files (std click-through is served virtually elsewhere).
    std::string uriForFileId(const std::string& requestUri, int fid) {
        if (fid == 1) return requestUri;
        auto sit = sources_.find(requestUri);
        if (sit == sources_.end()) return "";
        const std::string& canon = sit->second.canon(fid);
        if (canon.empty()) return "";
        if (fs::path(canon).is_absolute()) return pathToUri(canon);            // a resolver-loaded .pg file
        if (canon.rfind("std.", 0) == 0) return "polyglot:" + canon;           // embedded std -> virtual document
        return "";
    }

    std::string definition(const json::Value& params) {
        int line, col; std::string uri;
        const SemanticModel* m = modelFor(params, line, col, uri);
        if (!m) return "null";
        const SymbolDef* d = m->definitionAt(line, col);
        if (!d) return "null";
        std::string targetUri = uriForFileId(uri, d->nameSpan.start.fileId);
        if (targetUri.empty()) return "null";
        int sl = d->nameSpan.start.line, sc = d->nameSpan.start.col;
        return "{\"uri\":" + json::quote(targetUri) + ",\"range\":" + encRange(targetUri, sl, sc, sl, sc + d->nameSpan.length) + "}";
    }

    std::string references(const json::Value& params) {
        int line, col; std::string uri;
        const SemanticModel* m = modelFor(params, line, col, uri);
        if (!m) return "[]";
        int d = m->symbolAt(line, col);
        if (d < 0) return "[]";
        bool incDecl = params["context"]["includeDeclaration"].asBool(true);
        std::string arr;
        bool first = true;
        auto emit = [&](const std::string& u, int sl, int sc, int len) {
            if (u.empty()) return;
            if (!first) arr += ",";
            first = false;
            arr += "{\"uri\":" + json::quote(u) + ",\"range\":" + encRange(u, sl, sc, sl, sc + len) + "}";
        };
        for (const Span& s : m->referencesTo(d)) emit(uri, s.start.line, s.start.col, s.length); // uses (this file)
        if (incDecl) { // the declaration (its own file)
            const SymbolDef& def = m->defs[static_cast<std::size_t>(d)];
            emit(uriForFileId(uri, def.nameSpan.start.fileId), def.nameSpan.start.line, def.nameSpan.start.col,
                 def.nameSpan.length);
        }
        return "[" + arr + "]";
    }

    std::string rename(const json::Value& params) {
        int line, col; std::string uri;
        const SemanticModel* m = modelFor(params, line, col, uri);
        if (!m) return "null";
        int d = m->symbolAt(line, col);
        if (d < 0) return "null";
        const SymbolDef& def = m->defs[static_cast<std::size_t>(d)];
        // Only file-local symbols: the declaration and all recorded uses are in this file, so one atomic
        // single-file edit. A symbol defined in another module would need edits across files we don't index.
        if (def.external || def.nameSpan.start.fileId != 1) return "null";
        std::string newName = params["newName"].asString();
        std::string edits;
        bool first = true;
        auto edit = [&](int sl, int sc, int len) {
            if (!first) edits += ",";
            first = false;
            edits += "{\"range\":" + encRange(uri, sl, sc, sl, sc + len) + ",\"newText\":" + json::quote(newName) + "}";
        };
        edit(def.nameSpan.start.line, def.nameSpan.start.col, def.nameSpan.length);
        for (const Span& s : m->referencesTo(d)) edit(s.start.line, s.start.col, s.length);
        return "{\"changes\":{" + json::quote(uri) + ":[" + edits + "]}}";
    }

    std::string hover(const json::Value& params) {
        int line, col; std::string uri;
        const SemanticModel* m = modelFor(params, line, col, uri);
        if (!m) return "null";
        const SymbolDef* d = m->definitionAt(line, col);
        if (!d) return "null";
        static const char* kindNames[] = {"fn", "type", "value", "param", "let", "field", "method", "case", "case"};
        std::string sig = std::string(kindNames[static_cast<int>(d->kind)]) + " " + d->name;
        if (!d->type.name.empty()) sig += ": " + d->type.name;
        std::string md = "```polyglot\n" + sig + "\n```";
        return "{\"contents\":{\"kind\":\"markdown\",\"value\":" + json::quote(md) + "}}";
    }

    // Accurate identifier coloring: emit an LSP semantic token for every definition and reference the model
    // knows, classified by symbol kind. Delta-encoded (5 ints/token, document order) with `declaration` set on
    // def sites. Layers on top of the TextMate grammar (which still colors keywords/strings/numbers/operators).
    std::string semanticTokens(const json::Value& params) {
        std::string uri = params["textDocument"]["uri"].asString();
        auto it = model_.find(uri);
        if (it == model_.end()) return "{\"data\":[]}";
        const SemanticModel& m = it->second;
        auto tt = text_.find(uri);
        const std::string* textp = tt == text_.end() ? nullptr : &tt->second; // for utf-16 column conversion
        struct Tok { int line, col, len, type, mod; };
        std::vector<Tok> toks;
        for (const auto& d : m.defs)
            if (!d.external && d.nameSpan.length > 0)
                toks.push_back({d.nameSpan.start.line, d.nameSpan.start.col, d.nameSpan.length, semanticTokenType(d.kind), 1});
        for (const auto& r : m.refs)
            if (r.def >= 0 && r.span.length > 0)
                toks.push_back({r.span.start.line, r.span.start.col, r.span.length,
                                semanticTokenType(m.defs[static_cast<std::size_t>(r.def)].kind), 0});
        // stable_sort keeps insertion order (defs before body refs) so a position emitted for two overlapping
        // symbols — e.g. a record field that's also an in-scope local in a method body — keeps the first.
        std::stable_sort(toks.begin(), toks.end(),
                         [](const Tok& a, const Tok& b) { return a.line != b.line ? a.line < b.line : a.col < b.col; });
        std::string data;
        int prevLine = 0, prevChar = 0, lastLine = -1, lastCol = -1;
        bool first = true;
        for (const auto& t : toks) {
            if (t.line == lastLine && t.col == lastCol) continue; // LSP requires non-overlapping tokens
            lastLine = t.line;
            lastCol = t.col;
            int col1 = t.col, len1 = t.len;
            if (utf16_ && textp) { // byte column/length -> utf-16 code units on this line
                std::string ln = lineOf(*textp, t.line);
                col1 = protoColFromByte(ln, t.col);
                len1 = protoColFromByte(ln, t.col + t.len) - col1;
            }
            int line0 = t.line - 1, char0 = col1 - 1;
            int dLine = line0 - prevLine;
            int dChar = dLine == 0 ? char0 - prevChar : char0;
            if (!first) data += ",";
            first = false;
            data += std::to_string(dLine) + "," + std::to_string(dChar) + "," + std::to_string(len1) + "," +
                    std::to_string(t.type) + "," + std::to_string(t.mod);
            prevLine = line0;
            prevChar = char0;
        }
        return "{\"data\":[" + data + "]}";
    }

    // Completion. In a member context (`obj.` / `obj.pre`) it lists the receiver type's members; otherwise it
    // offers keywords + every bare symbol the model knows (file-local + imported). Context-insensitive within
    // each mode — the client filters by the typed prefix.
    std::string completion(const json::Value& params) {
        std::string items;
        bool first = true;
        std::set<std::string> seen;
        auto add = [&](const std::string& label, int kind) {
            if (kind == 0 || !seen.insert(label + "\x1f" + std::to_string(kind)).second) return;
            if (!first) items += ",";
            first = false;
            items += "{\"label\":" + json::quote(label) + ",\"kind\":" + std::to_string(kind) + "}";
        };

        std::string uri = params["textDocument"]["uri"].asString();
        int line = static_cast<int>(params["position"]["line"].asInt()) + 1;
        int col = inCol(uri, line, static_cast<int>(params["position"]["character"].asInt()));

        // Member context: an identifier prefix immediately preceded by '.' — resolve the receiver's type and
        // list its members. Analyze a REPAIRED buffer (the `.member` under the cursor removed) so the receiver
        // parses even mid-edit; look the receiver up in that model; then emit defs owned by its type.
        auto tit = text_.find(uri);
        if (tit != text_.end()) {
            const std::string& text = tit->second;
            std::size_t off = byteOffset(text, line, col);
            std::size_t ps = off;
            while (ps > 0 && identPart(text[ps - 1])) --ps;           // start of the partial member
            if (ps > 0 && text[ps - 1] == '.') {                      // member access
                std::size_t dot = ps - 1, re = dot, rs = dot;
                while (rs > 0 && identPart(text[rs - 1])) --rs;       // the receiver identifier [rs, re)
                if (rs < re) {
                    std::string repaired = text.substr(0, dot) + text.substr(off);
                    SemanticModel m = analyzeText(uri, repaired);
                    std::size_t lineStart = byteOffset(text, line, 1);
                    int rcol = static_cast<int>(rs - lineStart) + 1;
                    const SymbolDef* rd = m.definitionAt(line, rcol);
                    if (rd && !rd->type.name.empty()) {
                        const std::string& ty = rd->type.name;
                        // Explicit member kinds (Field=5 / Method=2) — completionKind() deliberately maps these
                        // to 0 to keep them OUT of the bare-symbol list; here in member context we want them.
                        for (const auto& d : m.defs)
                            if (d.owner == ty) add(d.name, d.kind == SymbolKind::Field ? 5 : 2);
                    }
                }
                return "[" + items + "]"; // after a '.', never fall through to keywords/bare symbols
            }
        }

        auto it = model_.find(uri);
        if (it != model_.end())
            for (const auto& d : it->second.defs) {
                // A local/param is offered only where it's in scope (its enclosing fn/method extent). Defs with
                // no recorded scope (scopeEnd.line==0 — e.g. top-level, lambdas) are always offered, as before.
                if ((d.kind == SymbolKind::Local || d.kind == SymbolKind::Parameter) && d.scopeEnd.line != 0) {
                    bool inScope =
                        (line > d.scopeStart.line || (line == d.scopeStart.line && col >= d.scopeStart.col)) &&
                        (line < d.scopeEnd.line   || (line == d.scopeEnd.line   && col <= d.scopeEnd.col));
                    if (!inScope) continue;
                }
                add(d.name, completionKind(d.kind));
            }
        static const char* kw[] = {
            "fn", "let", "var", "const", "if", "else", "while", "do", "for", "in", "match", "when", "return",
            "break", "continue", "yield", "record", "class", "interface", "enum", "union", "extension",
            "import", "async", "await", "try", "catch", "finally", "throw", "use", "with", "operator",
            "true", "false", "null", "this", "super"};
        for (const char* k : kw) add(k, 14); // Keyword
        return "[" + items + "]";
    }

    std::string formatting(const json::Value& params) {
        std::string uri = params["textDocument"]["uri"].asString();
        auto it = text_.find(uri);
        if (it == text_.end()) return "null";
        EmitResult r = format(it->second);
        if (!r.ok) return "[]"; // a parse error — leave the buffer untouched (diagnostics show why)
        int endLine = 0, endCol = 0;
        for (char c : it->second) { if (c == '\n') { ++endLine; endCol = 0; } else ++endCol; }
        return "[{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":" + std::to_string(endLine) +
               ",\"character\":" + std::to_string(endCol) + "}},\"newText\":" + json::quote(r.code) + "}]";
    }

    std::string documentSymbol(const json::Value& params) {
        std::string uri = params["textDocument"]["uri"].asString();
        auto it = model_.find(uri);
        if (it == model_.end()) return "[]";
        std::string arr;
        bool first = true;
        for (const SymbolDef* s : it->second.documentSymbols()) {
            int sl = s->nameSpan.start.line, sc = s->nameSpan.start.col;
            std::string r = encRange(uri, sl, sc, sl, sc + s->nameSpan.length);
            if (!first) arr += ",";
            first = false;
            arr += "{\"name\":" + json::quote(s->name) + ",\"kind\":" + std::to_string(lspSymbolKind(s->kind)) +
                   ",\"range\":" + r + ",\"selectionRange\":" + r + "}";
        }
        return "[" + arr + "]";
    }
};

void lspReply(const json::Value& id, const std::string& resultJson) {
    std::string idStr = id.kind == json::Value::Kind::String ? json::quote(id.asString())
                      : id.kind == json::Value::Kind::Number ? id.str
                      : "null";
    lspSend("{\"jsonrpc\":\"2.0\",\"id\":" + idStr + ",\"result\":" + resultJson + "}");
}

int runLsp(const std::vector<std::string>&) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    LspServer srv;
    std::string body;
    while (lspRead(body)) {
        json::Value msg = json::parse(body);
        const std::string& method = msg["method"].asString();
        const json::Value& id = msg["id"];
        const json::Value& params = msg["params"];

        if (method == "initialize") {
            std::string root = uriToPath(params["rootUri"].asString());
            if (root.empty()) root = params["rootPath"].asString();
            const json::Value& opts = params["initializationOptions"];
            if (opts["root"].kind == json::Value::Kind::String) root = opts["root"].asString();
            if (opts["lib"].kind == json::Value::Kind::String) srv.lib_ = opts["lib"].asString();
            srv.root_ = root;
            // Only pick "utf-8" if the client offered it (LSP requires choosing from its positionEncodings);
            // otherwise fall back to the utf-16 default. Our columns are byte offsets; under utf-16 the server
            // converts columns per line (inCol/encRange/semanticTokens) so non-ASCII positions are correct too.
            bool utf8 = false;
            for (const auto& e : params["capabilities"]["general"]["positionEncodings"].items())
                if (e.asString() == "utf-8") utf8 = true;
            std::string enc = utf8 ? "utf-8" : "utf-16";
            srv.utf16_ = !utf8;
            lspReply(id, "{\"capabilities\":{\"positionEncoding\":\"" + enc + "\",\"textDocumentSync\":1,"
                         "\"definitionProvider\":true,\"documentSymbolProvider\":true,\"hoverProvider\":true,"
                         "\"documentFormattingProvider\":true,\"referencesProvider\":true,\"renameProvider\":true,"
                         "\"completionProvider\":{\"triggerCharacters\":[\".\"]},"
                         "\"semanticTokensProvider\":{\"legend\":{\"tokenTypes\":[\"function\",\"type\","
                         "\"method\",\"property\",\"variable\",\"parameter\",\"enumMember\"],"
                         "\"tokenModifiers\":[\"declaration\"]},\"full\":true}},"
                         "\"serverInfo\":{\"name\":\"polyglot-lsp\",\"version\":\"" +
                             Compiler::version() + "\"}}");
        } else if (method == "shutdown") {
            lspReply(id, "null");
        } else if (method == "exit") {
            break;
        } else if (method == "textDocument/didOpen") {
            std::string uri = params["textDocument"]["uri"].asString();
            srv.text_[uri] = params["textDocument"]["text"].asString();
            srv.analyzeDoc(uri);
        } else if (method == "textDocument/didChange") {
            std::string uri = params["textDocument"]["uri"].asString();
            const auto& changes = params["contentChanges"].items();
            if (!changes.empty()) srv.text_[uri] = changes.back()["text"].asString(); // Full sync
            // Re-analyze every open doc (not just the edited one) so a dependent importing this now-edited,
            // still-unsaved buffer refreshes live — the BufferResolver serves the open text. Cheap for the
            // handful of files an editor holds open; dependency-tracked re-analysis is a later optimization.
            for (const auto& kv : srv.text_) srv.analyzeDoc(kv.first);
        } else if (method == "workspace/didChangeWatchedFiles") {
            // pgconfig.json changed — re-analyze every open document so their diagnostics reflect the new
            // root/lib immediately (each analyzeDoc re-reads the manifest and re-publishes).
            for (const auto& kv : srv.text_) srv.analyzeDoc(kv.first);
        } else if (method == "textDocument/didClose") {
            std::string uri = params["textDocument"]["uri"].asString();
            srv.text_.erase(uri);
            srv.model_.erase(uri);
            if (uri.rfind("file:", 0) == 0) srv.publishDiagnostics(uri, {}); // clear squiggles for a closed file
        } else if (method == "textDocument/definition") {
            lspReply(id, srv.definition(params));
        } else if (method == "textDocument/documentSymbol") {
            lspReply(id, srv.documentSymbol(params));
        } else if (method == "textDocument/formatting") {
            lspReply(id, srv.formatting(params));
        } else if (method == "textDocument/semanticTokens/full") {
            lspReply(id, srv.semanticTokens(params));
        } else if (method == "textDocument/references") {
            lspReply(id, srv.references(params));
        } else if (method == "textDocument/rename") {
            lspReply(id, srv.rename(params));
        } else if (method == "textDocument/completion") {
            lspReply(id, srv.completion(params));
        } else if (method == "polyglot/moduleSource") {
            // Custom request: serve an embedded std module's source for a `polyglot:<name>` virtual document.
            std::string u = params["uri"].asString();
            std::string name = u.rfind("polyglot:", 0) == 0 ? u.substr(9) : u;
            lspReply(id, "{\"source\":" + json::quote(embeddedModuleSource(name)) + "}");
        } else if (method == "polyglot/emit") {
            // Custom request: emit a live in-memory preview of the target source for an open .pg doc.
            lspReply(id, srv.generatedSource(params));
        } else if (method == "textDocument/hover") {
            lspReply(id, srv.hover(params));
        } else if (!id.isNull()) {
            lspReply(id, "null"); // unknown request — answer so the client doesn't hang
        }
    }
    return 0;
}

// `polyglot install <dir | name[@spec]> [--update]` (P19 slice 11, rebuilt at P30 slice 3): warm
// the versioned plugin cache through the SAME in-exe pipeline `polyglot build` auto-resolves with
// (registry HTTP API, SRI verify, in-exe extract — no npm, no system tar). Run inside a project it
// also writes the pgconfig.lock.json pin; `--update` re-resolves a range past a satisfied lock.
// A DIRECTORY argument validates a local manifest and points at the `file:` dependency form (local
// plugins resolve in place — no install step to forget).
int runInstall(const std::vector<std::string>& args) {
    std::string spec;
    bool update = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--update") update = true;
        else if (spec.empty()) spec = args[i];
        else { std::cerr << "polyglot: install takes one package (got '" << args[i] << "' too)\n"; return 64; }
    }
    if (spec.empty()) {
        std::cerr << "polyglot: install needs a plugin directory or npm package name\n";
        return 64;
    }

    std::error_code ec;
    if (fs::is_directory(spec, ec)) {
        const fs::path manifestPath = fs::path(spec) / "polyglot-plugin.json";
        std::string manifest, err;
        if (!readFile(manifestPath, manifest)) {
            std::cerr << "polyglot: no polyglot-plugin.json at " << manifestPath.string() << "\n";
            return 1;
        }
        if (!validateBackend(manifest, err)) {
            std::cerr << "polyglot: invalid plugin: " << err << "\n";
            return 1;
        }
        const std::string name = json::parse(manifest)["name"].asString();
        std::cout << "plugin '" << name << "' is valid; local plugins resolve in place - declare it in "
                  << "pgconfig.json:\n  \"dependencies\": { \"" << name << "\": \"file:" << spec << "\" }\n";
        return 0;
    }

    // name[@range]: split a trailing @ (position > 0, so a scope's leading @ survives).
    std::string name = spec, range = "latest";
    if (const std::size_t at = spec.rfind('@'); at != std::string::npos && at > 0) {
        name = spec.substr(0, at);
        range = spec.substr(at + 1);
    }
    name = cli::normalizePluginPackageName(name);

    // Inside a project, install pins the lock; outside one, it only warms the machine cache.
    const PgConfig pc = loadPgConfig(fs::current_path());
    cli::Lockfile lock = pc.found ? cli::loadLockfile(pc.dir) : cli::Lockfile{};

    cli::ResolvedPlugin rp;
    std::string err;
    if (!cli::resolvePluginDependency(cli::defaultHttpGet(), name, range, pc.found ? pc.dir : fs::current_path(),
                                      lock, update, rp, err)) {
        std::cerr << "polyglot: " << err << "\n";
        return 1;
    }
    if (rp.fetched && pc.found) {
        lock.packages[name] = {rp.version, rp.resolvedUrl, rp.integrity};
        if (!cli::saveLockfile(pc.dir, lock))
            std::cerr << "polyglot: warning: could not write " << cli::lockfilePath(pc.dir).string() << "\n";
    }
    std::cout << "installed target '" << rp.targetName << "' (" << name << "@" << rp.version << ")"
              << (rp.fetched ? "" : " [already available]") << " -> "
              << cli::cacheEntryDir(name, rp.version).string() << "\n";
    return 0;
}

// Load every target plugin found next to the executable (`plugins/<target>/polyglot-plugin.json`). The
// CLI is a pure engine — no target is compiled in (PRD §4.11); pgconfig-driven resolution (local paths /
// cache / registry) layers on top at P19 slice 10. A missing plugins dir just leaves the registry empty
// (findTarget then explains what was expected); a MALFORMED artifact is reported and skipped.
void loadPluginsNextToExe(const char* argv0) {
    const fs::path exe = cli::executablePath(argv0);
    std::error_code ec;
    const fs::path dir = exe.parent_path() / "plugins";
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        const fs::path manifest = entry.path() / "polyglot-plugin.json";
        if (!fs::exists(manifest, ec)) continue;
        std::ifstream in(manifest, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        std::string err;
        if (!loadBackend(ss.str(), err))
            std::cerr << "polyglot: " << manifest.string() << ": " << err << "\n";
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);

    if (args.empty() || args[0] == "-h" || args[0] == "--help") {
        printUsage();
        return 0;
    }
    if (args[0] == "--version" || args[0] == "-v") {
        std::cout << Compiler::version() << "\n";
        return 0;
    }
    loadPluginsNextToExe(argv[0]);
    if (args[0] == "build") {
        return runBuild(args);
    }
    if (args[0] == "fmt") {
        return runFmt(args);
    }
    if (args[0] == "check") {
        return runCheck(args);
    }
    if (args[0] == "lsp") {
        return runLsp(args);
    }
    if (args[0] == "install") {
        return runInstall(args);
    }

    std::cerr << "polyglot: unknown command '" << args[0] << "'\n\n";
    printUsage();
    return 64; // EX_USAGE
}
