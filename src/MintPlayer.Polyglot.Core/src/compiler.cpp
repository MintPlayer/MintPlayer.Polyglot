#include "mintplayer/polyglot/polyglot.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
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
  }
  init() {
  }
  let count: i32 {
  }
  fn add(item: T) {
  }
  fn clear() {
  }
  fn removeAll(pred: (T) => bool) {
  }
  fn removeAt(index: i32) {
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

expect fn readText(path: string): string

expect fn writeText(path: string, content: string)

expect fn appendText(path: string, content: string)

expect fn fileExists(path: string): bool

expect fn deleteFile(path: string)
)PG";

// std.math — the Math namespace as a native-backed `extern class` (preserving the `Math.sqrt(x)` / `Math.PI`
// surface). sqrt/ln/floor/ceil are f64; min/max/abs are generic and CALL-SITE-INLINED (a generic C#
// Math.Min over unconstrained T wouldn't compile, so the binding substitutes the concrete-typed args at
// each call). The TS min/max/abs use single-eval IIFE templates that work for both `number` and `bigint`
// (the `a - a` zero is type-agnostic). PI/E are bound consts.
// Determinism (§3.D): only sqrt is correctly-rounded/reproducible across targets. The TRANSCENDENTAL tier
// (sin/cos/tan/asin/acos/atan/atan2, sinh/cosh/tanh, exp/log/ln/log2/log10, pow) is a documented
// BEST-EFFORT tier — available on every target, but results may differ by <=1 ULP across the .NET JIT and a
// JS/Python/PHP runtime. Code that needs cross-target identity must use + - * / sqrt (or a future fixed-point
// std type), NOT these. Every member is a plain 1:1 bound static (no emulation); cbrt/sign are intentionally
// omitted (not uniformly available as a clean binding — cbrt is absent on PHP and Python<3.11; sign diverges,
// C# Math.Sign throws on NaN while JS returns NaN).
const char* STD_MATH = R"PG(
extern class Math {
  const PI: f64 {
  }
  const E: f64 {
  }
  static fn sqrt(x: f64): f64 {
  }
  static fn ln(x: f64): f64 {
  }
  static fn log(x: f64): f64 {
  }
  static fn log2(x: f64): f64 {
  }
  static fn log10(x: f64): f64 {
  }
  static fn exp(x: f64): f64 {
  }
  static fn pow(x: f64, y: f64): f64 {
  }
  static fn sin(x: f64): f64 {
  }
  static fn cos(x: f64): f64 {
  }
  static fn tan(x: f64): f64 {
  }
  static fn asin(x: f64): f64 {
  }
  static fn acos(x: f64): f64 {
  }
  static fn atan(x: f64): f64 {
  }
  static fn atan2(y: f64, x: f64): f64 {
  }
  static fn sinh(x: f64): f64 {
  }
  static fn cosh(x: f64): f64 {
  }
  static fn tanh(x: f64): f64 {
  }
  static fn trunc(x: f64): f64 {
  }
  static fn floor(x: f64): f64 {
  }
  static fn ceil(x: f64): f64 {
  }
  static fn min<T: INumber>(a: T, b: T): T {
  }
  static fn max<T: INumber>(a: T, b: T): T {
  }
  static fn abs<T: INumber>(x: T): T {
  }
  // round-to-nearest, type-preserving (f32 in -> f32 out, f64 -> f64). Generic covers both float widths, so
  // no overload is needed. C# `Math.Round` has only a `double` overload, so the result is cast back to `$T`
  // (the inferred return type: `float`/`double`); JS `Math.round` and Python `round` take a number directly.
  // Halfway cases are NOT bit-reproducible cross-target (C#/Python round-half-to-even, JS rounds half up) —
  // §3.D covers only + - * / sqrt — so callers relying on exact parity must avoid exact-.5 inputs.
  static fn round<T: INumber>(x: T): T {
  }
}
)PG";

// std.strings — string methods as bound extension methods (a method on the builtin `string`). Each arm maps
// `s.method(args)` to the target's idiom (C# `.ToUpper()` / JS `.toUpperCase()`). Both targets are UTF-16
// (SPEC §8) so `len`/`charAt` are code-unit operations; `codePoints` iterates code points (surrogate-aware).
const char* STD_STRINGS = R"PG(
extension fn string.isEmpty(): bool {
}
extension fn string.len(): i32 {
}
extension fn string.toUpper(): string {
}
extension fn string.toLower(): string {
}
extension fn string.charAt(index: i32): string {
}
extension fn string.codePoints(): Iterable<string> {
}
extension fn string.toI32(): i32 {
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
  }
  init(message: string) {
  }
  let message: string {
  }
}
extern class Iterable<T> {
  type {
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
     } }
extern class i16 { static fn parse(s: string): i16 {
     } }
extern class i32 { static fn parse(s: string): i32 {
     } }
extern class i64 { static fn parse(s: string): i64 {
     } }
extern class u8 { static fn parse(s: string): u8 {
     } }
extern class u16 { static fn parse(s: string): u16 {
     } }
extern class u32 { static fn parse(s: string): u32 {
     } }
extern class u64 { static fn parse(s: string): u64 {
     } }
extern class f32 { static fn parse(s: string): f32 {
     } }
extern class f64 { static fn parse(s: string): f64 {
     } }
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
// already processed by the caller). Everything lands in one unit for sema; module linking (§4.5) later
// partitions emission back out by `originModule`, stamped here: a user module's canonical path, or the
// "<prelude>" sentinel for embedded std/core/lib decls (which are never emitted as their own file).
void mergeDecls(CompilationUnit& mod, CompilationUnit& root, const std::string& origin) {
    for (auto& c : mod.classes)    { c.originModule = origin; root.classes.push_back(std::move(c)); }
    for (auto& r : mod.records)    { r.originModule = origin; root.records.push_back(std::move(r)); }
    for (auto& e : mod.enums)      { e.originModule = origin; root.enums.push_back(std::move(e)); }
    for (auto& u : mod.unions)     { u.originModule = origin; root.unions.push_back(std::move(u)); }
    for (auto& i : mod.interfaces) { i.originModule = origin; root.interfaces.push_back(std::move(i)); }
    for (auto& v : mod.values)     { v.originModule = origin; root.values.push_back(std::move(v)); }
    for (auto& f : mod.functions)  { f.originModule = origin; root.functions.push_back(std::move(f)); }
    for (auto& e : mod.extensions) { e.originModule = origin; root.extensions.push_back(std::move(e)); } // e.g. std.strings
}

// The origin sentinel for embedded std/core/lib decls — never emitted as their own module file (§4.5).
const char* kPreludeOrigin = "<prelude>";

// Module linking (§4.5): what each module imports from other USER modules, so emission can re-emit the
// cross-module reference (a target import statement) instead of the inlined copy. Keyed by the importer's
// canonical id ("" = the entry); std/lib specifiers are excluded (they're the inlined prelude). `fromCanon`
// is the imported module's canonical id (its emitted basename derives from it); `names` is the source's own
// `{ a, b as c }` group.
struct ResolvedImport { std::string fromCanon; std::vector<ImportName> names; bool isNamespace = false; std::string nsAlias; };
using ImportGraph = std::map<std::string, std::vector<ResolvedImport>>;

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
                 std::unordered_set<std::string>& visited, std::vector<std::string>& stack, SourceMap* src,
                 ImportGraph* graph) {
    stack.push_back(selfPath);
    for (const auto& imp : unit.imports) {
        std::string source, canon;
        bool isStd = imp.path.rfind("std.", 0) == 0;
        if (isStd) { // first-party std: embedded, no IO
            const char* stdSrc = nullptr;
            for (const auto& m : STD_MODULES) if (imp.path == m.path) stdSrc = m.source;
            if (!stdSrc) { diags.error(imp.pos, "unknown module '" + imp.path + "'"); continue; }
            source = stdSrc;
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
        // §4.5 linking: record every USER cross-module reference (even a re-import of an already-loaded
        // module) so the importer emits an import statement for it. std imports are the inlined prelude.
        if (!isStd && graph) (*graph)[selfPath].push_back({canon, imp.names, imp.isNamespace, imp.nsAlias});
        if (!visited.insert(canon).second) continue; // already loaded (imported more than once)

        int fileId = src ? src->add(canon) : 0; // stamp this module's tokens so its positions stay unambiguous
        std::vector<Token> toks = lex(source, diags, fileId);
        if (diags.hasErrors()) return;
        CompilationUnit mod = parse(toks, diags);
        if (diags.hasErrors()) return;

        validateImportNames(imp, mod, diags);
        loadImports(root, mod, canon, resolver, diags, visited, stack, src, graph); // dependencies first (post-order)
        mergeDecls(mod, root, isStd ? kPreludeOrigin : canon); // std -> inlined prelude; user module -> its canon
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
    for (auto& c : core.classes) if (!have.count(c.name)) { c.originModule = kPreludeOrigin; unit.classes.push_back(std::move(c)); }
    for (auto& u : core.unions)  if (!have.count(u.name)) { u.originModule = kPreludeOrigin; unit.unions.push_back(std::move(u)); }
}

// Resolve and merge the transitive closure of `unit`'s imports into it (see loadImports). `graph` (optional)
// collects the user cross-module import edges for §4.5 linking; nullptr = don't record (e.g. the lib prelude).
void linkModules(CompilationUnit& unit, ModuleResolver* resolver, DiagnosticBag& diags, SourceMap* src,
                 ImportGraph* graph = nullptr) {
    std::unordered_set<std::string> visited;
    std::vector<std::string> stack;
    loadImports(unit, unit, "", resolver, diags, visited, stack, src, graph);
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

    for (auto& d : staging.records)    if (!types.count(d.name)) { d.originModule = kPreludeOrigin; unit.records.push_back(std::move(d)); }
    for (auto& d : staging.classes)    if (!types.count(d.name)) { d.originModule = kPreludeOrigin; unit.classes.push_back(std::move(d)); }
    for (auto& d : staging.interfaces) if (!types.count(d.name)) { d.originModule = kPreludeOrigin; unit.interfaces.push_back(std::move(d)); }
    for (auto& d : staging.enums)      if (!types.count(d.name)) { d.originModule = kPreludeOrigin; unit.enums.push_back(std::move(d)); }
    for (auto& d : staging.unions)     if (!types.count(d.name)) { d.originModule = kPreludeOrigin; unit.unions.push_back(std::move(d)); }
    for (auto& v : staging.values)     if (!values.count(v.name)) { v.originModule = kPreludeOrigin; unit.values.push_back(std::move(v)); }
    for (auto& f : staging.functions)  if (!fns.count(fnSigKey(f.name, f.params))) { f.originModule = kPreludeOrigin; unit.functions.push_back(std::move(f)); }
    for (auto& e : staging.extensions) if (!exts.count(e.receiver.name + "." + e.name)) { e.originModule = kPreludeOrigin; unit.extensions.push_back(std::move(e)); }
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
                 SemanticModel* model, const SemanticRequest* req, SourceMap* src, ImportGraph* graph = nullptr) {
    linkCoreModule(unit, diags, src);                     if (diags.hasErrors()) return false; // Error/Iterable (no import)
    linkModules(unit, resolver, diags, src, graph);       if (diags.hasErrors()) return false; // imported std + user modules
    linkLibModules(unit, lib, resolver, diags, src);      if (diags.hasErrors()) return false; // ambient prelude (not recorded)
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

// Attach the ACTIVE target's std overlay arms (from its plugin) onto the linked skeleton decls (P19 slice
// 9b): binding members / `type` / `init` on extern classes, extension fns, and expect fns (a synthesized
// single-`extern` actual). Runs AFTER sema (templates are opaque — nothing to re-check) and BEFORE the
// capability gate, so a used member with no overlay entry refuses exactly like a missing `actual` arm.
// A decl that already carries an arm for this target keeps it (user/source arms win).
void injectStdOverlays(CompilationUnit& unit, const Backend& backend) {
    const auto& ov = backend.stdOverlays();
    if (ov.empty()) return;
    const std::string target = backend.name();
    auto find = [&](const std::string& key) -> const std::string* {
        auto it = ov.find(key);
        return it == ov.end() ? nullptr : &it->second;
    };
    auto hasArm = [&](const std::vector<TargetBinding>& bs) {
        for (const auto& b : bs)
            if (b.target == target) return true;
        return false;
    };
    for (auto& c : unit.classes) {
        if (!c.isExtern) continue;
        if (const std::string* t = find(c.name + ".type"); t && !hasArm(c.typeBindings))
            c.typeBindings.push_back({target, *t, c.pos});
        for (auto& mem : c.members) {
            const std::string key = c.name + "." + (mem.kind == MemberKind::Init ? "init" : mem.name);
            if (const std::string* t = find(key); t && !hasArm(mem.bindings))
                mem.bindings.push_back({target, *t, mem.pos});
        }
    }
    for (auto& e : unit.extensions)
        if (const std::string* t = find(e.receiver.name + "." + e.name); t && !hasArm(e.bindings))
            e.bindings.push_back({target, *t, e.pos});
    // Expect fns: synthesize the actual — the signature cloned, the body a single opaque extern (every
    // first-party actual was exactly that shape; richer bodies stay expressible as source arms).
    std::vector<FunctionDecl> synthesized;
    for (const auto& f : unit.functions) {
        if (!f.isExpect) continue;
        bool have = false;
        for (const auto& g : unit.functions)
            if (!g.isExpect && g.name == f.name && g.actualTarget == target) have = true;
        if (have) continue;
        const std::string* t = find(f.name);
        if (!t) continue;
        FunctionDecl a;
        a.name = f.name;
        a.mangledName = f.mangledName;
        a.pos = f.pos;
        a.namePos = f.namePos;
        a.generics = f.generics;
        for (const auto& p : f.params) { // Param owns its default expr — clone the declared shape only
            Param cp;                    // (no std expect declares a default; source arms cover that case)
            cp.name = p.name;
            cp.type = p.type;
            a.params.push_back(std::move(cp));
        }
        a.returnType = f.returnType;
        a.isAsync = f.isAsync;
        a.actualTarget = target;
        a.originModule = f.originModule; // §4.5: the synthesized actual inherits the expect fn's origin (prelude)
        auto ex = std::make_unique<Expr>();
        ex->kind = ExprKind::Extern;
        ex->pos = f.pos;
        ex->text = *t;
        auto st = std::make_unique<Stmt>();
        st->pos = f.pos;
        const bool returnsUnit = a.returnType.kind == TypeRef::Kind::Named && a.returnType.name == "unit";
        st->kind = returnsUnit ? StmtKind::ExprStmt : StmtKind::Return;
        st->value = std::move(ex);
        a.body.push_back(std::move(st));
        synthesized.push_back(std::move(a));
    }
    for (auto& f : synthesized) unit.functions.push_back(std::move(f));
}

// §4.5: the emitted basename for an imported user module — its canonical path's last path component with a
// trailing `.pg` stripped (pure string arithmetic; Core does no filesystem IO). v1 output is flat.
static std::string moduleBasename(const std::string& canon) {
    std::size_t slash = canon.find_last_of("/\\");
    std::string f = (slash == std::string::npos) ? canon : canon.substr(slash + 1);
    if (f.size() > 3 && f.compare(f.size() - 3, 3, ".pg") == 0) f.resize(f.size() - 3);
    return f;
}

// §4.5: the cross-module import list a given file emits. C# needs none (global-namespace types + `partial`
// wrappers in one assembly). Other targets get one entry per user import, keyed to the imported module's
// emitted basename; the selective group is pre-joined ("a, b as c") since TS and Python share that spelling.
static std::vector<ir::ModuleImport> buildImports(const ImportGraph& graph, const std::string& fileOrigin,
                                                  const std::map<std::string, std::string>& baseByCanon,
                                                  const std::string& target) {
    std::vector<ir::ModuleImport> out;
    if (target == "csharp") return out;
    auto it = graph.find(fileOrigin);
    if (it == graph.end()) return out;
    for (const auto& ri : it->second) {
        auto b = baseByCanon.find(ri.fromCanon);
        if (b == baseByCanon.end()) continue; // imported module contributed no emitted decls
        ir::ModuleImport mi;
        mi.path = b->second;
        if (ri.isNamespace) { mi.isNamespace = true; mi.ns = ri.nsAlias; }
        else {
            std::string joined;
            for (const auto& n : ri.names) {
                if (!joined.empty()) joined += ", ";
                joined += n.alias.empty() ? n.name : (n.name + " as " + n.alias);
            }
            mi.names = joined;
        }
        out.push_back(std::move(mi));
    }
    return out;
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

    ImportGraph importGraph; // §4.5: user cross-module import edges, keyed by importer ("" = entry)
    if (!runFrontEnd(unit, resolver, lib, diags, nullptr, nullptr, nullptr, &importGraph)) { result.diagnostics = diags.items(); return result; }

    injectStdOverlays(unit, *target.backend()); // the plugin's std arms land on the skeletons (P19 s9b)

    // §3.E: refuse any used feature this target can't emit, before lowering/emitting (never miscompile).
    checkCapabilities(unit, *target.backend(), diags);
    // P19 §7: user identifiers colliding with the target's generated names/globals (or pgconfig-forbidden
    // names) refuse here — the silent-collision miscompiles, made loud.
    checkReservedNames(unit, *target.backend(), lib.forbiddenIdentifiers, diags);
    if (diags.hasErrors()) { result.diagnostics = diags.items(); return result; }

    // §4.5 module linking: distinct imported-user-module origins present in the linked unit (non-entry,
    // non-prelude). Empty => a single-file program: take the exact historical path (byte-identical output).
    std::set<std::string> userOrigins;
    auto note = [&](const std::string& o) { if (!o.empty() && o != kPreludeOrigin) userOrigins.insert(o); };
    for (const auto& d : unit.enums)      note(d.originModule);
    for (const auto& d : unit.unions)     note(d.originModule);
    for (const auto& d : unit.records)    note(d.originModule);
    for (const auto& d : unit.classes)    note(d.originModule);
    for (const auto& d : unit.interfaces) note(d.originModule);
    for (const auto& d : unit.values)     note(d.originModule);
    for (const auto& d : unit.functions)  note(d.originModule);
    for (const auto& d : unit.extensions) note(d.originModule);

    if (userOrigins.empty()) { // single-file: unchanged
        ir::Module module = lower(unit, target.name());
        result.code = target.backend()->emit(module);
        result.ok = true;
        return result;
    }

    // Multi-module: emit one file per user module + the entry, each re-lowered fresh (lowering is pure; the
    // Lowerer needs the full type universe, so we lower the whole unit and then keep only this file's decls).
    // Basenames must be unique across the closure (v1 emits flat — see PRD §3.B).
    std::map<std::string, std::string> baseByCanon; // canon -> emitted basename
    std::set<std::string> seenBase;
    for (const auto& canon : userOrigins) {
        std::string base = moduleBasename(canon);
        if (!seenBase.insert(base).second) {
            diags.error({}, "module linking: two imported modules share the basename '" + base +
                                "' (v1 emits flat output; give them distinct file names)");
            result.diagnostics = diags.items();
            return result;
        }
        baseByCanon[canon] = base;
    }

    const bool preludeEverywhere = target.name() != "csharp"; // C#: prelude only in the entry (partial + one assembly)
    auto emitFile = [&](const std::string& fileOrigin) -> std::string {
        std::set<std::string> keep;
        keep.insert(fileOrigin);
        if (fileOrigin.empty() || preludeEverywhere) keep.insert(kPreludeOrigin);
        ir::Module m = lower(unit, target.name());
        auto prune = [&](auto& vec) {
            vec.erase(std::remove_if(vec.begin(), vec.end(),
                                     [&](const auto& d) { return !keep.count(d.originModule); }),
                      vec.end());
        };
        prune(m.enums); prune(m.unions); prune(m.records); prune(m.classes);
        prune(m.interfaces); prune(m.globals); prune(m.extensions); prune(m.functions);
        m.linked = true;
        m.imports = buildImports(importGraph, fileOrigin, baseByCanon, target.name());
        return target.backend()->emit(m);
    };

    result.code = emitFile(""); // the entry file (the CLI names it after the input)
    for (const auto& canon : userOrigins)
        result.modules.push_back({baseByCanon[canon], emitFile(canon)});
    result.ok = true;
    return result;
}

std::vector<std::string> importSpecifiers(const std::string& source) {
    std::vector<std::string> specs;
    DiagnosticBag diags;
    std::vector<Token> tokens = lex(source, diags);
    if (diags.hasErrors()) return specs;
    CompilationUnit unit = parse(tokens, diags);
    if (diags.hasErrors()) return specs;
    for (const auto& imp : unit.imports)
        if (imp.path.rfind("std.", 0) != 0) specs.push_back(imp.path);
    return specs;
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
