#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
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
#include "mintplayer/polyglot/ir.hpp"
#include "mintplayer/polyglot/json.hpp"
#include "mintplayer/polyglot/lexer.hpp"
#include "mintplayer/polyglot/lower.hpp"
#include "mintplayer/polyglot/parser.hpp"
#include "mintplayer/polyglot/pg_printer.hpp"
#include "mintplayer/polyglot/polyglot.hpp"
#include "mintplayer/polyglot/sema.hpp"

#include "exe_path.hpp" // the CLI's portable executable-path lookup; tests add Cli/src to their includes
#include "watch.hpp"    // the CLI's watch-mode support (P21); same include path

#include "inflate.hpp"  // P30 slice 0: the auto-download primitives (same include path as watch.hpp)
#include "lockfile.hpp" // P30 slice 2: the pgconfig.lock.json pin + the versioned plugin cache
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
    explicit StubBackend(Feature missing) : missing_(missing) {}
    StubBackend(Feature missing, Feature emulated) : missing_(missing), emulated_(emulated), hasEmulated_(true) {}
    std::string name() const override { return "stub"; }
    std::string emit(const ir::Module&) const override { return ""; }
    bool supports(Feature f) const override { return f != missing_; }
    std::string capabilityStance(Feature f) const override {
        if (f == missing_) return "false";
        if (hasEmulated_ && f == emulated_) return "emulated";
        return "native";
    }
private:
    Feature missing_;
    Feature emulated_ = Feature::ExtensionMethods;
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
                   "class Disk : Shape, Disposable {\n  let r: f64\n  init(r: f64) {\n    this.r = r\n  }\n"
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
        check(has(cs.code, "global::System.Console.WriteLine(x is bool"), "C#: std.io print body -> Console.WriteLine (bool-lowercasing)");
        check(has(cs.code, "PolyglotProgram.print(PolyglotProgram.add(sum, 100))"), "C#: print call site -> PolyglotProgram.print");
        check(has(cs.code, "static void Main() {") && has(cs.code, "InvariantCulture") && has(cs.code, "main(); }"),
              "C#: main entry point emitted (with invariant-culture pin)");
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
        MapModuleResolver res({{"lib", "class Box { var v: i32\n  init(v: i32) { this.v = v } }\nfn dbl(x: i32): i32 { return x * 2 }\n"}});
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

    // Access modifier — `--access public` (LibConfig::access) prefixes emitted C# types + wrappers; the
    // default (empty) keeps C#'s modifier-less `internal` (byte-identical).
    {
        LibConfig pub; pub.access = "public";
        EmitResult r = compile("record Point(x: i32, y: i32)\nfn origin(): Point => Point(0, 0)\n", findTarget("csharp"), nullptr, pub);
        check(r.ok && has(r.code, "public record Point") && has(r.code, "public static class PolyglotProgram"),
              "issue#11 access: --access public emits public types + wrapper");
        EmitResult def = compile("record Point(x: i32, y: i32)\n", findTarget("csharp"));
        check(def.ok && !has(def.code, "public record Point"), "issue#11 access: default is internal (no public modifier)");
    }

    // Issue #14 — a multi-file C# PROJECT build (LibConfig::sharedPrelude) hoists the runtime prelude
    // (Option/Some/None + wrapper) into a separate `__polyglot_prelude` module so N independent roots don't
    // each emit it (CS0101/CS8863). The default (sharedPrelude=false) inlines it -> byte-identical.
    {
        const char* src = "fn fa(): i32 { return 1 }\n";
        LibConfig proj; proj.sharedPrelude = true;
        EmitResult r = compile(src, findTarget("csharp"), nullptr, proj);
        check(r.ok, "issue#14: C# project build compiles");
        check(!has(r.code, "record Option"), "issue#14: entry file does NOT inline the prelude Option union");
        check(has(r.code, "static partial class PolyglotProgram"), "issue#14: entry wrapper is `partial` (mergeable)");
        bool preludeFile = false;
        for (const auto& mf : r.modules)
            if (mf.basename == "__polyglot_prelude" && has(mf.code, "record Option")) preludeFile = true;
        check(preludeFile, "issue#14: the prelude (Option) is emitted once into __polyglot_prelude");
        // Default (single-file) still inlines the prelude — byte-identical.
        EmitResult def = compile(src, findTarget("csharp"));
        check(def.ok && has(def.code, "record Option") && def.modules.empty(),
              "issue#14: default (non-project) build still inlines the prelude (unchanged)");
        // C#-only: a TS project build keeps the prelude per-file (no shared file).
        EmitResult ts = compile(src, findTarget("typescript"), nullptr, proj);
        check(ts.ok && ts.modules.empty(), "issue#14: sharedPrelude is a no-op for non-C# targets");
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
        for (const auto& u : uses) if (u.feature == Feature::ExtensionMethods) sawExt = true;
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
    resolves("open class Base {\n  let id: i32\n  init() { this.id = 0 }\n}\n"
             "class Sub : Base {\n  init() { super() }\n}\n"
             "fn f(s: Sub): i32 => s.id\n", "P13: a subclass reaches an inherited base member");

    // P13 follow-up — `Error.message`: resolves on an `: Error` subclass (base walk) and binds per target
    // (C# `.Message` on System.Exception vs JS `.message`), since `Error` isn't a source `extern class`.
    {
        const char* prog =
            "class MyErr : Error {\n  init(message: string) { super(message) }\n}\n"
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
            "  init(n: i32) {\n    actual(csharp)     extern(\"new $T($0)\")\n    actual(typescript) extern(\"new $T($0)\")\n  }\n"
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
                    && has(ts.code, "{ tag: \"Full\", value: 5 }"),
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
        check(ts.ok && has(ts.code, "{ tag: \"Some\", value: v }") && has(ts.code, "{ tag: \"None\" }"),
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
            "class Node {\n  var next: List<Node?>\n  init() { this.next = List<Node?>() }\n}\n"
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
        EmitResult tn = compileStd("class Node {\n  var v: i32\n  init() { this.v = 0 }\n}\n"
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
            "class Node { var v: i32\n  init() { this.v = 0 } }\n"
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
        check(cs.ok && has(cs.code, "s.ToUpper()") && !has(cs.code, "s.toUpper()"),
              "P14b: bound string extension -> C# .ToUpper()");
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
        check(!loadBackend(R"({"name":"stubby",
                              "spec":{"name":"stubby"},"rules":{"Program":"x","Type":"x"}})", err) &&
                  has(err, "anti-silent-drop"),
              "P19: loadBackend refuses a plugin with undeclared coverage gaps");
        // A "native" capability claim with no rule behind it is also a lie the loader catches.
        check(!loadBackend(R"({"name":"stubby2","spec":{"name":"stubby2"},
                              "capabilities":{"patternMatching":"native"},
                              "rules":{"Program":"x","Type":"x"}})", err) &&
                  has(err, "no rule"),
              "P19: loadBackend refuses coverage gaps regardless of claims");
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
        check(!py.ok && has(py.diagnostics[0].message, "'_pg_idiv' is reserved by target 'python'"),
              "P19: a python-runtime-helper fn name refuses on python");
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
            "class Box {\n  var v: f64\n  init(v: f64) { this.v = v }\n}\n"
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
        std::string self = capIr("class C {\n  let v: i32\n  init() {\n    this.v = 0\n    let f = () => this.v\n  }\n}\n");
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
            for (const auto& use : collectFeatureUses(u)) if (use.feature == f) return true;
            return false;
        };

        // Detection: each new axis is marked from a type annotation / a mutable class field.
        auto width = parseOf("fn f(x: u8): i32 { return x }\n");
        check(usesFeature(width, Feature::FixedWidthIntegers), "P26 s0: a sub-64/unsigned width marks fixedWidthIntegers");
        auto ch = parseOf("fn f(c: char): char { return c }\n");
        check(usesFeature(ch, Feature::Utf16Strings), "P26 s0: a `char` type marks utf16Strings");
        auto mref = parseOf("class Box { var v: i32\n  init() { this.v = 0 } }\n");
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
        const std::string manifest = "{ \"schema\": 1, \"name\": \"demo\" }";
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
        const std::string manifest = "{ \"schema\": 1, \"name\": \"python\" }";
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
                       "{ \"schema\": 1, \"name\": \"python\", \"evil\": true }");
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
        check(ts.ok && has(ts.code, ".codePointAt(0)!"), "issue#34: TS codePointAt -> .codePointAt()!");
        EmitResult py = compileStd(prog, findTarget("python"));
        check(py.ok && has(py.code, "ord("), "issue#34: Python codePointAt -> ord()");
        EmitResult php = compileStd(prog, findTarget("php"));
        check(php.ok && has(php.code, "ord(substr("), "issue#34: PHP codePointAt -> ord(substr()) (byte-consistent, no mbstring)");
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
        // A typed match binding is NOT a runtime type test — refuse instead of first-arm-always-wins.
        EmitResult typedPat = compileStd(
            "open class Shape {\n  open fn kind(): i32 => 0\n}\n"
            "class Disk : Shape {\n  override fn kind(): i32 => 1\n}\n"
            "fn f(s: Shape): i32 => match s { d: Disk => 1, _ => 0 }\nfn main() {}\n", findTarget("csharp"));
        check(!typedPat.ok, "issue#29: typed 'match' binding (fake narrowing) is refused");
        bool namedPat = false;
        for (const auto& dg : typedPat.diagnostics) if (has(dg.message, "not a runtime type test")) namedPat = true;
        check(namedPat, "issue#29: the typed-pattern diagnostic explains itself");
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
            "class C {\n  var x: i32\n  init(x: i32) { this.x = x }\n  fn get(): i32 => this.x\n}\nfn main() { print(C(3).get()) }\n",
            findTarget("csharp"));
        check(initOk.ok, "G11: field assigned in init passes");

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

    if (g_failures == 0) {
        std::cout << "\nAll tests passed.\n";
        return 0;
    }
    std::cout << "\n" << g_failures << " test(s) failed.\n";
    return 1;
}
