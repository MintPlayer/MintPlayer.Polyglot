// P40 slice 8 (part 2) review r5/r6 refusal+emission pins, runtime-name lists, and slice 9 Map<K, V> (issue #73).
// P41 D14: one translation unit of the unit suite (split from tests_main.cpp so the areas compile in
// parallel). The bodies are unchanged; the shared harness lives in test_support.hpp.

#include "test_support.hpp"

void run_p40_review() {
    {
        {
            // P40 review r5 — nullability / functions half: each refusal by message, each fix by emission; the accepted
            // neighbours compile.
            // P40 review r5 — classes / construction / names / overloads half: each refusal by message, each fix by
            // emission. (The four-target behaviour is pinned by the conformance programs and refusal fixtures.)
            {
                auto refusesR5 = [&](const std::string& src, const char* frag, const std::string& label) {
                    EmitResult r = compileStd(src, findTarget("csharp"));
                    bool named = false;
                    for (const auto& d : r.diagnostics) if (has(d.message, frag)) named = true;
                    check(!r.ok && named, "r5: " + label);
                };
                auto acceptsR5 = [&](const std::string& src, const std::string& label) {
                    check(compileStd(src, findTarget("csharp")).ok, "r5: " + label);
                };
                auto emitsR5 = [&](const std::string& src, const char* target, std::initializer_list<const char*> frags,
                                   std::initializer_list<const char*> absent, const std::string& label) {
                    EmitResult r = compileStd(src, findTarget(target));
                    bool ok = r.ok;
                    for (const char* f : frags) ok = ok && has(r.code, f);
                    for (const char* f : absent) ok = ok && !has(r.code, f);
                    check(ok, "r5: " + label);
                };
                // 18: every slot converts its value.
                refusesR5("fn m(): i32? => null\nfn f(k: i32): i32 => match k {\n  1 => m(),\n  _ => 0,\n}\n",
                          "the value may be null", "a nullable match arm into i32 (item 18)");
                refusesR5("fn f(k: i32): i32 => match k {\n  1 => \"one\",\n  _ => 2,\n}\n", "expected i32, found string",
                          "a mismatched match arm (item 18)");
                refusesR5("fn f(c: bool): i32 => if c { 2 } else { \"x\" }\n", "expected i32, found string",
                          "a mismatched if-expression branch (item 18)");
                refusesR5("fn g(): Iterable<i32> {\n  yield null\n}\n", "(yielded value)", "yield null from Iterable<i32> (item 18)");
                refusesR5("record P(x: i32)\nfn f(p: P): P => p with { x = \"s\" }\n", "(field 'x' of 'with')",
                          "a mistyped with-field (item 18)");
                refusesR5("import \"std.collections\"\nfn f(xs: List<i32>): i32 => xs[\"0\"]\n", "type mismatch (index)",
                          "a string index (item 18)");
                refusesR5("fn m(): i32? => null\nfn f(): (i32, i32) {\n  return (m(), 1)\n}\n", "the value may be null",
                          "a nullable tuple element (item 18)");
                refusesR5("import \"std.collections\"\nfn f(xs: List<i32?>): List<i32> => xs\n", "type arguments must match exactly",
                          "List<i32?> into List<i32> (item 18)");
                refusesR5("fn ap(f: (i32) => i32): i32 => f(3)\nfn g(): i32 => ap((x: string) => 1)\n",
                          "lambda parameter 'x' is declared 'string'", "a mistyped lambda parameter (item 18)");
                refusesR5("fn two(f: (i32, i32) => i32): i32 => f(3, 4)\nfn g(): i32 => two((a: i32) => a)\n",
                          "this lambda takes 1 parameter(s)", "a lambda of the wrong arity (item 18)");
                refusesR5("fn f<T>(x: T = null): T => x\n", "the type parameter 'T' may be a non-null type",
                          "a null default for a T parameter (item 18)");
                refusesR5("fn f<T>(x: T?): T => x\n", "the type parameter 'T' may be a non-null type",
                          "a T? returned as T (item 18)");
                acceptsR5("fn m(): i32? => null\nfn f(k: i32): i32? => match k {\n  1 => m(),\n  _ => 0,\n}\n",
                          "a nullable match arm into i32? (item 18)");
                acceptsR5("fn f(k: i32): f64 => match k {\n  1 => 1,\n  _ => 2.5,\n}\n", "an i32 arm widens into f64 (item 18)");
                acceptsR5("fn m(): i32? => null\nfn f(): i32 {\n  let v = m()\n  if v != null {\n    return match 1 {\n"
                          "      1 => v,\n      _ => 0,\n    }\n  }\n  return 0\n}\n", "a narrowed read in a match arm (item 18)");
                acceptsR5("fn f<T>(x: T?, d: T): T => x ?? d\nfn g<T>(x: T): T? => null\n", "T? fallbacks and a T? slot (item 18)");
                acceptsR5("import \"std.collections\"\nfn f(xs: List<i32?>): List<i32?> => xs\n", "matching nullable type args (item 18)");
                acceptsR5("fn ap(f: (i32?) => i32): i32 => f(null)\nfn g(): i32 => ap(x => (x ?? 0) + 1)\n",
                          "an unannotated lambda parameter takes the slot's type (item 18)");
                // 19: a test inside a lambda narrows a captured parameter / let.
                acceptsR5("fn t(s: string): string => s\nfn r(f: () => string): string => f()\n"
                          "fn v(s: string?): string => r(() => {\n  if s != null {\n    return t(s)\n  }\n  return \"-\"\n})\n",
                          "a captured parameter narrowed inside the lambda (item 19)");
                refusesR5("fn t(s: string): string => s\nfn r(f: () => string): string => f()\n"
                          "fn v(s: string?): string {\n  if s != null {\n    return r(() => t(s))\n  }\n  return \"-\"\n}\n",
                          "the value may be null", "an OUTER narrowing still stops at the lambda (item 19)");
                // 20: `?.` calls keep their `?.`, are typed nullable, and normalize TS's undefined.
                const std::string ns = "class R {\n  var n: i32 = 1\n  fn run() {\n  }\n  fn v(): i32 => 3\n}\n"
                                       "fn f(r: R?): i32 {\n  r?.run()\n  let a: i32? = r?.v()\n  let b: i32? = r?.n\n"
                                       "  return (a ?? 0) + (b ?? 0)\n}\n";
                emitsR5(ns, "csharp", {"if (r != null)", "switch { null => null"}, {}, "C# null-safe call forms (item 20)");
                emitsR5(ns, "typescript", {"if (r !== null)", "(r?.n ?? null)"}, {}, "TS null-safe forms, undefined -> null (item 20)");
                emitsR5(ns, "python", {"if (r is not None):"}, {}, "Python null-safe call statement (item 20)");
                emitsR5(ns, "php", {"if ($r !== null)"}, {}, "PHP null-safe call statement (item 20)");
                refusesR5("class R {\n  fn v(): i32 => 3\n}\nfn f(r: R?): i32 => r?.v()\n", "the value may be null",
                          "r?.v() is nullable (item 20)");
                // 21: a lambda operand keeps its parentheses.
                emitsR5("fn f(cb: (() => i32)?): i32 {\n  let g: () => i32 = cb ?? (() => 9)\n  return g()\n}\n", "csharp",
                        {"cb ?? (() => 9)"}, {}, "C# parenthesizes a lambda '??' operand (item 21)");
                emitsR5("fn f(cb: (() => i32)?): i32 {\n  let g: () => i32 = cb ?? (() => 9)\n  return g()\n}\n", "typescript",
                        {"cb ?? (() => 9)"}, {}, "TS parenthesizes a lambda '??' operand (item 21)");
                // 22: T = unit inference.
                refusesR5("fn ap<T>(f: () => T): T => f()\nfn u() {\n}\nfn g() {\n  ap(() => u())\n}\n",
                          "cannot infer 'T' of 'ap' from a lambda that returns no value", "T inferred as unit (item 22)");
                acceptsR5("fn ap<T>(f: () => T): T => f()\nfn g(): i32 => ap<i32>(() => 5)\n", "an explicit T (item 22)");
                // 23: a plain assignment hoists the target's parts on a valueFirst target only.
                const std::string vf = "import \"std.collections\"\nfn f() {\n  var xs: List<i32> = [1, 2]\n  var j = 0\n"
                                       "  let inc: () => i32 = () => {\n    j = 1\n    return 5\n  }\n  xs[j] = inc()\n}\n";
                emitsR5(vf, "php", {"$__c"}, {"$xs[$j] = $inc()"}, "PHP snapshots a rebindable index (item 23)");
                emitsR5(vf, "python", {"__c"}, {"xs[j] = inc()"}, "Python snapshots a rebindable index (item 23)");
                emitsR5(vf, "csharp", {"xs[j] = inc();"}, {"__c"}, "C# keeps the plain assignment (item 23)");
                emitsR5("import \"std.collections\"\nfn g(): i32 => 1\nfn f(xs: List<i32>, i: i32) {\n  xs[i] = g()\n}\n",
                        "php", {"$xs[$i] = g();"}, {"$__c"}, "PHP leaves a stable index in place (item 23)");
                // Part 2.
                refusesR5("class P {\n  var name: string? = null\n  fn g(): string {\n    if this.name != null {\n"
                          "      return s(this.name)\n    }\n    return \"\"\n  }\n}\nfn s(x: string): string => x\n",
                          "fields are not narrowed by a null test", "the field-narrowing rule is named");
                acceptsR5("open class A {\n}\nclass D : A {\n}\nfn feed(f: (D) => i32): i32 => f(D())\n"
                          "fn g(): i32 {\n  let h: (A) => i32 = (a: A) => 1\n  return feed(h)\n}\n",
                          "a contravariant function value converts");
                refusesR5("fn feed(f: (i32) => i32): i32 => f(1)\nfn g(): i32 {\n  let h: (i64) => i32 = (a: i64) => 1\n"
                          "  return feed(h)\n}\n", "function types must match exactly", "scalar parameters stay exact");
                emitsR5("let later: i32 = 5\nfn mk(): () => i32 => () => later * 2\n", "php", {"$GLOBALS['later']"}, {},
                        "PHP reads a module value live inside a lambda");
                refusesR5("fn f(): i32 {\n  let cb: () => unit = null\n  return 0\n}\n", "declare the slot '(() => unit)?'",
                          "a nullable function slot hint is parenthesized");
                // #4: a class a module value reaches is defined before the value.
                {
                    const std::string o1 = "class Shop {\n  var stock: Item\n  constructor() {\n    this.stock = Item()\n  }\n}\n"
                                           "let shop: Shop = Shop()\nclass Item {\n  var n: i32 = 1\n}\n";
                    EmitResult r = compileStd(o1, findTarget("typescript"));
                    const auto item = r.code.find("class Item"), val = r.code.find("let shop");
                    check(r.ok && item != std::string::npos && val != std::string::npos && item < val,
                          "r5: TS defines a class a module value reaches through a constructor before the value (#4)");
                }
                refusesR5("class Shop {\n  var n: i32\n  constructor() {\n    this.n = later\n  }\n}\n"
                          "let shop: Shop = Shop()\nlet later: i32 = 4\n",
                          "calls 'Shop', which reads 'later', declared later", "a later value read through a constructor (#4)");
                refusesR5("class Config {\n  static let base: string = \"/api\"\n  static let users: string = join(\"u\")\n}\n"
                          "fn join(p: string): string => Config.base + p\n",
                          "while class 'Config' is still being defined", "a static initializer reaching its class from outside (#12)");
                // #5/#6: lambdas and escapes are followed; a class nothing derives from is not checked.
                refusesR5("open class B {\n  var l: string = \"\"\n  constructor() {\n    this.l = (() => this.d())()\n  }\n"
                          "  open fn d(): string => \"b\"\n}\n",
                          "(through a lambda)", "an open call through an in-place lambda in a constructor (#5)");
                refusesR5("fn show(w: W): string => w.d()\nopen class W {\n  var l: string = \"\"\n  constructor() {\n"
                          "    this.l = show(this)\n  }\n  open fn d(): string => \"w\"\n}\n",
                          "(through 'show')", "'this' passed to a function that calls an open method (#5)");
                check(compileStd("open class W {\n  open fn r(): string => \"w\"\n}\nclass B : W {\n  var l: string\n"
                                 "  constructor() {\n    super()\n    this.l = \"\"\n    this.l = this.r()\n  }\n"
                                 "  override fn r(): string => \"b\"\n}\n", findTarget("csharp")).ok,
                      "r5: a class nothing derives from may call its own override in its constructor (#6)");
                // #7: C# assigns a subclass's non-literal field initializers after the base call.
                emitsR5("fn n(): i32 => 1\nopen class A {\n  var a: i32 = n()\n}\nclass B : A {\n  var b: i32 = n()\n  var c: i32 = 3\n}\n",
                        "csharp", {"public int b;", "public int c = 3;", "this.b = PolyglotProgram.n();"}, {},
                        "C# defers a subclass's non-literal field initializer into its constructor (#7)");
                // #8, #10
                refusesR5("open class A {\n  constructor(v: i32) {\n  }\n}\nclass B : A {\n  constructor(n: i32) {\n"
                          "    let d = n * 2\n    super(d)\n  }\n}\n", "a statement before", "a statement before super(...) (#8)");
                refusesR5("open class A {\n  open static fn m(): i32 => 1\n}\n", "cannot be 'open'", "open on a static (#10)");
                // #9: real abstract members.
                {
                    const std::string ab = "abstract class S {\n  abstract fn a(): i32\n}\nclass T : S {\n  override fn a(): i32 => 4\n}\n";
                    emitsR5(ab, "csharp", {"abstract class S", "public abstract int a();", "public override int a()"}, {},
                            "C# emits an abstract class and method (#9)");
                    emitsR5(ab, "typescript", {"abstract class S", "abstract a(): number;"}, {}, "TS emits abstract members (#9)");
                    emitsR5(ab, "python", {"class S(_PG_abc.ABC):", "@_PG_abc.abstractmethod", "import abc as _PG_abc"}, {},
                            "Python emits an abc.ABC class with @abc.abstractmethod (#9)");
                    emitsR5(ab, "php", {"abstract class S", "abstract public function a();"}, {}, "PHP emits abstract members (#9)");
                    refusesR5("abstract class S {\n  abstract fn a(): i32\n}\nfn f(): i32 => S().a()\n",
                              "cannot construct abstract class 'S'", "constructing an abstract class (#9)");
                }
                // #11: omitted record defaults are filled at the construction.
                emitsR5("record P(x: i32, y: i32 = 5)\nfn f(): i32 => P(1).y\n", "csharp", {"new P(1, 5)"}, {},
                        "an omitted record default is filled at the construction (#11)");
                // #13/#14 (one module): declarations alone decide a clash; a lone declaration keeps its name.
                emitsR5("fn same<T>(x: T): T => x\nfn same<U>(x: U, y: i32): U => x\nfn f(): i32 => 1\n", "csharp", {"same<T>(", "same<U>("},
                        {"same__"}, "two declarations with different C# signatures keep plain names (#13)");
                emitsR5("fn d(s: string): string => s\nfn d(s: string?): string => \"n\"\nfn f(): string => d(\"a\")\n", "csharp",
                        {"d__string(string s)", "d__stringOrNull(string? s)"}, {}, "string / string? declarations get distinct link names (#13)");
                // #15
                refusesR5("union Shape {\n  Circle(r: i32)\n  Square(s: i32)\n}\nfn kind(s: Shape): i32 => 1\nfn f(): i32 => kind(5)\n",
                          "cannot convert 'i32' to 'Shape'", "an i32 into a union parameter (#15)");
                // #16
                emitsR5("fn id(n: i32): i32 => n\nfn f(): i32 => id(1)\n", "python", {"def id("}, {"id_"},
                        "Python keeps a user fn 'id' (the output calls builtins.id) (#16)");
                emitsR5("class Box {\n  var id: i32 = 1\n  var len: i32 = 2\n  var str: string = \"s\"\n  fn max(x: i32): i32 => x\n}\n"
                        "fn f(b: Box): i32 => b.id + b.len + b.max(3)\n", "python", {"self.id = 1", "self.len = 2", "self.str = \"s\"",
                        "def max(self, x):", "b.id + b.len"}, {"id_", "len_", "str_", "max_"},
                        "Python keeps fields/methods named like builtins (r5)");
                emitsR5("class Box {\n  var strpos: i32 = 1\n  var error: i32 = 2\n  fn intdiv(fmod: i32): i32 => fmod\n}\n"
                        "class Directory {\n  var n: i32 = 1\n}\nfn f(b: Box): i32 => b.strpos + b.intdiv(2) + Directory().n\n", "php",
                        {"$strpos", "$error", "function intdiv($fmod)", "->intdiv(2)", "class Directory_", "new Directory_("},
                        {"strpos_pg", "error_", "intdiv_pg", "fmod_pg"},
                        "PHP escapes a builtin class name only as a class, a builtin function name only as a free function (r5)");
                emitsR5("fn intdiv(a: i32, b: i32): i32 => a\nfn f(): i32 => intdiv(1, 2)\n", "php", {"function intdiv_pg("}, {},
                        "PHP suffixes a user fn named like a builtin its templates call (#16)");
                emitsR5("class Symbol {\n  var n: i32 = 1\n}\nfn f(): i32 => Symbol().n\n", "typescript", {"class Symbol"}, {},
                        "TS accepts a class named Symbol (no template uses the global) (#16)");
                refusesR5("class Error {\n  var v: i32 = 1\n}\n", "redeclares the std core type 'Error'", "a user class Error (#16)");
                // #17, #24
                check(compileStd("fn c(): i32 => 4\nfn f() {\n  var _last = 0\n  _last = c()\n}\n", findTarget("csharp")).ok,
                      "r5: a '_' local later assigned a call result is accepted (#17)");
                emitsR5("fn f() {\n  let g = () => {\n    let _w = 4\n  }\n  g()\n}\n", "python", {"pass"}, {},
                        "Python gives an elided-only local def a 'pass' (#17)");
                emitsR5("import \"std.strings\"\nfn f(_: bool, s: string): bool {\n  s.len()\n  return _\n}\n", "csharp",
                        {"bool __u", "_ = s.Length;", "return __u;"}, {}, "C# respells a user '_' so the discard cannot bind it (#24)");
                // Part 2.
                refusesR5("fn five(): i32 => 5\nfn f() {\n  five\n}\n", "names a function but does not call it",
                          "a bare function name as a statement");
                emitsR5("class point {\n  var x: i32 = 1\n}\n", "csharp", {"#pragma warning disable CS8981"}, {},
                        "C# disables CS8981 for an all-lowercase type name");
                emitsR5("class Point {\n  var x: i32 = 1\n}\n", "csharp", {}, {"CS8981"},
                        "C# emits no CS8981 pragma without an all-lowercase type name");
                check(compileStd("let twice: (i32) => i32 = (x: i32) => x * 2\nfn f(): i32 => twice(4)\n", findTarget("csharp")).ok,
                      "r5: a module value of function type is callable by name");
                {
                    const std::string ue = "record Pt(x: i32)\nunion S {\n  Dot(at: Pt)\n  E\n}\nfn f(): bool => Dot(Pt(1)) == Dot(Pt(1))\n";
                    emitsR5(ue, "typescript", {"function __pgUnionEq(", "__pgUnionEq("}, {"JSON.stringify"},
                            "TS compares union values through __pgUnionEq (not JSON.stringify)");
                    emitsR5(ue, "php", {"function __pg_ueq(", "__pg_ueq("}, {}, "PHP compares union values through __pg_ueq");
                }
            }
            // P40 review r5 #16: each target's runtime-name lists cover every identifier its templates and prelude
            // reference unqualified (a call `x(`, `new X`, or the head of `X.` / `X::`) — so a user name can never shadow
            // what the generated code calls, and the lists cannot drift again. Comments and string literals are skipped;
            // the allowlist holds names the templates DECLARE themselves (helper locals, the runtime class's own helpers,
            // a record's `equals`), contextual keywords, and `main` (the user's own entry function).
            {
                const std::map<std::string, std::set<std::string>> allow = {
                    {"csharp", {"Equals", "GetHashCode", "ReferenceEquals", "LowerAscii", "UpperAscii", "Show", "Str",
                                "a", "dg", "digits", "fm", "s", "sb", "x", "global", "var", "when", "main"}},
                    {"typescript", {"as", "catch", "for", "if", "super", "constructor", "ka"}},
                    {"python", {"a", "b", "d", "m", "n", "r", "s", "x", "cache", "ctypes", "super"}},
                    {"php", {"equals"}},
                };
                // Python: the output qualifies every builtin it uses (`builtins.len`, the `builtins` module itself the one
                // escaped name), so a user `fn max`, field `id` or param `len` keeps its name. No builtin may appear bare —
                // called, passed (`isinstance(x, bool)`) or as a decorator — except `super` (a .pg keyword: never a user
                // name, and zero-argument `super()` must be spelled bare).
                const std::set<std::string> pyBuiltins = {
                    "abs", "all", "any", "bin", "bool", "bytearray", "bytes", "callable", "chr", "classmethod", "complex",
                    "dict", "dir", "divmod", "enumerate", "filter", "float", "format", "frozenset", "getattr", "hasattr",
                    "hash", "hex", "id", "int", "isinstance", "issubclass", "iter", "len", "list", "map", "max", "min",
                    "next", "object", "oct", "open", "ord", "pow", "print", "property", "range", "repr", "reversed",
                    "round", "set", "setattr", "slice", "sorted", "staticmethod", "str", "sum", "tuple", "type", "vars",
                    "zip", "TypeError", "ValueError", "ZeroDivisionError", "KeyError", "IndexError", "StopIteration"};
                // The TS lib's global types (lib.es*.d.ts). A template may spell one only if a user type can never
                // take its name: it is in the identifier lists (refused), or it is a Polyglot core/std type of the same
                // name (`tsPgTypes`) — a user type cannot redeclare those (`linkCoreModule`, the flat std namespace).
                const std::set<std::string> tsLibTypes = {
                    "Record", "Partial", "Required", "Readonly", "Pick", "Omit", "Exclude", "Extract", "NonNullable",
                    "ReturnType", "Parameters", "InstanceType", "Awaited", "NoInfer", "ThisType", "Uppercase",
                    "Lowercase", "Capitalize", "Uncapitalize", "PropertyKey", "ArrayLike", "Map", "Set", "WeakMap",
                    "WeakSet", "Array", "ReadonlyArray", "Iterable", "Iterator", "IterableIterator", "Generator",
                    "AsyncGenerator", "AsyncIterable", "AsyncIterator", "Promise", "PromiseLike", "Error", "TypeError",
                    "RangeError", "Symbol", "Object", "Function", "Date", "RegExp", "BigInt", "Number", "String",
                    "Boolean", "TemplateStringsArray"};
                const std::set<std::string> tsPgTypes = {"Map", "Iterable", "Error"};
                for (const char* t : {"csharp", "typescript", "python", "php"}) {
                    std::string text;
                    cli::readFile(cli::executablePath().parent_path() / "plugins" / t / "polyglot-plugin.json", text);
                    std::string err;
                    const json::Value doc = json::parse(text, err);
                    const json::Value& ids = doc["spec"]["identifiers"].isNull() ? doc["identifiers"] : doc["spec"]["identifiers"];
                    const bool ci = ids["caseInsensitive"].asBool();
                    auto key = [&](std::string n) {
                        if (ci) for (char& c : n) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                        return n;
                    };
                    std::set<std::string> covered;
                    for (const char* list : {"keywords", "reserved", "runtimeNames", "globals", "classBuiltins"})
                        for (const auto& v : ids[list].items()) covered.insert(key(v.asString()));
                    for (const auto& v : ids["functionBuiltins"]["names"].items()) covered.insert(key(v.asString()));
                    std::vector<std::string> strings;
                    std::function<void(const json::Value&)> walk = [&](const json::Value& v) {
                        if (v.kind == json::Value::Kind::String) strings.push_back(v.asString());
                        for (const auto& a : v.items()) walk(a);
                        for (const auto& [k, m] : v.members) if (k != "identifiers") walk(m);
                    };
                    walk(doc);
                    auto idc = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
                    std::set<std::string> missing;
                    for (std::string s : strings) {
                        bool path = !s.empty();
                        for (char c : s) if (!(idc(c) || c == '.' || c == '#')) path = false;
                        if (path) continue; // a template path (`decl.name`), not code
                        std::string code; // comments and string literals removed
                        for (std::size_t i = 0; i < s.size(); ++i) {
                            const bool lineStart = i == 0 || s[i - 1] == '\n' || s[i - 1] == ' ';
                            if (lineStart && (s.compare(i, 3, "// ") == 0 || s.compare(i, 2, "# ") == 0)) {
                                while (i < s.size() && s[i] != '\n') ++i;
                                code += '\n';
                                continue;
                            }
                            if (s[i] == '"' || s[i] == '\'') {
                                const char q = s[i];
                                for (++i; i < s.size() && s[i] != q; ++i) if (s[i] == '\\') ++i;
                                code += " \"\" ";
                                continue;
                            }
                            code += s[i];
                        }
                        for (std::size_t i = 0; i < code.size(); ++i) {
                            if (!(std::isalpha(static_cast<unsigned char>(code[i])) || code[i] == '_')) continue;
                            if (i > 0 && (idc(code[i - 1]) || code[i - 1] == '$' || code[i - 1] == '.' || code[i - 1] == '>' ||
                                          code[i - 1] == ':' || code[i - 1] == '\\')) {
                                while (i < code.size() && idc(code[i])) ++i;
                                continue;
                            }
                            std::size_t j = i;
                            while (j < code.size() && idc(code[j])) ++j;
                            const std::string name = code.substr(i, j - i);
                            std::size_t k = j;
                            while (k < code.size() && code[k] == ' ') ++k;
                            const bool use = k < code.size() &&
                                             (code[k] == '(' || code.compare(k, 2, "::") == 0 ||
                                              (code[k] == '.' && k + 1 < code.size() && std::isalpha(static_cast<unsigned char>(code[k + 1]))));
                            if (use && name[0] != '_' && name != "new" && !covered.count(key(name)) && !allow.at(t).count(name))
                                missing.insert(name);
                            const bool defined = i >= 4 && code.compare(i - 4, 4, "def ") == 0; // the template's own def
                            if (std::string(t) == "python" && pyBuiltins.count(name) && !defined)
                                missing.insert(name + "(bare)");
                            // P40 review r6 #7: a TS lib TYPE spelled unqualified in a type position (`as Record<…>`)
                            // is shadowed by a user type of that name (TS2315) — calls alone did not cover it.
                            if (std::string(t) == "typescript" && tsLibTypes.count(name) && !covered.count(name) &&
                                !tsPgTypes.count(name))
                                missing.insert(name + "(type)");
                            i = j;
                        }
                    }
                    std::string list;
                    for (const auto& m : missing) list += " " + m;
                    check(err.empty() && missing.empty(),
                          std::string("r5: every unqualified runtime reference in the ") + t +
                              " manifest is in its identifier lists (uncovered:" + list + ")");
                }
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
            // P40 review r6 — records / names / python half: each fix by emission, each refusal by message, and the
            // accepted neighbours. (Four-target behaviour: the conformance programs and the refusal fixtures.)
            {
                auto refusesR6 = [&](const std::string& src, const char* target, const char* frag, const std::string& label) {
                    EmitResult r = compileStd(src, findTarget(target));
                    bool named = false;
                    for (const auto& d : r.diagnostics) if (has(d.message, frag)) named = true;
                    check(!r.ok && named, "r6: " + label);
                };
                auto acceptsR6 = [&](const std::string& src, const char* target, const std::string& label) {
                    check(compileStd(src, findTarget(target)).ok, "r6: " + label);
                };
                auto emitsR6 = [&](const std::string& src, const char* target, std::initializer_list<const char*> frags,
                                   std::initializer_list<const char*> absent, const std::string& label) {
                    EmitResult r = compileStd(src, findTarget(target));
                    bool ok = r.ok;
                    for (const char* f : frags) ok = ok && has(r.code, f);
                    for (const char* f : absent) ok = ok && !has(r.code, f);
                    check(ok, "r6: " + label);
                };
                // #6: a default filled in at a construction site in another module imports the default's own types.
                {
                    MapModuleResolver res{{"./paint", "enum Color { Red, Blue }\nrecord P(x: i32, c: Color = Color.Blue)\n"},
                                          {"./tones", "enum Tone { Soft, Loud }\n"},
                                          {"./vol", "import { Tone } from \"./tones\"\nrecord Q(n: i32, t: Tone = Tone.Loud)\n"}};
                    const std::string prog = "import { P } from \"./paint\"\nimport { Q } from \"./vol\"\n"
                                             "fn main() {\n  let p = P(1)\n  let q = Q(2)\n}\n";
                    EmitResult t = compile(prog, findTarget("typescript"), &res);
                    check(t.ok && has(t.code, "import { P, Color } from \"./paint\";") &&
                              has(t.code, "import { Tone } from \"./tones\";"),
                          "r6 #6: TS imports the types a filled-in default names (its own module's and a third module's)");
                    EmitResult p = compile(prog, findTarget("python"), &res);
                    check(p.ok && has(p.code, "from paint import P, Color") && has(p.code, "from tones import Tone"),
                          "r6 #6: Python imports the types a filled-in default names");
                    // a name already imported is not imported twice; an explicitly passed field needs no import
                    const std::string both = "import { P, Color } from \"./paint\"\nfn main() {\n  let p = P(1, Color.Red)\n}\n";
                    EmitResult t2 = compile(both, findTarget("typescript"), &res);
                    check(t2.ok && has(t2.code, "import { P, Color } from \"./paint\";") && !has(t2.code, "Color, Color"),
                          "r6 #6: a name the source already imports is not added again");
                }
                // #7: the TS union-equality helper spells no TS lib type a user type could shadow.
                emitsR6("union S {\n  A(x: i32)\n  B\n}\nclass Record {\n  var id: i32 = 1\n}\nfn f(): bool => A(1) == A(1)\n",
                        "typescript", {"{ [k: string]: unknown }"}, {"Record<"}, "#7 __pgUnionEq uses an index-signature type");
                // #8: signature link names are structural, so differing erased signatures never share a name.
                emitsR6("fn pairOf(p: (string, i32)): i32 => 1\nfn pairOf(p: (string?, i32)): i32 => 2\n", "csharp",
                        {"pairOf____tuple__string__and__i32__end(", "pairOf____tuple__stringOrNull__and__i32__end("},
                        {"pairOf__x("}, "#8 tuple parameters spell their elements (and nested nullability)");
                emitsR6("fn runIt(f: (string) => i32): i32 => 1\nfn runIt(f: (string?) => i32): i32 => 2\n", "csharp",
                        {"runIt____fn__string__to__i32__end(", "runIt____fn__stringOrNull__to__i32__end("}, {"runIt__x("},
                        "#8 function-typed parameters spell their signature");
                emitsR6("fn pairOf(p: (string, i32)): i32 => 1\nfn pairOf(p: (string?, i32)): i32 => 2\n", "php",
                        {"function pairOf____tuple__string__and__i32__end(", "function pairOf____tuple__stringOrNull__and__i32__end("},
                        {}, "#8 PHP's unique names differ for tuple parameters (one function namespace)");
                emitsR6("class A_B {\n}\nclass A {\n}\nclass B {\n}\nfn f(x: A_B): i32 => 1\nfn f(x: A, y: B): i32 => 2\n",
                        "typescript", {"function f$A$uB(", "function f$A_B("}, {}, "#8 a type name's own '_' cannot meet the parameter separator");
                emitsR6("fn describe(s: string): string => s\nfn describe(s: string?): string => \"n\"\n", "csharp",
                        {"describe__string(", "describe__stringOrNull("}, {}, "#8 the common forms keep their readable spelling");
                // #9: T = unit from ANY function value that returns nothing; a value-returning one stays accepted.
                refusesR6("fn apply<T>(f: () => T): T => f()\nfn hello() {\n}\nfn g() {\n  apply(hello)\n}\n", "csharp",
                          "from a function value that returns no value", "#9 a named unit function binding T = unit is refused");
                refusesR6("fn apply<T>(f: () => T): T => f()\nfn g() {\n  let h: () => unit = () => g()\n  apply(h)\n}\n",
                          "csharp", "from a function value that returns no value", "#9 a () => unit local binding T = unit is refused");
                acceptsR6("fn apply<T>(f: () => T): T => f()\nfn five(): i32 => 5\nfn g(): i32 => apply(five)\n", "csharp",
                          "#9 a named value-returning function still infers T");
                acceptsR6("fn apply<T>(f: () => T): T => f()\nfn g(): i32 {\n  let h: () => i32 = () => 4\n  return apply(h)\n}\n",
                          "csharp", "#9 a value-returning function local still infers T");
                acceptsR6("fn run(f: () => unit) {\n  f()\n}\nfn hello() {\n}\nfn g() {\n  run(hello)\n}\n", "csharp",
                          "#9 a unit function into a non-generic () => unit slot is accepted");
                // #10: every template context records its prelude requirement — each Python shape alone in a module.
                const struct { const char* src; const char* shape; } pyShapes[] = {
                    {"class Fmt {\n  static fn show(): string => \"s\"\n}\n", "@builtins.staticmethod"},
                    {"class Box {\n  var v: string = \"p\"\n  let label: string => this.v\n}\n", "@builtins.property"},
                    {"fn total(n: i32): i32 {\n  var s = 0\n  for i in 0..n {\n    s += i\n  }\n  return s\n}\n", "builtins.range("},
                    {"class Oops : Error {\n  constructor(m: string) {\n    super(m)\n  }\n}\nfn g(k: i32): i32 {\n  try {\n    if k > 5 {\n"
                     "      throw Oops(\"x\")\n    }\n  } catch (e: Oops) when (k > 0) {\n    return 1\n  }\n  return 0\n}\n",
                     "builtins.isinstance(__e"},
                };
                for (const auto& sh : pyShapes) {
                    EmitResult r = compileStd(sh.src, findTarget("python"));
                    check(r.ok && has(r.code, sh.shape) && has(r.code, "import builtins\n"),
                          std::string("r6 #10: a Python module using `builtins` only through ") + sh.shape + " imports it");
                }
                // ...and statically: every Python manifest string that spells `builtins.` sits in a rule that requires
                // `pyBuiltins` (a template array holding the require), a binding that lists it, or a prelude that
                // `preludeRequires` it.
                {
                    std::string text;
                    cli::readFile(cli::executablePath().parent_path() / "plugins" / "python" / "polyglot-plugin.json", text);
                    std::string err;
                    const json::Value doc = json::parse(text, err);
                    std::set<std::string> uncovered;
                    auto isReq = [](const json::Value& v) {
                        return v.kind == json::Value::Kind::Object && v["fn"].asString() == "require" &&
                               !v["args"].items().empty() && v["args"].items()[0].asString() == "pyBuiltins";
                    };
                    std::function<void(const json::Value&, bool, const std::string&)> walk =
                        [&](const json::Value& v, bool covered, const std::string& where) {
                            if (v.kind == json::Value::Kind::String) {
                                if (!covered && has(v.asString(), "builtins.")) uncovered.insert(where);
                                return;
                            }
                            bool arrCovered = covered;
                            for (const auto& a : v.items()) if (isReq(a)) arrCovered = true;
                            for (const auto& a : v.items()) walk(a, arrCovered, where);
                            bool objCovered = covered;
                            for (const auto& r : v["requires"].items()) if (r.asString() == "pyBuiltins") objCovered = true;
                            for (const auto& [k, m] : v.members) {
                                if (k == "identifiers") continue;
                                walk(m, objCovered, where.empty() ? k : where + "." + k);
                            }
                        };
                    const json::Value& spec = doc["spec"].isNull() ? doc : doc["spec"];
                    for (const auto& [k, m] : doc.members) {
                        if (k == "spec" || k == "preludes") continue;
                        walk(m, false, k);
                    }
                    for (const auto& [k, m] : spec.members) {
                        if (k == "preludes" || k == "identifiers") continue;
                        if (&spec != &doc) walk(m, false, "spec." + k);
                    }
                    const json::Value& preludes = spec["preludes"].isNull() ? doc["preludes"] : spec["preludes"];
                    const json::Value& preqs = spec["preludeRequires"].isNull() ? doc["preludeRequires"] : spec["preludeRequires"];
                    for (const auto& [k, m] : preludes.members) {
                        bool req = k == "pyBuiltins";
                        for (const auto& r : preqs[k].items()) if (r.asString() == "pyBuiltins") req = true;
                        if (!req && m.kind == json::Value::Kind::String && has(m.asString(), "builtins.")) uncovered.insert("preludes." + k);
                    }
                    std::string list;
                    for (const auto& u : uncovered) list += " " + u;
                    check(err.empty() && uncovered.empty(),
                          "r6 #10: every Python template spelling `builtins.` requires pyBuiltins (uncovered:" + list + ")");
                }
                // Part 2 — TS: a class named `Object` is refused (TS2725 under nodenext); its other bindings are fine.
                refusesR6("class Object {\n  var v: i32 = 2\n}\n", "typescript", "class name 'Object' is refused on target 'typescript'",
                          "a class named Object is refused on TS (TS2725)");
                refusesR6("record Object(v: i32)\n", "typescript", "TS2725", "a record named Object is refused on TS");
                acceptsR6("class Object {\n  var v: i32 = 2\n}\n", "csharp", "a class named Object stays accepted on C#");
                acceptsR6("enum Object { A }\nclass H {\n  var Object: i32 = 1\n}\nclass Symbol {\n}\nfn f(): i32 {\n  let Object = 2\n  return Object\n}\n",
                          "typescript", "an enum, field, local named Object and a class Symbol stay accepted on TS");
                // Part 2 — TS: an extension sharing a name is emitted under its own free-function name.
                emitsR6("extension fn i32.twice(): i32 => this * 2\nlet twice = 7\nfn f(): i32 => 3.twice() + twice\n",
                        "typescript", {"function twice$i32(", "twice$i32(3)", "let twice"}, {"function twice("},
                        "an extension named like a module value becomes twice$i32 on TS");
                emitsR6("extension fn i32.twice(): i32 => this * 2\nlet twice = 7\nfn f(): i32 => 3.twice() + twice\n",
                        "csharp", {"twice(this int self)", "3.twice()"}, {"twice__i32"}, "C# keeps the extension method's name");
                emitsR6("extension fn i32.twice(): i32 => this * 2\nfn f(): i32 => 3.twice()\n", "typescript",
                        {"function twice(", "twice(3)"}, {"$i32"}, "a lone extension keeps its plain name");
                // Part 2 — TS: a `null` initializer is widened to the declared type (tsc narrows `let x: T | null = null`).
                emitsR6("fn f(): string {\n  let t: string? = null\n  return t ?? \"n\"\n}\n", "typescript",
                        {"const t: (string | null) = null as (string | null);"}, {}, "TS widens a null initializer");
                emitsR6("fn f(): string {\n  let t: string? = null\n  return t ?? \"n\"\n}\n", "csharp", {"string? t = null;"},
                        {" as "}, "C# keeps the bare null initializer");
                // Part 2 — hints follow the slot kind; a nullable no-parameter function type is inferable.
                acceptsR6("fn pick(): (() => unit)? => null\nfn f() {\n  let cb = pick()\n  if cb != null {\n    cb()\n  }\n}\n",
                          "csharp", "let cb = pick() with a (() => unit)? result is inferable");
                {
                    EmitResult r = compileStd("fn f() {\n  var xs = []\n}\n", findTarget("csharp"));
                    bool hint = false;
                    for (const auto& d : r.diagnostics) if (has(d.message, "'var xs: List<i32> = []'")) hint = true;
                    check(!r.ok && hint, "r6: an empty list literal's hint is a list annotation");
                }
                // Part 2 — i32 into a NULLABLE wider slot widens (TS BigInt); narrowing into one needs a cast.
                emitsR6("fn f(a: i32): i64? => a\n", "typescript", {"BigInt(a)"}, {}, "an i32 into i64? widens on TS");
                emitsR6("fn f(c: bool, a: i32): i64? => if c { a } else { null }\n", "typescript", {"BigInt(a)"}, {},
                        "an i32 arm into an i64? return widens on TS");
                refusesR6("fn f(b: i64): i32? => b\n", "csharp", "cannot implicitly convert i64 to i32?", "i64 into i32? needs a cast");
                acceptsR6("fn f(): u8? => 5\nfn g(): i64? => 5\nfn h(): f64? => 1\n", "csharp", "literals into nullable numeric slots stay accepted");
                // Part 2 — overloads differing only in nullability: a maybe-null argument binds the nullable member.
                emitsR6("fn show(s: string): string => s\nfn show(s: string?): string => \"n\"\nfn mk(): string? => null\n"
                        "fn f(): string => show(mk()) + show(null) + show(\"x\")\n", "typescript",
                        {"show$stringOrNull(mk())", "show$stringOrNull(null)", "show$string(\"x\")"}, {}, "nullable arguments bind the nullable overload");
                // Part 2 — `==` on function values is refused; the null test stays accepted.
                refusesR6("fn f(): bool {\n  let k: () => i32 = () => 1\n  return k == (() => 1)\n}\n", "csharp",
                          "'==' on a function value is refused", "== on function values is refused");
                acceptsR6("fn pick(): (() => i32)? => null\nfn f(): bool {\n  let k = pick()\n  return k != null\n}\n", "csharp",
                          "a function value's null test stays accepted");
                // Part 2 — PHP concatenates operands the lambda's slot types as strings.
                emitsR6("fn mapS(f: (string) => string): string => f(\"q\")\nfn g(): string => mapS(s => s + \"!\")\n", "php",
                        {"$s . \"!\""}, {"$s + \"!\""}, "PHP concatenates a slot-typed lambda parameter");
                emitsR6("fn g(): string {\n  let f = () => \"x\"\n  return f() + \"|\" + \"z\"\n}\n", "php", {"$f() . \"|\" . \"z\""}, {},
                        "PHP concatenates a typed lambda local's result");
                emitsR6("fn f(): string => \"top\"\nfn k(): string {\n  let f = () => \"loc\"\n  return f()\n}\n", "csharp",
                        {"return f();"}, {"PolyglotProgram.f()"}, "a local lambda shadowing a function is called through its value");
                // Part 2 — C#: a generic T? compared with null is a presence test (no `(T?)` switch arms: CS8506).
                emitsR6("fn has<T>(a: T?): bool => a != null\n", "csharp", {"=> true", "=> false"}, {"(T?)"},
                        "a generic T? null test matches on the case");
                // Part 2 — Python parenthesizes a lambda operand of a conditional expression.
                emitsR6("fn pick(b: bool): (() => i32)? => if b { () => 1 } else { null }\n", "python",
                        {"((lambda : 1) if b else None)"}, {}, "Python wraps a lambda before ` if `");
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
            check(py.ok && has(py.code, "(m.pop(\"a\", _PG_ABSENT) is not _PG_ABSENT)") && has(py.code, "_PG_ABSENT = builtins.object()") &&
                      has(py.code, "builtins.list(m.keys())"),
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

}
