// P4-P15: name/type resolution, lowering, exhaustiveness, capability gating, refusals, overloading, portability, async.
// P41 D14: one translation unit of the unit suite (split from tests_main.cpp so the areas compile in
// parallel). The bodies are unchanged; the shared harness lives in test_support.hpp.

#include "test_support.hpp"

void run_sema_targets() {
    // P4 — name / type resolution across the declaration surface.
    rejects("fn f(x: Widget) {}\n", "P4: rejects unknown type in a parameter");
    rejects("record A(x: i32)\nrecord A(y: i32)\n", "P4: rejects duplicate type declaration");
    rejects("fn f<T>(x: T): Nope => x\n", "P4: rejects unknown type in a generic return");
    rejects("record B(x: i32) : Missing\n", "P4: rejects unknown base type");
    resolves("record Box<T>(value: T)\nfn id<T>(x: T): T => x\n", "P4: generics + declared types resolve");
    resolves("enum E { A, B }\nfn pick(): E => E.A\n", "P4: declared enum type resolves");
    // P4-2 — nominal expression typing.
    rejects("record V(x: f64)\nfn f() { let v = V(1.0)\n print(v.y) }\n", "P4: rejects access to a missing member");
    rejects("record V(x: f64, y: f64)\nfn f() { let v = V(1.0) }\n", "P4: rejects wrong construction arity");
    rejects("class C { fn go() {} }\nfn f() { let c = C()\n c.nope() }\n", "P4: rejects call to a missing method");
    resolves("record V(x: f64, y: f64)\nfn f(): f64 => V(1.0, 2.0).x\n", "P4: member access types to the field type");
    resolves("record P(x: i32) {\n  fn getX(): i32 => this.x\n}\n", "P4: this.member resolves in a method");
    resolves("class C { fn go(): i32 => 5 }\nfn f(): i32 => C().go()\n", "P4: method call resolves to its return type");

    // P4 — AST -> typed IR lowering.
    {
        // (print is now a std module function, not an intrinsic, so this drives the pipeline with a
        // user-defined unit-returning function `show` to exercise typed-call lowering without the prelude.)
        const char* src = "fn add(a: i32, b: i32): i32 {\n  return a + b\n}\n"
                          "fn show(n: i32) {}\n"
                          "fn main() {\n  show(add(1, 2))\n}\n";
        DiagnosticBag d;
        auto unit = parse(lex(src, d), d);
        mintplayer::polyglot::check(unit, d); // annotate resolved types
        check(!d.hasErrors(), "IR: program type-checks");
        std::string ir = ir::dump(lower(unit, "csharp"));
        check(has(ir, "fn add(a: i32, b: i32): i32 {"), "IR: function signature carries types");
        check(has(ir, "return (a:i32 + b:i32):i32"), "IR: binary expression is typed");
        check(has(ir, "fn main(): unit [entry] {"), "IR: main resolved as the entry point");
        check(has(ir, "show(add(1:i32, 2:i32):i32):unit"), "IR: nested typed call resolved");
    }

    // P4-5 — pattern-match exhaustiveness.
    resolves("union Sh { Circle(r: f64), Square(s: f64) }\nfn area(x: Sh): f64 => match x { Circle(r) => r, Square(s) => s }\n", "P4: exhaustive union match resolves");
    rejects("union Sh { Circle(r: f64), Square(s: f64) }\nfn bad(x: Sh): f64 => match x { Circle(r) => r }\n", "P4: rejects non-exhaustive union match");
    rejects("enum Dir { N, S }\nfn f(d: Dir): i32 => match d { N => 1 }\n", "P4: rejects non-exhaustive enum match");
    rejects("fn g(b: bool): i32 => match b { true => 1 }\n", "P4: rejects non-exhaustive bool match");
    rejects("fn h(n: i32): i32 => match n { 0 => 0 }\n", "P4: rejects scalar match without a catch-all");
    resolves("fn ok2(n: i32): i32 => match n { 0 => 0, _ => 1 }\n", "P4: scalar match with catch-all resolves");
    resolves("enum Dir { N, S }\nfn turn(d: Dir): i32 => match d { N => 1, S => 2 }\n", "P4: exhaustive enum match resolves");

    // P5 — backend registry seam.
    check(findBackend("csharp") != nullptr && findBackend("csharp")->name() == "csharp", "P5: csharp backend registered");
    check(findBackend("typescript") != nullptr && findBackend("typescript")->name() == "typescript", "P5: typescript backend registered");
    check(findBackend("python") != nullptr && findBackend("python")->name() == "python", "P9: python backend registered");
    check(findBackend("ruby") == nullptr, "P5: unknown backend is not found");

    // P5 §3.E — per-target capability gating.
    {
        const char* extProg =
            "extension fn i32.doubled(): i32 => this * 2\n"
            "fn main() { let n = 5\n print(n.doubled()) }\n";
        DiagnosticBag d;
        auto unit = parse(lex(extProg, d), d);

        std::vector<FeatureUse> uses = collectFeatureUses(unit);
        bool sawExt = false;
        for (const auto& u : uses) if (u.key == featureName(Feature::ExtensionMethods)) sawExt = true;
        check(sawExt, "P5.E: collectFeatureUses detects extensionMethods");

        DiagnosticBag dcs;
        checkCapabilities(unit, *findBackend("csharp"), dcs);
        check(!dcs.hasErrors(), "P5.E: csharp (full capability set) accepts extension methods");

        StubBackend stub(Feature::ExtensionMethods);
        DiagnosticBag ds;
        checkCapabilities(unit, stub, ds);
        check(ds.hasErrors(), "P5.E: a backend lacking extensionMethods refuses the program");
        bool named = false;
        for (const auto& it : ds.items())
            if (has(it.message, "extensionMethods") && has(it.message, "stub")) named = true;
        check(named, "P5.E: the capability diagnostic names the feature and the target");

        // A feature the program does NOT use must not be gated.
        StubBackend noIter(Feature::Iterators);
        DiagnosticBag dn;
        checkCapabilities(unit, noIter, dn);
        check(!dn.hasErrors(), "P5.E: a missing capability the program doesn't use is not refused");
    }

    // P6 — §3.B refusals surface as targeted "Polyglot refuses X" diagnostics, not generic "unknown type".
    refuses("fn f(x: decimal) {}\n", "decimal", "P6: refuses 'decimal' with a targeted message");
    refuses("fn f(t: Thread) {}\n", "threads", "P6: refuses Thread (threads/locks)");
    refuses("fn f(p: IntPtr) {}\n", "pointers", "P6: refuses IntPtr (pointers/unsafe)");
    refuses("fn f(e: Expression<i32>) {}\n", "expression trees", "P6: refuses LINQ expression trees");
    refuses("fn f(d: dynamic) {}\n", "dynamic", "P6: refuses 'dynamic' / runtime code-gen");
    // §3.B statement forms: `lock`/`unsafe` are unspeakable, so a C#-habit statement is refused out loud
    // (a targeted message, not a confusing parse error) with no cascade errors.
    refuses("fn main() { lock (m) { print(1) } }\n", "locks", "P6: refuses a `lock` statement");
    refuses("fn main() { unsafe { print(1) } }\n", "unsafe", "P6: refuses an `unsafe` block");
    // §3.B: a finalizer (`~Name() {}`) is unspeakable — no cross-target deterministic-destruction contract
    // (issue #54). The parser refuses the `~` member form out loud instead of two raw parse errors.
    refuses("class Foo {\n  ~Foo() {\n  }\n}\n", "finalizers", "P6: refuses a finalizer (`~Name`)");
    // §3.B: a match arm must be an expression — a block-bodied arm is refused (Python/PHP lower `match`
    // to an expression fold with no statement block; issue #51 — was a silent drop-to-`0`).
    refuses("fn f(n: i32): i32 => match n {\n  0 => { return 1 }\n  _ => 0\n}\n",
            "block-bodied match arm", "P6: refuses a block-bodied match arm");
    { // #56a: an unresolvable method on a concrete string receiver (missing `import "std.strings"`) is
      // refused, not emitted verbatim to crash at every target's runtime.
        EmitResult r = compile("fn f(s: string): i32 => s.codePointAt(0)\n", findTarget("csharp"));
        bool named = false;
        for (const auto& d : r.diagnostics) if (has(d.message, "import missing")) named = true;
        check(!r.ok && named, "P36 #56a: unresolvable string method (missing import) is refused");
    }
    { // #38 (as relaxed by P37 B4): an interface type-test gates per-target — refused iff a configured
      // target lacks runtime interface identity (TS erases interfaces); a csharp-only build allows it.
        const char* src = "interface Shape {}\nclass Circle : Shape { constructor() {} }\n"
                          "fn f(s: Circle): i32 => match s {\n  x: Shape => 1\n  _ => 0\n}\n";
        check(compile(src, findTarget("csharp")).ok,
              "P36 #38 / P37 B4: an interface typed-match arm is allowed on a csharp-only build");
        EmitResult r = compile(src, findTarget("typescript"));
        bool named = false;
        for (const auto& d : r.diagnostics) if (has(d.message, "interfaces:runtimeIdentity")) named = true;
        check(!r.ok && named, "P36 #38 / P37 B4: an interface typed-match arm is refused when TS is configured");
    }
    { // #53: member overloading is not supported (top-level only) — a duplicate same-named method is
      // refused at the declaration site (was silently shadowed + a misleading call-site arity error).
        const char* src = "class Calc {\n  fn add(a: i32): i32 => a + 1\n  fn add(a: i32, b: i32): i32 => a + b\n}\n";
        EmitResult r = compile(src, findTarget("csharp"));
        bool named = false;
        for (const auto& d : r.diagnostics) if (has(d.message, "already defined")) named = true;
        check(!r.ok && named, "P36 #53: a duplicate member method is refused at the declaration site");
    }
    // P6 — function overloading: resolves by arity/type; true duplicates still rejected.
    resolvesStd("fn f(x: i32): i32 => x\nfn f(x: f64): i32 => 0\nfn f(a: i32, b: i32): i32 => a\n"
             "fn main() { print(f(1))\n print(f(1.0))\n print(f(1, 2)) }\n", "P6: function overloading resolves");
    rejects("fn f(x: i32): i32 => x\nfn f(y: i32): i32 => y\nfn main() {}\n",
            "P6: rejects a true duplicate (same parameter types)");

    // P7 — portable-core guard: `extern` is only allowed inside a target-gated `actual`.
    rejects("fn portable(): i32 => extern(\"42\")\nfn main() {}\n",
            "P7: extern is refused in portable code");
    resolvesStd("expect fn a(): i32\nactual(csharp) fn a(): i32 => extern(\"42\")\n"
             "actual(typescript) fn a(): i32 => extern(\"42\")\nfn main() { print(a()) }\n",
             "P7: extern is allowed inside an actual");

    // P9-V — target-gated portability (§3.B): calling a portable fn (one with `actual`s) on a target that has
    // no `actual` for it must refuse, not silently emit a call to an undefined function. Keyed on call sites,
    // so an *unused* portable fn missing this target's arm is fine (e.g. std.io.readText on Python).
    {
        const char* noPy = "expect fn answer(): i32\nactual(csharp) fn answer(): i32 => 42\n"
                           "actual(typescript) fn answer(): i32 => 42\nfn main() { print(answer()) }\n";
        EmitResult py = compileStd(noPy, findTarget("python"));
        bool named = false;
        for (const auto& d : py.diagnostics) if (has(d.message, "no 'actual'") && has(d.message, "python")) named = true;
        check(!py.ok && named, "P9-V: calling a portable fn with no actual for the target is refused");
        check(compileStd(noPy, findTarget("csharp")).ok, "P9-V: the same call compiles for a target that has the actual");
        check(compileStd("expect fn unused(): i32\nactual(csharp) fn unused(): i32 => 1\nfn main() { print(1) }\n",
                         findTarget("python")).ok,
              "P9-V: an unused portable fn missing this target's actual is not refused");
        // P40 review B5: the same call from a module value's INITIALIZER (`let SEP = sep()`) is refused too —
        // it used to build with exit 0 and fail at runtime (Python NameError / PHP undefined function).
        const char* topLet = "expect fn sep(): string\nactual(csharp) fn sep(): string => \"/\"\n"
                             "actual(typescript) fn sep(): string => \"/\"\nlet SEP = sep()\nfn main() { print(SEP) }\n";
        for (const char* t : {"python", "php"}) {
            EmitResult r = compileStd(topLet, findTarget(t));
            bool namedTop = false;
            for (const auto& d : r.diagnostics) if (has(d.message, "no 'actual'") && has(d.message, t)) namedTop = true;
            check(!r.ok && namedTop, std::string("B5: a portable call in a module initializer is refused on ") + t);
        }
        check(compileStd(topLet, findTarget("typescript")).ok, "B5: the module-initializer call compiles where the actual exists");
    }
    // P19 (1) — Python's two genuine target limits refuse out loud. Both previously emitted a silent
    // sentinel string (`__py_unsupported_block_lambda__` / `__py_unsupported_expr__`) into "valid" output —
    // the exact §3.B silent-broken-output failure mode P9-V caught for break/continue.
    {
        const char* blockLambda = "fn main() { var total: i32 = 0\n  let add = (n: i32) => { total += n }\n  add(2)\n  print(total) }\n";
        // P25 §4.18: python now SUPPORTS block lambdas by hoisting them to a nested `def` (Python `lambda` is
        // expression-only) — this supersedes the P19 refusal. The mutated capture becomes a `nonlocal`.
        EmitResult py = compileStd(blockLambda, findTarget("python"));
        check(py.ok && has(py.code, "def ") && has(py.code, "nonlocal total"),
              "P25: python emits a block lambda as a hoisted def with nonlocal");
        check(compileStd(blockLambda, findTarget("csharp")).ok && compileStd(blockLambda, findTarget("typescript")).ok,
              "P19: C#/TS still compile a statement-bodied lambda");

        // P19 (2): `with` emits everywhere via the lowering-precomputed ctor rebuild (Python included).
        const char* withExpr = "record P(x: i32, y: i32)\n"
                               "fn main() { let a = P(1, 2)\n  let b = a with { x = 3 }\n  print(b.x) }\n";
        EmitResult pyW = compileStd(withExpr, findTarget("python"));
        check(pyW.ok && has(pyW.code, "P(3, a.y)"), "P19: python with-expression -> ctor rebuild");
        EmitResult tsW = compileStd(withExpr, findTarget("typescript"));
        check(tsW.ok && has(tsW.code, "new P(3, a.y)"), "P19: TS with-expression -> ctor rebuild (simple base)");
        EmitResult csW = compileStd(withExpr, findTarget("csharp"));
        check(csW.ok && has(csW.code, "a with { x = 3 }"), "P19: C# with-expression stays native");
    }
    // P9-V audit — a tuple pattern in `match` binds + type-checks but has no lowering, so it must refuse
    // (never miscompile) rather than emit a call against undefined binders. (for-in destructuring is fine.)
    {
        EmitResult r = compileStd("fn f(p: (i32, i32)): i32 => match p { (a, b) => a + b }\n"
                                  "fn main() { print(f((1, 2))) }\n", findTarget("csharp"));
        bool named = false;
        for (const auto& d : r.diagnostics) if (has(d.message, "tuple patterns in 'match'")) named = true;
        check(!r.ok && named, "P9-V audit: a tuple pattern in match is refused, not miscompiled");
    }
    // INumber — a generic param bounded by the numeric marker must infer to a number; a non-numeric type
    // argument is refused at Polyglot compile time (better DX than a target-compiler error / TS NaN).
    {
        auto refusesNum = [&](const char* src, const std::string& name) {
            EmitResult r = compileStd(src, findTarget("csharp"));
            bool named = false;
            for (const auto& d : r.diagnostics) if (has(d.message, "INumber")) named = true;
            check(!r.ok && named, name);
        };
        refusesNum("fn main() { print(Math.max(\"a\", \"b\")) }\n", "INumber: Math.max on strings is refused");
        refusesNum("fn main() { print(Math.abs(true)) }\n", "INumber: Math.abs on bool is refused");
        resolvesStd("fn main() { print(Math.max(3, 8))\n print((i32)Math.round(2.7)) }\n",
                    "INumber: Math.max/round on numbers still resolve");
        // P28 — sign/clamp are generic <T: INumber> like abs/max, so the same constraint applies.
        refusesNum("fn main() { print(Math.sign(\"x\")) }\n", "INumber: Math.sign on strings is refused");
        resolvesStd("fn main() { print(Math.sign(-5))\n print(Math.clamp(7, 0, 10)) }\n",
                    "P28: Math.sign/clamp on numbers resolve");
        // The constraint is general, not Math-specific: a user `<T: INumber>` fn enforces it too.
        refusesNum("fn twice<T: INumber>(x: T): T => x\nfn main() { print(twice(\"hi\")) }\n",
                   "INumber: a user <T: INumber> fn rejects a non-numeric arg");
        resolvesStd("fn twice<T: INumber>(x: T): T => x\nfn main() { print(twice(21)) }\n",
                    "INumber: a user <T: INumber> fn accepts a numeric arg");
    }

    // P15 — async/await. Author writes the unwrapped `T` (§4.7 Option B); each backend synthesizes its wrapper
    // (C# Task<T> + GetAwaiter().GetResult(); TS Promise<T> + floating main(); Python async def + asyncio.run).
    {
        const char* prog =
            "async fn doubleIt(x: i32): i32 => x * 2\n"
            "async fn main() {\n  let r = await doubleIt(21)\n  print(r)\n}\n";

        EmitResult cs = compileStd(prog, findTarget("csharp"));
        check(cs.ok, "P15 C#: async program compiles");
        check(has(cs.code, "async global::System.Threading.Tasks.Task<int> doubleIt"),
              "P15 C#: async fn -> `async Task<T>` (unwrapped T wrapped by the backend)");
        check(has(cs.code, "async global::System.Threading.Tasks.Task main"),
              "P15 C#: async unit fn -> `async Task` (bare, no <T>)");
        check(has(cs.code, "await PolyglotProgram.doubleIt(21)"), "P15 C#: `await e` emits `await`");
        check(has(cs.code, "main().GetAwaiter().GetResult();"),
              "P15 C#: async main is driven synchronously from Main()");

        EmitResult ts = compileStd(prog, findTarget("typescript"));
        check(ts.ok, "P15 TS: async program compiles");
        check(has(ts.code, "async function doubleIt(x: number): Promise<number>"),
              "P15 TS: async fn -> `async function …: Promise<T>`");
        check(has(ts.code, "async function main(): Promise<void>"),
              "P15 TS: async unit fn -> `Promise<void>`");
        check(has(ts.code, "await doubleIt(21)"), "P15 TS: `await e` emits `await`");
        check(has(ts.code, "\nmain();\n"), "P15 TS: async main is a floating top-level call");

        EmitResult py = compileStd(prog, findTarget("python"));
        check(py.ok, "P15 Python: async program compiles");
        check(has(py.code, "async def doubleIt("), "P15 Python: async fn -> `async def`");
        check(has(py.code, "async def main("), "P15 Python: async main -> `async def`");
        check(has(py.code, "await doubleIt(21)"), "P15 Python: `await e` emits `await`");
        check(has(py.code, "asyncio.run(main())") && has(py.code, "import asyncio"),
              "P15 Python: async main -> asyncio.run + `import asyncio` prepended");

        // sema §4.7: `await` outside an async fn is refused (else a native compile error = a §3.B miscompile).
        {
            EmitResult r = compileStd("fn doubleIt(x: i32): i32 => x * 2\n"
                                      "fn main() { print(await doubleIt(1)) }\n", findTarget("csharp"));
            bool named = false;
            for (const auto& d : r.diagnostics) if (has(d.message, "await") && has(d.message, "async fn")) named = true;
            check(!r.ok && named, "P15 sema: `await` outside an async fn is refused");
        }
        // sema §4.7: `async` + `yield` (async iterators) is refused — out of scope for v1.
        {
            EmitResult r = compileStd("async fn g(): Iterable<i32> {\n  yield 1\n}\nfn main() { print(1) }\n",
                                      findTarget("csharp"));
            bool named = false;
            for (const auto& d : r.diagnostics) if (has(d.message, "async") && has(d.message, "yield")) named = true;
            check(!r.ok && named, "P15 sema: an `async` fn using `yield` is refused (no async iterators)");
        }
        // §3.E: a backend that lacks Async refuses an async program (proves the capability bites).
        {
            DiagnosticBag d;
            auto unit = parse(lex(prog, d), d);
            StubBackend noAsync(Feature::Async);
            DiagnosticBag da;
            checkCapabilities(unit, noAsync, da);
            bool named = false;
            for (const auto& it : da.items()) if (has(it.message, "async") && has(it.message, "stub")) named = true;
            check(da.hasErrors() && named, "P15 §3.E: a backend lacking `async` refuses the program");
            // all three real backends support async, so none of them gate it
            DiagnosticBag dcs; checkCapabilities(unit, *findBackend("csharp"), dcs);
            check(!dcs.hasErrors(), "P15 §3.E: csharp (full capability set) accepts async");
        }
    }

    // P15 — Awaitable<T> unwrap (§4.7). An async call types as `Awaitable<T>`; `await` unwraps it to `T`.
    // This lets sema catch "forgot to await" and "awaited a non-async value" — and mirrors C#/TS, where
    // `return f()` from an async fn (f async) requires `return await f()`.
    {
        auto asyncRefuses = [&](const char* src, const std::string& needle, const std::string& name) {
            EmitResult r = compileStd(src, findTarget("csharp"));
            bool named = false;
            for (const auto& d : r.diagnostics) if (has(d.message, needle)) named = true;
            check(!r.ok && named, name);
        };
        const char* leaf = "async fn f(): i32 => 21\n";
        // `await` unwraps to the underlying T: `let x = await f()` gives an i32 usable in arithmetic.
        resolvesStd((std::string(leaf) + "async fn main() {\n  let x = await f()\n  print(x + 1)\n}\n").c_str(),
                    "P15 Awaitable: `await f()` unwraps to i32 (usable in arithmetic)");
        // `return await g()` chains async fns (the awaited result is the unwrapped T).
        resolvesStd((std::string(leaf) + "async fn g(): i32 {\n  return await f()\n}\n"
                     "async fn main() { print(await g()) }\n").c_str(),
                    "P15 Awaitable: `return await g()` chains async calls");
        // Forgot-await, caught: returning an async call's Awaitable<i32> where i32 is expected is a type error.
        asyncRefuses((std::string(leaf) + "async fn g(): i32 {\n  return f()\n}\n"
                      "async fn main() { print(await g()) }\n").c_str(), "Awaitable",
                     "P15 Awaitable: `return f()` (missing await) is a type error");
        // Forgot-await, caught: printing an un-awaited async result would print a Task/Promise — refused.
        asyncRefuses((std::string(leaf) + "async fn main() { print(f()) }\n").c_str(), "await",
                     "P15 Awaitable: printing an un-awaited async result is refused");
        // Awaiting a plain value (not an async result) is a mistake — the only awaitables are async calls.
        asyncRefuses("fn plain(): i32 => 1\nasync fn main() {\n  let x = await plain()\n  print(x)\n}\n",
                     "not awaitable", "P15 Awaitable: awaiting a non-async call is refused");
    }

}
