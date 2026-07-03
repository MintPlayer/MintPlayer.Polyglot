#include "mintplayer/polyglot/polyglot.hpp"

#include <algorithm>
#include <optional>
#include <unordered_set>

#include "mintplayer/polyglot/ast.hpp"
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
  type {
    actual(csharp)     extern("global::System.Collections.Generic.List<$0>")
    actual(typescript) extern("$0[]")
    actual(python)     extern("list")
  }
  init() {
    actual(csharp)     extern("new $T()")
    actual(typescript) extern("[]")
    actual(python)     extern("[]")
  }
  let count: i32 {
    actual(csharp)     extern("$this.Count")
    actual(typescript) extern("$this.length")
    actual(python)     extern("len($this)")
  }
  fn add(item: T) {
    actual(csharp)     extern("$this.Add($0)")
    actual(typescript) extern("$this.push($0)")
    actual(python)     extern("$this.append($0)")
  }
  fn clear() {
    actual(csharp)     extern("$this.Clear()")
    actual(typescript) extern("$this = []")
    actual(python)     extern("$this = []")
  }
  fn removeAll(pred: (T) => bool) {
    actual(csharp)     extern("$this.RemoveAll($0)")
    actual(typescript) extern("$this = $this.filter(__e => !(($0)(__e)))")
    actual(python)     extern("$this = [__e for __e in $this if not (($0)(__e))]")
  }
  fn removeAt(index: i32) {
    actual(csharp)     extern("$this.RemoveAt($0)")
    actual(typescript) extern("$this.splice($0, 1)")
    actual(python)     extern("$this.pop($0)")
  }
}
)PG";

// std.io — filesystem access bound per target via the capability mechanism (expect + per-target actual).
// A top-level `actual` `extern("…")` is emitted verbatim, so the template references the function's
// parameters BY NAME (`path`, `content`) — they emit unchanged to both targets. Unit-returning bindings
// use a block body `{ extern("…") }` (an `=> extern` expression-body would expect a value). File IO is not
// on the §3.B refuse-list, so it's permitted.
const char* STD_IO = R"PG(
expect fn print<T>(x: T)
actual(csharp)     fn print<T>(x: T) { extern("global::System.Console.WriteLine(x is bool __pb ? (__pb ? \"true\" : \"false\") : (object)x)") }
actual(typescript) fn print<T>(x: T) { extern("console.log(String(x))") }
actual(python)     fn print<T>(x: T) { extern("__builtins__.print(str(x).lower() if isinstance(x, bool) else x)") }

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

// std.math — the Math namespace as a native-backed `extern class` (preserving the `Math.sqrt(x)` / `Math.PI`
// surface). sqrt/ln/floor/ceil are f64; min/max/abs are generic and CALL-SITE-INLINED (a generic C#
// Math.Min over unconstrained T wouldn't compile, so the binding substitutes the concrete-typed args at
// each call). The TS min/max/abs use single-eval IIFE templates that work for both `number` and `bigint`
// (the `a - a` zero is type-agnostic). PI/E are bound consts. Determinism (§3.D): only sqrt is reproducible
// across targets; ln/transcendentals are not promised bit-exact.
const char* STD_MATH = R"PG(
extern class Math {
  const PI: f64 {
    actual(csharp)     extern("global::System.Math.PI")
    actual(typescript) extern("Math.PI")
    actual(python)     extern("__import__('math').pi")
  }
  const E: f64 {
    actual(csharp)     extern("global::System.Math.E")
    actual(typescript) extern("Math.E")
    actual(python)     extern("__import__('math').e")
  }
  static fn sqrt(x: f64): f64 {
    actual(csharp)     extern("global::System.Math.Sqrt($0)")
    actual(typescript) extern("Math.sqrt($0)")
    actual(python)     extern("__import__('math').sqrt($0)")
  }
  static fn ln(x: f64): f64 {
    actual(csharp)     extern("global::System.Math.Log($0)")
    actual(typescript) extern("Math.log($0)")
    actual(python)     extern("__import__('math').log($0)")
  }
  static fn floor(x: f64): f64 {
    actual(csharp)     extern("global::System.Math.Floor($0)")
    actual(typescript) extern("Math.floor($0)")
    actual(python)     extern("float(__import__('math').floor($0))")
  }
  static fn ceil(x: f64): f64 {
    actual(csharp)     extern("global::System.Math.Ceiling($0)")
    actual(typescript) extern("Math.ceil($0)")
    actual(python)     extern("float(__import__('math').ceil($0))")
  }
  static fn min<T: INumber>(a: T, b: T): T {
    actual(csharp)     extern("global::System.Math.Min($0, $1)")
    actual(typescript) extern("((a, b) => (a <= b ? a : b))($0, $1)")
    actual(python)     extern("min($0, $1)")
  }
  static fn max<T: INumber>(a: T, b: T): T {
    actual(csharp)     extern("global::System.Math.Max($0, $1)")
    actual(typescript) extern("((a, b) => (a >= b ? a : b))($0, $1)")
    actual(python)     extern("max($0, $1)")
  }
  static fn abs<T: INumber>(x: T): T {
    actual(csharp)     extern("global::System.Math.Abs($0)")
    actual(typescript) extern("((a) => (a < (a - a) ? -a : a))($0)")
    actual(python)     extern("abs($0)")
  }
  // round-to-nearest, type-preserving (f32 in -> f32 out, f64 -> f64). Generic covers both float widths, so
  // no overload is needed. C# `Math.Round` has only a `double` overload, so the result is cast back to `$T`
  // (the inferred return type: `float`/`double`); JS `Math.round` and Python `round` take a number directly.
  // Halfway cases are NOT bit-reproducible cross-target (C#/Python round-half-to-even, JS rounds half up) —
  // §3.D covers only + - * / sqrt — so callers relying on exact parity must avoid exact-.5 inputs.
  static fn round<T: INumber>(x: T): T {
    actual(csharp)     extern("(($T) global::System.Math.Round($0))")
    actual(typescript) extern("Math.round($0)")
    actual(python)     extern("float(round($0))")
  }
}
)PG";

// std.strings — string methods as bound extension methods (a method on the builtin `string`). Each arm maps
// `s.method(args)` to the target's idiom (C# `.ToUpper()` / JS `.toUpperCase()`). Both targets are UTF-16
// (SPEC §8) so `len`/`charAt` are code-unit operations; `codePoints` iterates code points (surrogate-aware).
const char* STD_STRINGS = R"PG(
extension fn string.isEmpty(): bool {
  actual(csharp)     extern("($this.Length == 0)")
  actual(typescript) extern("($this.length === 0)")
  actual(python)     extern("(len($this) == 0)")
}
extension fn string.len(): i32 {
  actual(csharp)     extern("$this.Length")
  actual(typescript) extern("$this.length")
  actual(python)     extern("len($this)")
}
extension fn string.toUpper(): string {
  actual(csharp)     extern("$this.ToUpper()")
  actual(typescript) extern("$this.toUpperCase()")
  actual(python)     extern("$this.upper()")
}
extension fn string.toLower(): string {
  actual(csharp)     extern("$this.ToLower()")
  actual(typescript) extern("$this.toLowerCase()")
  actual(python)     extern("$this.lower()")
}
extension fn string.charAt(index: i32): string {
  actual(csharp)     extern("$this[$0].ToString()")
  actual(typescript) extern("$this.charAt($0)")
  actual(python)     extern("$this[$0]")
}
extension fn string.codePoints(): Iterable<string> {
  actual(csharp)     extern("$this.EnumerateRunes()")
  actual(typescript) extern("Array.from($this)")
  actual(python)     extern("list($this)")
}
extension fn string.toI32(): i32 {
  actual(csharp)     extern("global::System.Int32.Parse($this)")
  actual(typescript) extern("parseInt($this, 10)")
  actual(python)     extern("int($this)")
}
)PG";

// The core prelude — `extern class`es always linked into every compilation (no import: `throw`/typed
// `catch`/`Error`/`yield`/`Iterable` are core language surface, not std). They declare their per-target
// type spelling (and, for Error, construction + the `message` property) via the binding mechanism, so the
// emitters carry no hardcoded Error/Iterable mapping — they're data here, dogfooding the P10 plugin path.
// The scalar `parse` classes (P19 slice 1, the P13 "std, not compiler" move): sema still types
// `i32.parse(s)` intrinsically, but the EMISSION comes from these binding arms via the general `ir::Bound`
// path — deleting the per-target parse special case each backend carried. Scalar names are plain
// identifiers, so `extern class i32` parses; the classes declare no `type` block, so the scalar spelling
// tables stay authoritative.
const char* STD_CORE = R"PG(
extern class Error {
  type {
    actual(csharp)     extern("global::System.Exception")
    actual(typescript) extern("Error")
    actual(python)     extern("Exception")
  }
  init(message: string) {
    actual(csharp)     extern("new $T($0)")
    actual(typescript) extern("new $T($0)")
    actual(python)     extern("$T($0)")
  }
  let message: string {
    actual(csharp)     extern("$this.Message")
    actual(typescript) extern("$this.message")
    actual(python)     extern("str($this)")
  }
}
extern class Iterable<T> {
  type {
    actual(csharp)     extern("global::System.Collections.Generic.IEnumerable<$0>")
    actual(typescript) extern("Iterable<$0>")
    actual(python)     extern("list")
  }
}
// A marker constraint for numeric type parameters, like .NET's System.Numerics.INumber<T>: the numeric
// scalars (i8..u64, f32, f64) satisfy it. Used only as a generic bound (`<T: INumber>`) so a non-numeric
// type argument to a numeric-generic (Math.min/max/abs/round, or user code) is rejected at Polyglot compile
// time — better DX than the target compiler catching it (C#) or, worse, a silent NaN (TS). It is
// compile-time-only: the bound is ERASED from the emitted C#/TS (see csWhere / tsGenerics), since the target
// languages already type-check numeric operations and have no matching constraint spelling.
extern class INumber { }
union Option<T> {
  Some(value: T)
  None
}
extern class i8 { static fn parse(s: string): i8 {
  actual(csharp) extern("sbyte.Parse($0)") actual(typescript) extern("(Number.parseInt($0, 10) << 24 >> 24)") actual(python) extern("int($0)") } }
extern class i16 { static fn parse(s: string): i16 {
  actual(csharp) extern("short.Parse($0)") actual(typescript) extern("(Number.parseInt($0, 10) << 16 >> 16)") actual(python) extern("int($0)") } }
extern class i32 { static fn parse(s: string): i32 {
  actual(csharp) extern("int.Parse($0)") actual(typescript) extern("(Number.parseInt($0, 10) | 0)") actual(python) extern("int($0)") } }
extern class i64 { static fn parse(s: string): i64 {
  actual(csharp) extern("long.Parse($0)") actual(typescript) extern("BigInt($0)") actual(python) extern("int($0)") } }
extern class u8 { static fn parse(s: string): u8 {
  actual(csharp) extern("byte.Parse($0)") actual(typescript) extern("(Number.parseInt($0, 10) & 0xff)") actual(python) extern("int($0)") } }
extern class u16 { static fn parse(s: string): u16 {
  actual(csharp) extern("ushort.Parse($0)") actual(typescript) extern("(Number.parseInt($0, 10) & 0xffff)") actual(python) extern("int($0)") } }
extern class u32 { static fn parse(s: string): u32 {
  actual(csharp) extern("uint.Parse($0)") actual(typescript) extern("(Number.parseInt($0, 10) >>> 0)") actual(python) extern("int($0)") } }
extern class u64 { static fn parse(s: string): u64 {
  actual(csharp) extern("ulong.Parse($0)") actual(typescript) extern("BigInt($0)") actual(python) extern("int($0)") } }
extern class f32 { static fn parse(s: string): f32 {
  actual(csharp) extern("float.Parse($0, global::System.Globalization.CultureInfo.InvariantCulture)") actual(typescript) extern("Number($0)") actual(python) extern("float($0)") } }
extern class f64 { static fn parse(s: string): f64 {
  actual(csharp) extern("double.Parse($0, global::System.Globalization.CultureInfo.InvariantCulture)") actual(typescript) extern("Number($0)") actual(python) extern("float($0)") } }
)PG";

// The first-party std module registry: module path -> embedded .pg source. Adding a std module is data,
// not control flow. No filesystem resolver yet (the source is compiled into the binary); user-module
// resolution across files is future work.
struct StdModule { const char* path; const char* source; };
const StdModule STD_MODULES[] = {
    {"std.collections", STD_COLLECTIONS},
    {"std.io", STD_IO},
    {"std.math", STD_MATH},
    {"std.strings", STD_STRINGS},
};

// The always-linked core prelude's logical name, so its embedded source is servable (Error/Iterable/…).
const char* kCoreModuleName = "std.core";

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
    for (auto& e : mod.extensions) root.extensions.push_back(std::move(e)); // e.g. std.strings' bound methods
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
                 std::unordered_set<std::string>& visited, std::vector<std::string>& stack, SourceMap* src) {
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

        int fileId = src ? src->add(canon) : 0; // stamp this module's tokens so its positions stay unambiguous
        std::vector<Token> toks = lex(source, diags, fileId);
        if (diags.hasErrors()) return;
        CompilationUnit mod = parse(toks, diags);
        if (diags.hasErrors()) return;

        validateImportNames(imp, mod, diags);
        loadImports(root, mod, canon, resolver, diags, visited, stack, src); // dependencies first (post-order)
        mergeDecls(mod, root);
    }
    stack.pop_back();
}

// Merge the always-on core prelude (Error/Iterable) into `unit`. Idempotent and user-shadow-safe: a core
// type already declared in `unit` (e.g. a user `class Error`) is left to collide in sema, not silently
// double-merged. Runs before any import linking so the core types are authoritative.
void linkCoreModule(CompilationUnit& unit, DiagnosticBag& diags, SourceMap* src) {
    std::vector<Token> toks = lex(STD_CORE, diags, src ? src->add(kCoreModuleName) : 0);
    if (diags.hasErrors()) return;
    CompilationUnit core = parse(toks, diags);
    if (diags.hasErrors()) return;
    std::unordered_set<std::string> have;
    for (const auto& c : unit.classes) have.insert(c.name);
    for (const auto& u : unit.unions)  have.insert(u.name);
    for (auto& c : core.classes) if (!have.count(c.name)) unit.classes.push_back(std::move(c));
    for (auto& u : core.unions)  if (!have.count(u.name)) unit.unions.push_back(std::move(u));
}

// Resolve and merge the transitive closure of `unit`'s imports into it (see loadImports).
void linkModules(CompilationUnit& unit, ModuleResolver* resolver, DiagnosticBag& diags, SourceMap* src) {
    std::unordered_set<std::string> visited;
    std::vector<std::string> stack;
    loadImports(unit, unit, "", resolver, diags, visited, stack, src);
}

// A stable identity string for a type (so a lib function only shadows a user function of the SAME signature,
// letting user overloads of a lib function coexist).
std::string typeKey(const TypeRef& t) {
    std::string s = t.name;
    if (t.nullable) s += "?";
    if (!t.args.empty()) { s += "<"; for (const auto& a : t.args) s += typeKey(a) + ","; s += ">"; }
    return s;
}
std::string fnSigKey(const std::string& name, const std::vector<Param>& ps) {
    std::string s = name + "(";
    for (const auto& p : ps) s += typeKey(p.type) + ",";
    return s + ")";
}

// Auto-import the `lib` prelude. Each entry ("io" -> std.io) is linked whole-module into a staging unit,
// then its declarations are merged into `unit` ONLY where they don't shadow an existing identity — a user
// declaration or an explicitly-imported one (functions compared by name+signature, so user overloads of a
// lib function survive). Shadowed lib decls are dropped SILENTLY (ambient, lowest priority). A name exported
// by two different lib entries is left to collide in sema (a prelude must be internally consistent).
void linkLibModules(CompilationUnit& unit, const LibConfig& lib, ModuleResolver* resolver, DiagnosticBag& diags,
                    SourceMap* src) {
    if (lib.libs.empty()) return;

    CompilationUnit staging;
    for (const auto& name : lib.libs) {
        ImportDecl d;
        // A bare word ("io") is sugar for the std module `std.io`; a qualified name ("acme.physics") is a
        // full specifier used as-is — resolved through the same chain as `import` (std registry, then the
        // resolver / plugin registry). So a third-party plugin auto-imports by its own namespace, no
        // per-publisher special-casing.
        d.path = name.find('.') != std::string::npos ? name : "std." + name;
        staging.imports.push_back(std::move(d));
    }
    linkModules(staging, resolver, diags, src); // stamp lib modules so their symbols get fileIds (std click-through)
    if (diags.hasErrors()) return;

    std::unordered_set<std::string> types, values, fns, exts;
    for (const auto& d : unit.records)    types.insert(d.name);
    for (const auto& d : unit.classes)    types.insert(d.name);
    for (const auto& d : unit.interfaces) types.insert(d.name);
    for (const auto& d : unit.enums)      types.insert(d.name);
    for (const auto& d : unit.unions)     types.insert(d.name);
    for (const auto& v : unit.values)     values.insert(v.name);
    for (const auto& f : unit.functions)  fns.insert(fnSigKey(f.name, f.params));
    for (const auto& e : unit.extensions) exts.insert(e.receiver.name + "." + e.name);

    for (auto& d : staging.records)    if (!types.count(d.name)) unit.records.push_back(std::move(d));
    for (auto& d : staging.classes)    if (!types.count(d.name)) unit.classes.push_back(std::move(d));
    for (auto& d : staging.interfaces) if (!types.count(d.name)) unit.interfaces.push_back(std::move(d));
    for (auto& d : staging.enums)      if (!types.count(d.name)) unit.enums.push_back(std::move(d));
    for (auto& d : staging.unions)     if (!types.count(d.name)) unit.unions.push_back(std::move(d));
    for (auto& v : staging.values)     if (!values.count(v.name)) unit.values.push_back(std::move(v));
    for (auto& f : staging.functions)  if (!fns.count(fnSigKey(f.name, f.params))) unit.functions.push_back(std::move(f));
    for (auto& e : staging.extensions) if (!exts.count(e.receiver.name + "." + e.name)) unit.extensions.push_back(std::move(e));
}

} // namespace

// The embedded source of a first-party module by logical name (for the language server to serve std
// definitions as read-only virtual documents). Empty when the name isn't a known embedded module.
std::string embeddedModuleSource(const std::string& name) {
    if (name == kCoreModuleName || name == "core") return STD_CORE;
    for (const auto& m : STD_MODULES) if (name == m.path) return m.source;
    return std::string();
}

// Link the core prelude + imported + lib modules into a parsed `unit`, then type-check it — the shared
// front end behind both `compile()` (which then lowers/emits) and `analyze()` (which returns the checked
// AST for tooling). Bails between passes on the first error. `model`/`req` are forwarded to `check` so the
// LSP path can request a semantic model; `compile` passes nullptr and pays nothing.
bool runFrontEnd(CompilationUnit& unit, ModuleResolver* resolver, const LibConfig& lib, DiagnosticBag& diags,
                 SemanticModel* model, const SemanticRequest* req, SourceMap* src) {
    linkCoreModule(unit, diags, src);               if (diags.hasErrors()) return false; // Error/Iterable (no import)
    linkModules(unit, resolver, diags, src);        if (diags.hasErrors()) return false; // imported std + user modules
    linkLibModules(unit, lib, resolver, diags, src); if (diags.hasErrors()) return false; // ambient prelude
    check(unit, diags, model, req);
    return !diags.hasErrors();
}

BackendHandle findTarget(const std::string& name) {
    BackendHandle h;
    h.name_ = name;
    h.backend_ = findBackend(name);
    if (!h.backend_) {
        std::string known;
        for (const auto& n : backendNames()) { if (!known.empty()) known += ", "; known += n; }
        h.error_ = known.empty()
                       ? "unknown target '" + name + "' (no target plugins are loaded — expected "
                             "plugins/<target>/polyglot-plugin.json next to the executable)"
                       : "unknown target '" + name + "' (known targets: " + known + ")";
    }
    return h;
}

EmitResult compile(const std::string& source, const BackendHandle& target, ModuleResolver* resolver, const LibConfig& lib) {
    EmitResult result;
    DiagnosticBag diags;

    if (!target.ok()) { // validated at resolve; refuse here so callers can't emit with a bad handle
        diags.error({}, target.error());
        result.diagnostics = diags.items();
        return result;
    }

    std::vector<Token> tokens = lex(source, diags);
    if (diags.hasErrors()) { result.diagnostics = diags.items(); return result; }

    CompilationUnit unit = parse(tokens, diags);
    if (diags.hasErrors()) { result.diagnostics = diags.items(); return result; }

    if (!runFrontEnd(unit, resolver, lib, diags, nullptr, nullptr, nullptr)) { result.diagnostics = diags.items(); return result; }

    // §3.E: refuse any used feature this target can't emit, before lowering/emitting (never miscompile).
    checkCapabilities(unit, *target.backend(), diags);
    if (diags.hasErrors()) { result.diagnostics = diags.items(); return result; }

    ir::Module module = lower(unit);
    result.code = target.backend()->emit(module);
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

AnalysisResult analyze(const std::string& source, ModuleResolver* resolver, const LibConfig& lib,
                       const std::string& entryPath) {
    AnalysisResult result;
    DiagnosticBag diags;

    int entryFileId = result.sources.add(entryPath.empty() ? "<entry>" : entryPath); // fileId 1 = the entry
    std::vector<Token> tokens = lex(source, diags, entryFileId);
    if (diags.hasErrors()) { result.diagnostics = diags.items(); return result; }

    CompilationUnit unit = parse(tokens, diags);
    if (diags.hasErrors()) { result.diagnostics = diags.items(); result.unit = std::move(unit); return result; }

    // The entry file's own decls occupy indices [0, count) in each vector; the front end APPENDS merged
    // std/import/lib decls after them, so this boundary tells the semantic model which symbols are file-local.
    SemanticRequest req;
    req.userFunctions  = unit.functions.size();
    req.userRecords    = unit.records.size();
    req.userClasses    = unit.classes.size();
    req.userInterfaces = unit.interfaces.size();
    req.userEnums      = unit.enums.size();
    req.userUnions     = unit.unions.size();
    req.userExtensions = unit.extensions.size();
    req.userValues     = unit.values.size();

    runFrontEnd(unit, resolver, lib, diags, &result.model, &req, &result.sources); // build model even on check errors
    result.diagnostics = diags.items();
    result.unit = std::move(unit);
    return result;
}

} // namespace mintplayer::polyglot
