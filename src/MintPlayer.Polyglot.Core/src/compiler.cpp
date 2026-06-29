#include "mintplayer/polyglot/polyglot.hpp"

#include <algorithm>
#include <optional>
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

// std.io — filesystem access bound per target via the capability mechanism (expect + per-target actual).
// A top-level `actual` `extern("…")` is emitted verbatim, so the template references the function's
// parameters BY NAME (`path`, `content`) — they emit unchanged to both targets. Unit-returning bindings
// use a block body `{ extern("…") }` (an `=> extern` expression-body would expect a value). File IO is not
// on the §3.B refuse-list, so it's permitted.
const char* STD_IO = R"PG(
expect fn readText(path: string): string
actual(csharp)     fn readText(path: string): string => extern("global::System.IO.File.ReadAllText(path)")
actual(typescript) fn readText(path: string): string => extern("require('fs').readFileSync(path, 'utf8')")

expect fn writeText(path: string, content: string)
actual(csharp)     fn writeText(path: string, content: string) { extern("global::System.IO.File.WriteAllText(path, content)") }
actual(typescript) fn writeText(path: string, content: string) { extern("require('fs').writeFileSync(path, content)") }

expect fn appendText(path: string, content: string)
actual(csharp)     fn appendText(path: string, content: string) { extern("global::System.IO.File.AppendAllText(path, content)") }
actual(typescript) fn appendText(path: string, content: string) { extern("require('fs').appendFileSync(path, content)") }

expect fn fileExists(path: string): bool
actual(csharp)     fn fileExists(path: string): bool => extern("global::System.IO.File.Exists(path)")
actual(typescript) fn fileExists(path: string): bool => extern("require('fs').existsSync(path)")

expect fn deleteFile(path: string)
actual(csharp)     fn deleteFile(path: string) { extern("global::System.IO.File.Delete(path)") }
actual(typescript) fn deleteFile(path: string) { extern("require('fs').unlinkSync(path)") }
)PG";

// The first-party std module registry: module path -> embedded .pg source. Adding a std module is data,
// not control flow. No filesystem resolver yet (the source is compiled into the binary); user-module
// resolution across files is future work.
struct StdModule { const char* path; const char* source; };
const StdModule STD_MODULES[] = {
    {"std.collections", STD_COLLECTIONS},
    {"std.io", STD_IO},
};

// Append all top-level declarations of a loaded module into the root unit (the module's own `imports` were
// already processed by the caller). Output stays single-file per target: everything lands in one unit.
void mergeDecls(CompilationUnit& mod, CompilationUnit& root) {
    for (auto& c : mod.classes)    root.classes.push_back(std::move(c));
    for (auto& r : mod.records)    root.records.push_back(std::move(r));
    for (auto& e : mod.enums)      root.enums.push_back(std::move(e));
    for (auto& u : mod.unions)     root.unions.push_back(std::move(u));
    for (auto& i : mod.interfaces) root.interfaces.push_back(std::move(i));
    for (auto& v : mod.values)     root.values.push_back(std::move(v));
    for (auto& f : mod.functions)  root.functions.push_back(std::move(f));
}

void validateImportNames(const ImportDecl& imp, const CompilationUnit& mod, DiagnosticBag& diags) {
    if (imp.names.empty()) return; // namespace / bare import: nothing to validate
    std::unordered_set<std::string> exports;
    for (const auto& c : mod.classes)    exports.insert(c.name);
    for (const auto& r : mod.records)    exports.insert(r.name);
    for (const auto& e : mod.enums)      exports.insert(e.name);
    for (const auto& u : mod.unions)     exports.insert(u.name);
    for (const auto& i : mod.interfaces) exports.insert(i.name);
    for (const auto& v : mod.values)     exports.insert(v.name);
    for (const auto& f : mod.functions)  exports.insert(f.name);
    for (const auto& n : imp.names)
        if (!exports.count(n.name)) diags.error(imp.pos, "module '" + imp.path + "' has no export '" + n.name + "'");
}

// Recursively resolve+load every module `unit` imports, merging each (and its transitive dependencies,
// dependencies-first) into `root`. `std.*` is served from the embedded registry; everything else goes
// through `resolver`. `visited` dedups; `stack` (the in-progress chain) detects import cycles.
void loadImports(CompilationUnit& root, const CompilationUnit& unit, const std::string& selfPath,
                 ModuleResolver* resolver, DiagnosticBag& diags,
                 std::unordered_set<std::string>& visited, std::vector<std::string>& stack) {
    stack.push_back(selfPath);
    for (const auto& imp : unit.imports) {
        std::string source, canon;
        if (imp.path.rfind("std.", 0) == 0) { // first-party std: embedded, no IO
            const char* src = nullptr;
            for (const auto& m : STD_MODULES) if (imp.path == m.path) src = m.source;
            if (!src) { diags.error(imp.pos, "unknown module '" + imp.path + "'"); continue; }
            source = src;
            canon = imp.path;
        } else { // user module: ask the resolver (none => unresolvable)
            std::optional<ResolvedModule> r = resolver ? resolver->resolve(imp.path, selfPath) : std::nullopt;
            if (!r) { diags.error(imp.pos, "unknown module '" + imp.path + "'"); continue; }
            source = std::move(r->source);
            canon = std::move(r->canonicalPath);
        }

        if (std::find(stack.begin(), stack.end(), canon) != stack.end()) {
            std::string chain;
            for (const auto& s : stack) chain += (s.empty() ? "<entry>" : s) + " -> ";
            diags.error(imp.pos, "import cycle: " + chain + canon);
            continue;
        }
        if (!visited.insert(canon).second) continue; // already loaded (imported more than once)

        std::vector<Token> toks = lex(source, diags);
        if (diags.hasErrors()) return;
        CompilationUnit mod = parse(toks, diags);
        if (diags.hasErrors()) return;

        validateImportNames(imp, mod, diags);
        loadImports(root, mod, canon, resolver, diags, visited, stack); // dependencies first (post-order)
        mergeDecls(mod, root);
    }
    stack.pop_back();
}

// Resolve and merge the transitive closure of `unit`'s imports into it (see loadImports).
void linkModules(CompilationUnit& unit, ModuleResolver* resolver, DiagnosticBag& diags) {
    std::unordered_set<std::string> visited;
    std::vector<std::string> stack;
    loadImports(unit, unit, "", resolver, diags, visited, stack);
}

} // namespace

EmitResult compile(const std::string& source, Target target, ModuleResolver* resolver) {
    EmitResult result;
    DiagnosticBag diags;

    std::vector<Token> tokens = lex(source, diags);
    if (diags.hasErrors()) { result.diagnostics = diags.items(); return result; }

    CompilationUnit unit = parse(tokens, diags);
    if (diags.hasErrors()) { result.diagnostics = diags.items(); return result; }

    linkModules(unit, resolver, diags); // pull in imported std + user modules (transitively) before checking
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
