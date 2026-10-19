// P18 BackendSpec + emission DSL, P19 validation + identifier hygiene, P37 feature batch, precedence, P21 watch, P25/P26.
// P41 D14: one translation unit of the unit suite (split from tests_main.cpp so the areas compile in
// parallel). The bodies are unchanged; the shared harness lives in test_support.hpp.

#include "test_support.hpp"

void run_engine_p37() {
    // P18 slice 1 — BackendSpec loads from / serializes to JSON (the tabular ~70% becomes data; PRD §4.10).
    {
        // A representative spec parses; the fields land where expected; blockStyle maps.
        const char* js = R"({ "name":"demo", "scalarType":{"i32":"int"}, "intSuffix":{"i64":"L"},
            "binaryOp":{"==":"==="}, "delimited":{"tuple":{"open":"[","sep":", ","close":"]"}},
            "blockStyle":"colonIndent", "stmtEnd":"", "throwKeyword":"raise",
            "trueLit":"True","falseLit":"False","nullLit":"None", "assignmentOrder":"targetFirst" })";
        SpecLoadResult r = loadBackendSpec(js);
        check(r.ok && r.spec.name == "demo" && r.spec.scalarType.at("i32") == "int" &&
              r.spec.binOp("==") == "===" && r.spec.blockStyle == BlockStyle::ColonIndent &&
              r.spec.stmtEnd == "" && r.spec.throwKeyword == "raise" && r.spec.nullLit == "None" &&
              r.spec.delimited.at("tuple").open == "[",
              "P18: loadBackendSpec parses fields + blockStyle enum");

        // Missing name / unknown blockStyle fail loudly (never a silent bad spec).
        check(!loadBackendSpec(R"({"scalarType":{}})").ok, "P18: spec without 'name' is rejected");
        check(!loadBackendSpec(R"({"name":"x","blockStyle":"squiggly"})").ok, "P18: unknown blockStyle is rejected");
        // P40 review r7: an error pushed without an early return still fails the load (it used to be dropped).
        check(!loadBackendSpec(R"({"name":"x"})").ok, "r7: a spec without the required 'assignmentOrder' is rejected");
        check(!loadBackendSpec(R"({"name":"x","assignmentOrder":"targetFirst","exprStmt":{"bogus":1}})").ok,
              "r7: an unknown exprStmt key fails the load");

        // Round-trip: serialize → parse → equal struct.
        SpecLoadResult back = loadBackendSpec(backendSpecToJson(r.spec));
        check(back.ok && back.spec.name == r.spec.name &&
              back.spec.scalarType == r.spec.scalarType && back.spec.intSuffix == r.spec.intSuffix &&
              back.spec.binaryOp == r.spec.binaryOp && back.spec.blockStyle == r.spec.blockStyle &&
              back.spec.stmtEnd == r.spec.stmtEnd && back.spec.throwKeyword == r.spec.throwKeyword &&
              back.spec.trueLit == r.spec.trueLit && back.spec.falseLit == r.spec.falseLit &&
              back.spec.nullLit == r.spec.nullLit &&
              back.spec.delimited.at("tuple").open == r.spec.delimited.at("tuple").open,
              "P18: BackendSpec round-trips through JSON");
    }

    // P18 slice 3 — the emission-DSL interpreter spine (Design A, PRD §4.10): literal / tmpl / get / case+Test / fn.
    {
        MockEval ctx;
        ctx.fields["node.op"] = "+";
        ctx.fields["node.name"] = "widget";
        ctx.fields["node.kind"] = "Binary";
        // node.guard is absent -> Test has:false

        check(runRule(R"("hello")", ctx) == "hello", "P18: interpreter literal");
        check(runRule(R"({"get":"node.op"})", ctx) == "+", "P18: interpreter get");
        check(runRule(R"({"tmpl":["a ",{"get":"node.op"}," b"]})", ctx) == "a + b", "P18: interpreter tmpl concat");
        check(runRule(R"({"fn":"upper","args":[{"get":"node.name"}]})", ctx) == "WIDGET", "P18: interpreter fn builtin");
        check(runRule(R"({"fn":"join","args":[", ",{"get":"node.name"},{"get":"node.op"}]})", ctx) == "widget, +",
              "P18: interpreter fn with multiple args");
        // case: first matching arm wins; else fallback.
        const char* caseRule =
            R"({"case":{"when":[[{"eq":["node.kind","Unary"]},"U"],[{"eq":["node.kind","Binary"]},"B"]],"else":"?"}})";
        check(runRule(caseRule, ctx) == "B", "P18: interpreter case picks matching arm");
        ctx.fields["node.kind"] = "Call";
        check(runRule(caseRule, ctx) == "?", "P18: interpreter case falls to else");
        // Test combinators: has + not + and.
        check(runRule(R"({"case":{"when":[[{"has":"node.op"},"has-op"]],"else":"no"}})", ctx) == "has-op",
              "P18: interpreter Test has");
        check(runRule(R"({"case":{"when":[[{"not":{"has":"node.guard"}},"unguarded"]],"else":"g"}})", ctx) == "unguarded",
              "P18: interpreter Test not+has");
        check(runRule(R"({"case":{"when":[[{"and":[{"has":"node.op"},{"eq":["node.op","+"]}]},"plus"]],"else":"x"}})", ctx) == "plus",
              "P18: interpreter Test and");
        // --- Arm coverage (docs/prd/code-coverage-upload) ----------------------------------------
        {
            // Source provenance: the JSON parser stamps every value with its byte offset, and
            // LineIndex converts offsets to 1-based lines at the reporting boundary.
            const std::string doc = "{\n  \"a\": 1,\n  \"b\": [\n    2\n  ]\n}";
            const json::Value v = json::parse(doc);
            const json::LineIndex idx(doc);
            check(idx.lineAt(v.offset) == 1, "coverage: root value on line 1");
            check(idx.lineAt(v["a"].offset) == 2, "coverage: nested member offset -> line 2");
            check(idx.lineAt(v["b"].items()[0].offset) == 4, "coverage: array element offset -> line 4");
            check(idx.lineAt(0) == 1 && idx.lineAt(doc.size() + 99) >= 1, "coverage: LineIndex clamps");

            // A case rule with two arms, one per line. indexRule collects the DENOMINATOR from the
            // same parse that produces the rule, so an arm that never fires is still counted.
            const std::string ruleText =
                "{\n"
                "  \"case\": {\n"
                "    \"when\": [\n"
                "      [{\"eq\": [\"node.kind\", \"Unary\"]}, \"U\"],\n"
                "      [{\"eq\": [\"node.kind\", \"Binary\"]}, \"B\"]\n"
                "    ],\n"
                "    \"else\": \"?\"\n"
                "  }\n"
                "}";
            bool ok = true;
            std::string err;
            const json::Value ruleJson = json::parse(ruleText);
            engine::Rule rule = engine::parseRule(ruleJson, ok, err);
            check(ok, "coverage: case rule parses");

            std::vector<std::size_t> offsets;
            engine::indexRule(rule, 7, offsets);
            check(rule.srcId == 7, "coverage: indexRule stamps srcId");
            check(rule.arms.size() == 2 && rule.arms[0].second.srcId == 7 && rule.arms[0].first.srcId == 7,
                  "coverage: indexRule reaches nested arms and their tests");
            const json::LineIndex ruleLines(ruleText);
            check(ruleLines.lineAt(rule.arms[0].second.offset) == 4 &&
                  ruleLines.lineAt(rule.arms[1].second.offset) == 5,
                  "coverage: sibling case arms land on DISTINCT lines (branch-level signal)");

            // Evaluating picks arm 2; arm 1 is in the denominator but never hit — the guarantee
            // that a dead template arm reports as UNCOVERED rather than vanishing from the total.
            struct CollectingSink : engine::TraceSink {
                std::set<std::pair<int, std::size_t>> hits;
                void hit(int srcId, std::size_t offset) override { hits.insert({srcId, offset}); }
            } sink;
            MockEval armCtx;
            armCtx.fields["node.kind"] = "Binary";
            engine::setTraceSink(&sink);
            const std::string armOut = engine::evalRule(rule, armCtx);
            engine::setTraceSink(nullptr);

            check(armOut == "B", "coverage: tracing does not change evaluation");
            check(sink.hits.count({7, rule.arms[1].second.offset}) == 1, "coverage: taken arm is recorded hit");
            check(sink.hits.count({7, rule.arms[0].second.offset}) == 0, "coverage: untaken arm is NOT hit");
            check(std::find(offsets.begin(), offsets.end(), rule.arms[0].second.offset) != offsets.end(),
                  "coverage: untaken arm IS in the denominator (uncovered, not absent)");
            check(engine::traceSink() == nullptr, "coverage: sink is off by default");
        }

        // emit / emitChild route to the context's child-recursion (marker output in the mock).
        check(runRule(R"({"tmpl":[{"emitChild":"node.lhs","side":"l"}," + ",{"emit":"node.rhs"}]})", ctx) ==
              "<node.lhs:l> + <node.rhs>", "P18: interpreter emit / emitChild");
        // map: walk a child list by `<path>.count`, emit each indexed child, join with sep. Wrap to get `(a, b)`.
        ctx.fields["node.args.count"] = "3";
        check(runRule(R"J({"tmpl":["(",{"map":"node.args","sep":", "},")"]})J", ctx) ==
              "(<node.args.0>, <node.args.1>, <node.args.2>)", "P18: interpreter map over a child list");
        ctx.fields["node.args.count"] = "0";
        check(runRule(R"J({"tmpl":["(",{"map":"node.args","sep":", "},")"]})J", ctx) == "()",
              "P18: interpreter map over an empty child list");
        // map with an item template: `item.…` paths resolve against each element's indexed path.
        ctx.fields["node.fields.count"] = "2";
        ctx.fields["node.fields.0.name"] = "x";
        ctx.fields["node.fields.1.name"] = "y";
        check(runRule(R"J({"map":"node.fields","sep":", ","item":{"tmpl":[{"get":"item.name"},": ",{"emit":"item.value"}]}})J", ctx) ==
              "x: <node.fields.0.value>, y: <node.fields.1.value>",
              "P18: interpreter map with an item template (scoped item.* paths)");

        // Malformed rules fail loudly (never silently misparse).
        bool ok = true;
        runRule(R"({"bogus":1})", ctx, &ok);
        check(!ok, "P18: unknown rule form is rejected");
    }

    // ---- P18: Target -> BackendHandle (a target is a name, validated at resolve) ----------------------
    {
        check(findTarget("csharp").ok() && findTarget("typescript").ok() && findTarget("python").ok(),
              "P18: findTarget resolves the built-in targets");
        BackendHandle bad = findTarget("rust");
        check(!bad.ok() && has(bad.error(), "unknown target 'rust'") && has(bad.error(), "csharp"),
              "P18: findTarget on an unknown name fails with the known-target list");
        EmitResult r = compile("fn main() {}\n", bad);
        check(!r.ok && !r.diagnostics.empty() && has(r.diagnostics[0].message, "unknown target 'rust'"),
              "P18: compile with an invalid handle refuses with the resolution error");
    }

    // ---- P19 slice 8: loadBackend validation (anti-silent-drop + reference checks) ----------------------
    {
        std::string err;
        check(!loadBackend("not json", err) && has(err, "JSON object"),
              "P19: loadBackend rejects a non-object artifact");
        check(!loadBackend(R"({"name":"csharp"})", err) && has(err, "already loaded"),
              "P19: loadBackend rejects a duplicate name");
        // A minimal artifact missing nearly every rule: the coverage contract must name the first gap
        // (a construct with no rule and no declared stance).
        check(!loadBackend(R"({"schema":2,"name":"stubby",
                              "spec":{"name":"stubby","assignmentOrder":"targetFirst"},"rules":{"Program":"x","Type":"x"}})", err) &&
                  has(err, "anti-silent-drop"),
              "P19: loadBackend refuses a plugin with undeclared coverage gaps");
        // A "native" capability claim with no rule behind it is also a lie the loader catches.
        check(!loadBackend(R"({"schema":2,"name":"stubby2","spec":{"name":"stubby2","assignmentOrder":"targetFirst"},
                              "capabilities":{"patternMatching":"native"},
                              "rules":{"Program":"x","Type":"x"}})", err) &&
                  has(err, "no rule"),
              "P19: loadBackend refuses coverage gaps regardless of claims");
        // Review round 8: a schema-2 manifest from before `assignmentOrder` existed names the missing key and the
        // schema that requires it (it used to say only "must be targetFirst or valueFirst").
        check(!loadBackend(R"({"schema":2,"name":"olden","spec":{"name":"olden"},"rules":{"Program":"x","Type":"x"}})", err) &&
                  has(err, "plugin 'olden'") && has(err, "missing required key 'assignmentOrder'") &&
                  has(err, "manifest schema " + std::to_string(kPluginSchemaVersion) + " requires"),
              "review r8: a manifest without 'assignmentOrder' fails naming the key and the manifest schema it expects");
        check(!loadBackend(R"({"schema":2,"name":"olden2","spec":{"name":"olden2","assignmentOrder":"sideways"},"rules":{"Program":"x","Type":"x"}})", err) &&
                  has(err, "'assignmentOrder' must be") && !has(err, "missing required key"),
              "review r8: ... while a present but invalid value keeps the value diagnostic");
    }

    // ---- P37 slice 0: keyed capability vocabulary (closed + load-validated, umbrella fallback) ----------
    {
        // An unknown capability key is a LOAD error — never a silently-defaulted-to-native typo.
        std::string err;
        check(!loadBackend(R"({"schema":2,"name":"stubby3","spec":{"name":"stubby3","assignmentOrder":"targetFirst"},
                              "capabilities":{"operatorOverloadng":"native"},
                              "rules":{"Program":"x","Type":"x"}})", err) &&
                  has(err, "unknown capability key 'operatorOverloadng'"),
              "P37 s0: loadBackend refuses a capability key outside the closed vocabulary");
        check(isKnownCapabilityKey("operatorOverloading") && isKnownCapabilityKey("interfaces") &&
                  !isKnownCapabilityKey("operatorOverloadng"),
              "P37 s0: the vocabulary covers the legacy names + coverage-only entries, nothing else");

        // Umbrella rule on a loaded plugin: a `parent:child` key with no explicit entry inherits the bare
        // parent's stance; an absent parent stays "native".
        const Backend* php = findBackend("php");
        const Backend* cs = findBackend("csharp");
        check(php && php->capabilityStance(std::string("operatorOverloading:arithmetic")) == "false",
              "P37 s0: php's blanket operatorOverloading=false umbrella-covers an undeclared sub-key");
        check(php && php->capabilityStance(std::string("operatorOverloading:eq")) == "native",
              "P37 s0/C6: an explicit sub-key entry overrides the umbrella (php :eq)");
        check(cs && cs->capabilityStance(std::string("operatorOverloading:eq")) == "native",
              "P37 s0: an absent parent leaves a sub-key native");
    }

    // ---- P37 slice 1: `constructor` keyword (M4 guard) --------------------------------------------------
    {
        // `constructor` is a live JS/TS prototype member: the internal member name and the plugin skeleton
        // keys are lookup keys only, and emitted TS/JS must never contain a bare `.constructor` member
        // access (which would resolve to Object.prototype.constructor at runtime — a silent miscompile).
        const char* prog = "class Box {\n  var v: i32\n  constructor(v: i32) { this.v = v }\n"
                           "  fn get(): i32 => this.v\n}\n"
                           "fn main() {\n  let b = Box(7)\n  print(b.get())\n}\n";
        EmitResult ts = compileStd(prog, findTarget("typescript"));
        check(ts.ok, "P37 s1: a `constructor` class compiles on typescript");
        check(has(ts.code, "constructor("), "P37 s1: TS emits a native class constructor");
        check(!has(ts.code, ".constructor"), "P37 s1 (M4): emitted TS never accesses `.constructor`");
        check(!compile("class B {\n  init() {}\n}\n", findTarget("typescript")).ok,
              "P37 s1: the old `init` keyword no longer parses as a constructor");
    }

    // ---- P37 slice 2: `is` / `as` --------------------------------------------------------------------
    {
        const char* hier = "open class Shape {\n}\nclass Circle : Shape {\n  var r: i32\n"
                           "  constructor(r: i32) {\n    super()\n    this.r = r\n  }\n}\n";
        // The binding form: hoisted narrowed Let + null test; TS uses instanceof, C# uses `as`.
        std::string prog = std::string(hier) +
            "fn f(x: Shape): i32 {\n  if x is Circle c && c.r > 1 {\n    return c.r\n  }\n  return 0\n}\n"
            "fn main() { print(f(Circle(3))) }\n";
        EmitResult ts = compileStd(prog, findTarget("typescript"));
        check(ts.ok, "P37 s2: `is` binding compiles on typescript");
        check(has(ts.code, "instanceof Circle") && !has(ts.code, " as Circle"),
              "P37 s2 (M1): TS emits a runtime instanceof guard, never a bare `as`");
        EmitResult cs = compileStd(prog, findTarget("csharp"));
        check(cs.ok && has(cs.code, "x as Circle"), "P37 s2: C# lowers the binding through `as`");
        EmitResult py = compileStd(prog, findTarget("python"));
        check(py.ok && has(py.code, "isinstance"), "P37 s2: Python emits isinstance");

        // `as` yields T? — null on failure — and never a TS type assertion.
        std::string asProg = std::string(hier) +
            "fn g(x: Shape): i32 {\n  let c = x as Circle\n  return c?.r ?? -1\n}\n"
            "fn main() { print(g(Circle(2))) }\n";
        EmitResult tsAs = compileStd(asProg, findTarget("typescript"));
        check(tsAs.ok && has(tsAs.code, "instanceof Circle ? x : null"),
              "P37 s2 (M1): TS `as` is a guarded conditional yielding null");

        // Refusals: binding under `||` (test doesn't dominate the branch); union targets; type params;
        // provably-unrelated types; redeclaring a binding name later in the same block.
        auto refuses = [&](const std::string& src, const char* frag, const char* label) {
            EmitResult r = compileStd(src, findTarget("csharp"));
            bool named = false;
            for (const auto& d : r.diagnostics) if (has(d.message, frag)) named = true;
            check(!r.ok && named, label);
        };
        refuses(std::string(hier) + "fn f(x: Shape, b: bool): i32 {\n  if x is Circle c || b {\n    return 1\n  }\n  return 0\n}\nfn main() { }\n",
                "only allowed on an `if` condition's `&&` chain",
                "P37 s2 (B3): a binding under `||` is refused");
        refuses("union U {\n  A(v: i32)\n  B(w: i32)\n}\nfn f(x: U): bool => x is A\nfn main() { }\n",
                "use 'match'", "P37 s2: `is` on a union case is refused toward match");
        refuses("open class Shape {\n}\nfn f<T>(x: Shape): bool => x is T\nfn main() { }\n",
                "type parameter", "P37 s2: `is` on a type parameter is refused (generic-preserving)");
        refuses(std::string(hier) + "class Other {\n  var v: i32\n  constructor() { this.v = 0 }\n}\n"
                "fn f(o: Other): bool => o is Circle\nfn main() { }\n",
                "provably impossible", "P37 s2: an unrelated-type test is refused");
        refuses(std::string(hier) + "fn f(x: Shape): i32 {\n  if x is Circle c {\n    return c.r\n  }\n  let c = 5\n  return c\n}\nfn main() { }\n",
                "bound by an `is` test earlier in this block",
                "P37 s2: redeclaring an `is`-binding name later in the block is refused");

        // B4: interface `is` gates per-target — refused when TS is configured, allowed on C#.
        std::string iface = "interface Named {\n  fn label(): string\n}\n"
                            "class Tag2 : Named {\n  constructor() {}\n  fn label(): string => \"t\"\n}\n"
                            "fn f(x: Named): bool => x is Tag2\nfn main() { print(f(Tag2())) }\n";
        check(compileStd(iface, findTarget("csharp")).ok,
              "P37 s2 (B4): a CLASS test on an interface-typed operand passes everywhere");
        std::string ifaceTest = "interface Named {\n  fn label(): string\n}\n"
                                "open class Base {\n}\nclass Tag2 : Base, Named {\n  constructor() { super() }\n  fn label(): string => \"t\"\n}\n"
                                "fn f(x: Base): bool => x is Named\nfn main() { print(f(Tag2())) }\n";
        check(compileStd(ifaceTest, findTarget("csharp")).ok,
              "P37 s2 (B4): interface `is` allowed on a csharp-only build");
        EmitResult tsIface = compileStd(ifaceTest, findTarget("typescript"));
        bool gated = false;
        for (const auto& d : tsIface.diagnostics)
            if (has(d.message, "interfaces:runtimeIdentity")) gated = true;
        check(!tsIface.ok && gated,
              "P37 s2 (B4): interface `is` refused when typescript is configured (no runtime identity)");
    }

    // ---- P37 slice 3: operators complete ---------------------------------------------------------------
    {
        const char* vec = "class Vec {\n  var x: i32\n  constructor(x: i32) { this.x = x }\n"
                          "  operator fn plus(o: Vec): Vec => Vec(this.x + o.x)\n"
                          "  operator fn eq(o: Vec): bool => this.x == o.x\n"
                          "  operator fn hash(): i32 => this.x\n"
                          "  operator fn neg(): Vec => Vec(0 - this.x)\n"
                          "  operator fn band(o: Vec): Vec => Vec(this.x & o.x)\n}\n";
        // C5: TS moves to static-on-type; C#/Python stay native.
        std::string prog = std::string(vec) +
            "fn main() {\n  let a = Vec(3)\n  let b = Vec(4)\n  print((a + b).x)\n  print(a == b)\n"
            "  print((-a).x)\n  print((a & b).x)\n}\n";
        EmitResult ts = compileStd(prog, findTarget("typescript"));
        check(ts.ok, "P37 s3: TS compiles the full operator surface");
        check(has(ts.code, "static plus(lhs: Vec") && has(ts.code, "Vec.plus(a, b)"),
              "P37 s3 (C5): TS operators are static-on-type at decl and call site");
        check(has(ts.code, "static eq(lhs: Vec | null") && has(ts.code, "Vec.eq(a, b)") &&
                  has(ts.code, "if (lhs === null ||"),
              "P37 s3 (C5): TS eq is static and null-tolerant");
        check(has(ts.code, "Vec.neg(a)"), "P37 s3 (#62): TS routes user unary neg");
        check(has(ts.code, "Vec.band(a, b)"), "P37 s3 (#63): TS routes user bitwise band");
        EmitResult cs = compileStd(prog, findTarget("csharp"));
        check(cs.ok && has(cs.code, "operator +(Vec lhs") && has(cs.code, "operator &(Vec lhs"),
              "P37 s3: C# operators stay native (incl. bitwise)");
        EmitResult py = compileStd(prog, findTarget("python"));
        check(py.ok && has(py.code, "__add__") && has(py.code, "__neg__") && has(py.code, "__and__"),
              "P37 s3: Python dunders cover arithmetic + unary + bitwise");

        // Explicit conversion: cast-site rewrite on method targets, native explicit operator on C#.
        std::string conv = "record Cel(deg: f64) {\n  operator fn explicit(): f64 => this.deg * 2.0\n}\n"
                           "fn main() {\n  let c = Cel(5.0)\n  print((f64)c)\n}\n";
        EmitResult tsc = compileStd(conv, findTarget("typescript"));
        check(tsc.ok && has(tsc.code, "Cel.toF64(c)") && has(tsc.code, "static toF64(lhs: Cel)"),
              "P37 s3: TS rewrites the cast site to the static Cel.toF64(c)");
        EmitResult csc2 = compileStd(conv, findTarget("csharp"));
        check(csc2.ok && has(csc2.code, "explicit operator double(Cel lhs)"),
              "P37 s3: C# emits the native explicit operator");

        // Refusals: implicit conversion; unary without a declared neg; compound with a computed receiver.
        auto refuses3 = [&](const std::string& src, const char* frag, const char* label) {
            EmitResult r = compileStd(src, findTarget("csharp"));
            bool named = false;
            for (const auto& d : r.diagnostics) if (has(d.message, frag)) named = true;
            check(!r.ok && named, label);
        };
        refuses3("record M(v: f64) {\n  operator fn implicit(): f64 => this.v\n}\nfn main() { }\n",
                 "refuses implicit user conversions", "P37 s3: implicit conversion refused");
        refuses3(std::string(vec) + "fn main() {\n  let a = Vec(1)\n  let t = ~a\n}\n",
                 "declares no 'operator fn bnot'",
                 "P37 s3 (#62): `~` on a user type without bnot is refused, not mis-emitted");
        refuses3("class Box {\n  var a: Vec\n  constructor() { this.a = Vec(0) }\n}\n" + std::string(vec) +
                 "fn mk(): Box => Box()\nfn main() {\n  mk().a += Vec(2)\n}\n",
                 "needs a simple receiver",
                 "P37 s3 (C7): a compound through a computed receiver is refused (single-eval)");
        check(compileStd(std::string(vec) + "fn main() {\n  var v = Vec(1)\n  v += Vec(2)\n  print(v.x)\n}\n",
                         findTarget("typescript")).ok,
              "P37 s3 (C7): a simple compound on a user type compiles");

        // C6: PHP gains :eq (+:indexers) while :arithmetic stays refused.
        std::string eqOnly = "class M {\n  var c: i32\n  constructor(c: i32) { this.c = c }\n"
                             "  operator fn eq(o: M): bool => this.c == o.c\n"
                             "  operator fn hash(): i32 => this.c\n}\n"
                             "fn main() { print(M(1) == M(1)) }\n";
        EmitResult phpEq = compileStd(eqOnly, findTarget("php"));
        check(phpEq.ok && has(phpEq.code, "->eq("), "P37 s3 (C6): PHP supports user eq (->eq call)");
        std::string arith = std::string(vec) + "fn main() { print((Vec(1) + Vec(2)).x) }\n";
        EmitResult phpAr = compileStd(arith, findTarget("php"));
        bool namedArith = false;
        for (const auto& d : phpAr.diagnostics)
            if (has(d.message, "operatorOverloading:arithmetic")) namedArith = true;
        check(!phpAr.ok && namedArith, "P37 s3 (C6): PHP still refuses :arithmetic, by sub-key");
    }

    // ---- P37 slice 4: attributes — Tier 2 portable metadata + Tier 1 pass-through ----------------------
    {
        // Tier 2: Meta queries resolve at transpile time and lower to inline constants.
        const char* meta =
            "attribute Range(min: i32, max: i32)\n"
            "[Range(1, 8)]\nclass Box {\n  [Range(0, 100)]\n  var fill: i32\n"
            "  constructor() { this.fill = 0 }\n}\n"
            "class Plain {\n  var v: i32\n  constructor() { this.v = 0 }\n}\n";
        std::string q = std::string(meta) +
            "fn main() {\n  print(Meta.has<Box, Range>())\n  print(Meta.has<Plain, Range>())\n"
            "  let r = Meta.get<Box, Range>()\n  print(r?.max ?? -1)\n"
            "  print(Meta.member<Box, Range>(\"fill\")?.max ?? -1)\n}\n";
        EmitResult ts = compileStd(q, findTarget("typescript"));
        check(ts.ok, "P37 s4: Meta queries compile on typescript");
        check(has(ts.code, "new Range(1, 8)") && has(ts.code, "new Range(0, 100)"),
              "P37 s4: Meta.get/member lower to inline constructions of the recorded constants");
        check(!has(ts.code, "Meta"), "P37 s4: no Meta runtime surface exists in the output");
        EmitResult php = compileStd(q, findTarget("php"));
        check(php.ok, "P37 s4: Tier 2 metadata needs NO capability — it runs on PHP too");

        // Zero output when unqueried: attach but never query -> the attribute record vanishes.
        std::string unq = std::string(meta) + "fn main() { print(Box().fill) }\n";
        EmitResult tsu = compileStd(unq, findTarget("typescript"));
        check(tsu.ok && !has(tsu.code, "Range"),
              "P37 s4: an attached-but-unqueried attribute emits ZERO runtime output");

        // Tier 1: the binding IS the native annotation line; imports accumulate once; refuse/D12 gate.
        const char* bind =
            "extern attribute JsonProp(name: string) {\n"
            "  actual(csharp) extern(\"[Newtonsoft.Json.JsonProperty($0)]\") import \"using Newtonsoft.Json;\"\n"
            "  actual(typescript) extern(\"@JsonProp($name)\")\n"
            "  actual(python) refuse\n"
            "}\n";
        std::string prog1 = std::string(bind) +
            "[JsonProp(\"x\")]\nclass C {\n  var x: i32\n  constructor() { this.x = 0 }\n}\n"
            "fn main() { print(C().x) }\n";
        EmitResult cs = compileStd(prog1, findTarget("csharp"));
        check(cs.ok && has(cs.code, "[Newtonsoft.Json.JsonProperty(\"x\")]") &&
                  has(cs.code, "using Newtonsoft.Json;"),
              "P37 s4 (Tier 1): C# gets the native attribute line + the using once");
        EmitResult ts1 = compileStd(prog1, findTarget("typescript"));
        check(ts1.ok && has(ts1.code, "@JsonProp(\"x\")"),
              "P37 s4 (Tier 1): TS gets the decorator line ($name substitution)");
        EmitResult py1 = compileStd(prog1, findTarget("python"));
        bool refusedArm = false;
        for (const auto& d : py1.diagnostics) if (has(d.message, "declares `refuse`")) refusedArm = true;
        check(!py1.ok && refusedArm, "P37 s4 (D12): an explicit `refuse` arm refuses loudly");
        EmitResult ph1 = compileStd(prog1, findTarget("php"));
        bool noArm = false;
        for (const auto& d : ph1.diagnostics) if (has(d.message, "no binding for target 'php'")) noArm = true;
        check(!ph1.ok && noArm, "P37 s4 (D12): a target with NO arm refuses loudly");

        // Attachment-point gating: TS has no function decorators.
        std::string fnAttr = std::string(bind) +
            "[JsonProp(\"f\")]\nfn helper(): i32 => 1\nfn main() { print(helper()) }\n";
        EmitResult tsf = compileStd(fnAttr, findTarget("typescript"));
        bool gatedFn = false;
        for (const auto& d : tsf.diagnostics)
            if (has(d.message, "attributes:target.function")) gatedFn = true;
        check(!tsf.ok && gatedFn, "P37 s4 (D10): a function attribute gates on TS (no function decorators)");
        check(compileStd(fnAttr, findTarget("csharp")).ok,
              "P37 s4 (D10): the same function attribute is fine on C#");

        // M6 + attachment refusals.
        auto refuses4 = [&](const std::string& src, const char* frag, const char* label) {
            EmitResult r = compileStd(src, findTarget("csharp"));
            bool named = false;
            for (const auto& d : r.diagnostics) if (has(d.message, frag)) named = true;
            check(!r.ok && named, label);
        };
        refuses4(std::string(meta) + "fn f<T>(): bool => Meta.has<T, Range>()\nfn main() { }\n",
                 "cannot be a type parameter",
                 "P37 s4 (M6): a type-parameter type arg refuses — permanently");
        refuses4(std::string(meta) + "fn f(n: string): bool => Meta.member<Box, Range>(n) != null\nfn main() { }\n",
                 "STRING LITERAL", "P37 s4 (M6): a computed member name refuses");
        refuses4(std::string(meta) + "fn main() { print(Meta.member<Box, Range>(\"nope\") == null) }\n",
                 "no member named 'nope'", "P37 s4: an unknown member name refuses");
        refuses4("attribute Tag()\n[Tag, Tag]\nclass C {\n  var v: i32\n  constructor() { this.v = 0 }\n}\nfn main() { }\n",
                 "applied more than once", "P37 s4: repeat application refuses (AllowMultiple deferred)");
        refuses4("attribute Tag(v: i32)\nfn cfg(): i32 => 3\n[Tag(cfg())]\nclass C {\n  var v: i32\n  constructor() { this.v = 0 }\n}\nfn main() { }\n",
                 "compile-time constants", "P37 s4: a non-const attribute arg refuses");
        refuses4("attribute Tag()\nfn main() {\n  let t = Tag()\n}\n",
                 "cannot be constructed directly", "P37 s4: direct construction of an attribute refuses");
        refuses4("class C {\n  var v: i32\n  constructor() { this.v = 0 }\n  fn m(): i32 => this.v\n}\n"
                 "fn main() {\n  let c = C()\n  print(c.m<i32>())\n}\n",
                 "explicit type arguments are not supported",
                 "P37 s4 (D.3): silently-ignored member-call type args now refuse (anti-silent-drop)");
    }

    // ---- P37 feature-complete: field/param attributes, enum/array values, Meta.param ------------------
    {
        const char* bind =
            "extern attribute JsonProp(name: string) {\n"
            "  actual(csharp) extern(\"[Newtonsoft.Json.JsonProperty($0)]\") import \"using Newtonsoft.Json;\"\n"
            "  actual(php) extern(\"#[JsonProp($0)]\")\n"
            "}\n";
        // Tier 1 on a FIELD: the annotation line lands above the field; Python gates the point.
        std::string fieldProg = std::string(bind) +
            "class C {\n  [JsonProp(\"x\")]\n  var x: i32\n  constructor() { this.x = 0 }\n}\n"
            "fn main() { print(C().x) }\n";
        EmitResult csf = compileStd(fieldProg, findTarget("csharp"));
        check(csf.ok && has(csf.code, "[Newtonsoft.Json.JsonProperty(\"x\")]"),
              "P37 fc: a Tier 1 FIELD attribute emits above the field on C#");
        EmitResult pyf = compileStd(fieldProg, findTarget("python"));
        bool gatedField = false;
        for (const auto& d : pyf.diagnostics)
            if (has(d.message, "attributes:target.field")) gatedField = true;
        check(!pyf.ok && gatedField, "P37 fc: a field attribute gates on Python (no field annotations)");

        // Tier 1 on a PARAMETER: inline before the param on C#/PHP; TS gates the point.
        std::string paramProg = std::string(bind) +
            "fn f([JsonProp(\"p\")] p: i32): i32 => p\nfn main() { print(f(3)) }\n";
        EmitResult csp = compileStd(paramProg, findTarget("csharp"));
        check(csp.ok && has(csp.code, "[Newtonsoft.Json.JsonProperty(\"p\")] int p"),
              "P37 fc: a Tier 1 PARAM attribute emits inline on C#");
        EmitResult php2 = compileStd(paramProg, findTarget("php"));
        check(php2.ok && has(php2.code, "#[JsonProp(\"p\")] $p"),
              "P37 fc: a Tier 1 PARAM attribute emits inline on PHP");
        EmitResult tsp = compileStd(paramProg, findTarget("typescript"));
        bool gatedParam = false;
        for (const auto& d : tsp.diagnostics)
            if (has(d.message, "attributes:target.param")) gatedParam = true;
        check(!tsp.ok && gatedParam, "P37 fc: a param attribute gates on TS (TC39 has none)");

        // Enum + array constant values render per plugin SPEC data (never target-name checks in Core).
        std::string constProg =
            "enum Sev {\n  Low\n  High\n}\n"
            "extern attribute Cfg(s: Sev, ids: i32[]) {\n"
            "  actual(csharp) extern(\"[Cfg($0, $1)]\")\n"
            "  actual(php) extern(\"#[Cfg($0, $1)]\")\n"
            "}\n"
            "[Cfg(Sev.High, [1, 2])]\nclass C {\n  var v: i32\n  constructor() { this.v = 0 }\n}\n"
            "fn main() { print(C().v) }\n";
        EmitResult csc3 = compileStd(constProg, findTarget("csharp"));
        check(csc3.ok && has(csc3.code, "[Cfg(Sev.High, new[] {1, 2})]"),
              "P37 fc: enum + array values spell per the C# plugin spec (constArray*)");
        EmitResult phc = compileStd(constProg, findTarget("php"));
        check(phc.ok && has(phc.code, "#[Cfg(Sev::High, [1, 2])]"),
              "P37 fc: enum member spells via the PHP plugin's enumMemberOp");

        // Tier 2 param metadata via Meta.param.
        std::string metaParam =
            "attribute Range(min: i32, max: i32)\n"
            "class Box {\n  var v: i32\n  constructor() { this.v = 0 }\n"
            "  fn scale([Range(1, 4)] k: i32): i32 => this.v * k\n}\n"
            "fn main() {\n  print(Meta.param<Box, Range>(\"scale\", \"k\")?.max ?? -1)\n}\n";
        EmitResult mp = compileStd(metaParam, findTarget("typescript"));
        check(mp.ok && has(mp.code, "new Range(1, 4)"),
              "P37 fc: Meta.param lowers to the recorded constants");
        auto refusesFc = [&](const std::string& src, const char* frag, const char* label) {
            EmitResult r = compileStd(src, findTarget("csharp"));
            bool named = false;
            for (const auto& d : r.diagnostics) if (has(d.message, frag)) named = true;
            check(!r.ok && named, label);
        };
        refusesFc("attribute Range(min: i32, max: i32)\n"
                  "class Box {\n  var v: i32\n  constructor() { this.v = 0 }\n"
                  "  fn scale([Range(1, 4)] k: i32): i32 => this.v * k\n}\n"
                  "fn main() {\n  print(Meta.param<Box, Range>(\"scale\", \"nope\") == null)\n}\n",
                  "no parameter named 'nope'", "P37 fc: an unknown param name refuses");
        refusesFc("attribute Tag(v: i32)\nattribute Wrap([Tag(1)] x: i32)\nfn main() { }\n",
                  "attribute` declaration's parameters are refused",
                  "P37 fc: attributes on an attribute's own params refuse");
    }

    // ---- P19 slices 13-15: reserved/forbidden identifiers (identifier hygiene, json-plugins.md §7) -----
    {
        // A name a target's generated code uses is refused for that target only (kind-blind v1).
        EmitResult ts = compile("fn main() {\n  let tag = 1\n  var y = tag\n}\n", findTarget("typescript"));
        check(!ts.ok && has(ts.diagnostics[0].message, "'tag' is reserved by target 'typescript'"),
              "P19: a TS-reserved local ('tag') refuses on typescript");
        check(compile("fn main() {\n  let tag = 1\n  var y = tag\n}\n", findTarget("csharp")).ok,
              "P19: the same name compiles fine on a target that doesn't reserve it");
        EmitResult py = compile("fn _pg_idiv(a: i32, b: i32): i32 {\n  return a\n}\nfn main() {}\n",
                                findTarget("python"));
        check(!py.ok && has(py.diagnostics[0].message, "'_pg_idiv' uses a prefix reserved for the Polyglot std library"),
              "P19/P40 s8: a `_pg_` runtime-helper fn name refuses (std-reserved prefix, every target)");
        EmitResult pyU = compile("fn main() {\n  let _PG_UPPER = 1\n}\n", findTarget("python"));
        check(!pyU.ok && has(pyU.diagnostics[0].message, "'_PG_UPPER' is reserved by target 'python'"),
              "P19/P40 s8: a Python prelude table name (`_PG_*`) refuses on python");
        EmitResult cs = compile("record PolyglotProgram(x: i32)\nfn main() {}\n", findTarget("csharp"));
        check(!cs.ok && has(cs.diagnostics[0].message, "'PolyglotProgram' is reserved by target 'csharp'"),
              "P19: a C#-scaffolding type name ('PolyglotProgram') refuses on csharp");
        // Prefix families: `__w*` covers every lowering temp, not just one spelled-out name.
        EmitResult w = compile("fn main() {\n  let __w1 = 1\n  var y = __w1\n}\n", findTarget("csharp"));
        bool wReserved = false, wCompiler = false;
        for (const auto& d : w.diagnostics) {
            if (has(d.message, "'__w1' is reserved")) wReserved = true;
            if (has(d.message, "'__w1' contains '__', which is reserved for compiler-generated names")) wCompiler = true;
        }
        check(!w.ok && wReserved, "P19: a reserved prefix family ('__w*') matches '__w1'");
        // P40 review r4 (part 2): `__` anywhere in a user name is the compiler's, on every target.
        check(wCompiler, "r4: a user name containing '__' is refused language-wide");
        EmitResult w2 = compile("fn parse__string(s: string): i32 => 1\nfn main() {}\n", findTarget("typescript"));
        check(!w2.ok && has(w2.diagnostics[0].message, "'parse__string' contains '__'"),
              "r4: an infix '__' (the overload signature-name spelling) is refused too");
        // Runtime globals get their own shadowing message.
        EmitResult g = compile("fn main() {\n  let console = 1\n  var y = console\n}\n", findTarget("typescript"));
        check(!g.ok && has(g.diagnostics[0].message, "'console' shadows a runtime global of target 'typescript'"),
              "P19: shadowing a target runtime global refuses");
        // pgconfig forbiddenIdentifiers: "*" bans everywhere; a target-keyed ban bites only that target.
        LibConfig banAll;  banAll.forbiddenIdentifiers = {{"*", "temp"}};
        LibConfig banPy;   banPy.forbiddenIdentifiers = {{"python", "data"}};
        EmitResult f1 = compile("fn main() {\n  let temp = 1\n  var y = temp\n}\n", findTarget("csharp"), nullptr, banAll);
        check(!f1.ok && has(f1.diagnostics[0].message, "'temp' is forbidden by pgconfig"),
              "P19: a '*' pgconfig-forbidden identifier refuses on every target");
        EmitResult f2 = compile("fn main() {\n  let data = 1\n  var y = data\n}\n", findTarget("python"), nullptr, banPy);
        check(!f2.ok && has(f2.diagnostics[0].message, "'data' is forbidden by pgconfig"),
              "P19: a target-keyed pgconfig ban bites on that target");
        check(compile("fn main() {\n  let data = 1\n  var y = data\n}\n", findTarget("csharp"), nullptr, banPy).ok,
              "P19: a target-keyed pgconfig ban is ignored by other targets");

        // Slice 15: target-keyword names escape CONSISTENTLY at decl + reference + type sites (ident is
        // wired through every rule's name holes), and refuse on a target that declares no escape (TS).
        EmitResult kcs = compile("record switch(delegate: i32)\nfn checked(a: i32): i32 {\n  return a\n}\n"
                                 "fn main() {\n  let s = switch(5)\n  var y = checked(s.delegate)\n}\n",
                                 findTarget("csharp"));
        check(kcs.ok && has(kcs.code, "record @switch(int @delegate)") && has(kcs.code, "int @checked(int a)") &&
                  has(kcs.code, "PolyglotProgram.@checked(s.@delegate)") && has(kcs.code, "new @switch(5)"),
              "P19: C#-keyword names escape as @name at decl, call, member, and construction sites");
        EmitResult kpy = compile("fn global(a: i32): i32 {\n  return a\n}\nfn main() {\n  var y = global(7)\n}\n",
                                 findTarget("python"));
        check(kpy.ok && has(kpy.code, "def global_(a)") && has(kpy.code, "global_(7)"),
              "P19: python-keyword fn names escape with the suffix strategy, decl and call agreeing");
        EmitResult kts = compile("fn function(a: i32): i32 {\n  return a\n}\nfn main() {}\n",
                                 findTarget("typescript"));
        check(!kts.ok && has(kts.diagnostics[0].message, "'function' is reserved by target 'typescript'"),
              "P19: TS declares no escape, so its keywords are reserved names -> honest refusal");
    }

    // ---- Precedence parenthesization (the 2026-07-04 FruitCake NaN miscompile) -------------------------
    // `??`/bitwise/shifts were missing from the engine's operatorPrecedence table and fell through to the
    // TIGHTEST level, dropping required parens: `basis + (opt?.v ?? 0.0)` emitted bare reparses as
    // `(basis + opt?.v) ?? 0.0` — wrong on both targets and divergent (C# 0.0 vs JS NaN).
    {
        const char* src =
            "class Box {\n  var v: f64\n  constructor(v: f64) { this.v = v }\n}\n"
            "fn addOpt(basis: f64, opt: Box?): f64 => basis + (opt?.v ?? 0.0)\n"
            "fn main() {\n  print(addOpt(5.0, null))\n}\n";
        EmitResult cs = compileStd(src, findTarget("csharp"));
        check(cs.ok && has(cs.code, "+ (opt?.v ?? "),
              "precedence: C# keeps the parens around a ?? operand of +");
        EmitResult ts = compileStd(src, findTarget("typescript"));
        check(ts.ok && has(ts.code, "+ (opt?.v ?? "),
              "precedence: TS keeps the parens around a ?? operand of +");

        const char* shifts =
            "fn f(a: i32, b: i32, c: i32): i32 => (a << b) + c\n"
            "fn main() {\n  print(f(5, 2, 3))\n}\n";
        EmitResult scs = compileStd(shifts, findTarget("csharp"));
        check(scs.ok && has(scs.code, "(a << b) + c"),
              "precedence: C# keeps the parens around a shift operand of +");

        const char* cmp =
            "fn g(a: i32, b: i32, c: i32): bool => (b > a) == (c > a)\n"
            "fn main() {\n  print(g(1, 2, 3))\n}\n";
        EmitResult pcs = compileStd(cmp, findTarget("python"));
        check(pcs.ok && has(pcs.code, "(b > a) == (c > a)"),
              "precedence: python keeps comparison-under-comparison parens (no chaining)");
    }

    // ---- P21 slice 1: watch-mode support (FileWatcher polling + RecordingResolver) ---------------------
    {
        using cli::FileWatcher;
        using cli::PollingFileWatcher;
        using cli::RecordingResolver;
        namespace fs = std::filesystem;
        using namespace std::chrono_literals;

        const fs::path dir = fs::temp_directory_path() / "polyglot-watch-tests";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
        const fs::path a = dir / "a.pg";
        const fs::path b = dir / "b.pg"; // watched before it exists: its appearance must count as a change
        { std::ofstream(a, std::ios::binary) << "fn main() {}\n"; }

        PollingFileWatcher w(10ms); // fast tick so the suite stays quick
        w.watch({a, b});
        check(w.waitNext(50ms) == FileWatcher::Event::TimedOut, "P21: unchanged set times out");

        { std::ofstream(a, std::ios::binary) << "fn main() {\n  print(1)\n}\n"; }
        check(w.waitNext(2000ms) == FileWatcher::Event::Changed, "P21: a modified file reports Changed");
        check(w.waitNext(50ms) == FileWatcher::Event::TimedOut,
              "P21: detected changes fold into the baseline (no double report)");

        { std::ofstream(b, std::ios::binary) << "fn helper() {}\n"; }
        check(w.waitNext(2000ms) == FileWatcher::Event::Changed,
              "P21: a missing watched file appearing reports Changed");

        fs::remove(a);
        check(w.waitNext(2000ms) == FileWatcher::Event::Changed, "P21: a deleted file reports Changed");

        w.stop();
        check(w.waitNext(2000ms) == FileWatcher::Event::Stopped, "P21: stop() makes waitNext return Stopped");

        fs::remove_all(dir, ec);

        // RecordingResolver captures the loaded closure + the unresolved specifiers.
        MapModuleResolver inner({{"geometry", "fn area(w: i32, h: i32): i32 {\n  return w * h\n}\n"}});
        RecordingResolver rec(inner);
        check(rec.resolve("geometry", "").has_value() && rec.loaded().count("geometry") == 1,
              "P21: RecordingResolver records a resolved module's canonical path");
        check(!rec.resolve("missing", "entry.pg").has_value() &&
                  rec.unresolved().count({"missing", "entry.pg"}) == 1,
              "P21: RecordingResolver records unresolved (spec, importer) pairs");
        EmitResult r = compile("import { area } from \"geometry\"\nfn main() {\n  print(area(3, 4))\n}\n",
                               findTarget("csharp"), &rec, LibConfig{{"io"}});
        check(r.ok && rec.loaded().count("geometry") == 1,
              "P21: compile through a RecordingResolver captures the import closure");
    }

    // P25 §4.18 — capture analysis: the pass classifies each lambda's captures and stamps needsCell.
    // Asserted on the typed IR dump: a lambda shows `[caps <name>(cell)? … this?]`; a celled local shows
    // `[cell]`, a captured-but-snapshot local `[captured]`.
    {
        auto capIr = [&](const char* src) -> std::string {
            DiagnosticBag d;
            auto unit = parse(lex(src, d), d);
            mintplayer::polyglot::check(unit, d);
            if (d.hasErrors()) return "<type error>";
            return ir::dump(lower(unit, "csharp"));
        };
        // Accumulator: `total` is mutated through the closure -> SHARED-RW -> needsCell (the golden sample).
        std::string acc = capIr("fn f(): i32 {\n  var total = 0\n  let add = (n: i32) => { total += n }\n"
                                "  add(10)\n  add(20)\n  return total\n}\n");
        check(has(acc, "[caps total(cell)]"), "P25: mutated capture is classified needsCell");
        check(has(acc, "var total: i32 = 0:i32 [cell]"), "P25: the mutated local's declaration is celled");

        // Pure read of an immutable local -> SNAPSHOT -> captured but NO cell.
        std::string snap = capIr("fn f(): i32 {\n  let base = 10\n  let g = (x: i32) => x + base\n  return g(5)\n}\n");
        check(has(snap, "[caps base]") && !has(snap, "base(cell)"), "P25: unmutated capture is snapshot (no cell)");
        check(has(snap, "let base: i32 = 10:i32 [captured]"), "P25: snapshot local is captured but not celled");

        // Loop-variable capture: `i` is read-only -> snapshot per iteration, no cell.
        std::string loop = capIr("fn f() {\n  for i in 1..=3 {\n    let g = () => i\n  }\n}\n");
        check(has(loop, "[caps i]") && !has(loop, "i(cell)"), "P25: read-only loop-var capture is snapshot");

        // A global is not a local binding -> never a capture.
        std::string glob = capIr("let base = 5\nfn f(): i32 {\n  let rd = () => base\n  return rd()\n}\n");
        check(!has(glob, "[caps"), "P25: a module global is not captured");

        // Nested lambdas: a variable two levels up is captured by BOTH inner lambdas (propagation).
        std::string nest = capIr("fn f(): i32 {\n  let x = 5\n  let g = () => {\n    let h = () => x\n    return h()\n  }\n  return g()\n}\n");
        check(has(nest, "[caps x]"), "P25: nested lambda propagates the capture to each level");

        // `this` inside a lambda sets capturesThis (drives TS-arrow / PHP $this / C++ [this]). Asserted in an
        // `init` body, which `ir::dump` renders (method bodies aren't dumped, but the flag is set the same way).
        std::string self = capIr("class C {\n  let v: i32\n  constructor() {\n    this.v = 0\n    let f = () => this.v\n  }\n}\n");
        check(has(self, "[caps this]"), "P25: a lambda referencing `this` sets capturesThis");

        // Slice 4 — PHP real closures. PHP has no interpreter in this environment, so the differential gate
        // can't run it; assert the emission directly: a mutated capture -> `use (&$x)` (driven off needsCell,
        // never syntax); a closure-valued local is called via `$f(...)`; a pure expression lambda -> `fn(…)`.
        EmitResult phpAcc = compileStd("fn main() {\n  var total = 0\n  let add = (n: i32) => { total += n }\n"
                                       "  add(10)\n  print(total)\n}\n", findTarget("php"));
        check(phpAcc.ok && has(phpAcc.code, "use (&$total)"), "P25/PHP: a mutated capture emits use(&$total)");
        check(phpAcc.ok && has(phpAcc.code, "$add("), "P25/PHP: a closure-valued local is called via $f(...)");
        EmitResult phpSnap = compileStd("fn apply(f: (i32) => i32, x: i32): i32 => f(x)\n"
                                        "fn main() {\n  let base = 10\n  print(apply((n: i32) => n + base, 5))\n}\n",
                                        findTarget("php"));
        check(phpSnap.ok && has(phpSnap.code, "fn("), "P25/PHP: a pure expression lambda emits the fn(...) form");
    }

    // P26 slice 0 — the tri-state capability vocabulary. The three new flags detect from the syntactic
    // surface and gate-on-`false` / warn-on-`emulated`; they are inert for the current native backends.
    {
        auto parseOf = [](const char* src) { DiagnosticBag d; return parse(lex(src, d), d); };
        auto usesFeature = [](const CompilationUnit& u, Feature f) {
            for (const auto& use : collectFeatureUses(u)) if (use.key == featureName(f)) return true;
            return false;
        };

        // Detection: each new axis is marked from a type annotation / a mutable class field.
        auto width = parseOf("fn f(x: u8): i32 { return x }\n");
        check(usesFeature(width, Feature::FixedWidthIntegers), "P26 s0: a sub-64/unsigned width marks fixedWidthIntegers");
        auto ch = parseOf("fn f(c: char): char { return c }\n");
        check(usesFeature(ch, Feature::Utf16Strings), "P26 s0: a `char` type marks utf16Strings");
        auto mref = parseOf("class Box { var v: i32\n  constructor() { this.v = 0 } }\n");
        check(usesFeature(mref, Feature::MutableRefClasses), "P26 s0: a mutable-field class marks mutableRefClasses");
        auto immut = parseOf("record P(x: i32)\nfn f(): i32 { let p = P(1)\n  return p.x }\n");
        check(!usesFeature(immut, Feature::MutableRefClasses), "P26 s0: a record (no var field) does not mark mutableRefClasses");

        // Refuse-on-false: a target lacking the flag refuses a program that uses it, naming the feature + target.
        StubBackend noWidth(Feature::FixedWidthIntegers);
        DiagnosticBag dw; checkCapabilities(width, noWidth, dw);
        bool namedW = false;
        for (const auto& it : dw.items()) if (has(it.message, "fixedWidthIntegers") && has(it.message, "stub")) namedW = true;
        check(dw.hasErrors() && namedW, "P26 s0: a target lacking fixedWidthIntegers refuses + names it");

        // Warn-on-emulated: a used emulated feature warns (no error); an unused gated feature stays clean.
        StubBackend emuWidth(Feature::Async /* unused by the program */, Feature::FixedWidthIntegers);
        DiagnosticBag de; checkCapabilities(width, emuWidth, de);
        bool warned = false;
        for (const auto& it : de.items())
            if (it.severity == Severity::Warning && has(it.message, "emulates") && has(it.message, "fixedWidthIntegers")) warned = true;
        check(!de.hasErrors() && warned, "P26 s0: an emulated feature warns, does not error");

        // Inert for the current backends: C# declares these native/absent, so a width program is not gated.
        DiagnosticBag dcs; checkCapabilities(width, *findBackend("csharp"), dcs);
        check(!dcs.hasErrors(), "P26 s0: csharp (native) does not gate a fixed-width program");
    }

}
