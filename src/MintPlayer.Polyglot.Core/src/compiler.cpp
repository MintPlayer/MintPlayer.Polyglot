#include "mintplayer/polyglot/polyglot.hpp"

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

// Merge the declarations of any imported first-party std module into the unit, so its types and bindings
// are visible to sema/lowering. No filesystem resolver yet — std modules are embedded source.
void linkStdModules(CompilationUnit& unit, DiagnosticBag& diags) {
    bool wantsCollections = false;
    for (const auto& imp : unit.imports)
        if (imp.path == "std.collections") wantsCollections = true;
    if (!wantsCollections) return;

    std::vector<Token> toks = lex(STD_COLLECTIONS, diags);
    if (diags.hasErrors()) return;
    CompilationUnit std = parse(toks, diags);
    if (diags.hasErrors()) return;
    for (auto& c : std.classes)   unit.classes.push_back(std::move(c));
    for (auto& r : std.records)   unit.records.push_back(std::move(r));
    for (auto& f : std.functions) unit.functions.push_back(std::move(f));
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
