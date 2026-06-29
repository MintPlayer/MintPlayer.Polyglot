#include <iostream>
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
        roundtrips("import std.io.{ print }\nimport std.math.{ sqrt, PI }\n"
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
        EmitResult cs = compile(kProgram, Target::CSharp);
        check(cs.ok, "compile: program -> C# succeeds");
        check(!has(cs.code, "using System;"), "C#: no `using` (BCL refs are global::-qualified)");
        check(has(cs.code, "public static int add(int a, int b)"), "C#: function signature mapped");
        check(has(cs.code, "global::System.Console.WriteLine(Program.add(sum, 100))"), "C#: print -> global::System.Console.WriteLine");
        check(has(cs.code, "static void Main() { main(); }"), "C#: main entry point emitted");
    }

    // TypeScript emission (golden substrings).
    {
        EmitResult ts = compile(kProgram, Target::TypeScript);
        check(ts.ok, "compile: program -> TS succeeds");
        check(has(ts.code, "function add(a: number, b: number): number"), "TS: function signature mapped");
        check(has(ts.code, "console.log(add(sum, 100))"), "TS: print -> console.log");
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
        const char* src = "fn add(a: i32, b: i32): i32 {\n  return a + b\n}\n"
                          "fn main() {\n  print(add(1, 2))\n}\n";
        DiagnosticBag d;
        auto unit = parse(lex(src, d), d);
        mintplayer::polyglot::check(unit, d); // annotate resolved types
        check(!d.hasErrors(), "IR: program type-checks");
        std::string ir = ir::dump(lower(unit));
        check(has(ir, "fn add(a: i32, b: i32): i32 {"), "IR: function signature carries types");
        check(has(ir, "return (a:i32 + b:i32):i32"), "IR: binary expression is typed");
        check(has(ir, "fn main(): unit [entry] {"), "IR: main resolved as the entry point");
        check(has(ir, "print(add(1:i32, 2:i32):i32):unit"), "IR: print intrinsic + typed call resolved");
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
    resolves("fn f(x: i32): i32 => x\nfn f(x: f64): i32 => 0\nfn f(a: i32, b: i32): i32 => a\n"
             "fn main() { print(f(1))\n print(f(1.0))\n print(f(1, 2)) }\n", "P6: function overloading resolves");
    rejects("fn f(x: i32): i32 => x\nfn f(y: i32): i32 => y\nfn main() {}\n",
            "P6: rejects a true duplicate (same parameter types)");

    // P7 — portable-core guard: `extern` is only allowed inside a target-gated `actual`.
    rejects("fn portable(): i32 => extern(\"42\")\nfn main() {}\n",
            "P7: extern is refused in portable code");
    resolves("expect fn a(): i32\nactual(csharp) fn a(): i32 => extern(\"42\")\n"
             "actual(typescript) fn a(): i32 => extern(\"42\")\nfn main() { print(a()) }\n",
             "P7: extern is allowed inside an actual");

    // P8 — List<T> as a first-party .pg std type, bound to each target via the FFI binding mechanism.
    {
        const char* prog =
            "import std.collections.{ List }\n"
            "fn first(xs: List<i32>): i32 => xs[0]\n"
            "fn main() {\n"
            "  var xs: List<i32> = [1, 2]\n"
            "  xs.add(3)\n"
            "  print(xs.count)\n"
            "  print(first(xs))\n"
            "  xs.removeAll((v) => v < 2)\n"
            "  xs.clear()\n"
            "}\n";
        EmitResult cs = compile(prog, Target::CSharp);
        check(cs.ok, "P8: List program -> C# compiles");
        check(has(cs.code, "global::System.Collections.Generic.List<int>"), "P8 C#: List<i32> -> System.Collections.Generic.List<int>");
        check(has(cs.code, ".Add(3)"), "P8 C#: list.add -> .Add");
        check(has(cs.code, ".Count"), "P8 C#: list.count -> .Count");
        check(has(cs.code, ".RemoveAll("), "P8 C#: list.removeAll -> .RemoveAll");
        check(has(cs.code, "xs.Clear()"), "P8 C#: list.clear -> .Clear()");
        check(!has(cs.code, "class List"), "P8 C#: extern class List is not emitted");

        EmitResult ts = compile(prog, Target::TypeScript);
        check(ts.ok, "P8: List program -> TS compiles");
        check(has(ts.code, "number[]"), "P8 TS: List<i32> -> number[]");
        check(has(ts.code, ".push(3)"), "P8 TS: list.add -> .push");
        check(has(ts.code, ".length"), "P8 TS: list.count -> .length");
        check(has(ts.code, "xs = xs.filter("), "P8 TS: list.removeAll -> receiver = receiver.filter(...)");
        check(has(ts.code, "xs = []"), "P8 TS: list.clear -> receiver = [] (receiver reassignment)");
        check(!has(ts.code, "class List"), "P8 TS: extern class List is not emitted");
    }

    // P8 — importing an unknown std module is a diagnostic (real module resolution, not a silent no-op).
    rejects("import std.bogus.{ Thing }\nfn main() {}\n", "P8: unknown std module is rejected");

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
