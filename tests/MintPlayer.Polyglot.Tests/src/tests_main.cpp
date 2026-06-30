#include <iostream>
#include <map>
#include <optional>
#include <string>

#include "mintplayer/polyglot/backend.hpp"
#include "mintplayer/polyglot/capability.hpp"
#include "mintplayer/polyglot/ir.hpp"
#include "mintplayer/polyglot/lexer.hpp"
#include "mintplayer/polyglot/lower.hpp"
#include "mintplayer/polyglot/parser.hpp"
#include "mintplayer/polyglot/pg_printer.hpp"
#include "mintplayer/polyglot/polyglot.hpp"
#include "mintplayer/polyglot/sema.hpp"

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
EmitResult compileStd(const std::string& src, Target target) {
    return compile(src, target, nullptr, LibConfig{{"io", "math", "collections"}});
}

// A backend that supports the whole §3.A surface except one feature — used to prove §3.E gating bites.
class StubBackend : public Backend {
public:
    explicit StubBackend(Feature missing) : missing_(missing) {}
    std::string name() const override { return "stub"; }
    std::string emit(const ir::Module&) const override { return ""; }
    bool supports(Feature f) const override { return f != missing_; }
private:
    Feature missing_;
};

// An in-memory module resolver (the tests' filesystem-free stand-in for the CLI's FileModuleResolver):
// module specifier -> source text. Proves cross-`.pg` resolution works without touching disk.
class MapModuleResolver : public ModuleResolver {
public:
    explicit MapModuleResolver(std::map<std::string, std::string> modules) : modules_(std::move(modules)) {}
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
    check(!Compiler::version().empty(), "version is non-empty");
    check(Compiler::version() == std::string(kVersion), "version matches the kVersion constant");

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
    }

    // C# emission (golden substrings).
    {
        EmitResult cs = compileStd(kProgram, Target::CSharp);
        check(cs.ok, "compile: program -> C# succeeds");
        check(!has(cs.code, "using System;"), "C#: no `using` (BCL refs are global::-qualified)");
        check(has(cs.code, "public static int add(int a, int b)"), "C#: function signature mapped");
        // print is now std.io's generic `print<T>` (capability mechanism): its csharp `actual` body is the
        // Console.WriteLine, and the call site is a normal free-function call (Program.print(...)).
        check(has(cs.code, "global::System.Console.WriteLine(x is bool"), "C#: std.io print body -> Console.WriteLine (bool-lowercasing)");
        check(has(cs.code, "Program.print(Program.add(sum, 100))"), "C#: print call site -> Program.print");
        check(has(cs.code, "static void Main() { main(); }"), "C#: main entry point emitted");
    }

    // TypeScript emission (golden substrings).
    {
        EmitResult ts = compileStd(kProgram, Target::TypeScript);
        check(ts.ok, "compile: program -> TS succeeds");
        check(has(ts.code, "function add(a: number, b: number): number"), "TS: function signature mapped");
        // print's typescript `actual` wraps in String(...) so bigint/number print identically to C# WriteLine.
        check(has(ts.code, "console.log(String(x))"), "TS: std.io print body -> console.log(String(x))");
        check(has(ts.code, "print(add(sum, 100))"), "TS: print call site");
        check(has(ts.code, "\nmain();\n"), "TS: top-level main() call emitted");
    }

    // Operator precedence is preserved with minimal parentheses.
    {
        EmitResult a = compile("fn f(): i32 => 1 + 2 * 3\n", Target::CSharp);
        check(a.ok && has(a.code, "return 1 + 2 * 3;"), "emit: precedence keeps 1 + 2 * 3 unparenthesized");
        EmitResult b = compile("fn g(): i32 => (1 + 2) * 3\n", Target::CSharp);
        check(b.ok && has(b.code, "return (1 + 2) * 3;"), "emit: precedence parenthesizes (1 + 2) * 3");
    }

    // Type checker rejects bad programs with diagnostics (never a miscompile).
    auto rejects = [&](const char* src, const std::string& name) {
        EmitResult r = compile(src, Target::CSharp);
        check(!r.ok && !r.diagnostics.empty(), name);
    };
    rejects("fn main() { let x = 1 + true; }\n", "sema: rejects i32 + bool");
    rejects("fn main() { print(y); }\n", "sema: rejects undeclared name");
    rejects("fn main() { let x = 1; x = 2; }\n", "sema: rejects assignment to immutable let");
    rejects("fn main() { if 1 { print(0); } }\n", "sema: rejects non-bool if condition");

    // P4 — name / type resolution across the declaration surface.
    auto resolves = [&](const char* src, const std::string& name) {
        EmitResult r = compile(src, Target::CSharp);
        check(r.ok, name);
    };
    // Same, but with the std prelude (for programs that call `print` and friends).
    auto resolvesStd = [&](const char* src, const std::string& name) {
        EmitResult r = compileStd(src, Target::CSharp);
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
        std::string ir = ir::dump(lower(unit));
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
    check(findBackend("python") == nullptr, "P5: unknown backend is not found");

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
        EmitResult r = compile(src, Target::CSharp);
        bool named = false;
        for (const auto& d : r.diagnostics) if (has(d.message, "refuses") && has(d.message, needle)) named = true;
        check(!r.ok && named, name);
    };
    refuses("fn f(x: decimal) {}\n", "decimal", "P6: refuses 'decimal' with a targeted message");
    refuses("fn f(t: Thread) {}\n", "threads", "P6: refuses Thread (threads/locks)");
    refuses("fn f(p: IntPtr) {}\n", "pointers", "P6: refuses IntPtr (pointers/unsafe)");
    refuses("fn f(e: Expression<i32>) {}\n", "expression trees", "P6: refuses LINQ expression trees");
    refuses("fn f(d: dynamic) {}\n", "dynamic", "P6: refuses 'dynamic' / runtime code-gen");
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
        EmitResult cs = compileStd(prog, Target::CSharp);
        check(cs.ok, "P8: List program -> C# compiles");
        check(has(cs.code, "global::System.Collections.Generic.List<int>"), "P8 C#: List<i32> -> System.Collections.Generic.List<int>");
        check(has(cs.code, ".Add(3)"), "P8 C#: list.add -> .Add");
        check(has(cs.code, ".Count"), "P8 C#: list.count -> .Count");
        check(has(cs.code, ".RemoveAll("), "P8 C#: list.removeAll -> .RemoveAll");
        check(has(cs.code, "xs.Clear()"), "P8 C#: list.clear -> .Clear()");
        check(!has(cs.code, "class List"), "P8 C#: extern class List is not emitted");

        EmitResult ts = compileStd(prog, Target::TypeScript);
        check(ts.ok, "P8: List program -> TS compiles");
        check(has(ts.code, "number[]"), "P8 TS: List<i32> -> number[]");
        check(has(ts.code, ".push(3)"), "P8 TS: list.add -> .push");
        check(has(ts.code, ".length"), "P8 TS: list.count -> .length");
        check(has(ts.code, "xs = xs.filter("), "P8 TS: list.removeAll -> receiver = receiver.filter(...)");
        check(has(ts.code, "xs = []"), "P8 TS: list.clear -> receiver = [] (receiver reassignment)");
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
        EmitResult cs = compileStd(prog, Target::CSharp);
        check(cs.ok, "P8: std.io program -> C# compiles");
        check(has(cs.code, "global::System.IO.File.ReadAllText(path)"), "P8 C#: readText -> File.ReadAllText");
        check(has(cs.code, "global::System.IO.File.WriteAllText(path, content)"), "P8 C#: writeText -> File.WriteAllText");
        EmitResult ts = compileStd(prog, Target::TypeScript);
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
        EmitResult cs = compile(prog, Target::CSharp, &r, LibConfig{{"io"}});
        check(cs.ok, "P12: transitive user-module resolution compiles");
        check(has(cs.code, "baseVal") && has(cs.code, "bump"), "P12: imported module decls are merged");
        EmitResult ts = compile(prog, Target::TypeScript, &r, LibConfig{{"io"}});
        check(ts.ok, "P12: transitive resolution compiles to TS too");
    }
    {
        // Import cycle a -> b -> a is a clear diagnostic, not a hang.
        MapModuleResolver r({
            {"a", "import { fromB } from \"b\"\nfn fromA(): i32 => fromB()\n"},
            {"b", "import { fromA } from \"a\"\nfn fromB(): i32 => fromA()\n"},
        });
        EmitResult res = compile("import { fromA } from \"a\"\nfn main() { print(fromA()) }\n", Target::CSharp, &r);
        bool cyclic = false;
        for (const auto& d : res.diagnostics) if (has(d.message, "import cycle")) cyclic = true;
        check(!res.ok && cyclic, "P12: an import cycle is reported (no hang)");
    }
    {
        // An unknown user module (resolver returns nullopt) is an "unknown module" diagnostic.
        MapModuleResolver r({});
        EmitResult res = compile("import { x } from \"missing\"\nfn main() {}\n", Target::CSharp, &r);
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
        EmitResult res = compile("import { R } from \"m\"\nrecord R(y: i32)\nfn main() {}\n", Target::CSharp, &r);
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
                               Target::CSharp, nullptr, lib);
        check(r.ok && has(r.code, ".Count"), "P13: lib auto-imports std.collections (List.count -> .Count)");
        // (2) a user declaration silently shadows the lib's same-named one (NO collision error).
        EmitResult sh = compile("record List(x: i32)\nfn main() { print(List(5).x) }\n",
                                Target::CSharp, nullptr, lib);
        check(sh.ok, "P13: a user decl silently shadows the lib prelude (no collision)");
        // (3) an explicit import of a lib module dedups against the lib auto-import (no collision).
        EmitResult dd = compile("import { List } from \"std.collections\"\nfn main() { var xs: List<i32> = [1]\n  print(xs.count) }\n",
                                Target::CSharp, nullptr, lib);
        check(dd.ok, "P13: explicit import + lib dedups (no collision)");
        // (4) a QUALIFIED lib entry is a full specifier resolved like any import (third-party plugin
        //     namespace, not just std) — `app.helpers` resolves through the resolver, used un-imported.
        MapModuleResolver plug({{"app.helpers", "fn helper(): i32 => 42\n"}});
        LibConfig pluginLib{{"app.helpers", "io"}};
        EmitResult pl = compile("fn main() { print(helper()) }\n", Target::CSharp, &plug, pluginLib);
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
        EmitResult cs = compile(prog, Target::CSharp);
        check(cs.ok && has(cs.code, "w.Poke(3)") && !has(cs.code, "class Widget"),
              "P13: user plugin class member binding fires in C# (extern class not emitted)");
        EmitResult ts = compile(prog, Target::TypeScript);
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
        EmitResult cs = compileStd(prog, Target::CSharp);
        check(cs.ok && has(cs.code, "(5 * 2)") && has(cs.code, "Program.print(42)") && !has(cs.code, "class MyMath"),
              "P13: static method + static const bindings fire on an extern class (C#)");
        EmitResult ts = compileStd(prog, Target::TypeScript);
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
            return ir::dump(lower(unit));
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
        EmitResult cs = compileStd(prog, Target::CSharp);
        check(cs.ok && has(cs.code, "e.Message"), "P13: Error.message resolves on a subclass and binds to C# .Message");
        EmitResult ts = compileStd(prog, Target::TypeScript);
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
        EmitResult cs = compileStd(prog, Target::CSharp);
        check(cs.ok && has(cs.code, "firstOrNull<T>("), "P13: extension on a generic receiver scopes T (C# generic signature)");
        EmitResult ts = compileStd(prog, Target::TypeScript);
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
        EmitResult cs = compileStd(prog, Target::CSharp);
        check(cs.ok && has(cs.code, "global::Acme.Widget make()") && has(cs.code, "new global::Acme.Widget(7)")
                    && !has(cs.code, "class Widget"),
              "P10: user extern class maps its type + constructs ($T) in C#");
        EmitResult ts = compileStd(prog, Target::TypeScript);
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
        EmitResult cs = compileStd(prog, Target::CSharp);
        check(cs.ok && has(cs.code, "global::System.Collections.Generic.List<int>"),
              "P10: List type spelling comes from its extern-class mapping (C#)");
        EmitResult ts = compileStd(prog, Target::TypeScript);
        check(ts.ok && has(ts.code, "number[]"), "P10: List<i32> -> number[] from the mapping (TS)");
    }

    // P14 slice 1 — generic discriminated unions: `union Box<T>` declares + constructs (type inferred) +
    // matches, byte-identical across targets (C# generic record hierarchy, TS generic tagged union).
    {
        const char* prog =
            "union Box<T> { Full(value: T), Empty }\n"
            "fn unwrap<T>(b: Box<T>, dflt: T): T => match b { Full(v) => v, Empty => dflt }\n"
            "fn main() { print(unwrap(Full(5), 0)) }\n";
        EmitResult cs = compileStd(prog, Target::CSharp);
        check(cs.ok && has(cs.code, "abstract record Box<T>") && has(cs.code, "sealed record Full<T>(T value) : Box<T>")
                    && has(cs.code, "new Full<int>(5)") && has(cs.code, "Full<T>(var v)"),
              "P14: generic union — C# record hierarchy + typed construction + typed pattern");
        EmitResult ts = compileStd(prog, Target::TypeScript);
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
        EmitResult cs = compileStd(prog, Target::CSharp);
        check(cs.ok && has(cs.code, "new Some<int>(v)") && has(cs.code, "new None<int>()"),
              "P14: core Option — Some/None construct in C# (None typed from context)");
        EmitResult ts = compileStd(prog, Target::TypeScript);
        check(ts.ok && has(ts.code, "{ tag: \"Some\", value: v }") && has(ts.code, "{ tag: \"None\" }"),
              "P14: core Option — Some/None construct in TS");
    }

    // P14 slice 3 — the `T?` sugar: `T?` over a generic desugars to Option<T>; a bare value coerces to Some,
    // `null` to None; `?? d` lowers to a match. Plus extension-receiver inference (List<i32> -> T=i32).
    {
        const char* prog =
            "fn pickIf<T>(c: bool, v: T): T? => if c { v } else { null }\n"
            "fn main() { print(pickIf(true, 7) ?? -1) }\n";
        EmitResult cs = compileStd(prog, Target::CSharp);
        check(cs.ok && has(cs.code, "Option<T> pickIf<T>") && has(cs.code, "new Some<T>(v)") && has(cs.code, "new None<T>()")
                    && has(cs.code, "Some<int>(var __opt0) => __opt0") && has(cs.code, "None<int> _ => -1"),
              "P14: T? sugar — desugars to Option, ?? lowers to match (C#)");
        EmitResult ts = compileStd(prog, Target::TypeScript);
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
        EmitResult cs = compileStd(prog, Target::CSharp);
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
        EmitResult cs = compileStd(prog, Target::CSharp);
        check(cs.ok && has(cs.code, "new global::System.Collections.Generic.List<int> {") && !has(cs.code, "List<object>"),
              "P14b: empty list literal takes its element type from the target (List<int>, not List<object>)");
    }

    // P10 — the core prelude (Error/Iterable) is ALWAYS linked (no import, no lib): with a bare `compile`,
    // Error constructs + maps per target and `.message` binds — proving they're core-prelude extern classes,
    // not emitter hardcodes.
    {
        const char* prog =
            "fn boom(): Error => Error(\"x\")\n"
            "fn msg(e: Error): string => e.message\n"
            "fn main() {}\n";
        EmitResult cs = compile(prog, Target::CSharp);
        check(cs.ok && has(cs.code, "new global::System.Exception(\"x\")") && has(cs.code, "e.Message"),
              "core prelude: Error constructs + .message maps to C# (no import/lib)");
        EmitResult ts = compile(prog, Target::TypeScript);
        check(ts.ok && has(ts.code, "new Error(\"x\")") && has(ts.code, "e.message"),
              "core prelude: Error maps to JS (no import/lib)");
    }

    // P13 — unknown/unimported types fail compilation, not just in signatures but in LOCAL positions too
    // (previously a local `let x: T`/`var xs: List<…>` slipped, silently miscompiling).
    rejects("fn main() { let w: Widget = 0 }\n", "P13: unknown type on a local `let` is rejected");
    rejects("fn main() { var xs: List<i32> = [1] }\n", "P13: List used without importing std.collections is rejected");

    // A normal unknown type still gets the plain diagnostic (not a refusal).
    {
        EmitResult r = compile("fn f(x: Widget) {}\n", Target::CSharp);
        bool plain = false;
        for (const auto& d : r.diagnostics) if (has(d.message, "unknown type")) plain = true;
        check(!r.ok && plain, "P6: a non-refused unknown type still says 'unknown type'");
    }

    if (g_failures == 0) {
        std::cout << "\nAll tests passed.\n";
        return 0;
    }
    std::cout << "\n" << g_failures << " test(s) failed.\n";
    return 1;
}
