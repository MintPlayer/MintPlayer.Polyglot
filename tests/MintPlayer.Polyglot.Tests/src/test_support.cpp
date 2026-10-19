// P41 D14: definitions of the shared test harness (see test_support.hpp).

#include "test_support.hpp"

int g_failures = 0;

void check(bool condition, const std::string& name) {
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << name << "\n";
    if (!condition) ++g_failures;
}

bool has(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

EmitResult compileStd(const std::string& src, const BackendHandle& target) {
    return compile(src, target, nullptr, LibConfig{{"io", "math", "collections"}});
}

// Type checker rejects bad programs with diagnostics (never a miscompile).
void rejects(const std::string& src, const std::string& name) {
    EmitResult r = compile(src, findTarget("csharp"));
    check(!r.ok && !r.diagnostics.empty(), name);
}

void resolves(const std::string& src, const std::string& name) {
    EmitResult r = compile(src, findTarget("csharp"));
    check(r.ok, name);
}

// Same, but with the std prelude (for programs that call `print` and friends).
void resolvesStd(const std::string& src, const std::string& name) {
    EmitResult r = compileStd(src, findTarget("csharp"));
    check(r.ok, name);
}

// §3.B refusals surface as targeted "Polyglot refuses X" diagnostics, not generic "unknown type".
void refuses(const std::string& src, const std::string& needle, const std::string& name) {
    EmitResult r = compile(src, findTarget("csharp"));
    bool named = false;
    for (const auto& d : r.diagnostics) if (has(d.message, "refuses") && has(d.message, needle)) named = true;
    check(!r.ok && named, name);
}

std::string firstErr(const EmitResult& r) {
    return r.diagnostics.empty() ? std::string() : r.diagnostics[0].message;
}

std::string typeErr(const std::string& src) { // the first diagnostic of a csharp compile ("" = it compiled)
    EmitResult r = compileStd(src, findTarget("csharp"));
    return r.ok ? std::string() : firstErr(r);
}

std::string runRule(const std::string& json, const engine::EvalContext& ctx, bool* okOut) {
    bool ok = true;
    std::string err;
    engine::Rule r = engine::parseRule(json::parse(json), ok, err);
    if (okOut) *okOut = ok;
    return ok ? engine::evalRule(r, ctx) : std::string("<parse-error:" + err + ">");
}

const char* kProgram =
    "fn add(a: i32, b: i32): i32 {\n"
    "  return a + b\n"
    "}\n"
    "fn main() {\n"
    "  let x = 7\n"
    "  var sum = 0\n"
    "  var i = 1\n"
    "  while i <= x {\n"
    "    sum = sum + i\n"
    "    i = i + 1\n"
    "  }\n"
    "  print(add(sum, 100))\n"
    "  if sum > 20 { print(sum) } else { print(0) }\n"
    "}\n";
