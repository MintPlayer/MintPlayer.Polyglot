// P40 slices 1-4: manifest schema 2 + reachability, target options, escapes, ranges (issue #73).
// P41 D14: one translation unit of the unit suite (split from tests_main.cpp so the areas compile in
// parallel). The bodies are unchanged; the shared harness lives in test_support.hpp.

#include "test_support.hpp"

void run_p40_manifest() {
    // ---- P40 slice 1: manifest schema 2, binding `requires`, the preludeSlot marker, reachability ---------
    {
        auto manifestOf = [](const char* target) {
            std::string m;
            cli::readFile(cli::executablePath().parent_path() / "plugins" / target / "polyglot-plugin.json", m);
            return m;
        };
        auto replaceAll = [](std::string s, const std::string& from, const std::string& to) {
            for (std::size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size())
                s.replace(at, from.size(), to);
            return s;
        };
        auto replaceOnce = [](std::string s, const std::string& from, const std::string& to, bool& found) {
            const std::size_t at = s.find(from);
            found = at != std::string::npos;
            if (found) s.replace(at, from.size(), to);
            return s;
        };
        auto renamed = [&](const char* target, const std::string& newName) {
            return replaceAll(manifestOf(target), std::string("\"name\": \"") + target + "\"",
                              "\"name\": \"" + newName + "\"");
        };
        const std::string py = renamed("python", "p40py");
        bool f = false;

        // Schema: the Core validates the field it never read before — schema 1, a missing schema and an
        // unknown version are all refused, naming the version this compiler reads.
        std::string err;
        check(validateBackend(py, err), "P40 s1: the in-box (schema 2) manifest validates");
        std::string s1 = replaceOnce(py, "\"schema\": 2", "\"schema\": 1", f);
        err.clear();
        check(f && !validateBackend(s1, err) && has(err, "schema (1)") && has(err, "reads schema 2"),
              "P40 s1: a schema-1 manifest is refused, naming the expected schema");
        std::string s0 = replaceOnce(py, "\"schema\": 2,", "", f);
        err.clear();
        check(f && !validateBackend(s0, err) && has(err, "schema (missing)"),
              "P40 s1: a manifest without a schema is refused");
        std::string s3 = replaceOnce(py, "\"schema\": 2", "\"schema\": 3", f);
        err.clear();
        check(f && !validateBackend(s3, err) && has(err, "schema (3)"),
              "P40 s1: an unknown (newer) schema is refused");

        // preludeSlot: required, exactly once, unconditional, Program only.
        std::string noSlot = replaceOnce(py, "\"fn\": \"preludeSlot\"", "\"fn\": \"ident\"", f);
        err.clear();
        check(f && !validateBackend(noSlot, err) && has(err, "preludeSlot") && has(err, "found 0"),
              "P40 s1: a Program rule without a preludeSlot marker is refused");
        std::string twoSlots = replaceOnce(py, "\"fn\": \"ident\"", "\"fn\": \"preludeSlot\"", f);
        err.clear();
        check(f && !validateBackend(twoSlots, err) && has(err, "exactly one"),
              "P40 s1: a second preludeSlot marker (anywhere) is refused");

        // Binding `requires`: every key must be a declared prelude; object form needs a string template;
        // a type spelling cannot carry requirements.
        const std::string sqrtArm = "\"Math.sqrt\": \"__import__('math').sqrt($0)\"";
        std::string badReq = replaceOnce(py, sqrtArm,
                                         "\"Math.sqrt\": { \"template\": \"x($0)\", \"requires\": [\"nope\"] }", f);
        err.clear();
        check(f && !validateBackend(badReq, err) && has(err, "requires 'nope'") && has(err, "not a declared prelude"),
              "P40 s1: a binding requiring an undeclared prelude is refused (anti-silent-drop)");
        std::string noTmpl = replaceOnce(py, sqrtArm, "\"Math.sqrt\": { \"requires\": [] }", f);
        err.clear();
        check(f && !validateBackend(noTmpl, err) && has(err, "string 'template'"),
              "P40 s1: an object binding without a template is refused");
        std::string typeReq = replaceOnce(py, "\"List.type\": \"list\"",
                                          "\"List.type\": { \"template\": \"list\", \"requires\": [\"fstr\"] }", f);
        err.clear();
        check(f && !validateBackend(typeReq, err) && has(err, "type spelling"),
              "P40 s1: `requires` on a type spelling is refused (nothing would record it)");
        std::string badRuleReq = replaceOnce(py, "\"idiv\": \"def _pg_idiv", "\"idivX\": \"def _pg_idiv", f);
        err.clear();
        check(f && !validateBackend(badRuleReq, err) && has(err, "\"require\"") && has(err, "'idiv'"),
              "P40 s1: a rule-level {\"fn\":\"require\"} naming an undeclared prelude is refused");

        // A binding that requires a helper pulls it in — through a member binding (Bound) and through a
        // synthesized expect-fn actual (Extern) — and ONLY where something calls it.
        std::string pyReq = replaceOnce(py, sqrtArm,
                                        "\"Math.sqrt\": { \"template\": \"_pg_sq($0)\", \"requires\": [\"sq\"] }", f);
        pyReq = replaceOnce(pyReq, "\"readText\": \"__import__('pathlib').Path(path).read_text()\"",
                            "\"readText\": { \"template\": \"_pg_rd(path)\", \"requires\": [\"rd\"] }", f);
        pyReq = replaceOnce(pyReq, "\"preludes\": {",
                            "\"preludes\": { \"sq\": \"def _pg_sq(x):\\n    return x ** 0.5\\n\", "
                            "\"rd\": \"def _pg_rd(p):\\n    return p\\n\",", f);
        err.clear();
        check(f && loadBackend(pyReq, err), "P40 s1: a manifest using the binding object form loads");
        BackendHandle pyr = findTarget("p40py");
        EmitResult sq = compile("import { Math } from \"std.math\"\nfn root(): f64 => Math.sqrt(4.0)\n", pyr);
        check(sq.ok && has(sq.code, "def _pg_sq(x):") && has(sq.code, "_pg_sq(4.0)") && !has(sq.code, "def _pg_rd"),
              "P40 s1: a member binding's `requires` emits its helper (and only that one)");
        check(sq.ok && sq.code.find("def _pg_sq") < sq.code.find("def root"),
              "P40 s1: Python's preludeSlot sits at the top of the file");
        EmitResult rd = compile("import { print, readText } from \"std.io\"\nfn main() { print(readText(\"a\")) }\n", pyr);
        check(rd.ok && has(rd.code, "def _pg_rd(p):") && !has(rd.code, "def _pg_sq"),
              "P40 s1: an expect-fn actual's `requires` emits its helper via the kept prelude function");
        EmitResult neither = compile("fn root(): i32 => 1\n", pyr);
        check(neither.ok && !has(neither.code, "_pg_sq") && !has(neither.code, "_pg_rd") &&
                  !has(neither.code, "def readText"),
              "P40 s1: an unreached binding emits no helper, and an unreached std fn is pruned");

        // C#: manifest helpers land in the file-local `file static class __PgRt`, after the usings.
        std::string cs = renamed("csharp", "p40cs");
        cs = replaceOnce(cs, "\"Math.sqrt\": \"global::System.Math.Sqrt($0)\"",
                         "\"Math.sqrt\": { \"template\": \"__PgRt.Sq($0)\", \"requires\": [\"sq\"] }", f);
        cs = replaceOnce(cs, "\"preludes\": {",
                         "\"preludes\": { \"sq\": \"    internal static double Sq(double x) => "
                         "global::System.Math.Sqrt(x);\\n\",", f);
        err.clear();
        check(f && loadBackend(cs, err), "P40 s1: a C# manifest with a declared prelude loads");
        EmitResult csq = compile("import { Math } from \"std.math\"\nfn root(): f64 => Math.sqrt(4.0)\n",
                                 findTarget("p40cs"));
        const std::size_t pgrt = csq.code.find("file static class __PgRt\n{\n    internal static double Sq(");
        check(csq.ok && pgrt != std::string::npos && has(csq.code, "__PgRt.Sq(4.0)"),
              "P40 s1: C# helpers are wrapped in the slot's `file static class __PgRt`");
        check(csq.ok && csq.code.find("#nullable enable") < pgrt && pgrt < csq.code.find("class PolyglotProgram"),
              "P40 s1: C#'s preludeSlot sits after the usings, before the first declaration");
        EmitResult cnone = compile("fn root(): i32 => 1\n", findTarget("p40cs"));
        check(cnone.ok && !has(cnone.code, "__PgRt"), "P40 s1: no helper required -> no __PgRt class at all");

        // PHP: the slot follows `<?php` (and the require_once lines); prelude functions are guarded by a
        // NAMESPACE-qualified function_exists, so a same-named global function cannot shadow them (S12).
        std::string php = renamed("php", "p40php");
        php = replaceOnce(php, "\"Math.sqrt\": \"sqrt($0)\"",
                          "\"Math.sqrt\": { \"template\": \"__pg_sq($0)\", \"requires\": [\"sq\"] }", f);
        php = replaceOnce(php, "\"preludes\": {",
                          "\"preludes\": { \"sq\": \"function __pg_sq($x) { return sqrt($x); }\\n\",", f);
        err.clear();
        check(f && loadBackend(php, err), "P40 s1: a PHP manifest with a declared prelude loads");
        EmitResult psq = compile("import { Math } from \"std.math\"\nfn root(): f64 => Math.sqrt(4.0)\n",
                                 findTarget("p40php"));
        const std::size_t ph = psq.code.find("function __pg_sq(");
        check(psq.ok && ph != std::string::npos && psq.code.find("<?php") < ph &&
                  ph < psq.code.find("function root"),
              "P40 s1: PHP's preludeSlot follows <?php, before the declarations");
        EmitResult pio = compile("import { print, readText } from \"std.io\"\nfn main() { print(readText(\"a\")) }\n",
                                 findTarget("php"));
        check(pio.ok && has(pio.code, "function_exists(__NAMESPACE__.'\\\\readText')"),
              "P40 s1: PHP prelude functions are guarded by a namespace-qualified function_exists");

        // TS: prelude declarations are module-local — never exported — while user declarations still are.
        EmitResult tsio = compile("import { print, readText } from \"std.io\"\nfn main() { print(readText(\"a\")) }\n",
                                  findTarget("typescript"));
        check(tsio.ok && has(tsio.code, "function readText(") && !has(tsio.code, "export function readText(") &&
                  has(tsio.code, "export function main("),
              "P40 s1: TS prelude declarations are not exported; user declarations are");
        EmitResult tsOpt = compile("fn f(): Option<i32> => None\n", findTarget("typescript"));
        check(tsOpt.ok && has(tsOpt.code, "type Option") && !has(tsOpt.code, "export type Option"),
              "P40 s1: a reached prelude union (via a bare case) is emitted, unexported");

        // Reachability on a hand-built module: user -> A -> B -> U (transitive), C unreached; a forced name
        // keeps an otherwise-unreached declaration (the shared prelude's union across roots).
        auto fnDecl = [](const std::string& name, const std::string& origin, const std::string& calls,
                         const std::string& returns) {
            ir::Function fn;
            fn.name = fn.mangledName = name;
            fn.originModule = origin;
            fn.returnType = namedType(returns.empty() ? "unit" : returns);
            if (!calls.empty())
                fn.body.push_back(std::make_unique<ir::ExprStmt>(
                    SourcePos{}, std::make_unique<ir::Call>(SourcePos{}, namedType("unit"), calls, false)));
            return fn;
        };
        auto buildModule = [&]() {
            ir::Module m;
            m.functions.push_back(fnDecl("userMain", "", "A", ""));
            m.functions.push_back(fnDecl("A", ir::kPreludeOrigin, "B", ""));
            m.functions.push_back(fnDecl("B", ir::kPreludeOrigin, "", "U"));
            m.functions.push_back(fnDecl("C", ir::kPreludeOrigin, "", "V"));
            ir::Union u; u.name = "U"; u.originModule = ir::kPreludeOrigin; m.unions.push_back(std::move(u));
            ir::Union v; v.name = "V"; v.originModule = ir::kPreludeOrigin; m.unions.push_back(std::move(v));
            return m;
        };
        ir::Module rm = buildModule();
        const std::set<std::string> reach = preludeReach(rm);
        check(reach.count("A") && reach.count("B") && reach.count("U") && !reach.count("C") && !reach.count("V"),
              "P40 s1: reachability is transitive through prelude declarations (A -> B -> U), C/V unreached");
        pruneUnreachablePrelude(rm);
        check(rm.functions.size() == 3 && rm.unions.size() == 1 && rm.unions[0].name == "U",
              "P40 s1: pruning erases exactly the unreached prelude declarations, never user ones");
        ir::Module forced = buildModule();
        const std::set<std::string> extra{"C"};
        pruneUnreachablePrelude(forced, &extra);
        check(forced.functions.size() == 4 && forced.unions.size() == 2,
              "P40 s1: a forced name (reached by another root) is kept, with what IT reaches (C -> V)");
        // The backstop: a prelude name spelled only inside opaque extern text still keeps it alive.
        ir::Module ext;
        ir::Function uf = fnDecl("userMain", "", "", "");
        uf.body.push_back(std::make_unique<ir::ExprStmt>(
            SourcePos{}, std::make_unique<ir::Extern>(SourcePos{}, namedType("unit"), "C();")));
        ext.functions.push_back(std::move(uf));
        ext.functions.push_back(fnDecl("C", ir::kPreludeOrigin, "", ""));
        check(preludeReach(ext).count("C"), "P40 s1: the extern-text identifier scan keeps a referenced prelude decl");
    }

    // ---- P40 slice 2: target options (manifest `options`, pgconfig/CLI targetOptions), namespaces + classes --
    {
        using OptMap = std::map<std::string, std::string>;
        // A LibConfig answering `opts` for `target` (every file), nothing for any other target.
        auto optsFor = [](const std::string& target, OptMap opts) {
            LibConfig l;
            l.targetOptions = [target, opts](const std::string& t, const std::string&) {
                return t == target ? opts : OptMap{};
            };
            return l;
        };
        const char* prog = "import { print } from \"std.io\"\nrecord Point(x: i32, y: i32)\n"
                           "fn find(): Option<i32> => Some(1)\nfn main() { print(find() ?? 0) }\n";

        // Resolution: defaults fill in, an unknown option and a pattern mismatch refuse (naming the fix).
        {
            OptMap res;
            std::string err;
            check(resolveTargetOptions(*findBackend("csharp"), {}, res, err) && res["class"] == "PolyglotProgram" &&
                      res["namespace"].empty() && res["access"].empty(),
                  "P40 s2: unset options resolve to the manifest defaults");
            check(!resolveTargetOptions(*findBackend("csharp"), {{"nmspace", "X"}}, res, err) &&
                      has(err, "unknown target option 'csharp.nmspace'") && has(err, "namespace, class, access"),
                  "P40 s2: an unknown option is refused, listing the declared ones");
            check(!resolveTargetOptions(*findBackend("csharp"), {{"access", "private"}}, res, err) &&
                      has(err, "invalid value 'private'"),
                  "P40 s2: a value failing the option's pattern is refused");
            check(!resolveTargetOptions(*findBackend("csharp"), {{"namespace", "My..Lib"}}, res, err),
                  "P40 s2: a malformed namespace is refused");
            check(!resolveTargetOptions(*findBackend("typescript"), {{"namespace", "X"}}, res, err) &&
                      has(err, "declares no options"),
                  "P40 s2: a target declaring no options refuses any");
            EmitResult bad = compile(prog, findTarget("csharp"), nullptr, optsFor("csharp", {{"access", "nope"}}));
            check(!bad.ok && !bad.diagnostics.empty() && has(bad.diagnostics[0].message, "csharp.access"),
                  "P40 s2: compile() refuses an invalid option with a diagnostic");
        }

        // P40 review B11: csharp.entryPoint=false drops the `Main` an entry file gets (CS7022/CS0017 in an app
        // that owns its entry point); the default keeps it.
        {
            EmitResult on = compile(prog, findTarget("csharp"), nullptr, optsFor("csharp", {}));
            EmitResult off = compile(prog, findTarget("csharp"), nullptr, optsFor("csharp", {{"entryPoint", "false"}}));
            check(on.ok && has(on.code, "static void Main()") && off.ok && !has(off.code, "static void Main()") &&
                      has(off.code, "void main()"),
                  "P40 B11: csharp.entryPoint=false omits Main, keeps main()");
        }

        // C#: file-scoped namespace before the usings, the configured wrapper class, access, internal helpers.
        {
            EmitResult r = compile(prog, findTarget("csharp"), nullptr,
                                   optsFor("csharp", {{"namespace", "My.Lib"}, {"class", "Api"}, {"access", "public"}}));
            // The two generated-header lines (P40 B5) come first; the namespace follows #nullable.
            check(r.ok && r.code.find("// <auto-generated/>\n// Generated by Polyglot. Do not edit: regenerate with "
                                      "`polyglot build`.\n#nullable enable\nnamespace My.Lib;\n") == 0,
                  "P40 s2: C# namespace is a file-scoped declaration right after #nullable");
            check(r.ok && has(r.code, "public static class Api") && has(r.code, "Api.print(") && has(r.code, "Api.find()") &&
                      !has(r.code, "PolyglotProgram"),
                  "P40 s2: C# `class` renames the wrapper and every qualified call");
            check(r.ok && has(r.code, "public abstract record Option<T>") && has(r.code, "public record Point"),
                  "P40 s2: prelude types in the namespace follow `access`");
            check(r.ok && has(r.code, "internal static void print<T>") && has(r.code, "public static Option<int> find()"),
                  "P40 s2: prelude helper functions stay internal under access=public");
            EmitResult g = compile("let LIMIT: i32 = 3\nfn f(): i32 => LIMIT\n", findTarget("csharp"), nullptr,
                                   optsFor("csharp", {{"namespace", "N"}, {"class", "Api"}}));
            check(g.ok && g.code.find("namespace N;\nusing static Api;") != std::string::npos,
                  "P40 s2: `using static <class>` follows the namespace (resolves inside it)");
            EmitResult def = compile(prog, findTarget("csharp"));
            check(def.ok && !has(def.code, "namespace ") && has(def.code, "static class PolyglotProgram"),
                  "P40 s2: no options = no namespace and the PolyglotProgram wrapper (unchanged output)");
        }

        // The wrapper class is emitted only when it has members.
        {
            EmitResult types = compile("record Point(x: i32, y: i32)\nenum Color { Red, Green }\n", findTarget("csharp"));
            check(types.ok && !has(types.code, "PolyglotProgram") && has(types.code, "record Point"),
                  "P40 s2: a C# file with no functions/globals emits no empty wrapper class");
            check(types.ok && types.code.back() == '\n' && types.code.find("\n\n\n") == std::string::npos,
                  "P40 s2: dropping the wrapper leaves no dangling blank lines");
        }

        // `reserve`: the configured wrapper name is claimed like an identifiers.reserved entry.
        {
            EmitResult clash = compile("record Api(x: i32)\n", findTarget("csharp"), nullptr,
                                       optsFor("csharp", {{"class", "Api"}}));
            check(!clash.ok && !clash.diagnostics.empty() && has(clash.diagnostics[0].message, "'Api' is reserved") &&
                      has(clash.diagnostics[0].message, "option 'class'"),
                  "P40 s2: a user identifier spelled like the configured class is refused");
            EmitResult dflt = compile("record PolyglotProgram(x: i32)\n", findTarget("csharp"));
            check(!dflt.ok, "P40 s2: the DEFAULT class name stays reserved (via the option default)");
            EmitResult freed = compile("record PolyglotProgram(x: i32)\n", findTarget("csharp"), nullptr,
                                       optsFor("csharp", {{"class", "Api"}}));
            check(freed.ok, "P40 s2: renaming the wrapper frees the old name");
        }

        // Closure consistency: a closure-scoped option must agree across files that import each other.
        {
            MapModuleResolver res({{"lib", "fn twice(x: i32): i32 => x * 2\n"}});
            LibConfig split;
            split.targetOptions = [](const std::string& t, const std::string& origin) {
                if (t != "csharp") return OptMap{};
                return origin.empty() ? OptMap{{"namespace", "A"}} : OptMap{{"namespace", "B"}};
            };
            EmitResult r = compile("import { twice } from \"lib\"\nfn f(): i32 => twice(2)\n", findTarget("csharp"),
                                   &res, split, "entry.pg");
            check(!r.ok && !r.diagnostics.empty() && has(r.diagnostics[0].message, "must agree across one import closure") &&
                      has(r.diagnostics[0].message, "csharp.namespace"),
                  "P40 s2: files of one closure resolving different namespaces are refused");
            EmitResult same = compile("import { twice } from \"lib\"\nfn f(): i32 => twice(2)\n", findTarget("csharp"),
                                      &res, optsFor("csharp", {{"namespace", "A"}}), "entry.pg");
            check(same.ok && same.modules.size() == 1 && has(same.code, "namespace A;") &&
                      has(same.modules[0].code, "namespace A;"),
                  "P40 s2: an agreeing closure puts every emitted file in the namespace");
        }

        // PHP: `namespace X;` right after <?php, before the prelude slot (the guarded helpers follow it).
        {
            EmitResult p = compile("import { print, readText } from \"std.io\"\nfn main() { print(readText(\"a\")) }\n",
                                   findTarget("php"), nullptr, optsFor("php", {{"namespace", "My\\Lib"}}));
            const std::size_t ns = p.code.find("<?php\n// <auto-generated/>\n// Generated by Polyglot. Do not edit: "
                                               "regenerate with `polyglot build`.\nnamespace My\\Lib;\n");
            check(p.ok && ns == 0 && p.code.find("function_exists(__NAMESPACE__") > ns,
                  "P40 s2: PHP namespace follows <?php and precedes the guarded prelude");
        }

        // TS: a file whose declarations are all (unexported) prelude stays an ES module.
        {
            EmitResult empty = compile("import { readText } from \"std.io\"\n", findTarget("typescript"));
            check(empty.ok && has(empty.code, "export {};"), "P40 s2: a TS file with no exports emits `export {};`");
            EmitResult some = compile("fn f(): i32 => 1\n", findTarget("typescript"));
            check(some.ok && !has(some.code, "export {};"), "P40 s2: a TS file with exports emits no `export {};`");
        }

        // Loader: the options block is validated, and a rule may only read declared options.
        {
            auto manifest = [](const char* target) {
                std::string m;
                cli::readFile(cli::executablePath().parent_path() / "plugins" / target / "polyglot-plugin.json", m);
                // The swaps below spell multi-line fragments with "\n"; a CRLF checkout (core.autocrlf) must not
                // make them silently miss.
                m.erase(std::remove(m.begin(), m.end(), '\r'), m.end());
                return m;
            };
            auto swap = [](std::string s, const std::string& from, const std::string& to) {
                const std::size_t at = s.find(from);
                if (at != std::string::npos) s.replace(at, from.size(), to);
                return s;
            };
            std::string err;
            std::string php = manifest("php"); // renamed everywhere (top-level + spec.name must agree)
            for (std::size_t at = 0; (at = php.find("\"name\": \"php\"", at)) != std::string::npos;)
                php.replace(at, 13, "\"name\": \"p40s2php\"");
            check(!validateBackend(swap(php, "\"namespace\": {\n      \"default\": \"\",",
                                        "\"namespace\": {\n      \"default\": \"\", \"colour\": \"x\","), err) &&
                      has(err, "unknown key 'colour'"),
                  "P40 s2: an unknown key in an option declaration is refused");
            check(!validateBackend(swap(php, "\"default\": \"\",", "\"default\": \"a-b\","), err) &&
                      has(err, "does not match its own pattern"),
                  "P40 s2: a default failing its own pattern is refused");
            check(!validateBackend(swap(php, "\"scope\": \"closure\"", "\"scope\": \"file\""), err) &&
                      has(err, "'scope' must be"),
                  "P40 s2: an unknown scope is refused");
            check(!validateBackend(swap(php, "\"get\": \"option.namespace\"", "\"get\": \"option.nmspace\""), err) &&
                      has(err, "option.nmspace"),
                  "P40 s2: a rule reading an undeclared option is refused");
            check(!validateBackend(swap(php, "\"pattern\": \"^(", "\"pattern\": \"^(("), err) &&
                      has(err, "not a valid regular expression"),
                  "P40 s2: an uncompilable pattern is refused");
        }

        // CLI layering: project < include rule < --target-option; flags parse; validation names the layer.
        {
            namespace fs = std::filesystem;
            const fs::path proj = (fs::temp_directory_path() / "p40-s2-opts" / "proj").lexically_normal();
            cli::PgConfig pc;
            pc.found = true;
            pc.dir = proj;
            pc.targetOptions["csharp"] = {{"namespace", "Proj"}, {"access", "public"}};
            cli::IncludeRule rule{"libs/**/*.pg", {}, "gen/%(Filename)"};
            rule.targetOptions["csharp"] = {{"namespace", "Proj.Libs"}};
            pc.include.push_back(rule);
            cli::TargetOptionsByTarget flags;
            std::string err;
            check(cli::parseTargetOptionFlag("csharp.class=Api", flags, err) && flags["csharp"]["class"] == "Api",
                  "P40 s2: --target-option <target>.<name>=<value> parses");
            check(cli::parseTargetOptionFlag("csharp.namespace=A.B=C", flags, err) && flags["csharp"]["namespace"] == "A.B=C",
                  "P40 s2: --target-option keeps everything after the first '=' as the value");
            check(!cli::parseTargetOptionFlag("csharp=x", flags, err) && !cli::parseTargetOptionFlag("namespace", flags, err),
                  "P40 s2: a malformed --target-option is refused");
            const auto top = cli::fileTargetOptions(pc, {}, proj / "main.pg", "csharp");
            const auto lib = cli::fileTargetOptions(pc, {}, proj / "libs/x/strings.pg", "csharp");
            check(top.at("namespace") == "Proj" && lib.at("namespace") == "Proj.Libs" && lib.at("access") == "public",
                  "P40 s2: an include rule's targetOptions overlay the project level for the files it matches");
            cli::TargetOptionsByTarget f2;
            f2["csharp"]["namespace"] = "Flag";
            check(cli::fileTargetOptions(pc, f2, proj / "libs/x/strings.pg", "csharp").at("namespace") == "Flag",
                  "P40 s2: --target-option wins over every pgconfig layer");
            check(cli::fileTargetOptions(pc, {}, proj / "main.pg", "php").empty(),
                  "P40 s2: another target's options never leak (a lookup by target name)");
            std::vector<std::string> errs;
            cli::PgConfig badPc = pc;
            badPc.targetOptions["csharpp"] = {{"namespace", "X"}};
            badPc.include[0].targetOptions["csharp"] = {{"klass", "X"}};
            cli::validateTargetOptions(badPc, {}, errs);
            check(errs.size() == 2 && has(errs[0] + errs[1], "unknown target 'csharpp'") &&
                      has(errs[0] + errs[1], "include rule 'libs/**/*.pg'"),
                  "P40 s2: config validation names the unknown target and the offending include rule");

            // pgconfig parsing: targetOptions shape errors and the removed top-level `access` are refused.
            const fs::path base = fs::temp_directory_path() / "polyglot-p40-s2-cfg";
            fs::create_directories(base);
            cli::writeFile(base / "pgconfig.json",
                           "{ \"access\": \"public\", \"targetOptions\": { \"csharp\": { \"namespace\": 3 } },"
                           " \"include\": [ { \"pattern\": \"*.pg\", \"output\": \"%(Filename)\","
                           " \"targetOptions\": { \"php\": { \"namespace\": \"R\" } } } ] }");
            const cli::PgConfig parsed = cli::loadPgConfig(base);
            bool accessRefused = false, shapeRefused = false;
            for (const auto& e : parsed.errors) {
                accessRefused = accessRefused || has(e, "top-level \"access\" was removed");
                shapeRefused = shapeRefused || has(e, "targetOptions.csharp.namespace must be a string");
            }
            check(accessRefused && shapeRefused && parsed.include.size() == 1 &&
                      parsed.include[0].targetOptions.at("php").at("namespace") == "R",
                  "P40 s2: pgconfig parses rule targetOptions and refuses a bad shape and the old top-level access");
            fs::remove_all(base);
        }
    }

    // ---- P40 slice 3: escapes — one decoder, char validation, per-target `literals`, fmt inverse, JSON -------
    {
        // Lex one literal; return its decoded text and the diagnostics.
        struct Lexed { std::string text; std::vector<Diagnostic> diags; int errors = 0; int warnings = 0; };
        auto lexOne = [](const std::string& src) {
            DiagnosticBag d;
            auto toks = lex(src, d);
            Lexed l;
            if (!toks.empty()) l.text = toks[0].text;
            l.diags = d.items();
            for (const auto& x : l.diags) (x.severity == Severity::Error ? l.errors : l.warnings)++;
            return l;
        };
        auto errAt = [](const Lexed& l, int col, const std::string& frag) {
            for (const auto& x : l.diags)
                if (x.severity == Severity::Error && x.pos.col == col && has(x.message, frag)) return true;
            return false;
        };

        // The decode table: every accepted escape, in one literal.
        std::string want = "\n\t\r";
        want += '\0';
        want += "\\\"'$A\xF0\x9F\x98\x80";
        Lexed all = lexOne("\"\\n\\t\\r\\0\\\\\\\"\\'\\$\\u{41}\\u{1F600}\"");
        check(all.errors == 0 && all.text == want, "P40 s3: decode table (\\n \\t \\r \\0 \\\\ \\\" \\' \\$ \\u{41} \\u{1F600})");
        check(lexOne("\"\\u{00001D}\"").text == "\x1D" && lexOne("\"\\u{1d}\"").text == "\x1D",
              "P40 s3: \\u{} takes 1-6 hex digits, either case");
        check(lexOne("\"\\${y}\"").text == "${y}", "P40 s3: \\${ is a literal ${, not a hole");

        // Every refused escape is an error AT THE BACKSLASH (col 4 in "ab\…").
        Lexed q = lexOne("\"ab\\qc\" x");
        check(q.errors == 1 && errAt(q, 4, "unknown escape sequence `\\q`") && errAt(q, 4, "valid escapes are"),
              "P40 s3: unknown escape -> one error at the backslash naming the valid set");
        check(errAt(lexOne("\"ab\\u0041\""), 4, "write the code point in braces"),
              "P40 s3: \\uXXXX refused with the guiding message");
        check(errAt(lexOne("\"ab\\x41\""), 4, "`\\u{HH}`"), "P40 s3: \\xHH refused with the guiding message");
        check(errAt(lexOne("\"ab\\u{}\""), 4, "malformed") && errAt(lexOne("\"ab\\u{1234567}\""), 4, "malformed") &&
                  errAt(lexOne("\"ab\\u{41\""), 4, "malformed"),
              "P40 s3: empty / 7-digit / unclosed \\u{} are malformed");
        check(errAt(lexOne("\"ab\\u{110000}\""), 4, "beyond U+10FFFF"), "P40 s3: \\u{110000} is out of range");
        check(errAt(lexOne("\"ab\\u{D83D}\""), 4, "surrogate"), "P40 s3: a surrogate \\u{} is refused (PHP would write WTF-8)");
        check(errAt(lexOne("\"ab\xFF\""), 4, "invalid UTF-8"), "P40 s3: invalid UTF-8 in a literal is an error");
        check(errAt(lexOne("'\xC3'"), 2, "invalid UTF-8"), "P40 s3: invalid UTF-8 in a char literal is an error");

        // Char literals: exactly one character that fits one UTF-16 code unit.
        check(errAt(lexOne("''"), 1, "empty char literal"), "P40 s3: an empty char literal is an error");
        check(errAt(lexOne("'ab'"), 1, "more than one character"), "P40 s3: a multi-character char literal is an error");
        check(errAt(lexOne("'\\u{1F600}'"), 1, "one UTF-16 code unit"), "P40 s3: an astral char literal is an error");
        Lexed ca = lexOne("'\\u{41}'");
        check(ca.errors == 0 && ca.warnings == 0 && ca.text == "A", "P40 s3: '\\u{41}' is the char A");
        Lexed ce = lexOne("'\\u{E9}'");
        check(ce.errors == 0 && ce.warnings == 0 && ce.text == "\xC3\xA9", "P40 s3/s8: '\\u{E9}' decodes, no warning (UTF-16 faithful everywhere)");
        check(lexOne("'\\''").text == "'" && lexOne("'\"'").text == "\"" && lexOne("'\\\"'").text == "\"",
              "P40 s3: quotes in chars, escaped or not");

        // P40 s8: non-ASCII text is faithful on every target now — no warning, decoded or raw.
        Lexed w = lexOne("\"caf\\u{E9} \\u{E8}\"");
        check(w.errors == 0 && w.warnings == 0, "P40 s8: a decoded non-ASCII escape no longer warns");
        check(lexOne("\"plain \\u{7F}\"").warnings == 0, "P40 s3: ASCII escapes do not warn");
        {
            EmitResult ok = compile("extension fn i32.twice(): i32 => this * 2\n", findTarget("php"));
            bool warned = false;
            for (const auto& d : ok.diagnostics) warned = warned || (d.severity == Severity::Warning && has(d.message, "emulates extensionMethods"));
            check(ok.ok && warned, "P40 s3: a SUCCESSFUL compile still reports its warnings to the host");
        }

        // Emission: per-target goldens of the literal text. The Core picks WHICH characters escape (C0, DEL,
        // C1 incl. U+0085 NEL, U+2028); printable non-ASCII (é, 😀) passes through; PHP escapes `$`.
        const std::string body = "fn s(): string => \"a\\u{1D}\\u{85}\\u{2028}\\u{E9}$x\\u{1F600}\\u{0}1\"\n";
        auto emitted = [&](const char* target) { return compile(body, findTarget(target)).code; };
        check(has(emitted("csharp"), "\"a\\u001D\\u0085\\u2028\xC3\xA9$x\xF0\x9F\x98\x80\\u00001\""),
              "P40 s3: C# literal golden (\\u$H4, raw printable non-ASCII, NUL never \\0)");
        check(has(emitted("typescript"), "\"a\\u{1D}\\u{85}\\u{2028}\xC3\xA9$x\xF0\x9F\x98\x80\\u{0}1\""),
              "P40 s3: TS literal golden (\\u{$H})");
        check(has(emitted("python"), "\"a\\u001D\\u0085\\u2028\xC3\xA9$x\xF0\x9F\x98\x80\\u00001\""),
              "P40 s3: Python literal golden (\\u$H4)");
        check(has(emitted("php"), "\"a\\u{1D}\\u{85}\\u{2028}\xC3\xA9\\$x\xF0\x9F\x98\x80\\u{0}1\""),
              "P40 s3: PHP literal golden ($ -> \\$, the \"cost $x\" miscompile)");
        check(compile("fn s(): string => \"plain ascii \\\"q\\\" \\\\ \\n\"\n", findTarget("csharp")).code.find(
                  "\"plain ascii \\\"q\\\" \\\\ \\n\"") != std::string::npos,
              "P40 s3: plain ASCII literals are byte-identical to the pre-P40 output");
        const std::string interp = "fn f(x: i32): string => \"{${x}}`$\\u{2028}\"\n";
        check(has(compile(interp, findTarget("csharp")).code, "$\"{{{__PgRt.Str(x)}}}`$\\u2028\""),
              "P40 s3: C# interp context doubles braces and escapes U+2028");
        check(has(compile(interp, findTarget("typescript")).code, "`{${x}}\\`$\\u{2028}`"),
              "P40 s3: TS interp context escapes the backtick");
        check(has(compile(interp, findTarget("php")).code, "\"}`\\$\\u{2028}\""),
              "P40 s3: PHP interp chunks escape $");
        check(has(compile("fn c(): char => '\\''\n", findTarget("csharp")).code, "'\\''") &&
                  has(compile("fn c(): char => '\\u{1D}'\n", findTarget("csharp")).code, "'\\u001D'"),
              "P40 s3: C# char context (quote ', \\u$H4)");

        // A bound template's receiver placeholder takes the receiver parenthesization policy (found by
        // escapes.pg: `("a\0" + "1").len()` emitted `"a\u0000" + "1".Length` on C# and TS).
        {
            const char* lp = "import \"std.strings\"\nfn n(): i32 => (\"a\" + \"b\").len()\n";
            check(has(compileStd(lp, findTarget("csharp")).code, "(\"a\" + \"b\").Length") &&
                      has(compileStd(lp, findTarget("typescript")).code, "(\"a\" + \"b\").length"),
                  "P40 s3: a binary receiver of a bound member template is parenthesized");
        }

        // The astral template, and the "surrogates" default.
        auto specOf = [](const char* target) {
            std::string m;
            cli::readFile(cli::executablePath().parent_path() / "plugins" / target / "polyglot-plugin.json", m);
            return loadBackendSpec(json::parse(m)["spec"]);
        };
        SpecLoadResult cs = specOf("csharp"), ts = specOf("typescript"), py = specOf("python"), php = specOf("php");
        check(cs.ok && ts.ok && py.ok && php.ok, "P40 s3: the four in-box specs load");
        check(specCodepointEscape(cs.spec.literals.at("string"), 0x1F600) == "\\U0001F600" &&
                  specCodepointEscape(ts.spec.literals.at("string"), 0x1F600) == "\\u{1F600}" &&
                  specCodepointEscape(py.spec.literals.at("string"), 0x1F600) == "\\U0001F600" &&
                  specCodepointEscape(php.spec.literals.at("string"), 0x1F600) == "\\u{1F600}",
              "P40 s3: astral templates (C#/Python \\U$H8, TS/PHP \\u{$H})");
        check(specCodepointEscape(cs.spec.literals.at("char"), 0x1F600) == "\\uD83D\\uDE00",
              "P40 s3: no astral template = UTF-16 surrogate halves through `codepoint`");

        // Serializer round-trip of the literals block.
        SpecLoadResult back = loadBackendSpec(backendSpecToJson(php.spec));
        bool same = back.ok && back.spec.literals.size() == php.spec.literals.size();
        for (const auto& kv : php.spec.literals) {
            auto it = back.spec.literals.find(kv.first);
            same = same && it != back.spec.literals.end() && it->second.quote == kv.second.quote &&
                   it->second.map == kv.second.map && it->second.codepoint == kv.second.codepoint &&
                   it->second.astral == kv.second.astral;
        }
        check(same, "P40 s3: spec.literals round-trips through backendSpecToJson");

        // Loader refusals (anti-silent-drop): codepoint required, string context required, a rule naming an
        // undeclared context, the retired `escapes` block, a map not escaping its quote.
        std::string csm;
        cli::readFile(cli::executablePath().parent_path() / "plugins" / "csharp" / "polyglot-plugin.json", csm);
        auto refused = [&](const std::string& from, const std::string& to, const std::string& frag) {
            const std::size_t at = csm.find(from);
            if (at == std::string::npos) return false;
            std::string m = csm;
            m.replace(at, from.size(), to);
            std::string err;
            return !validateBackend(m, err) && has(err, frag);
        };
        check(refused("\"codepoint\": \"\\\\u$H4\",", "", "needs a 'codepoint'"),
              "P40 s3: a literal context without a codepoint template is refused");
        check(refused("\"codepoint\": \"\\\\u$H4\"", "\"codepoint\": \"\\\\x$H2\"", "needs a 'codepoint'"),
              "P40 s3: a \\x$H2 codepoint template (cannot spell U+2028) is refused");
        check(refused("\"string\": {", "\"strung\": {", "'string' context"),
              "P40 s3: a manifest without a string literal context is refused");
        check(refused("\"interp\",", "\"interpx\",", "'interpx'"), // (the first `"interp",` is a literal context use;
                                                                     //  wrapAtom.recvPrimary spells it last, comma-free)
              "P40 s3: a rule naming an undeclared literal context is refused");
        check(refused("\"literals\": {", "\"escapes\": {}, \"literals\": {", "replaced by 'literals'"),
              "P40 s3: the retired `escapes` block is refused, naming its replacement");
        check(refused("\"\\\"\": \"\\\\\\\"\",", "", "must escape the backslash and the context's quote"),
              "P40 s3: a context whose map does not escape its quote is refused");

        // `polyglot fmt` is the exact inverse of the lexer: `\${`, `\0`, `\u{…}` for non-printables, raw é.
        std::string fsrc = "fn f(x: i32): string => \"a\\${b} \\u{1D}\\0\\u{2028} \\u{E9} \\$ ${x}\"\n"
                           "fn c(): char => '\\u{0}'\n";
        EmitResult f1 = format(fsrc);
        EmitResult f2 = format(f1.code);
        check(f1.ok && f2.ok && f1.code == f2.code, "P40 s3: fmt is idempotent over escapes");
        check(has(f1.code, "\"a\\${b} \\u{1D}\\0\\u{2028} \xC3\xA9 $ ${x}\"") && has(f1.code, "'\\0'"),
              "P40 s3: fmt prints \\${, \\0, \\u{H} for non-printables and raw printable non-ASCII");
        auto lits = [](const std::string& src) {
            DiagnosticBag d;
            std::vector<std::string> out;
            for (const auto& t : lex(src, d))
                if (t.kind == TokKind::InterpStart || t.kind == TokKind::InterpEnd || t.kind == TokKind::StringLit ||
                    t.kind == TokKind::CharLit)
                    out.push_back(t.text);
            return out;
        };
        check(lits(fsrc) == lits(f1.code), "P40 s3: fmt output re-lexes to the same decoded literal values");

        // The Core JSON reader: an escape JSON does not define is an error, never a silently dropped backslash.
        std::string jerr;
        json::Value jv = json::parse("{\"lib\":\"Libs\\Alpha\"}", jerr);
        check(has(jerr, "invalid escape '\\A'") && jv["lib"].asString() == "Libs\\Alpha",
              "P40 s3: JSON `\"Libs\\Alpha\"` is a parse error (and never reads as LibsAlpha)");
        jerr.clear();
        json::parse("{\"a\":\"x\\\\y\\u0041\\n\"}", jerr);
        check(jerr.empty(), "P40 s3: valid JSON escapes report no error");
        jerr.clear();
        json::parse("{\"a\":\"\\u00G1\"}", jerr);
        check(has(jerr, "hex digits"), "P40 s3: a malformed JSON \\u escape is an error");

        // P40 review (A5): STRUCTURAL malformations are errors too — `{` used to parse as an empty object with no
        // error, so a truncated pgconfig.json was silently ignored and the build exited 0.
        bool allRefused = true;
        for (const char* bad : {"{", "", "   ", "{\"a\":1", "{\"a\" 1}", "{\"a\":1,}", "[1,]", "[1 2]", "{\"a\":tru}",
                                "{\"a\":01}", "{\"a\":1.}", "{\"a\":+1}", "{\"a\":\"x}", "{} x", "{\"a\":1}}", "{a:1}"}) {
            jerr.clear();
            json::parse(bad, jerr);
            if (jerr.empty()) { allRefused = false; std::cerr << "    not refused: '" << bad << "'\n"; }
        }
        check(allRefused, "P40 review A5: structurally malformed JSON reports an error (truncation, separators, literals, numbers, trailing content)");
        bool allAccepted = true;
        for (const char* good : {"{}", "[]", " {\"a\": [1, -2.5e+3, 0, true, false, null, \"s\"], \"b\": {}} \n",
                                 "\xEF\xBB\xBF{\"targets\": [\"csharp\"]}", "0", "\"x\""}) {
            jerr.clear();
            json::parse(good, jerr);
            if (!jerr.empty()) { allAccepted = false; std::cerr << "    refused: '" << good << "': " << jerr << "\n"; }
        }
        check(allAccepted, "P40 review A5: well-formed JSON (incl. a UTF-8 BOM) reports no error");
        jerr.clear();
        check(json::parse("\xEF\xBB\xBF{\"targets\": [\"csharp\"]}", jerr)["targets"].items().size() == 1,
              "P40 review A5: a BOM-prefixed document parses to its value (it used to read as a number 0)");
    }

    // P40 slice 4 (A2) — Rust-style range precedence, evaluate-once bounds, the super refusal, the parser
    // progress guard, and the lowering backstop.
    {
        auto parseSrc = [](const std::string& src, DiagnosticBag& d) { return parse(lex(src, d), d); };
        auto firstErr = [](const EmitResult& r) { return r.diagnostics.empty() ? std::string() : r.diagnostics[0].message; };

        // Parse shapes: the range is the loosest binary form, so every binary bound stays inside it.
        auto iterOf = [&](const std::string& bound, DiagnosticBag& d) -> CompilationUnit {
            return parseSrc("fn f(n: i32, a: i32?) {\n  for i in " + bound + " { }\n}\n", d);
        };
        struct Shape { const char* src; const char* lhs; const char* rhs; bool inclusive; };
        const Shape shapes[] = {
            {"0..n - 1", "IntLit", "-", false}, {"-2..2", "-", "IntLit", false},
            {"0..=n * 2", "IntLit", "*", true}, {"i + 1..n", "+", "Name", false},
            {"0..a ?? 3", "IntLit", "??", false},
        };
        for (const auto& sh : shapes) {
            DiagnosticBag d;
            CompilationUnit u = iterOf(sh.src, d);
            const Expr* r = (!d.hasErrors() && !u.functions.empty() && !u.functions[0].body.empty())
                                ? u.functions[0].body[0]->value.get() : nullptr;
            auto tag = [](const Expr& e) -> std::string {
                if (e.kind == ExprKind::Binary || e.kind == ExprKind::Unary) return e.text;
                return e.kind == ExprKind::IntLit ? "IntLit" : e.kind == ExprKind::Name ? "Name" : "?";
            };
            check(r && r->kind == ExprKind::Range && tag(*r->lhs) == sh.lhs && tag(*r->rhs) == sh.rhs &&
                      r->flag == sh.inclusive,
                  std::string("P40 s4: `") + sh.src + "` parses as one range with binary bounds");
        }
        {
            DiagnosticBag d;
            parseSrc("fn f() {\n  for i in 0..1..2 { }\n}\n", d);
            check(d.hasErrors() && has(d.items()[0].message, "ranges do not chain"), "P40 s4: ranges do not chain");
        }

        // S8: the throw arm used to hang the parser forever; now one targeted error (and the parse ends).
        {
            DiagnosticBag d;
            parseSrc("union O { Ok(v: i32), Fail }\nfn f(o: O): i32 => match o {\n  Ok(v) => v,\n  Fail => throw Error(\"x\")\n}\n", d);
            check(d.items().size() == 1 && has(d.items()[0].message, "'throw' is a statement"),
                  "P40 s4: a throw match arm is one diagnostic, not a parser hang");
            DiagnosticBag d2;
            parseSrc("enum E { 1 }\nunion U { 2 }\n", d2);
            check(d2.hasErrors(), "P40 s4: enum/union case loops have a progress guard (no hang)");
        }

        // The formatter stops parenthesizing binary bounds, and stays idempotent.
        {
            const std::string src = "fn f(n: i32, a: i32?) {\n  for j in (n + 1)..n { }\n  for i in (0 - 2)..=(n * 2) { }\n"
                                    "  for k in 0..(a ?? 5) { }\n}\n";
            EmitResult f1 = format(src);
            EmitResult f2 = format(f1.code);
            check(f1.ok && has(f1.code, "for j in n + 1..n") && has(f1.code, "for i in 0 - 2..=n * 2") &&
                      has(f1.code, "for k in 0..a ?? 5"),
                  "P40 s4: fmt prints range bounds without parentheses");
            check(f2.ok && f1.code == f2.code, "P40 s4: fmt is idempotent over ranges");
        }

        // Sema: a range outside a for iterable, float bounds, incompatible bounds, and the super refusal.
        EmitResult op = compile("fn f(n: i32): i32 => (0..n) - 1\n", findTarget("csharp"));
        check(!op.ok && op.diagnostics.size() == 1 && has(firstErr(op), "a range ('a..b') is only valid as a for-loop iterable"),
              "P40 s4: a range operand is one error (no cascade)");
        EmitResult lt = compile("fn f() {\n  let r = 0..5\n}\n", findTarget("csharp"));
        check(!lt.ok && has(firstErr(lt), "only valid as a for-loop iterable"), "P40 s4: a range in a let is an error");
        EmitResult fl = compile("fn f() {\n  var n = 0\n  for x in 1.5..3 { n += 1 }\n}\n", findTarget("csharp"));
        check(!fl.ok && has(firstErr(fl), "range bounds must be integers (got f64)"), "P40 s4: float range bounds are an error");
        EmitResult mix = compile("fn f(a: i64, b: u64) {\n  for i in a..b { }\n}\n", findTarget("csharp"));
        check(!mix.ok && has(firstErr(mix), "incompatible types i64 and u64"), "P40 s4: an incompatible bound pair is an error");
        EmitResult sm = compile("open class A {\n  open fn g(): i32 => 1\n}\nclass B : A {\n  override fn g(): i32 => super.g() + 1\n}\n",
                                findTarget("csharp"));
        check(!sm.ok && has(firstErr(sm), "'super' is only supported as a base-constructor call"), "P40 s4: super.m() is refused (D6)");
        EmitResult sc = compileStd("class E2 : Error {\n  constructor() {\n    super(\"x\")\n  }\n}\n", findTarget("csharp"));
        check(sc.ok, "P40 s4: the hoisted base-constructor super(...) stays legal");

        // Widening + D7: the i32 start widens to the i64 end; a non-simple end is hoisted with the spelled type.
        const char* wide = "fn f(a: i64, b: i64): i64 {\n  var s: i64 = 0\n  for k in 0..a + b { s += k }\n"
                           "  for j in 0..a { s += j }\n  return s\n}\n";
        EmitResult wcs = compile(wide, findTarget("csharp"));
        check(wcs.ok && has(wcs.code, "for (long k = ") && has(wcs.code, ", __end0 = a + b; k < __end0; k++)"),
              "P40 s4: C# hoists a non-simple end into a multi-declarator with the widened (long) type");
        check(wcs.ok && has(wcs.code, "for (var j = ") && has(wcs.code, "; j < a; j++)"),
              "P40 s4: a parameter end stays readable (re-read, not hoisted)");
        EmitResult wts = compile(wide, findTarget("typescript"));
        check(wts.ok && has(wts.code, "for (let k = BigInt(0), __end0 = BigInt.asIntN(64, a + b); k < __end0; k++)"),
              "P40 s4: TS hoists with `let` (same shape)");
        EmitResult wphp = compile(wide, findTarget("php"));
        check(wphp.ok && has(wphp.code, ", $__end0 = $a + $b; $k < $__end0; $k++)"), "P40 s4: PHP hoists with a comma init");
        const char* mut = "fn f(): i32 {\n  var m = 3\n  var c = 0\n  for i in 0..m { m += 1\n c += 1 }\n  return c\n}\n";
        EmitResult mcs = compile(mut, findTarget("csharp"));
        check(mcs.ok && has(mcs.code, "__end0 = m;"), "P40 s4: a mutable-local end is hoisted (evaluated once)");

        // The backstop: lowering records an expression it has no rule for (sema refuses these first, so the
        // AST is fed to lower() directly), and compile() would turn the record into an ICE.
        {
            DiagnosticBag d;
            CompilationUnit u = parseSrc("fn f() {\n  let r = 0..5\n  let s = super\n}\n", d);
            ir::Module m = lower(u, "csharp");
            check(m.loweringFailures.size() == 2 && m.loweringFailures[0].kind == "range" &&
                      m.loweringFailures[1].kind == "super" && m.loweringFailures[0].pos.line == 2,
                  "P40 s4: lowering records each unlowerable expression with its kind and position");
            DiagnosticBag d2;
            ir::Module ok = lower(parseSrc("fn f(n: i32) {\n  for i in 0..n { }\n}\n", d2), "csharp");
            check(ok.loweringFailures.empty(), "P40 s4: a for-iterable range lowers without a failure");
        }
    }

}
