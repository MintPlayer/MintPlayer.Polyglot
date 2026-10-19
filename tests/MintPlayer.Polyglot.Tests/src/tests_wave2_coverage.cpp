// Wave-2 front-end robustness (G42-G48) and P39 / issue #71 coverage attribution.
// P41 D14: one translation unit of the unit suite (split from tests_main.cpp so the areas compile in
// parallel). The bodies are unchanged; the shared harness lives in test_support.hpp.

#include "test_support.hpp"

void run_wave2_coverage() {
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
            // P40 follow-up: the same rule for a block lambda whose result (from the function type it flows into)
            // is a value; a unit-result block lambda, or one whose every path returns/throws, passes.
            const std::string applyFn = "fn apply(f: (i32) => i32, x: i32): i32 => f(x)\nfn run(f: (i32) => unit) { f(1) }\n";
            check(refused(applyFn + "fn main() {\n  print(apply((n) => {\n    if n > 0 { return n }\n  }, 3))\n}\n",
                          "this lambda can reach the end of its body without returning a value"),
                  "P40 follow-up: a value-result block lambda with a reachable end is refused");
            check(ok(applyFn + "fn main() {\n  print(apply((n) => {\n    if n > 0 { return n }\n    throw Error(\"neg\")\n  }, 3))\n}\n"),
                  "P40 follow-up: a block lambda that returns or throws on every path passes");
            check(ok(applyFn + "fn main() {\n  run((n) => {\n    if n > 0 { print(n) }\n  })\n}\n"),
                  "P40 follow-up: a unit-result block lambda needs no return");
            {
                // P40 follow-up: PHP class statics. Only a literal-initialized `static let` is a `const`; any other
                // static is a static property (`K::$x`), a non-literal initializer assigned right after the class.
                EmitResult php = compileStd("fn seed(): i32 => 42\nclass K {\n  static let A: i32 = 7\n  static let B: i32 = seed()\n"
                                            "  static var C: i32 = 1\n}\nfn main() {\n  K.C = K.C + K.B\n  print(K.A + K.C)\n}\n",
                                            findTarget("php"));
                check(php.ok && has(php.code, "const A = 7;") && has(php.code, "public static $B;") &&
                          !has(php.code, "const B") && has(php.code, "public static $C = 1;") &&
                          has(php.code, "K::$B = seed();") && has(php.code, "K::$C = ") && has(php.code, "K::A + K::$C"),
                      "P40 follow-up: PHP `static let Y = f()` is a static property assigned after the class, not `const Y = f();`");
            }
            {
                // P40 follow-up: a bare-name WRITE to a class static resolves like the read, in static and instance
                // methods, and lowers to the qualified access on every target; compound ops too.
                const std::string src = "class K {\n  static var C: i32 = 1\n  static fn up() {\n    C = C + 1\n    C += 2\n  }\n"
                                        "  fn inst() { C = C * 3 }\n}\nfn main() {\n  K.up()\n  K().inst()\n  print(K.C)\n}\n";
                EmitResult cs = compileStd(src, findTarget("csharp"));
                EmitResult ts = compileStd(src, findTarget("typescript"));
                EmitResult py = compileStd(src, findTarget("python"));
                EmitResult php = compileStd(src, findTarget("php"));
                check(cs.ok && has(cs.code, "K.C = K.C + 1;") && has(cs.code, "K.C += 2;") && has(cs.code, "K.C = K.C * 3;"),
                      "P40 follow-up: C# bare static write `C = C + 1` lowers to the static access");
                check(ts.ok && has(ts.code, "K.C = (K.C + 1 | 0);") && has(ts.code, "K.C = (K.C + 2 | 0);"),
                      "P40 follow-up: TS bare static write lowers to `K.C`");
                check(py.ok && has(py.code, "K.C = ((((K.C + 1)") && has(py.code, "K.C = ((((K.C + 2)"),
                      "P40 follow-up: Python bare static write lowers to `K.C`");
                check(php.ok && has(php.code, "K::$C = ((((K::$C + 1)") && has(php.code, "K::$C = ((((K::$C + 2)"),
                      "P40 follow-up: PHP bare static write lowers to the static property `K::$C`");
                // A local of the same name shadows the static for the write as for the read.
                EmitResult sh = compileStd("class K {\n  static var C: i32 = 1\n  static fn f(): i32 {\n    var C = 5\n    C = C + 1\n"
                                           "    return C\n  }\n}\nfn main() { print(K.f()) }\n", findTarget("csharp"));
                check(sh.ok && has(sh.code, "C = C + 1;") && !has(sh.code, "K.C = "),
                      "P40 follow-up: a local shadows a static of the same name for a bare write");
                // Immutable members: `static let` (bare or qualified), `const`, a `let` field outside its own `init`.
                check(refused("class K {\n  static let M: i32 = 7\n  static fn f() { M = M + 1 }\n}\nfn main() { K.f() }\n",
                              "cannot assign to immutable static field 'K.M' (declared with 'static let')"),
                      "P40 follow-up: a bare write to a `static let` is refused");
                check(refused("class K {\n  static let M: i32 = 7\n}\nfn main() { K.M += 1 }\n",
                              "cannot assign to immutable static field 'K.M'"),
                      "P40 follow-up: a qualified write to a `static let` is refused (C# CS0198, a PHP const)");
                check(refused("class K {\n  let y: i32 = 1\n  fn f() { this.y = 2 }\n}\nfn main() { K().f() }\n",
                              "cannot assign to immutable field 'K.y' (declared with 'let') outside a constructor of 'K'"),
                      "P40 follow-up: a `let` field written outside a constructor is refused (C# readonly, CS0191)");
                check(refused("class K {\n  let y: i32 = 1\n}\nfn main() {\n  let k = K()\n  k.y = 2\n}\n",
                              "cannot assign to immutable field 'K.y'"),
                      "P40 follow-up: a `let` field written through another receiver is refused");
                check(ok("class K {\n  let y: i32\n  constructor(v: i32) { this.y = v }\n}\nfn main() { print(K(3).y) }\n"),
                      "P40 follow-up: a `let` field is assigned through `this` in its own constructor");
                check(refused("class K {\n  let y: i32 = 0\n  constructor(v: i32) {\n    let f = () => { this.y = v }\n    f()\n  }\n}\n"
                              "fn main() { print(K(3).y) }\n", "cannot assign to immutable field 'K.y'"),
                      "P40 follow-up: a `let` field cannot be assigned from a lambda inside a constructor (C# CS1673/CS0191)");
            }
            {
                // P40: a compound `t op= v` is the binary op plus a store. C# keeps its native compound (the
                // `nativeCompoundAssign` trait — it narrows back implicitly); TS/Python/PHP expand it so the
                // Binary rule's §3.C wrap, integer division and concatenation apply, with an impure
                // receiver/index evaluated once into a temp, and fold back to `op=` where the rule is plain.
                const std::string src =
                    "import { List } from \"std.collections\"\n"
                    "class P {\n  var n: i32 = 0\n  var big: i64 = 0\n  var s: string = \"\"\n}\n"
                    "fn at(i: i32): i32 => i\nfn pick(p: P): P => p\n"
                    "fn main() {\n  var a: i32 = 1\n  a += 2\n  a /= 2\n  var w: i64 = 1i64\n  w += 2i64\n"
                    "  var b: u8 = 1u8\n  b -= 2u8\n  var f: f64 = 1.0\n  f += 0.5\n  var t = \"x\"\n  t += \"y\"\n"
                    "  var xs: List<i32> = [1, 2]\n  xs[at(0)] += 1\n  let p = P()\n  pick(p).n *= 2\n  p.big += 1\n"
                    "  p.s += \"z\"\n  print(a)\n  print(w)\n  print(b)\n  print(f)\n  print(t)\n  print(xs[0])\n}\n";
                EmitResult cs = compileStd(src, findTarget("csharp"));
                EmitResult ts = compileStd(src, findTarget("typescript"));
                EmitResult py = compileStd(src, findTarget("python"));
                EmitResult php = compileStd(src, findTarget("php"));
                check(cs.ok && has(cs.code, "a += 2;") && has(cs.code, "a /= 2;") && has(cs.code, "b -= 2;") &&
                          has(cs.code, ".at(0)] += 1;") && has(cs.code, ".pick(p).n *= 2;") && has(cs.code, "t += \"y\";") &&
                          has(cs.code, "p.big += 1L;") &&
                          !has(cs.code, "__c"),
                      "P40: C# keeps native compounds (nativeCompoundAssign) — no expansion, no temps");
                check(ts.ok && has(ts.code, "a = (a + 2 | 0);") && has(ts.code, "a = (a / 2 | 0);") &&
                          has(ts.code, "w = BigInt.asIntN(64, w + 2n);") && has(ts.code, "b = (b - 2 & 0xff);") &&
                          has(ts.code, "f += 0.5;") && has(ts.code, "t += \"y\";") && has(ts.code, "p.s += \"z\";") &&
                          has(ts.code, "p.big = BigInt.asIntN(64, p.big + 1n);"),
                      "P40: TS integer compounds wrap like the binary op; float/string compounds fold back to `op=`");
                check(ts.ok && has(ts.code, "const __c0 = at(0);") && has(ts.code, "xs[__c0] = (xs[__c0] + 1 | 0);") &&
                          has(ts.code, "const __c1 = pick(p);") && has(ts.code, "__c1.n = Math.imul(__c1.n, 2);"),
                      "P40: an impure index/receiver of a compound target is evaluated once into a temp");
                check(py.ok && has(py.code, "a = ((((a + 2)") && has(py.code, "_pg_idiv(a, 2)") &&
                          has(py.code, "b = ((b - 2) & 0xff)") && has(py.code, "t += \"y\"") &&
                          has(py.code, "xs[__c0] = ((((xs[__c0] + 1)"),
                      "P40: Python integer compounds wrap and floor-free divide like the binary op");
                check(php.ok && has(php.code, "$a = (((($a + 2)") && has(php.code, "$t = $t . \"y\";") &&
                          has(php.code, "$p->s = $p->s . \"z\";") && !has(php.code, "+= \"") &&
                          has(php.code, "$xs[$__c0] = (((($xs[$__c0] + 1)"),
                      "P40: PHP string compounds concatenate with `.` (a `+=` on a string is a TypeError)");
                // P41 (issue #76) D1/D2: the `__c` allocator is PER BODY and shared with the plugin `fresh`
                // temps — a second function of the same shape restarts at `__c0`/`__c1`, never `__c2`/`__c3`.
                {
                    const std::string two =
                        "import { List } from \"std.collections\"\n"
                        "class P {\n  var n: i32 = 0\n}\n"
                        "fn at(i: i32): i32 => i\nfn pick(p: P): P => p\n"
                        "fn f() {\n  var xs: List<i32> = [1, 2]\n  xs[at(0)] += 1\n  let p = P()\n  pick(p).n *= 2\n}\n"
                        "fn g() {\n  var xs: List<i32> = [1, 2]\n  xs[at(0)] += 1\n  let p = P()\n  pick(p).n *= 2\n}\n";
                    auto countOf = [](const std::string& hay, const std::string& needle) {
                        std::size_t n = 0;
                        for (std::size_t at = hay.find(needle); at != std::string::npos;
                             at = hay.find(needle, at + needle.size())) ++n;
                        return n;
                    };
                    EmitResult ts2 = compileStd(two, findTarget("typescript"));
                    check(ts2.ok && countOf(ts2.code, "const __c0 = at(0);") == 2 &&
                              countOf(ts2.code, "const __c1 = pick(p);") == 2 && !has(ts2.code, "__c2"),
                          "P41 #76: sibling bodies restart the `__c` temps at 0 (TS)");
                    EmitResult py2 = compileStd(two, findTarget("python"));
                    check(py2.ok && countOf(py2.code, "__c0 = at(0)") == 2 && !has(py2.code, "__c2"),
                          "P41 #76: sibling bodies restart the `__c` temps at 0 (Python, one allocator per body)");
                    EmitResult php2 = compileStd(two, findTarget("php"));
                    check(php2.ok && countOf(php2.code, "$__c0 = ") == 2 && !has(php2.code, "__c2"),
                          "P41 #76: sibling bodies restart the `__c` temps at 0 (PHP)");
                }
                check(refused("class P {\n  var n: i32 = 0\n}\nfn main() {\n  let p = P()\n  p.n += 1.5\n}\n",
                              "cannot implicitly convert f64 to i32"),
                      "P40: a compound on a field converts its value into the slot (an f64 into an i32 field is refused)");
            }
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

}
