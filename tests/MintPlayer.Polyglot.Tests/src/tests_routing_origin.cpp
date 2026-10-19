// P30 slice 7 include rules, P38 slices 3/8 origin mapping + v3 source map, P30 slice 8 crossDirImports, issues #29 #33-#36.
// P41 D14: one translation unit of the unit suite (split from tests_main.cpp so the areas compile in
// parallel). The bodies are unchanged; the shared harness lives in test_support.hpp.

#include "test_support.hpp"

void run_routing_origin() {
    // ---- P30 slice 7: include file-mapping rules (glob / templates / routing / closure rule) --

    // Glob semantics table (`**` = zero-or-more segments, captured as RecursiveDir; `*`/`?` stay
    // within one segment) — pinned here so engine drift is a test failure, not a surprise.
    {
        auto m = [](const char* pat, const char* path) { return cli::globMatch(pat, path); };
        check(m("**/*.pg", "a.pg").matched && m("**/*.pg", "a.pg").recursiveDir.empty(),
              "P30 s7: ** matches zero segments (empty RecursiveDir)");
        check(m("**/*.pg", "x/y/a.pg").matched && m("**/*.pg", "x/y/a.pg").recursiveDir == "x/y/",
              "P30 s7: ** captures the matched span as RecursiveDir (trailing '/')");
        check(m("./**/*.pg", "x/a.pg").matched, "P30 s7: a leading ./ is tolerated on patterns");
        check(m("src/*.pg", "src/a.pg").matched && !m("src/*.pg", "src/sub/a.pg").matched,
              "P30 s7: * stays within one segment");
        check(m("FruitCake/polyglot/*.pg", "FruitCake/polyglot/fruitcake_solver.pg").matched &&
                  !m("FruitCake/polyglot/*.pg", "Snake/polyglot/snake_solver.pg").matched,
              "P30 s7: the dogfood per-destination rule shape matches exactly");
        check(m("a?c.pg", "abc.pg").matched && !m("a?c.pg", "abbc.pg").matched, "P30 s7: ? is one char");
        check(m("src/**/gen/*.pg", "src/a/b/gen/x.pg").recursiveDir == "a/b/",
              "P30 s7: RecursiveDir is only the **-matched span");
    }

    // Output templates: expansion + the unknown-placeholder refusal (extension is auto-appended).
    {
        cli::TemplateValues v{"solver", "FruitCake/polyglot", "x/y/", "typescript"};
        std::string out, err;
        check(cli::expandOutputTemplate("dist/%(TargetLanguage)/%(RecursiveDir)%(Filename)", v, out, err) &&
                  out == "dist/typescript/x/y/solver",
              "P30 s7: template placeholders expand");
        check(cli::expandOutputTemplate("%(Directory)/%(Filename)", v, out, err) &&
                  out == "FruitCake/polyglot/solver",
              "P30 s7: %(Directory)/%(Filename) is the next-to-source shape");
        check(!cli::expandOutputTemplate("%(TargetExtension)/x", v, out, err) &&
                  has(err, "unknown placeholder") && has(err, "appended automatically"),
              "P30 s7: an unknown placeholder is refused, naming the auto-appended extension");
    }

    // Rule routing: first-match-wins, target filtering, fallback, config-relative resolution.
    // (Fixture paths are temp-dir-based so they are genuinely ABSOLUTE on POSIX too — a literal
    // "C:/proj" is a relative path there, and the fs arithmetic diverges: the linux CI catch.)
    {
        namespace fs = std::filesystem;
        const fs::path proj = (fs::temp_directory_path() / "p30-s7-route" / "proj").lexically_normal();
        cli::PgConfig pc;
        pc.found = true;
        pc.dir = proj;
        pc.include.push_back({"FruitCake/polyglot/*.pg", {"typescript"}, "../web/app/fruit-cake/%(Filename)"});
        pc.include.push_back({"**/*.pg", {"typescript"}, "gen/%(RecursiveDir)%(Filename)"});
        std::string err;

        auto r1 = cli::routeIncludeOutput(pc, proj / "FruitCake/polyglot/fruitcake_solver.pg",
                                          "typescript", err);
        check(r1 && *r1 == (proj.parent_path() / "web/app/fruit-cake/fruitcake_solver").lexically_normal(),
              "P30 s7: the FIRST matching rule wins (narrow above broad)");
        auto r2 = cli::routeIncludeOutput(pc, proj / "Snake/polyglot/snake_solver.pg",
                                          "typescript", err);
        check(r2 && *r2 == (proj / "gen/Snake/polyglot/snake_solver").lexically_normal(),
              "P30 s7: the broad rule mirrors the tree via %(RecursiveDir)");
        check(!cli::routeIncludeOutput(pc, proj / "Snake/polyglot/snake_solver.pg", "csharp", err) &&
                  err.empty(),
              "P30 s7: a target no rule names falls through cleanly (no match, no error)");
        check(!cli::routeIncludeOutput(pc, proj.parent_path() / "elsewhere/a.pg", "typescript", err),
              "P30 s7: a source outside the config's tree never routes");
    }

    // Closure outputs: prelude follows the entry; a split closure is refused, never miscompiled.
    {
        namespace fs = std::filesystem;
        const fs::path proj = (fs::temp_directory_path() / "p30-s7-closure" / "proj").lexically_normal();
        const fs::path outAlt = (fs::temp_directory_path() / "p30-s7-closure" / "out").lexically_normal();
        cli::PgConfig pc;
        pc.found = true;
        pc.dir = proj;
        pc.include.push_back({"main.pg", {"typescript"}, "web/%(Filename)"});
        EmitResult fake;
        fake.ok = true;
        fake.code = "entry";
        fake.modules.push_back({"util", "code", (proj / "util.pg").string()});
        fake.modules.push_back({"__polyglot_prelude", "prelude", ""});
        std::vector<fs::path> outs;
        std::string err;

        // Entry routed to web/, util unmatched -> fallback dir: a SPLIT closure must refuse.
        check(!cli::resolveClosureOutputs(pc, false, proj / "main.pg", fake, "typescript", ".ts",
                                          proj, outs, err) &&
                  has(err, "split the import closure"),
              "P30 s7: include rules splitting a closure are refused with the split named");

        // A rule covering the whole closure routes everything together; the prelude follows the entry.
        pc.include.clear();
        pc.include.push_back({"**/*.pg", {"typescript"}, "web/%(Filename)"});
        check(cli::resolveClosureOutputs(pc, false, proj / "main.pg", fake, "typescript", ".ts",
                                         proj, outs, err) &&
                  outs.size() == 3 && outs[0] == proj / "web/main.ts" &&
                  outs[1] == proj / "web/util.ts" && outs[2] == proj / "web/__polyglot_prelude.ts",
              "P30 s7: a closure-covering rule routes entry+modules together, prelude follows the entry");

        // flagRouted (explicit --target + --out) bypasses the rules wholesale.
        check(cli::resolveClosureOutputs(pc, true, proj / "main.pg", fake, "typescript", ".ts",
                                         outAlt, outs, err) &&
                  outs[0] == outAlt / "main.ts" && outs[2] == outAlt / "__polyglot_prelude.ts",
              "P30 s7: explicit --target + --out keeps flag semantics (rules bypassed)");
    }

    // Core: ModuleFile carries its source path (per-file routing needs to attribute linked modules).
    {
        MapModuleResolver res({{"lib", "fn dbl(x: i32): i32 { return x * 2 }\n"}});
        EmitResult r = compile("import { dbl } from \"lib\"\nfn f(): i32 {\n  return dbl(2)\n}\n",
                               findTarget("csharp"), &res, LibConfig{});
        check(r.ok && r.modules.size() == 1 && r.modules[0].sourcePath == "lib",
              "P30 s7: a linked module surfaces its canonical source path");
    }

    // pgconfig parse: include rules round-trip; a malformed rule is an ERROR, never a silent no-op.
    {
        namespace fs = std::filesystem;
        const fs::path base = fs::temp_directory_path() / "polyglot-p30-s7-cfg";
        std::error_code ec;
        fs::remove_all(base, ec);
        fs::create_directories(base);
        cli::writeFile(base / "pgconfig.json",
                       "{ \"targets\": [\"csharp\", \"typescript\"], \"include\": ["
                       "{ \"pattern\": \"**/*.pg\", \"target\": [\"typescript\", \"python\"], \"output\": \"gen/%(Filename)\" },"
                       "{ \"pattern\": \"a/*.pg\", \"output\": \"b/%(Filename)\" } ] }");
        const cli::PgConfig pc = cli::loadPgConfig(base);
        check(pc.errors.empty() && pc.include.size() == 2 && pc.include[0].targets.size() == 2 &&
                  pc.include[0].targets[1] == "python" && pc.include[1].targets.empty() &&
                  pc.include[1].output == "b/%(Filename)",
              "P30 s7: include rules parse (target string-array and absent forms)");
        cli::writeFile(base / "pgconfig.json", "{ \"include\": [ { \"pattern\": \"**/*.pg\" } ] }");
        const cli::PgConfig bad = cli::loadPgConfig(base);
        check(!bad.errors.empty() && has(bad.errors[0], "pattern") && has(bad.errors[0], "output"),
              "P30 s7: an include rule missing pattern/output is a manifest error, not a silent no-op");
        // Review round 5 — CLI half: `"target": []` is refused (it read as "every target"), like `"targets": []`.
        cli::writeFile(base / "pgconfig.json", "{ \"include\": [ { \"pattern\": \"*.pg\", \"target\": [], \"output\": \"gen/%(Filename)\" } ] }");
        const cli::PgConfig emptyTarget = cli::loadPgConfig(base);
        check(emptyTarget.errors.size() == 1 && has(emptyTarget.errors[0], "\"target\" is an empty list"),
              "review r5: an include rule's \"target\": [] is a config error");
        // Review round 5 (gating 1): where a route starts — the config folder climbed by the template's leading `..`.
        const fs::path cfgDir = cli::absoluteDir(base / "proj");
        check(cli::includeRouteRoot(cfgDir, "gen/%(Filename)").empty() && cli::includeRouteRoot(cfgDir, "%(Filename)").empty(),
              "review r5: a route inside the config folder starts at the config folder (no extra root)");
        check(cli::includeRouteRoot(cfgDir, "../web/gen/%(RecursiveDir)%(Filename)") == cli::absoluteDir(base) &&
                  cli::includeRouteRoot(cfgDir, "..\\..\\x/%(Filename)") == cli::absoluteDir(base / "..") &&
                  cli::includeRouteRoot(cfgDir, "gen/../../web/Gen/%(Filename)") == cli::absoluteDir(base),
              "review r5: a route leaving the config folder starts where its leading `..` segments land");
        fs::remove_all(base, ec);
    }

    // ---- P38/issue #69 slice 3: originMapping is manifest DATA, never a target-name comparison ----
    {
        // Each target's origin story round-trips from its own manifest. C# writes `#line` pragmas into the
        // source; TS writes a v3 sidecar beside it; Python and PHP say nothing and are unaffected.
        const OriginMapping& cs = findBackend("csharp")->originMapping();
        const OriginMapping& ts = findBackend("typescript")->originMapping();
        check(cs.style == OriginMapping::Style::Directive && cs.emitsDirectives() && cs.line == "#line $n \"$f\"" &&
                  cs.hidden == "#line hidden" && cs.column == 0,
              "P38 s3: C# declares the directive style with both templates");
        check(ts.style == OriginMapping::Style::SourceMapV3 && ts.emitsSourceMap() &&
                  ts.sidecarExtension == ".map" && has(ts.footer, "sourceMappingURL"),
              "P38 s3: TypeScript declares the v3-sidecar style");
        // P39/issue #71 reversed this: Python and PHP now declare the SAME v3-sidecar sink, and doing so
        // took a manifest entry and no engine change — origin recording was always target-neutral, so
        // they were missing a sink DECLARATION, not the data.
        const Backend* py = findBackend("python");
        const Backend* php = findBackend("php");
        check(py && py->originMapping().emitsSourceMap() && py->originMapping().sidecarExtension == ".map" &&
                  has(py->originMapping().footer, "sourceMappingURL"),
              "P39: Python declares the v3-sidecar style (was Style::None through P38)");
        check(php && php->originMapping().emitsSourceMap() &&
                  php->originMapping().sidecarExtension == ".map" &&
                  has(php->originMapping().footer, "sourceMappingURL"),
              "P39: PHP declares the v3-sidecar style (was Style::None through P38)");

        // The vocabulary is CLOSED: an unknown style is a plugin LOAD ERROR, never a silent default —
        // that is what makes version skew loud (the `blockStyle` precedent).
        const std::string head =
            "{\"schema\":2,\"name\":\"t\",\"spec\":{\"name\":\"t\",\"assignmentOrder\":\"targetFirst\"},"
            "\"rules\":{\"Program\":{\"line\":\"\"},\"Type\":{\"line\":\"\"}},";
        std::string err;
        check(!validateBackend(head + "\"originMapping\":{\"style\":\"dwarf\"}}", err) &&
                  has(err, "unknown originMapping style"),
              "P38 s3: an unknown originMapping style refuses at load (never a silent no-op)");
        err.clear();
        check(!validateBackend(head + "\"originMapping\":{\"style\":\"directive\",\"line\":\"#line $n\"}}", err) &&
                  has(err, "needs both"),
              "P38 s3: a directive style without both templates refuses (a declared-but-dead sink is a bug)");
        err.clear();
        check(!validateBackend(head + "\"originMapping\":\"yes\"}", err) && has(err, "must be an object"),
              "P38 s3: a non-object originMapping refuses");
        // Review round 7 — language half: the import-binding kinds are a closed, validated vocabulary.
        {
            auto anyError = [](const SpecLoadResult& r, const std::string& what) {
                for (const auto& e : r.errors) if (e.find(what) != std::string::npos) return true;
                return false;
            };
            SpecLoadResult bad = loadBackendSpec(R"({"name":"t","assignmentOrder":"targetFirst","typeOnlyImports":["record"]})");
            check(!bad.ok && anyError(bad, "typeOnlyImports: unknown declaration kind 'record'"),
                  "r7: an unknown typeOnlyImports kind refuses at load");
            SpecLoadResult notArr = loadBackendSpec(R"({"name":"t","assignmentOrder":"targetFirst","unboundImports":"union"})");
            check(!notArr.ok && anyError(notArr, "unboundImports must be an array"),
                  "r7: a non-array unboundImports refuses at load");
            SpecLoadResult good = loadBackendSpec(R"({"name":"t","assignmentOrder":"targetFirst","typeOnlyImports":["union"],"unboundImports":["unionCase"]})");
            check(good.ok && good.spec.typeOnlyImports == std::vector<std::string>{"union"} &&
                      good.spec.unboundImports == std::vector<std::string>{"unionCase"},
                  "r7: the import-binding kinds load");
        }
        const Backend* tsb = findBackend("typescript");
        const Backend* pyb = findBackend("python");
        check(tsb && tsb->typeOnlyImports() == std::vector<std::string>{"union", "interface"} &&
                  tsb->unboundImports() == std::vector<std::string>{"unionCase"},
              "r7: TS imports a union/interface with `import type` and drops a union case's name");
        check(pyb && pyb->typeOnlyImports().empty() &&
                  pyb->unboundImports() == std::vector<std::string>{"union", "unionCase"},
              "r7: Python drops a union's and a union case's name from an import (an interface is an ABC: imported)");
    }

    // ---- P38/issue #69 slice 8: Base64-VLQ + the v3 source map ------------------------------------
    {
        // The encoding the v3 `mappings` field is built from: sign in the low bit, 5-bit groups
        // little-endian, bit 6 = "another group follows". These are the values every v3 reference
        // implementation agrees on, so a wrong encoder shows up here rather than in a consumer's tooling.
        check(vlqEncode(0) == "A", "P38 s8: VLQ 0");
        check(vlqEncode(1) == "C", "P38 s8: VLQ 1");
        check(vlqEncode(-1) == "D", "P38 s8: VLQ -1");
        check(vlqEncode(15) == "e", "P38 s8: VLQ 15");
        check(vlqEncode(16) == "gB", "P38 s8: VLQ 16 (two groups)");
        check(vlqEncode(-16) == "hB", "P38 s8: VLQ -16");
        check(vlqEncode(123) == "2H", "P38 s8: VLQ 123");

        SourceMapInput in;
        in.file = "out.ts";
        in.sources = {"../src/a.pg"};
        in.sourcesContent = {"fn main() {}\n"};
        in.fileIdToSourceIndex = {-1, 0};        // fileId 0 = unknown, fileId 1 -> sources[0]
        in.origins = {{1, 1, 4}, {3, 1, 5}, {4, 9, 9}}; // the last has an unmapped fileId
        const std::string doc = buildSourceMapV3(in);
        check(has(doc, "\"version\":3"), "P38 s8: the map declares v3");
        check(has(doc, "\"file\":\"out.ts\""), "P38 s8: the map names the emitted file");
        check(has(doc, "\"sources\":[\"../src/a.pg\"]"), "P38 s8: sources are map-relative, as given");
        check(has(doc, "fn main()"), "P38 s8: sourcesContent is embedded (survives bundling/relocation)");
        // Output line 1 -> source line 4, then line 3 -> line 5. Two ';' separate them (line 2 is
        // unmapped), and the out-of-range fileId is skipped rather than poisoning the table.
        check(has(doc, "\"mappings\":\"AAGA;;AACA\""), "P38 s8: mappings encode line deltas and skip unmapped origins");
    }

    // ---- P30 slice 8: cross-directory import specifiers (crossDirImports) ---------------------
    {
        // The manifest flag round-trips: TS declares it, Python/C# don't (their module systems make
        // cross-dir imports non-trivial — the closure rule stays for them).
        check(findBackend("typescript")->crossDirImports() && !findBackend("python")->crossDirImports() &&
                  !findBackend("csharp")->crossDirImports(),
              "P30 s8: crossDirImports parses from the manifest (TS yes, Python/C# no)");

        // Flat (no router): the emitted specifier is byte-identical to the historical "./<name>".
        MapModuleResolver res({{"lib", "fn dbl(x: i32): i32 { return x * 2 }\n"}});
        const std::string src = "import { dbl } from \"lib\"\nfn f(): i32 {\n  return dbl(2)\n}\n";
        EmitResult flat = compile(src, findTarget("typescript"), &res, LibConfig{});
        check(flat.ok && has(flat.code, "from \"./lib\""),
              "P30 s8: without a router the TS specifier stays ./<name> (byte-identical)");

        // Routed apart: a real relative specifier climbs and descends between the routed dirs.
        LibConfig up;
        up.moduleOutputDir = [](const std::string& origin) {
            return origin.empty() ? std::string("C:/o/app") : std::string("C:/o/shared/geom");
        };
        EmitResult cross = compile(src, findTarget("typescript"), &res, up);
        check(cross.ok && has(cross.code, "from \"../shared/geom/lib\""),
              "P30 s8: a routed-apart module gets a climbing relative specifier");

        LibConfig down;
        down.moduleOutputDir = [](const std::string& origin) {
            return origin.empty() ? std::string("C:/o") : std::string("C:/o/sub");
        };
        EmitResult below = compile(src, findTarget("typescript"), &res, down);
        check(below.ok && has(below.code, "from \"./sub/lib\""),
              "P30 s8: a below-tree module gets a ./-prefixed specifier (never a bare package name)");

        // A non-crossDirImports target ignores the router entirely (flag-gated, never half-applied).
        EmitResult py = compile(src, findTarget("python"), &res, up);
        check(py.ok && has(py.code, "from lib import") && !has(py.code, "../"),
              "P30 s8: python ignores the router (bare basename, closure rule still guards it)");

        // The closure check permits a split for a crossDirImports target (allowSplit).
        namespace fs = std::filesystem;
        const fs::path proj = (fs::temp_directory_path() / "p30-s8-split" / "proj").lexically_normal();
        cli::PgConfig pc;
        pc.found = true;
        pc.dir = proj;
        pc.include.push_back({"main.pg", {"typescript"}, "app/%(Filename)"});
        EmitResult fake;
        fake.ok = true;
        fake.code = "entry";
        fake.modules.push_back({"util", "code", (proj / "lib/util.pg").string()});
        std::vector<fs::path> outs;
        std::string err;
        check(!cli::resolveClosureOutputs(pc, false, proj / "main.pg", fake, "typescript", ".ts",
                                          proj, outs, err, /*allowSplit=*/false),
              "P30 s8: (control) the split still refuses without the capability");
        check(cli::resolveClosureOutputs(pc, false, proj / "main.pg", fake, "typescript", ".ts",
                                         proj, outs, err, /*allowSplit=*/true) &&
                  outs[0] == proj / "app/main.ts" && outs[1] == proj / "util.ts",
              "P30 s8: allowSplit routes a closure across directories (entry routed, module fallback)");
    }

    // issue #33 — hex literals: real radix lexing + radix-aware suffix stripping. The trailing lowercase
    // 'f'/'d' of a hex literal was eaten by the float-suffix strip (`0xff` -> `0xf`, `0x11d` -> `0x11`),
    // and a `f32`/`f64` tail stripped THREE digits (`0xf32` -> invalid `0x`).
    {
        DiagnosticBag d;
        auto t = lex("0xff 0x11d 0xf32 0xFF 0xffi64 0x1D", d);
        check(!d.hasErrors(), "issue#33: hex literals lex clean");
        auto textAt = [&](std::size_t i) { return i < t.size() ? t[i].text : std::string(); };
        check(t[0].kind == TokKind::IntLit && textAt(0) == "0xff", "issue#33: 0xff keeps its digits");
        check(textAt(1) == "0x11d", "issue#33: 0x11d keeps its trailing 'd'");
        check(textAt(2) == "0xf32", "issue#33: 0xf32 is three hex digits, not a float suffix");
        check(textAt(3) == "0xFF", "issue#33: uppercase hex unchanged");
        check(textAt(4) == "0xffi64", "issue#33: integer width suffix still accepted on hex");
        DiagnosticBag bad;
        lex("0xffz", bad);
        check(bad.hasErrors(), "issue#33: unknown suffix on a hex literal is a diagnostic");
        DiagnosticBag bare;
        lex("0x + 1", bare);
        check(bare.hasErrors(), "issue#33: '0x' with no digits is a diagnostic");
        // End-to-end: the emitted code carries the intact literal on every target (lowering was the
        // truncation site — stripNumericSuffix ran suffix rules on radix literals).
        const char* prog = "fn main() {\n  print(0xff)\n  print(0x11d)\n}\n";
        for (const char* tgt : {"csharp", "typescript", "python", "php"}) {
            EmitResult r = compileStd(prog, findTarget(tgt));
            check(r.ok && has(r.code, "0xff") && has(r.code, "0x11d"),
                  std::string("issue#33: emitted ") + tgt + " keeps 0xff/0x11d intact");
        }
    }

    // issue #34 — the portable char→ordinal path: std.strings codePointAt maps per target, and the
    // silently-miscompiling `(i32)char` is a diagnostic naming the fix ((int)'A' was 65 on C# but 0 on
    // TS/PHP and the cast was dropped on Python).
    {
        const char* prog = "import \"std.strings\"\nfn main() {\n  print(\"A\".codePointAt(0))\n}\n";
        EmitResult cs = compileStd(prog, findTarget("csharp"));
        check(cs.ok && has(cs.code, "char.ConvertToUtf32("), "issue#34: C# codePointAt -> char.ConvertToUtf32");
        EmitResult ts = compileStd(prog, findTarget("typescript"));
        check(ts.ok && has(ts.code, "_pg_codePointAt(\"A\", 0)") && has(ts.code, "self.codePointAt(index)!"),
              "issue#34/P40 s8: TS codePointAt -> the _pg_codePointAt guard over .codePointAt()!");
        EmitResult py = compileStd(prog, findTarget("python"));
        check(py.ok && has(py.code, "ord("), "issue#34: Python codePointAt -> ord()");
        EmitResult php = compileStd(prog, findTarget("php"));
        check(php.ok && has(php.code, "__pg_u16_cp($self_, $index)"),
              "issue#34/P40 s8: PHP codePointAt -> the UTF-16 helper (iconv, no mbstring)");
        EmitResult castChar = compileStd("fn main() {\n  var c = 'A'\n  print((i32)c)\n}\n", findTarget("csharp"));
        check(!castChar.ok, "issue#34: (i32) on a char is rejected");
        bool named = false;
        for (const auto& dg : castChar.diagnostics) if (has(dg.message, "codePointAt")) named = true;
        check(named, "issue#34: the char-cast diagnostic names codePointAt");
        EmitResult callCast = compileStd("fn main() {\n  var c = 'A'\n  print(i32(c))\n}\n", findTarget("csharp"));
        check(!callCast.ok, "issue#34: call-syntax i32(char) is rejected too");
        EmitResult castStr = compileStd("fn main() {\n  var s = \"x\"\n  print((i32)s)\n}\n", findTarget("csharp"));
        check(!castStr.ok, "issue#34: (i32) on a string stays rejected (regression)");
    }

    // issue #35 — PHP: module globals must be visible inside methods and property getters, not just free
    // functions (ir::Method now carries globalRefs; the PHP MethodDecl emits the `global $…;` prologue).
    {
        auto phpOf = [&](const char* src) { return compileStd(src, findTarget("php")); };
        EmitResult method = phpOf("let TABLE: i32[] = [10, 20, 30]\n"
                                  "class Box {\n  fn lookup(i: i32): i32 => TABLE[i]\n}\nfn main() {}\n");
        check(method.ok && has(method.code, "global $TABLE;"), "issue#35: PHP class method gets `global`");
        EmitResult getter = phpOf("const LIMIT: i32 = 7\n"
                                  "record Vec(x: i32) {\n  let cap: i32 => LIMIT\n}\nfn main() {}\n");
        check(getter.ok && has(getter.code, "global $LIMIT;"), "issue#35: PHP property getter gets `global`");
        EmitResult lambda = phpOf("let BASE: i32 = 5\n"
                                  "class Box {\n  fn all(): i32 {\n    let f = (x: i32) => x + BASE\n    return f(1)\n  }\n}\nfn main() {}\n");
        check(lambda.ok && has(lambda.code, "global $BASE;"), "issue#35: closure inside a method reaches the global");
        EmitResult shadow = phpOf("let TABLE: i32[] = [1]\n"
                                  "class Box {\n  fn own(TABLE: i32): i32 => TABLE\n}\nfn main() {}\n");
        check(shadow.ok && !has(shadow.code, "global $TABLE;"), "issue#35: a shadowing param suppresses `global`");
        EmitResult freeFn = phpOf("let TABLE: i32[] = [10, 20]\nfn readIt(): i32 => TABLE[1]\nfn main() {}\n");
        check(freeFn.ok && has(freeFn.code, "global $TABLE;"), "issue#35: free-function path unchanged (regression)");
        // The C# leg (found by module_globals.pg): globals are fields of PolyglotProgram, so a member of
        // ANOTHER type referencing one bare didn't compile. `using static PolyglotProgram;` (emitted only
        // when globals exist) + `internal` fields make them reachable with correct shadowing semantics.
        EmitResult cs = compileStd("let TABLE: i32[] = [10, 20]\n"
                                   "class Box {\n  fn lookup(i: i32): i32 => TABLE[i]\n}\nfn main() {}\n",
                                   findTarget("csharp"));
        check(cs.ok && has(cs.code, "using static PolyglotProgram;") && has(cs.code, "internal static readonly"),
              "issue#35: C# type members reach module globals (using static + internal fields)");
        EmitResult csNone = compileStd("fn main() {}\n", findTarget("csharp"));
        check(csNone.ok && !has(csNone.code, "using static"),
              "issue#35: no globals -> no using-static line (byte-gate)");
    }

    // issue #36 — PHP List<T>/T[] are \ArrayObject (true reference semantics): construction boxes, and
    // clear/removeAll/removeAt mutate in place through the handle instead of reassigning a copied array.
    {
        const char* prog =
            "fn mutate(xs: List<i32>) {\n  xs.add(99)\n}\n"
            "fn main() {\n  var ys: List<i32> = [1]\n  mutate(ys)\n  print(ys.count)\n"
            "  ys.removeAt(0)\n  ys.clear()\n  ys.removeAll((v) => v > 0)\n}\n";
        EmitResult php = compileStd(prog, findTarget("php"));
        check(php.ok && has(php.code, "new \\ArrayObject([1])"), "issue#36: PHP list literal boxes into \\ArrayObject");
        check(php.ok && has(php.code, "->exchangeArray("), "issue#36: PHP size-mutation goes through exchangeArray");
        check(php.ok && !has(php.code, "$ys = ["), "issue#36: no value-typed array reassignment remains");
        EmitResult arr = compileStd("fn main() {\n  var a: i32[] = [1, 2]\n  a[0] = 9\n  print(a[0])\n}\n",
                                    findTarget("php"));
        check(arr.ok && has(arr.code, "new \\ArrayObject([1, 2])"), "issue#36: PHP T[] array boxes too");
        EmitResult ts = compileStd(prog, findTarget("typescript"));
        check(ts.ok && !has(ts.code, "ArrayObject"), "issue#36: TS output untouched by the PHP boxing");
    }

    // issue #29 — interfaces made honest: implements-conformance + nominal assignability + the
    // C#-convention override rule in the checker, abc.ABC emission on Python, and the fake typed-pattern
    // narrowing refused.
    {
        const std::string iface = "interface IPgNet {\n  fn forward(x: i32): i32\n}\n";
        EmitResult missing = compileStd(iface + "class Net : IPgNet {\n  fn other(): i32 => 1\n}\nfn main() {}\n",
                                        findTarget("csharp"));
        check(!missing.ok, "issue#29: missing interface method is a diagnostic");
        EmitResult wrongSig = compileStd(iface + "class Net : IPgNet {\n  fn forward(x: string): i32 => 1\n}\nfn main() {}\n",
                                         findTarget("csharp"));
        check(!wrongSig.ok, "issue#29: wrong implementing signature is a diagnostic");
        EmitResult okImpl = compileStd(iface +
            "class Net : IPgNet {\n  fn forward(x: i32): i32 => x + 1\n}\n"
            "fn drive(net: IPgNet): i32 => net.forward(1)\n"
            "fn feed(n: Net): i32 => drive(n)\nfn main() {}\n", findTarget("csharp"));
        check(okImpl.ok, "issue#29: conforming class widens into an interface-typed param");
        EmitResult badAssign = compileStd(iface +
            "class Other {\n  fn forward(x: i32): i32 => x\n}\n"
            "fn drive(net: IPgNet): i32 => net.forward(1)\n"
            "fn feed(o: Other): i32 => drive(o)\nfn main() {}\n", findTarget("csharp"));
        check(!badAssign.ok, "issue#29: non-implementing class refused at an interface-typed slot (nominal)");
        // Generic interface + record implementer (the sample-04 shape) stays green.
        EmitResult generic = compileStd(
            "interface Comparable<T> {\n  fn compareTo(other: T): i32\n}\n"
            "record Version(major: i32) : Comparable<Version> {\n  fn compareTo(o: Version): i32 => this.major - o.major\n}\n"
            "fn main() {}\n", findTarget("csharp"));
        check(generic.ok, "issue#29: generic interface conformance (type-arg substitution) accepts sample-04 shape");
        // `override` follows C# conventions: base-class open members only, never interface implementations.
        EmitResult strayOverride = compileStd(iface +
            "class Net : IPgNet {\n  override fn forward(x: i32): i32 => x\n}\nfn main() {}\n", findTarget("csharp"));
        check(!strayOverride.ok, "issue#29: 'override' on an interface implementation is a diagnostic");
        EmitResult baseOverride = compileStd(
            "open class Shape {\n  open fn area(): f64 => 0.0\n}\n"
            "class Disk : Shape {\n  override fn area(): f64 => 1.0\n}\nfn main() {}\n", findTarget("csharp"));
        check(baseOverride.ok, "issue#29: 'override' of an open base-class member stays valid");
        EmitResult noBase = compileStd("class Lone {\n  override fn area(): f64 => 1.0\n}\nfn main() {}\n",
                                       findTarget("csharp"));
        check(!noBase.ok, "issue#29: 'override' with no base-class member is a diagnostic");
        // Interface bodies are bodiless, non-static signatures only (previously accepted + silently dropped).
        EmitResult bodied = compileStd("interface I {\n  fn f(): i32 => 1\n}\nfn main() {}\n", findTarget("csharp"));
        check(!bodied.ok, "issue#29: a bodied interface method is a diagnostic");
        EmitResult field = compileStd("interface I {\n  let x: i32\n}\nfn main() {}\n", findTarget("csharp"));
        check(!field.ok, "issue#29: a field in an interface is a diagnostic");
        // Python: the interface exists as an abc.ABC class, so the implements clause resolves at import.
        EmitResult py = compileStd(iface +
            "class Net : IPgNet {\n  fn forward(x: i32): i32 => x + 1\n}\nfn main() {}\n", findTarget("python"));
        check(py.ok && has(py.code, "import abc as _PG_abc") && has(py.code, "class IPgNet(_PG_abc.ABC):") &&
                  has(py.code, "@_PG_abc.abstractmethod") && has(py.code, "class Net(IPgNet):"),
              "issue#29: Python emits the interface as an abc.ABC base");
        // #38 (issue #29 follow-up): a typed match binding IS now a runtime type test on a concrete class —
        // it compiles and emits a C# declaration pattern (`Disk d`), not a fake first-arm-always-wins.
        EmitResult typedPat = compileStd(
            "open class Shape {\n  open fn kind(): i32 => 0\n}\n"
            "class Disk : Shape {\n  override fn kind(): i32 => 1\n}\n"
            "fn f(s: Shape): i32 => match s { d: Disk => 1, _ => 0 }\nfn main() {}\n", findTarget("csharp"));
        check(typedPat.ok && has(typedPat.code, "Disk d"),
              "issue#29/#38: a typed 'match' binding is a concrete-class type test (C# declaration pattern)");
    }

}
