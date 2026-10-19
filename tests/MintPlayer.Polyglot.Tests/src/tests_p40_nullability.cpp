// P40 slices 5-7: null-assert + nullable rules + narrowing, stringification, generics prerequisites (issue #73).
// P41 D14: one translation unit of the unit suite (split from tests_main.cpp so the areas compile in
// parallel). The bodies are unchanged; the shared harness lives in test_support.hpp.

#include "test_support.hpp"

void run_p40_nullability() {
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

    }
}
