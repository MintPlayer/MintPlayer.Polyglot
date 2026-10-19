// P41 (issue #76) — deterministic per-module output. Compiler temporaries (`__end`, `__opt`, `__c`, `__pg_t`,
// `__dw`, `__pg_lam`, `__cs`) number PER BODY (PRD D1-D3), so an imported module's emitted text is a function
// of its own source — never of which root reached it or how many temps that root spent first. These pins are
// the unit half of D8; the CLI smoke test (slice 5) proves the same through `build` + `build --check`.

#include "test_support.hpp"

namespace {

// The emitted text of the imported module named `basename` ("" when it was not emitted as its own file).
std::string moduleCode(const EmitResult& r, const std::string& basename) {
    for (const auto& m : r.modules) if (m.basename == basename) return m.code;
    return "";
}

std::size_t countOf(const std::string& hay, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + needle.size())) ++n;
    return n;
}

} // namespace

void run_p41_determinism() {
    // ---- D8 (a): `lib` compiled alone, via root A (one loop), via root B (two loops) — byte-identical --------
    // The issue's shape: both roots spend `__end` temps on their own range loops before reaching lib's loop.
    // Pre-P41 the counter ran module-wide over the closure, so lib's bound was `__end1` via A and `__end2` via B.
    {
        const char* lib =
            "import \"std.strings\"\n"
            "fn countUnits(s: string): i32 {\n  var n = 0\n  for i in 0..s.len() { n = n + i }\n  return n\n}\n";
        const char* rootA =
            "import \"std.strings\"\nimport { countUnits } from \"./lib\"\n"
            "fn main() {\n  let s = \"abc\"\n  var n = 0\n  for i in 0..s.len() { n = n + i }\n"
            "  print(n + countUnits(s))\n}\n";
        const char* rootB =
            "import \"std.strings\"\nimport { countUnits } from \"./lib\"\n"
            "fn main() {\n  let s = \"abc\"\n  var n = 0\n  for i in 0..s.len() { n = n + i }\n"
            "  for j in 0..s.len() { n = n + j }\n  print(n + countUnits(s))\n}\n";
        MapModuleResolver res({{"./lib", lib}});
        struct Pin { const char* target; const char* end0; const char* end1; };
        for (const Pin& p : {Pin{"typescript", "__end0", "__end1"}, Pin{"csharp", "__end0", "__end1"},
                             Pin{"php", "$__end0", "$__end1"}}) {
            const std::string t = p.target;
            EmitResult a = compile(rootA, findTarget(p.target), &res, LibConfig{{"io"}}, "a.pg");
            EmitResult b = compile(rootB, findTarget(p.target), &res, LibConfig{{"io"}}, "b.pg");
            EmitResult alone = compile(lib, findTarget(p.target), &res, LibConfig{{"io"}}, "./lib");
            check(a.ok && b.ok && alone.ok, "P41 #76: lib via A, via B and alone all compile on " + t);
            const std::string viaA = moduleCode(a, "lib"), viaB = moduleCode(b, "lib");
            check(!viaA.empty() && viaA == viaB, "P41 #76: lib's text is byte-identical via root A and root B on " + t);
            check(!viaA.empty() && viaA == alone.code, "P41 #76: lib's text via a root equals lib compiled alone on " + t);
            check(has(viaA, p.end0) && !has(viaA, p.end1),
                  "P41 #76: lib's loop bound is the body-local `__end0` (never the root's continuation) on " + t);
            // The roots themselves still number from 0: A spends one temp, B two.
            check(has(a.code, p.end0) && !has(a.code, p.end1), "P41 #76: root A's own loop is `__end0` on " + t);
            check(has(b.code, p.end0) && has(b.code, p.end1), "P41 #76: root B's two loops are `__end0`/`__end1` on " + t);
        }
        // Python's range loop needs no temp; the identity must hold all the same.
        EmitResult a = compile(rootA, findTarget("python"), &res, LibConfig{{"io"}}, "a.pg");
        EmitResult b = compile(rootB, findTarget("python"), &res, LibConfig{{"io"}}, "b.pg");
        EmitResult alone = compile(lib, findTarget("python"), &res, LibConfig{{"io"}}, "./lib");
        const std::string viaA = moduleCode(a, "lib");
        check(a.ok && b.ok && alone.ok && !viaA.empty() && viaA == moduleCode(b, "lib") && viaA == alone.code,
              "P41 #76: lib's Python text is identical via A, via B and alone");
        check(!has(viaA, "__end"), "P41 #76: Python's range loop spends no `__end` temp");
    }

    // ---- D8 (a'): the shift through `maybe(x)!` — a null-assert in the root used to move lib's numbering ----
    {
        const char* lib =
            "fn maybe(x: i32): i32? {\n  if x > 0 { return x }\n  return null\n}\n"
            "fn useIt(x: i32): i32 {\n  var n = 0\n  for i in 0..maybe(x)! { n = n + i }\n  return n\n}\n";
        const char* rootA =
            "import { maybe, useIt } from \"./lib\"\n"
            "fn main() {\n  let a = maybe(1)!\n  print(useIt(a))\n}\n";
        const char* rootB =
            "import { maybe, useIt } from \"./lib\"\n"
            "fn main() {\n  let a = maybe(1)!\n  let b = maybe(2)!\n  print(useIt(a + b))\n}\n";
        MapModuleResolver res({{"./lib", lib}});
        for (const char* target : {"typescript", "csharp"}) {
            const std::string t = target;
            EmitResult a = compile(rootA, findTarget(target), &res, LibConfig{{"io"}}, "a.pg");
            EmitResult b = compile(rootB, findTarget(target), &res, LibConfig{{"io"}}, "b.pg");
            EmitResult alone = compile(lib, findTarget(target), &res, LibConfig{{"io"}}, "./lib");
            check(a.ok && b.ok && alone.ok, "P41 #76 (!-shift): lib via A, via B and alone all compile on " + t);
            const std::string viaA = moduleCode(a, "lib");
            check(!viaA.empty() && viaA == moduleCode(b, "lib") && viaA == alone.code,
                  "P41 #76 (!-shift): a root's null-asserts do not renumber lib's temps on " + t);
            // The null-assert in the range end reserves the body's first slot (`__nn0`, spent by the targets that
            // emit it) and the loop bound takes the second — the same number in every closure, which is the point.
            check(has(viaA, "__end1") && !has(viaA, "__end2") && !has(viaA, "__end0"),
                  "P41 #76 (!-shift): lib's `for i in 0..maybe(x)!` bound is `__end1` after the null-assert's slot on " + t);
        }
    }

    // ---- D8 (c): sibling bodies each restart at 0; a lambda shares its enclosing body's allocator (D1) --------
    {
        const char* siblings =
            "import \"std.strings\"\n"
            "fn f(s: string): i32 {\n  var n = 0\n  for i in 0..s.len() { n = n + i }\n  return n\n}\n"
            "fn g(s: string): i32 {\n  var n = 0\n  for i in 0..s.len() { n = n + i }\n  return n\n}\n";
        EmitResult ts = compile(siblings, findTarget("typescript"), nullptr, LibConfig{{"io"}});
        check(ts.ok && countOf(ts.code, "__end0 = ") == 2 && !has(ts.code, "__end1"),
              "P41 #76: two sibling functions with one range loop each both hoist `__end0` (TS)");
        EmitResult cs = compile(siblings, findTarget("csharp"), nullptr, LibConfig{{"io"}});
        check(cs.ok && countOf(cs.code, "__end0 = ") == 2 && !has(cs.code, "__end1"),
              "P41 #76: two sibling functions with one range loop each both hoist `__end0` (C#)");

        // A lambda is NOT a body for numbering: on C# a nested scope redeclaring an enclosing local is CS0136,
        // and on Python/PHP (function scope) a reused name would alias — so the lambda's temp continues at 1.
        const char* nested =
            "import \"std.strings\"\n"
            "fn f(s: string): i32 {\n  var n = 0\n"
            "  for i in 0..s.len() {\n"
            "    let inner = (k: i32) => {\n      for j in 0..s.len() { n = n + j + k }\n    }\n"
            "    inner(i)\n  }\n  return n\n}\n";
        EmitResult nts = compile(nested, findTarget("typescript"), nullptr, LibConfig{{"io"}});
        check(nts.ok && has(nts.code, "__end0") && has(nts.code, "__end1") && !has(nts.code, "__end2"),
              "P41 #76: a lambda's loop inside a loop body continues the enclosing body's sequence (`__end1`)");
        check(nts.ok && countOf(nts.code, "__end0 = ") == 1 && countOf(nts.code, "__end1 = ") == 1,
              "P41 #76: the outer loop declares `__end0` once and the lambda's loop declares `__end1` once");
        EmitResult ncs = compile(nested, findTarget("csharp"), nullptr, LibConfig{{"io"}});
        check(ncs.ok && has(ncs.code, "__end0") && has(ncs.code, "__end1") && !has(ncs.code, "__end2"),
              "P41 #76: the shared body allocator holds on C# too (no CS0136 from a reused `__end0`)");
    }

    // ---- D5: a Tier-1 attribute's import line is derived from the FILE's own declarations -------------------
    // Only the root binds `JsonProp`; lib has nothing to annotate. Pre-P41 the imports were collected while
    // lowering the whole closure and filtered afterwards; D5 walks each slice's declarations instead, so lib's
    // output cannot pick up a `using` it has no use for.
    {
        const char* lib = "fn twice(x: i32): i32 => x * 2\n";
        const char* root =
            "import { twice } from \"./lib\"\n"
            "extern attribute JsonProp(name: string) {\n"
            "  actual(csharp) extern(\"[Newtonsoft.Json.JsonProperty($0)]\") import \"using Newtonsoft.Json;\"\n"
            "  actual(typescript) extern(\"@JsonProp($name)\")\n"
            "}\n"
            "[JsonProp(\"x\")]\nclass C {\n  var x: i32\n  constructor() { this.x = 0 }\n}\n"
            "fn main() { print(twice(C().x)) }\n";
        MapModuleResolver res({{"./lib", lib}});
        EmitResult cs = compile(root, findTarget("csharp"), &res, LibConfig{{"io"}}, "root.pg");
        check(cs.ok && has(cs.code, "[Newtonsoft.Json.JsonProperty(\"x\")]") && has(cs.code, "using Newtonsoft.Json;"),
              "P41 D5: the root that binds the Tier-1 attribute gets the annotation line + its using");
        const std::string libCs = moduleCode(cs, "lib");
        check(!libCs.empty() && has(libCs, "twice") && !has(libCs, "Newtonsoft"),
              "P41 D5: the imported module that binds no attribute gets NO `using Newtonsoft.Json;`");
        // And lib alone equals lib via the root (the attribute does not leak as a text difference either).
        EmitResult alone = compile(lib, findTarget("csharp"), &res, LibConfig{{"io"}}, "./lib");
        check(alone.ok && alone.code == libCs, "P41 D5: lib's C# text via the attribute-using root equals lib alone");
        EmitResult ts = compile(root, findTarget("typescript"), &res, LibConfig{{"io"}}, "root.pg");
        check(ts.ok && has(ts.code, "@JsonProp(\"x\")") && !has(moduleCode(ts, "lib"), "JsonProp"),
              "P41 D5: on TS the decorator stays in the root; lib's file never mentions it");
    }
}
