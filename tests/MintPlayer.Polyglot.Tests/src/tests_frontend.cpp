// Lexer / parser / golden emission / type-checker rejects / issues #9 #11 #14 (P1-P3).
// P41 D14: one translation unit of the unit suite (split from tests_main.cpp so the areas compile in
// parallel). The bodies are unchanged; the shared harness lives in test_support.hpp.

#include "test_support.hpp"

void run_frontend() {

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

}
