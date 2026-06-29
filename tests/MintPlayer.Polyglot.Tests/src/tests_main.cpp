#include <iostream>
#include <string>

#include "mintplayer/polyglot/lexer.hpp"
#include "mintplayer/polyglot/parser.hpp"
#include "mintplayer/polyglot/pg_printer.hpp"
#include "mintplayer/polyglot/polyglot.hpp"

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
    }

    // C# emission (golden substrings).
    {
        EmitResult cs = compile(kProgram, Target::CSharp);
        check(cs.ok, "compile: program -> C# succeeds");
        check(has(cs.code, "using System;"), "C#: emits using System");
        check(has(cs.code, "static int add(int a, int b)"), "C#: function signature mapped");
        check(has(cs.code, "Console.WriteLine(add(sum, 100))"), "C#: print -> Console.WriteLine");
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

    if (g_failures == 0) {
        std::cout << "\nAll tests passed.\n";
        return 0;
    }
    std::cout << "\n" << g_failures << " test(s) failed.\n";
    return 1;
}
