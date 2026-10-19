#pragma once
// P41 D14: the shared test harness. tests_main.cpp used to be ONE 6757-line translation unit (g++ -O2 took
// ~9.5 min in CI after P40); it is now a family of per-area units (tests_<area>.cpp, each exposing one
// run_<area>()) over this header. Names are unchanged so the test bodies did not move a character.

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <thread>
#include <chrono>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#undef Yield // winbase.h macro; ir::StmtKind::Yield must survive
#endif

#include "mintplayer/polyglot/backend.hpp"
#include "mintplayer/polyglot/backend_engine.hpp"
#include "mintplayer/polyglot/backend_spec_json.hpp"
#include "mintplayer/polyglot/capability.hpp"
#include "mintplayer/polyglot/coverage.hpp"
#include "mintplayer/polyglot/ir.hpp"
#include "mintplayer/polyglot/json.hpp"
#include "mintplayer/polyglot/sourcemap.hpp"
#include "mintplayer/polyglot/lexer.hpp"
#include "mintplayer/polyglot/lower.hpp"
#include "mintplayer/polyglot/parser.hpp"
#include "mintplayer/polyglot/pg_printer.hpp"
#include "mintplayer/polyglot/reachability.hpp"
#include "mintplayer/polyglot/polyglot.hpp"
#include "mintplayer/polyglot/sema.hpp"

#include "exe_path.hpp" // the CLI's portable executable-path lookup; tests add Cli/src to their includes
#include "watch.hpp"    // the CLI's watch-mode support (P21); same include path

#include "inflate.hpp"  // P30 slice 0: the auto-download primitives (same include path as watch.hpp)
#include "lockfile.hpp" // P30 slice 2: the pgconfig.lock.json pin + the versioned plugin cache
#include "outputsink.hpp" // P40 B5 + D17: the one writer, the outputs record, owned scopes
#include "pgconfig.hpp"
#include "plugincache.hpp"
#include "pluginresolve.hpp" // P30 slice 3: the auto-download pipeline
#include "registry.hpp"      // P30 slice 1: semver subset + the npm-registry client
#include "semver.hpp"
#include "sha.hpp"
#include "tar.hpp"

#include <cstring>

// A deliberately tiny, zero-dependency assert harness. The cross-process differential conformance suite
// (compile+run the emitted C# and TS, compare stdout) lives in tests/conformance/ (PLAN P2 gate); these
// in-process tests cover the passes and golden emission.

using namespace mintplayer::polyglot;

// The flat harness: one failure counter, shared by every area unit (defined in test_support.cpp).
extern int g_failures;

void check(bool condition, const std::string& name);
bool has(const std::string& haystack, const std::string& needle);

// Compile with the std prelude auto-imported (print/Math/List available without explicit imports), the
// way a normal build runs with `--lib`. Tests that exercise import-only / unknown-type behavior use the
// bare `compile` (no lib) instead.
EmitResult compileStd(const std::string& src, const BackendHandle& target);

// Cross-area assertion helpers (were lambdas at the top of main(); the areas that use them now live in
// different units, so they are plain functions with the same names and argument shapes).
void rejects(const std::string& src, const std::string& name);       // bare compile fails with diagnostics
void resolves(const std::string& src, const std::string& name);      // bare compile succeeds
void resolvesStd(const std::string& src, const std::string& name);   // std-prelude compile succeeds
void refuses(const std::string& src, const std::string& needle, const std::string& name); // a §3.B "refuses X"
std::string firstErr(const EmitResult& r);                           // first diagnostic message ("" if none)
std::string typeErr(const std::string& src);                         // first diagnostic of a csharp compileStd ("" = compiled)

// A backend that supports the whole §3.A surface except one feature — used to prove §3.E gating bites.
// The two-arg form additionally marks one feature "emulated", to exercise the warn-on-emulated path (slice 0).
class StubBackend : public Backend {
public:
    explicit StubBackend(Feature missing) : missing_(featureName(missing)) {}
    StubBackend(Feature missing, Feature emulated)
        : missing_(featureName(missing)), emulated_(featureName(emulated)), hasEmulated_(true) {}
    std::string name() const override { return "stub"; }
    std::string emit(const ir::Module&) const override { return ""; }
    std::string capabilityStance(const std::string& key) const override {
        if (key == missing_) return "false";
        if (hasEmulated_ && key == emulated_) return "emulated";
        return "native";
    }
    using Backend::capabilityStance;
private:
    std::string missing_;
    std::string emulated_;
    bool hasEmulated_ = false;
};

// A stand-in EvalContext for the P18 emission-DSL interpreter: field values + present-keys from maps, and a
// couple of demo builtins. (The real backend implements EvalContext over ir::Expr in a later slice.)
class MockEval : public engine::EvalContext {
public:
    std::map<std::string, std::string> fields;
    std::string get(const std::string& path) const override {
        auto it = fields.find(path);
        return it == fields.end() ? std::string() : it->second;
    }
    bool has(const std::string& path) const override { return fields.count(path) != 0; }
    std::string emitChild(const std::string& path, const std::string& side) const override {
        return "<" + path + (side.empty() ? "" : ":" + side) + ">"; // marker (no real IR in the mock)
    }
    std::string builtin(const std::string& name, const std::vector<std::string>& args) const override {
        if (name == "upper") {
            std::string s = args.empty() ? "" : args[0];
            for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return s;
        }
        if (name == "join") { // join(sep, a, b, …)
            std::string out;
            for (std::size_t i = 1; i < args.size(); ++i) { if (i > 1) out += args[0]; out += args[i]; }
            return out;
        }
        return "";
    }
};

// Parse + evaluate a Rule from JSON text against a context (test helper).
std::string runRule(const std::string& json, const engine::EvalContext& ctx, bool* okOut = nullptr);

// An in-memory module resolver (the tests' filesystem-free stand-in for the CLI's FileModuleResolver):
// module specifier -> source text. Proves cross-`.pg` resolution works without touching disk.
class MapModuleResolver : public ModuleResolver {
public:
    explicit MapModuleResolver(std::map<std::string, std::string> modules) : modules_(std::move(modules)) {}
    // gcc 11 finds MapModuleResolver({{k, v}, …}) ambiguous between the map ctor and the copy/move ctors
    // (gcc 13 and MSVC resolve it). A dedicated initializer_list ctor is preferred for braced-init on every
    // compiler, so the call sites stay `{{k, v}}` and compile on the whole supported floor (gcc 10+).
    MapModuleResolver(std::initializer_list<std::pair<std::string, std::string>> modules) {
        for (const auto& [spec, src] : modules) modules_.emplace(spec, src);
    }
    std::optional<ResolvedModule> resolve(const std::string& spec, const std::string&) override {
        auto it = modules_.find(spec);
        if (it == modules_.end()) return std::nullopt;
        return ResolvedModule{spec, it->second};
    }
private:
    std::map<std::string, std::string> modules_;
};

// A small program exercising the whole MVP subset.
extern const char* kProgram;

// The area units, in the order tests_main.cpp runs them (= the original top-to-bottom order of main()).
void run_frontend();          // P1-P3 lexer/parser/emission/sema + issues #9 #11 #14
void run_p40_manifest();      // P40 slices 1-4
void run_p40_nullability();   // P40 slices 5-7
void run_p40_strings();       // P40 slice 8 (part 1)
void run_p40_review();        // P40 slice 8 (part 2) + slice 9
void run_sema_targets();      // P4-P15
void run_lsp_modules();       // P16, P38 s1-2, P8, P12, P13, P10, P14, issue #27
void run_engine_p37();        // P18, P19, P37, precedence, P21, P25, P26
void run_p30_download();      // P30 slices 0-4
void run_routing_origin();    // P30 slices 7-8, P38 s3/s8, issues #29 #33-#36
void run_wave2_coverage();    // wave 2 (G42-G48), P39
void run_p40_outputs();       // P40 slices 10-12
void run_p41_determinism();   // P41 (issue #76) D5/D8: per-body temps, byte-identical modules across roots
