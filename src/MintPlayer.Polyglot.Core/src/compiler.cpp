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
#include "mintplayer/polyglot/reachability.hpp"
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
  constructor() {
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
// std type), NOT these. Most members are plain 1:1 bound statics (no emulation). `sign` and `clamp` are the
// exception (P28): they are NOT 1:1 to any native primitive — `sign` is emitted from a type-preserving
// operator template (x>0 ? 1 : x<0 ? -1 : x-x) precisely because the natives diverge (C# Math.Sign throws on
// NaN, JS returns NaN, Python/PHP have none), and `clamp` from min(max(..)) — so both live in the reproducible
// + - * / sqrt tier (only subtraction + comparison), integer-in/integer-out. `cbrt` stays omitted (absent on
// PHP and Python<3.11 — a faithful add would need §3.C-documented emulation).
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
  static fn asinh(x: f64): f64 {
  }
  static fn acosh(x: f64): f64 {
  }
  static fn atanh(x: f64): f64 {
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
  // signum, type-preserving (i32 in -> i32 out, f64 -> f64). Emitted from an operator template, NOT each
  // target's native `sign` (which returns int and/or diverges on NaN) — so it stays in the reproducible tier
  // and NaN -> NaN (via the x-x identity) on every target. See §3.D.
  static fn sign<T: INumber>(x: T): T {
  }
  // clamp x into [lo, hi], type-preserving. min(max(x, lo), hi); reproducible (comparison only). Assumes
  // lo <= hi (caller's contract — no runtime check, following C#'s Math.Clamp which throws only on lo > hi).
  static fn clamp<T: INumber>(x: T, lo: T, hi: T): T {
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
// `codePointAt` yields the code point starting at the index — the portable char→ordinal path. Surrogate-aware
// on C#/TS; Python indexes by code point (charAt's existing caveat); PHP stays byte-oriented like its charAt
// (`ord(substr(...))` — mb_ord would demand the mbstring extension), so cross-target parity is exact on ASCII.
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
extension fn string.codePointAt(index: i32): i32 {
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
  constructor(message: string) {
  }
  let message: string {
  }
}
extern class Iterable<T> {
  type {
  }
}
// The fixed-size array `T[]` (surface sugar parses to `Array<T>`). Always linked, like `Iterable` — `[]` is
// core syntax, not a std import. It deliberately declares NO add/remove/clear: an array is fixed-size (maps
// to a C# `T[]`), so mutating its size is a type error. Element get/set (`a[i]`), `count`, and `for x in a`
// are compiler-handled (same as List); construction and the element-type spelling come from the bindings.
extern class Array<T> {
  type {
  }
  constructor() {
  }
  let count: i32 {
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
constexpr const char* kPreludeOrigin = ir::kPreludeOrigin;

// The reserved basename of the shared C# prelude file (issue #14): when a multi-file C# project build hoists
// the prelude out, all roots emit this one file (identical content -> the CLI's writeDedup collapses it).
const char* kPreludeBasename = "__polyglot_prelude";

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
        // #39e (⚑ PRD §2): a renaming (`{ a as b }`) or namespace (`* as ns`) import used to parse but then
        // resolve to nothing (a silent "undeclared name"). Modules merge into one flat namespace, which has
        // no place for an importer-local alias or a synthesized namespace object, so both forms are refused
        // out loud (§3.B) instead of miscompiling. A plain `import { a, b } from "…"` is unaffected.
        if (imp.isNamespace)
            diags.error(imp.pos, "Polyglot refuses a namespace import ('import * as " + imp.nsAlias +
                                     "') — import the members by name (`import { a, b } from \"" + imp.path +
                                     "\"`) instead (PRD §3.B)");
        for (const auto& n : imp.names)
            if (!n.alias.empty())
                diags.error(imp.pos, "Polyglot refuses a renaming import ('" + n.name + " as " + n.alias +
                                         "') — import '" + n.name + "' under its own name (PRD §3.B)");
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
    auto find = [&](const std::string& key) -> const StdBinding* {
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
        if (const StdBinding* t = find(c.name + ".type"); t && !hasArm(c.typeBindings))
            c.typeBindings.push_back({target, t->tmpl, c.pos, t->preludeKeys});
        for (auto& mem : c.members) {
            const std::string key = c.name + "." + (mem.kind == MemberKind::Constructor ? "constructor" : mem.name);
            if (const StdBinding* t = find(key); t && !hasArm(mem.bindings))
                mem.bindings.push_back({target, t->tmpl, mem.pos, t->preludeKeys});
        }
    }
    for (auto& e : unit.extensions)
        if (const StdBinding* t = find(e.receiver.name + "." + e.name); t && !hasArm(e.bindings))
            e.bindings.push_back({target, t->tmpl, e.pos, t->preludeKeys});
    // Expect fns: synthesize the actual — the signature cloned, the body a single opaque extern (every
    // first-party actual was exactly that shape; richer bodies stay expressible as source arms).
    std::vector<FunctionDecl> synthesized;
    for (const auto& f : unit.functions) {
        if (!f.isExpect) continue;
        bool have = false;
        for (const auto& g : unit.functions)
            if (!g.isExpect && g.name == f.name && g.actualTarget == target) have = true;
        if (have) continue;
        const StdBinding* t = find(f.name);
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
        ex->text = t->tmpl;
        ex->preludeKeys = t->preludeKeys; // P40 §0: the actual's helper requirements ride on its extern
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

// P30 slice 8: the relative import specifier from one emitted file's directory to another's — pure
// lexical arithmetic over the host-provided (already normalized, '/'-separated) dir keys; Core does
// no filesystem IO. Same dir → "./<base>" (the historical flat shape, byte-identical); a specifier
// that doesn't climb gets a "./" prefix (a bare "sub/mod" would read as a package import in ESM).
static std::string relativeSpecifier(const std::string& fromDir, const std::string& toDir,
                                     const std::string& base) {
    if (fromDir == toDir) return "./" + base;
    auto split = [](const std::string& s) {
        std::vector<std::string> v;
        std::size_t b = 0;
        while (b <= s.size()) {
            std::size_t e = s.find('/', b);
            if (e == std::string::npos) e = s.size();
            if (e > b) v.push_back(s.substr(b, e - b));
            b = e + 1;
        }
        return v;
    };
    const auto f = split(fromDir), t = split(toDir);
    std::size_t i = 0;
    while (i < f.size() && i < t.size() && f[i] == t[i]) ++i;
    std::string rel;
    for (std::size_t k = i; k < f.size(); ++k) rel += "../";
    for (std::size_t k = i; k < t.size(); ++k) rel += t[k] + "/";
    if (rel.rfind("../", 0) != 0) rel = "./" + rel;
    return rel + base;
}

// §4.5: the cross-module import list a given file emits. C# needs none (global-namespace types + `partial`
// wrappers in one assembly). Other targets get one entry per user import, keyed to the imported module's
// emitted basename; the selective group is pre-joined ("a, b as c") since TS and Python share that spelling.
// For a `crossDirImports` target (P30 slice 8) `mi.path` is the FULL relative specifier, computed from the
// host's routed output dirs (no router = flat = "./<base>"); otherwise it stays the bare basename.
static std::vector<ir::ModuleImport> buildImports(const ImportGraph& graph, const std::string& fileOrigin,
                                                  const std::map<std::string, std::string>& baseByCanon,
                                                  bool linksWithoutImports, bool crossDir,
                                                  const std::function<std::string(const std::string&)>& outDirOf) {
    std::vector<ir::ModuleImport> out;
    if (linksWithoutImports) return out; // modules link by compilation into one unit (plugin trait flag)
    auto it = graph.find(fileOrigin);
    if (it == graph.end()) return out;
    const std::string fromDir = (crossDir && outDirOf) ? outDirOf(fileOrigin) : std::string();
    for (const auto& ri : it->second) {
        auto b = baseByCanon.find(ri.fromCanon);
        if (b == baseByCanon.end()) continue; // imported module contributed no emitted decls
        ir::ModuleImport mi;
        mi.path = crossDir ? relativeSpecifier(fromDir, outDirOf ? outDirOf(ri.fromCanon) : std::string(),
                                               b->second)
                           : b->second;
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

// The one compile pipeline. `sharedOnly` (P40 §0, the shared prelude's phase 2) emits ONLY the prelude,
// pruned to the union reach handed in; `source` then just links the std modules that union came from.
static EmitResult compileImpl(const std::string& source, const BackendHandle& target, ModuleResolver* resolver,
                              const LibConfig& lib, const std::string& entryPath, const PreludeReach* sharedOnly) {
    EmitResult result;
    DiagnosticBag diags;

    if (!target.ok()) { // validated at resolve; refuse here so callers can't emit with a bad handle
        diags.error({}, target.error());
        result.diagnostics = diags.items();
        return result;
    }

    // P38/issue #69: stamp real fileIds on the BUILD path too. Before this, compile() lexed with no fileId
    // and handed runFrontEnd a null SourceMap, so every token of every module in the closure was fileId 0 —
    // origin info was unavailable to the emitter, and every diagnostic was misattributed to the entry file.
    // The entry keeps id 1 (analyze() establishes that invariant and the LSP's `fileId != 1` filter relies
    // on it). The path is stored verbatim; canonicalization is the caller's job (the Core does no IO).
    const int entryFileId = result.sources.add(entryPath.empty() ? "<entry>" : entryPath);

    std::vector<Token> tokens = lex(source, diags, entryFileId);
    if (diags.hasErrors()) { result.diagnostics = diags.items(); return result; }

    CompilationUnit unit = parse(tokens, diags);
    if (diags.hasErrors()) { result.diagnostics = diags.items(); return result; }

    ImportGraph importGraph; // §4.5: user cross-module import edges, keyed by importer ("" = entry)
    if (!runFrontEnd(unit, resolver, lib, diags, nullptr, nullptr, &result.sources, &importGraph)) { result.diagnostics = diags.items(); return result; }

    injectStdOverlays(unit, *target.backend()); // the plugin's std arms land on the skeletons (P19 s9b)

    // §3.E: refuse any used feature this target can't emit, before lowering/emitting (never miscompile).
    checkCapabilities(unit, *target.backend(), diags);

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

    // P40 B1: resolve every emitted file's target options against the plugin's declared `options` (unknown /
    // pattern-mismatch refuse), then hold closure-scoped options to one value across the closure — files that
    // reference each other must agree on, e.g., the namespace their declarations live in, or they cannot link.
    std::map<std::string, std::map<std::string, std::string>> fileOptions; // origin ("" = entry) -> resolved
    {
        std::set<std::string> reported;
        auto resolveFor = [&](const std::string& origin) {
            std::map<std::string, std::string> given;
            if (lib.targetOptions) given = lib.targetOptions(target.name(), origin);
            std::string err;
            if (!resolveTargetOptions(*target.backend(), given, fileOptions[origin], err) && reported.insert(err).second)
                diags.error({}, err);
        };
        resolveFor("");
        for (const auto& o : userOrigins) resolveFor(o);
        if (!diags.hasErrors()) {
            const std::string entryName = entryPath.empty() ? std::string("<entry>") : entryPath;
            for (const auto& d : target.backend()->options()) {
                if (!d.closureScope) continue;
                const std::string& want = fileOptions[""][d.name];
                for (const auto& o : userOrigins) {
                    const std::string& got = fileOptions[o][d.name];
                    if (got == want) continue;
                    diags.error({}, "target option '" + target.name() + "." + d.name +
                                        "' must agree across one import closure (scope \"closure\"): '" + entryName +
                                        "' resolves '" + want + "' but its import '" + o + "' resolves '" + got + "'");
                    break;
                }
            }
        }
        if (diags.hasErrors()) { result.diagnostics = diags.items(); return result; }
        result.targetOptions = fileOptions[""];
    }
    // A `reserve` option's resolved value is a name the generated code claims (the C# wrapper class).
    std::vector<std::pair<std::string, std::string>> optionReserved;
    for (const auto& d : target.backend()->options()) {
        if (!d.reserve) continue;
        std::set<std::string> vals;
        for (const auto& [origin, opts] : fileOptions) {
            auto it = opts.find(d.name);
            if (it != opts.end() && !it->second.empty() && vals.insert(it->second).second)
                optionReserved.push_back({d.name, it->second});
        }
    }
    auto optionsOf = [&](const std::string& origin) -> const std::map<std::string, std::string>& {
        auto it = fileOptions.find(origin);
        return it != fileOptions.end() ? it->second : fileOptions[""];
    };

    // P19 §7: user identifiers colliding with the target's generated names/globals (or pgconfig-forbidden
    // names) refuse here — the silent-collision miscompiles, made loud.
    checkReservedNames(unit, *target.backend(), lib.forbiddenIdentifiers, diags, &optionReserved);
    if (diags.hasErrors()) { result.diagnostics = diags.items(); return result; }

    // §4.5 / issue #14: for a multi-file PROJECT build, a target may hoist the shared prelude
    // (Option/Some/None + the program wrapper) into one reserved `__polyglot_prelude` file instead of
    // inlining it, so N independent link roots don't each emit it (C#: CS0101/CS8863 in one assembly).
    // P39: manifest data (`sharedPreludeFile`), not a target-name comparison — the Core never asks
    // "is this C#?".
    const bool splitPrelude = !sharedOnly && target.backend()->sharedPreludeFile() && lib.sharedPrelude;

    // P40 A2 backstop: an expression lowering had no rule for is a compiler bug — report it and write nothing,
    // instead of shipping the `__polyglot_unlowered_expr__` placeholder (a runtime-only failure on TS/Python/PHP).
    auto loweringFailed = [&](const ir::Module& m) {
        for (const auto& f : m.loweringFailures)
            diags.error(f.pos, "internal compiler error: no lowering for " + f.kind +
                                   " expression — this is a Polyglot bug, please report it");
        return !m.loweringFailures.empty();
    };

    if (userOrigins.empty() && !splitPrelude && !sharedOnly) { // single-file
        ir::Module module = lower(unit, target.name());
        if (loweringFailed(module)) { result.diagnostics = diags.items(); return result; }
        pruneUnreachablePrelude(module); // P40 §0: only the prelude this program reaches
        module.options = optionsOf(""); // P40 B1: rules read `option.<name>` (defaults = byte-identical)
        module.originInfo = lib.originInfo;      // P38: off => byte-identical
        module.sourceFiles = result.sources.files;
        if (lib.originInfo && lib.originBaseDir) module.originBaseDir = lib.originBaseDir(""); // P40 B7
        if (lib.originInfo) {
            // P38: collect per-line origins alongside the code. The host turns them into a sidecar — the
            // Core never learns where a file is routed, so it cannot compute map-relative paths itself.
            std::vector<std::array<int, 3>> origins;
            result.code = target.backend()->emitWithOrigins(module, origins);
            for (const auto& o : origins) result.origins.push_back(OriginRecord{o[0], o[1], o[2]});
        } else {
            result.code = target.backend()->emit(module);
        }
        result.diagnostics = diags.items(); // warnings reach the host on success too (P40 A1)
        result.ok = true;
        return result;
    }

    // Partitioned emit: one file per user module + the entry (+ a shared prelude file for C# project builds),
    // each re-lowered fresh (lowering is pure; the Lowerer needs the full type universe, so we lower the whole
    // unit and then keep only this file's decls). Basenames must be unique across the closure (flat output).
    std::map<std::string, std::string> baseByCanon; // canon -> emitted basename
    std::set<std::string> seenBase;
    for (const auto& canon : userOrigins) {
        std::string base = moduleBasename(canon);
        if (base == kPreludeBasename) {
            diags.error({}, std::string("module '") + canon + "' uses the reserved basename '" + kPreludeBasename + "'");
            result.diagnostics = diags.items();
            return result;
        }
        if (!seenBase.insert(base).second) {
            diags.error({}, "module linking: two imported modules share the basename '" + base +
                                "' (v1 emits flat output; give them distinct file names)");
            result.diagnostics = diags.items();
            return result;
        }
        baseByCanon[canon] = base;
    }

    // P39: manifest data (`preludePerFile`). Targets whose modules don't share a link root (TS/Python/PHP)
    // need the prelude in every emitted file; C# keeps it in the entry, or in its own file when split.
    // This is load-bearing for origins, not cosmetic: prelude prepending shifts every recorded origin
    // line, and Python — the only target declaring preludes — gains a source-map sink in this milestone.
    const bool preludeEverywhere = target.backend()->preludePerFile();
    // Emit one file: re-lower the unit, keep only `keep`-origin decls, set link/access, resolve imports.
    // P38: the origin list produced by the most recent emitKeep() call, stashed onto the matching output
    // below. Each emitted file has its own line numbering, so origins cannot be accumulated across them.
    std::vector<OriginRecord> lastOrigins;
    bool anyLoweringFailed = false; // P40 A2: set by emitKeep; the whole result is then withheld
    auto withheld = [&]() {
        result.code.clear();
        result.modules.clear();
        result.origins.clear();
        result.diagnostics = diags.items();
    };
    // P40 §0: `forcedReach` names prelude declarations reached ELSEWHERE (the shared prelude's union across
    // roots); null = compute it here.
    auto emitKeep = [&](const std::set<std::string>& keep, const std::string& importOrigin,
                        const std::set<std::string>* forcedReach = nullptr) -> std::string {
        ir::Module m = lower(unit, target.name());
        // Every file re-lowers the whole unit, so a failure repeats identically: report it once.
        if (!anyLoweringFailed && loweringFailed(m)) anyLoweringFailed = true;
        // P40 §0: a file that holds the prelude on behalf of OTHER files too (the C# entry, or the split
        // prelude file) keeps the union of what every file of the closure reaches — computed over the whole
        // module, before it is partitioned. A per-file-prelude target keeps only what THIS file reaches.
        const bool holdsPrelude = keep.count(kPreludeOrigin) != 0;
        const bool holdsForOthers = holdsPrelude && !preludeEverywhere;
        std::set<std::string> closureReach;
        if (holdsForOthers && !forcedReach) {
            closureReach = preludeReach(m);
            forcedReach = &closureReach;
        }
        auto prune = [&](auto& vec) {
            vec.erase(std::remove_if(vec.begin(), vec.end(),
                                     [&](const auto& d) { return !keep.count(d.originModule); }),
                      vec.end());
        };
        prune(m.enums); prune(m.unions); prune(m.records); prune(m.classes);
        prune(m.interfaces); prune(m.globals); prune(m.extensions); prune(m.functions);
        if (holdsPrelude) pruneUnreachablePrelude(m, holdsForOthers ? forcedReach : nullptr);
        m.linked = true;
        m.options = optionsOf(importOrigin); // the prelude file (importOrigin = its basename) takes the entry's
        m.originInfo = lib.originInfo;      // P38: off => byte-identical
        m.sourceFiles = result.sources.files;
        // P40 B7: the entry and the prelude file (which follows the entry's directory) route as origin "".
        if (lib.originInfo && lib.originBaseDir)
            m.originBaseDir = lib.originBaseDir(importOrigin == kPreludeBasename ? std::string() : importOrigin);
        m.imports = buildImports(importGraph, importOrigin, baseByCanon,
                                 target.backend()->linksWithoutImports(),
                                 target.backend()->crossDirImports(), lib.moduleOutputDir);
        if (!lib.originInfo) return target.backend()->emit(m);
        // P38: same as the single-file path, but each emitted FILE gets its own origin list — the caller
        // below stashes it on the matching ModuleFile.
        std::vector<std::array<int, 3>> origins;
        std::string code = target.backend()->emitWithOrigins(m, origins);
        lastOrigins.clear();
        for (const auto& o : origins) lastOrigins.push_back(OriginRecord{o[0], o[1], o[2]});
        return code;
    };
    auto emitFile = [&](const std::string& fileOrigin) -> std::string {
        std::set<std::string> keep{fileOrigin};
        // The prelude folds into the entry inline UNLESS it's hoisted to its own file (splitPrelude); non-C#
        // targets inline it into every file (preludeEverywhere), each file being its own module scope.
        if ((fileOrigin.empty() && !splitPrelude) || preludeEverywhere) keep.insert(kPreludeOrigin);
        return emitKeep(keep, fileOrigin);
    };

    if (sharedOnly) { // P40 §0 phase 2: the shared prelude alone, pruned to the union across roots
        const std::set<std::string> unionReach(sharedOnly->names.begin(), sharedOnly->names.end());
        result.code = emitKeep({kPreludeOrigin}, kPreludeBasename, &unionReach);
        result.origins = lastOrigins;
        if (anyLoweringFailed) { withheld(); return result; }
        result.diagnostics = diags.items(); // warnings reach the host on success too (P40 A1)
        result.ok = true;
        return result;
    }

    result.code = emitFile(""); // the entry file (the CLI names it after the input)
    result.origins = lastOrigins; // P38
    for (const auto& canon : userOrigins) {
        std::string code = emitFile(canon);
        result.modules.push_back({baseByCanon[canon], std::move(code), canon, lastOrigins});
    }
    if (splitPrelude) {
        // P40 §0 / S15: report what this root reaches, and which std modules it links, so a host compiling
        // several roots can build ONE prelude from their union (compileSharedPrelude). The file emitted here
        // is this root's own share — correct on its own, and empty when the root reaches no prelude at all
        // (then no file should exist). The prelude is synthesized runtime text with no `.pg` origin — its
        // lines are hidden, so it stays root-independent (PRD §4.F / SP2).
        const std::set<std::string> reach = preludeReach(lower(unit, target.name()));
        result.preludeReach.names.assign(reach.begin(), reach.end());
        for (std::size_t fid = 1; fid < result.sources.files.size(); ++fid) {
            const std::string& f = result.sources.files[fid];
            if (f.rfind("std.", 0) == 0 && f != kCoreModuleName) result.preludeReach.stdModules.push_back(f);
        }
        std::sort(result.preludeReach.stdModules.begin(), result.preludeReach.stdModules.end());
        result.preludeReach.stdModules.erase(
            std::unique(result.preludeReach.stdModules.begin(), result.preludeReach.stdModules.end()),
            result.preludeReach.stdModules.end());
        std::string code = reach.empty() ? std::string() : emitKeep({kPreludeOrigin}, kPreludeBasename, &reach);
        ModuleFile pf{kPreludeBasename, std::move(code), "", {}};
        pf.sharedPrelude = true;
        result.modules.push_back(std::move(pf));
    }
    if (anyLoweringFailed) { withheld(); return result; }
    result.diagnostics = diags.items(); // warnings reach the host on success too (P40 A1)
    result.ok = true;
    return result;
}

EmitResult compile(const std::string& source, const BackendHandle& target, ModuleResolver* resolver, const LibConfig& lib,
                   const std::string& entryPath) {
    return compileImpl(source, target, resolver, lib, entryPath, nullptr);
}

EmitResult compileSharedPrelude(const BackendHandle& target, const PreludeReach& unionReach, const LibConfig& lib) {
    if (unionReach.names.empty()) { // nothing reached by any root: no prelude file at all
        EmitResult r;
        r.ok = true;
        return r;
    }
    // The std modules the roots linked, re-linked by a source that declares nothing of its own; the core
    // prelude (Option, Error, …) is always linked, and `lib` brings the same ambient modules as the roots.
    std::string synth;
    for (const auto& m : unionReach.stdModules) synth += "import \"" + m + "\"\n";
    LibConfig l = lib;
    l.sharedPrelude = false;
    return compileImpl(synth, target, nullptr, l, std::string(), &unionReach);
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
