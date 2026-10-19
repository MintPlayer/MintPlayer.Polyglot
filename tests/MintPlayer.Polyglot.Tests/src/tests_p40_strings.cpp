// P40 slice 8 (part 1): the ordinal string std, index units, module scoping (D16), review r3/r4 (issue #73).
// P41 D14: one translation unit of the unit suite (split from tests_main.cpp so the areas compile in
// parallel). The bodies are unchanged; the shared harness lives in test_support.hpp.

#include "test_support.hpp"

void run_p40_strings() {
    {
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
                        // P40 review r3: the pin checked 4 method names. The other culture-sensitive .NET string
                        // members have no Ordinal overload to require, so they must not appear at all (the
                        // Invariant/Ordinal spellings — ToUpperInvariant, CompareOrdinal — are what std uses).
                        for (const char* banned : {".ToUpper(", ".ToLower(", ".CompareTo(", "string.Compare(", "String.Compare(",
                                                   ".ToString(\"N", "double.Parse(", "float.Parse(", ".ToUpper()", ".ToLower()"})
                            if (s.find(banned) != std::string::npos) {
                                allOrdinal = false;
                                std::cerr << "    culture-sensitive '" << banned << "' in std.strings: " << s << "\n";
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
                      has(rphp.code, "strcmp($a, $b)") && !has(rphp.code, "<=>") && has(rphp.code, "__pg_u16_split($self_, $separator)") &&
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
                // P40 review r3: names mangle per DECLARING module — ai sees one `check`, so it keeps `check`.
                check(ok.ok && has(ok.code, "check(5)") && !has(ok.code, "check$i32") && has(ok.code, "check$string(\"01\")"),
                      "A8: an imported overload competes with the module's own (per-module mangling)");
                // C# links every module into one class. Review r4 #13: a name declared by two modules keeps its plain
                // name when no call could bind both (`check("x")` can never take ai's `check(i32)`)…
                EmitResult cs = compile(leak.substr(0, leak.find("fn main")) + "fn main() { let b = check(\"x\") }\n",
                                        findTarget("csharp"), &res);
                check(cs.ok && has(cs.code, "PolyglotProgram.check(\"x\")") && has(cs.code, "bool check(string") &&
                          !has(cs.code, "check__"),
                      "r4 #13: C# keeps a cross-module name plain without a real clash");
                // …and takes `__`-joined signature link names on a real clash (r3 2(e): show<T> vs ai's show(i32)).
                MapModuleResolver res2{{"./ai", "fn show(n: i32): string => \"ai\"\nfn aiShow(n: i32): string => show(n)\n"}};
                EmitResult cs2 = compile("import { aiShow } from \"./ai\"\nfn show<T>(x: T): string => \"g\"\n"
                                         "fn main() { let a = show(5)\n  let b = aiShow(5) }\n",
                                         findTarget("csharp"), &res2);
                check(cs2.ok && has(cs2.code, "PolyglotProgram.show__T(5)") && has(cs2.code, "string show__T<T>(") &&
                          !has(cs2.code, "PolyglotProgram.show(5)"),
                      "r3 2(e) / r4 #13: C# spells a clashing cross-module name with its signature link name");
            }
            // P40 review r3: every shape `build` used to accept and then miscompile, diverge on, or fail to
            // compile on a target is refused at the source now (each named in its message).
            {
                auto refusesR3 = [&](const std::string& src, const char* frag, const std::string& label) {
                    EmitResult r = compileStd(src, findTarget("csharp"));
                    bool named = false;
                    for (const auto& d : r.diagnostics) if (has(d.message, frag)) named = true;
                    check(!r.ok && named, "r3: " + label);
                };
                refusesR3("fn f(x: i32): i32 => x ?? 0\n", "cannot be null here", "'??' on a non-nullable left operand");
                refusesR3("fn f(t: i32?): i32 {\n  if t != null {\n    return t ?? 0\n  }\n  return 0\n}\n",
                          "cannot be null here", "'??' on a narrowed left operand");
                refusesR3("record P(x: i32)\nfn f(): i32 {\n  var p = P(1)\n  p.x = 5\n  return p.x\n}\n",
                          "cannot assign to record field 'P.x'", "a record field write (2i)");
                refusesR3("open class B {\n  var name: string = \"b\"\n}\nclass D : B {\n  var name: string = \"d\"\n}\n",
                          "redeclares a member inherited from 'B'", "member hiding (2f)");
                refusesR3("open class B {\n  open fn who(): string => \"b\"\n}\nclass D : B {\n  fn who(): string => \"d\"\n}\n",
                          "mark it 'override'", "an open method redeclared without override (2f)");
                refusesR3("class P {\n  var name: string\n  constructor() {\n    let n = this.name\n    this.name = n\n  }\n}\n",
                          "reads field 'name' before assigning it", "a field read before init assigns it (2g)");
                refusesR3("class P {\n  var a: i32\n  constructor() {\n    let f = () => this.a\n    this.a = 1\n  }\n}\n",
                          "uses 'this' before field 'a' is assigned", "a lambda capturing this before init completes (2g)");
                refusesR3("class P {\n  let a: i32 = 1\n  let b: i32 = this.a + 1\n}\n",
                          "the initializer of field 'b' uses 'this'", "this in a field initializer (2g)");
                refusesR3("class A {\n  static let X: i32 = B.Y + 1\n}\nclass B {\n  static let Y: i32 = 2\n}\n",
                          "reads 'B.Y', which is declared later", "a static initializer reading a later static (2h)");
                refusesR3("class C {\n  static let P: i32 = C.Q + 1\n  static let Q: i32 = 2\n}\n",
                          "reads 'C.Q', which is declared later", "a static reading a later static of its class (2h)");
                refusesR3("class G {\n  static let b: Box = Box()\n}\nclass Box {\n  let v: i32 = 1\n}\n",
                          "reads 'Box', which is declared later", "a static initializer constructing a later class (2h)");
                refusesR3("let M: i32 = K + 1\nlet K: i32 = 2\n", "reads 'K', which is declared later",
                          "a module value reading a later one (2h)");
                refusesR3("fn sep(): string => \"/\"\nfn j(a: string, b: string = sep()): string => a + b\n",
                          "must be a literal", "a non-constant default parameter (2j)");
                refusesR3("fn f(a: i32): i32 => a >>> 1\n", "unsigned shift", "'>>>' (item 6)");
                refusesR3("fn f(a: i32): i32 {\n  var x = a\n  x >>>= 1\n  return x\n}\n", "unsigned shift", "'>>>=' (item 6)");
                refusesR3("fn run(f: () => unit) { f() }\nfn g() { run(() => {\n  return 5\n}) }\n",
                          "'return' takes no value", "a value-returning block lambda into () => unit (item 6)");
                refusesR3("fn run(f: () => unit) { f() }\nfn g(k: i32) { run(() => k + 1) }\n",
                          "must be a call or a block", "a value expression lambda into () => unit (item 6)");
                refusesR3("record M(c: i32) {\n  operator fn plus(o: M): M => M(this.c + o.c)\n}\nfn f(): M => M(1).plus(M(2))\n",
                          "'plus' is an operator member of 'M'", "calling an operator fn by name (item 6)");
                refusesR3("fn u(g: (i32) => i32): i32 => g.whatever\n", "has no member 'whatever'",
                          "member access on a function value (item 6)");
                refusesR3("fn f(n: i32): i32 => n(3)\n", "is not a function", "calling a non-function value");
                refusesR3("fn f(): i32 => (() => {\n  return 1\n})()\n", "immediately-invoked lambda",
                          "an untyped immediately-invoked block lambda");
                refusesR3("fn f(): i32 {\n  if true {\n    return 1\n  }\n  return 2\n}\n", "condition is always true",
                          "a constant if condition (CS0162)");
                refusesR3("fn f(): i32 {\n  while 1 == 1 {\n    return 5\n  }\n  return 0\n}\n", "condition is always true",
                          "a constant while condition (CS0162)");
                refusesR3("fn f(x: i32): i32 {\n  var y = x\n  y = y\n  return y\n}\n", "assigns a value to itself",
                          "a self-assignment (CS1717)");
                refusesR3("class K {\n  static var count: i32 = 0\n  fn same(): bool => count == K.count\n}\n",
                          "compares a value with itself", "a bare static compared with itself (CS1718)");
                refusesR3("fn f(n: i32): bool => n == null\n", "never null here", "'== null' on an i32 (CS0472)");
                refusesR3("fn f(): i32 {\n  let unused = 5\n  return 1\n}\n", "is set to a constant but never read",
                          "an unused constant local (CS0219)");
                // A float constant default folds to one literal (C#/PHP accept only constants there).
                EmitResult fold = compileStd("fn g(dt: f64 = 1.0 / 60.0): f64 => dt\n", findTarget("php"));
                check(fold.ok && has(fold.code, "0.016666666666666666"), "r3 2(j): a float constant default folds to a literal");
                // 2(b): a unary operand that is a unary is parenthesized (`--x` is a pre-decrement).
                EmitResult neg = compileStd("fn f(x: i32): i32 => -(-x)\n", findTarget("csharp"));
                check(neg.ok && has(neg.code, "-(-x)") && !has(neg.code, "--x"), "r3 2(b): -(-x) keeps its parentheses");
                // 2(a): a non-name callee is emitted; C# casts an immediately-invoked lambda to its delegate type.
                EmitResult cur = compileStd("fn adder(k: i32): (i32) => i32 => (n: i32) => n + k\n"
                                            "fn f(): i32 => adder(10)(4)\nfn g(): i32 => ((n: i32) => n * 2)(4)\n",
                                            findTarget("csharp"));
                check(cur.ok && has(cur.code, "PolyglotProgram.adder(10)(4)") && has(cur.code, "Func<int, int>)((int n) =>"),
                      "r3 2(a): a curried call and an IIFE keep their callee on C#");
                EmitResult curPhp = compileStd("fn adder(k: i32): (i32) => i32 => (n: i32) => n + k\nfn f(): i32 => adder(10)(4)\n",
                                               findTarget("php"));
                check(curPhp.ok && has(curPhp.code, "adder(10)(4)") && !has(curPhp.code, "$(4)"), "r3 2(a): PHP keeps the callee");
                // item 6: PHP keywords match case-insensitively — a class `Finally` is escaped.
                EmitResult kw = compileStd("class Finally {\n  let x: i32 = 1\n}\nfn f(): i32 => Finally().x\n", findTarget("php"));
                check(kw.ok && has(kw.code, "class Finally_") && has(kw.code, "new Finally_("), "r3 item 6: PHP escapes a class named Finally");
                // item 6: an i64 compound shift keeps an int count on C#.
                EmitResult sh = compileStd("fn f(): i64 {\n  var h: i64 = 1i64\n  h <<= 3\n  return h\n}\n", findTarget("csharp"));
                check(sh.ok && has(sh.code, "h <<= 3;") && !has(sh.code, "3L"), "r3 item 6: an i64 compound shift count stays i32");
                // item 5: `check` runs every configured target's gate (an expect fn without that target's actual).
                const std::string ex = "expect fn sep(): string\nactual(csharp) fn sep(): string => \"/\"\n"
                                       "fn main() { let s = sep() }\n";
                EmitResult chk = checkForTargets(ex, {findTarget("csharp"), findTarget("python")});
                bool namesPy = false;
                for (const auto& d : chk.diagnostics) if (has(d.message, "'python'")) namesPy = true;
                check(!chk.ok && namesPy, "r3 item 5: checkForTargets refuses an expect fn with no actual for a target");
                // A true duplicate across modules names both declarations.
                MapModuleResolver dup{{"./date", "fn check(s: string): bool => s == \"x\"\n"}};
                EmitResult d = compile("import { check } from \"./date\"\nfn check(s: string): bool => s == \"01\"\nfn main() { }\n",
                                       findTarget("csharp"), &dup);
                bool both = false;
                for (const auto& dg : d.diagnostics)
                    if (has(dg.message, "duplicate function 'check'") && has(dg.message, "module")) both = true;
                check(!d.ok && both, "A8: the cross-module duplicate-overload error names both modules");
            }
            // P40 review r4 — nullability / functions / names half: each refusal by message, each fix by emission.
            {
                auto refusesR4 = [&](const std::string& src, const char* frag, const std::string& label) {
                    EmitResult r = compileStd(src, findTarget("csharp"));
                    bool named = false;
                    for (const auto& d : r.diagnostics) if (has(d.message, frag)) named = true;
                    check(!r.ok && named, "r4: " + label);
                };
                auto emitsR4 = [&](const std::string& src, const char* target, std::initializer_list<const char*> frags,
                                   std::initializer_list<const char*> absent, const std::string& label) {
                    EmitResult r = compileStd(src, findTarget(target));
                    bool ok = r.ok;
                    for (const char* f : frags) ok = ok && has(r.code, f);
                    for (const char* f : absent) ok = ok && !has(r.code, f);
                    check(ok, "r4: " + label);
                };
                // 14: nullable -> non-null is refused; narrowing still converts.
                refusesR4("fn f(x: i32?): i32 => x\n", "the value may be null", "a nullable return into i32 (item 14)");
                refusesR4("fn f(x: i32?, y: i32?): i32 {\n  let w: i32 = x ?? y\n  return w\n}\n", "the value may be null",
                          "'x ?? y' with a nullable fallback into i32 (item 14)");
                refusesR4("fn f(): i32 {\n  let n: i32 = null\n  return n\n}\n", "cannot convert null to 'i32'",
                          "a null literal into i32 (item 14)");
                refusesR4("fn g(n: i32): i32 => n\nfn f(x: i32?): i32 => g(x)\n", "the value may be null",
                          "a nullable argument into an i32 parameter (item 14)");
                check(compileStd("fn f(x: i32?): i32 {\n  if x != null {\n    return x\n  }\n  return 0\n}\n",
                                 findTarget("csharp")).ok,
                      "r4: a narrowed nullable converts to i32 (item 14)");
                // 15: a narrowed nullable function value is callable; nullable function types are spelled nullable.
                const std::string cb = "fn run(cb: (() => unit)?) {\n  if cb != null {\n    cb()\n  }\n}\n"
                                       "fn ap(f: ((i32) => i32)?): i32 => f!(1)\n";
                emitsR4(cb, "csharp", {"global::System.Action? cb", "global::System.Func<int, int>? f"}, {},
                        "C# spells a nullable function type with '?' (item 15)");
                emitsR4(cb, "typescript", {"cb: ((() => void) | null)", "f: (((arg0: number) => number) | null)"}, {},
                        "TS parenthesizes a nullable function type (item 15)");
                // 16: `m.get(k)!` drops the absent-normalization (TS2871).
                emitsR4("import \"std.collections\"\nfn g(m: Map<string, i32>, k: string): i32 => m.get(k)!\n", "typescript",
                        {"m.get(k) ?? __pgNullAssert("}, {"?? null) ??"}, "TS m.get(k)! has no inner '?? null' (item 16)");
                // 17: `??=` — refused on a non-null target, native on C#, lowered on Python.
                refusesR4("fn f(): i32 {\n  var y: i32 = 1\n  y ??= 5\n  return y\n}\n", "which cannot be null here",
                          "'??=' on a non-null target (item 17)");
                const std::string ca = "fn f(): i32 {\n  var y: i32? = null\n  y ??= 5\n  return y ?? 0\n}\n";
                emitsR4(ca, "csharp", {"y ??= 5;"}, {}, "C# keeps a native '??=' (item 17)");
                emitsR4(ca, "python", {"if (y is None):"}, {"??="}, "Python lowers '??=' to an if-null store (item 17)");
                // 18: a value-returning named function into `() => unit` is refused; a non-statement unit body discards.
                refusesR4("fn run(f: () => unit) { f() }\nfn five(): i32 => 5\nfn g() { run(five) }\n",
                          "it returns a value the slot would discard", "a value-returning fn into () => unit (item 18)");
                emitsR4("import \"std.strings\"\nfn run(f: () => unit) { f() }\nfn g(s: string) { run(() => s.len()) }\n",
                        "csharp", {"() => { _ = s.Length; }"}, {}, "C# discards a property-read unit body (item 18)");
                emitsR4("import \"std.strings\"\nfn g(s: string) {\n  s.len()\n}\n", "csharp", {"_ = s.Length;"}, {},
                        "C# discards a property-read expression statement (item 18)");
                // 19: a method call on a function value.
                refusesR4("fn adder(k: i32): (i32) => i32 => (n: i32) => n + k\nfn f(): string => adder(1).toString()\n",
                          "has no method 'toString'", "a method call on a function value (item 19)");
                // 20: a named function passed as a value is a callable on PHP.
                emitsR4("fn apply(f: () => i32): i32 => f()\nfn five(): i32 => 5\nfn g(): i32 => apply(five)\n", "php",
                        {"apply(five(...))"}, {"$five"}, "PHP passes a named function as a first-class callable (item 20)");
                refusesR4("fn apply(f: (i32) => i32): i32 => f(1)\nfn o(n: i32): i32 => n\nfn o(s: string): i32 => 0\n"
                          "fn g(): i32 => apply(o)\n", "it is overloaded", "an overloaded function as a value (item 20)");
                // 21: an arrow body that prints starting with `{` is parenthesized on TS.
                emitsR4("fn get(f: () => Option<i32>): i32 => match f() {\n  Some(x) => x,\n  None => -1,\n}\n"
                        "fn g(k: i32): i32 => get(() => Some(k))\n", "typescript", {"get(() => ({"}, {"get(() => {"},
                        "TS parenthesizes an object-literal arrow body (item 21)");
                // 22: runtime names — Python/PHP escape, TS refuses an unqualifiable global, TS qualifies TypeError.
                // (r5: Python no longer escapes it — the output calls `builtins.max`, so the user name stays as written.)
                emitsR4("import \"std.math\"\nfn max(a: i32, b: i32): i32 => a\nfn g(): i32 => max(1, 2) + Math.max(3, 4)\n", "python",
                        {"def max(", "max(1, 2)", "builtins.max(3, 4)", "import builtins"}, {"max_"},
                        "Python keeps a user fn named like a builtin and calls the builtin qualified (item 22, r5)");
                emitsR4("class Directory {\n  let n: i32 = 1\n}\nfn g(): i32 => Directory().n\n", "php",
                        {"class Directory_", "new Directory_("}, {}, "PHP escapes a class named like a builtin class (item 22)");
                emitsR4("fn g(x: i32?): i32 => x!\n", "typescript", {"throw new globalThis.TypeError("}, {},
                        "TS null-assert throws globalThis.TypeError (item 22)");
                {
                    EmitResult r = compileStd("class Math {\n  let n: i32 = 1\n}\n", findTarget("typescript"));
                    check(!r.ok && has(firstErr(r), "'Math' shadows a runtime global of target 'typescript'"),
                          "r4: TS refuses a class shadowing a runtime global it cannot qualify (item 22)");
                }
                // Part 2 (language).
                emitsR4("fn f(): i32 {\n  let _x = 5\n  return 1\n}\n", "csharp", {}, {"_x"},
                        "an unread '_x = <const>' is not emitted (CS0219)");
                refusesR4("fn f(): i32 {\n  var _x = 5\n  _x = 6\n  return 1\n}\n", "is assigned but never read",
                          "an assigned-but-unread '_x' (CS0219)");
                refusesR4("enum Color { Red, Green }\nfn f(): i32 {\n  let c = Color.Red\n  return 1\n}\n",
                          "is set to a constant but never read", "an unused enum-member local (CS0219)");
                refusesR4("fn f(): i32 {\n  let y = (i64)5\n  return 1\n}\n", "is set to a constant but never read",
                          "an unused cast-constant local (CS0219)");
                refusesR4("fn f(): i32 {\n  if \"a\" == \"a\" {\n    return 1\n  }\n  return 2\n}\n", "condition is always true",
                          "a string-literal constant condition (CS0162)");
                refusesR4("fn f(): i32 {\n  if 1 + 1 == 3 {\n    return 1\n  }\n  return 2\n}\n", "condition is always false",
                          "an arithmetic constant condition (CS0162)");
                emitsR4("class H {\n  var f: (i32) => i32 = (x: i32) => x * 3\n}\nfn g(): i32 => H().f(4)\n", "php",
                        {"(new H())->f)(4)"}, {}, "a function-typed field is called through its value (PHP parenthesizes)");
                emitsR4("fn f(): i32 {\n  let a = 1\n  ((x: i32) => x)(8)\n  return a\n}\n", "csharp", {"var a = 1;"}, {"1((", "1("},
                        "a line starting with '(' is a new statement");
                emitsR4("fn f(): i32 => match \"01\" {\n  \"1\" => 1,\n  _ => 2,\n}\n", "typescript", {"const _m: string = \"01\""},
                        {}, "TS annotates a scalar match temp (TS2367)");
                emitsR4("fn inv(x: f64 = -0.0): f64 => x\n", "csharp", {"= -0.0"}, {}, "a -0.0 default keeps its sign");
                emitsR4("fn d(x: f32 = 0.1f + 0.2f): f32 => x\n", "csharp", {"= 0.3f"}, {}, "an f32 constant default folds to f32");
                emitsR4("fn lo(x: i64 = -9223372036854775808i64): i64 => x\n", "php", {"$x = PHP_INT_MIN"}, {},
                        "PHP spells the i64 minimum PHP_INT_MIN");
                refusesR4("fn f(): i32 {\n  let v: i32 = 2147483648\n  return v\n}\n", "does not fit 'i32'",
                          "an i32 slot refuses an out-of-range literal");
                refusesR4("fn f(): i32 {\n  let v = 2147483648\n  return 0\n}\n", "does not fit 'i32'",
                          "an inferred i32 local refuses an out-of-range literal");
            }
            // Review round 6 — init-order/constructors half: a static computed property keeps `static` on every target
            // (its four-target behaviour is pinned by conformance static_computed_property), and a static property
            // SETTER refuses on Python by its capability key (a class-level assignment bypasses any descriptor).
            {
                const std::string sp = "class Cfg {\n  static var raw: i32 = 1\n  static var level: i32 {\n"
                                       "    get => Cfg.raw * 2\n    set(v) { Cfg.raw = v }\n  }\n}\n"
                                       "fn f(): i32 {\n  Cfg.level = 4\n  return Cfg.level\n}\n";
                EmitResult py = compileStd(sp, findTarget("python"));
                bool named = false;
                for (const auto& d : py.diagnostics) if (has(d.message, "does not support propertySetters:static")) named = true;
                check(!py.ok && named, "r6: a static property setter refuses on Python (propertySetters:static)");
                EmitResult cs = compileStd(sp, findTarget("csharp"));
                check(cs.ok && has(cs.code, "public static int level {"), "r6: C# emits a static property with its setter");
                EmitResult ts = compileStd(sp, findTarget("typescript"));
                check(ts.ok && has(ts.code, "static get level()") && has(ts.code, "static set level("),
                      "r6: TS emits a static getter and setter");
                const std::string gp = "class Cfg {\n  static let level: i32 => 3\n}\nfn f(): i32 => Cfg.level\n";
                EmitResult pyg = compileStd(gp, findTarget("python"));
                check(pyg.ok && has(pyg.code, "Cfg.level()") && !has(pyg.code, "@builtins.property"),
                      "r6: Python emits a static property as a static method and calls it");
                EmitResult phg = compileStd(gp, findTarget("php"));
                check(phg.ok && has(phg.code, "Cfg::level()"), "r6: PHP calls a static property's getter");
            }
        }
    }
}
