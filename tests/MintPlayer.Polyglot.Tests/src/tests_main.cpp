#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
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

#include "gzip_fixtures.hpp" // generated golden gzip/SRI vectors (Node zlib/crypto — independent impl)

#include <cstring>

// A deliberately tiny, zero-dependency assert harness. The cross-process differential conformance suite
// (compile+run the emitted C# and TS, compare stdout) lives in tests/conformance/ (PLAN P2 gate); these
// in-process tests cover the passes and golden emission.

using namespace mintplayer::polyglot;

namespace {

int g_failures = 0;

void check(bool condition, const std::string& name) {
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << name << "\n";
    if (!condition) ++g_failures;
}

bool has(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// Compile with the std prelude auto-imported (print/Math/List available without explicit imports), the
// way a normal build runs with `--lib`. Tests that exercise import-only / unknown-type behavior use the
// bare `compile` (no lib) instead.
EmitResult compileStd(const std::string& src, const BackendHandle& target) {
    return compile(src, target, nullptr, LibConfig{{"io", "math", "collections"}});
}

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
std::string runRule(const std::string& json, const engine::EvalContext& ctx, bool* okOut = nullptr) {
    bool ok = true;
    std::string err;
    engine::Rule r = engine::parseRule(json::parse(json), ok, err);
    if (okOut) *okOut = ok;
    return ok ? engine::evalRule(r, ctx) : std::string("<parse-error:" + err + ">");
}

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
const char* kProgram =
    "fn add(a: i32, b: i32): i32 {\n"
    "  return a + b\n"
    "}\n"
    "fn main() {\n"
    "  let x = 7\n"
    "  var sum = 0\n"
    "  var i = 1\n"
    "  while i <= x {\n"
    "    sum = sum + i\n"
    "    i = i + 1\n"
    "  }\n"
    "  print(add(sum, 100))\n"
    "  if sum > 20 { print(sum) } else { print(0) }\n"
    "}\n";

} // namespace

int main() {
    // Zero backends are compiled in (P19 slice 7e): load the target plugins copied next to this exe by the
    // post-build step. The suite depends on all three — fail hard, not test-by-test, if any is missing.
    {
        namespace fs = std::filesystem;
        const fs::path exe = cli::executablePath();
        for (const char* t : {"csharp", "typescript", "python", "php"}) {
            const fs::path manifest = exe.parent_path() / "plugins" / t / "polyglot-plugin.json";
            std::ifstream in(manifest, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            std::string err;
            if (!in || !loadBackend(ss.str(), err)) {
                std::cerr << "FATAL: cannot load target plugin " << manifest.string() << ": "
                          << (in ? err : "file not readable") << "\n";
                return 1;
            }
        }
    }

    // The version is injected at build time (PRD §4.16 / -DPOLYGLOT_VERSION); there's no committed constant to
    // compare against, so assert its SHAPE: non-empty, digit-led, dot-bearing (e.g. "0.5.0" or "0.0.0-dev").
    {
        const std::string v = Compiler::version();
        check(v.size() >= 3 && v[0] >= '0' && v[0] <= '9' && v.find('.') != std::string::npos,
              "version has a semver-ish shape (digit-led, contains a dot)");
    }

    // Lexer.
    {
        DiagnosticBag d;
        auto toks = lex("fn main() {}", d);
        check(!d.hasErrors(), "lex: clean source has no diagnostics");
        check(!toks.empty() && toks.front().kind == TokKind::KwFn, "lex: first token is 'fn'");
        check(toks.back().kind == TokKind::End, "lex: stream ends with End");
    }

    // Lexer — full (P3) token set: new operators, compound assigns, char literals, keywords.
    {
        DiagnosticBag d;
        auto t = lex("a?.b ?? c[0] ..= >>> += 'x' match with", d);
        check(!d.hasErrors(), "lex: full token set has no diagnostics");
        auto kindAt = [&](std::size_t i) { return i < t.size() ? t[i].kind : TokKind::End; };
        check(kindAt(1) == TokKind::QuestionDot, "lex: '?.' is QuestionDot");
        check(kindAt(3) == TokKind::QuestionQuestion, "lex: '??' is QuestionQuestion");
        check(kindAt(5) == TokKind::LBracket && kindAt(7) == TokKind::RBracket, "lex: '[' ']' brackets");
        check(kindAt(8) == TokKind::DotDotEq, "lex: '..=' is DotDotEq");
        check(kindAt(9) == TokKind::UShr, "lex: '>>>' is UShr");
        check(kindAt(10) == TokKind::PlusEq, "lex: '+=' is PlusEq");
        check(kindAt(11) == TokKind::CharLit && t[11].text == "x", "lex: char literal decoded");
        check(kindAt(12) == TokKind::KwMatch && kindAt(13) == TokKind::KwWith, "lex: 'match'/'with' keywords");
    }

    // Parser fidelity (P3 gate, MVP subset so far): printSource is idempotent on its own output.
    {
        auto roundtrips = [&](const char* src, const std::string& name) {
            DiagnosticBag d1; auto u1 = parse(lex(src, d1), d1);
            std::string s1 = printSource(u1);
            DiagnosticBag d2; auto u2 = parse(lex(s1, d2), d2);
            std::string s2 = printSource(u2);
            check(!d1.hasErrors() && !d2.hasErrors() && s1 == s2, name);
        };
        roundtrips(kProgram, "round-trip: arithmetic program is stable");
        roundtrips("fn f(): i32 => (1 + 2) * 3 - -4\n", "round-trip: precedence/unary stable");
        roundtrips("fn g(a: i32) {\n  if a > 0 { print(a) } else { print(0) }\n}\n",
                   "round-trip: if/else stable");
        roundtrips("fn f(a: i32) {\n  let p = a?.b ?? c.d.e\n  let q = arr[a + 1]\n"
                   "  let r = obj.method(1, 2) + x!\n}\n",
                   "round-trip: member/call/index/?./??/!");
        roundtrips("fn f() {\n  let s = 0..10\n  let t = 0..=10\n"
                   "  let g = (x: i32) => x * 2\n  let h = (x) => { return x }\n}\n",
                   "round-trip: ranges + lambdas");
        roundtrips("fn f() {\n  let l = [1, 2, 3]\n  let tup = (1, 2, 3)\n"
                   "  let w = v with { x = 0, y = 1 }\n}\n",
                   "round-trip: list / tuple / with");
        roundtrips("fn f(a: i32) {\n  let m = if a > 0 { 1 } else { 0 }\n"
                   "  let bits = a & a | a ^ a << 2 >> 1\n}\n",
                   "round-trip: if-expr + bitwise/shift");
        roundtrips("enum Direction { North, East, South, West }\n"
                   "enum HttpCode { Ok = 200, NotFound = 404 }\n"
                   "union Shape {\n  Circle(r: f64)\n  Rect(w: f64, h: f64)\n  Empty\n}\n"
                   "fn area(s: Shape): f64 => match s {\n"
                   "  Circle(r) => 3.14 * r * r,\n  Rect(w, h) => w * h,\n  Empty => 0.0,\n}\n"
                   "fn classify(n: i32): string => match n {\n"
                   "  0 => \"zero\",\n  x if x < 0 => \"negative\",\n  _ => \"many\",\n}\n",
                   "round-trip: enum / union / match / patterns / named types");
        roundtrips("import { print } from \"std.io\"\nimport { sqrt, PI } from \"std.math\"\n"
                   "fn main() {\n  for x in items {\n    if x > 0 { continue }\n"
                   "    print(x)\n    break\n  }\n}\n",
                   "round-trip: imports + for-in + break/continue");
        roundtrips("fn f(input: string) {\n  var n = 0\n  n += 1\n  total -= compute(n)\n"
                   "  obj.field = 5\n  arr[i] = n\n"
                   "  use file = openFile(path) {\n    file.write(n)\n  }\n"
                   "  try {\n    throw Err(\"x\")\n  } catch (e: ParseError) when (n > 0) {\n    print(e)\n"
                   "  } catch (e: Error) {\n    print(e)\n  } finally {\n    cleanup()\n  }\n}\n"
                   "fn gen() {\n  yield 1\n  yield 2\n}\n",
                   "round-trip: try/catch/when/finally/throw/use/yield + compound & lvalue assign");
        roundtrips("const Pi: f64 = 3.14159\nlet table: List<i32> = [1, 2, 3]\n"
                   "record Pair<A, B>(first: A, second: B)\n"
                   "record Money(cents: i64) {\n"
                   "  operator fn plus(o: Money): Money => Money(this.cents + o.cents)\n"
                   "  let dollars: f64 => 0.01\n}\n"
                   "fn clamp<T>(x: T, lo: T = zero, hi: T): T => x\n",
                   "round-trip: records + members + generics + default params + top-level const/let");
        roundtrips("interface Disposable {\n  fn dispose()\n}\n"
                   "open class Shape {\n  open fn area(): f64 => 0.0\n}\n"
                   "class Disk : Shape, Disposable {\n  let r: f64\n  constructor(r: f64) {\n    this.r = r\n  }\n"
                   "  override fn area(): f64 => 3.14 * this.r * this.r\n"
                   "  override fn dispose() {\n    cleanup()\n  }\n}\n"
                   "extension fn string.shout(): string => this + \"!\"\n",
                   "round-trip: class / interface / extension / inheritance / override");
        roundtrips("fn f(name: string, age: i32) {\n"
                   "  print(\"hi ${name}, you are ${age * 2} in dog years!\")\n"
                   "  print(\"${a.b(1)}-${c}-plain\")\n  print(\"no holes here\")\n}\n",
                   "round-trip: string interpolation");
        // P15 async/await: `async fn`/method prefix + `await` expr survive a print/parse round-trip.
        roundtrips("async fn fetch(url: string): i32 => 200\n"
                   "async fn main() {\n  let code = await fetch(\"x\")\n  print(code)\n}\n",
                   "round-trip: async fn + await stable");
        roundtrips("class Client {\n  async fn get(url: string): i32 => 1\n}\n",
                   "round-trip: async method stable");
    }

    // C# emission (golden substrings).
    {
        EmitResult cs = compileStd(kProgram, findTarget("csharp"));
        check(cs.ok, "compile: program -> C# succeeds");
        check(!has(cs.code, "using System;"), "C#: no `using` (BCL refs are global::-qualified)");
        check(has(cs.code, "public static int add(int a, int b)"), "C#: function signature mapped");
        // print is now std.io's generic `print<T>` (capability mechanism): its csharp `actual` body is the
        // Console.WriteLine, and the call site is a normal free-function call (PolyglotProgram.print(...)).
        check(has(cs.code, "global::System.Console.WriteLine(__PgRt.Show(x))") &&
                  has(cs.code, "internal static string Show(object? x)"),
              "C#: std.io print body -> Console.WriteLine(__PgRt.Show(x)) (P40 A3: the one rule)");
        check(has(cs.code, "PolyglotProgram.print(PolyglotProgram.add(sum, 100))"), "C#: print call site -> PolyglotProgram.print");
        // P40 A3: no culture guard — library and program code are identical, and __PgRt is culture-free.
        check(has(cs.code, "static void Main() { main(); }") && !has(cs.code, "CurrentCulture"),
              "C#: main entry point emitted (no culture guard)");
    }

    // TypeScript emission (golden substrings).
    {
        EmitResult ts = compileStd(kProgram, findTarget("typescript"));
        check(ts.ok, "compile: program -> TS succeeds");
        check(has(ts.code, "function add(a: number, b: number): number"), "TS: function signature mapped");
        // print's typescript `actual` wraps in String(...) so bigint/number print identically to C# WriteLine.
        check(has(ts.code, "console.log(String(x))"), "TS: std.io print body -> console.log(String(x))");
        check(has(ts.code, "print(add(sum, 100))"), "TS: print call site");
        check(has(ts.code, "\nmain();\n"), "TS: top-level main() call emitted");
    }

    // Operator precedence is preserved with minimal parentheses.
    {
        EmitResult a = compile("fn f(): i32 => 1 + 2 * 3\n", findTarget("csharp"));
        check(a.ok && has(a.code, "return 1 + 2 * 3;"), "emit: precedence keeps 1 + 2 * 3 unparenthesized");
        EmitResult b = compile("fn g(): i32 => (1 + 2) * 3\n", findTarget("csharp"));
        check(b.ok && has(b.code, "return (1 + 2) * 3;"), "emit: precedence parenthesizes (1 + 2) * 3");
    }

    // Type checker rejects bad programs with diagnostics (never a miscompile).
    auto rejects = [&](const char* src, const std::string& name) {
        EmitResult r = compile(src, findTarget("csharp"));
        check(!r.ok && !r.diagnostics.empty(), name);
    };
    rejects("fn main() { let x = 1 + true; }\n", "sema: rejects i32 + bool");
    rejects("fn main() { print(y); }\n", "sema: rejects undeclared name");
    rejects("fn main() { let x = 1; x = 2; }\n", "sema: rejects assignment to immutable let");
    rejects("fn main() { if 1 { print(0); } }\n", "sema: rejects non-bool if condition");

    // Issue #9 Bug 1 — call-syntax primitive cast `i32(x)` is a cast, not construction (used to emit
    // `new i32(x)`). The happy path is casts_call.pg (conformance); here we lock in the diagnostics for the
    // bad forms that previously emitted silently.
    rejects("fn f(): i32 => i32()\n", "issue#9 Bug1: i32() with no argument is rejected");
    rejects("fn f(x: i32, y: i32): i32 => i32(x, y)\n", "issue#9 Bug1: i32(a, b) with two arguments is rejected");
    rejects("fn f(): i32 => i32(true)\n", "issue#9 Bug1: i32(true) from a bool source is rejected");

    // Issue #11 (1.A) — the transcendental std.math tier emits as plain 1:1 bound statics on both targets.
    {
        EmitResult cs = compileStd("fn f(x: f64): f64 => Math.cos(x) + Math.tanh(x) + Math.pow(x, 2.0)\n", findTarget("csharp"));
        check(cs.ok && has(cs.code, "Math.Cos(") && has(cs.code, "Math.Tanh(") && has(cs.code, "Math.Pow("),
              "issue#11 1.A: transcendental std.math emits (C# Math.Cos/Tanh/Pow)");
        EmitResult ts = compileStd("fn f(x: f64): f64 => Math.cos(x) + Math.tanh(x)\n", findTarget("typescript"));
        check(ts.ok && has(ts.code, "Math.cos(") && has(ts.code, "Math.tanh("),
              "issue#11 1.A: transcendental std.math emits (TS Math.cos/tanh)");
    }

    // Issue #11 (1.C) — a float->i32 cast emits plain Math.trunc on TS (no 32-bit `| 0` wrap that diverges
    // from C#'s (int) across the full range).
    {
        EmitResult ts = compile("fn f(x: f64): i32 => i32(x)\n", findTarget("typescript"));
        check(ts.ok && has(ts.code, "Math.trunc(x)") && !has(ts.code, "| 0"),
              "issue#11 1.C: i32(float) TS emits plain Math.trunc (no | 0)");
    }

    // Issue #11 (1.B) — proper module linking: the importer REFERENCES imported symbols (does not re-define
    // them), the imported module is emitted as its own file, and the C# wrappers become `partial`.
    {
        MapModuleResolver res({{"lib", "class Box { var v: i32\n  constructor(v: i32) { this.v = v } }\nfn dbl(x: i32): i32 { return x * 2 }\n"}});
        EmitResult r = compile("import { dbl, Box } from \"lib\"\nfn compute(): i32 {\n  let b = Box(21)\n  return dbl(b.v)\n}\n",
                               findTarget("csharp"), &res, LibConfig{});
        check(r.ok, "issue#11 1.B: a cross-module program compiles");
        check(!has(r.code, "class Box"), "issue#11 1.B: the importer does NOT re-define the imported type");
        check(has(r.code, "static partial class PolyglotProgram"), "issue#11 1.B: linked C# wrapper is `partial`");
        check(r.modules.size() == 1 && r.modules[0].basename == "lib", "issue#11 1.B: the imported module is emitted as its own file");
        check(!r.modules.empty() && has(r.modules[0].code, "class Box") && has(r.modules[0].code, "int dbl("),
              "issue#11 1.B: the imported type + function are defined in the imported module's file");
    }
    check(importSpecifiers("import { a } from \"nn\"\nimport { b } from \"std.io\"\nfn f() {}\n") == std::vector<std::string>{"nn"},
          "issue#11 1.B: importSpecifiers returns non-std imports only");

    // Access modifier — targetOptions.csharp.access (P40 B1/D9; was LibConfig::access) prefixes emitted C#
    // types + wrappers; the default (empty) keeps C#'s modifier-less `internal` (byte-identical).
    {
        LibConfig pub;
        pub.targetOptions = [](const std::string& t, const std::string&) {
            return t == "csharp" ? std::map<std::string, std::string>{{"access", "public"}}
                                 : std::map<std::string, std::string>{};
        };
        EmitResult r = compile("record Point(x: i32, y: i32)\nfn origin(): Point => Point(0, 0)\n", findTarget("csharp"), nullptr, pub);
        check(r.ok && has(r.code, "public record Point") && has(r.code, "public static class PolyglotProgram"),
              "issue#11 access: csharp.access=public emits public types + wrapper");
        EmitResult def = compile("record Point(x: i32, y: i32)\n", findTarget("csharp"));
        check(def.ok && !has(def.code, "public record Point"), "issue#11 access: default is internal (no public modifier)");
    }

    // Issue #14 — a multi-file C# PROJECT build (LibConfig::sharedPrelude) hoists the runtime prelude
    // (Option/Some/None + wrapper) into a separate `__polyglot_prelude` module so N independent roots don't
    // each emit it (CS0101/CS8863). The default (sharedPrelude=false) inlines it -> byte-identical.
    // P40 §0 changed what "the prelude" means here: only REACHED prelude declarations are emitted, so the
    // program has to use Option for Option to appear at all.
    {
        const char* src = "fn fa(): Option<i32> { return Some(1) }\n";
        LibConfig proj; proj.sharedPrelude = true;
        EmitResult r = compile(src, findTarget("csharp"), nullptr, proj);
        check(r.ok, "issue#14: C# project build compiles");
        check(!has(r.code, "record Option"), "issue#14: entry file does NOT inline the prelude Option union");
        check(has(r.code, "static partial class PolyglotProgram"), "issue#14: entry wrapper is `partial` (mergeable)");
        bool preludeFile = false;
        for (const auto& mf : r.modules)
            if (mf.basename == "__polyglot_prelude" && mf.sharedPrelude && has(mf.code, "record Option"))
                preludeFile = true;
        check(preludeFile, "issue#14: a reached prelude (Option) is emitted once into __polyglot_prelude");
        bool reportsOption = false;
        for (const auto& n : r.preludeReach.names) reportsOption = reportsOption || n == "Option";
        check(reportsOption, "P40 s1: a shared-prelude root reports the prelude names it reaches");
        // Default (single-file) still inlines the REACHED prelude.
        EmitResult def = compile(src, findTarget("csharp"));
        check(def.ok && has(def.code, "record Option") && def.modules.empty(),
              "issue#14: default (non-project) build inlines the reached prelude");
        // C#-only: a TS project build keeps the prelude per-file (no shared file).
        EmitResult ts = compile(src, findTarget("typescript"), nullptr, proj);
        check(ts.ok && ts.modules.empty(), "issue#14: sharedPrelude is a no-op for non-C# targets");
        // P40 s1: a program that never touches Option emits no Option at all — and a project root that
        // reaches nothing reports an empty shared prelude (the host then writes no file).
        EmitResult none = compile("fn fb(): i32 { return 1 }\n", findTarget("csharp"));
        check(none.ok && !has(none.code, "Option"), "P40 s1: an unreached prelude Option is not emitted");
        EmitResult noneProj = compile("fn fb(): i32 { return 1 }\n", findTarget("csharp"), nullptr, proj);
        bool emptyShare = false;
        for (const auto& mf : noneProj.modules) emptyShare = emptyShare || (mf.sharedPrelude && mf.code.empty());
        check(noneProj.ok && emptyShare && noneProj.preludeReach.names.empty(),
              "P40 s1: a root reaching no prelude has an empty shared-prelude share");
    }

    // ---- P40 slice 1: manifest schema 2, binding `requires`, the preludeSlot marker, reachability ---------
    {
        auto manifestOf = [](const char* target) {
            std::string m;
            cli::readFile(cli::executablePath().parent_path() / "plugins" / target / "polyglot-plugin.json", m);
            return m;
        };
        auto replaceAll = [](std::string s, const std::string& from, const std::string& to) {
            for (std::size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size())
                s.replace(at, from.size(), to);
            return s;
        };
        auto replaceOnce = [](std::string s, const std::string& from, const std::string& to, bool& found) {
            const std::size_t at = s.find(from);
            found = at != std::string::npos;
            if (found) s.replace(at, from.size(), to);
            return s;
        };
        auto renamed = [&](const char* target, const std::string& newName) {
            return replaceAll(manifestOf(target), std::string("\"name\": \"") + target + "\"",
                              "\"name\": \"" + newName + "\"");
        };
        const std::string py = renamed("python", "p40py");
        bool f = false;

        // Schema: the Core validates the field it never read before — schema 1, a missing schema and an
        // unknown version are all refused, naming the version this compiler reads.
        std::string err;
        check(validateBackend(py, err), "P40 s1: the in-box (schema 2) manifest validates");
        std::string s1 = replaceOnce(py, "\"schema\": 2", "\"schema\": 1", f);
        err.clear();
        check(f && !validateBackend(s1, err) && has(err, "schema (1)") && has(err, "reads schema 2"),
              "P40 s1: a schema-1 manifest is refused, naming the expected schema");
        std::string s0 = replaceOnce(py, "\"schema\": 2,", "", f);
        err.clear();
        check(f && !validateBackend(s0, err) && has(err, "schema (missing)"),
              "P40 s1: a manifest without a schema is refused");
        std::string s3 = replaceOnce(py, "\"schema\": 2", "\"schema\": 3", f);
        err.clear();
        check(f && !validateBackend(s3, err) && has(err, "schema (3)"),
              "P40 s1: an unknown (newer) schema is refused");

        // preludeSlot: required, exactly once, unconditional, Program only.
        std::string noSlot = replaceOnce(py, "\"fn\": \"preludeSlot\"", "\"fn\": \"ident\"", f);
        err.clear();
        check(f && !validateBackend(noSlot, err) && has(err, "preludeSlot") && has(err, "found 0"),
              "P40 s1: a Program rule without a preludeSlot marker is refused");
        std::string twoSlots = replaceOnce(py, "\"fn\": \"ident\"", "\"fn\": \"preludeSlot\"", f);
        err.clear();
        check(f && !validateBackend(twoSlots, err) && has(err, "exactly one"),
              "P40 s1: a second preludeSlot marker (anywhere) is refused");

        // Binding `requires`: every key must be a declared prelude; object form needs a string template;
        // a type spelling cannot carry requirements.
        const std::string sqrtArm = "\"Math.sqrt\": \"__import__('math').sqrt($0)\"";
        std::string badReq = replaceOnce(py, sqrtArm,
                                         "\"Math.sqrt\": { \"template\": \"x($0)\", \"requires\": [\"nope\"] }", f);
        err.clear();
        check(f && !validateBackend(badReq, err) && has(err, "requires 'nope'") && has(err, "not a declared prelude"),
              "P40 s1: a binding requiring an undeclared prelude is refused (anti-silent-drop)");
        std::string noTmpl = replaceOnce(py, sqrtArm, "\"Math.sqrt\": { \"requires\": [] }", f);
        err.clear();
        check(f && !validateBackend(noTmpl, err) && has(err, "string 'template'"),
              "P40 s1: an object binding without a template is refused");
        std::string typeReq = replaceOnce(py, "\"List.type\": \"list\"",
                                          "\"List.type\": { \"template\": \"list\", \"requires\": [\"fstr\"] }", f);
        err.clear();
        check(f && !validateBackend(typeReq, err) && has(err, "type spelling"),
              "P40 s1: `requires` on a type spelling is refused (nothing would record it)");
        std::string badRuleReq = replaceOnce(py, "\"idiv\": \"def _pg_idiv", "\"idivX\": \"def _pg_idiv", f);
        err.clear();
        check(f && !validateBackend(badRuleReq, err) && has(err, "\"require\"") && has(err, "'idiv'"),
              "P40 s1: a rule-level {\"fn\":\"require\"} naming an undeclared prelude is refused");

        // A binding that requires a helper pulls it in — through a member binding (Bound) and through a
        // synthesized expect-fn actual (Extern) — and ONLY where something calls it.
        std::string pyReq = replaceOnce(py, sqrtArm,
                                        "\"Math.sqrt\": { \"template\": \"_pg_sq($0)\", \"requires\": [\"sq\"] }", f);
        pyReq = replaceOnce(pyReq, "\"readText\": \"__import__('pathlib').Path(path).read_text()\"",
                            "\"readText\": { \"template\": \"_pg_rd(path)\", \"requires\": [\"rd\"] }", f);
        pyReq = replaceOnce(pyReq, "\"preludes\": {",
                            "\"preludes\": { \"sq\": \"def _pg_sq(x):\\n    return x ** 0.5\\n\", "
                            "\"rd\": \"def _pg_rd(p):\\n    return p\\n\",", f);
        err.clear();
        check(f && loadBackend(pyReq, err), "P40 s1: a manifest using the binding object form loads");
        BackendHandle pyr = findTarget("p40py");
        EmitResult sq = compile("import { Math } from \"std.math\"\nfn root(): f64 => Math.sqrt(4.0)\n", pyr);
        check(sq.ok && has(sq.code, "def _pg_sq(x):") && has(sq.code, "_pg_sq(4.0)") && !has(sq.code, "def _pg_rd"),
              "P40 s1: a member binding's `requires` emits its helper (and only that one)");
        check(sq.ok && sq.code.find("def _pg_sq") < sq.code.find("def root"),
              "P40 s1: Python's preludeSlot sits at the top of the file");
        EmitResult rd = compile("import { print, readText } from \"std.io\"\nfn main() { print(readText(\"a\")) }\n", pyr);
        check(rd.ok && has(rd.code, "def _pg_rd(p):") && !has(rd.code, "def _pg_sq"),
              "P40 s1: an expect-fn actual's `requires` emits its helper via the kept prelude function");
        EmitResult neither = compile("fn root(): i32 => 1\n", pyr);
        check(neither.ok && !has(neither.code, "_pg_sq") && !has(neither.code, "_pg_rd") &&
                  !has(neither.code, "def readText"),
              "P40 s1: an unreached binding emits no helper, and an unreached std fn is pruned");

        // C#: manifest helpers land in the file-local `file static class __PgRt`, after the usings.
        std::string cs = renamed("csharp", "p40cs");
        cs = replaceOnce(cs, "\"Math.sqrt\": \"global::System.Math.Sqrt($0)\"",
                         "\"Math.sqrt\": { \"template\": \"__PgRt.Sq($0)\", \"requires\": [\"sq\"] }", f);
        cs = replaceOnce(cs, "\"preludes\": {",
                         "\"preludes\": { \"sq\": \"    internal static double Sq(double x) => "
                         "global::System.Math.Sqrt(x);\\n\",", f);
        err.clear();
        check(f && loadBackend(cs, err), "P40 s1: a C# manifest with a declared prelude loads");
        EmitResult csq = compile("import { Math } from \"std.math\"\nfn root(): f64 => Math.sqrt(4.0)\n",
                                 findTarget("p40cs"));
        const std::size_t pgrt = csq.code.find("file static class __PgRt\n{\n    internal static double Sq(");
        check(csq.ok && pgrt != std::string::npos && has(csq.code, "__PgRt.Sq(4.0)"),
              "P40 s1: C# helpers are wrapped in the slot's `file static class __PgRt`");
        check(csq.ok && csq.code.find("#nullable enable") < pgrt && pgrt < csq.code.find("class PolyglotProgram"),
              "P40 s1: C#'s preludeSlot sits after the usings, before the first declaration");
        EmitResult cnone = compile("fn root(): i32 => 1\n", findTarget("p40cs"));
        check(cnone.ok && !has(cnone.code, "__PgRt"), "P40 s1: no helper required -> no __PgRt class at all");

        // PHP: the slot follows `<?php` (and the require_once lines); prelude functions are guarded by a
        // NAMESPACE-qualified function_exists, so a same-named global function cannot shadow them (S12).
        std::string php = renamed("php", "p40php");
        php = replaceOnce(php, "\"Math.sqrt\": \"sqrt($0)\"",
                          "\"Math.sqrt\": { \"template\": \"__pg_sq($0)\", \"requires\": [\"sq\"] }", f);
        php = replaceOnce(php, "\"preludes\": {",
                          "\"preludes\": { \"sq\": \"function __pg_sq($x) { return sqrt($x); }\\n\",", f);
        err.clear();
        check(f && loadBackend(php, err), "P40 s1: a PHP manifest with a declared prelude loads");
        EmitResult psq = compile("import { Math } from \"std.math\"\nfn root(): f64 => Math.sqrt(4.0)\n",
                                 findTarget("p40php"));
        const std::size_t ph = psq.code.find("function __pg_sq(");
        check(psq.ok && ph != std::string::npos && psq.code.find("<?php") < ph &&
                  ph < psq.code.find("function root"),
              "P40 s1: PHP's preludeSlot follows <?php, before the declarations");
        EmitResult pio = compile("import { print, readText } from \"std.io\"\nfn main() { print(readText(\"a\")) }\n",
                                 findTarget("php"));
        check(pio.ok && has(pio.code, "function_exists(__NAMESPACE__.'\\\\readText')"),
              "P40 s1: PHP prelude functions are guarded by a namespace-qualified function_exists");

        // TS: prelude declarations are module-local — never exported — while user declarations still are.
        EmitResult tsio = compile("import { print, readText } from \"std.io\"\nfn main() { print(readText(\"a\")) }\n",
                                  findTarget("typescript"));
        check(tsio.ok && has(tsio.code, "function readText(") && !has(tsio.code, "export function readText(") &&
                  has(tsio.code, "export function main("),
              "P40 s1: TS prelude declarations are not exported; user declarations are");
        EmitResult tsOpt = compile("fn f(): Option<i32> => None\n", findTarget("typescript"));
        check(tsOpt.ok && has(tsOpt.code, "type Option") && !has(tsOpt.code, "export type Option"),
              "P40 s1: a reached prelude union (via a bare case) is emitted, unexported");

        // Reachability on a hand-built module: user -> A -> B -> U (transitive), C unreached; a forced name
        // keeps an otherwise-unreached declaration (the shared prelude's union across roots).
        auto fnDecl = [](const std::string& name, const std::string& origin, const std::string& calls,
                         const std::string& returns) {
            ir::Function fn;
            fn.name = fn.mangledName = name;
            fn.originModule = origin;
            fn.returnType = namedType(returns.empty() ? "unit" : returns);
            if (!calls.empty())
                fn.body.push_back(std::make_unique<ir::ExprStmt>(
                    SourcePos{}, std::make_unique<ir::Call>(SourcePos{}, namedType("unit"), calls, false)));
            return fn;
        };
        auto buildModule = [&]() {
            ir::Module m;
            m.functions.push_back(fnDecl("userMain", "", "A", ""));
            m.functions.push_back(fnDecl("A", ir::kPreludeOrigin, "B", ""));
            m.functions.push_back(fnDecl("B", ir::kPreludeOrigin, "", "U"));
            m.functions.push_back(fnDecl("C", ir::kPreludeOrigin, "", "V"));
            ir::Union u; u.name = "U"; u.originModule = ir::kPreludeOrigin; m.unions.push_back(std::move(u));
            ir::Union v; v.name = "V"; v.originModule = ir::kPreludeOrigin; m.unions.push_back(std::move(v));
            return m;
        };
        ir::Module rm = buildModule();
        const std::set<std::string> reach = preludeReach(rm);
        check(reach.count("A") && reach.count("B") && reach.count("U") && !reach.count("C") && !reach.count("V"),
              "P40 s1: reachability is transitive through prelude declarations (A -> B -> U), C/V unreached");
        pruneUnreachablePrelude(rm);
        check(rm.functions.size() == 3 && rm.unions.size() == 1 && rm.unions[0].name == "U",
              "P40 s1: pruning erases exactly the unreached prelude declarations, never user ones");
        ir::Module forced = buildModule();
        const std::set<std::string> extra{"C"};
        pruneUnreachablePrelude(forced, &extra);
        check(forced.functions.size() == 4 && forced.unions.size() == 2,
              "P40 s1: a forced name (reached by another root) is kept, with what IT reaches (C -> V)");
        // The backstop: a prelude name spelled only inside opaque extern text still keeps it alive.
        ir::Module ext;
        ir::Function uf = fnDecl("userMain", "", "", "");
        uf.body.push_back(std::make_unique<ir::ExprStmt>(
            SourcePos{}, std::make_unique<ir::Extern>(SourcePos{}, namedType("unit"), "C();")));
        ext.functions.push_back(std::move(uf));
        ext.functions.push_back(fnDecl("C", ir::kPreludeOrigin, "", ""));
        check(preludeReach(ext).count("C"), "P40 s1: the extern-text identifier scan keeps a referenced prelude decl");
    }

    // ---- P40 slice 2: target options (manifest `options`, pgconfig/CLI targetOptions), namespaces + classes --
    {
        using OptMap = std::map<std::string, std::string>;
        // A LibConfig answering `opts` for `target` (every file), nothing for any other target.
        auto optsFor = [](const std::string& target, OptMap opts) {
            LibConfig l;
            l.targetOptions = [target, opts](const std::string& t, const std::string&) {
                return t == target ? opts : OptMap{};
            };
            return l;
        };
        const char* prog = "import { print } from \"std.io\"\nrecord Point(x: i32, y: i32)\n"
                           "fn find(): Option<i32> => Some(1)\nfn main() { print(find() ?? 0) }\n";

        // Resolution: defaults fill in, an unknown option and a pattern mismatch refuse (naming the fix).
        {
            OptMap res;
            std::string err;
            check(resolveTargetOptions(*findBackend("csharp"), {}, res, err) && res["class"] == "PolyglotProgram" &&
                      res["namespace"].empty() && res["access"].empty(),
                  "P40 s2: unset options resolve to the manifest defaults");
            check(!resolveTargetOptions(*findBackend("csharp"), {{"nmspace", "X"}}, res, err) &&
                      has(err, "unknown target option 'csharp.nmspace'") && has(err, "namespace, class, access"),
                  "P40 s2: an unknown option is refused, listing the declared ones");
            check(!resolveTargetOptions(*findBackend("csharp"), {{"access", "private"}}, res, err) &&
                      has(err, "invalid value 'private'"),
                  "P40 s2: a value failing the option's pattern is refused");
            check(!resolveTargetOptions(*findBackend("csharp"), {{"namespace", "My..Lib"}}, res, err),
                  "P40 s2: a malformed namespace is refused");
            check(!resolveTargetOptions(*findBackend("typescript"), {{"namespace", "X"}}, res, err) &&
                      has(err, "declares no options"),
                  "P40 s2: a target declaring no options refuses any");
            EmitResult bad = compile(prog, findTarget("csharp"), nullptr, optsFor("csharp", {{"access", "nope"}}));
            check(!bad.ok && !bad.diagnostics.empty() && has(bad.diagnostics[0].message, "csharp.access"),
                  "P40 s2: compile() refuses an invalid option with a diagnostic");
        }

        // P40 review B11: csharp.entryPoint=false drops the `Main` an entry file gets (CS7022/CS0017 in an app
        // that owns its entry point); the default keeps it.
        {
            EmitResult on = compile(prog, findTarget("csharp"), nullptr, optsFor("csharp", {}));
            EmitResult off = compile(prog, findTarget("csharp"), nullptr, optsFor("csharp", {{"entryPoint", "false"}}));
            check(on.ok && has(on.code, "static void Main()") && off.ok && !has(off.code, "static void Main()") &&
                      has(off.code, "void main()"),
                  "P40 B11: csharp.entryPoint=false omits Main, keeps main()");
        }

        // C#: file-scoped namespace before the usings, the configured wrapper class, access, internal helpers.
        {
            EmitResult r = compile(prog, findTarget("csharp"), nullptr,
                                   optsFor("csharp", {{"namespace", "My.Lib"}, {"class", "Api"}, {"access", "public"}}));
            // The two generated-header lines (P40 B5) come first; the namespace follows #nullable.
            check(r.ok && r.code.find("// <auto-generated/>\n// Generated by Polyglot. Do not edit: regenerate with "
                                      "`polyglot build`.\n#nullable enable\nnamespace My.Lib;\n") == 0,
                  "P40 s2: C# namespace is a file-scoped declaration right after #nullable");
            check(r.ok && has(r.code, "public static class Api") && has(r.code, "Api.print(") && has(r.code, "Api.find()") &&
                      !has(r.code, "PolyglotProgram"),
                  "P40 s2: C# `class` renames the wrapper and every qualified call");
            check(r.ok && has(r.code, "public abstract record Option<T>") && has(r.code, "public record Point"),
                  "P40 s2: prelude types in the namespace follow `access`");
            check(r.ok && has(r.code, "internal static void print<T>") && has(r.code, "public static Option<int> find()"),
                  "P40 s2: prelude helper functions stay internal under access=public");
            EmitResult g = compile("let LIMIT: i32 = 3\nfn f(): i32 => LIMIT\n", findTarget("csharp"), nullptr,
                                   optsFor("csharp", {{"namespace", "N"}, {"class", "Api"}}));
            check(g.ok && g.code.find("namespace N;\nusing static Api;") != std::string::npos,
                  "P40 s2: `using static <class>` follows the namespace (resolves inside it)");
            EmitResult def = compile(prog, findTarget("csharp"));
            check(def.ok && !has(def.code, "namespace ") && has(def.code, "static class PolyglotProgram"),
                  "P40 s2: no options = no namespace and the PolyglotProgram wrapper (unchanged output)");
        }

        // The wrapper class is emitted only when it has members.
        {
            EmitResult types = compile("record Point(x: i32, y: i32)\nenum Color { Red, Green }\n", findTarget("csharp"));
            check(types.ok && !has(types.code, "PolyglotProgram") && has(types.code, "record Point"),
                  "P40 s2: a C# file with no functions/globals emits no empty wrapper class");
            check(types.ok && types.code.back() == '\n' && types.code.find("\n\n\n") == std::string::npos,
                  "P40 s2: dropping the wrapper leaves no dangling blank lines");
        }

        // `reserve`: the configured wrapper name is claimed like an identifiers.reserved entry.
        {
            EmitResult clash = compile("record Api(x: i32)\n", findTarget("csharp"), nullptr,
                                       optsFor("csharp", {{"class", "Api"}}));
            check(!clash.ok && !clash.diagnostics.empty() && has(clash.diagnostics[0].message, "'Api' is reserved") &&
                      has(clash.diagnostics[0].message, "option 'class'"),
                  "P40 s2: a user identifier spelled like the configured class is refused");
            EmitResult dflt = compile("record PolyglotProgram(x: i32)\n", findTarget("csharp"));
            check(!dflt.ok, "P40 s2: the DEFAULT class name stays reserved (via the option default)");
            EmitResult freed = compile("record PolyglotProgram(x: i32)\n", findTarget("csharp"), nullptr,
                                       optsFor("csharp", {{"class", "Api"}}));
            check(freed.ok, "P40 s2: renaming the wrapper frees the old name");
        }

        // Closure consistency: a closure-scoped option must agree across files that import each other.
        {
            MapModuleResolver res({{"lib", "fn twice(x: i32): i32 => x * 2\n"}});
            LibConfig split;
            split.targetOptions = [](const std::string& t, const std::string& origin) {
                if (t != "csharp") return OptMap{};
                return origin.empty() ? OptMap{{"namespace", "A"}} : OptMap{{"namespace", "B"}};
            };
            EmitResult r = compile("import { twice } from \"lib\"\nfn f(): i32 => twice(2)\n", findTarget("csharp"),
                                   &res, split, "entry.pg");
            check(!r.ok && !r.diagnostics.empty() && has(r.diagnostics[0].message, "must agree across one import closure") &&
                      has(r.diagnostics[0].message, "csharp.namespace"),
                  "P40 s2: files of one closure resolving different namespaces are refused");
            EmitResult same = compile("import { twice } from \"lib\"\nfn f(): i32 => twice(2)\n", findTarget("csharp"),
                                      &res, optsFor("csharp", {{"namespace", "A"}}), "entry.pg");
            check(same.ok && same.modules.size() == 1 && has(same.code, "namespace A;") &&
                      has(same.modules[0].code, "namespace A;"),
                  "P40 s2: an agreeing closure puts every emitted file in the namespace");
        }

        // PHP: `namespace X;` right after <?php, before the prelude slot (the guarded helpers follow it).
        {
            EmitResult p = compile("import { print, readText } from \"std.io\"\nfn main() { print(readText(\"a\")) }\n",
                                   findTarget("php"), nullptr, optsFor("php", {{"namespace", "My\\Lib"}}));
            const std::size_t ns = p.code.find("<?php\n// <auto-generated/>\n// Generated by Polyglot. Do not edit: "
                                               "regenerate with `polyglot build`.\nnamespace My\\Lib;\n");
            check(p.ok && ns == 0 && p.code.find("function_exists(__NAMESPACE__") > ns,
                  "P40 s2: PHP namespace follows <?php and precedes the guarded prelude");
        }

        // TS: a file whose declarations are all (unexported) prelude stays an ES module.
        {
            EmitResult empty = compile("import { readText } from \"std.io\"\n", findTarget("typescript"));
            check(empty.ok && has(empty.code, "export {};"), "P40 s2: a TS file with no exports emits `export {};`");
            EmitResult some = compile("fn f(): i32 => 1\n", findTarget("typescript"));
            check(some.ok && !has(some.code, "export {};"), "P40 s2: a TS file with exports emits no `export {};`");
        }

        // Loader: the options block is validated, and a rule may only read declared options.
        {
            auto manifest = [](const char* target) {
                std::string m;
                cli::readFile(cli::executablePath().parent_path() / "plugins" / target / "polyglot-plugin.json", m);
                // The swaps below spell multi-line fragments with "\n"; a CRLF checkout (core.autocrlf) must not
                // make them silently miss.
                m.erase(std::remove(m.begin(), m.end(), '\r'), m.end());
                return m;
            };
            auto swap = [](std::string s, const std::string& from, const std::string& to) {
                const std::size_t at = s.find(from);
                if (at != std::string::npos) s.replace(at, from.size(), to);
                return s;
            };
            std::string err;
            std::string php = manifest("php"); // renamed everywhere (top-level + spec.name must agree)
            for (std::size_t at = 0; (at = php.find("\"name\": \"php\"", at)) != std::string::npos;)
                php.replace(at, 13, "\"name\": \"p40s2php\"");
            check(!validateBackend(swap(php, "\"namespace\": {\n      \"default\": \"\",",
                                        "\"namespace\": {\n      \"default\": \"\", \"colour\": \"x\","), err) &&
                      has(err, "unknown key 'colour'"),
                  "P40 s2: an unknown key in an option declaration is refused");
            check(!validateBackend(swap(php, "\"default\": \"\",", "\"default\": \"a-b\","), err) &&
                      has(err, "does not match its own pattern"),
                  "P40 s2: a default failing its own pattern is refused");
            check(!validateBackend(swap(php, "\"scope\": \"closure\"", "\"scope\": \"file\""), err) &&
                      has(err, "'scope' must be"),
                  "P40 s2: an unknown scope is refused");
            check(!validateBackend(swap(php, "\"get\": \"option.namespace\"", "\"get\": \"option.nmspace\""), err) &&
                      has(err, "option.nmspace"),
                  "P40 s2: a rule reading an undeclared option is refused");
            check(!validateBackend(swap(php, "\"pattern\": \"^(", "\"pattern\": \"^(("), err) &&
                      has(err, "not a valid regular expression"),
                  "P40 s2: an uncompilable pattern is refused");
        }

        // CLI layering: project < include rule < --target-option; flags parse; validation names the layer.
        {
            namespace fs = std::filesystem;
            const fs::path proj = (fs::temp_directory_path() / "p40-s2-opts" / "proj").lexically_normal();
            cli::PgConfig pc;
            pc.found = true;
            pc.dir = proj;
            pc.targetOptions["csharp"] = {{"namespace", "Proj"}, {"access", "public"}};
            cli::IncludeRule rule{"libs/**/*.pg", {}, "gen/%(Filename)"};
            rule.targetOptions["csharp"] = {{"namespace", "Proj.Libs"}};
            pc.include.push_back(rule);
            cli::TargetOptionsByTarget flags;
            std::string err;
            check(cli::parseTargetOptionFlag("csharp.class=Api", flags, err) && flags["csharp"]["class"] == "Api",
                  "P40 s2: --target-option <target>.<name>=<value> parses");
            check(cli::parseTargetOptionFlag("csharp.namespace=A.B=C", flags, err) && flags["csharp"]["namespace"] == "A.B=C",
                  "P40 s2: --target-option keeps everything after the first '=' as the value");
            check(!cli::parseTargetOptionFlag("csharp=x", flags, err) && !cli::parseTargetOptionFlag("namespace", flags, err),
                  "P40 s2: a malformed --target-option is refused");
            const auto top = cli::fileTargetOptions(pc, {}, proj / "main.pg", "csharp");
            const auto lib = cli::fileTargetOptions(pc, {}, proj / "libs/x/strings.pg", "csharp");
            check(top.at("namespace") == "Proj" && lib.at("namespace") == "Proj.Libs" && lib.at("access") == "public",
                  "P40 s2: an include rule's targetOptions overlay the project level for the files it matches");
            cli::TargetOptionsByTarget f2;
            f2["csharp"]["namespace"] = "Flag";
            check(cli::fileTargetOptions(pc, f2, proj / "libs/x/strings.pg", "csharp").at("namespace") == "Flag",
                  "P40 s2: --target-option wins over every pgconfig layer");
            check(cli::fileTargetOptions(pc, {}, proj / "main.pg", "php").empty(),
                  "P40 s2: another target's options never leak (a lookup by target name)");
            std::vector<std::string> errs;
            cli::PgConfig badPc = pc;
            badPc.targetOptions["csharpp"] = {{"namespace", "X"}};
            badPc.include[0].targetOptions["csharp"] = {{"klass", "X"}};
            cli::validateTargetOptions(badPc, {}, errs);
            check(errs.size() == 2 && has(errs[0] + errs[1], "unknown target 'csharpp'") &&
                      has(errs[0] + errs[1], "include rule 'libs/**/*.pg'"),
                  "P40 s2: config validation names the unknown target and the offending include rule");

            // pgconfig parsing: targetOptions shape errors and the removed top-level `access` are refused.
            const fs::path base = fs::temp_directory_path() / "polyglot-p40-s2-cfg";
            fs::create_directories(base);
            cli::writeFile(base / "pgconfig.json",
                           "{ \"access\": \"public\", \"targetOptions\": { \"csharp\": { \"namespace\": 3 } },"
                           " \"include\": [ { \"pattern\": \"*.pg\", \"output\": \"%(Filename)\","
                           " \"targetOptions\": { \"php\": { \"namespace\": \"R\" } } } ] }");
            const cli::PgConfig parsed = cli::loadPgConfig(base);
            bool accessRefused = false, shapeRefused = false;
            for (const auto& e : parsed.errors) {
                accessRefused = accessRefused || has(e, "top-level \"access\" was removed");
                shapeRefused = shapeRefused || has(e, "targetOptions.csharp.namespace must be a string");
            }
            check(accessRefused && shapeRefused && parsed.include.size() == 1 &&
                      parsed.include[0].targetOptions.at("php").at("namespace") == "R",
                  "P40 s2: pgconfig parses rule targetOptions and refuses a bad shape and the old top-level access");
            fs::remove_all(base);
        }
    }

    // ---- P40 slice 3: escapes — one decoder, char validation, per-target `literals`, fmt inverse, JSON -------
    {
        // Lex one literal; return its decoded text and the diagnostics.
        struct Lexed { std::string text; std::vector<Diagnostic> diags; int errors = 0; int warnings = 0; };
        auto lexOne = [](const std::string& src) {
            DiagnosticBag d;
            auto toks = lex(src, d);
            Lexed l;
            if (!toks.empty()) l.text = toks[0].text;
            l.diags = d.items();
            for (const auto& x : l.diags) (x.severity == Severity::Error ? l.errors : l.warnings)++;
            return l;
        };
        auto errAt = [](const Lexed& l, int col, const std::string& frag) {
            for (const auto& x : l.diags)
                if (x.severity == Severity::Error && x.pos.col == col && has(x.message, frag)) return true;
            return false;
        };

        // The decode table: every accepted escape, in one literal.
        std::string want = "\n\t\r";
        want += '\0';
        want += "\\\"'$A\xF0\x9F\x98\x80";
        Lexed all = lexOne("\"\\n\\t\\r\\0\\\\\\\"\\'\\$\\u{41}\\u{1F600}\"");
        check(all.errors == 0 && all.text == want, "P40 s3: decode table (\\n \\t \\r \\0 \\\\ \\\" \\' \\$ \\u{41} \\u{1F600})");
        check(lexOne("\"\\u{00001D}\"").text == "\x1D" && lexOne("\"\\u{1d}\"").text == "\x1D",
              "P40 s3: \\u{} takes 1-6 hex digits, either case");
        check(lexOne("\"\\${y}\"").text == "${y}", "P40 s3: \\${ is a literal ${, not a hole");

        // Every refused escape is an error AT THE BACKSLASH (col 4 in "ab\…").
        Lexed q = lexOne("\"ab\\qc\" x");
        check(q.errors == 1 && errAt(q, 4, "unknown escape sequence `\\q`") && errAt(q, 4, "valid escapes are"),
              "P40 s3: unknown escape -> one error at the backslash naming the valid set");
        check(errAt(lexOne("\"ab\\u0041\""), 4, "write the code point in braces"),
              "P40 s3: \\uXXXX refused with the guiding message");
        check(errAt(lexOne("\"ab\\x41\""), 4, "`\\u{HH}`"), "P40 s3: \\xHH refused with the guiding message");
        check(errAt(lexOne("\"ab\\u{}\""), 4, "malformed") && errAt(lexOne("\"ab\\u{1234567}\""), 4, "malformed") &&
                  errAt(lexOne("\"ab\\u{41\""), 4, "malformed"),
              "P40 s3: empty / 7-digit / unclosed \\u{} are malformed");
        check(errAt(lexOne("\"ab\\u{110000}\""), 4, "beyond U+10FFFF"), "P40 s3: \\u{110000} is out of range");
        check(errAt(lexOne("\"ab\\u{D83D}\""), 4, "surrogate"), "P40 s3: a surrogate \\u{} is refused (PHP would write WTF-8)");
        check(errAt(lexOne("\"ab\xFF\""), 4, "invalid UTF-8"), "P40 s3: invalid UTF-8 in a literal is an error");
        check(errAt(lexOne("'\xC3'"), 2, "invalid UTF-8"), "P40 s3: invalid UTF-8 in a char literal is an error");

        // Char literals: exactly one character that fits one UTF-16 code unit.
        check(errAt(lexOne("''"), 1, "empty char literal"), "P40 s3: an empty char literal is an error");
        check(errAt(lexOne("'ab'"), 1, "more than one character"), "P40 s3: a multi-character char literal is an error");
        check(errAt(lexOne("'\\u{1F600}'"), 1, "one UTF-16 code unit"), "P40 s3: an astral char literal is an error");
        Lexed ca = lexOne("'\\u{41}'");
        check(ca.errors == 0 && ca.warnings == 0 && ca.text == "A", "P40 s3: '\\u{41}' is the char A");
        Lexed ce = lexOne("'\\u{E9}'");
        check(ce.errors == 0 && ce.warnings == 0 && ce.text == "\xC3\xA9", "P40 s3/s8: '\\u{E9}' decodes, no warning (UTF-16 faithful everywhere)");
        check(lexOne("'\\''").text == "'" && lexOne("'\"'").text == "\"" && lexOne("'\\\"'").text == "\"",
              "P40 s3: quotes in chars, escaped or not");

        // P40 s8: non-ASCII text is faithful on every target now — no warning, decoded or raw.
        Lexed w = lexOne("\"caf\\u{E9} \\u{E8}\"");
        check(w.errors == 0 && w.warnings == 0, "P40 s8: a decoded non-ASCII escape no longer warns");
        check(lexOne("\"plain \\u{7F}\"").warnings == 0, "P40 s3: ASCII escapes do not warn");
        {
            EmitResult ok = compile("extension fn i32.twice(): i32 => this * 2\n", findTarget("php"));
            bool warned = false;
            for (const auto& d : ok.diagnostics) warned = warned || (d.severity == Severity::Warning && has(d.message, "emulates extensionMethods"));
            check(ok.ok && warned, "P40 s3: a SUCCESSFUL compile still reports its warnings to the host");
        }

        // Emission: per-target goldens of the literal text. The Core picks WHICH characters escape (C0, DEL,
        // C1 incl. U+0085 NEL, U+2028); printable non-ASCII (é, 😀) passes through; PHP escapes `$`.
        const std::string body = "fn s(): string => \"a\\u{1D}\\u{85}\\u{2028}\\u{E9}$x\\u{1F600}\\u{0}1\"\n";
        auto emitted = [&](const char* target) { return compile(body, findTarget(target)).code; };
        check(has(emitted("csharp"), "\"a\\u001D\\u0085\\u2028\xC3\xA9$x\xF0\x9F\x98\x80\\u00001\""),
              "P40 s3: C# literal golden (\\u$H4, raw printable non-ASCII, NUL never \\0)");
        check(has(emitted("typescript"), "\"a\\u{1D}\\u{85}\\u{2028}\xC3\xA9$x\xF0\x9F\x98\x80\\u{0}1\""),
              "P40 s3: TS literal golden (\\u{$H})");
        check(has(emitted("python"), "\"a\\u001D\\u0085\\u2028\xC3\xA9$x\xF0\x9F\x98\x80\\u00001\""),
              "P40 s3: Python literal golden (\\u$H4)");
        check(has(emitted("php"), "\"a\\u{1D}\\u{85}\\u{2028}\xC3\xA9\\$x\xF0\x9F\x98\x80\\u{0}1\""),
              "P40 s3: PHP literal golden ($ -> \\$, the \"cost $x\" miscompile)");
        check(compile("fn s(): string => \"plain ascii \\\"q\\\" \\\\ \\n\"\n", findTarget("csharp")).code.find(
                  "\"plain ascii \\\"q\\\" \\\\ \\n\"") != std::string::npos,
              "P40 s3: plain ASCII literals are byte-identical to the pre-P40 output");
        const std::string interp = "fn f(x: i32): string => \"{${x}}`$\\u{2028}\"\n";
        check(has(compile(interp, findTarget("csharp")).code, "$\"{{{__PgRt.Str(x)}}}`$\\u2028\""),
              "P40 s3: C# interp context doubles braces and escapes U+2028");
        check(has(compile(interp, findTarget("typescript")).code, "`{${x}}\\`$\\u{2028}`"),
              "P40 s3: TS interp context escapes the backtick");
        check(has(compile(interp, findTarget("php")).code, "\"}`\\$\\u{2028}\""),
              "P40 s3: PHP interp chunks escape $");
        check(has(compile("fn c(): char => '\\''\n", findTarget("csharp")).code, "'\\''") &&
                  has(compile("fn c(): char => '\\u{1D}'\n", findTarget("csharp")).code, "'\\u001D'"),
              "P40 s3: C# char context (quote ', \\u$H4)");

        // A bound template's receiver placeholder takes the receiver parenthesization policy (found by
        // escapes.pg: `("a\0" + "1").len()` emitted `"a\u0000" + "1".Length` on C# and TS).
        {
            const char* lp = "import \"std.strings\"\nfn n(): i32 => (\"a\" + \"b\").len()\n";
            check(has(compileStd(lp, findTarget("csharp")).code, "(\"a\" + \"b\").Length") &&
                      has(compileStd(lp, findTarget("typescript")).code, "(\"a\" + \"b\").length"),
                  "P40 s3: a binary receiver of a bound member template is parenthesized");
        }

        // The astral template, and the "surrogates" default.
        auto specOf = [](const char* target) {
            std::string m;
            cli::readFile(cli::executablePath().parent_path() / "plugins" / target / "polyglot-plugin.json", m);
            return loadBackendSpec(json::parse(m)["spec"]);
        };
        SpecLoadResult cs = specOf("csharp"), ts = specOf("typescript"), py = specOf("python"), php = specOf("php");
        check(cs.ok && ts.ok && py.ok && php.ok, "P40 s3: the four in-box specs load");
        check(specCodepointEscape(cs.spec.literals.at("string"), 0x1F600) == "\\U0001F600" &&
                  specCodepointEscape(ts.spec.literals.at("string"), 0x1F600) == "\\u{1F600}" &&
                  specCodepointEscape(py.spec.literals.at("string"), 0x1F600) == "\\U0001F600" &&
                  specCodepointEscape(php.spec.literals.at("string"), 0x1F600) == "\\u{1F600}",
              "P40 s3: astral templates (C#/Python \\U$H8, TS/PHP \\u{$H})");
        check(specCodepointEscape(cs.spec.literals.at("char"), 0x1F600) == "\\uD83D\\uDE00",
              "P40 s3: no astral template = UTF-16 surrogate halves through `codepoint`");

        // Serializer round-trip of the literals block.
        SpecLoadResult back = loadBackendSpec(backendSpecToJson(php.spec));
        bool same = back.ok && back.spec.literals.size() == php.spec.literals.size();
        for (const auto& kv : php.spec.literals) {
            auto it = back.spec.literals.find(kv.first);
            same = same && it != back.spec.literals.end() && it->second.quote == kv.second.quote &&
                   it->second.map == kv.second.map && it->second.codepoint == kv.second.codepoint &&
                   it->second.astral == kv.second.astral;
        }
        check(same, "P40 s3: spec.literals round-trips through backendSpecToJson");

        // Loader refusals (anti-silent-drop): codepoint required, string context required, a rule naming an
        // undeclared context, the retired `escapes` block, a map not escaping its quote.
        std::string csm;
        cli::readFile(cli::executablePath().parent_path() / "plugins" / "csharp" / "polyglot-plugin.json", csm);
        auto refused = [&](const std::string& from, const std::string& to, const std::string& frag) {
            const std::size_t at = csm.find(from);
            if (at == std::string::npos) return false;
            std::string m = csm;
            m.replace(at, from.size(), to);
            std::string err;
            return !validateBackend(m, err) && has(err, frag);
        };
        check(refused("\"codepoint\": \"\\\\u$H4\",", "", "needs a 'codepoint'"),
              "P40 s3: a literal context without a codepoint template is refused");
        check(refused("\"codepoint\": \"\\\\u$H4\"", "\"codepoint\": \"\\\\x$H2\"", "needs a 'codepoint'"),
              "P40 s3: a \\x$H2 codepoint template (cannot spell U+2028) is refused");
        check(refused("\"string\": {", "\"strung\": {", "'string' context"),
              "P40 s3: a manifest without a string literal context is refused");
        check(refused("\"interp\",", "\"interpx\",", "'interpx'"), // (the first `"interp",` is a literal context use;
                                                                     //  wrapAtom.recvPrimary spells it last, comma-free)
              "P40 s3: a rule naming an undeclared literal context is refused");
        check(refused("\"literals\": {", "\"escapes\": {}, \"literals\": {", "replaced by 'literals'"),
              "P40 s3: the retired `escapes` block is refused, naming its replacement");
        check(refused("\"\\\"\": \"\\\\\\\"\",", "", "must escape the backslash and the context's quote"),
              "P40 s3: a context whose map does not escape its quote is refused");

        // `polyglot fmt` is the exact inverse of the lexer: `\${`, `\0`, `\u{…}` for non-printables, raw é.
        std::string fsrc = "fn f(x: i32): string => \"a\\${b} \\u{1D}\\0\\u{2028} \\u{E9} \\$ ${x}\"\n"
                           "fn c(): char => '\\u{0}'\n";
        EmitResult f1 = format(fsrc);
        EmitResult f2 = format(f1.code);
        check(f1.ok && f2.ok && f1.code == f2.code, "P40 s3: fmt is idempotent over escapes");
        check(has(f1.code, "\"a\\${b} \\u{1D}\\0\\u{2028} \xC3\xA9 $ ${x}\"") && has(f1.code, "'\\0'"),
              "P40 s3: fmt prints \\${, \\0, \\u{H} for non-printables and raw printable non-ASCII");
        auto lits = [](const std::string& src) {
            DiagnosticBag d;
            std::vector<std::string> out;
            for (const auto& t : lex(src, d))
                if (t.kind == TokKind::InterpStart || t.kind == TokKind::InterpEnd || t.kind == TokKind::StringLit ||
                    t.kind == TokKind::CharLit)
                    out.push_back(t.text);
            return out;
        };
        check(lits(fsrc) == lits(f1.code), "P40 s3: fmt output re-lexes to the same decoded literal values");

        // The Core JSON reader: an escape JSON does not define is an error, never a silently dropped backslash.
        std::string jerr;
        json::Value jv = json::parse("{\"lib\":\"Libs\\Alpha\"}", jerr);
        check(has(jerr, "invalid escape '\\A'") && jv["lib"].asString() == "Libs\\Alpha",
              "P40 s3: JSON `\"Libs\\Alpha\"` is a parse error (and never reads as LibsAlpha)");
        jerr.clear();
        json::parse("{\"a\":\"x\\\\y\\u0041\\n\"}", jerr);
        check(jerr.empty(), "P40 s3: valid JSON escapes report no error");
        jerr.clear();
        json::parse("{\"a\":\"\\u00G1\"}", jerr);
        check(has(jerr, "hex digits"), "P40 s3: a malformed JSON \\u escape is an error");

        // P40 review (A5): STRUCTURAL malformations are errors too — `{` used to parse as an empty object with no
        // error, so a truncated pgconfig.json was silently ignored and the build exited 0.
        bool allRefused = true;
        for (const char* bad : {"{", "", "   ", "{\"a\":1", "{\"a\" 1}", "{\"a\":1,}", "[1,]", "[1 2]", "{\"a\":tru}",
                                "{\"a\":01}", "{\"a\":1.}", "{\"a\":+1}", "{\"a\":\"x}", "{} x", "{\"a\":1}}", "{a:1}"}) {
            jerr.clear();
            json::parse(bad, jerr);
            if (jerr.empty()) { allRefused = false; std::cerr << "    not refused: '" << bad << "'\n"; }
        }
        check(allRefused, "P40 review A5: structurally malformed JSON reports an error (truncation, separators, literals, numbers, trailing content)");
        bool allAccepted = true;
        for (const char* good : {"{}", "[]", " {\"a\": [1, -2.5e+3, 0, true, false, null, \"s\"], \"b\": {}} \n",
                                 "\xEF\xBB\xBF{\"targets\": [\"csharp\"]}", "0", "\"x\""}) {
            jerr.clear();
            json::parse(good, jerr);
            if (!jerr.empty()) { allAccepted = false; std::cerr << "    refused: '" << good << "': " << jerr << "\n"; }
        }
        check(allAccepted, "P40 review A5: well-formed JSON (incl. a UTF-8 BOM) reports no error");
        jerr.clear();
        check(json::parse("\xEF\xBB\xBF{\"targets\": [\"csharp\"]}", jerr)["targets"].items().size() == 1,
              "P40 review A5: a BOM-prefixed document parses to its value (it used to read as a number 0)");
    }

    // P40 slice 4 (A2) — Rust-style range precedence, evaluate-once bounds, the super refusal, the parser
    // progress guard, and the lowering backstop.
    {
        auto parseSrc = [](const std::string& src, DiagnosticBag& d) { return parse(lex(src, d), d); };
        auto firstErr = [](const EmitResult& r) { return r.diagnostics.empty() ? std::string() : r.diagnostics[0].message; };

        // Parse shapes: the range is the loosest binary form, so every binary bound stays inside it.
        auto iterOf = [&](const std::string& bound, DiagnosticBag& d) -> CompilationUnit {
            return parseSrc("fn f(n: i32, a: i32?) {\n  for i in " + bound + " { }\n}\n", d);
        };
        struct Shape { const char* src; const char* lhs; const char* rhs; bool inclusive; };
        const Shape shapes[] = {
            {"0..n - 1", "IntLit", "-", false}, {"-2..2", "-", "IntLit", false},
            {"0..=n * 2", "IntLit", "*", true}, {"i + 1..n", "+", "Name", false},
            {"0..a ?? 3", "IntLit", "??", false},
        };
        for (const auto& sh : shapes) {
            DiagnosticBag d;
            CompilationUnit u = iterOf(sh.src, d);
            const Expr* r = (!d.hasErrors() && !u.functions.empty() && !u.functions[0].body.empty())
                                ? u.functions[0].body[0]->value.get() : nullptr;
            auto tag = [](const Expr& e) -> std::string {
                if (e.kind == ExprKind::Binary || e.kind == ExprKind::Unary) return e.text;
                return e.kind == ExprKind::IntLit ? "IntLit" : e.kind == ExprKind::Name ? "Name" : "?";
            };
            check(r && r->kind == ExprKind::Range && tag(*r->lhs) == sh.lhs && tag(*r->rhs) == sh.rhs &&
                      r->flag == sh.inclusive,
                  std::string("P40 s4: `") + sh.src + "` parses as one range with binary bounds");
        }
        {
            DiagnosticBag d;
            parseSrc("fn f() {\n  for i in 0..1..2 { }\n}\n", d);
            check(d.hasErrors() && has(d.items()[0].message, "ranges do not chain"), "P40 s4: ranges do not chain");
        }

        // S8: the throw arm used to hang the parser forever; now one targeted error (and the parse ends).
        {
            DiagnosticBag d;
            parseSrc("union O { Ok(v: i32), Fail }\nfn f(o: O): i32 => match o {\n  Ok(v) => v,\n  Fail => throw Error(\"x\")\n}\n", d);
            check(d.items().size() == 1 && has(d.items()[0].message, "'throw' is a statement"),
                  "P40 s4: a throw match arm is one diagnostic, not a parser hang");
            DiagnosticBag d2;
            parseSrc("enum E { 1 }\nunion U { 2 }\n", d2);
            check(d2.hasErrors(), "P40 s4: enum/union case loops have a progress guard (no hang)");
        }

        // The formatter stops parenthesizing binary bounds, and stays idempotent.
        {
            const std::string src = "fn f(n: i32, a: i32?) {\n  for j in (n + 1)..n { }\n  for i in (0 - 2)..=(n * 2) { }\n"
                                    "  for k in 0..(a ?? 5) { }\n}\n";
            EmitResult f1 = format(src);
            EmitResult f2 = format(f1.code);
            check(f1.ok && has(f1.code, "for j in n + 1..n") && has(f1.code, "for i in 0 - 2..=n * 2") &&
                      has(f1.code, "for k in 0..a ?? 5"),
                  "P40 s4: fmt prints range bounds without parentheses");
            check(f2.ok && f1.code == f2.code, "P40 s4: fmt is idempotent over ranges");
        }

        // Sema: a range outside a for iterable, float bounds, incompatible bounds, and the super refusal.
        EmitResult op = compile("fn f(n: i32): i32 => (0..n) - 1\n", findTarget("csharp"));
        check(!op.ok && op.diagnostics.size() == 1 && has(firstErr(op), "a range ('a..b') is only valid as a for-loop iterable"),
              "P40 s4: a range operand is one error (no cascade)");
        EmitResult lt = compile("fn f() {\n  let r = 0..5\n}\n", findTarget("csharp"));
        check(!lt.ok && has(firstErr(lt), "only valid as a for-loop iterable"), "P40 s4: a range in a let is an error");
        EmitResult fl = compile("fn f() {\n  var n = 0\n  for x in 1.5..3 { n += 1 }\n}\n", findTarget("csharp"));
        check(!fl.ok && has(firstErr(fl), "range bounds must be integers (got f64)"), "P40 s4: float range bounds are an error");
        EmitResult mix = compile("fn f(a: i64, b: u64) {\n  for i in a..b { }\n}\n", findTarget("csharp"));
        check(!mix.ok && has(firstErr(mix), "incompatible types i64 and u64"), "P40 s4: an incompatible bound pair is an error");
        EmitResult sm = compile("open class A {\n  open fn g(): i32 => 1\n}\nclass B : A {\n  override fn g(): i32 => super.g() + 1\n}\n",
                                findTarget("csharp"));
        check(!sm.ok && has(firstErr(sm), "'super' is only supported as a base-constructor call"), "P40 s4: super.m() is refused (D6)");
        EmitResult sc = compileStd("class E2 : Error {\n  constructor() {\n    super(\"x\")\n  }\n}\n", findTarget("csharp"));
        check(sc.ok, "P40 s4: the hoisted base-constructor super(...) stays legal");

        // Widening + D7: the i32 start widens to the i64 end; a non-simple end is hoisted with the spelled type.
        const char* wide = "fn f(a: i64, b: i64): i64 {\n  var s: i64 = 0\n  for k in 0..a + b { s += k }\n"
                           "  for j in 0..a { s += j }\n  return s\n}\n";
        EmitResult wcs = compile(wide, findTarget("csharp"));
        check(wcs.ok && has(wcs.code, "for (long k = ") && has(wcs.code, ", __end0 = a + b; k < __end0; k++)"),
              "P40 s4: C# hoists a non-simple end into a multi-declarator with the widened (long) type");
        check(wcs.ok && has(wcs.code, "for (var j = ") && has(wcs.code, "; j < a; j++)"),
              "P40 s4: a parameter end stays readable (re-read, not hoisted)");
        EmitResult wts = compile(wide, findTarget("typescript"));
        check(wts.ok && has(wts.code, "for (let k = BigInt(0), __end0 = BigInt.asIntN(64, a + b); k < __end0; k++)"),
              "P40 s4: TS hoists with `let` (same shape)");
        EmitResult wphp = compile(wide, findTarget("php"));
        check(wphp.ok && has(wphp.code, ", $__end0 = $a + $b; $k < $__end0; $k++)"), "P40 s4: PHP hoists with a comma init");
        const char* mut = "fn f(): i32 {\n  var m = 3\n  var c = 0\n  for i in 0..m { m += 1\n c += 1 }\n  return c\n}\n";
        EmitResult mcs = compile(mut, findTarget("csharp"));
        check(mcs.ok && has(mcs.code, "__end0 = m;"), "P40 s4: a mutable-local end is hoisted (evaluated once)");

        // The backstop: lowering records an expression it has no rule for (sema refuses these first, so the
        // AST is fed to lower() directly), and compile() would turn the record into an ICE.
        {
            DiagnosticBag d;
            CompilationUnit u = parseSrc("fn f() {\n  let r = 0..5\n  let s = super\n}\n", d);
            ir::Module m = lower(u, "csharp");
            check(m.loweringFailures.size() == 2 && m.loweringFailures[0].kind == "range" &&
                      m.loweringFailures[1].kind == "super" && m.loweringFailures[0].pos.line == 2,
                  "P40 s4: lowering records each unlowerable expression with its kind and position");
            DiagnosticBag d2;
            ir::Module ok = lower(parseSrc("fn f(n: i32) {\n  for i in 0..n { }\n}\n", d2), "csharp");
            check(ok.loweringFailures.empty(), "P40 s4: a for-iterable range lowers without a failure");
        }
    }

    // P40 slice 5 (A4) — the checked null-assert, the nullable operand rules (N1–N3 + the S7 holes), null
    // flow-narrowing (D14), `opt!` (D12), PHP `\Throwable` (D13) and explicit call type args.
    {
        auto firstErr = [](const EmitResult& r) { return r.diagnostics.empty() ? std::string() : r.diagnostics[0].message; };
        auto irOf = [](const char* src, bool& ok) {
            DiagnosticBag d;
            auto unit = parse(lex(src, d), d);
            mintplayer::polyglot::check(unit, d);
            ok = !d.hasErrors();
            return ir::dump(lower(unit, "csharp"));
        };
        auto typeErr = [&](const char* src) { // the first diagnostic of a csharp compile ("" = it compiled)
            EmitResult r = compileStd(src, findTarget("csharp"));
            return r.ok ? std::string() : firstErr(r);
        };
        bool ok = false;
        std::string i1 = irOf("fn f(n: i32?): i32 => n!\n", ok);
        check(ok && has(i1, "n:i32?!"), "P40 s5: `!` on a nullable operand lowers to ir::NullAssert");
        std::string i2 = irOf("fn f(n: i32): i32 => n!\n", ok);
        check(ok && !has(i2, "!") && !has(i2, "(i32)"), "P40 s5: `!` on a non-nullable operand is the bare operand");
        std::string i3 = irOf("fn f(n: i32?): i32 {\n  if n != null { return n + 1 }\n  return 0\n}\n", ok);
        check(ok && has(i3, "(i32)n:i32?"), "P40 s5: a narrowed read converts the nullable variable (no NullAssert)");
        std::string i4 = irOf("fn f(n: i32?): i32 {\n  if n != null { return n! }\n  return 0\n}\n", ok);
        check(ok && !has(i4, "!:"), "P40 s5: `!` on a narrowed variable lowers to the bare (converted) operand");

        // Emission: every target, the Core-owned site, helpers only when reached.
        EmitResult ecs = compileStd("fn f(n: i32?): i32 => n!\n", findTarget("csharp"));
        check(ecs.ok && has(ecs.code, "(n ?? throw new global::System.NullReferenceException(\"null-assert failed (<entry>:1:24)\"))"),
              "P40 s5: C# emits `(x ?? throw new NullReferenceException(\"null-assert failed (m.pg:L:C)\"))`");
        EmitResult ets = compileStd("fn f(n: i32?): i32 => n!\n", findTarget("typescript"));
        check(ets.ok && has(ets.code, "(n ?? __pgNullAssert(\"") && has(ets.code, "\nfunction __pgNullAssert(at: string): never {") &&
                  !has(ets.code, "export function __pgNullAssert"),
              "P40 s5: TS emits `x ?? __pgNullAssert(site)` with a module-local never-helper");
        EmitResult ets0 = compileStd("fn f(n: i32): i32 => n + 1\n", findTarget("typescript"));
        check(ets0.ok && !has(ets0.code, "__pgNullAssert"), "P40 s5: the TS helper is emitted only when a `!` reaches it");
        EmitResult epy = compileStd("fn f(n: i32?): i32 => n!\nfn g(xs: List<i32?>): i32 => xs[0]!\n", findTarget("python"));
        check(epy.ok && has(epy.code, "(n if n is not None else _pg_fail(\"") && has(epy.code, ":= xs[0]) is not None else _pg_fail(") &&
                  has(epy.code, "def _pg_fail(at):"),
              "P40 s5: Python inlines the check — a name directly, any other operand through a walrus (S9)");
        EmitResult ephp = compileStd("fn f(n: i32?): i32 => n!\n", findTarget("php"));
        check(ephp.ok && has(ephp.code, "__pg_nn($n, \"") && has(ephp.code, "function_exists(__NAMESPACE__.'\\\\__pg_nn')") &&
                  has(ephp.code, "throw new \\UnexpectedValueException("),
              "P40 s5: PHP calls the namespace-guarded `__pg_nn` helper");
        EmitResult eopt = compileStd("fn pick<T>(c: bool, v: T): T? => if c { v } else { null }\nfn f(): i32 => pick(true, 1)!\n",
                                     findTarget("csharp"));
        check(eopt.ok && has(eopt.code, "None<int> _ => throw new global::System.NullReferenceException(\"null-assert failed ("),
              "P40 s5: `opt!` on Option<T> lowers to a match whose None arm throws (D12)");
        EmitResult eta = compileStd("fn failT<T>(m: string): T {\n  throw Error(m)\n}\nfn g(): i32 => failT<i32>(\"x\")\n",
                                    findTarget("csharp"));
        check(eta.ok && has(eta.code, "failT<int>(\"x\")"), "P40 s5: C# keeps explicit call-site type arguments (no CS0411)");
        EmitResult ecatch = compileStd("fn g() {}\nfn f() {\n  try {\n    g()\n  } catch (e: Error) {\n    g()\n  }\n}\n", findTarget("php"));
        check(ecatch.ok && has(ecatch.code, "catch (\\Throwable $__e)"), "P40 s5: PHP `catch (e: Error)` widens to \\Throwable (D13)");

        // N1–N3 and the S7 holes.
        check(has(typeErr("fn f(n: string?): string => \"a\" + n\n"), "'+' operand of type 'string?' may be null; supply a fallback with '??' or assert with '!'"),
              "P40 s5: N1 refuses a nullable '+' operand with the PRD wording");
        check(typeErr("fn f(n: i32?): bool => n == null || n != 3\n").empty(), "P40 s5: N1 exempts ==, != (and narrows `||`'s right side)");
        check(has(typeErr("fn f(n: i32?): string => \"${n}\"\n"), "interpolation hole of type 'i32?' may be null"), "P40 s5: N2 refuses a nullable interpolation hole");
        check(has(typeErr("fn f(n: i32?) { print(n) }\n"), "print argument of type 'i32?' may be null"), "P40 s5: N2 refuses a nullable print argument");
        check(has(typeErr("fn f<T>(x: T): string => \"v\" + x\n"), "'string + T' concatenates a type parameter"), "P40 s5: N3 refuses string + T");
        check(has(typeErr("fn f(): i32 {\n  var n: i32? = 1\n  n += 1\n  return 0\n}\n"), "'+=' target 'n' of type 'i32?' may be null"),
              "P40 s5: a compound assignment on a nullable target is refused");
        check(has(typeErr("fn f(b: bool?): bool {\n  if b && true { return true }\n  return false\n}\n"), "of type 'bool?' may be null"),
              "P40 s5: a bool? operand of && in a condition is refused");
        check(has(typeErr("fn f(b: bool?): i32 {\n  while b { }\n  return 0\n}\n"), "'while' condition of type 'bool?' may be null"),
              "P40 s5: requireBool refuses a bool? condition");
        check(has(typeErr("fn f(c: bool): i32 => (if c { 1 } else { null }) + 1\n"), "'+' operand of type 'i32?'"),
              "P40 s5: an if-expression with a null branch is typed nullable");
        check(has(typeErr("class B {\n  var v: i32\n  constructor(v: i32) { this.v = v }\n  fn get(): i32 => this.v\n}\nfn f(b: B?): i32 => b.get()\n"),
                  "'.get' on a receiver of type 'B?', which may be null"),
              "P40 s5: a method call on a nullable receiver is refused");
        check(typeErr("class B {\n  var v: i32\n  constructor(v: i32) { this.v = v }\n}\nfn f(b: B?): i32 => b?.v ?? 0\n").empty(),
              "P40 s5: '?.' on a nullable receiver stays legal");

        // Narrowing scopes (D14).
        check(typeErr("fn f(n: i32?): i32 {\n  if n == null { return 0 }\n  return n + 1\n}\n").empty(), "P40 s5: an early exit narrows the rest of the block");
        check(typeErr("fn f(n: i32?): i32 {\n  if n == null { return 1 } else { return n + 1 }\n}\n").empty(), "P40 s5: `== null` narrows the else-branch");
        check(typeErr("fn f(n: i32?): bool => n != null && n > 0\n").empty(), "P40 s5: `x != null && …` narrows the right operand");
        check(typeErr("fn f(n: i32?): i32 => if n != null { n * 2 } else { 0 }\n").empty(), "P40 s5: an if-expression narrows its branch");
        check(!typeErr("fn f(n: i32?, c: bool): i32 {\n  if c {\n    if n == null { return 0 }\n  }\n  return n + 1\n}\n").empty(),
              "P40 s5: an early exit's narrowing ends with its own block");
        check(!typeErr("fn f(): i32 {\n  var n: i32? = 1\n  if n != null {\n    n = null\n    return n + 1\n  }\n  return 0\n}\n").empty(),
              "P40 s5: narrowing ends at an assignment");
        check(!typeErr("fn f(): i32 {\n  var n: i32? = 1\n  if n != null {\n    if n != null { n = null }\n    return n + 1\n  }\n  return 0\n}\n").empty(),
              "P40 s5: an assignment inside a nested narrowed region is not undone at its exit");
        check(!typeErr("fn f(): i32 {\n  var n: i32? = 1\n  if n != null {\n    while true {\n      let k = n + 1\n      n = null\n    }\n  }\n  return 0\n}\n").empty(),
              "P40 s5: a loop that assigns the variable is not narrowed anywhere in it (back edge)");
        check(typeErr("class Nd {\n  var v: i32\n  var next: Nd?\n  constructor(v: i32) {\n    this.v = v\n    this.next = null\n  }\n}\n"
                      "fn sum(h: Nd?): i32 {\n  var s = 0\n  var c: Nd? = h\n  while c != null {\n    s += c.v\n    c = c.next\n  }\n  return s\n}\n").empty(),
              "P40 s5: a while condition narrows the body up to the assignment");
        check(!typeErr("fn f(n: i32?): i32 {\n  if n != null {\n    let g = () => n + 1\n    return g()\n  }\n  return 0\n}\n").empty(),
              "P40 s5: narrowing does not reach into a lambda");
        check(!typeErr("fn f(): i32 {\n  var n: i32? = 1\n  let clear = () => { n = null }\n  if n != null {\n    clear()\n    return n + 1\n  }\n  return 0\n}\n").empty(),
              "P40 s5: a mutable local a closure assigns is never narrowed");

        // ---- P40 slice 6 (A3 + A6): one stringification rule, culture-free C# ----
        {
            auto cstr = [](const std::string& src, const BackendHandle& t) {
                return compile(src, t, nullptr, LibConfig{{"io", "math", "collections", "strings"}});
            };
            const std::string holes =
                "fn f<T>(n: i32, d: f64, s: string, c: bool, ch: char, t: T): string => "
                "\"${n}|${d}|${s}|${c}|${ch}|${t}|${if c { \"a:b\" } else { s }}|${if c { 1 } else { 2 }}\"\n";
            EmitResult cs = compileStd(holes, findTarget("csharp"));
            check(cs.ok && has(cs.code, "{__PgRt.Str(n)}|{__PgRt.Str(d)}|{s}|{(c ? \"true\" : \"false\")}|{ch}|{__PgRt.Show(t)}"),
                  "P40 s6: C# holes — int/float via __PgRt.Str, string/char bare, bool spelled, T via __PgRt.Show");
            check(cs.ok && has(cs.code, "{((c ? \"a:b\" : s))}") && has(cs.code, "{__PgRt.Str((c ? 1 : 2))}"),
                  "P40 s6: A6 — a string hole holding ':' is parenthesized (parenIfContains); a Str(...) call needs none");
            check(cs.ok && has(cs.code, "file static class __PgRt") && has(cs.code, "internal static string Str(double x)") &&
                      has(cs.code, "global::System.Numerics.BigInteger") && !has(cs.code, "ToString(\"R\""),
                  "P40 s6: C# Str(double) is the exact Burger–Dybvig helper, never \"R\"");
            EmitResult onlyInt = compileStd("fn f(n: i32): string => \"${n}\"\n", findTarget("csharp"));
            check(onlyInt.ok && has(onlyInt.code, "internal static string Str(int x)") &&
                      !has(onlyInt.code, "Str(double x)") && !has(onlyInt.code, "Show("),
                  "P40 s6: an int-only hole reaches only the integer helpers");
            EmitResult lit = compileStd("fn f(): f32 => 0.1f\nfn g(): f32 => 2.5f\n", findTarget("csharp"));
            check(lit.ok && has(lit.code, "return 0.1f;") && has(lit.code, "return 2.5f;"),
                  "P40 s6: the C# f32 literal keeps its F suffix (S13: was CS0664)");
            EmitResult tlit = compileStd("fn f(): f32 => 0.1f\nfn g(): f32 => 2.5f\nfn h(): f64 => 0.1\n", findTarget("typescript"));
            check(tlit.ok && has(tlit.code, "return 0.10000000149011612;") && has(tlit.code, "return 2.5;") &&
                      has(tlit.code, "return 0.1;"),
                  "P40 s6: a double-holding target gets the f32 literal's widened value (exact ones keep their text)");
            EmitResult plit = compileStd("fn f(): f32 => 16777217.0f\n", findTarget("python"));
            check(plit.ok && has(plit.code, "return 16777216.0"), "P40 s6: a widened integral f32 stays a float literal");
            EmitResult parse = cstr("fn f(s: string): i32 => i32.parse(s) + s.toI32()\nfn g(s: string): f64 => f64.parse(s)\n",
                                          findTarget("csharp"));
            check(parse.ok && has(parse.code, "int.Parse(s, global::System.Globalization.NumberStyles.Integer, global::System.Globalization.CultureInfo.InvariantCulture)") &&
                      has(parse.code, "double.Parse(s, global::System.Globalization.NumberStyles.Float, global::System.Globalization.CultureInfo.InvariantCulture)") &&
                      !has(parse.code, "AllowLeadingSign"),
                  "P40 s6: C# parse/toI32 are NumberStyles.Integer/Float + InvariantCulture");
            EmitResult tsI32 = cstr("fn f(s: string): i32 => s.toI32()\n", findTarget("typescript"));
            check(tsI32.ok && has(tsI32.code, "(Number.parseInt(s, 10) | 0)"), "P40 s6: TS toI32 truncates to i32, like i32.parse");
            EmitResult py = compileStd("fn main() { print(1.5) }\n", findTarget("python"));
            check(py.ok && has(py.code, "__import__('builtins').print(_pg_out16(_pg_str(x)))") && !has(py.code, "__builtins__"),
                  "P40 s6: the Python print helper works in an imported module (no __builtins__.print)");
            EmitResult php = compileStd("fn f(n: i32, d: f64, s: string): string => \"${n}${d}${s}\"\nfn main() { print(1.5) }\n",
                                        findTarget("php"));
            check(php.ok && has(php.code, " . ($n) . ") && has(php.code, " . __pg_str($d) . ") && has(php.code, " . ($s) . ") &&
                      has(php.code, "echo __pg_str($x)") && has(php.code, "ini_set('serialize_precision', '-1')") &&
                      has(php.code, "function_exists(__NAMESPACE__.'\\\\__pg_str')"),
                  "P40 s6: PHP floats print through the namespace-guarded __pg_str (immune to precision ini)");
            const std::string asc = "fn f(s: string): string => s.toUpperAscii() + s.toLowerAscii()\n";
            EmitResult acs = cstr(asc, findTarget("csharp"));
            check(acs.ok && has(acs.code, "__PgRt.UpperAscii(s) + __PgRt.LowerAscii(s)") && has(acs.code, "UpperAscii(string s)"),
                  "P40 s6: C# toUpperAscii/toLowerAscii are an __PgRt loop");
            check(has(cstr(asc, findTarget("typescript")).code, "s.replace(/[a-z]+/g, (__m) => __m.toUpperCase())"),
                  "P40 s6: TS toUpperAscii is a regex replace");
            check(has(cstr(asc, findTarget("python")).code, "s.translate(_PG_UPPER)"), "P40 s6: Python toUpperAscii is translate");
            check(has(cstr(asc, findTarget("php")).code, "strtr($s, 'abcdefghijklmnopqrstuvwxyz', 'ABCDEFGHIJKLMNOPQRSTUVWXYZ')"),
                  "P40 s6: PHP toUpperAscii is strtr");
            check(has(cstr("fn f(s: string): string => s.toUpper()\n", findTarget("csharp")).code, "s.ToUpperInvariant()"),
                  "P40 s6: C# toUpper is ToUpperInvariant");

            // The printable guard: a hole accepts exactly what print accepts; enums are refused (D1).
            check(has(typeErr("record P(x: i32)\nfn f(p: P): string => \"${p}\"\n"), "cannot interpolate a value of type 'P'"),
                  "P40 s6: a record in a hole is refused");
            check(has(typeErr("fn f(xs: List<i32>): string => \"${xs}\"\n"), "cannot interpolate a value of type 'List<i32>'"),
                  "P40 s6: a list in a hole is refused");
            check(has(typeErr("enum C { A, B }\nfn f(c: C): string => \"${c}\"\n"), "an enum has no portable text"),
                  "P40 s6: an enum in a hole is refused (D1)");
            check(has(typeErr("enum C { A, B }\nfn f(c: C) { print(c) }\n"), "print cannot print a value of type 'C'"),
                  "P40 s6: print of an enum is refused (D1)");
            check(has(typeErr("record P(x: i32)\nfn f(p: P) { print(p) }\n"), "print cannot print a value of type 'P'"),
                  "P40 s6: print of a record is refused (it used to slip through as an 'unknown' scalar)");
            check(typeErr("fn f<T>(t: T, c: char, b: u8, x: f32): string => \"${t}${c}${b}${x}\"\n").empty(),
                  "P40 s6: type parameters, char and every numeric scalar stay printable");

            // TS (D10 follow-ups): `as const` tags; `override` on a field shadowing the native root's member.
            const std::string err2 = "class A : Error {\n  constructor(m: string) { super(m) }\n}\n"
                                     "class B : A {\n  let cause: string\n  let extra: string\n"
                                     "  constructor(m: string) {\n    super(m)\n    this.cause = m\n    this.extra = m\n  }\n}\n"
                                     "class C {\n  let cause: string\n  constructor(m: string) { this.cause = m }\n}\n";
            EmitResult ets = compileStd(err2, findTarget("typescript"));
            check(ets.ok && has(ets.code, "override cause: string;") && !has(ets.code, "override extra") &&
                      ets.code.find("override cause") == ets.code.rfind("override cause"),
                  "P40 s6: TS `override` on a field shadowing Error.cause through a user base — only there");
        }

        // ---- P40 slice 7: generics prerequisites (generic member typing, class bounds, extension defaults, $tmp) ----
        {
            const std::string cell =
                "class Cell<V> {\n  var v: V\n  constructor(v: V) { this.v = v }\n  fn first(): V => this.v\n"
                "  fn put(n: V) { this.v = n }\n  fn apply(f: (V) => V): V => f(this.v)\n}\n";
            const std::string box =
                "class Box<T> {\n  var item: T\n  constructor(item: T) { this.item = item }\n  fn get(): T? => this.item\n}\n";
            // Substitution from the receiver: a method result, a field, a lambda param.
            const std::string concat = cell + "fn f(c: Cell<string>): string => c.first() + c.v + c.apply(s => s + \"?\")\n";
            EmitResult ts = compileStd(concat, findTarget("typescript"));
            check(ts.ok && has(ts.code, "c.first() + c.v + c.apply((s: string) => s + \"?\")") && !has(ts.code, "V.plus"),
                  "P40 s7: Cell<string>.first()/.v/lambda param type as string on TS (was `V.plus(f, \"!\")`)");
            EmitResult php = compileStd(concat, findTarget("php"));
            check(php.ok && has(php.code, "$c->first() . $c->v . ") && has(php.code, "$s . \"?\""),
                  "P40 s7: PHP concatenates a substituted string with `.` (was `$f + \"!\"`, a TypeError)");
            EmitResult arith = compileStd(cell + "fn f(c: Cell<i32>): i32 => c.first() * 2 + c.v\n", findTarget("typescript"));
            check(arith.ok && has(arith.code, "Math.imul(c.first(), 2)") && !has(arith.code, "V.times"),
                  "P40 s7: arithmetic on a substituted i32 uses the i32 rules (Math.imul), not a user operator");
            // `T?` of a user generic is an Option of the ARGUMENT after substitution (the body's representation).
            EmitResult opt = compileStd(box + "fn f(b: Box<i32>): i32 => b.get() ?? 0\n", findTarget("csharp"));
            check(opt.ok && has(opt.code, "Some<int>(var") && has(opt.code, "None<int> _"),
                  "P40 s7: Box<i32>.get(): T? is Option<i32> at the call (was Option<T>, CS0246)");
            // Inferred construction type args are recorded, so C# (no ctor inference) spells them.
            EmitResult inf = compileStd(cell + "fn f(): i32 {\n  let c = Cell(3)\n  return c.first() + 1\n}\n", findTarget("csharp"));
            check(inf.ok && has(inf.code, "new Cell<int>(3)"), "P40 s7: `Cell(3)` infers Cell<i32> and C# spells `new Cell<int>(3)`");
            // Params are substituted too: a wrong argument to an instantiated method is a Polyglot error.
            check(has(typeErr((cell + "fn f(c: Cell<i32>) { c.put(\"s\") }\n").c_str()), "argument 1 of method 'put'"),
                  "P40 s7: Cell<i32>.put(\"s\") is refused (the param is i32 after substitution)");
            check(has(typeErr((box + "fn f() { let b = Box<i32>(\"s\") }\n").c_str()), "argument 1 of 'Box'"),
                  "P40 s7: construction args are checked against the instantiated ctor params");
            // Through a generic base.
            const std::string inh = "open class H<T> {\n  var held: T\n  constructor(held: T) { this.held = held }\n"
                                    "  fn take(): T => this.held\n}\nclass D<U> : H<List<U>> {\n"
                                    "  constructor(h: List<U>) { super(h) }\n}\nfn f(d: D<string>): i32 => d.take().count + 1\n";
            EmitResult ih = compileStd(inh, findTarget("typescript"));
            check(ih.ok && has(ih.code, "(d.take().length + 1 | 0)"),
                  "P40 s7: a member inherited through `D<U> : H<List<U>>` substitutes T = List<string>");
            // Extern generic: `V?` is a native nullable once V is concrete; refused while V is a type parameter.
            const std::string shelf =
                "extern class Shelf<V> {\n  type { actual(csharp) extern(\"global::System.Collections.Generic.Dictionary<string, $0>\") }\n"
                "  fn find(k: string): V? { actual(csharp) extern(\"($this.TryGetValue($0, out var $tmp) ? $tmp : ($T)null)\") }\n"
                "  fn at(i: i32 = 7): V { actual(csharp) extern(\"$this.ElementAt($0).Value\") }\n}\n";
            EmitResult sh = compileStd(shelf + "fn f(s: Shelf<i32>): i32 => (s.find(\"a\") ?? 0) + (s.find(\"b\") ?? 0)\n",
                                       findTarget("csharp"));
            check(sh.ok && has(sh.code, "(s.TryGetValue(\"a\", out var __pg_t1) ? __pg_t1 : (int?)null)") &&
                      has(sh.code, "(s.TryGetValue(\"b\", out var __pg_t2) ? __pg_t2 : (int?)null)"),
                  "P40 s7: extern V? -> native int?; `$tmp` is one fresh name per expansion (no CS0128/CS0136)");
            check(has(typeErr((shelf + "fn g<V>(s: Shelf<V>): bool => s.find(\"a\") == null\n").c_str()),
                      "'Shelf.find' yields 'V?' while 'V' is still a type parameter"),
                  "P40 s7: an extern `V?` read with V unresolved is refused (no faithful native form)");
            // ---- P40: the T?-representation boundary (SPEC §4.4) — Option<X> <-> native X? converts in every position ----
            {
                const std::string slot = box + "class Slot<T> {\n  fn take(x: T?): bool => match x { Some(_) => true, None => false }\n}\n"
                                               "fn echo<T>(x: T?): T? => x\n";
                EmitResult letCs = compileStd(slot + "fn f(b: Box<string>): string? {\n  let n: string? = b.get()\n  return n\n}\n",
                                              findTarget("csharp"));
                check(letCs.ok && has(letCs.code, "string? n = b.get() switch { Some<string>(var") &&
                          has(letCs.code, "=> (string?)(__opt") && has(letCs.code, "None<string> _ => null"),
                      "P40 boundary: `let n: string? = box.get()` unwraps the Option (was CS0029 Option<string> -> string?)");
                EmitResult letTs = compileStd(slot + "fn f(b: Box<string>): string? {\n  let n: string? = b.get()\n  return n\n}\n",
                                              findTarget("typescript"));
                check(letTs.ok && !has(letTs.code, "const n: (string | null) = b.get();") && has(letTs.code, "_m.tag === \"Some\""),
                      "P40 boundary: TS no longer stores the {tag:\"Some\"} object in a string local ([object Object])");
                EmitResult ret = compileStd(slot + "fn f(b: Box<i32>): i32? => b.get()\n", findTarget("csharp"));
                check(ret.ok && has(ret.code, "=> (int?)(__opt"), "P40 boundary: a return into a native i32? converts");
                EmitResult rev = compileStd(slot + "fn f(s: Slot<string>, n: string?): bool => s.take(n)\n", findTarget("csharp"));
                check(rev.ok && has(rev.code, "n switch { null => (Option<string>)(new None<string>()), var __opt") &&
                          !has(rev.code, "new Some<string>(n)"),
                      "P40 boundary: a native string? into a T? param is None for null, never Some(null)");
                EmitResult tinf = compileStd(slot + "fn f(n: string?): string? => echo(n)\n", findTarget("csharp"));
                check(tinf.ok && has(tinf.code, "new Some<string>((string)(") && !has(tinf.code, "new None<T>()"),
                      "P40 boundary: echo(n: string?) binds T = string (not string?) and converts at the call");
                EmitResult eq = compileStd(slot + "fn f(b: Box<string>): bool => b.get() == null\n", findTarget("typescript"));
                check(eq.ok && !has(eq.code, "b.get() === null"),
                      "P40 boundary: `box.get() == null` is an absence test (was a constant-false reference test)");
                EmitResult nar = compileStd(slot + "fn f(b: Box<string>): string {\n  let o = b.get()\n  if o != null { return o }\n  return \"\"\n}\n",
                                            findTarget("csharp"));
                check(nar.ok && has(nar.code, "return o switch { Some<string>(var"),
                      "P40 boundary: an Option local narrowed by `!= null` reads as the unwrapped element");
                EmitResult fld = compileStd(slot + "class K {\n  var item: string?\n  constructor(b: Box<string>) { this.item = b.get() }\n}\n",
                                            findTarget("csharp"));
                check(fld.ok && has(fld.code, "this.item = b.get() switch"),
                      "P40 boundary: a field assignment converts too (member targets skipped checkConvert)");
                EmitResult arr = compileStd("fn first<T>(xs: T[]): T? => if xs.count > 0 { xs[0] } else { null }\n"
                                            "fn f(): i32 => first([4, 5]) ?? 0\n", findTarget("csharp"));
                check(arr.ok && has(arr.code, "new int[] { 4, 5 }"),
                      "P40 boundary: a generic call's args convert into the INSTANTIATED params (was `new T[]`)");
                check(has(typeErr((slot + "fn f(b: Box<string>) { let x: string = b.get() }\n").c_str()),
                          "cannot convert 'Option<string>' to 'string'"),
                      "P40 boundary: an Option into a non-nullable slot is refused (it may be absent)");
                check(has(typeErr((slot + "fn f(b: Box<string?>): string? => b.get()\n").c_str()), "its element is itself nullable"),
                      "P40 boundary: Option<string?> -> string? is refused (Some(null) and None would merge)");
                const std::string nullPat = "fn f(z: i32?): string => match z { null => \"n\", _ => \"v\" }\n";
                EmitResult tphp = compileStd(nullPat, findTarget("php"));
                check(tphp.ok && has(tphp.code, "$_m === null"), "P40 boundary: PHP spells a null pattern `=== null` (`0 == null` is true)");
                EmitResult py = compileStd(nullPat, findTarget("python"));
                check(py.ok && has(py.code, "_m is None"), "P40 boundary: Python spells a null pattern `is None`");
            }
            EmitResult bd = compileStd(shelf + "fn f(s: Shelf<i32>): i32 => s.at()\n", findTarget("csharp"));
            check(bd.ok && has(bd.code, "s.ElementAt(7).Value"), "P40 s7: a bound member's omitted default fills its `$0`");
            // Class type-parameter bounds (SP-B3-S3), explicit / inferred / annotated; nominal bounds too.
            const std::string nb = "class NumBox<T: INumber> {\n  var n: T\n  constructor(n: T) { this.n = n }\n}\n";
            check(has(typeErr((nb + "fn f() { let b = NumBox<string>(\"x\") }\n").c_str()),
                      "type argument 'string' of 'NumBox' does not satisfy the bound 'T: INumber'"),
                  "P40 s7: an explicit class type arg violating INumber is refused");
            check(has(typeErr((nb + "fn f() { let b = NumBox(\"x\") }\n").c_str()), "does not satisfy the bound 'T: INumber'"),
                  "P40 s7: an inferred class type arg violating INumber is refused");
            check(has(typeErr((nb + "fn f(b: NumBox<bool>) {}\n").c_str()), "type argument 'bool' of 'NumBox'"),
                  "P40 s7: an annotation violating a class bound is refused");
            check(typeErr((nb + "fn f(b: NumBox<f64>): NumBox<i64> => NumBox(5i64)\nfn g<T: INumber>(x: T): NumBox<T> => NumBox<T>(x)\n").c_str()).empty(),
                  "P40 s7: numeric args and a forwarded in-scope type param satisfy the bound");
            const std::string rk = "interface Ranked {\n  fn rank(): i32\n}\nrecord R(v: i32) : Ranked {\n  fn rank(): i32 => this.v\n}\n"
                                   "class Ladder<T: Ranked> {\n  var top: T\n  constructor(top: T) { this.top = top }\n}\n";
            check(typeErr((rk + "fn f(l: Ladder<R>): i32 => l.top.rank()\n").c_str()).empty(),
                  "P40 s7: an implementing record satisfies an interface bound (and `l.top` is an R)");
            check(has(typeErr((rk + "fn f(l: Ladder<string>) {}\n").c_str()), "does not satisfy the bound 'T: Ranked'"),
                  "P40 s7: a non-implementing type violates an interface bound");
            check(has(typeErr((rk + "fn best<T: Ranked>(a: T): T => a\nfn f(): i32 => best(3)\n").c_str()),
                      "type argument 'i32' does not satisfy the bound 'T: Ranked'"),
                  "P40 s7: a generic FUNCTION's nominal bound is checked on the inferred argument too");
            // Extension default parameters, filled at the call (bound and plain).
            const std::string ext =
                "extension fn string.part(start: i32, len: i32 = 1): string { actual(csharp) extern(\"$this.Substring($0, $1)\") "
                "actual(typescript) extern(\"$this.substring($0, $0 + $1)\") }\n"
                "extension fn i32.plus(n: i32, extra: i32 = 100): i32 => this + n + extra\n"
                "fn f(): string => \"hello\".part(1) + \"hello\".part(1, 3)\nfn g(): i32 => 5.plus(1) + 5.plus(1, 2)\n";
            EmitResult ec = compileStd(ext, findTarget("csharp"));
            check(ec.ok && has(ec.code, "\"hello\".Substring(1, 1) + \"hello\".Substring(1, 3)") &&
                      has(ec.code, "5.plus(1, 100) + 5.plus(1, 2)"),
                  "P40 s7: extension defaults are filled at the call — the bound template's `$1` and a plain call");
            EmitResult et = compileStd(ext, findTarget("typescript"));
            check(et.ok && has(et.code, "\"hello\".substring(1, 1 + 1)"), "P40 s7: the filled default reaches the TS template");
            check(has(typeErr("let k = 3\nextension fn i32.sh(by: i32 = k): i32 => this + by\n"), "must be a literal"),
                  "P40 s7: a non-literal extension default is refused (it would be evaluated in the caller's scope)");
            check(has(typeErr("extension fn i32.sh(a: i32, by: i32 = 1): i32 => this + by\nfn f(): i32 => 1.sh()\n"),
                      "expects 1-2 argument(s), got 0"),
                  "P40 s7: an extension still requires its non-default params");
            // The printable guard now judges substituted member reads, and a never-bound type parameter.
            check(has(typeErr((cell + "record P(x: i32)\nfn f(c: Cell<P>) { print(c.first()) }\n").c_str()),
                      "print cannot print a value of type 'P'"),
                  "P40 s7: print of a substituted member read of record type is refused");
            check(has(typeErr((cell + "fn f(c: Cell) { print(c.first()) }\n").c_str()), "its type parameter was never bound"),
                  "P40 s7: print of a member read whose type parameter was never bound is refused (was not judged)");
            check(typeErr((cell + "fn f(c: Cell<i64>) { print(c.first())\n  print(\"${c.v}\") }\n").c_str()).empty(),
                  "P40 s7: a substituted numeric member read stays printable");
        }

        // ---- P40 slice 8: the ordinal string std (B3), index units (D2/D15), module scoping (D16) ----
        {
            auto warnsOn = [](const EmitResult& r, const std::string& what) {
                for (const auto& d : r.diagnostics) if (d.severity == Severity::Warning && has(d.message, what)) return true;
                return false;
            };
            // C# binds the ORDINAL overloads — never the culture-sensitive IndexOf/StartsWith/EndsWith.
            const std::string ord = "import \"std.strings\"\nfn f(s: string): bool => s.startsWith(\"a\") && s.endsWith(\"b\")\n"
                                    "fn g(s: string): i32 => s.indexOf(\"x\") + compareOrdinal(s, \"y\")\n";
            EmitResult cs = compileStd(ord, findTarget("csharp"));
            check(cs.ok && has(cs.code, "s.StartsWith(\"a\", global::System.StringComparison.Ordinal)") &&
                      has(cs.code, "s.EndsWith(\"b\", global::System.StringComparison.Ordinal)") &&
                      has(cs.code, "self.IndexOf(needle, from, global::System.StringComparison.Ordinal)") &&
                      has(cs.code, "global::System.Math.Sign(string.CompareOrdinal(a, b))"),
                  "P40 s8: C# std.strings binds the StringComparison.Ordinal overloads (+ CompareOrdinal)");
            check(cs.ok && !has(cs.code, "StartsWith(\"a\")") && !has(cs.code, "EndsWith(\"b\")") &&
                      !has(cs.code, "CurrentCulture") && !has(cs.code, "IndexOf(needle, from)"),
                  "P40 s8: no culture-sensitive C# string overload is emitted");
            {   // the pin over the WHOLE manifest: every C# std.strings search/compare call names Ordinal.
                // P40 review (B9): this used to slice the text from `"std.strings": {` to the FIRST `}` — the end
                // of the first nested object — so the templates after it were never checked (vacuous). Now the
                // manifest is parsed and EVERY string value under every "std.strings" object is walked; each call
                // is checked up to its own matching ')'; and the pin must see the calls it exists for.
                std::string m;
                cli::readFile(cli::executablePath().parent_path() / "plugins" / "csharp" / "polyglot-plugin.json", m);
                std::string jerr;
                const json::Value doc = json::parse(m, jerr);
                std::vector<const json::Value*> sections;
                std::function<void(const json::Value&)> findSections = [&](const json::Value& v) {
                    for (const auto& [k, sub] : v.members) {
                        if (k == "std.strings" && sub.kind == json::Value::Kind::Object) sections.push_back(&sub);
                        findSections(sub);
                    }
                    for (const auto& e : v.items()) findSections(e);
                };
                findSections(doc);
                int calls = 0;
                bool allOrdinal = jerr.empty() && !sections.empty();
                std::function<void(const json::Value&)> walk = [&](const json::Value& v) {
                    if (v.kind == json::Value::Kind::String) {
                        const std::string& s = v.asString();
                        for (const char* call : {".IndexOf(", ".StartsWith(", ".EndsWith(", ".LastIndexOf("})
                            for (std::size_t at = s.find(call); at != std::string::npos; at = s.find(call, at + 1)) {
                                ++calls;
                                std::size_t k = s.find('(', at), depth = 0, end = std::string::npos;
                                for (; k < s.size(); ++k) {
                                    if (s[k] == '(') ++depth;
                                    else if (s[k] == ')' && --depth == 0) { end = k; break; }
                                }
                                const std::string args = s.substr(at, end == std::string::npos ? std::string::npos : end - at);
                                if (args.find("StringComparison.Ordinal") == std::string::npos) allOrdinal = false;
                            }
                    }
                    for (const auto& [k, sub] : v.members) walk(sub);
                    for (const auto& e : v.items()) walk(e);
                };
                for (const json::Value* sec : sections) walk(*sec);
                check(allOrdinal && calls >= 3,
                      "P40 s8: every C# std.strings IndexOf/StartsWith/EndsWith template passes StringComparison.Ordinal "
                      "(whole manifest, " + std::to_string(calls) + " call(s) checked)");
            }
            // Guards are written once in .pg: one message on every target; emitted as `_pg_<name>`.
            const std::string at = "import \"std.strings\"\nfn f(s: string): string => s.charAt(1) + s.substring(0, 1)\n";
            bool sameMsg = true;
            for (const char* t : {"csharp", "typescript", "python", "php"}) {
                EmitResult r = compileStd(at, findTarget(t));
                sameMsg = sameMsg && r.ok && has(r.code, "charAt: index ") && has(r.code, " is out of range for length ") &&
                          has(r.code, "_pg_charAt") && has(r.code, "substring: range [");
            }
            check(sameMsg, "P40 s8: the charAt/substring guards (and their messages) come from the one .pg source on all 4");
            EmitResult tsAt = compileStd(at, findTarget("typescript"));
            check(tsAt.ok && has(tsAt.code, "function _pg_charAt(self: string, index: number): string") &&
                      !has(tsAt.code, "export function _pg_charAt") && has(tsAt.code, "self.charAt(index)"),
                  "P40 s8: TS emits the guard as a module-local function over the unchecked primitive");
            EmitResult csAt = compileStd(at, findTarget("csharp"));
            check(csAt.ok && has(csAt.code, "internal static string _pg_charAt(string self, int index)") &&
                      has(csAt.code, "PolyglotProgram._pg_charAt(s, 1)") && !has(csAt.code, "this string") &&
                      !has(csAt.code, "PolyglotExtensions"),
                  "P40 review B10: C# emits the guard as a plain internal static method, not a `this string` "
                  "extension an InternalsVisibleTo assembly would see on every string");
            EmitResult lenOnly = compileStd("import \"std.strings\"\nfn f(s: string): i32 => s.len()\n", findTarget("typescript"));
            check(lenOnly.ok && !has(lenOnly.code, "_pg_charAt") && !has(lenOnly.code, "_pg_split"),
                  "P40 s8: unused guards are pruned (reachability)");
            // A user function sharing a guard's plain name cannot collide with it on a free-function target.
            EmitResult col = compileStd("import \"std.strings\"\nfn charAt(n: i32): i32 => n\nfn f(s: string): string => s.charAt(charAt(0))\n",
                                        findTarget("python"));
            check(col.ok && has(col.code, "def charAt(n):") && has(col.code, "def _pg_charAt(self, index):") &&
                      has(col.code, "_pg_charAt(s, charAt(0))"),
                  "P40 s8: a user `fn charAt` and the std guard coexist on Python (the guard is `_pg_charAt`)");
            // The std-reserved prefixes are refused in user code (declarations and references).
            check(has(typeErr("import \"std.strings\"\nfn f(s: string): string => s.__pgCharAt(0)\n"),
                      "'__pgCharAt' uses a prefix reserved for the Polyglot std library"),
                  "P40 s8: a user call of an unchecked `__pg` primitive is refused");
            check(has(typeErr("fn __pgMine(): i32 => 1\n"), "uses a prefix reserved for the Polyglot std library"),
                  "P40 s8: a user `__pg`-prefixed declaration is refused");
            // Index units: Python and PHP go through UTF-16 helpers; C#/TS are native.
            EmitResult py = compileStd("import \"std.strings\"\nfn f(s: string): i32 => s.len() + s.indexOf(\"b\")\n", findTarget("python"));
            check(py.ok && has(py.code, "_pg_len16(s)") && has(py.code, "def _pg_u16v(s):") && has(py.code, "s.isascii()") &&
                      has(py.code, "_pg_u16_find(self, needle, from_)"),
                  "P40 s8: Python len/indexOf count UTF-16 units (isascii fast path, cached surrogate-expanded view)");
            check(warnsOn(py, "emulates utf16Strings:index"), "P40 s8: Python declares utf16Strings:index emulated (faithful via helpers)");
            EmitResult php = compileStd("import \"std.strings\"\nfn f(s: string): i32 => s.len()\n", findTarget("php"));
            check(php.ok && has(php.code, "__pg_len16($s)") && has(php.code, "iconv('UTF-8', 'UTF-16LE'") &&
                      has(php.code, "function_exists('iconv')") && warnsOn(php, "emulates utf16Strings:index"),
                  "P40 s8: PHP len counts UTF-16 units through the iconv helper (with a named ext-iconv error)");
            EmitResult csLen = compileStd("import \"std.strings\"\nfn f(s: string): i32 => s.len()\n", findTarget("csharp"));
            check(csLen.ok && has(csLen.code, "s.Length") && !warnsOn(csLen, "utf16Strings"), "P40 s8: C# len is native (no warning)");
            EmitResult noIdx = compileStd("fn main() { print(1) }\n", findTarget("python"));
            check(noIdx.ok && !has(noIdx.code, "_pg_u16v") && has(noIdx.code, "def _pg_out16(s):"),
                  "P40 s8: no index op -> no UTF-16 helper; Python print maps lone surrogates (U+FFFD)");
            // The rest of the surface, per target.
            const std::string rest = "import \"std.strings\"\nfn f(s: string): string => s.replaceAll(\"$\", \"$$\") + s.trimAscii()\n"
                                     "fn g(s: string): i32 => s.split(\",\").count\nfn h(): string => string.fromCodePoint(233)\n"
                                     "fn k(a: string): i32 => compareOrdinal(a, \"1e1\")\n";
            EmitResult rts = compileStd(rest, findTarget("typescript"));
            check(rts.ok && has(rts.code, "self.split(needle).join(replacement)") && has(rts.code, "String.fromCodePoint(cp)") &&
                      has(rts.code, "s.replace(/^[\\t\\n\\v\\f\\r ]+|[\\t\\n\\v\\f\\r ]+$/g, \"\")"),
                  "P40 s8: TS replaceAll is split/join (no `$$` replacement patterns); trimAscii is exactly TAB..CR + space");
            EmitResult rphp = compileStd(rest, findTarget("php"));
            check(rphp.ok && has(rphp.code, "function __pg_chr8($c)") && has(rphp.code, "__pg_chr8($cp)") &&
                      has(rphp.code, "strcmp($a, $b)") && !has(rphp.code, "<=>") && has(rphp.code, "__pg_u16_split($self, $separator)") &&
                      has(rphp.code, "explode($sep, $s)"),
                  "P40 s8: PHP fromCodePoint via the UTF-8 encoder helper (requires), compareOrdinal via strcmp (never <=>); "
                  "split is explode unless a lone surrogate needs the UTF-16 view (review B2)");
            EmitResult rcs = compileStd(rest, findTarget("csharp"));
            check(rcs.ok && has(rcs.code, "internal static string _pg_string_fromCodePoint(int cp)") &&
                      has(rcs.code, "is not a Unicode scalar value") && has(rcs.code, "char.ConvertFromUtf32(cp)"),
                  "P40 s8: string.fromCodePoint is a .pg guard emitted as a prelude function over the primitive");
            EmitResult rpy = compileStd(rest, findTarget("python"));
            check(rpy.ok && has(rpy.code, "s.strip(\"\\t\\n\\x0b\\x0c\\r \")"), "P40 s8: Python trimAscii strips exactly TAB..CR + space (not GS)");

            // D16: an import makes names visible only to the module that writes it.
            check(has(firstErr(compile("import \"std.strings\"\nfn f(): i32 {\n  let xs: List<string> = \"a,b\".split(\",\")\n  return xs.count\n}\n",
                                       findTarget("csharp"))),
                      "'List' is declared in module 'std.collections', which this module does not import"),
                  "P40 s8 (D16): std.strings' own import of std.collections does not leak `List` to the user");
            {
                // P40 review B10: the D16 diagnostic points at the offending type name (3:11), not the statement (3:3).
                EmitResult r = compile("import \"std.strings\"\nfn f(): i32 {\n  let xs: List<string> = \"a,b\".split(\",\")\n  return xs.count\n}\n",
                                       findTarget("csharp"));
                check(!r.diagnostics.empty() && r.diagnostics[0].pos.line == 3 && r.diagnostics[0].pos.col == 11,
                      "P40 review B10 (D16): the not-imported diagnostic's column is the type name's");
            }
            check(compile("import \"std.strings\"\nfn f(): i32 => \"a,b\".split(\",\").count\n", findTarget("csharp")).ok,
                  "P40 s8 (S18): split's List<string> is usable without naming List (no collections import)");
            check(compile("import \"std.strings\"\nimport \"std.collections\"\nfn f(): i32 {\n  let xs: List<string> = \"a\".split(\",\")\n  return xs.count\n}\n",
                          findTarget("csharp")).ok,
                  "P40 s8 (D16): naming List with its own import compiles");
            {
                MapModuleResolver res{{"./a", "import { mid } from \"./b\"\nfn top(): i32 => mid()\n"},
                                      {"./b", "fn mid(): i32 => 1\nfn deep(): i32 => 2\n"}};
                EmitResult tr = compile("import { top } from \"./a\"\nfn main() { let x = top() + mid() }\n",
                                        findTarget("csharp"), &res);
                check(!tr.ok && has(firstErr(tr), "'mid' is declared in module './b', which this module does not import"),
                      "P40 s8 (D16): a user module's transitive import is not visible to the importer");
                EmitResult sel = compile("import { top } from \"./a\"\nimport { mid } from \"./b\"\nfn main() { let x = deep() }\n",
                                         findTarget("csharp"), &res);
                check(!sel.ok && has(firstErr(sel), "'deep' is declared in './b' but not imported"),
                      "P40 s8 (D16): a name a selective import does not list is refused");
            }
            // P40 review A8 (D16): overload resolution only considers the overloads the calling module can see.
            {
                MapModuleResolver res{{"./ai", "fn check(n: i32): bool => n > 0\nfn aiOk(n: i32): bool => check(n)\n"}};
                const std::string leak = "import { aiOk } from \"./ai\"\nfn check(s: string): bool => s == \"01\"\n"
                                         "fn main() { let b = check(5) }\n";
                for (const char* t : {"csharp", "typescript"}) {
                    EmitResult r = compile(leak, findTarget(t), &res);
                    check(!r.ok && has(firstErr(r), "argument 1 of 'check'"),
                          std::string("A8: a never-imported overload does not win the call (refused, not bound) on ") + t);
                }
                // Imported, it competes: check(5) binds ai's i32 overload, check("01") the local one.
                EmitResult ok = compile("import { aiOk, check } from \"./ai\"\nfn check(s: string): bool => s == \"01\"\n"
                                        "fn main() { let a = check(5)\n  let b = check(\"01\") }\n",
                                        findTarget("typescript"), &res);
                check(ok.ok && has(ok.code, "check$i32(5)") && has(ok.code, "check$string(\"01\")"),
                      "A8: an imported overload competes with the module's own");
                // A true duplicate across modules names both declarations.
                MapModuleResolver dup{{"./date", "fn check(s: string): bool => s == \"x\"\n"}};
                EmitResult d = compile("import { check } from \"./date\"\nfn check(s: string): bool => s == \"01\"\nfn main() { }\n",
                                       findTarget("csharp"), &dup);
                bool both = false;
                for (const auto& dg : d.diagnostics)
                    if (has(dg.message, "duplicate function 'check'") && has(dg.message, "module")) both = true;
                check(!d.ok && both, "A8: the cross-module duplicate-overload error names both modules");
            }
            // A bare import of an extension-only module names its functions on TS and Python.
            {
                MapModuleResolver res{{"./ext", "extension fn i32.twice(): i32 => this * 2\n"}};
                const std::string prog = "import \"./ext\"\nfn main() { let x = 4.twice() }\n";
                EmitResult bt = compile(prog, findTarget("typescript"), &res);
                const bool tsOk = bt.ok && has(bt.code, "import { twice } from \"./ext\";") && has(bt.code, "twice(4)");
                EmitResult bp = compile(prog, findTarget("python"), &res);
                const bool pyOk = bp.ok && has(bp.code, "from ext import *");
                check(tsOk, "P40 s8: a bare import of an extension-only module imports its names on TS (was TS2304)");
                check(pyOk, "P40 s8: a bare import of an extension-only module star-imports on Python (was NameError)");
            }
        }

        // ---- P40 slice 9: Map<K, V> (B4) — the IMapKey bound, non-nullable V, snapshots, the four bindings ----
        {
            // Keys: string and the integer scalars only; every refusal names its reason.
            for (const char* k : {"string", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64"})
                check(typeErr((std::string("fn f(m: Map<") + k + ", i32>): i32 => m.count\n").c_str()).empty(),
                      std::string("P40 s9: '") + k + "' satisfies IMapKey");
            const struct { const char* key; const char* why; } refused[] = {
                {"f64", "floating-point keys are refused"}, {"f32", "floating-point keys are refused"},
                {"bool", "bool keys are refused"},          {"char", "char keys are refused"},
                {"P", "record keys are refused"},           {"C", "class and interface keys are refused"},
                {"string?", "a nullable key is refused"},   {"List<i32>", "only 'string' and the integer scalars"},
                {"E", "only 'string' and the integer scalars"},
            };
            for (const auto& r : refused) {
                const std::string src = std::string("record P(x: i32)\nclass C {\n  var x: i32 = 0\n}\nenum E { A }\n"
                                                    "fn f() { let m = Map<") + r.key + ", i32>() }\n";
                const std::string e = typeErr(src.c_str());
                check(has(e, "does not satisfy the bound 'K: IMapKey'") && has(e, r.why),
                      std::string("P40 s9: Map<") + r.key + ", _> is refused with its reason");
            }
            check(has(typeErr("record P(x: i32)\nfn f(m: Map<P, i32>) {}\n"), "record keys are refused"),
                  "P40 s9: the key bound is checked on an annotation too");
            check(typeErr("fn f<K: IMapKey, V>(m: Map<K, V>): i32 => m.count\nfn g(m: Map<u8, bool>): i32 => f(m)\n").empty(),
                  "P40 s9: a generic forwarding its own IMapKey-bounded K is accepted");
            // V must not be nullable — pointing at `has`.
            const std::string nv = typeErr("fn f() { let m = Map<string, i32?>() }\n");
            check(has(nv, "type argument 'i32?' for 'V' of 'Map' must not be nullable") && has(nv, "'Map.has'"),
                  "P40 s9: a nullable V is refused, pointing at Map.has");
            // Not directly for-iterable — pointing at the snapshots.
            const std::string it = typeErr("fn f(m: Map<string, i32>) {\n  for k in m {\n  }\n}\n");
            check(has(it, "'Map<string, i32>' is not iterable") && has(it, "'.keys()' or '.values()'"),
                  "P40 s9: `for k in m` is refused, pointing at keys()/values()");
            check(typeErr("fn f(m: Map<string, i32>): i32 {\n  var n = 0\n  for k in m.keys() {\n    n = n + 1\n  }\n"
                          "  for v in m.values() {\n    n = n + v\n  }\n  return n\n}\n").empty(),
                  "P40 s9: the snapshots iterate (keys() is List<K>, values() is List<V>)");

            // The bindings.
            const std::string use = "fn f(m: Map<string, i32>): i32 => (m.get(\"a\") ?? 0) + (m.get(\"b\") ?? 0)\n"
                                    "fn g(m: Map<string, i32>): bool => m.remove(\"a\")\n"
                                    "fn h(m: Map<string, i32>): List<string> => m.keys()\n"
                                    "fn mk(): Map<string, i32> => Map<string, i32>()\n"
                                    "fn s(m: Map<string, i32>) { m.set(\"k\", 1) }\n";
            EmitResult cs = compileStd(use, findTarget("csharp"));
            check(cs.ok && has(cs.code, "global::System.Collections.Generic.OrderedDictionary<string, int>") &&
                      has(cs.code, "(m.TryGetValue(\"a\", out var __pg_t1) ? __pg_t1 : (int?)null)") &&
                      has(cs.code, "(m.TryGetValue(\"b\", out var __pg_t2) ? __pg_t2 : (int?)null)") &&
                      has(cs.code, "new global::System.Collections.Generic.List<string>(m.Keys)") &&
                      has(cs.code, "m.Remove(\"a\")"),
                  "P40 s9: C# OrderedDictionary, typed else-arm, unique temps, List snapshot");
            EmitResult csg = compileStd("fn f<K: IMapKey, V>(m: Map<K, V>): i32 => m.count\n", findTarget("csharp"));
            check(csg.ok && has(csg.code, "where K : notnull"), "P40 s9: C# spells the IMapKey bound `where K : notnull`");
            EmitResult ts = compileStd(use + "fn n(m: Map<string, i32>): i32? => m.get(\"a\")\n", findTarget("typescript"));
            check(ts.ok && has(ts.code, "(m.get(\"a\") ?? null)") && has(ts.code, "[...m.keys()]") &&
                      has(ts.code, "new Map<string, number>()") && has(ts.code, "m.delete(\"a\")"),
                  "P40 s9: TS Map, `?? null`, spread snapshot");
            // P40 review A1: as the left operand of `??` the normalization is dropped — tsc >= 5.6 rejects
            // `(m.get(k) ?? null) ?? d` as TS2871 (always nullish).
            check(has(ts.code, "(m.get(\"a\") ?? 0)") && !has(ts.code, "?? null) ??"),
                  "A1: TS `m.get(k) ?? d` emits no inner `?? null` (spec absentNormalize)");
            EmitResult tsg = compileStd("fn f<K: IMapKey, V>(m: Map<K, V>): i32 => m.count\n", findTarget("typescript"));
            check(tsg.ok && !has(tsg.code, "IMapKey"), "P40 s9: TS erases the IMapKey bound");
            EmitResult py = compileStd(use, findTarget("python"));
            check(py.ok && has(py.code, "(m.pop(\"a\", _PG_ABSENT) is not _PG_ABSENT)") && has(py.code, "_PG_ABSENT = object()") &&
                      has(py.code, "list(m.keys())"),
                  "P40 s9: Python dict, remove via a dedicated pop sentinel, list snapshot");
            EmitResult pyNo = compileStd("fn f(m: Map<string, i32>): i32 => m.count\n", findTarget("python"));
            check(pyNo.ok && !has(pyNo.code, "_PG_ABSENT"), "P40 s9: the Python sentinel is emitted only when remove is used");
            EmitResult php = compileStd(use, findTarget("php"));
            check(php.ok && has(php.code, "new \\ArrayObject([])") && has(php.code, "__pg_mget($m, \"a\")") &&
                      has(php.code, "__pg_mkeys($m)") && has(php.code, "$m[__pg_mk(\"k\")] = 1") &&
                      has(php.code, "is_int($k) ? $k : \"s\" . $k") && has(php.code, "substr($k, 1)"),
                  "P40 s9: PHP \\ArrayObject with prefixed string keys, decoded by keys()");
            EmitResult phpEq = compileStd("fn f(a: string, b: string): bool => a == b && a != \"01\"\n", findTarget("php"));
            check(phpEq.ok && has(phpEq.code, "$a === $b") && has(phpEq.code, "$a !== \"01\""),
                  "P40 s9: PHP string equality is strict (`\"01\" == \"1\"` is true under ==)");
            // Records holding a Map compare by reference (Python would otherwise compare dicts by value).
            EmitResult rec = compileStd("record H(m: Map<string, i32>, n: i32)\n", findTarget("python"));
            check(rec.ok && has(rec.code, "self.m is other.m") && has(rec.code, "self.n == other.n"),
                  "P40 s9: a record's Map field compares by reference on Python, like List");
        }
    }

    // P4 — name / type resolution across the declaration surface.
    auto resolves = [&](const char* src, const std::string& name) {
        EmitResult r = compile(src, findTarget("csharp"));
        check(r.ok, name);
    };
    // Same, but with the std prelude (for programs that call `print` and friends).
    auto resolvesStd = [&](const char* src, const std::string& name) {
        EmitResult r = compileStd(src, findTarget("csharp"));
        check(r.ok, name);
    };
    rejects("fn f(x: Widget) {}\n", "P4: rejects unknown type in a parameter");
    rejects("record A(x: i32)\nrecord A(y: i32)\n", "P4: rejects duplicate type declaration");
    rejects("fn f<T>(x: T): Nope => x\n", "P4: rejects unknown type in a generic return");
    rejects("record B(x: i32) : Missing\n", "P4: rejects unknown base type");
    resolves("record Box<T>(value: T)\nfn id<T>(x: T): T => x\n", "P4: generics + declared types resolve");
    resolves("enum E { A, B }\nfn pick(): E => E.A\n", "P4: declared enum type resolves");
    // P4-2 — nominal expression typing.
    rejects("record V(x: f64)\nfn f() { let v = V(1.0)\n print(v.y) }\n", "P4: rejects access to a missing member");
    rejects("record V(x: f64, y: f64)\nfn f() { let v = V(1.0) }\n", "P4: rejects wrong construction arity");
    rejects("class C { fn go() {} }\nfn f() { let c = C()\n c.nope() }\n", "P4: rejects call to a missing method");
    resolves("record V(x: f64, y: f64)\nfn f(): f64 => V(1.0, 2.0).x\n", "P4: member access types to the field type");
    resolves("record P(x: i32) {\n  fn getX(): i32 => this.x\n}\n", "P4: this.member resolves in a method");
    resolves("class C { fn go(): i32 => 5 }\nfn f(): i32 => C().go()\n", "P4: method call resolves to its return type");

    // P4 — AST -> typed IR lowering.
    {
        // (print is now a std module function, not an intrinsic, so this drives the pipeline with a
        // user-defined unit-returning function `show` to exercise typed-call lowering without the prelude.)
        const char* src = "fn add(a: i32, b: i32): i32 {\n  return a + b\n}\n"
                          "fn show(n: i32) {}\n"
                          "fn main() {\n  show(add(1, 2))\n}\n";
        DiagnosticBag d;
        auto unit = parse(lex(src, d), d);
        mintplayer::polyglot::check(unit, d); // annotate resolved types
        check(!d.hasErrors(), "IR: program type-checks");
        std::string ir = ir::dump(lower(unit, "csharp"));
        check(has(ir, "fn add(a: i32, b: i32): i32 {"), "IR: function signature carries types");
        check(has(ir, "return (a:i32 + b:i32):i32"), "IR: binary expression is typed");
        check(has(ir, "fn main(): unit [entry] {"), "IR: main resolved as the entry point");
        check(has(ir, "show(add(1:i32, 2:i32):i32):unit"), "IR: nested typed call resolved");
    }

    // P4-5 — pattern-match exhaustiveness.
    resolves("union Sh { Circle(r: f64), Square(s: f64) }\nfn area(x: Sh): f64 => match x { Circle(r) => r, Square(s) => s }\n", "P4: exhaustive union match resolves");
    rejects("union Sh { Circle(r: f64), Square(s: f64) }\nfn bad(x: Sh): f64 => match x { Circle(r) => r }\n", "P4: rejects non-exhaustive union match");
    rejects("enum Dir { N, S }\nfn f(d: Dir): i32 => match d { N => 1 }\n", "P4: rejects non-exhaustive enum match");
    rejects("fn g(b: bool): i32 => match b { true => 1 }\n", "P4: rejects non-exhaustive bool match");
    rejects("fn h(n: i32): i32 => match n { 0 => 0 }\n", "P4: rejects scalar match without a catch-all");
    resolves("fn ok2(n: i32): i32 => match n { 0 => 0, _ => 1 }\n", "P4: scalar match with catch-all resolves");
    resolves("enum Dir { N, S }\nfn turn(d: Dir): i32 => match d { N => 1, S => 2 }\n", "P4: exhaustive enum match resolves");

    // P5 — backend registry seam.
    check(findBackend("csharp") != nullptr && findBackend("csharp")->name() == "csharp", "P5: csharp backend registered");
    check(findBackend("typescript") != nullptr && findBackend("typescript")->name() == "typescript", "P5: typescript backend registered");
    check(findBackend("python") != nullptr && findBackend("python")->name() == "python", "P9: python backend registered");
    check(findBackend("ruby") == nullptr, "P5: unknown backend is not found");

    // P5 §3.E — per-target capability gating.
    {
        const char* extProg =
            "extension fn i32.doubled(): i32 => this * 2\n"
            "fn main() { let n = 5\n print(n.doubled()) }\n";
        DiagnosticBag d;
        auto unit = parse(lex(extProg, d), d);

        std::vector<FeatureUse> uses = collectFeatureUses(unit);
        bool sawExt = false;
        for (const auto& u : uses) if (u.key == featureName(Feature::ExtensionMethods)) sawExt = true;
        check(sawExt, "P5.E: collectFeatureUses detects extensionMethods");

        DiagnosticBag dcs;
        checkCapabilities(unit, *findBackend("csharp"), dcs);
        check(!dcs.hasErrors(), "P5.E: csharp (full capability set) accepts extension methods");

        StubBackend stub(Feature::ExtensionMethods);
        DiagnosticBag ds;
        checkCapabilities(unit, stub, ds);
        check(ds.hasErrors(), "P5.E: a backend lacking extensionMethods refuses the program");
        bool named = false;
        for (const auto& it : ds.items())
            if (has(it.message, "extensionMethods") && has(it.message, "stub")) named = true;
        check(named, "P5.E: the capability diagnostic names the feature and the target");

        // A feature the program does NOT use must not be gated.
        StubBackend noIter(Feature::Iterators);
        DiagnosticBag dn;
        checkCapabilities(unit, noIter, dn);
        check(!dn.hasErrors(), "P5.E: a missing capability the program doesn't use is not refused");
    }

    // P6 — §3.B refusals surface as targeted "Polyglot refuses X" diagnostics, not generic "unknown type".
    auto refuses = [&](const char* src, const std::string& needle, const std::string& name) {
        EmitResult r = compile(src, findTarget("csharp"));
        bool named = false;
        for (const auto& d : r.diagnostics) if (has(d.message, "refuses") && has(d.message, needle)) named = true;
        check(!r.ok && named, name);
    };
    refuses("fn f(x: decimal) {}\n", "decimal", "P6: refuses 'decimal' with a targeted message");
    refuses("fn f(t: Thread) {}\n", "threads", "P6: refuses Thread (threads/locks)");
    refuses("fn f(p: IntPtr) {}\n", "pointers", "P6: refuses IntPtr (pointers/unsafe)");
    refuses("fn f(e: Expression<i32>) {}\n", "expression trees", "P6: refuses LINQ expression trees");
    refuses("fn f(d: dynamic) {}\n", "dynamic", "P6: refuses 'dynamic' / runtime code-gen");
    // §3.B statement forms: `lock`/`unsafe` are unspeakable, so a C#-habit statement is refused out loud
    // (a targeted message, not a confusing parse error) with no cascade errors.
    refuses("fn main() { lock (m) { print(1) } }\n", "locks", "P6: refuses a `lock` statement");
    refuses("fn main() { unsafe { print(1) } }\n", "unsafe", "P6: refuses an `unsafe` block");
    // §3.B: a finalizer (`~Name() {}`) is unspeakable — no cross-target deterministic-destruction contract
    // (issue #54). The parser refuses the `~` member form out loud instead of two raw parse errors.
    refuses("class Foo {\n  ~Foo() {\n  }\n}\n", "finalizers", "P6: refuses a finalizer (`~Name`)");
    // §3.B: a match arm must be an expression — a block-bodied arm is refused (Python/PHP lower `match`
    // to an expression fold with no statement block; issue #51 — was a silent drop-to-`0`).
    refuses("fn f(n: i32): i32 => match n {\n  0 => { return 1 }\n  _ => 0\n}\n",
            "block-bodied match arm", "P6: refuses a block-bodied match arm");
    { // #56a: an unresolvable method on a concrete string receiver (missing `import "std.strings"`) is
      // refused, not emitted verbatim to crash at every target's runtime.
        EmitResult r = compile("fn f(s: string): i32 => s.codePointAt(0)\n", findTarget("csharp"));
        bool named = false;
        for (const auto& d : r.diagnostics) if (has(d.message, "import missing")) named = true;
        check(!r.ok && named, "P36 #56a: unresolvable string method (missing import) is refused");
    }
    { // #38 (as relaxed by P37 B4): an interface type-test gates per-target — refused iff a configured
      // target lacks runtime interface identity (TS erases interfaces); a csharp-only build allows it.
        const char* src = "interface Shape {}\nclass Circle : Shape { constructor() {} }\n"
                          "fn f(s: Circle): i32 => match s {\n  x: Shape => 1\n  _ => 0\n}\n";
        check(compile(src, findTarget("csharp")).ok,
              "P36 #38 / P37 B4: an interface typed-match arm is allowed on a csharp-only build");
        EmitResult r = compile(src, findTarget("typescript"));
        bool named = false;
        for (const auto& d : r.diagnostics) if (has(d.message, "interfaces:runtimeIdentity")) named = true;
        check(!r.ok && named, "P36 #38 / P37 B4: an interface typed-match arm is refused when TS is configured");
    }
    { // #53: member overloading is not supported (top-level only) — a duplicate same-named method is
      // refused at the declaration site (was silently shadowed + a misleading call-site arity error).
        const char* src = "class Calc {\n  fn add(a: i32): i32 => a + 1\n  fn add(a: i32, b: i32): i32 => a + b\n}\n";
        EmitResult r = compile(src, findTarget("csharp"));
        bool named = false;
        for (const auto& d : r.diagnostics) if (has(d.message, "already defined")) named = true;
        check(!r.ok && named, "P36 #53: a duplicate member method is refused at the declaration site");
    }
    // P6 — function overloading: resolves by arity/type; true duplicates still rejected.
    resolvesStd("fn f(x: i32): i32 => x\nfn f(x: f64): i32 => 0\nfn f(a: i32, b: i32): i32 => a\n"
             "fn main() { print(f(1))\n print(f(1.0))\n print(f(1, 2)) }\n", "P6: function overloading resolves");
    rejects("fn f(x: i32): i32 => x\nfn f(y: i32): i32 => y\nfn main() {}\n",
            "P6: rejects a true duplicate (same parameter types)");

    // P7 — portable-core guard: `extern` is only allowed inside a target-gated `actual`.
    rejects("fn portable(): i32 => extern(\"42\")\nfn main() {}\n",
            "P7: extern is refused in portable code");
    resolvesStd("expect fn a(): i32\nactual(csharp) fn a(): i32 => extern(\"42\")\n"
             "actual(typescript) fn a(): i32 => extern(\"42\")\nfn main() { print(a()) }\n",
             "P7: extern is allowed inside an actual");

    // P9-V — target-gated portability (§3.B): calling a portable fn (one with `actual`s) on a target that has
    // no `actual` for it must refuse, not silently emit a call to an undefined function. Keyed on call sites,
    // so an *unused* portable fn missing this target's arm is fine (e.g. std.io.readText on Python).
    {
        const char* noPy = "expect fn answer(): i32\nactual(csharp) fn answer(): i32 => 42\n"
                           "actual(typescript) fn answer(): i32 => 42\nfn main() { print(answer()) }\n";
        EmitResult py = compileStd(noPy, findTarget("python"));
        bool named = false;
        for (const auto& d : py.diagnostics) if (has(d.message, "no 'actual'") && has(d.message, "python")) named = true;
        check(!py.ok && named, "P9-V: calling a portable fn with no actual for the target is refused");
        check(compileStd(noPy, findTarget("csharp")).ok, "P9-V: the same call compiles for a target that has the actual");
        check(compileStd("expect fn unused(): i32\nactual(csharp) fn unused(): i32 => 1\nfn main() { print(1) }\n",
                         findTarget("python")).ok,
              "P9-V: an unused portable fn missing this target's actual is not refused");
        // P40 review B5: the same call from a module value's INITIALIZER (`let SEP = sep()`) is refused too —
        // it used to build with exit 0 and fail at runtime (Python NameError / PHP undefined function).
        const char* topLet = "expect fn sep(): string\nactual(csharp) fn sep(): string => \"/\"\n"
                             "actual(typescript) fn sep(): string => \"/\"\nlet SEP = sep()\nfn main() { print(SEP) }\n";
        for (const char* t : {"python", "php"}) {
            EmitResult r = compileStd(topLet, findTarget(t));
            bool namedTop = false;
            for (const auto& d : r.diagnostics) if (has(d.message, "no 'actual'") && has(d.message, t)) namedTop = true;
            check(!r.ok && namedTop, std::string("B5: a portable call in a module initializer is refused on ") + t);
        }
        check(compileStd(topLet, findTarget("typescript")).ok, "B5: the module-initializer call compiles where the actual exists");
    }
    // P19 (1) — Python's two genuine target limits refuse out loud. Both previously emitted a silent
    // sentinel string (`__py_unsupported_block_lambda__` / `__py_unsupported_expr__`) into "valid" output —
    // the exact §3.B silent-broken-output failure mode P9-V caught for break/continue.
    {
        const char* blockLambda = "fn main() { var total: i32 = 0\n  let add = (n: i32) => { total += n }\n  add(2)\n  print(total) }\n";
        // P25 §4.18: python now SUPPORTS block lambdas by hoisting them to a nested `def` (Python `lambda` is
        // expression-only) — this supersedes the P19 refusal. The mutated capture becomes a `nonlocal`.
        EmitResult py = compileStd(blockLambda, findTarget("python"));
        check(py.ok && has(py.code, "def ") && has(py.code, "nonlocal total"),
              "P25: python emits a block lambda as a hoisted def with nonlocal");
        check(compileStd(blockLambda, findTarget("csharp")).ok && compileStd(blockLambda, findTarget("typescript")).ok,
              "P19: C#/TS still compile a statement-bodied lambda");

        // P19 (2): `with` emits everywhere via the lowering-precomputed ctor rebuild (Python included).
        const char* withExpr = "record P(x: i32, y: i32)\n"
                               "fn main() { let a = P(1, 2)\n  let b = a with { x = 3 }\n  print(b.x) }\n";
        EmitResult pyW = compileStd(withExpr, findTarget("python"));
        check(pyW.ok && has(pyW.code, "P(3, a.y)"), "P19: python with-expression -> ctor rebuild");
        EmitResult tsW = compileStd(withExpr, findTarget("typescript"));
        check(tsW.ok && has(tsW.code, "new P(3, a.y)"), "P19: TS with-expression -> ctor rebuild (simple base)");
        EmitResult csW = compileStd(withExpr, findTarget("csharp"));
        check(csW.ok && has(csW.code, "a with { x = 3 }"), "P19: C# with-expression stays native");
    }
    // P9-V audit — a tuple pattern in `match` binds + type-checks but has no lowering, so it must refuse
    // (never miscompile) rather than emit a call against undefined binders. (for-in destructuring is fine.)
    {
        EmitResult r = compileStd("fn f(p: (i32, i32)): i32 => match p { (a, b) => a + b }\n"
                                  "fn main() { print(f((1, 2))) }\n", findTarget("csharp"));
        bool named = false;
        for (const auto& d : r.diagnostics) if (has(d.message, "tuple patterns in 'match'")) named = true;
        check(!r.ok && named, "P9-V audit: a tuple pattern in match is refused, not miscompiled");
    }
    // INumber — a generic param bounded by the numeric marker must infer to a number; a non-numeric type
    // argument is refused at Polyglot compile time (better DX than a target-compiler error / TS NaN).
    {
        auto refusesNum = [&](const char* src, const std::string& name) {
            EmitResult r = compileStd(src, findTarget("csharp"));
            bool named = false;
            for (const auto& d : r.diagnostics) if (has(d.message, "INumber")) named = true;
            check(!r.ok && named, name);
        };
        refusesNum("fn main() { print(Math.max(\"a\", \"b\")) }\n", "INumber: Math.max on strings is refused");
        refusesNum("fn main() { print(Math.abs(true)) }\n", "INumber: Math.abs on bool is refused");
        resolvesStd("fn main() { print(Math.max(3, 8))\n print((i32)Math.round(2.7)) }\n",
                    "INumber: Math.max/round on numbers still resolve");
        // P28 — sign/clamp are generic <T: INumber> like abs/max, so the same constraint applies.
        refusesNum("fn main() { print(Math.sign(\"x\")) }\n", "INumber: Math.sign on strings is refused");
        resolvesStd("fn main() { print(Math.sign(-5))\n print(Math.clamp(7, 0, 10)) }\n",
                    "P28: Math.sign/clamp on numbers resolve");
        // The constraint is general, not Math-specific: a user `<T: INumber>` fn enforces it too.
        refusesNum("fn twice<T: INumber>(x: T): T => x\nfn main() { print(twice(\"hi\")) }\n",
                   "INumber: a user <T: INumber> fn rejects a non-numeric arg");
        resolvesStd("fn twice<T: INumber>(x: T): T => x\nfn main() { print(twice(21)) }\n",
                    "INumber: a user <T: INumber> fn accepts a numeric arg");
    }

    // P15 — async/await. Author writes the unwrapped `T` (§4.7 Option B); each backend synthesizes its wrapper
    // (C# Task<T> + GetAwaiter().GetResult(); TS Promise<T> + floating main(); Python async def + asyncio.run).
    {
        const char* prog =
            "async fn doubleIt(x: i32): i32 => x * 2\n"
            "async fn main() {\n  let r = await doubleIt(21)\n  print(r)\n}\n";

        EmitResult cs = compileStd(prog, findTarget("csharp"));
        check(cs.ok, "P15 C#: async program compiles");
        check(has(cs.code, "async global::System.Threading.Tasks.Task<int> doubleIt"),
              "P15 C#: async fn -> `async Task<T>` (unwrapped T wrapped by the backend)");
        check(has(cs.code, "async global::System.Threading.Tasks.Task main"),
              "P15 C#: async unit fn -> `async Task` (bare, no <T>)");
        check(has(cs.code, "await PolyglotProgram.doubleIt(21)"), "P15 C#: `await e` emits `await`");
        check(has(cs.code, "main().GetAwaiter().GetResult();"),
              "P15 C#: async main is driven synchronously from Main()");

        EmitResult ts = compileStd(prog, findTarget("typescript"));
        check(ts.ok, "P15 TS: async program compiles");
        check(has(ts.code, "async function doubleIt(x: number): Promise<number>"),
              "P15 TS: async fn -> `async function …: Promise<T>`");
        check(has(ts.code, "async function main(): Promise<void>"),
              "P15 TS: async unit fn -> `Promise<void>`");
        check(has(ts.code, "await doubleIt(21)"), "P15 TS: `await e` emits `await`");
        check(has(ts.code, "\nmain();\n"), "P15 TS: async main is a floating top-level call");

        EmitResult py = compileStd(prog, findTarget("python"));
        check(py.ok, "P15 Python: async program compiles");
        check(has(py.code, "async def doubleIt("), "P15 Python: async fn -> `async def`");
        check(has(py.code, "async def main("), "P15 Python: async main -> `async def`");
        check(has(py.code, "await doubleIt(21)"), "P15 Python: `await e` emits `await`");
        check(has(py.code, "asyncio.run(main())") && has(py.code, "import asyncio"),
              "P15 Python: async main -> asyncio.run + `import asyncio` prepended");

        // sema §4.7: `await` outside an async fn is refused (else a native compile error = a §3.B miscompile).
        {
            EmitResult r = compileStd("fn doubleIt(x: i32): i32 => x * 2\n"
                                      "fn main() { print(await doubleIt(1)) }\n", findTarget("csharp"));
            bool named = false;
            for (const auto& d : r.diagnostics) if (has(d.message, "await") && has(d.message, "async fn")) named = true;
            check(!r.ok && named, "P15 sema: `await` outside an async fn is refused");
        }
        // sema §4.7: `async` + `yield` (async iterators) is refused — out of scope for v1.
        {
            EmitResult r = compileStd("async fn g(): Iterable<i32> {\n  yield 1\n}\nfn main() { print(1) }\n",
                                      findTarget("csharp"));
            bool named = false;
            for (const auto& d : r.diagnostics) if (has(d.message, "async") && has(d.message, "yield")) named = true;
            check(!r.ok && named, "P15 sema: an `async` fn using `yield` is refused (no async iterators)");
        }
        // §3.E: a backend that lacks Async refuses an async program (proves the capability bites).
        {
            DiagnosticBag d;
            auto unit = parse(lex(prog, d), d);
            StubBackend noAsync(Feature::Async);
            DiagnosticBag da;
            checkCapabilities(unit, noAsync, da);
            bool named = false;
            for (const auto& it : da.items()) if (has(it.message, "async") && has(it.message, "stub")) named = true;
            check(da.hasErrors() && named, "P15 §3.E: a backend lacking `async` refuses the program");
            // all three real backends support async, so none of them gate it
            DiagnosticBag dcs; checkCapabilities(unit, *findBackend("csharp"), dcs);
            check(!dcs.hasErrors(), "P15 §3.E: csharp (full capability set) accepts async");
        }
    }

    // P15 — Awaitable<T> unwrap (§4.7). An async call types as `Awaitable<T>`; `await` unwraps it to `T`.
    // This lets sema catch "forgot to await" and "awaited a non-async value" — and mirrors C#/TS, where
    // `return f()` from an async fn (f async) requires `return await f()`.
    {
        auto asyncRefuses = [&](const char* src, const std::string& needle, const std::string& name) {
            EmitResult r = compileStd(src, findTarget("csharp"));
            bool named = false;
            for (const auto& d : r.diagnostics) if (has(d.message, needle)) named = true;
            check(!r.ok && named, name);
        };
        const char* leaf = "async fn f(): i32 => 21\n";
        // `await` unwraps to the underlying T: `let x = await f()` gives an i32 usable in arithmetic.
        resolvesStd((std::string(leaf) + "async fn main() {\n  let x = await f()\n  print(x + 1)\n}\n").c_str(),
                    "P15 Awaitable: `await f()` unwraps to i32 (usable in arithmetic)");
        // `return await g()` chains async fns (the awaited result is the unwrapped T).
        resolvesStd((std::string(leaf) + "async fn g(): i32 {\n  return await f()\n}\n"
                     "async fn main() { print(await g()) }\n").c_str(),
                    "P15 Awaitable: `return await g()` chains async calls");
        // Forgot-await, caught: returning an async call's Awaitable<i32> where i32 is expected is a type error.
        asyncRefuses((std::string(leaf) + "async fn g(): i32 {\n  return f()\n}\n"
                      "async fn main() { print(await g()) }\n").c_str(), "Awaitable",
                     "P15 Awaitable: `return f()` (missing await) is a type error");
        // Forgot-await, caught: printing an un-awaited async result would print a Task/Promise — refused.
        asyncRefuses((std::string(leaf) + "async fn main() { print(f()) }\n").c_str(), "await",
                     "P15 Awaitable: printing an un-awaited async result is refused");
        // Awaiting a plain value (not an async result) is a mistake — the only awaitables are async calls.
        asyncRefuses("fn plain(): i32 => 1\nasync fn main() {\n  let x = await plain()\n  print(x)\n}\n",
                     "not awaitable", "P15 Awaitable: awaiting a non-async call is refused");
    }

    // P16a — the position-indexed semantic model (LSP foundation, §4.8). `analyze()` returns a SemanticModel;
    // `definitionAt` maps a use site to its definition; `documentSymbols` lists file-local declarations.
    {
        const char* prog =
            "fn add(a: i32, b: i32): i32 => a + b\n"   // line 1
            "fn main() {\n"                            // line 2
            "  let x = add(1, 2)\n"                     // line 3: `add` call callee at col 11
            "  print(x)\n"                              // line 4: `x` use at col 9
            "}\n";
        AnalysisResult a = analyze(prog, nullptr, LibConfig{{"io", "math"}});
        check(a.diagnostics.empty(), "P16a: a clean program analyzes with no diagnostics");

        // go-to-def on the `add` call (line 3) resolves to the `fn add` definition on line 1.
        const SymbolDef* d = a.model.definitionAt(3, 11);
        check(d && d->name == "add" && d->kind == SymbolKind::Function && d->nameSpan.start.line == 1,
              "P16a: go-to-def on a function call resolves to its definition");

        // go-to-def on the `x` use (line 4) resolves to the local `let x` on line 3.
        const SymbolDef* lx = a.model.definitionAt(4, 9);
        check(lx && lx->name == "x" && lx->kind == SymbolKind::Local && lx->nameSpan.start.line == 3,
              "P16a: go-to-def on a local use resolves to its declaration");

        // a position with nothing resolvable returns nullptr (not a crash / false hit).
        check(a.model.definitionAt(2, 1) == nullptr, "P16a: definitionAt on a keyword returns nothing");

        // documentSymbols lists the file's functions, not its locals/parameters.
        bool hasAdd = false, hasMain = false, hasLocalOrParam = false;
        for (const SymbolDef* s : a.model.documentSymbols()) {
            if (s->name == "add")  hasAdd = true;
            if (s->name == "main") hasMain = true;
            if (s->kind == SymbolKind::Local || s->kind == SymbolKind::Parameter) hasLocalOrParam = true;
        }
        check(hasAdd && hasMain && !hasLocalOrParam,
              "P16a: documentSymbols lists functions and excludes locals/params");

        // `print` comes from the ambient lib (external) — it is not offered as a file-local definition.
        bool hasPrint = false;
        for (const SymbolDef* s : a.model.documentSymbols()) if (s->name == "print") hasPrint = true;
        check(!hasPrint, "P16a: an external (lib) symbol is not a file-local document symbol");
    }

    // P16 step 2 — type / member / value references. go-to-def on a construction, a member access, and a
    // top-level value; document symbols include types and members.
    {
        const char* prog =
            "const Scale: f64 = 2.0\n"                 // line 1
            "record Point(x: f64, y: f64)\n"           // line 2
            "fn main() {\n"                            // line 3
            "  let p = Point(1.0, 2.0)\n"               // line 4: `Point` at col 11
            "  print(p.x)\n"                            // line 5: `.x` member name at col 11
            "  print(Scale)\n"                          // line 6: `Scale` at col 9
            "}\n";
        AnalysisResult a = analyze(prog, nullptr, LibConfig{{"io", "math"}});
        check(a.diagnostics.empty(), "P16 step2: program with a record + value analyzes clean");

        const SymbolDef* ty = a.model.definitionAt(4, 11);
        check(ty && ty->name == "Point" && ty->kind == SymbolKind::Type && ty->nameSpan.start.line == 2,
              "P16 step2: go-to-def on a construction resolves to the type");

        const SymbolDef* fld = a.model.definitionAt(5, 11);
        check(fld && fld->name == "x" && fld->kind == SymbolKind::Field && fld->nameSpan.start.line == 2,
              "P16 step2: go-to-def on a member access resolves to the field");

        const SymbolDef* val = a.model.definitionAt(6, 9);
        check(val && val->name == "Scale" && val->kind == SymbolKind::Value && val->nameSpan.start.line == 1,
              "P16 step2: go-to-def on a value use resolves to its declaration");

        bool hasPoint = false, hasScale = false;
        for (const SymbolDef* s : a.model.documentSymbols()) {
            if (s->name == "Point") hasPoint = true;
            if (s->name == "Scale") hasScale = true;
        }
        check(hasPoint && hasScale, "P16 step2: documentSymbols includes the type and the value");
    }

    // P16 step 2 — a class method reference resolves to the method definition.
    {
        const char* prog =
            "class Counter {\n"                        // line 1
            "  fn tick(): i32 => 1\n"                   // line 2
            "}\n"
            "fn main() {\n"
            "  let c = Counter()\n"
            "  print(c.tick())\n"                       // line 6: `tick` method name at col 11
            "}\n";
        AnalysisResult a = analyze(prog, nullptr, LibConfig{{"io", "math"}});
        const SymbolDef* m = a.model.definitionAt(6, 11);
        check(m && m->name == "tick" && m->kind == SymbolKind::Method && m->nameSpan.start.line == 2,
              "P16 step2: go-to-def on a method call resolves to the method");
    }

    // P16 step 3 — the hand-written JSON reader that backs the language server's request parsing.
    {
        json::Value v = json::parse(R"({"a":1,"b":"hi\nA","c":[true,false,null],"d":{"e":3.5}})");
        check(v.kind == json::Value::Kind::Object, "json: parses an object");
        check(v["a"].asInt() == 1, "json: integer member");
        check(v["b"].asString() == "hi\nA", "json: string escapes + \\uXXXX");
        check(v["c"].items().size() == 3 && v["c"].items()[0].asBool(), "json: array + bool");
        check(v["c"].items()[2].isNull(), "json: null element");
        check(v["d"]["e"].asNumber() == 3.5, "json: nested object + number");
        check(v["missing"].isNull(), "json: absent member returns the Null sentinel");
        // a \uXXXX surrogate pair decodes to one code point (U+1F600 = the UTF-8 bytes F0 9F 98 80)
        check(json::parse(R"("😀")").asString() == "\xF0\x9F\x98\x80", "json: multibyte UTF-8 passes through");
        check(json::parse("\"\\uD83D\\uDE00\"").asString() == "\xF0\x9F\x98\x80", "json: \\u surrogate pair -> UTF-8");
        check(json::quote("a\"b\\c") == "\"a\\\"b\\\\c\"", "json: quote escapes \" and backslash");
    }

    // P16c — cross-module go-to-def: a reference to an imported symbol resolves to an EXTERNAL definition
    // tagged with its module's fileId, which the SourceMap names (the LSP turns that into a cross-file Location).
    {
        MapModuleResolver resolver({ {"./geo", "record Vec2(x: f64, y: f64)\n"} });
        const char* prog =
            "import { Vec2 } from \"./geo\"\n"       // line 1
            "fn main() {\n"                          // line 2
            "  let v = Vec2(1.0, 2.0)\n"              // line 3: `Vec2` call at col 11
            "  print(v.x)\n"
            "}\n";
        AnalysisResult a = analyze(prog, &resolver, LibConfig{{"io", "math"}}, "main.pg");

        const SymbolDef* d = a.model.definitionAt(3, 11);
        check(d && d->name == "Vec2" && d->kind == SymbolKind::Type && d->external,
              "P16c: a reference to an imported symbol resolves to an external definition");
        int fid = d ? d->nameSpan.start.fileId : 0;
        check(fid > 1 && a.sources.canon(fid) == "./geo",
              "P16c: the external def carries its module's fileId, named by the SourceMap");

        const SymbolDef* mainDef = nullptr;
        for (const auto& def : a.model.defs) if (def.name == "main") mainDef = &def;
        check(mainDef && !mainDef->external && mainDef->nameSpan.start.fileId == 1,
              "P16c: the entry file's own symbols are file-local (fileId 1)");
    }

    // P38/issue #69 slice 1 — compile() stamps real fileIds on the BUILD path (it passed a null SourceMap
    // before, so every token of every module was fileId 0 and a diagnostic from an imported module was
    // reported under the ENTRY file's name). The entry keeps id 1; the LSP's `fileId != 1` filter and this
    // test both depend on that invariant.
    {
        MapModuleResolver resolver({ {"./helper", "fn helperValue(): i32 => nope(1)\n"} });
        const char* prog =
            "import { helperValue } from \"./helper\"\n"
            "fn main() {\n"
            "  print(helperValue())\n"
            "}\n";
        EmitResult r = compile(prog, findTarget("csharp"), &resolver, LibConfig{{"io"}}, "entry.pg");
        check(!r.ok && !r.diagnostics.empty(), "P38 s1: an error inside an imported module still fails the build");
        const Diagnostic* d = r.diagnostics.empty() ? nullptr : &r.diagnostics.front();
        check(d && d->pos.fileId > 1, "P38 s1: the diagnostic carries the IMPORTED module's fileId, not 0");
        check(d && r.sources.canon(d->pos.fileId) == "./helper",
              "P38 s1: EmitResult::sources names the module the diagnostic came from");
        check(r.sources.canon(1) == "entry.pg", "P38 s1: the entry keeps fileId 1 (LSP filter invariant)");
    }
    {   // An entry-only error stays fileId 1, so single-file diagnostics are unchanged.
        EmitResult r = compile("fn main() { nope(1) }\n", findTarget("csharp"), nullptr, LibConfig{{"io"}}, "solo.pg");
        check(!r.ok && !r.diagnostics.empty() && r.diagnostics.front().pos.fileId == 1,
              "P38 s1: an entry-file diagnostic is fileId 1");
    }
    {   // No entryPath given (the historical call shape): still id 1, named "<entry>", never a crash.
        EmitResult r = compile("fn main() { print(1) }\n", findTarget("csharp"), nullptr, LibConfig{{"io"}});
        check(r.ok && r.sources.canon(1) == "<entry>", "P38 s1: an unnamed entry is stamped '<entry>'");
    }

    // P38/issue #69 slice 2 — ir::Method carries its declaration position, so the emitter can attribute a
    // method-entry sequence point to the .pg line (SP1 measured that this is what makes the declaration
    // line register a coverage hit). ClassField/Global need no equivalent: their `init` is an ir::Expr,
    // which is already positioned.
    {
        const char* prog =
            "class Counter {\n"                       // line 1
            "  var n: i32 = 7\n"                      // line 2: field initializer (an ir::Expr, positioned)
            "  fn bump(): i32 {\n"                    // line 3: the method declaration
            "    return this.n\n"
            "  }\n"
            "}\n"
            "fn main() {}\n"; // no std lib on this direct lex/parse/check path — `print` would be undeclared
        DiagnosticBag d;
        auto unit = parse(lex(prog, d, 1), d); // fileId 1 = the entry, as compile()/analyze() stamp it
        mintplayer::polyglot::check(unit, d);
        check(!d.hasErrors(), "P38 s2: the position-carrying program type-checks");
        ir::Module m = lower(unit, "csharp");
        const ir::Class* c = nullptr;
        for (const auto& cl : m.classes) if (cl.name == "Counter") c = &cl;
        check(c != nullptr, "P38 s2: the test class lowers");
        const ir::Method* bump = nullptr;
        if (c) for (const auto& mm : c->methods) if (mm.name == "bump") bump = &mm;
        check(bump && bump->pos.line == 3, "P38 s2: a lowered method carries its declaration line");
        check(bump && bump->pos.fileId == 1, "P38 s2: ... stamped with the entry's fileId");
        const ir::ClassField* f = nullptr;
        if (c) for (const auto& ff : c->fields) if (ff.name == "n") f = &ff;
        check(f && f->init && f->init->pos.line == 2,
              "P38 s2: a field initializer's origin comes from its already-positioned ir::Expr");
    }

    // P16c — find-references: symbolAt + referencesTo find every use of a symbol.
    {
        const char* prog =
            "fn twice(x: i32): i32 => x + x\n"    // line 1
            "fn main() {\n"                        // line 2
            "  let n = twice(2)\n"                 // line 3: `twice` call at col 11, `n` decl at col 7
            "  print(twice(n))\n"                  // line 4: `twice` call at col 9, `n` use at col 15
            "}\n";
        AnalysisResult a = analyze(prog, nullptr, LibConfig{{"io", "math"}}, "m.pg");
        int fn = a.model.symbolAt(3, 11);
        check(fn >= 0 && a.model.referencesTo(fn).size() == 2, "P16c refs: both calls to a function are found");
        int local = a.model.symbolAt(3, 7); // clicking the `n` declaration resolves to the same symbol
        check(local >= 0 && a.model.referencesTo(local).size() == 1, "P16c refs: the local's single use is found");
    }

    // P8 — List<T> as a first-party .pg std type, bound to each target via the FFI binding mechanism.
    {
        const char* prog =
            "import { List } from \"std.collections\"\n"
            "fn first(xs: List<i32>): i32 => xs[0]\n"
            "fn main() {\n"
            "  var xs: List<i32> = [1, 2]\n"
            "  xs.add(3)\n"
            "  print(xs.count)\n"
            "  print(first(xs))\n"
            "  xs.removeAll((v) => v < 2)\n"
            "  xs.clear()\n"
            "}\n";
        EmitResult cs = compileStd(prog, findTarget("csharp"));
        check(cs.ok, "P8: List program -> C# compiles");
        check(has(cs.code, "global::System.Collections.Generic.List<int>"), "P8 C#: List<i32> -> System.Collections.Generic.List<int>");
        check(has(cs.code, ".Add(3)"), "P8 C#: list.add -> .Add");
        check(has(cs.code, ".Count"), "P8 C#: list.count -> .Count");
        check(has(cs.code, ".RemoveAll("), "P8 C#: list.removeAll -> .RemoveAll");
        check(has(cs.code, "xs.Clear()"), "P8 C#: list.clear -> .Clear()");
        check(!has(cs.code, "class List"), "P8 C#: extern class List is not emitted");

        EmitResult ts = compileStd(prog, findTarget("typescript"));
        check(ts.ok, "P8: List program -> TS compiles");
        check(has(ts.code, "number[]"), "P8 TS: List<i32> -> number[]");
        check(has(ts.code, ".push(3)"), "P8 TS: list.add -> .push");
        check(has(ts.code, ".length"), "P8 TS: list.count -> .length");
        // Wave-2 G2: clear/removeAll mutate IN PLACE — a receiver reassignment (`xs = xs.filter(...)`)
        // is invisible through an alias or a fn parameter, diverging from C#/PHP (list_rebind_alias.pg).
        check(has(ts.code, "xs.splice(0, xs.length, ...xs.filter("), "P8 TS: list.removeAll -> in-place splice");
        check(has(ts.code, "xs.length = 0"), "P8 TS: list.clear -> in-place length reset");
        check(!has(ts.code, "xs = xs.filter(") && !has(ts.code, "xs = []"),
              "P8 TS: no receiver reassignment for clear/removeAll");
        check(!has(ts.code, "class List"), "P8 TS: extern class List is not emitted");
    }

    // P8 — importing an unknown std module is a diagnostic (real module resolution, not a silent no-op).
    rejects("import { Thing } from \"std.bogus\"\nfn main() {}\n", "P8: unknown std module is rejected");
    rejects("import { Nope } from \"std.collections\"\nfn main() {}\n", "P8: unknown imported name is rejected");

    // P8 — std.io: filesystem access via the capability mechanism (expect + per-target actual extern).
    {
        const char* prog =
            "import { readText, writeText } from \"std.io\"\n"
            "fn main() {\n"
            "  writeText(\"a.txt\", \"hi\")\n"
            "  print(readText(\"a.txt\"))\n"
            "}\n";
        EmitResult cs = compileStd(prog, findTarget("csharp"));
        check(cs.ok, "P8: std.io program -> C# compiles");
        check(has(cs.code, "global::System.IO.File.ReadAllText(path)"), "P8 C#: readText -> File.ReadAllText");
        check(has(cs.code, "global::System.IO.File.WriteAllText(path, content)"), "P8 C#: writeText -> File.WriteAllText");
        EmitResult ts = compileStd(prog, findTarget("typescript"));
        check(ts.ok, "P8: std.io program -> TS compiles");
        check(has(ts.code, "readFileSync(path"), "P8 TS: readText -> fs.readFileSync");
        check(has(ts.code, "writeFileSync(path, content)"), "P8 TS: writeText -> fs.writeFileSync");
    }

    // P12 — cross-.pg user-module resolution via a ModuleResolver (in-memory here; CLI uses the filesystem).
    {
        // Transitive: entry -> util -> base; entry and util both import base (deduped, no double-merge).
        MapModuleResolver r({
            {"base", "fn baseVal(): i32 => 40\n"},
            {"util", "import { baseVal } from \"base\"\nfn bump(x: i32): i32 => baseVal() + x\n"},
        });
        const char* prog =
            "import { bump } from \"util\"\n"
            "import { baseVal } from \"base\"\n"
            "fn main() { print(bump(2) + baseVal()) }\n"; // (40+2) + 40 = 82
        EmitResult cs = compile(prog, findTarget("csharp"), &r, LibConfig{{"io"}});
        check(cs.ok, "P12: transitive user-module resolution compiles");
        check(has(cs.code, "baseVal") && has(cs.code, "bump"), "P12: imported module decls are merged");
        EmitResult ts = compile(prog, findTarget("typescript"), &r, LibConfig{{"io"}});
        check(ts.ok, "P12: transitive resolution compiles to TS too");
    }
    {
        // Import cycle a -> b -> a is a clear diagnostic, not a hang.
        MapModuleResolver r({
            {"a", "import { fromB } from \"b\"\nfn fromA(): i32 => fromB()\n"},
            {"b", "import { fromA } from \"a\"\nfn fromB(): i32 => fromA()\n"},
        });
        EmitResult res = compile("import { fromA } from \"a\"\nfn main() { print(fromA()) }\n", findTarget("csharp"), &r);
        bool cyclic = false;
        for (const auto& d : res.diagnostics) if (has(d.message, "import cycle")) cyclic = true;
        check(!res.ok && cyclic, "P12: an import cycle is reported (no hang)");
    }
    {
        // An unknown user module (resolver returns nullopt) is an "unknown module" diagnostic.
        MapModuleResolver r({});
        EmitResult res = compile("import { x } from \"missing\"\nfn main() {}\n", findTarget("csharp"), &r);
        bool unknown = false;
        for (const auto& d : res.diagnostics) if (has(d.message, "unknown module")) unknown = true;
        check(!res.ok && unknown, "P12: an unresolvable user module is rejected");
    }

    // P12 — collisions are refused, never a silent last-wins overwrite (the §3 "never miscompile" law).
    rejects("let x: i32 = 1\nlet x: i32 = 2\nfn main() {}\n", "P12: duplicate top-level value is rejected");
    rejects("union U { A, A }\nfn main() {}\n", "P12: duplicate union case is rejected");
    rejects("extension fn i32.dup(): i32 => 1\nextension fn i32.dup(): i32 => 2\nfn main() {}\n",
            "P12: duplicate extension is rejected");
    {
        // A type defined in both the entry and an imported module collides (no silent merge).
        MapModuleResolver r({{"m", "record R(x: i32)\n"}});
        EmitResult res = compile("import { R } from \"m\"\nrecord R(y: i32)\nfn main() {}\n", findTarget("csharp"), &r);
        bool dup = false;
        for (const auto& d : res.diagnostics) if (has(d.message, "duplicate type")) dup = true;
        check(!res.ok && dup, "P12: a cross-module type collision is rejected");
    }

    // P13 — the `lib` prelude: auto-import std modules, ambient + silently shadowable.
    {
        LibConfig lib{{"collections", "io"}}; // io so the programs' `print(...)` resolves via the prelude too
        // (1) List usable with NO explicit import when 'collections' is in lib; it links the real module
        //     (so `xs.count` lowers to the .Count binding, not the lenient bare `.count`).
        EmitResult r = compile("fn main() { var xs: List<i32> = [1]\n  print(xs.count) }\n",
                               findTarget("csharp"), nullptr, lib);
        check(r.ok && has(r.code, ".Count"), "P13: lib auto-imports std.collections (List.count -> .Count)");
        // (2) a user declaration silently shadows the lib's same-named one (NO collision error).
        EmitResult sh = compile("record List(x: i32)\nfn main() { print(List(5).x) }\n",
                                findTarget("csharp"), nullptr, lib);
        check(sh.ok, "P13: a user decl silently shadows the lib prelude (no collision)");
        // (3) an explicit import of a lib module dedups against the lib auto-import (no collision).
        EmitResult dd = compile("import { List } from \"std.collections\"\nfn main() { var xs: List<i32> = [1]\n  print(xs.count) }\n",
                                findTarget("csharp"), nullptr, lib);
        check(dd.ok, "P13: explicit import + lib dedups (no collision)");
        // (4) a QUALIFIED lib entry is a full specifier resolved like any import (third-party plugin
        //     namespace, not just std) — `app.helpers` resolves through the resolver, used un-imported.
        MapModuleResolver plug({{"app.helpers", "fn helper(): i32 => 42\n"}});
        LibConfig pluginLib{{"app.helpers", "io"}};
        EmitResult pl = compile("fn main() { print(helper()) }\n", findTarget("csharp"), &plug, pluginLib);
        check(pl.ok && has(pl.code, "helper()"), "P13: a qualified lib entry auto-imports a non-std (plugin) module");
    }

    // P13 — a user-authored "plugin class" (extern class + per-target binding arms) works in a .pg file:
    // member/property bindings fire (the $this/$0 mechanism is general), and the extern class isn't emitted.
    {
        const char* prog =
            "extern class Widget {\n"
            "  fn poke(n: i32) { actual(csharp) extern(\"$this.Poke($0)\")  actual(typescript) extern(\"$this.poke($0)\") }\n"
            "}\n"
            "fn drive(w: Widget): i32 { w.poke(3)\n  return 0 }\n"
            "fn main() {}\n";
        EmitResult cs = compile(prog, findTarget("csharp"));
        check(cs.ok && has(cs.code, "w.Poke(3)") && !has(cs.code, "class Widget"),
              "P13: user plugin class member binding fires in C# (extern class not emitted)");
        EmitResult ts = compile(prog, findTarget("typescript"));
        check(ts.ok && has(ts.code, "w.poke(3)") && !has(ts.code, "class Widget"),
              "P13: user plugin class member binding fires in TS");
    }

    // P13 — STATIC bindings on an extern class: a static method call and a static const both fire their
    // per-target template (no receiver — only $0,$1,…), enabling Math.sqrt(x)/Math.PI as a std module.
    {
        const char* prog =
            "extern class MyMath {\n"
            "  static fn twice(n: i32): i32 { actual(csharp) extern(\"($0 * 2)\")  actual(typescript) extern(\"($0 * 2)\") }\n"
            "  const K: i32 { actual(csharp) extern(\"42\")  actual(typescript) extern(\"42\") }\n"
            "}\n"
            "fn main() { print(MyMath.twice(5))\n  print(MyMath.K) }\n";
        // twice(5) inlines to (5 * 2); the static const K inlines to 42 (passed to the print free function).
        EmitResult cs = compileStd(prog, findTarget("csharp"));
        check(cs.ok && has(cs.code, "(5 * 2)") && has(cs.code, "PolyglotProgram.print(42)") && !has(cs.code, "class MyMath"),
              "P13: static method + static const bindings fire on an extern class (C#)");
        EmitResult ts = compileStd(prog, findTarget("typescript"));
        check(ts.ok && has(ts.code, "(5 * 2)") && !has(ts.code, "class MyMath"),
              "P13: static bindings fire on an extern class (TS)");
    }

    // P13 — TypeArg inference: a generic call's return type is the inferred argument type, not a bare `T`.
    // Checked on the typed IR (`call(args):returnType`); the emitted-code correctness is covered by the
    // differential gate. The canary is the substituted return type on the call node.
    {
        auto callIr = [&](const char* src) -> std::string {
            DiagnosticBag d;
            auto unit = parse(lex(src, d), d);
            mintplayer::polyglot::check(unit, d);
            if (d.hasErrors()) return "<type error>";
            return ir::dump(lower(unit, "csharp"));
        };
        // single type-arg: pick<T>(T,T):T applied to i64 -> the call node is typed i64, not a bare T.
        std::string one = callIr("fn pick<T>(a: T, b: T): T => a\nfn use1(): i64 => pick(1i64, 2i64)\n");
        check(has(one, "pick(1:i64, 2:i64):i64"), "P13: single type-arg return inferred (i64)");
        // two type-args, return the SECOND (V=i64): inference must bind V, not just the first param.
        std::string two = callIr("fn snd<K, V>(k: K, v: V): V => v\nfn use2(): i64 => snd(1i32, 2i64)\n");
        check(has(two, "snd(1:i32, 2:i64):i64"), "P13: 2nd-of-two type-args return inferred (i64)");
        // return the FIRST (K=i32) while the 2nd arg is i64: the result is i32, not i64.
        std::string fst = callIr("fn fst<K, V>(k: K, v: V): K => k\nfn use3(): i32 => fst(1i32, 2i64)\n");
        check(has(fst, "fst(1:i32, 2:i64):i32"), "P13: 1st-of-two type-args return inferred (i32)");
    }

    // P13 follow-up — inherited member resolution: a subclass reaches a base's member (findMember walks bases).
    resolves("open class Base {\n  let id: i32\n  constructor() { this.id = 0 }\n}\n"
             "class Sub : Base {\n  constructor() { super() }\n}\n"
             "fn f(s: Sub): i32 => s.id\n", "P13: a subclass reaches an inherited base member");

    // P13 follow-up — `Error.message`: resolves on an `: Error` subclass (base walk) and binds per target
    // (C# `.Message` on System.Exception vs JS `.message`), since `Error` isn't a source `extern class`.
    {
        const char* prog =
            "class MyErr : Error {\n  constructor(message: string) { super(message) }\n}\n"
            "fn describe(e: MyErr): string => e.message\n"
            "fn main() {}\n";
        EmitResult cs = compileStd(prog, findTarget("csharp"));
        check(cs.ok && has(cs.code, "e.Message"), "P13: Error.message resolves on a subclass and binds to C# .Message");
        EmitResult ts = compileStd(prog, findTarget("typescript"));
        check(ts.ok && has(ts.code, "e.message"), "P13: Error.message binds to JS .message");
    }

    // P13 follow-up — an extension on a generic receiver binds the receiver's type variable: `List<T>` lifts
    // `T` into the extension's generics, so the signature/return `T?` resolve and both emitters carry `<T>`.
    {
        const char* prog =
            "import { List } from \"std.collections\"\n"
            "extension fn List<T>.firstOrNull(): T? => if this.count >= 1 { this[0] } else { null }\n"
            "fn head(xs: List<i32>): i32 => xs.firstOrNull() ?? -1\n"
            "fn main() {}\n";
        EmitResult cs = compileStd(prog, findTarget("csharp"));
        check(cs.ok && has(cs.code, "firstOrNull<T>("), "P13: extension on a generic receiver scopes T (C# generic signature)");
        EmitResult ts = compileStd(prog, findTarget("typescript"));
        check(ts.ok && has(ts.code, "firstOrNull<T>("), "P13: extension on a generic receiver scopes T (TS generic signature)");
    }

    // P10 — a user `extern class` declares its per-target type spelling (`type { … }`) and construction
    // (`init` binding arms); `$T` in a ctor template is the mapped type. The class itself isn't emitted.
    {
        const char* prog =
            "extern class Widget {\n"
            "  type {\n    actual(csharp)     extern(\"global::Acme.Widget\")\n    actual(typescript) extern(\"Widget\")\n  }\n"
            "  constructor(n: i32) {\n    actual(csharp)     extern(\"new $T($0)\")\n    actual(typescript) extern(\"new $T($0)\")\n  }\n"
            "}\n"
            "fn make(): Widget => Widget(7)\n"
            "fn main() {}\n";
        EmitResult cs = compileStd(prog, findTarget("csharp"));
        check(cs.ok && has(cs.code, "global::Acme.Widget make()") && has(cs.code, "new global::Acme.Widget(7)")
                    && !has(cs.code, "class Widget"),
              "P10: user extern class maps its type + constructs ($T) in C#");
        EmitResult ts = compileStd(prog, findTarget("typescript"));
        check(ts.ok && has(ts.code, "make(): Widget") && has(ts.code, "new Widget(7)") && !has(ts.code, "class Widget"),
              "P10: user extern class maps its type + constructs ($T) in TS");
    }

    // P10 — the std `List` type spelling + construction are now declared on its `extern class` (dogfood), not
    // hardcoded: List<T> -> C# System...List<int> / TS number[]; `[..]` literal + `List<T>()` ctor follow.
    {
        const char* prog =
            "import { List } from \"std.collections\"\n"
            "fn mk(): List<i32> { var xs: List<i32> = [1]\n  return xs }\n"
            "fn main() {}\n";
        EmitResult cs = compileStd(prog, findTarget("csharp"));
        check(cs.ok && has(cs.code, "global::System.Collections.Generic.List<int>"),
              "P10: List type spelling comes from its extern-class mapping (C#)");
        EmitResult ts = compileStd(prog, findTarget("typescript"));
        check(ts.ok && has(ts.code, "number[]"), "P10: List<i32> -> number[] from the mapping (TS)");
    }

    // P14 slice 1 — generic discriminated unions: `union Box<T>` declares + constructs (type inferred) +
    // matches, byte-identical across targets (C# generic record hierarchy, TS generic tagged union).
    {
        const char* prog =
            "union Box<T> { Full(value: T), Empty }\n"
            "fn unwrap<T>(b: Box<T>, dflt: T): T => match b { Full(v) => v, Empty => dflt }\n"
            "fn main() { print(unwrap(Full(5), 0)) }\n";
        EmitResult cs = compileStd(prog, findTarget("csharp"));
        check(cs.ok && has(cs.code, "abstract record Box<T>") && has(cs.code, "sealed record Full<T>(T value) : Box<T>")
                    && has(cs.code, "new Full<int>(5)") && has(cs.code, "Full<T>(var v)"),
              "P14: generic union — C# record hierarchy + typed construction + typed pattern");
        EmitResult ts = compileStd(prog, findTarget("typescript"));
        check(ts.ok && has(ts.code, "type Box<T> = { tag: \"Full\"; value: T } | { tag: \"Empty\" }")
                    && has(ts.code, "{ tag: \"Full\" as const, value: 5 }"),
              "P14: generic union — TS tagged union + construction");
    }

    // P14 slice 2 — Option<T> is a core-prelude generic union (no import): Some/None construct and match,
    // and a bare `None` takes its <T> from context (the Option<i32> return) -> C# `new None<int>()`.
    {
        const char* prog =
            "fn opt(b: bool, v: i32): Option<i32> => if b { Some(v) } else { None }\n"
            "fn orElse(o: Option<i32>, d: i32): i32 => match o { Some(x) => x, None => d }\n"
            "fn main() { print(orElse(opt(false, 9), -1)) }\n";
        // compileStd libs io (for print)/math/collections but NOT any "option" module — so Option resolving
        // proves it's in the always-linked core prelude.
        EmitResult cs = compileStd(prog, findTarget("csharp"));
        check(cs.ok && has(cs.code, "new Some<int>(v)") && has(cs.code, "new None<int>()"),
              "P14: core Option — Some/None construct in C# (None typed from context)");
        EmitResult ts = compileStd(prog, findTarget("typescript"));
        check(ts.ok && has(ts.code, "{ tag: \"Some\" as const, value: v }") && has(ts.code, "{ tag: \"None\" as const }"),
              "P14: core Option — Some/None construct in TS");
    }

    // P14 slice 3 — the `T?` sugar: `T?` over a generic desugars to Option<T>; a bare value coerces to Some,
    // `null` to None; `?? d` lowers to a match. Plus extension-receiver inference (List<i32> -> T=i32).
    {
        const char* prog =
            "fn pickIf<T>(c: bool, v: T): T? => if c { v } else { null }\n"
            "fn main() { print(pickIf(true, 7) ?? -1) }\n";
        EmitResult cs = compileStd(prog, findTarget("csharp"));
        check(cs.ok && has(cs.code, "Option<T> pickIf<T>") && has(cs.code, "new Some<T>(v)") && has(cs.code, "new None<T>()")
                    && has(cs.code, "Some<int>(var __opt0) => __opt0") && has(cs.code, "None<int> _ => -1"),
              "P14: T? sugar — desugars to Option, ?? lowers to match (C#)");
        EmitResult ts = compileStd(prog, findTarget("typescript"));
        check(ts.ok && has(ts.code, "_m.tag === \"Some\"") && has(ts.code, "_m.tag === \"None\""),
              "P14: T? sugar — ?? lowers to a tagged match (TS)");
    }
    {
        // Extension on a generic receiver returning T? -> the call infers T from the receiver (Option<int>).
        const char* prog =
            "import { List } from \"std.collections\"\n"
            "extension fn List<T>.secondOrNull(): T? => if this.count >= 2 { this[1] } else { null }\n"
            "fn head2(xs: List<i32>): i32 => xs.secondOrNull() ?? -1\n"
            "fn main() {}\n";
        EmitResult cs = compileStd(prog, findTarget("csharp"));
        check(cs.ok && has(cs.code, "Some<int>(var __opt0)"),
              "P14: extension returning T? — receiver inference gives Option<int> (C#)");
    }

    // P14b — bidirectional typing: an empty list literal takes its element type from the target slot, so it
    // emits `List<int>` (valid), not `List<object>` (a CS1503 miscompile). Also a match binding is typed
    // precisely from the scrutinee (no longer Unknown).
    {
        const char* prog =
            "import { List } from \"std.collections\"\n"
            "fn mk(): List<i32> { var xs: List<i32> = []\n  return xs }\n"
            "fn main() {}\n";
        EmitResult cs = compileStd(prog, findTarget("csharp"));
        check(cs.ok && has(cs.code, "new global::System.Collections.Generic.List<int> {") && !has(cs.code, "List<object>"),
              "P14b: empty list literal takes its element type from the target (List<int>, not List<object>)");
    }

    // Issue #27 Bug 2 — a collection spelled with a POSTFIX operator must parenthesize a union element type,
    // or the target mis-parses it. TS `List<T>` is `$0[]`, and `[]` binds tighter than the `|` of a nullable
    // union: `List<Node?>` must render `(Node | null)[]`, never `Node | null[]` (which TS reads as
    // `Node | (null[])`). C# is safe — its `List<…>` is angle-bracketed and self-delimiting. This is the
    // general "collections compose with union element types" invariant. (Issue #27 Bug 1 — un-inferable
    // initializers like `[]`/`null` losing their element type — is addressed at the source language level by
    // the explicit-typing spec change, see docs/prd/issue-27-explicit-typing/.)
    {
        const char* prog =
            "import { List } from \"std.collections\"\n"
            "class Node {\n  var next: List<Node?>\n  constructor() { this.next = List<Node?>() }\n}\n"
            "fn deltas(): List<(i32, i32)> {\n"
            "  var d: List<(i32, i32)> = List<(i32, i32)>()\n"
            "  d.add((1, 2))\n"
            "  return d\n"
            "}\n"
            "fn main() {}\n";
        EmitResult ts = compileStd(prog, findTarget("typescript"));
        check(ts.ok, "issue#27: repro program transpiles to TS");
        // The nullable-union element is parenthesized inside the postfix array; the mis-parenthesized form is gone.
        check(has(ts.code, "next: (Node | null)[];") && !has(ts.code, "Node | null[]"),
              "issue#27 Bug2: List<T?> field renders as (Node | null)[]");
        // Non-nullable arrays stay clean — no stray parens leak in from the nullable fix.
        check(has(ts.code, "deltas(): [number, number][]") && !has(ts.code, "([number, number])[]"),
              "issue#27 Bug2: non-nullable array element types keep no stray parentheses");
        // Bug 1 (Slice 2) — the `List<(i32,i32)>()` construction spells no element type on TS (`List.init` is
        // `[]`), so the DECLARATION carries it: `let d: [number, number][] = [];` (not the evolving-any `let d = [];`).
        check(has(ts.code, "let d: [number, number][] = [];") && !has(ts.code, "let d = [];"),
              "issue#27 Bug1: type-erased List<T>() construction carries its type on the declaration (TS)");
        // C#: `new List<…>()` carries the element type in the initializer; since wave 2 the EXPLICIT
        // source annotation also always emits on the declaration (declExplicit — a stamped initializer
        // type can hide the annotation's information on other shapes), so the local is typed, not `var`.
        EmitResult cs = compileStd(prog, findTarget("csharp"));
        check(cs.ok && has(cs.code, "global::System.Collections.Generic.List<Node?>"),
              "issue#27 Bug2: C# nullable list element is self-delimiting (List<Node?>)");
        check(has(cs.code, "List<(int, int)> d = new global::System.Collections.Generic.List<(int, int)>();"),
              "issue#27 Bug1: C# List construction keeps its element type (typed decl since wave 2)");
    }

    // Issue #27 Bug 1 (root-cause language rule) — a `let`/`var` whose initializer is UN-INFERABLE must
    // carry an explicit type annotation; otherwise no target can reconstruct the declared type and it would
    // silently degrade (C# `List<object>` / TS evolving-`any`). The un-inferable forms are an empty list
    // literal, a bare `null`, and a lambda with un-annotated parameters. Every UNAMBIGUOUS initializer keeps
    // inferring with no annotation. (docs/prd/issue-27-explicit-typing/)
    {
        auto rejectsStd = [&](const char* src, const std::string& name) {
            EmitResult r = compileStd(src, findTarget("csharp"));
            check(!r.ok && !r.diagnostics.empty(), name);
        };
        auto acceptsStd = [&](const char* src, const std::string& name) {
            EmitResult r = compileStd(src, findTarget("csharp"));
            check(r.ok, name);
        };
        // Rejected — un-inferable initializer, no annotation.
        rejectsStd("fn main() { var d = [] }\n",            "issue#27 Bug1: empty list literal without annotation is rejected");
        rejectsStd("fn main() { var x = null }\n",          "issue#27 Bug1: bare null without annotation is rejected");
        rejectsStd("fn main() { let f = (x) => x }\n",      "issue#27 Bug1: lambda with un-annotated params without annotation is rejected");
        // The diagnostic is the fixit-shaped 'cannot infer the type' message (not some unrelated error).
        {
            EmitResult r = compileStd("fn main() { var d = [] }\n", findTarget("csharp"));
            bool named = false;
            for (const auto& d : r.diagnostics) if (has(d.message, "cannot infer the type of 'd'")) named = true;
            check(named, "issue#27 Bug1: diagnostic names the binding and asks for an annotation");
        }
        // Accepted — annotated, or an unambiguous initializer that infers precisely.
        acceptsStd("fn main() { var d: List<i32> = [] }\n", "issue#27 Bug1: empty list WITH annotation is accepted");
        acceptsStd("fn main() { let e = [1, 2, 3] }\n",     "issue#27 Bug1: non-empty list infers without annotation");
        acceptsStd("fn main() { var d = List<i32>() }\n",   "issue#27 Bug1: List<i32>() constructor infers without annotation");
        acceptsStd("fn main() { var x: i32? = null }\n",    "issue#27 Bug1: null WITH annotation is accepted");
    }

    // Issue #27 Bug 1 (Slice 2, emission) — for a type-erased construction or a bare `null`, the emitter puts
    // the type on the declaration via `localDeclTyped`. This is target-driven: it fires only where the bound
    // template is a bare constant (TS `List.init` == `[]`) or the init is `null`, and only on targets that
    // declare a `localDeclTyped` row (C#/TS). No `as`-cast is synthesized.
    {
        // A constructor local with no source annotation (inferred List<i32>) still carries its type on TS.
        EmitResult ts = compileStd("fn main() { var d = List<i32>()\n  d.add(1) }\n", findTarget("typescript"));
        check(ts.ok && has(ts.code, "let d: number[] = [];") && !has(ts.code, "let d = [];"),
              "issue#27 Bug1: inferred List<i32>() local carries its type on the declaration (TS)");
        // A null local keeps its declared (parenthesized nullable) type on TS.
        EmitResult tn = compileStd("class Node {\n  var v: i32\n  constructor() { this.v = 0 }\n}\n"
                                   "fn main() { var x: Node? = null\n  x = Node() }\n", findTarget("typescript"));
        check(tn.ok && has(tn.code, "let x: (Node | null) = null;") && !has(tn.code, "let x = null;"),
              "issue#27 Bug1: null local keeps its declared type on TS");
        // A non-empty literal local is unchanged — its type is inferable from the initializer (no annotation added).
        EmitResult te = compileStd("fn main() { let e = [1, 2, 3] }\n", findTarget("typescript"));
        check(te.ok && has(te.code, "const e = [1, 2, 3];"),
              "issue#27 Bug1: inferable local keeps the idiomatic inferred form (no annotation churn)");
    }

    // Issue #27 (Slice 3) — the fixed-size array type `T[]` (sugar for the core `Array<T>`). Both an array and
    // a `List<T>` transpile to a JS array on TS; on C# the array is a distinct `T[]` (vs `List<T>`). Element
    // get, `.count`, iteration, and a nullable array element compose; size-mutation (`.add`) is a type error.
    {
        const char* prog =
            "fn nums(): i32[] { var a: i32[] = [1, 2, 3]\n  return a }\n"
            "fn nullables(): Node?[] { var xs: Node?[] = []\n  return xs }\n"
            "fn firstNum(): i32 => nums()[0]\n"
            "fn n(): i32 => nums().count\n"
            "class Node { var v: i32\n  constructor() { this.v = 0 } }\n"
            "fn main() {}\n";
        EmitResult cs = compileStd(prog, findTarget("csharp"));
        check(cs.ok && has(cs.code, "int[] nums()") && has(cs.code, "new int[] { 1, 2, 3 }")
                    && has(cs.code, "Node?[] nullables()") && has(cs.code, ".Length"),
              "issue#27 Slice3: C# array is a distinct T[] (int[], new int[]{…}, Node?[], .Length)");
        // C# must NOT emit a List for an array-typed literal.
        check(cs.ok && !has(cs.code, "new global::System.Collections.Generic.List<int> { 1, 2, 3 }"),
              "issue#27 Slice3: C# array literal is not a List<int>");
        EmitResult ts = compileStd(prog, findTarget("typescript"));
        check(ts.ok && has(ts.code, "nums(): number[]") && has(ts.code, "nullables(): (Node | null)[]")
                    && has(ts.code, ".length"),
              "issue#27 Slice3: TS array is a JS array (number[]), nullable element parenthesizes ((Node|null)[])");
        EmitResult py = compileStd(prog, findTarget("python"));
        check(py.ok, "issue#27 Slice3: array program transpiles to Python");
        EmitResult php = compileStd(prog, findTarget("php"));
        check(php.ok, "issue#27 Slice3: array program transpiles to PHP");
        // Fixed-size contract: size-mutation is rejected (Array declares no `add`).
        EmitResult bad = compileStd("fn main() { var a: i32[] = [1, 2]\n  a.add(3) }\n", findTarget("csharp"));
        check(!bad.ok && !bad.diagnostics.empty(), "issue#27 Slice3: .add on a fixed-size array is rejected");
    }

    // P14b — interfaces are emitted (were dropped at lowering), a record's `: Interface` base/implements
    // clause is carried, and `operator fn get` is a real indexer (C# `this[]`, TS `get(i)` method + `.get()`).
    {
        const char* prog =
            "interface Shrinkable { fn shrink(): i32 }\n"
            "record Box(n: i32) : Shrinkable {\n  fn shrink(): i32 => this.n - 1\n}\n"
            "class Bag {\n  operator fn get(i: i32): i32 => i\n}\n"
            "fn at(b: Bag): i32 => b[3]\n"
            "fn main() {}\n";
        EmitResult cs = compileStd(prog, findTarget("csharp"));
        check(cs.ok && has(cs.code, "interface Shrinkable") && has(cs.code, "record Box(int n) : Shrinkable")
                    && has(cs.code, "public int this[int i] =>"),
              "P14b: C# emits interface + record base + this[] indexer");
        EmitResult ts = compileStd(prog, findTarget("typescript"));
        check(ts.ok && has(ts.code, "interface Shrinkable") && has(ts.code, "class Box implements Shrinkable")
                    && has(ts.code, "b.get(3)"),
              "P14b: TS emits interface + implements + get() indexer access");
    }

    // P14b — std.strings: a bound extension method on `string` (a method on a builtin type via the binding
    // mechanism). `s.toUpper()` lowers to the per-target template, not a literal `.toUpper()` call.
    {
        const char* prog =
            "import \"std.strings\"\n"
            "fn shout(s: string): string => s.toUpper()\n"
            "fn main() {}\n";
        EmitResult cs = compileStd(prog, findTarget("csharp"));
        check(cs.ok && has(cs.code, "s.ToUpperInvariant()") && !has(cs.code, "s.toUpper()"),
              "P14b: bound string extension -> C# .ToUpperInvariant() (P40 A3: culture-free)");
        EmitResult ts = compileStd(prog, findTarget("typescript"));
        check(ts.ok && has(ts.code, "s.toUpperCase()"), "P14b: bound string extension -> JS .toUpperCase()");
    }

    // P10 — the core prelude (Error/Iterable) is ALWAYS linked (no import, no lib): with a bare `compile`,
    // Error constructs + maps per target and `.message` binds — proving they're core-prelude extern classes,
    // not emitter hardcodes.
    {
        const char* prog =
            "fn boom(): Error => Error(\"x\")\n"
            "fn msg(e: Error): string => e.message\n"
            "fn main() {}\n";
        EmitResult cs = compile(prog, findTarget("csharp"));
        check(cs.ok && has(cs.code, "new global::System.Exception(\"x\")") && has(cs.code, "e.Message"),
              "core prelude: Error constructs + .message maps to C# (no import/lib)");
        EmitResult ts = compile(prog, findTarget("typescript"));
        check(ts.ok && has(ts.code, "new Error(\"x\")") && has(ts.code, "e.message"),
              "core prelude: Error maps to JS (no import/lib)");
    }

    // P13 — unknown/unimported types fail compilation, not just in signatures but in LOCAL positions too
    // (previously a local `let x: T`/`var xs: List<…>` slipped, silently miscompiling).
    rejects("fn main() { let w: Widget = 0 }\n", "P13: unknown type on a local `let` is rejected");
    rejects("fn main() { var xs: List<i32> = [1] }\n", "P13: List used without importing std.collections is rejected");

    // A normal unknown type still gets the plain diagnostic (not a refusal).
    {
        EmitResult r = compile("fn f(x: Widget) {}\n", findTarget("csharp"));
        bool plain = false;
        for (const auto& d : r.diagnostics) if (has(d.message, "unknown type")) plain = true;
        check(!r.ok && plain, "P6: a non-refused unknown type still says 'unknown type'");
    }

    // P18 slice 1 — BackendSpec loads from / serializes to JSON (the tabular ~70% becomes data; PRD §4.10).
    {
        // A representative spec parses; the fields land where expected; blockStyle maps.
        const char* js = R"({ "name":"demo", "scalarType":{"i32":"int"}, "intSuffix":{"i64":"L"},
            "binaryOp":{"==":"==="}, "delimited":{"tuple":{"open":"[","sep":", ","close":"]"}},
            "blockStyle":"colonIndent", "stmtEnd":"", "throwKeyword":"raise",
            "trueLit":"True","falseLit":"False","nullLit":"None" })";
        SpecLoadResult r = loadBackendSpec(js);
        check(r.ok && r.spec.name == "demo" && r.spec.scalarType.at("i32") == "int" &&
              r.spec.binOp("==") == "===" && r.spec.blockStyle == BlockStyle::ColonIndent &&
              r.spec.stmtEnd == "" && r.spec.throwKeyword == "raise" && r.spec.nullLit == "None" &&
              r.spec.delimited.at("tuple").open == "[",
              "P18: loadBackendSpec parses fields + blockStyle enum");

        // Missing name / unknown blockStyle fail loudly (never a silent bad spec).
        check(!loadBackendSpec(R"({"scalarType":{}})").ok, "P18: spec without 'name' is rejected");
        check(!loadBackendSpec(R"({"name":"x","blockStyle":"squiggly"})").ok, "P18: unknown blockStyle is rejected");

        // Round-trip: serialize → parse → equal struct.
        SpecLoadResult back = loadBackendSpec(backendSpecToJson(r.spec));
        check(back.ok && back.spec.name == r.spec.name &&
              back.spec.scalarType == r.spec.scalarType && back.spec.intSuffix == r.spec.intSuffix &&
              back.spec.binaryOp == r.spec.binaryOp && back.spec.blockStyle == r.spec.blockStyle &&
              back.spec.stmtEnd == r.spec.stmtEnd && back.spec.throwKeyword == r.spec.throwKeyword &&
              back.spec.trueLit == r.spec.trueLit && back.spec.falseLit == r.spec.falseLit &&
              back.spec.nullLit == r.spec.nullLit &&
              back.spec.delimited.at("tuple").open == r.spec.delimited.at("tuple").open,
              "P18: BackendSpec round-trips through JSON");
    }

    // P18 slice 3 — the emission-DSL interpreter spine (Design A, PRD §4.10): literal / tmpl / get / case+Test / fn.
    {
        MockEval ctx;
        ctx.fields["node.op"] = "+";
        ctx.fields["node.name"] = "widget";
        ctx.fields["node.kind"] = "Binary";
        // node.guard is absent -> Test has:false

        check(runRule(R"("hello")", ctx) == "hello", "P18: interpreter literal");
        check(runRule(R"({"get":"node.op"})", ctx) == "+", "P18: interpreter get");
        check(runRule(R"({"tmpl":["a ",{"get":"node.op"}," b"]})", ctx) == "a + b", "P18: interpreter tmpl concat");
        check(runRule(R"({"fn":"upper","args":[{"get":"node.name"}]})", ctx) == "WIDGET", "P18: interpreter fn builtin");
        check(runRule(R"({"fn":"join","args":[", ",{"get":"node.name"},{"get":"node.op"}]})", ctx) == "widget, +",
              "P18: interpreter fn with multiple args");
        // case: first matching arm wins; else fallback.
        const char* caseRule =
            R"({"case":{"when":[[{"eq":["node.kind","Unary"]},"U"],[{"eq":["node.kind","Binary"]},"B"]],"else":"?"}})";
        check(runRule(caseRule, ctx) == "B", "P18: interpreter case picks matching arm");
        ctx.fields["node.kind"] = "Call";
        check(runRule(caseRule, ctx) == "?", "P18: interpreter case falls to else");
        // Test combinators: has + not + and.
        check(runRule(R"({"case":{"when":[[{"has":"node.op"},"has-op"]],"else":"no"}})", ctx) == "has-op",
              "P18: interpreter Test has");
        check(runRule(R"({"case":{"when":[[{"not":{"has":"node.guard"}},"unguarded"]],"else":"g"}})", ctx) == "unguarded",
              "P18: interpreter Test not+has");
        check(runRule(R"({"case":{"when":[[{"and":[{"has":"node.op"},{"eq":["node.op","+"]}]},"plus"]],"else":"x"}})", ctx) == "plus",
              "P18: interpreter Test and");
        // --- Arm coverage (docs/prd/code-coverage-upload) ----------------------------------------
        {
            // Source provenance: the JSON parser stamps every value with its byte offset, and
            // LineIndex converts offsets to 1-based lines at the reporting boundary.
            const std::string doc = "{\n  \"a\": 1,\n  \"b\": [\n    2\n  ]\n}";
            const json::Value v = json::parse(doc);
            const json::LineIndex idx(doc);
            check(idx.lineAt(v.offset) == 1, "coverage: root value on line 1");
            check(idx.lineAt(v["a"].offset) == 2, "coverage: nested member offset -> line 2");
            check(idx.lineAt(v["b"].items()[0].offset) == 4, "coverage: array element offset -> line 4");
            check(idx.lineAt(0) == 1 && idx.lineAt(doc.size() + 99) >= 1, "coverage: LineIndex clamps");

            // A case rule with two arms, one per line. indexRule collects the DENOMINATOR from the
            // same parse that produces the rule, so an arm that never fires is still counted.
            const std::string ruleText =
                "{\n"
                "  \"case\": {\n"
                "    \"when\": [\n"
                "      [{\"eq\": [\"node.kind\", \"Unary\"]}, \"U\"],\n"
                "      [{\"eq\": [\"node.kind\", \"Binary\"]}, \"B\"]\n"
                "    ],\n"
                "    \"else\": \"?\"\n"
                "  }\n"
                "}";
            bool ok = true;
            std::string err;
            const json::Value ruleJson = json::parse(ruleText);
            engine::Rule rule = engine::parseRule(ruleJson, ok, err);
            check(ok, "coverage: case rule parses");

            std::vector<std::size_t> offsets;
            engine::indexRule(rule, 7, offsets);
            check(rule.srcId == 7, "coverage: indexRule stamps srcId");
            check(rule.arms.size() == 2 && rule.arms[0].second.srcId == 7 && rule.arms[0].first.srcId == 7,
                  "coverage: indexRule reaches nested arms and their tests");
            const json::LineIndex ruleLines(ruleText);
            check(ruleLines.lineAt(rule.arms[0].second.offset) == 4 &&
                  ruleLines.lineAt(rule.arms[1].second.offset) == 5,
                  "coverage: sibling case arms land on DISTINCT lines (branch-level signal)");

            // Evaluating picks arm 2; arm 1 is in the denominator but never hit — the guarantee
            // that a dead template arm reports as UNCOVERED rather than vanishing from the total.
            struct CollectingSink : engine::TraceSink {
                std::set<std::pair<int, std::size_t>> hits;
                void hit(int srcId, std::size_t offset) override { hits.insert({srcId, offset}); }
            } sink;
            MockEval armCtx;
            armCtx.fields["node.kind"] = "Binary";
            engine::setTraceSink(&sink);
            const std::string armOut = engine::evalRule(rule, armCtx);
            engine::setTraceSink(nullptr);

            check(armOut == "B", "coverage: tracing does not change evaluation");
            check(sink.hits.count({7, rule.arms[1].second.offset}) == 1, "coverage: taken arm is recorded hit");
            check(sink.hits.count({7, rule.arms[0].second.offset}) == 0, "coverage: untaken arm is NOT hit");
            check(std::find(offsets.begin(), offsets.end(), rule.arms[0].second.offset) != offsets.end(),
                  "coverage: untaken arm IS in the denominator (uncovered, not absent)");
            check(engine::traceSink() == nullptr, "coverage: sink is off by default");
        }

        // emit / emitChild route to the context's child-recursion (marker output in the mock).
        check(runRule(R"({"tmpl":[{"emitChild":"node.lhs","side":"l"}," + ",{"emit":"node.rhs"}]})", ctx) ==
              "<node.lhs:l> + <node.rhs>", "P18: interpreter emit / emitChild");
        // map: walk a child list by `<path>.count`, emit each indexed child, join with sep. Wrap to get `(a, b)`.
        ctx.fields["node.args.count"] = "3";
        check(runRule(R"J({"tmpl":["(",{"map":"node.args","sep":", "},")"]})J", ctx) ==
              "(<node.args.0>, <node.args.1>, <node.args.2>)", "P18: interpreter map over a child list");
        ctx.fields["node.args.count"] = "0";
        check(runRule(R"J({"tmpl":["(",{"map":"node.args","sep":", "},")"]})J", ctx) == "()",
              "P18: interpreter map over an empty child list");
        // map with an item template: `item.…` paths resolve against each element's indexed path.
        ctx.fields["node.fields.count"] = "2";
        ctx.fields["node.fields.0.name"] = "x";
        ctx.fields["node.fields.1.name"] = "y";
        check(runRule(R"J({"map":"node.fields","sep":", ","item":{"tmpl":[{"get":"item.name"},": ",{"emit":"item.value"}]}})J", ctx) ==
              "x: <node.fields.0.value>, y: <node.fields.1.value>",
              "P18: interpreter map with an item template (scoped item.* paths)");

        // Malformed rules fail loudly (never silently misparse).
        bool ok = true;
        runRule(R"({"bogus":1})", ctx, &ok);
        check(!ok, "P18: unknown rule form is rejected");
    }

    // ---- P18: Target -> BackendHandle (a target is a name, validated at resolve) ----------------------
    {
        check(findTarget("csharp").ok() && findTarget("typescript").ok() && findTarget("python").ok(),
              "P18: findTarget resolves the built-in targets");
        BackendHandle bad = findTarget("rust");
        check(!bad.ok() && has(bad.error(), "unknown target 'rust'") && has(bad.error(), "csharp"),
              "P18: findTarget on an unknown name fails with the known-target list");
        EmitResult r = compile("fn main() {}\n", bad);
        check(!r.ok && !r.diagnostics.empty() && has(r.diagnostics[0].message, "unknown target 'rust'"),
              "P18: compile with an invalid handle refuses with the resolution error");
    }

    // ---- P19 slice 8: loadBackend validation (anti-silent-drop + reference checks) ----------------------
    {
        std::string err;
        check(!loadBackend("not json", err) && has(err, "JSON object"),
              "P19: loadBackend rejects a non-object artifact");
        check(!loadBackend(R"({"name":"csharp"})", err) && has(err, "already loaded"),
              "P19: loadBackend rejects a duplicate name");
        // A minimal artifact missing nearly every rule: the coverage contract must name the first gap
        // (a construct with no rule and no declared stance).
        check(!loadBackend(R"({"schema":2,"name":"stubby",
                              "spec":{"name":"stubby"},"rules":{"Program":"x","Type":"x"}})", err) &&
                  has(err, "anti-silent-drop"),
              "P19: loadBackend refuses a plugin with undeclared coverage gaps");
        // A "native" capability claim with no rule behind it is also a lie the loader catches.
        check(!loadBackend(R"({"schema":2,"name":"stubby2","spec":{"name":"stubby2"},
                              "capabilities":{"patternMatching":"native"},
                              "rules":{"Program":"x","Type":"x"}})", err) &&
                  has(err, "no rule"),
              "P19: loadBackend refuses coverage gaps regardless of claims");
    }

    // ---- P37 slice 0: keyed capability vocabulary (closed + load-validated, umbrella fallback) ----------
    {
        // An unknown capability key is a LOAD error — never a silently-defaulted-to-native typo.
        std::string err;
        check(!loadBackend(R"({"schema":2,"name":"stubby3","spec":{"name":"stubby3"},
                              "capabilities":{"operatorOverloadng":"native"},
                              "rules":{"Program":"x","Type":"x"}})", err) &&
                  has(err, "unknown capability key 'operatorOverloadng'"),
              "P37 s0: loadBackend refuses a capability key outside the closed vocabulary");
        check(isKnownCapabilityKey("operatorOverloading") && isKnownCapabilityKey("interfaces") &&
                  !isKnownCapabilityKey("operatorOverloadng"),
              "P37 s0: the vocabulary covers the legacy names + coverage-only entries, nothing else");

        // Umbrella rule on a loaded plugin: a `parent:child` key with no explicit entry inherits the bare
        // parent's stance; an absent parent stays "native".
        const Backend* php = findBackend("php");
        const Backend* cs = findBackend("csharp");
        check(php && php->capabilityStance(std::string("operatorOverloading:arithmetic")) == "false",
              "P37 s0: php's blanket operatorOverloading=false umbrella-covers an undeclared sub-key");
        check(php && php->capabilityStance(std::string("operatorOverloading:eq")) == "native",
              "P37 s0/C6: an explicit sub-key entry overrides the umbrella (php :eq)");
        check(cs && cs->capabilityStance(std::string("operatorOverloading:eq")) == "native",
              "P37 s0: an absent parent leaves a sub-key native");
    }

    // ---- P37 slice 1: `constructor` keyword (M4 guard) --------------------------------------------------
    {
        // `constructor` is a live JS/TS prototype member: the internal member name and the plugin skeleton
        // keys are lookup keys only, and emitted TS/JS must never contain a bare `.constructor` member
        // access (which would resolve to Object.prototype.constructor at runtime — a silent miscompile).
        const char* prog = "class Box {\n  var v: i32\n  constructor(v: i32) { this.v = v }\n"
                           "  fn get(): i32 => this.v\n}\n"
                           "fn main() {\n  let b = Box(7)\n  print(b.get())\n}\n";
        EmitResult ts = compileStd(prog, findTarget("typescript"));
        check(ts.ok, "P37 s1: a `constructor` class compiles on typescript");
        check(has(ts.code, "constructor("), "P37 s1: TS emits a native class constructor");
        check(!has(ts.code, ".constructor"), "P37 s1 (M4): emitted TS never accesses `.constructor`");
        check(!compile("class B {\n  init() {}\n}\n", findTarget("typescript")).ok,
              "P37 s1: the old `init` keyword no longer parses as a constructor");
    }

    // ---- P37 slice 2: `is` / `as` --------------------------------------------------------------------
    {
        const char* hier = "open class Shape {\n}\nclass Circle : Shape {\n  var r: i32\n"
                           "  constructor(r: i32) {\n    super()\n    this.r = r\n  }\n}\n";
        // The binding form: hoisted narrowed Let + null test; TS uses instanceof, C# uses `as`.
        std::string prog = std::string(hier) +
            "fn f(x: Shape): i32 {\n  if x is Circle c && c.r > 1 {\n    return c.r\n  }\n  return 0\n}\n"
            "fn main() { print(f(Circle(3))) }\n";
        EmitResult ts = compileStd(prog, findTarget("typescript"));
        check(ts.ok, "P37 s2: `is` binding compiles on typescript");
        check(has(ts.code, "instanceof Circle") && !has(ts.code, " as Circle"),
              "P37 s2 (M1): TS emits a runtime instanceof guard, never a bare `as`");
        EmitResult cs = compileStd(prog, findTarget("csharp"));
        check(cs.ok && has(cs.code, "x as Circle"), "P37 s2: C# lowers the binding through `as`");
        EmitResult py = compileStd(prog, findTarget("python"));
        check(py.ok && has(py.code, "isinstance"), "P37 s2: Python emits isinstance");

        // `as` yields T? — null on failure — and never a TS type assertion.
        std::string asProg = std::string(hier) +
            "fn g(x: Shape): i32 {\n  let c = x as Circle\n  return c?.r ?? -1\n}\n"
            "fn main() { print(g(Circle(2))) }\n";
        EmitResult tsAs = compileStd(asProg, findTarget("typescript"));
        check(tsAs.ok && has(tsAs.code, "instanceof Circle ? x : null"),
              "P37 s2 (M1): TS `as` is a guarded conditional yielding null");

        // Refusals: binding under `||` (test doesn't dominate the branch); union targets; type params;
        // provably-unrelated types; redeclaring a binding name later in the same block.
        auto refuses = [&](const std::string& src, const char* frag, const char* label) {
            EmitResult r = compileStd(src, findTarget("csharp"));
            bool named = false;
            for (const auto& d : r.diagnostics) if (has(d.message, frag)) named = true;
            check(!r.ok && named, label);
        };
        refuses(std::string(hier) + "fn f(x: Shape, b: bool): i32 {\n  if x is Circle c || b {\n    return 1\n  }\n  return 0\n}\nfn main() { }\n",
                "only allowed on an `if` condition's `&&` chain",
                "P37 s2 (B3): a binding under `||` is refused");
        refuses("union U {\n  A(v: i32)\n  B(w: i32)\n}\nfn f(x: U): bool => x is A\nfn main() { }\n",
                "use 'match'", "P37 s2: `is` on a union case is refused toward match");
        refuses("open class Shape {\n}\nfn f<T>(x: Shape): bool => x is T\nfn main() { }\n",
                "type parameter", "P37 s2: `is` on a type parameter is refused (generic-preserving)");
        refuses(std::string(hier) + "class Other {\n  var v: i32\n  constructor() { this.v = 0 }\n}\n"
                "fn f(o: Other): bool => o is Circle\nfn main() { }\n",
                "provably impossible", "P37 s2: an unrelated-type test is refused");
        refuses(std::string(hier) + "fn f(x: Shape): i32 {\n  if x is Circle c {\n    return c.r\n  }\n  let c = 5\n  return c\n}\nfn main() { }\n",
                "bound by an `is` test earlier in this block",
                "P37 s2: redeclaring an `is`-binding name later in the block is refused");

        // B4: interface `is` gates per-target — refused when TS is configured, allowed on C#.
        std::string iface = "interface Named {\n  fn label(): string\n}\n"
                            "class Tag2 : Named {\n  constructor() {}\n  fn label(): string => \"t\"\n}\n"
                            "fn f(x: Named): bool => x is Tag2\nfn main() { print(f(Tag2())) }\n";
        check(compileStd(iface, findTarget("csharp")).ok,
              "P37 s2 (B4): a CLASS test on an interface-typed operand passes everywhere");
        std::string ifaceTest = "interface Named {\n  fn label(): string\n}\n"
                                "open class Base {\n}\nclass Tag2 : Base, Named {\n  constructor() { super() }\n  fn label(): string => \"t\"\n}\n"
                                "fn f(x: Base): bool => x is Named\nfn main() { print(f(Tag2())) }\n";
        check(compileStd(ifaceTest, findTarget("csharp")).ok,
              "P37 s2 (B4): interface `is` allowed on a csharp-only build");
        EmitResult tsIface = compileStd(ifaceTest, findTarget("typescript"));
        bool gated = false;
        for (const auto& d : tsIface.diagnostics)
            if (has(d.message, "interfaces:runtimeIdentity")) gated = true;
        check(!tsIface.ok && gated,
              "P37 s2 (B4): interface `is` refused when typescript is configured (no runtime identity)");
    }

    // ---- P37 slice 3: operators complete ---------------------------------------------------------------
    {
        const char* vec = "class Vec {\n  var x: i32\n  constructor(x: i32) { this.x = x }\n"
                          "  operator fn plus(o: Vec): Vec => Vec(this.x + o.x)\n"
                          "  operator fn eq(o: Vec): bool => this.x == o.x\n"
                          "  operator fn hash(): i32 => this.x\n"
                          "  operator fn neg(): Vec => Vec(0 - this.x)\n"
                          "  operator fn band(o: Vec): Vec => Vec(this.x & o.x)\n}\n";
        // C5: TS moves to static-on-type; C#/Python stay native.
        std::string prog = std::string(vec) +
            "fn main() {\n  let a = Vec(3)\n  let b = Vec(4)\n  print((a + b).x)\n  print(a == b)\n"
            "  print((-a).x)\n  print((a & b).x)\n}\n";
        EmitResult ts = compileStd(prog, findTarget("typescript"));
        check(ts.ok, "P37 s3: TS compiles the full operator surface");
        check(has(ts.code, "static plus(lhs: Vec") && has(ts.code, "Vec.plus(a, b)"),
              "P37 s3 (C5): TS operators are static-on-type at decl and call site");
        check(has(ts.code, "static eq(lhs: Vec | null") && has(ts.code, "Vec.eq(a, b)") &&
                  has(ts.code, "if (lhs === null ||"),
              "P37 s3 (C5): TS eq is static and null-tolerant");
        check(has(ts.code, "Vec.neg(a)"), "P37 s3 (#62): TS routes user unary neg");
        check(has(ts.code, "Vec.band(a, b)"), "P37 s3 (#63): TS routes user bitwise band");
        EmitResult cs = compileStd(prog, findTarget("csharp"));
        check(cs.ok && has(cs.code, "operator +(Vec lhs") && has(cs.code, "operator &(Vec lhs"),
              "P37 s3: C# operators stay native (incl. bitwise)");
        EmitResult py = compileStd(prog, findTarget("python"));
        check(py.ok && has(py.code, "__add__") && has(py.code, "__neg__") && has(py.code, "__and__"),
              "P37 s3: Python dunders cover arithmetic + unary + bitwise");

        // Explicit conversion: cast-site rewrite on method targets, native explicit operator on C#.
        std::string conv = "record Cel(deg: f64) {\n  operator fn explicit(): f64 => this.deg * 2.0\n}\n"
                           "fn main() {\n  let c = Cel(5.0)\n  print((f64)c)\n}\n";
        EmitResult tsc = compileStd(conv, findTarget("typescript"));
        check(tsc.ok && has(tsc.code, "Cel.toF64(c)") && has(tsc.code, "static toF64(lhs: Cel)"),
              "P37 s3: TS rewrites the cast site to the static Cel.toF64(c)");
        EmitResult csc2 = compileStd(conv, findTarget("csharp"));
        check(csc2.ok && has(csc2.code, "explicit operator double(Cel lhs)"),
              "P37 s3: C# emits the native explicit operator");

        // Refusals: implicit conversion; unary without a declared neg; compound with a computed receiver.
        auto refuses3 = [&](const std::string& src, const char* frag, const char* label) {
            EmitResult r = compileStd(src, findTarget("csharp"));
            bool named = false;
            for (const auto& d : r.diagnostics) if (has(d.message, frag)) named = true;
            check(!r.ok && named, label);
        };
        refuses3("record M(v: f64) {\n  operator fn implicit(): f64 => this.v\n}\nfn main() { }\n",
                 "refuses implicit user conversions", "P37 s3: implicit conversion refused");
        refuses3(std::string(vec) + "fn main() {\n  let a = Vec(1)\n  let t = ~a\n}\n",
                 "declares no 'operator fn bnot'",
                 "P37 s3 (#62): `~` on a user type without bnot is refused, not mis-emitted");
        refuses3("class Box {\n  var a: Vec\n  constructor() { this.a = Vec(0) }\n}\n" + std::string(vec) +
                 "fn mk(): Box => Box()\nfn main() {\n  mk().a += Vec(2)\n}\n",
                 "needs a simple receiver",
                 "P37 s3 (C7): a compound through a computed receiver is refused (single-eval)");
        check(compileStd(std::string(vec) + "fn main() {\n  var v = Vec(1)\n  v += Vec(2)\n  print(v.x)\n}\n",
                         findTarget("typescript")).ok,
              "P37 s3 (C7): a simple compound on a user type compiles");

        // C6: PHP gains :eq (+:indexers) while :arithmetic stays refused.
        std::string eqOnly = "class M {\n  var c: i32\n  constructor(c: i32) { this.c = c }\n"
                             "  operator fn eq(o: M): bool => this.c == o.c\n"
                             "  operator fn hash(): i32 => this.c\n}\n"
                             "fn main() { print(M(1) == M(1)) }\n";
        EmitResult phpEq = compileStd(eqOnly, findTarget("php"));
        check(phpEq.ok && has(phpEq.code, "->eq("), "P37 s3 (C6): PHP supports user eq (->eq call)");
        std::string arith = std::string(vec) + "fn main() { print((Vec(1) + Vec(2)).x) }\n";
        EmitResult phpAr = compileStd(arith, findTarget("php"));
        bool namedArith = false;
        for (const auto& d : phpAr.diagnostics)
            if (has(d.message, "operatorOverloading:arithmetic")) namedArith = true;
        check(!phpAr.ok && namedArith, "P37 s3 (C6): PHP still refuses :arithmetic, by sub-key");
    }

    // ---- P37 slice 4: attributes — Tier 2 portable metadata + Tier 1 pass-through ----------------------
    {
        // Tier 2: Meta queries resolve at transpile time and lower to inline constants.
        const char* meta =
            "attribute Range(min: i32, max: i32)\n"
            "[Range(1, 8)]\nclass Box {\n  [Range(0, 100)]\n  var fill: i32\n"
            "  constructor() { this.fill = 0 }\n}\n"
            "class Plain {\n  var v: i32\n  constructor() { this.v = 0 }\n}\n";
        std::string q = std::string(meta) +
            "fn main() {\n  print(Meta.has<Box, Range>())\n  print(Meta.has<Plain, Range>())\n"
            "  let r = Meta.get<Box, Range>()\n  print(r?.max ?? -1)\n"
            "  print(Meta.member<Box, Range>(\"fill\")?.max ?? -1)\n}\n";
        EmitResult ts = compileStd(q, findTarget("typescript"));
        check(ts.ok, "P37 s4: Meta queries compile on typescript");
        check(has(ts.code, "new Range(1, 8)") && has(ts.code, "new Range(0, 100)"),
              "P37 s4: Meta.get/member lower to inline constructions of the recorded constants");
        check(!has(ts.code, "Meta"), "P37 s4: no Meta runtime surface exists in the output");
        EmitResult php = compileStd(q, findTarget("php"));
        check(php.ok, "P37 s4: Tier 2 metadata needs NO capability — it runs on PHP too");

        // Zero output when unqueried: attach but never query -> the attribute record vanishes.
        std::string unq = std::string(meta) + "fn main() { print(Box().fill) }\n";
        EmitResult tsu = compileStd(unq, findTarget("typescript"));
        check(tsu.ok && !has(tsu.code, "Range"),
              "P37 s4: an attached-but-unqueried attribute emits ZERO runtime output");

        // Tier 1: the binding IS the native annotation line; imports accumulate once; refuse/D12 gate.
        const char* bind =
            "extern attribute JsonProp(name: string) {\n"
            "  actual(csharp) extern(\"[Newtonsoft.Json.JsonProperty($0)]\") import \"using Newtonsoft.Json;\"\n"
            "  actual(typescript) extern(\"@JsonProp($name)\")\n"
            "  actual(python) refuse\n"
            "}\n";
        std::string prog1 = std::string(bind) +
            "[JsonProp(\"x\")]\nclass C {\n  var x: i32\n  constructor() { this.x = 0 }\n}\n"
            "fn main() { print(C().x) }\n";
        EmitResult cs = compileStd(prog1, findTarget("csharp"));
        check(cs.ok && has(cs.code, "[Newtonsoft.Json.JsonProperty(\"x\")]") &&
                  has(cs.code, "using Newtonsoft.Json;"),
              "P37 s4 (Tier 1): C# gets the native attribute line + the using once");
        EmitResult ts1 = compileStd(prog1, findTarget("typescript"));
        check(ts1.ok && has(ts1.code, "@JsonProp(\"x\")"),
              "P37 s4 (Tier 1): TS gets the decorator line ($name substitution)");
        EmitResult py1 = compileStd(prog1, findTarget("python"));
        bool refusedArm = false;
        for (const auto& d : py1.diagnostics) if (has(d.message, "declares `refuse`")) refusedArm = true;
        check(!py1.ok && refusedArm, "P37 s4 (D12): an explicit `refuse` arm refuses loudly");
        EmitResult ph1 = compileStd(prog1, findTarget("php"));
        bool noArm = false;
        for (const auto& d : ph1.diagnostics) if (has(d.message, "no binding for target 'php'")) noArm = true;
        check(!ph1.ok && noArm, "P37 s4 (D12): a target with NO arm refuses loudly");

        // Attachment-point gating: TS has no function decorators.
        std::string fnAttr = std::string(bind) +
            "[JsonProp(\"f\")]\nfn helper(): i32 => 1\nfn main() { print(helper()) }\n";
        EmitResult tsf = compileStd(fnAttr, findTarget("typescript"));
        bool gatedFn = false;
        for (const auto& d : tsf.diagnostics)
            if (has(d.message, "attributes:target.function")) gatedFn = true;
        check(!tsf.ok && gatedFn, "P37 s4 (D10): a function attribute gates on TS (no function decorators)");
        check(compileStd(fnAttr, findTarget("csharp")).ok,
              "P37 s4 (D10): the same function attribute is fine on C#");

        // M6 + attachment refusals.
        auto refuses4 = [&](const std::string& src, const char* frag, const char* label) {
            EmitResult r = compileStd(src, findTarget("csharp"));
            bool named = false;
            for (const auto& d : r.diagnostics) if (has(d.message, frag)) named = true;
            check(!r.ok && named, label);
        };
        refuses4(std::string(meta) + "fn f<T>(): bool => Meta.has<T, Range>()\nfn main() { }\n",
                 "cannot be a type parameter",
                 "P37 s4 (M6): a type-parameter type arg refuses — permanently");
        refuses4(std::string(meta) + "fn f(n: string): bool => Meta.member<Box, Range>(n) != null\nfn main() { }\n",
                 "STRING LITERAL", "P37 s4 (M6): a computed member name refuses");
        refuses4(std::string(meta) + "fn main() { print(Meta.member<Box, Range>(\"nope\") == null) }\n",
                 "no member named 'nope'", "P37 s4: an unknown member name refuses");
        refuses4("attribute Tag()\n[Tag, Tag]\nclass C {\n  var v: i32\n  constructor() { this.v = 0 }\n}\nfn main() { }\n",
                 "applied more than once", "P37 s4: repeat application refuses (AllowMultiple deferred)");
        refuses4("attribute Tag(v: i32)\nfn cfg(): i32 => 3\n[Tag(cfg())]\nclass C {\n  var v: i32\n  constructor() { this.v = 0 }\n}\nfn main() { }\n",
                 "compile-time constants", "P37 s4: a non-const attribute arg refuses");
        refuses4("attribute Tag()\nfn main() {\n  let t = Tag()\n}\n",
                 "cannot be constructed directly", "P37 s4: direct construction of an attribute refuses");
        refuses4("class C {\n  var v: i32\n  constructor() { this.v = 0 }\n  fn m(): i32 => this.v\n}\n"
                 "fn main() {\n  let c = C()\n  print(c.m<i32>())\n}\n",
                 "explicit type arguments are not supported",
                 "P37 s4 (D.3): silently-ignored member-call type args now refuse (anti-silent-drop)");
    }

    // ---- P37 feature-complete: field/param attributes, enum/array values, Meta.param ------------------
    {
        const char* bind =
            "extern attribute JsonProp(name: string) {\n"
            "  actual(csharp) extern(\"[Newtonsoft.Json.JsonProperty($0)]\") import \"using Newtonsoft.Json;\"\n"
            "  actual(php) extern(\"#[JsonProp($0)]\")\n"
            "}\n";
        // Tier 1 on a FIELD: the annotation line lands above the field; Python gates the point.
        std::string fieldProg = std::string(bind) +
            "class C {\n  [JsonProp(\"x\")]\n  var x: i32\n  constructor() { this.x = 0 }\n}\n"
            "fn main() { print(C().x) }\n";
        EmitResult csf = compileStd(fieldProg, findTarget("csharp"));
        check(csf.ok && has(csf.code, "[Newtonsoft.Json.JsonProperty(\"x\")]"),
              "P37 fc: a Tier 1 FIELD attribute emits above the field on C#");
        EmitResult pyf = compileStd(fieldProg, findTarget("python"));
        bool gatedField = false;
        for (const auto& d : pyf.diagnostics)
            if (has(d.message, "attributes:target.field")) gatedField = true;
        check(!pyf.ok && gatedField, "P37 fc: a field attribute gates on Python (no field annotations)");

        // Tier 1 on a PARAMETER: inline before the param on C#/PHP; TS gates the point.
        std::string paramProg = std::string(bind) +
            "fn f([JsonProp(\"p\")] p: i32): i32 => p\nfn main() { print(f(3)) }\n";
        EmitResult csp = compileStd(paramProg, findTarget("csharp"));
        check(csp.ok && has(csp.code, "[Newtonsoft.Json.JsonProperty(\"p\")] int p"),
              "P37 fc: a Tier 1 PARAM attribute emits inline on C#");
        EmitResult php2 = compileStd(paramProg, findTarget("php"));
        check(php2.ok && has(php2.code, "#[JsonProp(\"p\")] $p"),
              "P37 fc: a Tier 1 PARAM attribute emits inline on PHP");
        EmitResult tsp = compileStd(paramProg, findTarget("typescript"));
        bool gatedParam = false;
        for (const auto& d : tsp.diagnostics)
            if (has(d.message, "attributes:target.param")) gatedParam = true;
        check(!tsp.ok && gatedParam, "P37 fc: a param attribute gates on TS (TC39 has none)");

        // Enum + array constant values render per plugin SPEC data (never target-name checks in Core).
        std::string constProg =
            "enum Sev {\n  Low\n  High\n}\n"
            "extern attribute Cfg(s: Sev, ids: i32[]) {\n"
            "  actual(csharp) extern(\"[Cfg($0, $1)]\")\n"
            "  actual(php) extern(\"#[Cfg($0, $1)]\")\n"
            "}\n"
            "[Cfg(Sev.High, [1, 2])]\nclass C {\n  var v: i32\n  constructor() { this.v = 0 }\n}\n"
            "fn main() { print(C().v) }\n";
        EmitResult csc3 = compileStd(constProg, findTarget("csharp"));
        check(csc3.ok && has(csc3.code, "[Cfg(Sev.High, new[] {1, 2})]"),
              "P37 fc: enum + array values spell per the C# plugin spec (constArray*)");
        EmitResult phc = compileStd(constProg, findTarget("php"));
        check(phc.ok && has(phc.code, "#[Cfg(Sev::High, [1, 2])]"),
              "P37 fc: enum member spells via the PHP plugin's enumMemberOp");

        // Tier 2 param metadata via Meta.param.
        std::string metaParam =
            "attribute Range(min: i32, max: i32)\n"
            "class Box {\n  var v: i32\n  constructor() { this.v = 0 }\n"
            "  fn scale([Range(1, 4)] k: i32): i32 => this.v * k\n}\n"
            "fn main() {\n  print(Meta.param<Box, Range>(\"scale\", \"k\")?.max ?? -1)\n}\n";
        EmitResult mp = compileStd(metaParam, findTarget("typescript"));
        check(mp.ok && has(mp.code, "new Range(1, 4)"),
              "P37 fc: Meta.param lowers to the recorded constants");
        auto refusesFc = [&](const std::string& src, const char* frag, const char* label) {
            EmitResult r = compileStd(src, findTarget("csharp"));
            bool named = false;
            for (const auto& d : r.diagnostics) if (has(d.message, frag)) named = true;
            check(!r.ok && named, label);
        };
        refusesFc("attribute Range(min: i32, max: i32)\n"
                  "class Box {\n  var v: i32\n  constructor() { this.v = 0 }\n"
                  "  fn scale([Range(1, 4)] k: i32): i32 => this.v * k\n}\n"
                  "fn main() {\n  print(Meta.param<Box, Range>(\"scale\", \"nope\") == null)\n}\n",
                  "no parameter named 'nope'", "P37 fc: an unknown param name refuses");
        refusesFc("attribute Tag(v: i32)\nattribute Wrap([Tag(1)] x: i32)\nfn main() { }\n",
                  "attribute` declaration's parameters are refused",
                  "P37 fc: attributes on an attribute's own params refuse");
    }

    // ---- P19 slices 13-15: reserved/forbidden identifiers (identifier hygiene, json-plugins.md §7) -----
    {
        // A name a target's generated code uses is refused for that target only (kind-blind v1).
        EmitResult ts = compile("fn main() {\n  let tag = 1\n  var y = tag\n}\n", findTarget("typescript"));
        check(!ts.ok && has(ts.diagnostics[0].message, "'tag' is reserved by target 'typescript'"),
              "P19: a TS-reserved local ('tag') refuses on typescript");
        check(compile("fn main() {\n  let tag = 1\n  var y = tag\n}\n", findTarget("csharp")).ok,
              "P19: the same name compiles fine on a target that doesn't reserve it");
        EmitResult py = compile("fn _pg_idiv(a: i32, b: i32): i32 {\n  return a\n}\nfn main() {}\n",
                                findTarget("python"));
        check(!py.ok && has(py.diagnostics[0].message, "'_pg_idiv' uses a prefix reserved for the Polyglot std library"),
              "P19/P40 s8: a `_pg_` runtime-helper fn name refuses (std-reserved prefix, every target)");
        EmitResult pyU = compile("fn main() {\n  let _PG_UPPER = 1\n}\n", findTarget("python"));
        check(!pyU.ok && has(pyU.diagnostics[0].message, "'_PG_UPPER' is reserved by target 'python'"),
              "P19/P40 s8: a Python prelude table name (`_PG_*`) refuses on python");
        EmitResult cs = compile("record PolyglotProgram(x: i32)\nfn main() {}\n", findTarget("csharp"));
        check(!cs.ok && has(cs.diagnostics[0].message, "'PolyglotProgram' is reserved by target 'csharp'"),
              "P19: a C#-scaffolding type name ('PolyglotProgram') refuses on csharp");
        // Prefix families: `__w*` covers every lowering temp, not just one spelled-out name.
        EmitResult w = compile("fn main() {\n  let __w1 = 1\n  var y = __w1\n}\n", findTarget("csharp"));
        check(!w.ok && has(w.diagnostics[0].message, "'__w1' is reserved"),
              "P19: a reserved prefix family ('__w*') matches '__w1'");
        // Runtime globals get their own shadowing message.
        EmitResult g = compile("fn main() {\n  let console = 1\n  var y = console\n}\n", findTarget("typescript"));
        check(!g.ok && has(g.diagnostics[0].message, "'console' shadows a runtime global of target 'typescript'"),
              "P19: shadowing a target runtime global refuses");
        // pgconfig forbiddenIdentifiers: "*" bans everywhere; a target-keyed ban bites only that target.
        LibConfig banAll;  banAll.forbiddenIdentifiers = {{"*", "temp"}};
        LibConfig banPy;   banPy.forbiddenIdentifiers = {{"python", "data"}};
        EmitResult f1 = compile("fn main() {\n  let temp = 1\n  var y = temp\n}\n", findTarget("csharp"), nullptr, banAll);
        check(!f1.ok && has(f1.diagnostics[0].message, "'temp' is forbidden by pgconfig"),
              "P19: a '*' pgconfig-forbidden identifier refuses on every target");
        EmitResult f2 = compile("fn main() {\n  let data = 1\n  var y = data\n}\n", findTarget("python"), nullptr, banPy);
        check(!f2.ok && has(f2.diagnostics[0].message, "'data' is forbidden by pgconfig"),
              "P19: a target-keyed pgconfig ban bites on that target");
        check(compile("fn main() {\n  let data = 1\n  var y = data\n}\n", findTarget("csharp"), nullptr, banPy).ok,
              "P19: a target-keyed pgconfig ban is ignored by other targets");

        // Slice 15: target-keyword names escape CONSISTENTLY at decl + reference + type sites (ident is
        // wired through every rule's name holes), and refuse on a target that declares no escape (TS).
        EmitResult kcs = compile("record switch(delegate: i32)\nfn checked(a: i32): i32 {\n  return a\n}\n"
                                 "fn main() {\n  let s = switch(5)\n  var y = checked(s.delegate)\n}\n",
                                 findTarget("csharp"));
        check(kcs.ok && has(kcs.code, "record @switch(int @delegate)") && has(kcs.code, "int @checked(int a)") &&
                  has(kcs.code, "PolyglotProgram.@checked(s.@delegate)") && has(kcs.code, "new @switch(5)"),
              "P19: C#-keyword names escape as @name at decl, call, member, and construction sites");
        EmitResult kpy = compile("fn global(a: i32): i32 {\n  return a\n}\nfn main() {\n  var y = global(7)\n}\n",
                                 findTarget("python"));
        check(kpy.ok && has(kpy.code, "def global_(a)") && has(kpy.code, "global_(7)"),
              "P19: python-keyword fn names escape with the suffix strategy, decl and call agreeing");
        EmitResult kts = compile("fn function(a: i32): i32 {\n  return a\n}\nfn main() {}\n",
                                 findTarget("typescript"));
        check(!kts.ok && has(kts.diagnostics[0].message, "'function' is reserved by target 'typescript'"),
              "P19: TS declares no escape, so its keywords are reserved names -> honest refusal");
    }

    // ---- Precedence parenthesization (the 2026-07-04 FruitCake NaN miscompile) -------------------------
    // `??`/bitwise/shifts were missing from the engine's operatorPrecedence table and fell through to the
    // TIGHTEST level, dropping required parens: `basis + (opt?.v ?? 0.0)` emitted bare reparses as
    // `(basis + opt?.v) ?? 0.0` — wrong on both targets and divergent (C# 0.0 vs JS NaN).
    {
        const char* src =
            "class Box {\n  var v: f64\n  constructor(v: f64) { this.v = v }\n}\n"
            "fn addOpt(basis: f64, opt: Box?): f64 => basis + (opt?.v ?? 0.0)\n"
            "fn main() {\n  print(addOpt(5.0, null))\n}\n";
        EmitResult cs = compileStd(src, findTarget("csharp"));
        check(cs.ok && has(cs.code, "+ (opt?.v ?? "),
              "precedence: C# keeps the parens around a ?? operand of +");
        EmitResult ts = compileStd(src, findTarget("typescript"));
        check(ts.ok && has(ts.code, "+ (opt?.v ?? "),
              "precedence: TS keeps the parens around a ?? operand of +");

        const char* shifts =
            "fn f(a: i32, b: i32, c: i32): i32 => (a << b) + c\n"
            "fn main() {\n  print(f(5, 2, 3))\n}\n";
        EmitResult scs = compileStd(shifts, findTarget("csharp"));
        check(scs.ok && has(scs.code, "(a << b) + c"),
              "precedence: C# keeps the parens around a shift operand of +");

        const char* cmp =
            "fn g(a: i32, b: i32, c: i32): bool => (b > a) == (c > a)\n"
            "fn main() {\n  print(g(1, 2, 3))\n}\n";
        EmitResult pcs = compileStd(cmp, findTarget("python"));
        check(pcs.ok && has(pcs.code, "(b > a) == (c > a)"),
              "precedence: python keeps comparison-under-comparison parens (no chaining)");
    }

    // ---- P21 slice 1: watch-mode support (FileWatcher polling + RecordingResolver) ---------------------
    {
        using cli::FileWatcher;
        using cli::PollingFileWatcher;
        using cli::RecordingResolver;
        namespace fs = std::filesystem;
        using namespace std::chrono_literals;

        const fs::path dir = fs::temp_directory_path() / "polyglot-watch-tests";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
        const fs::path a = dir / "a.pg";
        const fs::path b = dir / "b.pg"; // watched before it exists: its appearance must count as a change
        { std::ofstream(a, std::ios::binary) << "fn main() {}\n"; }

        PollingFileWatcher w(10ms); // fast tick so the suite stays quick
        w.watch({a, b});
        check(w.waitNext(50ms) == FileWatcher::Event::TimedOut, "P21: unchanged set times out");

        { std::ofstream(a, std::ios::binary) << "fn main() {\n  print(1)\n}\n"; }
        check(w.waitNext(2000ms) == FileWatcher::Event::Changed, "P21: a modified file reports Changed");
        check(w.waitNext(50ms) == FileWatcher::Event::TimedOut,
              "P21: detected changes fold into the baseline (no double report)");

        { std::ofstream(b, std::ios::binary) << "fn helper() {}\n"; }
        check(w.waitNext(2000ms) == FileWatcher::Event::Changed,
              "P21: a missing watched file appearing reports Changed");

        fs::remove(a);
        check(w.waitNext(2000ms) == FileWatcher::Event::Changed, "P21: a deleted file reports Changed");

        w.stop();
        check(w.waitNext(2000ms) == FileWatcher::Event::Stopped, "P21: stop() makes waitNext return Stopped");

        fs::remove_all(dir, ec);

        // RecordingResolver captures the loaded closure + the unresolved specifiers.
        MapModuleResolver inner({{"geometry", "fn area(w: i32, h: i32): i32 {\n  return w * h\n}\n"}});
        RecordingResolver rec(inner);
        check(rec.resolve("geometry", "").has_value() && rec.loaded().count("geometry") == 1,
              "P21: RecordingResolver records a resolved module's canonical path");
        check(!rec.resolve("missing", "entry.pg").has_value() &&
                  rec.unresolved().count({"missing", "entry.pg"}) == 1,
              "P21: RecordingResolver records unresolved (spec, importer) pairs");
        EmitResult r = compile("import { area } from \"geometry\"\nfn main() {\n  print(area(3, 4))\n}\n",
                               findTarget("csharp"), &rec, LibConfig{{"io"}});
        check(r.ok && rec.loaded().count("geometry") == 1,
              "P21: compile through a RecordingResolver captures the import closure");
    }

    // P25 §4.18 — capture analysis: the pass classifies each lambda's captures and stamps needsCell.
    // Asserted on the typed IR dump: a lambda shows `[caps <name>(cell)? … this?]`; a celled local shows
    // `[cell]`, a captured-but-snapshot local `[captured]`.
    {
        auto capIr = [&](const char* src) -> std::string {
            DiagnosticBag d;
            auto unit = parse(lex(src, d), d);
            mintplayer::polyglot::check(unit, d);
            if (d.hasErrors()) return "<type error>";
            return ir::dump(lower(unit, "csharp"));
        };
        // Accumulator: `total` is mutated through the closure -> SHARED-RW -> needsCell (the golden sample).
        std::string acc = capIr("fn f(): i32 {\n  var total = 0\n  let add = (n: i32) => { total += n }\n"
                                "  add(10)\n  add(20)\n  return total\n}\n");
        check(has(acc, "[caps total(cell)]"), "P25: mutated capture is classified needsCell");
        check(has(acc, "var total: i32 = 0:i32 [cell]"), "P25: the mutated local's declaration is celled");

        // Pure read of an immutable local -> SNAPSHOT -> captured but NO cell.
        std::string snap = capIr("fn f(): i32 {\n  let base = 10\n  let g = (x: i32) => x + base\n  return g(5)\n}\n");
        check(has(snap, "[caps base]") && !has(snap, "base(cell)"), "P25: unmutated capture is snapshot (no cell)");
        check(has(snap, "let base: i32 = 10:i32 [captured]"), "P25: snapshot local is captured but not celled");

        // Loop-variable capture: `i` is read-only -> snapshot per iteration, no cell.
        std::string loop = capIr("fn f() {\n  for i in 1..=3 {\n    let g = () => i\n  }\n}\n");
        check(has(loop, "[caps i]") && !has(loop, "i(cell)"), "P25: read-only loop-var capture is snapshot");

        // A global is not a local binding -> never a capture.
        std::string glob = capIr("let base = 5\nfn f(): i32 {\n  let rd = () => base\n  return rd()\n}\n");
        check(!has(glob, "[caps"), "P25: a module global is not captured");

        // Nested lambdas: a variable two levels up is captured by BOTH inner lambdas (propagation).
        std::string nest = capIr("fn f(): i32 {\n  let x = 5\n  let g = () => {\n    let h = () => x\n    return h()\n  }\n  return g()\n}\n");
        check(has(nest, "[caps x]"), "P25: nested lambda propagates the capture to each level");

        // `this` inside a lambda sets capturesThis (drives TS-arrow / PHP $this / C++ [this]). Asserted in an
        // `init` body, which `ir::dump` renders (method bodies aren't dumped, but the flag is set the same way).
        std::string self = capIr("class C {\n  let v: i32\n  constructor() {\n    this.v = 0\n    let f = () => this.v\n  }\n}\n");
        check(has(self, "[caps this]"), "P25: a lambda referencing `this` sets capturesThis");

        // Slice 4 — PHP real closures. PHP has no interpreter in this environment, so the differential gate
        // can't run it; assert the emission directly: a mutated capture -> `use (&$x)` (driven off needsCell,
        // never syntax); a closure-valued local is called via `$f(...)`; a pure expression lambda -> `fn(…)`.
        EmitResult phpAcc = compileStd("fn main() {\n  var total = 0\n  let add = (n: i32) => { total += n }\n"
                                       "  add(10)\n  print(total)\n}\n", findTarget("php"));
        check(phpAcc.ok && has(phpAcc.code, "use (&$total)"), "P25/PHP: a mutated capture emits use(&$total)");
        check(phpAcc.ok && has(phpAcc.code, "$add("), "P25/PHP: a closure-valued local is called via $f(...)");
        EmitResult phpSnap = compileStd("fn apply(f: (i32) => i32, x: i32): i32 => f(x)\n"
                                        "fn main() {\n  let base = 10\n  print(apply((n: i32) => n + base, 5))\n}\n",
                                        findTarget("php"));
        check(phpSnap.ok && has(phpSnap.code, "fn("), "P25/PHP: a pure expression lambda emits the fn(...) form");
    }

    // P26 slice 0 — the tri-state capability vocabulary. The three new flags detect from the syntactic
    // surface and gate-on-`false` / warn-on-`emulated`; they are inert for the current native backends.
    {
        auto parseOf = [](const char* src) { DiagnosticBag d; return parse(lex(src, d), d); };
        auto usesFeature = [](const CompilationUnit& u, Feature f) {
            for (const auto& use : collectFeatureUses(u)) if (use.key == featureName(f)) return true;
            return false;
        };

        // Detection: each new axis is marked from a type annotation / a mutable class field.
        auto width = parseOf("fn f(x: u8): i32 { return x }\n");
        check(usesFeature(width, Feature::FixedWidthIntegers), "P26 s0: a sub-64/unsigned width marks fixedWidthIntegers");
        auto ch = parseOf("fn f(c: char): char { return c }\n");
        check(usesFeature(ch, Feature::Utf16Strings), "P26 s0: a `char` type marks utf16Strings");
        auto mref = parseOf("class Box { var v: i32\n  constructor() { this.v = 0 } }\n");
        check(usesFeature(mref, Feature::MutableRefClasses), "P26 s0: a mutable-field class marks mutableRefClasses");
        auto immut = parseOf("record P(x: i32)\nfn f(): i32 { let p = P(1)\n  return p.x }\n");
        check(!usesFeature(immut, Feature::MutableRefClasses), "P26 s0: a record (no var field) does not mark mutableRefClasses");

        // Refuse-on-false: a target lacking the flag refuses a program that uses it, naming the feature + target.
        StubBackend noWidth(Feature::FixedWidthIntegers);
        DiagnosticBag dw; checkCapabilities(width, noWidth, dw);
        bool namedW = false;
        for (const auto& it : dw.items()) if (has(it.message, "fixedWidthIntegers") && has(it.message, "stub")) namedW = true;
        check(dw.hasErrors() && namedW, "P26 s0: a target lacking fixedWidthIntegers refuses + names it");

        // Warn-on-emulated: a used emulated feature warns (no error); an unused gated feature stays clean.
        StubBackend emuWidth(Feature::Async /* unused by the program */, Feature::FixedWidthIntegers);
        DiagnosticBag de; checkCapabilities(width, emuWidth, de);
        bool warned = false;
        for (const auto& it : de.items())
            if (it.severity == Severity::Warning && has(it.message, "emulates") && has(it.message, "fixedWidthIntegers")) warned = true;
        check(!de.hasErrors() && warned, "P26 s0: an emulated feature warns, does not error");

        // Inert for the current backends: C# declares these native/absent, so a width program is not gated.
        DiagnosticBag dcs; checkCapabilities(width, *findBackend("csharp"), dcs);
        check(!dcs.hasErrors(), "P26 s0: csharp (native) does not gate a fixed-width program");
    }

    // ---- P30 slice 0: auto-download primitives (sha / SRI / gzip / tar / pgconfig glue) ------

    // SHA-512 / SHA-1 against the NIST FIPS 180-4 vectors — these pin the generated K/H tables.
    {
        check(cli::toHex(cli::sha512(std::string("abc"))) ==
                  "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                  "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
              "P30 s0: sha512('abc') matches NIST");
        check(cli::toHex(cli::sha512(std::string())) ==
                  "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
                  "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e",
              "P30 s0: sha512('') matches NIST");
        check(cli::toHex(cli::sha512(std::string(
                  "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
                  "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"))) ==
                  "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
                  "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909",
              "P30 s0: sha512(two-block vector) matches NIST");
        check(cli::toHex(cli::sha1(std::string("abc"))) == "a9993e364706816aba3e25717850c26c9cd0d89d",
              "P30 s0: sha1('abc') matches NIST");
        check(cli::toHex(cli::sha1(std::string(1000000, 'a'))) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f",
              "P30 s0: sha1(million 'a') matches NIST (multi-block path)");

        // SRI: npm's dist.integrity form. The reference string was produced by Node crypto.
        const std::string sri = cli::sriSha512("polyglot");
        check(sri == "sha512-CQa4A4lIW4ghrXVECgs3HAWEbMqpb5YHPlBw+nU+4pYViS88L5Hyh8OYBdfevZNSqElzL9ZJRpONx94rrPQDPQ==",
              "P30 s0: sriSha512 renders npm's base64 form");
        std::string err;
        check(cli::verifySri(sri, "polyglot", err), "P30 s0: verifySri accepts the right payload");
        check(!cli::verifySri(sri, "Polyglot", err) && has(err, "integrity mismatch"),
              "P30 s0: verifySri refuses a tampered payload naming the mismatch");
        check(!cli::verifySri("sha384-AAAA", "x", err) && has(err, "unsupported integrity algorithm 'sha384'"),
              "P30 s0: verifySri refuses an algorithm it cannot check (never a silent pass)");
        check(!cli::verifySri("garbage", "x", err) && has(err, "malformed integrity"),
              "P30 s0: verifySri refuses a malformed SRI string");
    }

    // CRC-32 (gzip trailer): the classic check value.
    {
        check(cli::crc32("123456789", 9) == 0xCBF43926u, "P30 s0: crc32 check value");
    }

    // gunzip against Node-zlib golden vectors: fixed-Huffman, FNAME header, dynamic-Huffman,
    // incompressible/stored — plus the corruption and zip-bomb-cap refusals.
    {
        std::string out, err;
        check(cli::gunzip(gzfix::kP1Gz, gzfix::kP1GzLen, out, 1 << 20, err) && out == gzfix::kP1,
              "P30 s0: gunzip decodes a fixed-Huffman member");
        out.clear();
        check(cli::gunzip(gzfix::kP1GzName, gzfix::kP1GzNameLen, out, 1 << 20, err) && out == gzfix::kP1,
              "P30 s0: gunzip skips an FNAME header field");
        out.clear();
        check(cli::gunzip(gzfix::kP2Gz, gzfix::kP2GzLen, out, 1 << 20, err) && out == gzfix::kP2,
              "P30 s0: gunzip decodes a dynamic-Huffman member");
        out.clear();
        check(cli::gunzip(gzfix::kP3Gz, gzfix::kP3GzLen, out, 1 << 20, err) &&
                  out.size() == gzfix::kP3Len && std::memcmp(out.data(), gzfix::kP3, gzfix::kP3Len) == 0,
              "P30 s0: gunzip decodes an incompressible (stored) member");
        check(cli::toHex(cli::sha512(out)) == gzfix::kP3Sha512Hex,
              "P30 s0: sha512 of the inflated payload matches Node crypto (binary input)");
        check(cli::verifySri(gzfix::kP3Sri, out, err),
              "P30 s0: verifySri agrees with an npm-shaped integrity for binary bytes");

        // Corruption: flip one payload byte — the CRC32 trailer must catch it.
        std::string bad(reinterpret_cast<const char*>(gzfix::kP2Gz), gzfix::kP2GzLen);
        bad[bad.size() / 2] ^= 0x40;
        out.clear();
        check(!cli::gunzip(reinterpret_cast<const std::uint8_t*>(bad.data()), bad.size(), out, 1 << 20, err),
              "P30 s0: gunzip refuses a corrupted stream");
        // Zip-bomb guard: a cap below the payload size fails BEFORE unbounded growth.
        out.clear();
        check(!cli::gunzip(gzfix::kP2Gz, gzfix::kP2GzLen, out, 16, err) && has(err, "size cap"),
              "P30 s0: gunzip enforces the output size cap");
        out.clear();
        check(!cli::gunzip(reinterpret_cast<const std::uint8_t*>("nope"), 4, out, 16, err),
              "P30 s0: gunzip refuses a non-gzip buffer");
    }

    // tar: ustar + pax reading with the zip-slip refusals. Archives are built in-test.
    {
        auto oct = [](std::string& h, std::size_t at, std::size_t width, unsigned long long v) {
            for (std::size_t i = width - 1; i-- > 0;) { h[at + i] = static_cast<char>('0' + (v & 7)); v >>= 3; }
            h[at + width - 1] = '\0';
        };
        auto hdr = [&](const std::string& name, std::size_t size, char type, const std::string& prefix = "") {
            std::string h(512, '\0');
            std::memcpy(&h[0], name.data(), name.size());
            oct(h, 100, 8, 0644); oct(h, 108, 8, 0); oct(h, 116, 8, 0);
            oct(h, 124, 12, size); oct(h, 136, 12, 0);
            std::memset(&h[148], ' ', 8);
            h[156] = type;
            std::memcpy(&h[257], "ustar", 6); h[263] = '0'; h[264] = '0';
            if (!prefix.empty()) std::memcpy(&h[345], prefix.data(), prefix.size());
            unsigned sum = 0;
            for (unsigned char c : h) sum += c;
            for (int i = 5; i >= 0; --i) { h[148 + i] = static_cast<char>('0' + (sum & 7)); sum >>= 3; }
            h[154] = '\0'; h[155] = ' ';
            return h;
        };
        auto pad512 = [](std::string s) { s.resize((s.size() + 511) & ~std::size_t(511), '\0'); return s; };
        const std::string endMark(1024, '\0');

        // A plain npm-shaped archive: dir + manifest under package/.
        const std::string manifest = "{ \"schema\": 2, \"name\": \"demo\" }";
        std::string ar = hdr("package/", 0, '5') +
                         hdr("package/polyglot-plugin.json", manifest.size(), '0') + pad512(manifest) + endMark;
        std::vector<cli::TarEntry> es;
        std::string err;
        check(cli::tarRead(ar, es, err) && es.size() == 2 && es[0].isDir && es[0].path == "package" &&
                  es[1].path == "package/polyglot-plugin.json" && es[1].data == manifest,
              "P30 s0: tar reads an npm-shaped ustar archive");

        // pax extended header overrides the (truncated) name field of the entry that follows.
        const std::string longPath = "package/deeply/nested/dirs/going/on/for/quite/a/while/to/exceed/"
                                     "the/ustar/name/field/limit/of/one/hundred/characters/manifest.json";
        std::string rec = " path=" + longPath + "\n";
        std::size_t recLen = rec.size() + 2; // 2-digit guess, then fix up
        recLen = std::to_string(recLen).size() + rec.size();
        rec = std::to_string(recLen) + rec;
        std::string ar2 = hdr("PaxHeader/manifest.json", rec.size(), 'x') + pad512(rec) +
                          hdr("package/short-name", 3, '0') + pad512("hey") + endMark;
        es.clear();
        check(cli::tarRead(ar2, es, err) && es.size() == 1 && es[0].path == longPath && es[0].data == "hey",
              "P30 s0: tar honors a pax path override");

        // The ustar prefix field joins ahead of name.
        std::string ar3 = hdr("file.json", 2, '0', "package/pfx") + pad512("{}") + endMark;
        es.clear();
        check(cli::tarRead(ar3, es, err) && es.size() == 1 && es[0].path == "package/pfx/file.json",
              "P30 s0: tar joins the ustar prefix field");

        // Zip-slip refusals: traversal, absolute, backslash, and link entries all fail the READ.
        for (const auto& evil : {std::string("package/../evil"), std::string("/abs"), std::string("a\\b")}) {
            std::string bad = hdr(evil, 1, '0') + pad512("x") + endMark;
            es.clear();
            check(!cli::tarRead(bad, es, err) && has(err, "unsafe path") && es.empty(),
                  "P30 s0: tar refuses unsafe path '" + evil + "'");
        }
        std::string link = hdr("package/link", 0, '2') + endMark;
        es.clear();
        check(!cli::tarRead(link, es, err) && has(err, "unsupported entry type"),
              "P30 s0: tar refuses a symlink entry");

        // A corrupted header checksum fails loudly.
        std::string badSum = ar;
        badSum[0] ^= 1;
        es.clear();
        check(!cli::tarRead(badSum, es, err) && has(err, "checksum"),
              "P30 s0: tar refuses a header checksum mismatch");

        // Truncation (entry claims more data than the buffer holds) fails loudly.
        std::string trunc = hdr("package/big", 100000, '0') + pad512("tiny");
        es.clear();
        check(!cli::tarRead(trunc, es, err) && has(err, "overruns"),
              "P30 s0: tar refuses an entry that overruns the archive");
    }

    // pgconfig glue: the walk-up loader and the POLYGLOT_CACHE override (P30's test seam).
    {
        namespace fs = std::filesystem;
        const fs::path base = fs::temp_directory_path() / "polyglot-p30-s0-test";
        std::error_code ec;
        fs::remove_all(base, ec);
        fs::create_directories(base / "sub" / "inner");
        cli::writeFile(base / "pgconfig.json",
                       "{ \"root\": \"src\", \"targets\": [\"python\"],"
                       " \"dependencies\": { \"python\": \"^0.3.0\" } }");
        const cli::PgConfig pc = cli::loadPgConfig(base / "sub" / "inner");
        check(pc.found && pc.dir == base, "P30 s0: loadPgConfig walks up to the config");
        check(pc.targets.size() == 1 && pc.targets[0] == "python", "P30 s0: pgconfig targets parsed");
        check(pc.dependencies.size() == 1 && pc.dependencies[0].first == "python" &&
                  pc.dependencies[0].second == "^0.3.0",
              "P30 s0: pgconfig versioned dependency spec parsed verbatim");

        const fs::path cacheOverride = base / "cache";
#ifdef _WIN32
        _putenv_s("POLYGLOT_CACHE", cacheOverride.string().c_str());
#else
        setenv("POLYGLOT_CACHE", cacheOverride.string().c_str(), 1);
#endif
        check(cli::pluginCacheDir() == cacheOverride, "P30 s0: POLYGLOT_CACHE overrides the plugin cache dir");
#ifdef _WIN32
        _putenv_s("POLYGLOT_CACHE", "");
#else
        unsetenv("POLYGLOT_CACHE");
#endif
        check(cli::pluginCacheDir() != cacheOverride, "P30 s0: clearing POLYGLOT_CACHE restores the default");
        fs::remove_all(base, ec);
    }

    // ---- P30 slice 1: semver subset + registry client ----------------------------------------

    // Version parsing + precedence (semver spec §11, incl. the classic prerelease ordering chain).
    {
        auto v = [](const char* s) { return *cli::parseSemVer(s); };
        check(cli::parseSemVer("1.2.3") && v("1.2.3").major == 1 && v("1.2.3").patch == 3,
              "P30 s1: parseSemVer parses a release version");
        check(cli::parseSemVer("v0.3.1") && !v("v0.3.1").isPrerelease(), "P30 s1: leading 'v' tolerated");
        check(v("1.2.3-rc.1").prerelease.size() == 2, "P30 s1: prerelease identifiers split");
        check(cli::parseSemVer("1.2.3+build.5") && !v("1.2.3+build.5").isPrerelease(),
              "P30 s1: build metadata parsed and ignored");
        check(!cli::parseSemVer("1.2") && !cli::parseSemVer("1.x") && !cli::parseSemVer(""),
              "P30 s1: partial/x-range versions are refused");

        const char* chain[] = {"1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
                               "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0"};
        bool ordered = true;
        for (int i = 0; i + 1 < 8; ++i)
            ordered &= cli::compareSemVer(v(chain[i]), v(chain[i + 1])) < 0;
        check(ordered, "P30 s1: the spec's prerelease precedence chain orders correctly");
    }

    // Range semantics: caret/tilde desugar, prerelease exclusion, maxSatisfying.
    {
        std::string err;
        auto sat = [&](const char* range, const char* ver) {
            return cli::satisfies(*cli::parseSemVer(ver), *cli::parseRange(range, err));
        };
        check(sat("^1.2.3", "1.2.3") && sat("^1.2.3", "1.9.9") && !sat("^1.2.3", "2.0.0") && !sat("^1.2.3", "1.2.2"),
              "P30 s1: ^1.2.3 = >=1.2.3 <2.0.0");
        check(sat("^0.2.3", "0.2.9") && !sat("^0.2.3", "0.3.0"), "P30 s1: ^0.2.3 = >=0.2.3 <0.3.0");
        check(sat("^0.0.3", "0.0.3") && !sat("^0.0.3", "0.0.4"), "P30 s1: ^0.0.3 pins the patch");
        check(sat("~1.2.3", "1.2.9") && !sat("~1.2.3", "1.3.0"), "P30 s1: ~1.2.3 allows patch drift only");
        check(sat(">=1.2.3", "2.5.0") && !sat(">=1.2.3", "1.2.2"), "P30 s1: >= has an open upper bound");
        check(sat("1.2.3", "1.2.3") && !sat("1.2.3", "1.2.4"), "P30 s1: a bare version is exact");
        check(!sat("^1.2.3", "1.3.0-rc.1"), "P30 s1: prereleases are invisible to a release range");
        check(sat("^1.2.3-rc.1", "1.2.3-rc.2") && !sat("^1.2.3-rc.1", "1.2.4-rc.1") && sat("^1.2.3-rc.1", "1.2.4"),
              "P30 s1: a prerelease anchor admits prereleases of its own core tuple only");
        check(!cli::parseRange("1.x", err) && has(err, "unsupported version spec"),
              "P30 s1: outside-the-grammar ranges are refused with the supported list");

        const std::vector<std::string> vers = {"0.2.9", "0.3.0", "0.3.1", "0.4.0-rc.1", "0.4.0"};
        check(cli::maxSatisfying(vers, *cli::parseRange("^0.3.0", err), "") == std::optional<std::string>("0.3.1"),
              "P30 s1: maxSatisfying picks the highest in-range version");
        check(cli::maxSatisfying(vers, *cli::parseRange(">=0.2.0", err), "") == std::optional<std::string>("0.4.0"),
              "P30 s1: maxSatisfying skips prereleases even at the top");
        check(cli::maxSatisfying(vers, *cli::parseRange("latest", err), "0.3.1") == std::optional<std::string>("0.3.1"),
              "P30 s1: 'latest' resolves through the dist-tag");
        check(!cli::maxSatisfying(vers, *cli::parseRange("^1.0.0", err), ""),
              "P30 s1: an unsatisfiable range resolves to nothing");
    }

    // Registry client: URL encoding, packument parsing (abbreviated shape), error responses.
    {
        check(cli::packumentUrl("https://registry.npmjs.org", "@mintplayer/polyglot-target-python") ==
                  "https://registry.npmjs.org/@mintplayer%2Fpolyglot-target-python",
              "P30 s1: the packument URL encodes the scope slash");

        const std::string fixture = R"({
            "name": "@mintplayer/polyglot-target-python",
            "dist-tags": { "latest": "0.3.1" },
            "versions": {
                "0.3.0": { "version": "0.3.0", "dist": {
                    "tarball": "https://registry.npmjs.org/@mintplayer/polyglot-target-python/-/polyglot-target-python-0.3.0.tgz",
                    "integrity": "sha512-AAAA" } },
                "0.3.1": { "version": "0.3.1", "dist": {
                    "tarball": "https://registry.npmjs.org/@mintplayer/polyglot-target-python/-/polyglot-target-python-0.3.1.tgz",
                    "integrity": "sha512-BBBB", "shasum": "abc123" } }
            }
        })";
        cli::Packument pk;
        std::string err;
        check(cli::parsePackument(fixture, pk, err) && pk.versions.size() == 2 &&
                  pk.distTags["latest"] == "0.3.1" && pk.versions[1].integrity == "sha512-BBBB" &&
                  has(pk.versions[0].tarball, "-0.3.0.tgz"),
              "P30 s1: parsePackument reads the abbreviated shape");
        cli::Packument none;
        check(!cli::parsePackument(R"({"error":"Not found"})", none, err) && has(err, "Not found"),
              "P30 s1: a registry error body becomes a clean diagnostic");
    }

    // Registry selection: POLYGLOT_REGISTRY env, then .npmrc (scoped beats unscoped), then npmjs.
    {
        namespace fs = std::filesystem;
        const fs::path base = fs::temp_directory_path() / "polyglot-p30-s1-test";
        std::error_code ec;
        fs::remove_all(base, ec);
        fs::create_directories(base / "proj");
        check(cli::registryBaseFor("@x/y", base / "proj") == "https://registry.npmjs.org",
              "P30 s1: the public registry is the default");
        cli::writeFile(base / ".npmrc",
                       "# comment\nregistry=https://mirror.example/npm/\n"
                       "@mintplayer:registry=https://plugins.example/registry\n");
        check(cli::registryBaseFor("@mintplayer/polyglot-target-python", base / "proj") ==
                  "https://plugins.example/registry",
              "P30 s1: a scoped .npmrc registry wins for its scope");
        check(cli::registryBaseFor("left-pad", base / "proj") == "https://mirror.example/npm",
              "P30 s1: the unscoped .npmrc registry applies otherwise (trailing '/' trimmed)");
#ifdef _WIN32
        _putenv_s("POLYGLOT_REGISTRY", "http://127.0.0.1:9999/");
#else
        setenv("POLYGLOT_REGISTRY", "http://127.0.0.1:9999/", 1);
#endif
        check(cli::registryBaseFor("@mintplayer/polyglot-target-python", base / "proj") == "http://127.0.0.1:9999",
              "P30 s1: POLYGLOT_REGISTRY overrides everything (the test-harness seam)");
#ifdef _WIN32
        _putenv_s("POLYGLOT_REGISTRY", "");
#else
        unsetenv("POLYGLOT_REGISTRY");
#endif
        fs::remove_all(base, ec);
    }

    // ---- P30 slice 2: pgconfig.lock.json + the versioned plugin cache ------------------------

    // Lockfile round-trip, stable emission, and the ignore-unknown-versions rule.
    {
        namespace fs = std::filesystem;
        const fs::path base = fs::temp_directory_path() / "polyglot-p30-s2-lock";
        std::error_code ec;
        fs::remove_all(base, ec);
        fs::create_directories(base);

        check(!cli::loadLockfile(base).found, "P30 s2: a missing lockfile reads as not-found");

        cli::Lockfile lf;
        lf.packages["@mintplayer/polyglot-target-python"] = {"0.3.1",
            "https://registry.npmjs.org/@mintplayer/polyglot-target-python/-/polyglot-target-python-0.3.1.tgz",
            "sha512-abc"};
        lf.packages["@mintplayer/polyglot-target-php"] = {"0.3.0", "https://x/php-0.3.0.tgz", "sha512-def"};
        check(cli::saveLockfile(base, lf), "P30 s2: saveLockfile writes");
        const cli::Lockfile rt = cli::loadLockfile(base);
        check(rt.found && rt.packages.size() == 2 &&
                  rt.packages.at("@mintplayer/polyglot-target-python").version == "0.3.1" &&
                  rt.packages.at("@mintplayer/polyglot-target-python").integrity == "sha512-abc" &&
                  has(rt.packages.at("@mintplayer/polyglot-target-php").resolved, "php-0.3.0.tgz"),
              "P30 s2: lockfile round-trips");
        std::string emitted1, emitted2;
        cli::readFile(cli::lockfilePath(base), emitted1);
        cli::saveLockfile(base, rt);
        cli::readFile(cli::lockfilePath(base), emitted2);
        check(emitted1 == emitted2 && has(emitted1, "\"lockfileVersion\": 1"),
              "P30 s2: lockfile emission is byte-stable (committed diffs stay quiet)");

        cli::writeFile(cli::lockfilePath(base), "{ \"lockfileVersion\": 99, \"packages\": {} }");
        check(!cli::loadLockfile(base).found, "P30 s2: an unknown future lockfileVersion is ignored");
        fs::remove_all(base, ec);
    }

    // Versioned cache: store/load, tamper refusal, lock-integrity cross-check, atomic layout.
    {
        namespace fs = std::filesystem;
        const fs::path cache = fs::temp_directory_path() / "polyglot-p30-s2-cache";
        std::error_code ec;
        fs::remove_all(cache, ec);
#ifdef _WIN32
        _putenv_s("POLYGLOT_CACHE", cache.string().c_str());
#else
        setenv("POLYGLOT_CACHE", cache.string().c_str(), 1);
#endif
        const std::string name = "@mintplayer/polyglot-target-python";
        const std::string manifest = "{ \"schema\": 2, \"name\": \"python\" }";
        std::string err, loaded;

        check(!cli::cacheLoad(name, "0.3.1", "", loaded, err) && has(err, "no entry"),
              "P30 s2: a cold cache misses cleanly");
        check(cli::cacheStore(name, "0.3.1", "https://x/py-0.3.1.tgz", "sha512-abc", manifest, err),
              "P30 s2: cacheStore writes a versioned entry");
        check(fs::exists(cache / "@mintplayer" / "polyglot-target-python" / "0.3.1" / "polyglot-plugin.json"),
              "P30 s2: the cache layout is <name>/<version>/ (scoped names nest)");
        loaded.clear();
        check(cli::cacheLoad(name, "0.3.1", "sha512-abc", loaded, err) && loaded == manifest,
              "P30 s2: cacheLoad verifies and returns the manifest");
        check(!cli::cacheLoad(name, "0.3.1", "sha512-OTHER", loaded, err) && has(err, "lockfile pins"),
              "P30 s2: a lock-integrity mismatch refuses the entry");

        // Tamper with the cached manifest on disk: the re-hash refuses it.
        cli::writeFile(cache / "@mintplayer" / "polyglot-target-python" / "0.3.1" / "polyglot-plugin.json",
                       "{ \"schema\": 2, \"name\": \"python\", \"evil\": true }");
        check(!cli::cacheLoad(name, "0.3.1", "sha512-abc", loaded, err) && has(err, "hash mismatch"),
              "P30 s2: a tampered cache entry reads as absent-with-reason");
        // And a fresh store over the refused entry heals it.
        check(cli::cacheStore(name, "0.3.1", "https://x/py-0.3.1.tgz", "sha512-abc", manifest, err) &&
                  cli::cacheLoad(name, "0.3.1", "sha512-abc", loaded, err),
              "P30 s2: re-storing over a refused entry heals the cache");

        check(!cli::cacheStore("../evil", "1.0.0", "u", "i", manifest, err) && has(err, "unsafe"),
              "P30 s2: a path-hostile package name is refused outright");
        check(!cli::cacheStore("a/b/c", "1.0.0", "u", "i", manifest, err),
              "P30 s2: more than one scope slash is refused");
#ifdef _WIN32
        _putenv_s("POLYGLOT_CACHE", "");
#else
        unsetenv("POLYGLOT_CACHE");
#endif
        fs::remove_all(cache, ec);
    }

    // ---- P30 slice 3: the auto-download pipeline (fake transport, real pipeline) --------------
    {
        namespace fs = std::filesystem;

        // npm-shaped .tgz built in-test: ustar entry under package/ + stored-block gzip (the
        // decoder's Huffman paths are covered by the slice-0 golden vectors).
        auto tarFile = [](const std::string& path, const std::string& data) {
            std::string h(512, '\0');
            std::memcpy(&h[0], path.data(), path.size());
            auto oct = [&](std::size_t at, std::size_t width, unsigned long long v) {
                for (std::size_t i = width - 1; i-- > 0;) { h[at + i] = static_cast<char>('0' + (v & 7)); v >>= 3; }
                h[at + width - 1] = '\0';
            };
            oct(100, 8, 0644); oct(108, 8, 0); oct(116, 8, 0);
            oct(124, 12, data.size()); oct(136, 12, 0);
            std::memset(&h[148], ' ', 8);
            h[156] = '0';
            std::memcpy(&h[257], "ustar", 6); h[263] = '0'; h[264] = '0';
            unsigned sum = 0;
            for (unsigned char c : h) sum += c;
            for (int i = 5; i >= 0; --i) { h[148 + i] = static_cast<char>('0' + (sum & 7)); sum >>= 3; }
            h[154] = '\0'; h[155] = ' ';
            std::string padded = data;
            padded.resize((padded.size() + 511) & ~std::size_t(511), '\0');
            return h + padded + std::string(1024, '\0');
        };
        auto gzipStored = [](const std::string& p) {
            std::string gz("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03", 10);
            std::size_t off = 0;
            do {
                const std::size_t chunk = std::min<std::size_t>(p.size() - off, 65535);
                const bool last = off + chunk == p.size();
                gz += last ? '\x01' : '\x00';
                gz += static_cast<char>(chunk & 0xFF); gz += static_cast<char>(chunk >> 8);
                gz += static_cast<char>(~chunk & 0xFF); gz += static_cast<char>((~chunk >> 8) & 0xFF);
                gz.append(p, off, chunk);
                off += chunk;
            } while (off < p.size());
            const std::uint32_t crc = cli::crc32(p.data(), p.size());
            for (int i = 0; i < 4; ++i) gz += static_cast<char>(crc >> (8 * i));
            const std::uint32_t sz = static_cast<std::uint32_t>(p.size());
            for (int i = 0; i < 4; ++i) gz += static_cast<char>(sz >> (8 * i));
            return gz;
        };
        // A REAL, fully valid manifest: the in-box python plugin renamed (schema + spec name must agree).
        auto renamedManifest = [&](const std::string& newName) {
            std::string m;
            cli::readFile(cli::executablePath().parent_path() / "plugins" / "python" / "polyglot-plugin.json", m);
            const std::string from = "\"name\": \"python\"", to = "\"name\": \"" + newName + "\"";
            for (std::size_t at; (at = m.find(from)) != std::string::npos;) m.replace(at, from.size(), to);
            return m;
        };
        auto packument = [](const std::string& name, const std::string& version, const std::string& tarballUrl,
                            const std::string& integrity) {
            return "{ \"name\": \"" + name + "\", \"dist-tags\": { \"latest\": \"" + version + "\" }, "
                   "\"versions\": { \"0.9.0\": { \"dist\": { \"tarball\": \"" + tarballUrl + "-0.9.0\", "
                   "\"integrity\": \"sha512-stale\" } }, \"" + version + "\": { \"dist\": { \"tarball\": \"" +
                   tarballUrl + "\", \"integrity\": \"" + integrity + "\" } } } }";
        };

        const fs::path base = fs::temp_directory_path() / "polyglot-p30-s3";
        std::error_code ec;
        fs::remove_all(base, ec);
        fs::create_directories(base / "proj");
#ifdef _WIN32
        _putenv_s("POLYGLOT_CACHE", (base / "cache").string().c_str());
#else
        setenv("POLYGLOT_CACHE", (base / "cache").string().c_str(), 1);
#endif

        std::map<std::string, std::string> served; // url -> body; absent = network failure
        int hits = 0;
        cli::HttpGet fake = [&](const std::string& url, const std::vector<std::string>&, std::string& body,
                                std::string& err) {
            ++hits;
            if (const auto it = served.find(url); it != served.end()) { body = it->second; return true; }
            err = "http: offline (test)";
            return false;
        };

        // Happy path: packument -> maxSatisfying -> download -> verify -> extract -> cache -> lock.
        const std::string pkg = "@mintplayer/polyglot-target-pydemo";
        const std::string tgz = gzipStored(tarFile("package/polyglot-plugin.json", renamedManifest("pydemo")));
        const std::string tgzUrl = "https://registry.npmjs.org/" + pkg + "/-/polyglot-target-pydemo-1.1.0.tgz";
        served["https://registry.npmjs.org/@mintplayer%2Fpolyglot-target-pydemo"] =
            packument(pkg, "1.1.0", tgzUrl, cli::sriSha512(tgz));
        served[tgzUrl] = tgz;

        cli::PgConfig pc;
        pc.found = true;
        pc.dir = base / "proj";
        pc.targets = {"pydemo"};
        pc.dependencies = {{"pydemo", "^1.0.0"}};
        cli::writeFile(pc.dir / "pgconfig.json", "{}"); // stamp source for the memo key

        cli::ResolveState st1;
        cli::ResolveResult r1 = cli::resolvePluginDependencies(pc, fake, false, &st1);
        check(r1.ok && r1.lockChanged && r1.messages.empty(), "P30 s3: a versioned dependency resolves clean");
        check(findBackend("pydemo") != nullptr, "P30 s3: the downloaded plugin registers its target");
        check(hits == 2, "P30 s3: exactly two requests (packument + tarball)");
        const cli::Lockfile lk = cli::loadLockfile(pc.dir);
        check(lk.found && lk.packages.count(pkg) && lk.packages.at(pkg).version == "1.1.0" &&
                  lk.packages.at(pkg).integrity == cli::sriSha512(tgz),
              "P30 s3: the lockfile pins version + SRI integrity");

        // Already-registered (and dev-CLI in-box rule): a re-resolve with fresh state is network-free.
        hits = 0;
        cli::ResolveState st2;
        check(cli::resolvePluginDependencies(pc, fake, false, &st2).ok && hits == 0,
              "P30 s3: an already-registered dependency costs zero network");

        // Lock-first offline: cache + lock present, network dead, target not yet loaded.
        {
            const std::string pkg2 = "@mintplayer/polyglot-target-pydemo2";
            const std::string man2 = renamedManifest("pydemo2");
            std::string err;
            check(cli::cacheStore(pkg2, "2.0.0", "https://x/pydemo2-2.0.0.tgz", "sha512-pin", man2, err),
                  "P30 s3: (setup) pre-warmed cache");
            cli::Lockfile lock2 = cli::loadLockfile(pc.dir);
            lock2.packages[pkg2] = {"2.0.0", "https://x/pydemo2-2.0.0.tgz", "sha512-pin"};
            cli::saveLockfile(pc.dir, lock2);
            cli::PgConfig pc2 = pc;
            pc2.targets = {"pydemo2"};
            pc2.dependencies = {{"pydemo2", "^2.0.0"}};
            served.clear();
            hits = 0;
            cli::ResolveState st;
            const cli::ResolveResult r = cli::resolvePluginDependencies(pc2, fake, false, &st);
            check(r.ok && hits == 0 && findBackend("pydemo2") != nullptr,
                  "P30 s3: lock+cache resolve fully OFFLINE (zero network I/O)");
        }

        // Integrity mismatch: the served tarball does not match the packument's SRI -> refused, uncached.
        {
            const std::string pkg3 = "@mintplayer/polyglot-target-pydemo3";
            const std::string tgz3 = gzipStored(tarFile("package/polyglot-plugin.json", renamedManifest("pydemo3")));
            const std::string url3 = "https://registry.npmjs.org/" + pkg3 + "/-/x-3.0.0.tgz";
            served["https://registry.npmjs.org/@mintplayer%2Fpolyglot-target-pydemo3"] =
                packument(pkg3, "3.0.0", url3, cli::sriSha512("something else entirely"));
            served[url3] = tgz3;
            cli::PgConfig pc3 = pc;
            pc3.dependencies = {{"pydemo3", "^3.0.0"}};
            cli::ResolveState st;
            const cli::ResolveResult r = cli::resolvePluginDependencies(pc3, fake, false, &st);
            check(!r.ok && r.messages.size() == 1 && has(r.messages[0], "integrity mismatch"),
                  "P30 s3: a tarball failing its SRI check is refused with the mismatch named");
            std::string m, err;
            check(!cli::cacheLoad(pkg3, "3.0.0", "", m, err) && findBackend("pydemo3") == nullptr,
                  "P30 s3: nothing is cached or registered from a refused tarball");
        }

        // Unsatisfiable range + failure memoization (no re-network within a config generation).
        // (A fresh package name: an already-REGISTERED target short-circuits on the in-box rule.)
        {
            cli::PgConfig pc4 = pc;
            pc4.dependencies = {{"pydemo4", "^9.0.0"}};
            served["https://registry.npmjs.org/@mintplayer%2Fpolyglot-target-pydemo4"] =
                packument("@mintplayer/polyglot-target-pydemo4", "1.1.0", tgzUrl + "-p4", cli::sriSha512(tgz));
            cli::ResolveState st;
            cli::ResolveResult r = cli::resolvePluginDependencies(pc4, fake, false, &st);
            check(!r.ok && has(r.messages[0], "no published version satisfies '^9.0.0'"),
                  "P30 s3: an unsatisfiable range names the range and what exists");
            hits = 0;
            r = cli::resolvePluginDependencies(pc4, fake, false, &st);
            check(!r.ok && hits == 0 && has(r.messages[0], "no published version satisfies"),
                  "P30 s3: a failed dependency is memoized per config generation (no network hammering)");
        }

        // Offline with no lock: the transport's diagnostic reaches the user.
        {
            cli::PgConfig pc5 = pc;
            pc5.dependencies = {{"pydemo9", "^1.0.0"}};
            served.clear();
            cli::ResolveState st;
            const cli::ResolveResult r = cli::resolvePluginDependencies(pc5, fake, false, &st);
            check(!r.ok && has(r.messages[0], "offline (test)"),
                  "P30 s3: offline-with-no-lock surfaces the transport diagnostic");
        }

        // G37 (wave 2): a lock pin OUTSIDE an edited range re-resolves (online), and REFUSES offline —
        // never silently keeps the stale plugin.
        {
            const std::string pkg7 = "@mintplayer/polyglot-target-pydemo7";
            cli::Lockfile stale = cli::loadLockfile(pc.dir);
            stale.found = true;
            stale.packages[pkg7] = {"0.9.0", "https://registry.npmjs.org/x-0.9.0.tgz", "sha512-stale"};
            check(cli::saveLockfile(pc.dir, stale), "G37: (setup) stale lock pin written");

            const std::string tgz7 = gzipStored(tarFile("package/polyglot-plugin.json", renamedManifest("pydemo7")));
            const std::string url7 = "https://registry.npmjs.org/" + pkg7 + "/-/p7-1.1.0.tgz";
            served.clear();
            served["https://registry.npmjs.org/@mintplayer%2Fpolyglot-target-pydemo7"] =
                packument(pkg7, "1.1.0", url7, cli::sriSha512(tgz7));
            served[url7] = tgz7;
            hits = 0;
            cli::PgConfig pc7 = pc;
            pc7.dependencies = {{"pydemo7", "^1.0.0"}}; // the 0.9.0 pin does NOT satisfy this
            cli::ResolveState st;
            const cli::ResolveResult r = cli::resolvePluginDependencies(pc7, fake, false, &st);
            const cli::Lockfile after = cli::loadLockfile(pc.dir);
            check(r.ok && hits > 0 && after.packages.count(pkg7) && after.packages.at(pkg7).version == "1.1.0",
                  "G37: a pin outside the edited range re-resolves and advances the lock");
        }
        {
            const std::string pkg8 = "@mintplayer/polyglot-target-pydemo8";
            cli::Lockfile stale = cli::loadLockfile(pc.dir);
            stale.found = true;
            stale.packages[pkg8] = {"0.9.0", "https://registry.npmjs.org/x8-0.9.0.tgz", "sha512-stale"};
            cli::saveLockfile(pc.dir, stale);
            served.clear(); // network dead
            cli::PgConfig pc8 = pc;
            pc8.dependencies = {{"pydemo8", "^1.0.0"}};
            cli::ResolveState st;
            const cli::ResolveResult r = cli::resolvePluginDependencies(pc8, fake, false, &st);
            check(!r.ok, "G37: an out-of-range pin refuses OFFLINE (never silently keeps the old plugin)");
        }

        // G37 (wave 2): `update=true` advances a SATISFIED pin to the newest satisfying version.
        {
            const std::string pkg9 = "@mintplayer/polyglot-target-pydemo9u";
            const std::string tgz12 = gzipStored(tarFile("package/polyglot-plugin.json", renamedManifest("pydemo9u")));
            const std::string url12 = "https://registry.npmjs.org/" + pkg9 + "/-/p9-1.2.0.tgz";
            cli::Lockfile lk9 = cli::loadLockfile(pc.dir);
            lk9.found = true;
            lk9.packages[pkg9] = {"1.1.0", "https://registry.npmjs.org/x9-1.1.0.tgz", "sha512-oldpin"};
            cli::saveLockfile(pc.dir, lk9);
            served.clear();
            served["https://registry.npmjs.org/@mintplayer%2Fpolyglot-target-pydemo9u"] =
                packument(pkg9, "1.2.0", url12, cli::sriSha512(tgz12));
            served[url12] = tgz12;
            cli::PgConfig pc9 = pc;
            pc9.dependencies = {{"pydemo9u", "^1.0.0"}};
            hits = 0;
            cli::ResolveState st;
            const cli::ResolveResult r = cli::resolvePluginDependencies(pc9, fake, /*update=*/true, &st);
            const cli::Lockfile after = cli::loadLockfile(pc.dir);
            check(r.ok && hits > 0 && after.packages.count(pkg9) && after.packages.at(pkg9).version == "1.2.0",
                  "G37: update=true advances a satisfied pin to the newest satisfying version");
        }

        // A package that is not a Polyglot plugin (no manifest in the tarball) is named as such.
        {
            const std::string pkg6 = "@mintplayer/polyglot-target-notaplugin";
            const std::string tgz6 = gzipStored(tarFile("package/index.js", "module.exports = 1;"));
            const std::string url6 = "https://registry.npmjs.org/" + pkg6 + "/-/n-1.0.0.tgz";
            served["https://registry.npmjs.org/@mintplayer%2Fpolyglot-target-notaplugin"] =
                packument(pkg6, "1.0.0", url6, cli::sriSha512(tgz6));
            served[url6] = tgz6;
            cli::PgConfig pc6 = pc;
            pc6.dependencies = {{"notaplugin", "1.0.0"}};
            cli::ResolveState st;
            const cli::ResolveResult r = cli::resolvePluginDependencies(pc6, fake, false, &st);
            check(!r.ok && has(r.messages[0], "not a Polyglot target plugin"),
                  "P30 s3: a non-plugin npm package is refused by name");
        }

#ifdef _WIN32
        _putenv_s("POLYGLOT_CACHE", "");
#else
        unsetenv("POLYGLOT_CACHE");
#endif
        fs::remove_all(base, ec);
    }

    // ---- P30 slice 4: the derived reference backend (no hard-bound "csharp") -----------------
    {
        // Among the loaded fixture set, python/php carry `emulated` stances; csharp and typescript
        // are all-native, and name order breaks the tie — so the DERIVATION lands on csharp. The
        // assertion pins the derivation over this known set, not a hard-bound name in the product.
        const Backend* ref = cli::referenceBackend();
        check(ref != nullptr && ref->name() == "csharp",
              "P30 s4: referenceBackend derives the all-native, name-first backend");
        bool allNative = true;
        for (const Feature f : kAllFeatures) allNative &= ref->capabilityStance(f) == "native";
        check(allNative, "P30 s4: the derived reference backend is all-native (never warns spuriously)");
    }

    // ---- P30 slice 7: include file-mapping rules (glob / templates / routing / closure rule) --

    // Glob semantics table (`**` = zero-or-more segments, captured as RecursiveDir; `*`/`?` stay
    // within one segment) — pinned here so engine drift is a test failure, not a surprise.
    {
        auto m = [](const char* pat, const char* path) { return cli::globMatch(pat, path); };
        check(m("**/*.pg", "a.pg").matched && m("**/*.pg", "a.pg").recursiveDir.empty(),
              "P30 s7: ** matches zero segments (empty RecursiveDir)");
        check(m("**/*.pg", "x/y/a.pg").matched && m("**/*.pg", "x/y/a.pg").recursiveDir == "x/y/",
              "P30 s7: ** captures the matched span as RecursiveDir (trailing '/')");
        check(m("./**/*.pg", "x/a.pg").matched, "P30 s7: a leading ./ is tolerated on patterns");
        check(m("src/*.pg", "src/a.pg").matched && !m("src/*.pg", "src/sub/a.pg").matched,
              "P30 s7: * stays within one segment");
        check(m("FruitCake/polyglot/*.pg", "FruitCake/polyglot/fruitcake_solver.pg").matched &&
                  !m("FruitCake/polyglot/*.pg", "Snake/polyglot/snake_solver.pg").matched,
              "P30 s7: the dogfood per-destination rule shape matches exactly");
        check(m("a?c.pg", "abc.pg").matched && !m("a?c.pg", "abbc.pg").matched, "P30 s7: ? is one char");
        check(m("src/**/gen/*.pg", "src/a/b/gen/x.pg").recursiveDir == "a/b/",
              "P30 s7: RecursiveDir is only the **-matched span");
    }

    // Output templates: expansion + the unknown-placeholder refusal (extension is auto-appended).
    {
        cli::TemplateValues v{"solver", "FruitCake/polyglot", "x/y/", "typescript"};
        std::string out, err;
        check(cli::expandOutputTemplate("dist/%(TargetLanguage)/%(RecursiveDir)%(Filename)", v, out, err) &&
                  out == "dist/typescript/x/y/solver",
              "P30 s7: template placeholders expand");
        check(cli::expandOutputTemplate("%(Directory)/%(Filename)", v, out, err) &&
                  out == "FruitCake/polyglot/solver",
              "P30 s7: %(Directory)/%(Filename) is the next-to-source shape");
        check(!cli::expandOutputTemplate("%(TargetExtension)/x", v, out, err) &&
                  has(err, "unknown placeholder") && has(err, "appended automatically"),
              "P30 s7: an unknown placeholder is refused, naming the auto-appended extension");
    }

    // Rule routing: first-match-wins, target filtering, fallback, config-relative resolution.
    // (Fixture paths are temp-dir-based so they are genuinely ABSOLUTE on POSIX too — a literal
    // "C:/proj" is a relative path there, and the fs arithmetic diverges: the linux CI catch.)
    {
        namespace fs = std::filesystem;
        const fs::path proj = (fs::temp_directory_path() / "p30-s7-route" / "proj").lexically_normal();
        cli::PgConfig pc;
        pc.found = true;
        pc.dir = proj;
        pc.include.push_back({"FruitCake/polyglot/*.pg", {"typescript"}, "../web/app/fruit-cake/%(Filename)"});
        pc.include.push_back({"**/*.pg", {"typescript"}, "gen/%(RecursiveDir)%(Filename)"});
        std::string err;

        auto r1 = cli::routeIncludeOutput(pc, proj / "FruitCake/polyglot/fruitcake_solver.pg",
                                          "typescript", err);
        check(r1 && *r1 == (proj.parent_path() / "web/app/fruit-cake/fruitcake_solver").lexically_normal(),
              "P30 s7: the FIRST matching rule wins (narrow above broad)");
        auto r2 = cli::routeIncludeOutput(pc, proj / "Snake/polyglot/snake_solver.pg",
                                          "typescript", err);
        check(r2 && *r2 == (proj / "gen/Snake/polyglot/snake_solver").lexically_normal(),
              "P30 s7: the broad rule mirrors the tree via %(RecursiveDir)");
        check(!cli::routeIncludeOutput(pc, proj / "Snake/polyglot/snake_solver.pg", "csharp", err) &&
                  err.empty(),
              "P30 s7: a target no rule names falls through cleanly (no match, no error)");
        check(!cli::routeIncludeOutput(pc, proj.parent_path() / "elsewhere/a.pg", "typescript", err),
              "P30 s7: a source outside the config's tree never routes");
    }

    // Closure outputs: prelude follows the entry; a split closure is refused, never miscompiled.
    {
        namespace fs = std::filesystem;
        const fs::path proj = (fs::temp_directory_path() / "p30-s7-closure" / "proj").lexically_normal();
        const fs::path outAlt = (fs::temp_directory_path() / "p30-s7-closure" / "out").lexically_normal();
        cli::PgConfig pc;
        pc.found = true;
        pc.dir = proj;
        pc.include.push_back({"main.pg", {"typescript"}, "web/%(Filename)"});
        EmitResult fake;
        fake.ok = true;
        fake.code = "entry";
        fake.modules.push_back({"util", "code", (proj / "util.pg").string()});
        fake.modules.push_back({"__polyglot_prelude", "prelude", ""});
        std::vector<fs::path> outs;
        std::string err;

        // Entry routed to web/, util unmatched -> fallback dir: a SPLIT closure must refuse.
        check(!cli::resolveClosureOutputs(pc, false, proj / "main.pg", fake, "typescript", ".ts",
                                          proj, outs, err) &&
                  has(err, "split the import closure"),
              "P30 s7: include rules splitting a closure are refused with the split named");

        // A rule covering the whole closure routes everything together; the prelude follows the entry.
        pc.include.clear();
        pc.include.push_back({"**/*.pg", {"typescript"}, "web/%(Filename)"});
        check(cli::resolveClosureOutputs(pc, false, proj / "main.pg", fake, "typescript", ".ts",
                                         proj, outs, err) &&
                  outs.size() == 3 && outs[0] == proj / "web/main.ts" &&
                  outs[1] == proj / "web/util.ts" && outs[2] == proj / "web/__polyglot_prelude.ts",
              "P30 s7: a closure-covering rule routes entry+modules together, prelude follows the entry");

        // flagRouted (explicit --target + --out) bypasses the rules wholesale.
        check(cli::resolveClosureOutputs(pc, true, proj / "main.pg", fake, "typescript", ".ts",
                                         outAlt, outs, err) &&
                  outs[0] == outAlt / "main.ts" && outs[2] == outAlt / "__polyglot_prelude.ts",
              "P30 s7: explicit --target + --out keeps flag semantics (rules bypassed)");
    }

    // Core: ModuleFile carries its source path (per-file routing needs to attribute linked modules).
    {
        MapModuleResolver res({{"lib", "fn dbl(x: i32): i32 { return x * 2 }\n"}});
        EmitResult r = compile("import { dbl } from \"lib\"\nfn f(): i32 {\n  return dbl(2)\n}\n",
                               findTarget("csharp"), &res, LibConfig{});
        check(r.ok && r.modules.size() == 1 && r.modules[0].sourcePath == "lib",
              "P30 s7: a linked module surfaces its canonical source path");
    }

    // pgconfig parse: include rules round-trip; a malformed rule is an ERROR, never a silent no-op.
    {
        namespace fs = std::filesystem;
        const fs::path base = fs::temp_directory_path() / "polyglot-p30-s7-cfg";
        std::error_code ec;
        fs::remove_all(base, ec);
        fs::create_directories(base);
        cli::writeFile(base / "pgconfig.json",
                       "{ \"targets\": [\"csharp\", \"typescript\"], \"include\": ["
                       "{ \"pattern\": \"**/*.pg\", \"target\": [\"typescript\", \"python\"], \"output\": \"gen/%(Filename)\" },"
                       "{ \"pattern\": \"a/*.pg\", \"output\": \"b/%(Filename)\" } ] }");
        const cli::PgConfig pc = cli::loadPgConfig(base);
        check(pc.errors.empty() && pc.include.size() == 2 && pc.include[0].targets.size() == 2 &&
                  pc.include[0].targets[1] == "python" && pc.include[1].targets.empty() &&
                  pc.include[1].output == "b/%(Filename)",
              "P30 s7: include rules parse (target string-array and absent forms)");
        cli::writeFile(base / "pgconfig.json", "{ \"include\": [ { \"pattern\": \"**/*.pg\" } ] }");
        const cli::PgConfig bad = cli::loadPgConfig(base);
        check(!bad.errors.empty() && has(bad.errors[0], "pattern") && has(bad.errors[0], "output"),
              "P30 s7: an include rule missing pattern/output is a manifest error, not a silent no-op");
        fs::remove_all(base, ec);
    }

    // ---- P38/issue #69 slice 3: originMapping is manifest DATA, never a target-name comparison ----
    {
        // Each target's origin story round-trips from its own manifest. C# writes `#line` pragmas into the
        // source; TS writes a v3 sidecar beside it; Python and PHP say nothing and are unaffected.
        const OriginMapping& cs = findBackend("csharp")->originMapping();
        const OriginMapping& ts = findBackend("typescript")->originMapping();
        check(cs.style == OriginMapping::Style::Directive && cs.emitsDirectives() && cs.line == "#line $n \"$f\"" &&
                  cs.hidden == "#line hidden" && cs.column == 0,
              "P38 s3: C# declares the directive style with both templates");
        check(ts.style == OriginMapping::Style::SourceMapV3 && ts.emitsSourceMap() &&
                  ts.sidecarExtension == ".map" && has(ts.footer, "sourceMappingURL"),
              "P38 s3: TypeScript declares the v3-sidecar style");
        // P39/issue #71 reversed this: Python and PHP now declare the SAME v3-sidecar sink, and doing so
        // took a manifest entry and no engine change — origin recording was always target-neutral, so
        // they were missing a sink DECLARATION, not the data.
        const Backend* py = findBackend("python");
        const Backend* php = findBackend("php");
        check(py && py->originMapping().emitsSourceMap() && py->originMapping().sidecarExtension == ".map" &&
                  has(py->originMapping().footer, "sourceMappingURL"),
              "P39: Python declares the v3-sidecar style (was Style::None through P38)");
        check(php && php->originMapping().emitsSourceMap() &&
                  php->originMapping().sidecarExtension == ".map" &&
                  has(php->originMapping().footer, "sourceMappingURL"),
              "P39: PHP declares the v3-sidecar style (was Style::None through P38)");

        // The vocabulary is CLOSED: an unknown style is a plugin LOAD ERROR, never a silent default —
        // that is what makes version skew loud (the `blockStyle` precedent).
        const std::string head =
            "{\"schema\":2,\"name\":\"t\",\"spec\":{\"name\":\"t\"},"
            "\"rules\":{\"Program\":{\"line\":\"\"},\"Type\":{\"line\":\"\"}},";
        std::string err;
        check(!validateBackend(head + "\"originMapping\":{\"style\":\"dwarf\"}}", err) &&
                  has(err, "unknown originMapping style"),
              "P38 s3: an unknown originMapping style refuses at load (never a silent no-op)");
        err.clear();
        check(!validateBackend(head + "\"originMapping\":{\"style\":\"directive\",\"line\":\"#line $n\"}}", err) &&
                  has(err, "needs both"),
              "P38 s3: a directive style without both templates refuses (a declared-but-dead sink is a bug)");
        err.clear();
        check(!validateBackend(head + "\"originMapping\":\"yes\"}", err) && has(err, "must be an object"),
              "P38 s3: a non-object originMapping refuses");
    }

    // ---- P38/issue #69 slice 8: Base64-VLQ + the v3 source map ------------------------------------
    {
        // The encoding the v3 `mappings` field is built from: sign in the low bit, 5-bit groups
        // little-endian, bit 6 = "another group follows". These are the values every v3 reference
        // implementation agrees on, so a wrong encoder shows up here rather than in a consumer's tooling.
        check(vlqEncode(0) == "A", "P38 s8: VLQ 0");
        check(vlqEncode(1) == "C", "P38 s8: VLQ 1");
        check(vlqEncode(-1) == "D", "P38 s8: VLQ -1");
        check(vlqEncode(15) == "e", "P38 s8: VLQ 15");
        check(vlqEncode(16) == "gB", "P38 s8: VLQ 16 (two groups)");
        check(vlqEncode(-16) == "hB", "P38 s8: VLQ -16");
        check(vlqEncode(123) == "2H", "P38 s8: VLQ 123");

        SourceMapInput in;
        in.file = "out.ts";
        in.sources = {"../src/a.pg"};
        in.sourcesContent = {"fn main() {}\n"};
        in.fileIdToSourceIndex = {-1, 0};        // fileId 0 = unknown, fileId 1 -> sources[0]
        in.origins = {{1, 1, 4}, {3, 1, 5}, {4, 9, 9}}; // the last has an unmapped fileId
        const std::string doc = buildSourceMapV3(in);
        check(has(doc, "\"version\":3"), "P38 s8: the map declares v3");
        check(has(doc, "\"file\":\"out.ts\""), "P38 s8: the map names the emitted file");
        check(has(doc, "\"sources\":[\"../src/a.pg\"]"), "P38 s8: sources are map-relative, as given");
        check(has(doc, "fn main()"), "P38 s8: sourcesContent is embedded (survives bundling/relocation)");
        // Output line 1 -> source line 4, then line 3 -> line 5. Two ';' separate them (line 2 is
        // unmapped), and the out-of-range fileId is skipped rather than poisoning the table.
        check(has(doc, "\"mappings\":\"AAGA;;AACA\""), "P38 s8: mappings encode line deltas and skip unmapped origins");
    }

    // ---- P30 slice 8: cross-directory import specifiers (crossDirImports) ---------------------
    {
        // The manifest flag round-trips: TS declares it, Python/C# don't (their module systems make
        // cross-dir imports non-trivial — the closure rule stays for them).
        check(findBackend("typescript")->crossDirImports() && !findBackend("python")->crossDirImports() &&
                  !findBackend("csharp")->crossDirImports(),
              "P30 s8: crossDirImports parses from the manifest (TS yes, Python/C# no)");

        // Flat (no router): the emitted specifier is byte-identical to the historical "./<name>".
        MapModuleResolver res({{"lib", "fn dbl(x: i32): i32 { return x * 2 }\n"}});
        const std::string src = "import { dbl } from \"lib\"\nfn f(): i32 {\n  return dbl(2)\n}\n";
        EmitResult flat = compile(src, findTarget("typescript"), &res, LibConfig{});
        check(flat.ok && has(flat.code, "from \"./lib\""),
              "P30 s8: without a router the TS specifier stays ./<name> (byte-identical)");

        // Routed apart: a real relative specifier climbs and descends between the routed dirs.
        LibConfig up;
        up.moduleOutputDir = [](const std::string& origin) {
            return origin.empty() ? std::string("C:/o/app") : std::string("C:/o/shared/geom");
        };
        EmitResult cross = compile(src, findTarget("typescript"), &res, up);
        check(cross.ok && has(cross.code, "from \"../shared/geom/lib\""),
              "P30 s8: a routed-apart module gets a climbing relative specifier");

        LibConfig down;
        down.moduleOutputDir = [](const std::string& origin) {
            return origin.empty() ? std::string("C:/o") : std::string("C:/o/sub");
        };
        EmitResult below = compile(src, findTarget("typescript"), &res, down);
        check(below.ok && has(below.code, "from \"./sub/lib\""),
              "P30 s8: a below-tree module gets a ./-prefixed specifier (never a bare package name)");

        // A non-crossDirImports target ignores the router entirely (flag-gated, never half-applied).
        EmitResult py = compile(src, findTarget("python"), &res, up);
        check(py.ok && has(py.code, "from lib import") && !has(py.code, "../"),
              "P30 s8: python ignores the router (bare basename, closure rule still guards it)");

        // The closure check permits a split for a crossDirImports target (allowSplit).
        namespace fs = std::filesystem;
        const fs::path proj = (fs::temp_directory_path() / "p30-s8-split" / "proj").lexically_normal();
        cli::PgConfig pc;
        pc.found = true;
        pc.dir = proj;
        pc.include.push_back({"main.pg", {"typescript"}, "app/%(Filename)"});
        EmitResult fake;
        fake.ok = true;
        fake.code = "entry";
        fake.modules.push_back({"util", "code", (proj / "lib/util.pg").string()});
        std::vector<fs::path> outs;
        std::string err;
        check(!cli::resolveClosureOutputs(pc, false, proj / "main.pg", fake, "typescript", ".ts",
                                          proj, outs, err, /*allowSplit=*/false),
              "P30 s8: (control) the split still refuses without the capability");
        check(cli::resolveClosureOutputs(pc, false, proj / "main.pg", fake, "typescript", ".ts",
                                         proj, outs, err, /*allowSplit=*/true) &&
                  outs[0] == proj / "app/main.ts" && outs[1] == proj / "util.ts",
              "P30 s8: allowSplit routes a closure across directories (entry routed, module fallback)");
    }

    // issue #33 — hex literals: real radix lexing + radix-aware suffix stripping. The trailing lowercase
    // 'f'/'d' of a hex literal was eaten by the float-suffix strip (`0xff` -> `0xf`, `0x11d` -> `0x11`),
    // and a `f32`/`f64` tail stripped THREE digits (`0xf32` -> invalid `0x`).
    {
        DiagnosticBag d;
        auto t = lex("0xff 0x11d 0xf32 0xFF 0xffi64 0x1D", d);
        check(!d.hasErrors(), "issue#33: hex literals lex clean");
        auto textAt = [&](std::size_t i) { return i < t.size() ? t[i].text : std::string(); };
        check(t[0].kind == TokKind::IntLit && textAt(0) == "0xff", "issue#33: 0xff keeps its digits");
        check(textAt(1) == "0x11d", "issue#33: 0x11d keeps its trailing 'd'");
        check(textAt(2) == "0xf32", "issue#33: 0xf32 is three hex digits, not a float suffix");
        check(textAt(3) == "0xFF", "issue#33: uppercase hex unchanged");
        check(textAt(4) == "0xffi64", "issue#33: integer width suffix still accepted on hex");
        DiagnosticBag bad;
        lex("0xffz", bad);
        check(bad.hasErrors(), "issue#33: unknown suffix on a hex literal is a diagnostic");
        DiagnosticBag bare;
        lex("0x + 1", bare);
        check(bare.hasErrors(), "issue#33: '0x' with no digits is a diagnostic");
        // End-to-end: the emitted code carries the intact literal on every target (lowering was the
        // truncation site — stripNumericSuffix ran suffix rules on radix literals).
        const char* prog = "fn main() {\n  print(0xff)\n  print(0x11d)\n}\n";
        for (const char* tgt : {"csharp", "typescript", "python", "php"}) {
            EmitResult r = compileStd(prog, findTarget(tgt));
            check(r.ok && has(r.code, "0xff") && has(r.code, "0x11d"),
                  std::string("issue#33: emitted ") + tgt + " keeps 0xff/0x11d intact");
        }
    }

    // issue #34 — the portable char→ordinal path: std.strings codePointAt maps per target, and the
    // silently-miscompiling `(i32)char` is a diagnostic naming the fix ((int)'A' was 65 on C# but 0 on
    // TS/PHP and the cast was dropped on Python).
    {
        const char* prog = "import \"std.strings\"\nfn main() {\n  print(\"A\".codePointAt(0))\n}\n";
        EmitResult cs = compileStd(prog, findTarget("csharp"));
        check(cs.ok && has(cs.code, "char.ConvertToUtf32("), "issue#34: C# codePointAt -> char.ConvertToUtf32");
        EmitResult ts = compileStd(prog, findTarget("typescript"));
        check(ts.ok && has(ts.code, "_pg_codePointAt(\"A\", 0)") && has(ts.code, "self.codePointAt(index)!"),
              "issue#34/P40 s8: TS codePointAt -> the _pg_codePointAt guard over .codePointAt()!");
        EmitResult py = compileStd(prog, findTarget("python"));
        check(py.ok && has(py.code, "ord("), "issue#34: Python codePointAt -> ord()");
        EmitResult php = compileStd(prog, findTarget("php"));
        check(php.ok && has(php.code, "__pg_u16_cp($self, $index)"),
              "issue#34/P40 s8: PHP codePointAt -> the UTF-16 helper (iconv, no mbstring)");
        EmitResult castChar = compileStd("fn main() {\n  var c = 'A'\n  print((i32)c)\n}\n", findTarget("csharp"));
        check(!castChar.ok, "issue#34: (i32) on a char is rejected");
        bool named = false;
        for (const auto& dg : castChar.diagnostics) if (has(dg.message, "codePointAt")) named = true;
        check(named, "issue#34: the char-cast diagnostic names codePointAt");
        EmitResult callCast = compileStd("fn main() {\n  var c = 'A'\n  print(i32(c))\n}\n", findTarget("csharp"));
        check(!callCast.ok, "issue#34: call-syntax i32(char) is rejected too");
        EmitResult castStr = compileStd("fn main() {\n  var s = \"x\"\n  print((i32)s)\n}\n", findTarget("csharp"));
        check(!castStr.ok, "issue#34: (i32) on a string stays rejected (regression)");
    }

    // issue #35 — PHP: module globals must be visible inside methods and property getters, not just free
    // functions (ir::Method now carries globalRefs; the PHP MethodDecl emits the `global $…;` prologue).
    {
        auto phpOf = [&](const char* src) { return compileStd(src, findTarget("php")); };
        EmitResult method = phpOf("let TABLE: i32[] = [10, 20, 30]\n"
                                  "class Box {\n  fn lookup(i: i32): i32 => TABLE[i]\n}\nfn main() {}\n");
        check(method.ok && has(method.code, "global $TABLE;"), "issue#35: PHP class method gets `global`");
        EmitResult getter = phpOf("const LIMIT: i32 = 7\n"
                                  "record Vec(x: i32) {\n  let cap: i32 => LIMIT\n}\nfn main() {}\n");
        check(getter.ok && has(getter.code, "global $LIMIT;"), "issue#35: PHP property getter gets `global`");
        EmitResult lambda = phpOf("let BASE: i32 = 5\n"
                                  "class Box {\n  fn all(): i32 {\n    let f = (x: i32) => x + BASE\n    return f(1)\n  }\n}\nfn main() {}\n");
        check(lambda.ok && has(lambda.code, "global $BASE;"), "issue#35: closure inside a method reaches the global");
        EmitResult shadow = phpOf("let TABLE: i32[] = [1]\n"
                                  "class Box {\n  fn own(TABLE: i32): i32 => TABLE\n}\nfn main() {}\n");
        check(shadow.ok && !has(shadow.code, "global $TABLE;"), "issue#35: a shadowing param suppresses `global`");
        EmitResult freeFn = phpOf("let TABLE: i32[] = [10, 20]\nfn readIt(): i32 => TABLE[1]\nfn main() {}\n");
        check(freeFn.ok && has(freeFn.code, "global $TABLE;"), "issue#35: free-function path unchanged (regression)");
        // The C# leg (found by module_globals.pg): globals are fields of PolyglotProgram, so a member of
        // ANOTHER type referencing one bare didn't compile. `using static PolyglotProgram;` (emitted only
        // when globals exist) + `internal` fields make them reachable with correct shadowing semantics.
        EmitResult cs = compileStd("let TABLE: i32[] = [10, 20]\n"
                                   "class Box {\n  fn lookup(i: i32): i32 => TABLE[i]\n}\nfn main() {}\n",
                                   findTarget("csharp"));
        check(cs.ok && has(cs.code, "using static PolyglotProgram;") && has(cs.code, "internal static readonly"),
              "issue#35: C# type members reach module globals (using static + internal fields)");
        EmitResult csNone = compileStd("fn main() {}\n", findTarget("csharp"));
        check(csNone.ok && !has(csNone.code, "using static"),
              "issue#35: no globals -> no using-static line (byte-gate)");
    }

    // issue #36 — PHP List<T>/T[] are \ArrayObject (true reference semantics): construction boxes, and
    // clear/removeAll/removeAt mutate in place through the handle instead of reassigning a copied array.
    {
        const char* prog =
            "fn mutate(xs: List<i32>) {\n  xs.add(99)\n}\n"
            "fn main() {\n  var ys: List<i32> = [1]\n  mutate(ys)\n  print(ys.count)\n"
            "  ys.removeAt(0)\n  ys.clear()\n  ys.removeAll((v) => v > 0)\n}\n";
        EmitResult php = compileStd(prog, findTarget("php"));
        check(php.ok && has(php.code, "new \\ArrayObject([1])"), "issue#36: PHP list literal boxes into \\ArrayObject");
        check(php.ok && has(php.code, "->exchangeArray("), "issue#36: PHP size-mutation goes through exchangeArray");
        check(php.ok && !has(php.code, "$ys = ["), "issue#36: no value-typed array reassignment remains");
        EmitResult arr = compileStd("fn main() {\n  var a: i32[] = [1, 2]\n  a[0] = 9\n  print(a[0])\n}\n",
                                    findTarget("php"));
        check(arr.ok && has(arr.code, "new \\ArrayObject([1, 2])"), "issue#36: PHP T[] array boxes too");
        EmitResult ts = compileStd(prog, findTarget("typescript"));
        check(ts.ok && !has(ts.code, "ArrayObject"), "issue#36: TS output untouched by the PHP boxing");
    }

    // issue #29 — interfaces made honest: implements-conformance + nominal assignability + the
    // C#-convention override rule in the checker, abc.ABC emission on Python, and the fake typed-pattern
    // narrowing refused.
    {
        const std::string iface = "interface IPgNet {\n  fn forward(x: i32): i32\n}\n";
        EmitResult missing = compileStd(iface + "class Net : IPgNet {\n  fn other(): i32 => 1\n}\nfn main() {}\n",
                                        findTarget("csharp"));
        check(!missing.ok, "issue#29: missing interface method is a diagnostic");
        EmitResult wrongSig = compileStd(iface + "class Net : IPgNet {\n  fn forward(x: string): i32 => 1\n}\nfn main() {}\n",
                                         findTarget("csharp"));
        check(!wrongSig.ok, "issue#29: wrong implementing signature is a diagnostic");
        EmitResult okImpl = compileStd(iface +
            "class Net : IPgNet {\n  fn forward(x: i32): i32 => x + 1\n}\n"
            "fn drive(net: IPgNet): i32 => net.forward(1)\n"
            "fn feed(n: Net): i32 => drive(n)\nfn main() {}\n", findTarget("csharp"));
        check(okImpl.ok, "issue#29: conforming class widens into an interface-typed param");
        EmitResult badAssign = compileStd(iface +
            "class Other {\n  fn forward(x: i32): i32 => x\n}\n"
            "fn drive(net: IPgNet): i32 => net.forward(1)\n"
            "fn feed(o: Other): i32 => drive(o)\nfn main() {}\n", findTarget("csharp"));
        check(!badAssign.ok, "issue#29: non-implementing class refused at an interface-typed slot (nominal)");
        // Generic interface + record implementer (the sample-04 shape) stays green.
        EmitResult generic = compileStd(
            "interface Comparable<T> {\n  fn compareTo(other: T): i32\n}\n"
            "record Version(major: i32) : Comparable<Version> {\n  fn compareTo(o: Version): i32 => this.major - o.major\n}\n"
            "fn main() {}\n", findTarget("csharp"));
        check(generic.ok, "issue#29: generic interface conformance (type-arg substitution) accepts sample-04 shape");
        // `override` follows C# conventions: base-class open members only, never interface implementations.
        EmitResult strayOverride = compileStd(iface +
            "class Net : IPgNet {\n  override fn forward(x: i32): i32 => x\n}\nfn main() {}\n", findTarget("csharp"));
        check(!strayOverride.ok, "issue#29: 'override' on an interface implementation is a diagnostic");
        EmitResult baseOverride = compileStd(
            "open class Shape {\n  open fn area(): f64 => 0.0\n}\n"
            "class Disk : Shape {\n  override fn area(): f64 => 1.0\n}\nfn main() {}\n", findTarget("csharp"));
        check(baseOverride.ok, "issue#29: 'override' of an open base-class member stays valid");
        EmitResult noBase = compileStd("class Lone {\n  override fn area(): f64 => 1.0\n}\nfn main() {}\n",
                                       findTarget("csharp"));
        check(!noBase.ok, "issue#29: 'override' with no base-class member is a diagnostic");
        // Interface bodies are bodiless, non-static signatures only (previously accepted + silently dropped).
        EmitResult bodied = compileStd("interface I {\n  fn f(): i32 => 1\n}\nfn main() {}\n", findTarget("csharp"));
        check(!bodied.ok, "issue#29: a bodied interface method is a diagnostic");
        EmitResult field = compileStd("interface I {\n  let x: i32\n}\nfn main() {}\n", findTarget("csharp"));
        check(!field.ok, "issue#29: a field in an interface is a diagnostic");
        // Python: the interface exists as an abc.ABC class, so the implements clause resolves at import.
        EmitResult py = compileStd(iface +
            "class Net : IPgNet {\n  fn forward(x: i32): i32 => x + 1\n}\nfn main() {}\n", findTarget("python"));
        check(py.ok && has(py.code, "import abc") && has(py.code, "class IPgNet(abc.ABC):") &&
                  has(py.code, "@abc.abstractmethod") && has(py.code, "class Net(IPgNet):"),
              "issue#29: Python emits the interface as an abc.ABC base");
        // #38 (issue #29 follow-up): a typed match binding IS now a runtime type test on a concrete class —
        // it compiles and emits a C# declaration pattern (`Disk d`), not a fake first-arm-always-wins.
        EmitResult typedPat = compileStd(
            "open class Shape {\n  open fn kind(): i32 => 0\n}\n"
            "class Disk : Shape {\n  override fn kind(): i32 => 1\n}\n"
            "fn f(s: Shape): i32 => match s { d: Disk => 1, _ => 0 }\nfn main() {}\n", findTarget("csharp"));
        check(typedPat.ok && has(typedPat.code, "Disk d"),
              "issue#29/#38: a typed 'match' binding is a concrete-class type test (C# declaration pattern)");
    }

    // ---- Wave-2 front-end robustness (G42-G48) + slice-1/2 compiler fixes -----------------------------
    {
        // G42: a >2^63 enum value is a diagnostic, not a std::stoll process abort.
        EmitResult hugeEnum = compileStd("enum E { A = 99999999999999999999 }\nfn main() {}\n", findTarget("csharp"));
        check(!hugeEnum.ok, "G42: out-of-range enum value is rejected, not a crash");

        // G43: a UTF-8 BOM'd source compiles clean.
        EmitResult bom = compileStd("\xEF\xBB\xBF" "fn main() {\n  print(1)\n}\n", findTarget("csharp"));
        check(bom.ok, "G43: BOM-prefixed source compiles");

        // G44: an unterminated block comment is diagnosed instead of silently swallowing the file tail.
        EmitResult untermC = compileStd("fn main() {\n  print(1)\n}\n/* dangling", findTarget("csharp"));
        check(!untermC.ok, "G44: unterminated /* is a diagnostic");
        bool namesComment = false;
        for (const auto& dg : untermC.diagnostics) if (has(dg.message, "unterminated block comment")) namesComment = true;
        check(namesComment, "G44: the diagnostic names the unterminated comment");
        EmitResult eofSlash = compileStd("fn main() {\n  print(1)\n}\n// trailing comment at EOF", findTarget("csharp"));
        check(eofSlash.ok, "G44: '//' at EOF is fine");

        // G45 parser guard: 300 nested parens -> one clean "nests too deeply" error, no stack overflow.
        {
            std::string deep = "fn main() {\n  let x = ";
            for (int i = 0; i < 300; ++i) deep += "(";
            deep += "1";
            for (int i = 0; i < 300; ++i) deep += ")";
            deep += "\n  print(x)\n}\n";
            EmitResult r = compile(deep, findTarget("csharp"));
            bool named = false;
            for (const auto& dg : r.diagnostics) if (has(dg.message, "nests too deeply")) named = true;
            check(!r.ok && named, "G45: 300-deep parens -> 'nests too deeply' diagnostic");
        }
        // G45 emitter guard: a 600-term chain parses ITERATIVELY (no parser guard) but builds a
        // 600-deep IR spine — the emitter's depth guard throws; it must surface as an error, not UB.
        {
            std::string chain = "fn main() {\n  let a = 1\n  print(a";
            for (int i = 0; i < 600; ++i) chain += " + a";
            chain += ")\n}\n";
            bool guarded = false;
            try {
                EmitResult r = compile(chain, findTarget("python"));
                guarded = !r.ok; // if the engine converts the guard to a failed emit, that's fine too
            } catch (const std::exception& e) {
                guarded = has(e.what(), "too deeply nested");
            }
            check(guarded, "G45: 600-term chain -> emitter depth-guard error (caught, not a crash)");
        }

        // G46: the Activator refusal row (§3.B runtime reflection) fires.
        EmitResult act = compileStd("fn main() {\n  let a = Activator\n}\n", findTarget("csharp"));
        check(!act.ok, "G46: 'Activator' is refused (§3.B reflection)");

        // G48: a non-ASCII identifier is ONE positional diagnostic, not per-byte spam.
        {
            EmitResult r = compileStd("fn main() {\n  let caf\xC3\xA9 = 1\n}\n", findTarget("csharp"));
            int nonAsciiDiags = 0;
            for (const auto& dg : r.diagnostics) if (has(dg.message, "non-ASCII")) ++nonAsciiDiags;
            check(!r.ok && nonAsciiDiags == 1, "G48: non-ASCII identifier -> exactly one diagnostic");
        }
        // G47: non-ASCII STRING content compiles (published PHP byte-string caveat -> warning only).
        {
            EmitResult r = compileStd("fn main() {\n  print(\"caf\xC3\xA9\")\n}\n", findTarget("csharp"));
            check(r.ok, "G47: non-ASCII string literal still compiles (warns, not errors)");
        }

        // G11: a never-assigned class field is refused (uninitialized reads differ per target).
        EmitResult uninit = compileStd(
            "class C {\n  var x: i32\n  fn get(): i32 => this.x\n}\nfn main() { print(C().get()) }\n",
            findTarget("csharp"));
        check(!uninit.ok, "G11: never-initialized class field is refused");
        EmitResult initOk = compileStd(
            "class C {\n  var x: i32\n  constructor(x: i32) { this.x = x }\n  fn get(): i32 => this.x\n}\nfn main() { print(C(3).get()) }\n",
            findTarget("csharp"));
        check(initOk.ok, "G11: field assigned in init passes");

        // P40 review A9: definite assignment is per PATH through init, not "somewhere in init".
        {
            auto cls = [](const std::string& ctorBody) {
                return "class C {\n  let x: i32\n  constructor(f: bool) {\n" + ctorBody +
                       "  }\n}\nfn main() { print(C(true).x) }\n";
            };
            auto refusedWith = [&](const std::string& body, const std::string& frag) {
                EmitResult r = compileStd(cls(body), findTarget("csharp"));
                return !r.ok && !r.diagnostics.empty() && has(r.diagnostics[0].message, frag);
            };
            auto okWith = [&](const std::string& body) { return compileStd(cls(body), findTarget("csharp")).ok; };
            check(refusedWith("    if f { this.x = 1 }\n", "not assigned on every path through init"),
                  "P40 A9: a field assigned on one if-branch only is refused");
            check(refusedWith("    if f { return }\n    this.x = 1\n", "returns before field 'x' is assigned"),
                  "P40 A9: an early return before the assignment is refused (at the return)");
            check(refusedWith("    while f { this.x = 1 }\n", "not assigned on every path through init"),
                  "P40 A9: an assignment only inside a loop body is not definite");
            check(okWith("    if f { this.x = 1 } else { this.x = 2 }\n"),
                  "P40 A9: a field assigned on both branches passes");
            check(okWith("    if f { throw Error(\"no\") }\n    this.x = 1\n"),
                  "P40 A9: a throwing branch needs no assignment");
            check(okWith("    this.x = 1\n    if f { return }\n"),
                  "P40 A9: an early return after the assignment passes");
            check(okWith("    try { this.x = 1 } finally { print(0) }\n"),
                  "P40 A9: an assignment completing a try body passes");
        }

        // P40 review A9/B11: missing returns, unreachable code, self-comparison — one flow pass (flow.cpp).
        {
            auto refused = [](const std::string& src, const std::string& frag) {
                EmitResult r = compileStd(src, findTarget("csharp"));
                return !r.ok && !r.diagnostics.empty() && has(r.diagnostics[0].message, frag);
            };
            auto ok = [](const std::string& src) { return compileStd(src, findTarget("csharp")).ok; };
            check(refused("fn f(x: i32): i32 {\n  if x > 0 { return 1 }\n}\nfn main() { print(f(1)) }\n",
                          "can reach the end of its body without returning a value"),
                  "P40 A9: a value-returning body with a reachable end is refused");
            check(ok("fn f(x: i32): i32 {\n  if x > 0 { return 1 } else { return 2 }\n}\nfn main() { print(f(1)) }\n"),
                  "P40 A9: if/else that both return needs no trailing return");
            check(ok("fn f(x: i32): i32 {\n  while true {\n    if x > 0 { return x }\n  }\n}\nfn main() { print(f(1)) }\n"),
                  "P40 A9: `while true` without break never completes (no trailing return)");
            check(refused("fn f(x: i32): i32 {\n  while true {\n    if x > 0 { break }\n  }\n}\nfn main() { print(f(1)) }\n",
                          "without returning a value"),
                  "P40 A9: a `break` out of `while true` makes its end reachable");
            check(ok("fn f(x: i32): string {\n  try {\n    if x > 0 { throw Error(\"a\") }\n    return \"n\"\n"
                     "  } catch (e: Error) when (x > 5) {\n    return \"big\"\n  } catch (e: Error) {\n    return \"c\"\n  }\n}\n"
                     "fn main() { print(f(1)) }\n"),
                  "P40 A9: a try whose body and catches all return needs no trailing return");
            check(refused("fn f(): i32 {\n  throw Error(\"x\")\n  return 1\n}\nfn main() { print(f()) }\n", "unreachable code"),
                  "P40 B11: a statement after `throw` is refused as unreachable");
            check(refused("fn f(x: i32) {\n  for i in 0..x {\n    continue\n    print(i)\n  }\n}\nfn main() { f(1) }\n",
                          "unreachable code"),
                  "P40 B11: a statement after `continue` is refused as unreachable");
            check(refused("fn f(x: f64): bool => x != x\nfn main() { print(f(1.0)) }\n", "compares a value with itself"),
                  "P40 B11: `x != x` is refused (Math.isNaN is the NaN test)");
            check(refused("enum E { A, B }\nfn main() { print(E.A == E.A) }\n", "compares a value with itself"),
                  "P40 B11: a constant enum self-comparison is refused");
            check(ok("import { Math } from \"std.math\"\nfn main() { print(Math.isNaN(0.0 / 0.0)) }\n"),
                  "P40 B11: std.math gains Math.isNaN");
            // TS: a guarded catch chain whose arms all diverge ends in an unconditional rethrow (tsc TS2366).
            EmitResult ts = compileStd("fn f(x: i32): string {\n  try {\n    return \"n\"\n  } catch (e: Error) when (x > 5) {\n"
                                       "    return \"big\"\n  }\n}\nfn main() { print(f(1)) }\n", findTarget("typescript"));
            check(ts.ok && has(ts.code, "        throw __e;\n") && !has(ts.code, "if (!__handled) { throw __e; }"),
                  "P40 A9: TS guarded catches that all diverge end in `throw __e;`");
        }

        // P40 review A9: a custom eq comes with an author hash; C# Equals(T?) + GetHashCode, Python __hash__.
        {
            const char* money = "record Money(cents: i32) {\n  operator fn eq(o: Money): bool => this.cents % 100 == o.cents % 100\n"
                                "  operator fn hash(): i32 => this.cents % 100\n}\n"
                                "fn main() { print(Money(1) == Money(101)) }\n";
            EmitResult cs = compileStd(money, findTarget("csharp"));
            check(cs.ok && has(cs.code, "public virtual bool Equals(Money? o) => o is not null && (") &&
                      has(cs.code, "public override int GetHashCode() => this.cents % 100;"),
                  "P40 A9: C# record eq takes Money? and the author hash is GetHashCode");
            EmitResult py = compileStd(money, findTarget("python"));
            check(py.ok && has(py.code, "def __hash__(self)"), "P40 A9: Python emits the author hash as __hash__");
            EmitResult ts = compileStd(money, findTarget("typescript"));
            check(ts.ok && has(ts.code, "hash(): number {") && has(ts.code, "this.cents"),
                  "P40 A9: TS hash is an instance method over `this`");
            const char* cls = "class B {\n  var v: i32\n  constructor(v: i32) { this.v = v }\n"
                              "  operator fn eq(o: B): bool => this.v == o.v\n  operator fn hash(): i32 => this.v\n}\n"
                              "fn main() { print(B(1) == B(1)) }\n";
            EmitResult csc = compileStd(cls, findTarget("csharp"));
            check(csc.ok && has(csc.code, "operator ==(B? a, B? b)") && has(csc.code, "operator !=(B? a, B? b)") &&
                      !has(csc.code, "GetHashCode() => 0"),
                  "P40 A9: C# class == / != take nullable operands; no constant hash");
            EmitResult noHash = compileStd("record M(c: i32) {\n  operator fn eq(o: M): bool => this.c == o.c\n}\nfn main() { }\n",
                                           findTarget("csharp"));
            check(!noHash.ok && has(noHash.diagnostics[0].message, "operator fn hash(): i32"),
                  "P40 A9: a custom eq without a hash is refused");
            EmitResult noEq = compileStd("record M(c: i32) {\n  operator fn hash(): i32 => this.c\n}\nfn main() { }\n",
                                         findTarget("csharp"));
            check(!noEq.ok && has(noEq.diagnostics[0].message, "needs a custom"), "P40 A9: a hash without an eq is refused");
            EmitResult badOp = compileStd("record M(c: i32) {\n  operator fn frob(o: M): M => M(1)\n}\nfn main() { }\n",
                                          findTarget("csharp"));
            check(!badOp.ok && has(badOp.diagnostics[0].message, "is not an operator"),
                  "P40 A9: an unknown operator name is refused (it emitted `operator frob`)");
        }

        // P40 review A9: an unused catch binding (or `_`) is dropped — C# `catch (T)`, no CS0168.
        {
            const char* src = "class Boom : Error {\n  constructor() { super(\"b\") }\n}\n"
                              "fn main() {\n  try { throw Boom() } catch (e: Boom) { print(1) }\n"
                              "  try { throw Boom() } catch (_: Boom) { print(2) }\n"
                              "  try { throw Boom() } catch (e: Boom) { print(e.message) }\n}\n";
            EmitResult cs = compileStd(src, findTarget("csharp"));
            check(cs.ok && has(cs.code, "catch (Boom)\n") && !has(cs.code, "catch (Boom _)") && has(cs.code, "catch (Boom e)"),
                  "P40 A9: unused catch bindings are dropped, a used one kept");
        }

        // Slice-1 fix pins: PHP strict equality + record equals routing.
        EmitResult phpEq = compileStd("fn same(a: string, b: string): bool => a == b\nfn main() { print(same(\"a\", \"b\")) }\n", findTarget("php"));
        check(phpEq.ok && has(phpEq.code, "==="), "G1: PHP string == emits strict ===");
        EmitResult phpRec = compileStd("record P(x: i32)\nfn main() { print(P(1) == P(1)) }\n", findTarget("php"));
        check(phpRec.ok && has(phpRec.code, "->equals(") && has(phpRec.code, "public function equals($other)"),
              "G1/G10: PHP record == routes to a generated equals()");
        // Comparing to null stays a null TEST, never equals() (would call a method on null).
        EmitResult tsNull = compileStd("record P(x: i32)\nfn f(p: P?): i32 => if p != null { 1 } else { 0 }\nfn main() { print(f(null)) }\n", findTarget("typescript"));
        check(tsNull.ok && !has(tsNull.code, ".equals(null)"), "record != null does not route to equals()");

        // Slice-2 fix pins: i64 literal adoption + declared-type-differs locals.
        EmitResult tsLit = compileStd("fn main() {\n  let a: i64 = 9007199254740993\n  print(a)\n}\n", findTarget("typescript"));
        check(tsLit.ok && has(tsLit.code, "9007199254740993n") && !has(tsLit.code, "BigInt(9007199254740993)"),
              "G3: annotation-typed i64 literal emits the exact BigInt literal");
        EmitResult csNullable = compileStd("fn main() {\n  var n: i32? = 0\n  n = null\n  print(if n == null { 1 } else { 0 })\n}\n", findTarget("csharp"));
        check(csNullable.ok && has(csNullable.code, "int? n = 0"), "declared i32? with non-null init keeps the nullable slot (C#)");

        // Slice-2 shift rule: the count is i32; shiftee type is preserved (C# has no long<<long).
        EmitResult csShift = compileStd("fn main() {\n  let b: i64 = 1\n  print(b << 65)\n}\n", findTarget("csharp"));
        check(csShift.ok && !has(csShift.code, "(long)(65)"), "G4: shift count stays i32 on C#");
    }

    // ---------------------------------------------------------------------------------------------
    // P39 / issue #71 — coverage attribution.
    {
        using namespace mintplayer::polyglot::coverage;

        // --- the source-map read side -------------------------------------------------------------
        {
            bool roundTrips = true;
            for (int v : {0, 1, -1, 15, 16, -16, 1023, -1024, 123456, -123456}) {
                std::string enc = vlqEncode(v);
                std::size_t pos = 0;
                int back = 0;
                if (!vlqDecode(enc, pos, back) || back != v || pos != enc.size()) roundTrips = false;
            }
            check(roundTrips, "P39: vlqDecode round-trips vlqEncode");

            std::size_t pos = 0;
            int dummy = 0;
            check(!vlqDecode("$", pos, dummy), "P39: vlqDecode refuses a non-Base64 character");
            pos = 0;
            check(!vlqDecode("g", pos, dummy), "P39: vlqDecode refuses a truncated continuation run");

            DecodedSourceMap decoded;
            std::string err;
            // A map that is not v3 REFUSES rather than yielding an empty map, which would silently
            // become "nothing was covered".
            check(!parseSourceMapV3("{\"version\":2,\"mappings\":\"\"}", decoded, err),
                  "P39: parseSourceMapV3 refuses a non-3 version");
            check(!parseSourceMapV3("{\"version\":3}", decoded, err),
                  "P39: parseSourceMapV3 refuses a map with no mappings");
            check(!parseSourceMapV3("{\"version\":3,\"sources\":[\"a.pg\"],\"mappings\":\"A$\"}", decoded, err),
                  "P39: parseSourceMapV3 refuses a malformed VLQ run");

            // Two segments on ONE generated line: both are kept. First-wins is the interim tool's
            // under-reporting bug (PRD §4.6).
            const std::string twoSegs =
                "{\"version\":3,\"file\":\"o.ts\",\"sources\":[\"a.pg\",\"b.pg\"],"
                "\"sourcesContent\":[null,null],\"mappings\":\"" + vlqEncode(0) + vlqEncode(0) +
                vlqEncode(0) + vlqEncode(0) + "," + vlqEncode(4) + vlqEncode(1) + vlqEncode(2) +
                vlqEncode(0) + "\"}";
            check(parseSourceMapV3(twoSegs, decoded, err) && decoded.segments.size() == 2 &&
                      decoded.segments[0].sourceIndex == 0 && decoded.segments[0].sourceLine == 1 &&
                      decoded.segments[1].sourceIndex == 1 && decoded.segments[1].sourceLine == 3,
                  "P39: every segment on a generated line is decoded, not just the first");
        }

        // --- format sniffing ----------------------------------------------------------------------
        check(sniffFormat("TN:\nSF:a.ts\n") == Format::Lcov, "P39: sniffs lcov");
        check(sniffFormat("{\"a.ts\":{\"statementMap\":{}}}") == Format::Istanbul, "P39: sniffs istanbul");
        check(sniffFormat("<?xml version=\"1.0\"?>\n<coverage><packages><package><classes>"
                          "<class filename=\"a.ts\"/></classes></package></packages></coverage>") ==
                  Format::Cobertura,
              "P39: sniffs cobertura");
        // Cobertura and clover SHARE a <coverage> root — the discriminator must be structural, or a
        // clover file parses as an empty cobertura and is rejected as "no files".
        check(sniffFormat("<coverage generated=\"0\" clover=\"3.2.0\"><project><file name=\"a.php\"/>"
                          "</project></coverage>") == Format::Clover,
              "P39: clover is not mistaken for cobertura");

        // --- readers ------------------------------------------------------------------------------
        {
            Report r;
            std::string err;
            check(parseReport("TN:\nSF:a.ts\nDA:3,7\nDA:3,2\nBRDA:5,0,0,4\nBRDA:5,0,1,-\nend_of_record\n",
                              Format::Lcov, r, err) &&
                      r.files.size() == 1 && r.files[0].lines[3].hits == 7,
                  "P39: lcov duplicate DA records merge with max, not sum");
            check(r.files[0].lines[5].arms.size() == 2 && r.files[0].lines[5].arms[0].takenKnown &&
                      !r.files[0].lines[5].arms[1].takenKnown,
                  "P39: lcov BRDA '-' stays distinct from 0");

            // Several <class> elements legitimately share one filename (coverlet emits one per type AND
            // per closed generic instantiation); a <methods> section repeats the same lines.
            Report cob;
            check(parseReport("<coverage><packages><package><classes>"
                              "<class filename=\"a.ts\"><methods><method><lines>"
                              "<line number=\"1\" hits=\"99\"/></lines></method></methods>"
                              "<lines><line number=\"1\" hits=\"2\"/></lines></class>"
                              "<class filename=\"a.ts\"><lines>"
                              "<line number=\"1\" hits=\"5\"/>"
                              "<line number=\"2\" hits=\"0\" branch=\"true\" "
                              "condition-coverage=\"50% (1/2)\"/></lines></class>"
                              "</classes></package></packages></coverage>",
                              Format::Cobertura, cob, err) &&
                      cob.files.size() == 1 && cob.files[0].lines[1].hits == 5,
                  "P39: cobertura merges <class> elements sharing a filename, ignoring <methods>");
            check(cob.files[0].lines[2].countCovered == 1 && cob.files[0].lines[2].countTotal == 2,
                  "P39: cobertura condition-coverage reads as a count, with no arm identity");

            // truecount/falsecount are the numbers of TAKEN and UNTAKEN arms on the line, aggregated over
            // every branch on it — NOT a true-arm/false-arm pair. Reading them as two arms would flip
            // every partial line.
            Report clo;
            check(parseReport("<coverage clover=\"3.2.0\"><project><file name=\"a.php\">"
                              "<line num=\"9\" type=\"cond\" truecount=\"4\" falsecount=\"0\"/>"
                              "</file></project></coverage>",
                              Format::Clover, clo, err) &&
                      clo.files[0].lines[9].countCovered == 4 && clo.files[0].lines[9].countTotal == 4 &&
                      clo.files[0].lines[9].arms.empty(),
                  "P39: clover is count-only (truecount=4 is four taken arms, not a true/false pair)");

            // Arms attribute to the BRANCH's line, not to each arm's own start line: per-arm attribution
            // renders every multi-line ternary as two partial lines.
            Report ist;
            check(parseReport("{\"a.ts\":{\"path\":\"a.ts\","
                              "\"statementMap\":{\"0\":{\"start\":{\"line\":4},\"end\":{\"line\":4}}},"
                              "\"s\":{\"0\":3},"
                              "\"branchMap\":{\"0\":{\"line\":7,\"locations\":["
                              "{\"start\":{\"line\":7}},{\"start\":{\"line\":9}}]}},"
                              "\"b\":{\"0\":[1,0]}}}",
                              Format::Istanbul, ist, err) &&
                      ist.files[0].lines[4].hits == 3 && ist.files[0].lines[7].arms.size() == 2 &&
                      ist.files[0].lines.count(9) == 0,
                  "P39: istanbul arms attribute to the branch line, not to each arm's start line");

            Report bad;
            check(!parseReport("not a report", Format::Lcov, bad, err),
                  "P39: a reader refuses rather than returning an empty report");
        }

        // --- merge semantics ----------------------------------------------------------------------
        {
            Line a, b;
            a.hits = 3; a.hitsKnown = true;
            b.hits = 5; b.hitsKnown = true;
            mergeLine(a, b);
            check(a.hits == 5, "P39: line hits merge with MAX (a sum would read as an execution count)");

            Line unknown, known;
            unknown.hitsKnown = false;          // "executed, count unknown" — all JaCoCo can say
            known.hits = 0; known.hitsKnown = true;
            mergeLine(unknown, known);
            check(!unknown.hitsKnown || unknown.hits == 0,
                  "P39: unknown hits are not silently demoted to 0 by a merge");

            // §4.3: two generated 2-arm branches at 1/2 each onto ONE .pg line give 2/4, not 1/2. Within
            // one report the keys come from one instrumenter, so union is correct.
            Line l1, l2;
            l1.arms.push_back(BranchArm{"0:0", 1, true});
            l1.arms.push_back(BranchArm{"0:1", 0, true});
            l2.arms.push_back(BranchArm{"1:0", 1, true});
            l2.arms.push_back(BranchArm{"1:1", 0, true});
            mergeLine(l1, l2);
            int covered = 0;
            for (const auto& arm : l1.arms) if (arm.taken > 0) ++covered;
            check(l1.arms.size() == 4 && covered == 2,
                  "P39: branch arms UNION across generated lines - 2/4, not 1/2");

            Line c1, c2;
            c1.countCovered = 1; c1.countTotal = 2;
            c2.countCovered = 1; c2.countTotal = 2;
            mergeLine(c1, c2);
            check(c1.countCovered == 2 && c1.countTotal == 4,
                  "P39: count-only pairs sum, matching what union does on distinct keys");
        }

        // --- projection ---------------------------------------------------------------------------
        {
            OriginIndex index;
            // Generated lines 10 and 11 both come from .pg line 7; .pg line 9 is mappable but never
            // reached; b.pg is mappable and never appears in the report at all.
            index.byGenerated["out/a.ts"][10] = {{"src/a.pg", 7}};
            index.byGenerated["out/a.ts"][11] = {{"src/a.pg", 7}};
            index.mappedLines["src/a.pg"] = {7, 9};
            index.mappedLines["src/b.pg"] = {1, 2};

            Report gen;
            {
                File& f = gen.fileFor("out/a.ts");
                Line l10; l10.hits = 3; l10.hitsKnown = true;
                l10.arms.push_back(BranchArm{"0:0", 1, true});
                l10.arms.push_back(BranchArm{"0:1", 0, true});
                Line l11; l11.hits = 8; l11.hitsKnown = true;
                l11.arms.push_back(BranchArm{"1:0", 0, true});
                l11.arms.push_back(BranchArm{"1:1", 1, true});
                Line l99; l99.hits = 4; l99.hitsKnown = true; // no origin: leaves the report
                f.lines[10] = l10;
                f.lines[11] = l11;
                f.lines[99] = l99;
            }

            ProjectionStats stats;
            Report pg = projectReport(gen, index, stats);
            const File* a = nullptr;
            const File* b = nullptr;
            for (const auto& f : pg.files) {
                if (f.path == "src/a.pg") a = &f;
                if (f.path == "src/b.pg") b = &f;
            }
            check(a && a->lines.count(7) && a->lines.at(7).hits == 8,
                  "P39: many generated lines onto one .pg line take the MAX hit count");
            check(a && a->lines.at(7).arms.size() == 4,
                  "P39: projection unions the arms of every contributing generated line");
            check(a && a->lines.size() == 2 && a->lines.count(9) && a->lines.at(9).hits == 0,
                  "P39: the denominator is the MAPPED set - an unreached mapped line reports at zero");
            check(a && !a->lines.count(99), "P39: a generated line with no origin leaves the report");
            // D7: a module no test touched is emitted at zero rather than vanishing, which would inflate
            // the percentage precisely when a module is least tested.
            check(b && b->lines.size() == 2 && b->lines.at(1).hits == 0 && b->lines.at(2).hits == 0,
                  "P39: a .pg file absent from the report is emitted at zero, not dropped");
            check(stats.pgFilesWithoutReportData == 1 && stats.pgLinesMappable == 4 &&
                      stats.pgLinesCovered == 1,
                  "P39: projection stats count the mapped set, not the report");

            // Either direction of suffix match resolves; an ambiguous basename resolves to nothing.
            check(matchGeneratedPath("/abs/repo/out/a.ts", index.byGenerated) == "out/a.ts",
                  "P39: a report path longer than the known path still matches");
            check(matchGeneratedPath("a.ts", index.byGenerated) == "out/a.ts",
                  "P39: a report path shorter than the known path still matches");
            OriginIndex ambiguous;
            ambiguous.byGenerated["x/a.ts"][1] = {{"x.pg", 1}};
            ambiguous.byGenerated["y/a.ts"][1] = {{"y.pg", 1}};
            check(matchGeneratedPath("a.ts", ambiguous.byGenerated).empty(),
                  "P39: an ambiguous basename resolves to nothing rather than to the wrong file");
        }

        // --- writers, and the count-only emission rule ---------------------------------------------
        {
            Report r;
            File& f = r.fileFor("src/a.pg");
            Line l;
            l.hits = 2;
            l.hitsKnown = true;
            l.arms.push_back(BranchArm{"0:0", 1, true});
            l.arms.push_back(BranchArm{"0:1", 0, true});
            f.lines[7] = l;

            // §4.4a: branch data is emitted COUNT-ONLY. lcov's BRDA is arm-KEYED, and our keys are not
            // comparable with another target's for the same .pg line, so a union would OVER-credit
            // whenever both suites cover the same arm.
            const std::string lcovOut = writeReport(r, Format::Lcov);
            check(has(lcovOut, "SF:src/a.pg") && has(lcovOut, "DA:7,2") && !has(lcovOut, "BRDA:"),
                  "P39: lcov output carries no arm records by default (count-only emission)");
            WriteOptions arms;
            arms.branchArms = true;
            check(has(writeReport(r, Format::Lcov, arms), "BRDA:7,0,0,1"),
                  "P39: --branch-arms opts back into arm-keyed lcov output");

            const std::string cobOut = writeReport(r, Format::Cobertura);
            check(has(cobOut, "filename=\"src/a.pg\"") && has(cobOut, "condition-coverage=\"50% (1/2)\""),
                  "P39: cobertura expresses the branch pair as a count");
            check(has(cobOut, "hits=\"2\""),
                  "P39: cobertura always writes hits (a missing hits attribute reads as NOT covered)");

            const std::string cloOut = writeReport(r, Format::Clover);
            check(has(cloOut, "truecount=\"1\"") && has(cloOut, "falsecount=\"1\""),
                  "P39: clover expresses the branch pair as taken/untaken counts");

            // Read-back stability: what we write, we can read.
            Report back;
            std::string err;
            check(parseReport(cobOut, Format::Cobertura, back, err) && back.files.size() == 1 &&
                      back.files[0].path == "src/a.pg" && back.files[0].lines[7].hits == 2 &&
                      back.files[0].lines[7].countCovered == 1 && back.files[0].lines[7].countTotal == 2,
                  "P39: cobertura round-trips through its own reader");
            Report backLcov;
            check(parseReport(lcovOut, Format::Lcov, backLcov, err) &&
                      backLcov.files[0].lines[7].hits == 2,
                  "P39: lcov round-trips through its own reader");
            Report backIst;
            const std::string istOut = writeReport(r, Format::Istanbul);
            check(parseReport(istOut, Format::Istanbul, backIst, err) &&
                      backIst.files[0].lines[7].hits == 2,
                  "P39: istanbul round-trips through its own reader");
            Report backClo;
            check(parseReport(cloOut, Format::Clover, backClo, err) &&
                      backClo.files[0].lines[7].countTotal == 2,
                  "P39: clover round-trips through its own reader");
        }

        // --- the manifests slice 1 added ----------------------------------------------------------
        {
            BackendHandle py = findTarget("python");
            BackendHandle php = findTarget("php");
            check(py.ok() && py.backend()->originMapping().emitsSourceMap(),
                  "P39: python declares a sourceMapV3 origin sink");
            check(php.ok() && php.backend()->originMapping().emitsSourceMap(),
                  "P39: php declares a sourceMapV3 origin sink");
            // The Core must never compare target names: prelude placement is manifest data.
            BackendHandle cs = findTarget("csharp");
            check(cs.ok() && cs.backend()->sharedPreludeFile() && !cs.backend()->preludePerFile(),
                  "P39: csharp declares sharedPreludeFile, not preludePerFile");
            check(py.ok() && py.backend()->preludePerFile() && !py.backend()->sharedPreludeFile(),
                  "P39: python declares preludePerFile");
        }
    }

    // ================= P40 slices 10-12: generated header, outputs record, config walk, relative origins =================
    {
        namespace fs = std::filesystem;
        // --- B5: every in-box plugin emits BOTH header lines, within the first 5 lines (the ownership scan's
        // window), in entry, module and prelude files — with and without origin info (C# interleaves #line).
        auto headerOk = [](const std::string& code) {
            std::istringstream is(code);
            std::string ln;
            bool autoGen = false, ours = false;
            for (int i = 0; i < 5 && std::getline(is, ln); ++i) {
                autoGen = autoGen || has(ln, "<auto-generated/>");
                ours = ours || has(ln, "Generated by Polyglot. Do not edit: regenerate with `polyglot build`.");
            }
            return autoGen && ours;
        };
        for (const char* t : {"csharp", "typescript", "python", "php"}) {
            for (bool origins : {false, true}) {
                const BackendHandle h = findTarget(t);
                MapModuleResolver res({{"./util", "fn area(w: i32, h: i32): i32 { return w * h }\n"}});
                LibConfig lib{{"io"}};
                lib.sharedPrelude = true;
                lib.originInfo = origins;
                const EmitResult r = compile("import { area } from \"./util\"\n"
                                             "fn fa(): Option<i32> { return Some(area(2, 3)) }\n",
                                             h, &res, lib, "/w/main.pg");
                bool ok = r.ok && headerOk(r.code), sawModule = false, preludeOk = true;
                for (const auto& m : r.modules) {
                    if (m.sharedPrelude) { preludeOk = preludeOk && (m.code.empty() || headerOk(m.code)); continue; }
                    sawModule = true;
                    ok = ok && headerOk(m.code);
                }
                if (h.backend()->sharedPreludeFile()) { // the phase-2 prelude file (P40 §0) is a Program too
                    const EmitResult p = compileSharedPrelude(h, r.preludeReach, lib);
                    preludeOk = preludeOk && p.ok && !p.code.empty() && headerOk(p.code);
                }
                check(ok && sawModule && preludeOk, std::string("P40 B5: ") + t + " emits both header lines in entry, module and prelude files" +
                                                        (origins ? " (origin info on)" : ""));
            }
        }
        // PHP: after `<?php` (which must stay the first line).
        {
            const EmitResult r = compile("fn fa(): i32 { return 1 }\n", findTarget("php"));
            check(r.ok && r.code.rfind("<?php\n// <auto-generated/>\n// Generated by Polyglot.", 0) == 0,
                  "P40 B5: PHP's header follows <?php");
        }

        // --- B7: origin paths are relative to the generated file's directory ---
        {
            LibConfig lib;
            lib.originInfo = true;
            lib.originBaseDir = [](const std::string&) { return std::string("/w/gen/cs"); };
            const EmitResult r = compile("fn fa(): i32 { return 1 }\n", findTarget("csharp"), nullptr, lib, "/w/src/main.pg");
            check(r.ok && has(r.code, "#line 1 \"../../src/main.pg\"") && !has(r.code, "\"/w/src/main.pg\""),
                  "P40 B7: a #line path is relative to the generated file's directory");
            lib.originBaseDir = {};
            const EmitResult a = compile("fn fa(): i32 { return 1 }\n", findTarget("csharp"), nullptr, lib, "/w/src/main.pg");
            check(a.ok && has(a.code, "#line 1 \"/w/src/main.pg\""), "P40 B7: no base dir = the absolute path (unchanged)");
#ifdef _WIN32
            lib.originBaseDir = [](const std::string&) { return std::string("D:/out"); };
            const EmitResult x = compile("fn fa(): i32 { return 1 }\n", findTarget("csharp"), nullptr, lib, "C:/w/main.pg");
            check(x.ok && has(x.code, "#line 1 \"C:/w/main.pg\""), "P40 B7: a different drive falls back to the absolute path");
#endif
        }

        // --- A5 + the record: real files in a scratch directory ---
        const fs::path tmp = fs::temp_directory_path() / ("polyglot-p40-unit-" + std::to_string(std::rand()));
        std::error_code ec;
        fs::remove_all(tmp, ec);
        fs::create_directories(tmp / "proj" / "LibA", ec);
        cli::writeFile(tmp / "proj" / "pgconfig.json",
                       "{ \"targets\": [\"csharp\"], \"include\": [ { \"pattern\": \"*.pg\", \"output\": \"gen/%(Filename)\", \"owned\": true } ] }");
        {
            const fs::path old = fs::current_path();
            fs::current_path(tmp / "proj" / "LibA");
            const cli::PgConfig pc = cli::loadPgConfig(".");
            const cli::PgConfig pcEmpty = cli::loadPgConfig("");
            fs::current_path(old);
            check(pc.found && pc.dir.is_absolute() && fs::path(pc.root).is_absolute() &&
                      fs::equivalent(pc.dir, tmp / "proj") && pcEmpty.found,
                  "P40 A5: loadPgConfig walks up from a RELATIVE start ('.') and returns absolute dir/root");
            check(pc.include.size() == 1 && pc.include[0].owned, "P40 B5: an include rule's \"owned\": true is parsed");
            cli::writeFile(tmp / "bad" / "pgconfig.json", "{ \"include\": [ { \"pattern\": \"*.pg\", \"output\": \"x\", \"owned\": \"yes\" } ] }");
            check(!cli::loadPgConfig(tmp / "bad").errors.empty(), "P40 B5: a non-boolean \"owned\" is a config error");
        }
        {
            const fs::path cfg = tmp / "proj";
            fs::path dir;
            bool rec = false;
            std::string err;
            const bool a = cli::ownedScope(cfg, "web/gen/%(Filename)", dir, rec, err) && dir == cfg / "web" / "gen" && !rec;
            const bool b = cli::ownedScope(cfg, "web/gen/%(RecursiveDir)%(Filename)", dir, rec, err) && dir == cfg / "web" / "gen" && rec;
            const bool c = cli::ownedScope(cfg, "gen/%(TargetLanguage)/%(Filename)", dir, rec, err) && dir == cfg / "gen" && rec;
            const bool d = cli::ownedScope(cfg, "gen\\%(Filename)", dir, rec, err) && dir == cfg / "gen" && !rec;
            check(a && b && c && d, "P40 B5: the owned scope is the template's literal folder prefix (recursive when dirs can follow; `\\` separates)");
            // P40 review (#75): the reviewer's table — each of these deleted headed files outside a dedicated folder.
            bool refusedAll = true;
            for (const std::string& t : std::vector<std::string>{"./%(Filename)", "%(RecursiveDir)%(Filename)", "%(Directory)/%(Filename)", "../%(Filename)",
                                        "../%(RecursiveDir)%(Filename)", "gen-%(TargetLanguage)/%(Filename)", "/gen/%(Filename)",
                                        "\\gen\\%(Filename)", "gen/../%(Filename)", "../../%(Filename)",
                                        (tmp / "abs").generic_string() + "/%(Filename)"}) {
                err.clear();
                const bool ok = cli::ownedScope(cfg, t, dir, rec, err);
                if (ok || err.empty()) { std::cerr << "    owned scope NOT refused: " << t << "\n"; refusedAll = false; }
            }
#ifdef _WIN32
            refusedAll = refusedAll && !cli::ownedScope(cfg, "C:/gen/%(Filename)", dir, rec, err) &&
                         !cli::ownedScope(cfg, "C:gen/%(Filename)", dir, rec, err);
#endif
            check(refusedAll, "P40 review: owned is refused for an empty prefix, the config folder or an ancestor, a root, and rooted/absolute templates");
            const bool sib = cli::ownedScope(cfg / "a" / "b", "../../web/scanning/src/gs1/%(Filename)", dir, rec, err) &&
                             dir == cfg / "web" / "scanning" / "src" / "gs1";
            check(sib, "P40 review: `..` into a SIBLING subtree stays allowed (Spark's twin route)");
            cli::writeFile(tmp / "bad2" / "pgconfig.json", "{ \"include\": [ { \"pattern\": \"*.pg\", \"output\": \"../%(Filename)\", \"owned\": true } ] }");
            const auto bad2 = cli::loadPgConfig(tmp / "bad2");
            check(bad2.errors.size() == 1 && has(bad2.errors[0], "\"owned\": true is refused") && has(bad2.errors[0], "ancestors"),
                  "P40 review: an unbounded owned scope is a config error naming the reason");
            // Routing and the owned scan share one normalization: a leading separator stays config-relative.
            cli::PgConfig rp;
            rp.found = true;
            rp.dir = cfg;
            cli::IncludeRule rr;
            rr.pattern = "*.pg";
            rr.output = "\\gen\\%(Filename)";
            rp.include.push_back(rr);
            std::string terr;
            const auto routed = cli::routeIncludeOutput(rp, cfg / "a.pg", "typescript", terr);
            check(routed && *routed == cfg / "gen" / "a", "P40 review: routing treats `\\` as a separator and a leading one stays config-relative");
        }
        {
            const std::string hdr = findTarget("typescript").backend()->generatedHeaderLine();
            const fs::path marked = tmp / "marked.ts", unmarked = tmp / "nswag.ts", mention = tmp / "notes.ts";
            const fs::path pyMarked = tmp / "marked.py";
            cli::writeFile(marked, "// <auto-generated/>\n// Generated by Polyglot. Do not edit: regenerate with `polyglot build`.\nexport {}\n");
            cli::writeFile(unmarked, "// <auto-generated/>\nexport interface C {}\n");
            cli::writeFile(mention, "// hand-written\n// this file is not Generated by Polyglot.\nexport const x = 1;\n");
            cli::writeFile(pyMarked, "# Generated by Polyglot. Do not edit: regenerate with `polyglot build`.\n");
            check(hdr == "// Generated by Polyglot." && findTarget("python").backend()->generatedHeaderLine() == "# Generated by Polyglot.",
                  "P40 review: the header line comes from each manifest's `lineComment`");
            check(cli::carriesGeneratedLine(marked, hdr) && !cli::carriesGeneratedLine(unmarked, hdr) &&
                      !cli::carriesGeneratedLine(tmp / "missing.ts", hdr) && !cli::carriesGeneratedLine(marked, ""),
                  "P40 D20: only Polyglot's own line marks a file as deletable (<auto-generated/> alone does not)");
            check(!cli::carriesGeneratedLine(mention, hdr), "P40 review: a line that merely MENTIONS the phrase is not the header line");
            check(cli::carriesGeneratedLine(pyMarked, findTarget("python").backend()->generatedHeaderLine()) &&
                      !cli::carriesGeneratedLine(pyMarked, hdr),
                  "P40 review: the header is matched in the target's own comment syntax");
            cli::writeFile(tmp / "o.ts.map", "{\"version\":3,\"file\":\"o.ts\",\"sources\":[]}");
            cli::writeFile(tmp / "victim.txt", "precious");
            check(cli::isOwnSidecar(tmp / "o.ts.map", tmp / "o.ts") && !cli::isOwnSidecar(tmp / "o.ts.map", tmp / "p.ts") &&
                      !cli::isOwnSidecar(tmp / "victim.txt", tmp / "o.ts"),
                  "P40 review: a sidecar is ours only as a v3 map naming its owner as `file`");
        }
        {
            // P40 review (#75, D21): the overwrite guard.
            const std::string hdr = findTarget("typescript").backend()->generatedHeaderLine();
            const fs::path g = tmp / "guard";
            cli::writeFile(g / "hand.ts", "// hand-written\nexport const keep = 1;\n");
            cli::writeFile(g / "ours.ts", "// <auto-generated/>\n// Generated by Polyglot. Do not edit: regenerate with `polyglot build`.\nold\n");
            cli::writeFile(g / "obj" / "old.cs", "// 0.10.x output, no header\n");
            cli::OutputSink::Claim claim{"'n.pg'", hdr, {}};
            cli::OutputSink w;
            const bool refused = w.put(g / "hand.ts", "new", claim) == cli::OutputSink::Put::Refused;
            std::string kept;
            cli::readFile(g / "hand.ts", kept);
            const bool ours = w.put(g / "ours.ts", "new", claim) == cli::OutputSink::Put::Written;
            check(refused && has(kept, "hand-written") && ours, "P40 review: put never replaces an unmarked, unrecorded file; a marked one is replaced");
            cli::OutputSink c(cli::OutputSink::Mode::Check);
            check(c.put(g / "hand.ts", "new", claim) == cli::OutputSink::Put::Refused && !c.drifted(),
                  "P40 review: --check reports an unmarked file as an error, not `would update`");
            cli::OutputSink owned;
            owned.trust({}, {g / "obj"}, false);
            cli::OutputSink recorded;
            recorded.trust({cli::outputKey(g / "hand.ts")}, {}, false);
            cli::OutputSink adopt;
            adopt.trust({}, {}, true);
            check(owned.put(g / "obj" / "old.cs", "new", claim) == cli::OutputSink::Put::Written &&
                      owned.put(g / "hand.ts", "new", claim) == cli::OutputSink::Put::Refused &&
                      recorded.put(g / "hand.ts", "new2", claim) == cli::OutputSink::Put::Written,
                  "P40 review: a --build-owned dir and a recorded path may be overwritten (only them)");
            cli::writeFile(g / "hand2.ts", "// hand-written\n");
            check(adopt.put(g / "hand2.ts", "x", claim) == cli::OutputSink::Put::Written, "P40 review: --adopt takes an unmarked output over");
            cli::writeFile(g / "m.ts.map", "not a map");
            cli::OutputSink sm;
            check(sm.put(g / "m.ts.map", "{}", cli::OutputSink::Claim{"'m.pg'", {}, g / "m.ts"}) == cli::OutputSink::Put::Refused,
                  "P40 review: a foreign file at a sidecar path is never overwritten");
        }
        {
            // P40 review: `lineComment` is load-validated against the header line the rules emit.
            std::string src;
            cli::readFile(cli::executablePath().parent_path() / "plugins" / "typescript" / "polyglot-plugin.json", src);
            auto replaced = [&](const std::string& from, const std::string& to) {
                std::string s = src;
                const std::size_t at = s.find(from);
                if (at != std::string::npos) s.replace(at, from.size(), to);
                return s;
            };
            std::string e1, e2, e3;
            const bool wrongToken = !validateBackend(replaced("\"lineComment\": \"//\"", "\"lineComment\": \"#\""), e1);
            const bool missing = !validateBackend(replaced("\"lineComment\": \"//\"", "\"lineCommentX\": \"//\""), e2);
            const bool fine = validateBackend(src, e3);
            check(!src.empty() && wrongToken && has(e1, "no rule emits the header line") && missing && has(e2, "declares no 'lineComment'") && fine,
                  "P40 review: a lineComment that matches no header line, or a header without lineComment, is a load error");
        }
        {
            cli::OutputSink check1(cli::OutputSink::Mode::Check);
            const fs::path f = tmp / "sink" / "a.cs";
            const auto p1 = check1.put(f, "x");
            check(p1 == cli::OutputSink::Put::Written && !fs::exists(f) && !fs::exists(f.parent_path()) && check1.drifted() &&
                      has(check1.driftLines()[0], "would create"),
                  "P40 B5: Check mode writes nothing (not even the directory) and records `would create`");
            cli::OutputSink w(cli::OutputSink::Mode::Write);
            const bool written = w.put(f, "x") == cli::OutputSink::Put::Written && fs::exists(f);
            const bool dedup = w.put(tmp / "sink" / "." / "a.cs", "x") == cli::OutputSink::Put::Deduped;
            const bool conflict = w.put(f, "y") == cli::OutputSink::Put::Conflict;
            check(written && dedup && conflict, "P40 B5: one run-wide produced set: identical = dedup, different = conflict");
            cli::OutputSink check2(cli::OutputSink::Mode::Check);
            check(check2.put(f, "x") == cli::OutputSink::Put::Unchanged && !check2.drifted() &&
                      check2.put(tmp / "sink" / "b.cs", "z") == cli::OutputSink::Put::Written,
                  "P40 B5: Check mode: unchanged is not drift");
        }
        {
            cli::OutputsRecord rec;
            const fs::path recDir = tmp / "proj" / ".polyglot";
            cli::RecordSection s;
            s.inputs.push_back(cli::recordRelative(tmp / "proj" / "a.pg", recDir));
            s.outputs.push_back({cli::recordRelative(tmp / "proj" / "gen" / "a.ts", recDir),
                                 cli::recordRelative(tmp / "proj" / "gen" / "a.ts.map", recDir)});
            rec.sections[cli::recordSectionKey(true, tmp / "proj", "typescript", recDir)] = s;
            const fs::path file = recDir / "outputs.json";
            const bool saved = cli::saveOutputsRecord(file, rec);
            cli::OutputsRecord back;
            const bool loaded = cli::loadOutputsRecord(file, back) == cli::RecordLoad::Ok;
            const auto it = back.sections.find("..|typescript");
            check(saved && loaded && it != back.sections.end() && it->second.outputs.size() == 1 &&
                      it->second.outputs[0].path == "../gen/a.ts" && it->second.outputs[0].map == "../gen/a.ts.map" &&
                      cli::recordResolve(it->second.inputs[0], recDir) == (tmp / "proj" / "a.pg").lexically_normal() &&
                      cli::serializeOutputsRecord(back) == cli::serializeOutputsRecord(rec),
                  "P40 D17: the record round-trips, paths relative to the record's directory, byte-stable");
            cli::writeFile(file, "{ not json");
            check(cli::loadOutputsRecord(file, back) == cli::RecordLoad::Corrupt, "P40 D17: a corrupt record is detected");
            cli::writeFile(file, "{\"version\": 2, \"sections\": {}}");
            check(cli::loadOutputsRecord(file, back) == cli::RecordLoad::Newer, "P40 D17: a newer record is detected");
            check(cli::loadOutputsRecord(tmp / "none.json", back) == cli::RecordLoad::Missing, "P40 D17: a missing record is detected");

            // P40 review (A4): a record is validated as a WHOLE. A bad section after a good one used to leave the
            // good one loaded (Corrupt + partial trust: it drove deletions, order-dependently). Every malformed
            // part, wherever it sits, now yields Corrupt with NOTHING loaded.
            const std::string good = "\"..|typescript\": {\"inputs\": [\"../a.pg\"], \"outputs\": [{\"path\": \"../gen/b.ts\"}]}";
            bool allEmpty = true;
            for (const std::string& bad : {std::string("\"zz|x\": {\"inputs\": [], \"outputs\": [{\"path\": 5}]}"),
                                           std::string("\"zz|x\": {\"inputs\": [5], \"outputs\": []}"),
                                           std::string("\"zz|x\": {\"inputs\": [], \"outputs\": [{\"path\": \"a\", \"map\": 1}]}"),
                                           std::string("\"zz|x\": {\"inputs\": [], \"outputs\": {}}"),
                                           std::string("\"zz|x\": {\"outputs\": []}"),
                                           std::string("\"zz|x\": {\"inputs\": [], \"outputs\": [{\"path\": \"\"}]}"),
                                           std::string("\"nobar\": {\"inputs\": [], \"outputs\": []}"),
                                           std::string("\"zz|x\": 3")}) {
                for (int order = 0; order < 2; ++order) {
                    const std::string secs = order == 0 ? good + ", " + bad : bad + ", " + good;
                    cli::writeFile(file, "{\"version\": 1, \"sections\": {" + secs + "}}");
                    back = {};
                    const cli::RecordLoad rl = cli::loadOutputsRecord(file, back);
                    if (rl != cli::RecordLoad::Corrupt || !back.sections.empty()) {
                        allEmpty = false;
                        std::cerr << "    partial trust: {" << secs << "}\n";
                    }
                }
            }
            cli::writeFile(file, "{\"version\": 1, \"sections\": {" + good + "}} trailing");
            const bool trailing = cli::loadOutputsRecord(file, back) == cli::RecordLoad::Corrupt && back.sections.empty();
            check(allEmpty && trailing, "P40 review A4: any invalid part of a record (either order) -> Corrupt with nothing loaded");
        }
        {   // P40 review (A3): a case-only rename really renames on a case-insensitive filesystem.
            const fs::path dir = tmp / "case";
            fs::create_directories(dir, ec);
            const std::string hdr = "// <auto-generated/>\n// Generated by Polyglot. Do not edit.\n";
            cli::writeFile(dir / "Foo.ts", hdr + "export const x = 1;\n");
            const bool caseInsensitive = fs::exists(dir / "FOO.TS", ec);
            auto names = [&] {
                std::vector<std::string> v;
                for (const auto& e : fs::directory_iterator(dir, ec)) v.push_back(e.path().filename().string());
                std::sort(v.begin(), v.end());
                return v;
            };
            cli::OutputSink::Claim claim{"foo.pg", "// Generated by Polyglot.", {}};
            cli::OutputSink chk(cli::OutputSink::Mode::Check);
            const auto pc = chk.put(dir / "foo.ts", hdr + "export const x = 1;\n", claim);
            const bool checkOk = caseInsensitive
                ? (pc == cli::OutputSink::Put::Unchanged && chk.drifted() && has(chk.driftLines()[0], "would rename") &&
                   names() == std::vector<std::string>{"Foo.ts"})
                : (pc == cli::OutputSink::Put::Written && has(chk.driftLines()[0], "would create"));
            cli::OutputSink w(cli::OutputSink::Mode::Write);
            const auto pw = w.put(dir / "foo.ts", hdr + "export const x = 1;\n", claim);
            const bool writeOk = caseInsensitive
                ? (pw == cli::OutputSink::Put::Unchanged && names() == std::vector<std::string>{"foo.ts"} && w.isProduced(dir / "Foo.ts"))
                : (pw == cli::OutputSink::Put::Written && names() == std::vector<std::string>{"Foo.ts", "foo.ts"});
            // A case variant that is NOT Polyglot's is never renamed (the overwrite guard holds for renames too).
            cli::writeFile(dir / "Bar.ts", "// hand-written\n");
            cli::OutputSink w2(cli::OutputSink::Mode::Write);
            const auto pr = w2.put(dir / "bar.ts", hdr, claim);
            const std::vector<std::string> after = names();
            const bool guardOk = !caseInsensitive || (pr == cli::OutputSink::Put::Refused &&
                                                      std::find(after.begin(), after.end(), "Bar.ts") != after.end());
            check(checkOk && writeOk && guardOk,
                  std::string("P40 review A3: a case-only rename renames the existing file to the exact case (") +
                      (caseInsensitive ? "case-insensitive" : "case-sensitive") + " filesystem); --check reports it");
        }
        {   // P40 review (#75 pre-push, item 1): the owned-scope ancestor test asks the FILESYSTEM, not the spelling.
            const fs::path base = tmp / "t" / "Proj";
            fs::create_directories(base / "keep", ec);
            const bool ci = fs::exists(tmp / "T" / "PROJ", ec);
            fs::path dir;
            bool rec = false;
            std::string err;
            const bool aliasSelf = cli::ownedScope(base, "../PROJ/%(RecursiveDir)%(Filename)", dir, rec, err);
            const bool aliasGrand = cli::ownedScope(base, "../../T/%(RecursiveDir)%(Filename)", dir, rec, err);
            const bool sibling = cli::ownedScope(base, "../Web/gen/%(Filename)", dir, rec, err);
            check((ci ? (!aliasSelf && !aliasGrand) : true) && sibling,
                  std::string("P40 review: an owned scope that is a case alias of the config folder or an ancestor is refused (") +
                      (ci ? "case-insensitive" : "case-sensitive: aliases are distinct folders there") + "); a sibling still passes");
        }
        {   // P40 review: writeFile replaces atomically (temp + rename) and leaves no temp behind.
            const fs::path f = tmp / "atomic" / "a.ts";
            const bool w1 = cli::writeFile(f, "one"), w2 = cli::writeFile(f, "two");
            std::string back;
            int entries = 0;
            for (const auto& e : fs::directory_iterator(f.parent_path(), ec)) { (void)e; ++entries; }
            check(w1 && w2 && cli::readFile(f, back) && back == "two" && entries == 1,
                  "P40 review: writeFile replaces the file whole and leaves no temp file behind");
        }
        {   // P40 review (A5): a pgconfig that fails to parse or validate carries errors (callers refuse with 64).
            const fs::path cd = tmp / "cfg";
            fs::create_directories(cd, ec);
            auto errorsFor = [&](const std::string& text) {
                cli::writeFile(cd / "pgconfig.json", text);
                return cli::loadPgConfig(cd).errors;
            };
            bool allRefused = true;
            for (const char* bad : {"{", "[]", "{\"target\": [\"csharp\"]}", "{\"targets\": \"csharp\"}", "{\"targets\": [1]}",
                                    "{\"targets\": [\"\"]}", "{\"lib\": \"io\"}", "{\"root\": 1}", "{\"originInfo\": \"yes\"}",
                                    "{\"dependencies\": []}", "{\"include\": {}}", "{\"include\": [3]}",
                                    "{\"include\": [{\"pattern\": \"*.pg\", \"output\": \"x\", \"outptu\": 1}]}",
                                    "{\"include\": [{\"pattern\": \"*.pg\", \"output\": \"x\", \"target\": 3}]}",
                                    "{\"forbiddenIdentifiers\": \"temp\"}", "{\"targets\":[\"csharp\"],\"access\":\"public\"}"}) {
                if (errorsFor(bad).empty()) { allRefused = false; std::cerr << "    accepted: " << bad << "\n"; }
            }
            const bool goodOk = errorsFor("\xEF\xBB\xBF{ \"$schema\": \"x\", \"targets\": [\"csharp\"], \"lib\": [\"io\"], \"originInfo\": true, "
                                          "\"forbiddenIdentifiers\": {\"*\": [\"temp\"]}, \"dependencies\": {}, "
                                          "\"include\": [{\"pattern\": \"*.pg\", \"target\": [\"csharp\"], \"output\": \"gen/%(Filename)\", \"owned\": true}] }")
                                    .empty();
            check(allRefused && goodOk, "P40 review A5: malformed or invalid pgconfig.json carries errors; a valid one (with a BOM) none");
        }
        fs::remove_all(tmp, ec);
    }

    if (g_failures == 0) {
        std::cout << "\nAll tests passed.\n";
        return 0;
    }
    std::cout << "\n" << g_failures << " test(s) failed.\n";
    return 1;
}
