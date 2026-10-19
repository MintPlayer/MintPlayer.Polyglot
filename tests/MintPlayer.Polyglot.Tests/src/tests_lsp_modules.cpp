// P16 semantic model, P38 slices 1-2, P8 std, P12 module resolution, P13 prelude/bindings, P10, P14 unions/Option, issue #27.
// P41 D14: one translation unit of the unit suite (split from tests_main.cpp so the areas compile in
// parallel). The bodies are unchanged; the shared harness lives in test_support.hpp.

#include "test_support.hpp"

void run_lsp_modules() {
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

    // P38/issue #69 slice 1 — compile() stamps real fileIds on the BUILD path (it passed a null SourceMap
    // before, so every token of every module was fileId 0 and a diagnostic from an imported module was
    // reported under the ENTRY file's name). The entry keeps id 1; the LSP's `fileId != 1` filter and this
    // test both depend on that invariant.
    {
        MapModuleResolver resolver({ {"./helper", "fn helperValue(): i32 => nope(1)\n"} });
        const char* prog =
            "import { helperValue } from \"./helper\"\n"
            "fn main() {\n"
            "  print(helperValue())\n"
            "}\n";
        EmitResult r = compile(prog, findTarget("csharp"), &resolver, LibConfig{{"io"}}, "entry.pg");
        check(!r.ok && !r.diagnostics.empty(), "P38 s1: an error inside an imported module still fails the build");
        const Diagnostic* d = r.diagnostics.empty() ? nullptr : &r.diagnostics.front();
        check(d && d->pos.fileId > 1, "P38 s1: the diagnostic carries the IMPORTED module's fileId, not 0");
        check(d && r.sources.canon(d->pos.fileId) == "./helper",
              "P38 s1: EmitResult::sources names the module the diagnostic came from");
        check(r.sources.canon(1) == "entry.pg", "P38 s1: the entry keeps fileId 1 (LSP filter invariant)");
    }
    {   // An entry-only error stays fileId 1, so single-file diagnostics are unchanged.
        EmitResult r = compile("fn main() { nope(1) }\n", findTarget("csharp"), nullptr, LibConfig{{"io"}}, "solo.pg");
        check(!r.ok && !r.diagnostics.empty() && r.diagnostics.front().pos.fileId == 1,
              "P38 s1: an entry-file diagnostic is fileId 1");
    }
    {   // No entryPath given (the historical call shape): still id 1, named "<entry>", never a crash.
        EmitResult r = compile("fn main() { print(1) }\n", findTarget("csharp"), nullptr, LibConfig{{"io"}});
        check(r.ok && r.sources.canon(1) == "<entry>", "P38 s1: an unnamed entry is stamped '<entry>'");
    }

    // P38/issue #69 slice 2 — ir::Method carries its declaration position, so the emitter can attribute a
    // method-entry sequence point to the .pg line (SP1 measured that this is what makes the declaration
    // line register a coverage hit). ClassField/Global need no equivalent: their `init` is an ir::Expr,
    // which is already positioned.
    {
        const char* prog =
            "class Counter {\n"                       // line 1
            "  var n: i32 = 7\n"                      // line 2: field initializer (an ir::Expr, positioned)
            "  fn bump(): i32 {\n"                    // line 3: the method declaration
            "    return this.n\n"
            "  }\n"
            "}\n"
            "fn main() {}\n"; // no std lib on this direct lex/parse/check path — `print` would be undeclared
        DiagnosticBag d;
        auto unit = parse(lex(prog, d, 1), d); // fileId 1 = the entry, as compile()/analyze() stamp it
        mintplayer::polyglot::check(unit, d);
        check(!d.hasErrors(), "P38 s2: the position-carrying program type-checks");
        ir::Module m = lower(unit, "csharp");
        const ir::Class* c = nullptr;
        for (const auto& cl : m.classes) if (cl.name == "Counter") c = &cl;
        check(c != nullptr, "P38 s2: the test class lowers");
        const ir::Method* bump = nullptr;
        if (c) for (const auto& mm : c->methods) if (mm.name == "bump") bump = &mm;
        check(bump && bump->pos.line == 3, "P38 s2: a lowered method carries its declaration line");
        check(bump && bump->pos.fileId == 1, "P38 s2: ... stamped with the entry's fileId");
        const ir::ClassField* f = nullptr;
        if (c) for (const auto& ff : c->fields) if (ff.name == "n") f = &ff;
        check(f && f->init && f->init->pos.line == 2,
              "P38 s2: a field initializer's origin comes from its already-positioned ir::Expr");
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
    resolves("open class Base {\n  let id: i32\n  constructor() { this.id = 0 }\n}\n"
             "class Sub : Base {\n  constructor() { super() }\n}\n"
             "fn f(s: Sub): i32 => s.id\n", "P13: a subclass reaches an inherited base member");

    // P13 follow-up — `Error.message`: resolves on an `: Error` subclass (base walk) and binds per target
    // (C# `.Message` on System.Exception vs JS `.message`), since `Error` isn't a source `extern class`.
    {
        const char* prog =
            "class MyErr : Error {\n  constructor(message: string) { super(message) }\n}\n"
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
            "  constructor(n: i32) {\n    actual(csharp)     extern(\"new $T($0)\")\n    actual(typescript) extern(\"new $T($0)\")\n  }\n"
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
                    && has(ts.code, "{ tag: \"Full\" as const, value: 5 }"),
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
        check(ts.ok && has(ts.code, "{ tag: \"Some\" as const, value: v }") && has(ts.code, "{ tag: \"None\" as const }"),
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
            "class Node {\n  var next: List<Node?>\n  constructor() { this.next = List<Node?>() }\n}\n"
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
        acceptsStd("fn f(): i32? { var x: i32? = null\n  return x }\n", "issue#27 Bug1: null WITH annotation is accepted");
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
        EmitResult tn = compileStd("class Node {\n  var v: i32\n  constructor() { this.v = 0 }\n}\n"
                                   "fn f(): Node? { var x: Node? = null\n  x = Node()\n  return x }\n", findTarget("typescript"));
        // (P40 review r6: and the `null` initializer is widened back to it, or tsc narrows `x` to `null`)
        check(tn.ok && has(tn.code, "let x: (Node | null) = null as (Node | null);") && !has(tn.code, "let x = null;"),
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
            "class Node { var v: i32\n  constructor() { this.v = 0 } }\n"
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
        check(cs.ok && has(cs.code, "s.ToUpperInvariant()") && !has(cs.code, "s.toUpper()"),
              "P14b: bound string extension -> C# .ToUpperInvariant() (P40 A3: culture-free)");
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

}
