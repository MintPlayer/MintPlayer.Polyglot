#include <iostream>
#include <map>
#include <optional>
#include <string>

#include "mintplayer/polyglot/backend.hpp"
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
        // P15 async/await: `async fn`/method prefix + `await` expr survive a print/parse round-trip.
        roundtrips("async fn fetch(url: string): i32 => 200\n"
                   "async fn main() {\n  let code = await fetch(\"x\")\n  print(code)\n}\n",
                   "round-trip: async fn + await stable");
        roundtrips("class Client {\n  async fn get(url: string): i32 => 1\n}\n",
                   "round-trip: async method stable");
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
        check(has(cs.code, "static void Main() {") && has(cs.code, "InvariantCulture") && has(cs.code, "main(); }"),
              "C#: main entry point emitted (with invariant-culture pin)");
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
    // §3.B statement forms: `lock`/`unsafe` are unspeakable, so a C#-habit statement is refused out loud
    // (a targeted message, not a confusing parse error) with no cascade errors.
    refuses("fn main() { lock (m) { print(1) } }\n", "locks", "P6: refuses a `lock` statement");
    refuses("fn main() { unsafe { print(1) } }\n", "unsafe", "P6: refuses an `unsafe` block");
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
        EmitResult py = compileStd(noPy, Target::Python);
        bool named = false;
        for (const auto& d : py.diagnostics) if (has(d.message, "no 'actual'") && has(d.message, "python")) named = true;
        check(!py.ok && named, "P9-V: calling a portable fn with no actual for the target is refused");
        check(compileStd(noPy, Target::CSharp).ok, "P9-V: the same call compiles for a target that has the actual");
        check(compileStd("expect fn unused(): i32\nactual(csharp) fn unused(): i32 => 1\nfn main() { print(1) }\n",
                         Target::Python).ok,
              "P9-V: an unused portable fn missing this target's actual is not refused");
    }
    // P9-V audit — a tuple pattern in `match` binds + type-checks but has no lowering, so it must refuse
    // (never miscompile) rather than emit a call against undefined binders. (for-in destructuring is fine.)
    {
        EmitResult r = compileStd("fn f(p: (i32, i32)): i32 => match p { (a, b) => a + b }\n"
                                  "fn main() { print(f((1, 2))) }\n", Target::CSharp);
        bool named = false;
        for (const auto& d : r.diagnostics) if (has(d.message, "tuple patterns in 'match'")) named = true;
        check(!r.ok && named, "P9-V audit: a tuple pattern in match is refused, not miscompiled");
    }
    // INumber — a generic param bounded by the numeric marker must infer to a number; a non-numeric type
    // argument is refused at Polyglot compile time (better DX than a target-compiler error / TS NaN).
    {
        auto refusesNum = [&](const char* src, const std::string& name) {
            EmitResult r = compileStd(src, Target::CSharp);
            bool named = false;
            for (const auto& d : r.diagnostics) if (has(d.message, "INumber")) named = true;
            check(!r.ok && named, name);
        };
        refusesNum("fn main() { print(Math.max(\"a\", \"b\")) }\n", "INumber: Math.max on strings is refused");
        refusesNum("fn main() { print(Math.abs(true)) }\n", "INumber: Math.abs on bool is refused");
        resolvesStd("fn main() { print(Math.max(3, 8))\n print((i32)Math.round(2.7)) }\n",
                    "INumber: Math.max/round on numbers still resolve");
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

        EmitResult cs = compileStd(prog, Target::CSharp);
        check(cs.ok, "P15 C#: async program compiles");
        check(has(cs.code, "async global::System.Threading.Tasks.Task<int> doubleIt"),
              "P15 C#: async fn -> `async Task<T>` (unwrapped T wrapped by the backend)");
        check(has(cs.code, "async global::System.Threading.Tasks.Task main"),
              "P15 C#: async unit fn -> `async Task` (bare, no <T>)");
        check(has(cs.code, "await Program.doubleIt(21)"), "P15 C#: `await e` emits `await`");
        check(has(cs.code, "main().GetAwaiter().GetResult();"),
              "P15 C#: async main is driven synchronously from Main()");

        EmitResult ts = compileStd(prog, Target::TypeScript);
        check(ts.ok, "P15 TS: async program compiles");
        check(has(ts.code, "async function doubleIt(x: number): Promise<number>"),
              "P15 TS: async fn -> `async function …: Promise<T>`");
        check(has(ts.code, "async function main(): Promise<void>"),
              "P15 TS: async unit fn -> `Promise<void>`");
        check(has(ts.code, "await doubleIt(21)"), "P15 TS: `await e` emits `await`");
        check(has(ts.code, "\nmain();\n"), "P15 TS: async main is a floating top-level call");

        EmitResult py = compileStd(prog, Target::Python);
        check(py.ok, "P15 Python: async program compiles");
        check(has(py.code, "async def doubleIt("), "P15 Python: async fn -> `async def`");
        check(has(py.code, "async def main("), "P15 Python: async main -> `async def`");
        check(has(py.code, "await doubleIt(21)"), "P15 Python: `await e` emits `await`");
        check(has(py.code, "asyncio.run(main())") && has(py.code, "import asyncio"),
              "P15 Python: async main -> asyncio.run + `import asyncio` prepended");

        // sema §4.7: `await` outside an async fn is refused (else a native compile error = a §3.B miscompile).
        {
            EmitResult r = compileStd("fn doubleIt(x: i32): i32 => x * 2\n"
                                      "fn main() { print(await doubleIt(1)) }\n", Target::CSharp);
            bool named = false;
            for (const auto& d : r.diagnostics) if (has(d.message, "await") && has(d.message, "async fn")) named = true;
            check(!r.ok && named, "P15 sema: `await` outside an async fn is refused");
        }
        // sema §4.7: `async` + `yield` (async iterators) is refused — out of scope for v1.
        {
            EmitResult r = compileStd("async fn g(): Iterable<i32> {\n  yield 1\n}\nfn main() { print(1) }\n",
                                      Target::CSharp);
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
            EmitResult r = compileStd(src, Target::CSharp);
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

    // P14b — interfaces are emitted (were dropped at lowering), a record's `: Interface` base/implements
    // clause is carried, and `operator fn get` is a real indexer (C# `this[]`, TS `get(i)` method + `.get()`).
    {
        const char* prog =
            "interface Shrinkable { fn shrink(): i32 }\n"
            "record Box(n: i32) : Shrinkable {\n  override fn shrink(): i32 => this.n - 1\n}\n"
            "class Bag {\n  operator fn get(i: i32): i32 => i\n}\n"
            "fn at(b: Bag): i32 => b[3]\n"
            "fn main() {}\n";
        EmitResult cs = compileStd(prog, Target::CSharp);
        check(cs.ok && has(cs.code, "interface Shrinkable") && has(cs.code, "record Box(int n) : Shrinkable")
                    && has(cs.code, "public int this[int i] =>"),
              "P14b: C# emits interface + record base + this[] indexer");
        EmitResult ts = compileStd(prog, Target::TypeScript);
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
        EmitResult cs = compileStd(prog, Target::CSharp);
        check(cs.ok && has(cs.code, "s.ToUpper()") && !has(cs.code, "s.toUpper()"),
              "P14b: bound string extension -> C# .ToUpper()");
        EmitResult ts = compileStd(prog, Target::TypeScript);
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

    if (g_failures == 0) {
        std::cout << "\nAll tests passed.\n";
        return 0;
    }
    std::cout << "\n" << g_failures << " test(s) failed.\n";
    return 1;
}
