#include "mintplayer/polyglot/polyglot.hpp"

#include <unordered_set>

#include "mintplayer/polyglot/ast.hpp"
#include "mintplayer/polyglot/emit.hpp"
#include "mintplayer/polyglot/backend.hpp"
#include "mintplayer/polyglot/capability.hpp"
#include "mintplayer/polyglot/lexer.hpp"
#include "mintplayer/polyglot/lower.hpp"
#include "mintplayer/polyglot/parser.hpp"
#include "mintplayer/polyglot/pg_printer.hpp"
#include "mintplayer/polyglot/sema.hpp"

namespace mintplayer::polyglot {

namespace {

// First-party std modules, written in .pg and bound to each target via per-target FFI templates
// (extern class + `actual(target) extern("…")` arms). `List<T>` maps to System.Collections.Generic.List
// in C# and a native array in TS; the literal/index/for-in syntax is handled by the compiler, the
// operations by these bindings. `$this` is the receiver; `$0` the first argument.
const char* STD_COLLECTIONS = R"PG(
extern class List<T> {
  let count: i32 {
    actual(csharp)     extern("$this.Count")
    actual(typescript) extern("$this.length")
  }
  fn add(item: T) {
    actual(csharp)     extern("$this.Add($0)")
    actual(typescript) extern("$this.push($0)")
  }
  fn clear() {
    actual(csharp)     extern("$this.Clear()")
    actual(typescript) extern("$this = []")
  }
  fn removeAll(pred: (T) => bool) {
    actual(csharp)     extern("$this.RemoveAll($0)")
    actual(typescript) extern("$this = $this.filter(__e => !(($0)(__e)))")
  }
}
)PG";

// The first-party std module registry: module path -> embedded .pg source. Adding a std module is data,
// not control flow. No filesystem resolver yet (the source is compiled into the binary); user-module
// resolution across files is future work.
struct StdModule { const char* path; const char* source; };
const StdModule STD_MODULES[] = {
    {"std.collections", STD_COLLECTIONS},
};

// Resolve each `import`: a known `std.*` module is parsed and merged into the unit (once), with its
// selectively-imported names validated against what it actually exports. An unknown `std.*` module is a
// diagnostic. Non-`std.` imports have no resolver yet and are left untouched (future user-module work).
void linkStdModules(CompilationUnit& unit, DiagnosticBag& diags) {
    std::unordered_set<std::string> merged;
    for (const auto& imp : unit.imports) {
        if (imp.path.rfind("std.", 0) != 0) continue; // not a std module — leave for future resolution

        const char* source = nullptr;
        for (const auto& m : STD_MODULES) if (imp.path == m.path) source = m.source;
        if (!source) { diags.error(imp.pos, "unknown module '" + imp.path + "'"); continue; }
        if (!merged.insert(imp.path).second) continue; // already linked (imported more than once)

        std::vector<Token> toks = lex(source, diags);
        if (diags.hasErrors()) return;
        CompilationUnit mod = parse(toks, diags);
        if (diags.hasErrors()) return;

        // Validate the selective import list `{ A, B }` against the module's exported declarations.
        if (!imp.names.empty()) {
            std::unordered_set<std::string> exports;
            for (const auto& c : mod.classes)    exports.insert(c.name);
            for (const auto& r : mod.records)    exports.insert(r.name);
            for (const auto& e : mod.enums)      exports.insert(e.name);
            for (const auto& u : mod.unions)     exports.insert(u.name);
            for (const auto& i : mod.interfaces) exports.insert(i.name);
            for (const auto& f : mod.functions)  exports.insert(f.name);
            for (const auto& n : imp.names)
                if (!exports.count(n)) diags.error(imp.pos, "module '" + imp.path + "' has no export '" + n + "'");
        }

        for (auto& c : mod.classes)    unit.classes.push_back(std::move(c));
        for (auto& r : mod.records)    unit.records.push_back(std::move(r));
        for (auto& e : mod.enums)      unit.enums.push_back(std::move(e));
        for (auto& u : mod.unions)     unit.unions.push_back(std::move(u));
        for (auto& i : mod.interfaces) unit.interfaces.push_back(std::move(i));
        for (auto& f : mod.functions)  unit.functions.push_back(std::move(f));
    }
}

} // namespace

EmitResult compile(const std::string& source, Target target) {
    EmitResult result;
    DiagnosticBag diags;

    std::vector<Token> tokens = lex(source, diags);
    if (diags.hasErrors()) { result.diagnostics = diags.items(); return result; }

    CompilationUnit unit = parse(tokens, diags);
    if (diags.hasErrors()) { result.diagnostics = diags.items(); return result; }

    linkStdModules(unit, diags); // pull in imported first-party std types (e.g. List<T>) before checking
    if (diags.hasErrors()) { result.diagnostics = diags.items(); return result; }

    check(unit, diags);
    if (diags.hasErrors()) { result.diagnostics = diags.items(); return result; }

    const Backend* backend = findBackend(target == Target::CSharp ? "csharp" : "typescript");
    // §3.E: refuse any used feature this target can't emit, before lowering/emitting (never miscompile).
    checkCapabilities(unit, *backend, diags);
    if (diags.hasErrors()) { result.diagnostics = diags.items(); return result; }

    ir::Module module = lower(unit);
    result.code = backend->emit(module);
    result.ok = true;
    return result;
}

EmitResult format(const std::string& source) {
    EmitResult result;
    DiagnosticBag diags;

    std::vector<Token> tokens = lex(source, diags);
    if (diags.hasErrors()) { result.diagnostics = diags.items(); return result; }

    CompilationUnit unit = parse(tokens, diags);
    if (diags.hasErrors()) { result.diagnostics = diags.items(); return result; }

    result.code = printSource(unit);
    result.ok = true;
    return result;
}

} // namespace mintplayer::polyglot
