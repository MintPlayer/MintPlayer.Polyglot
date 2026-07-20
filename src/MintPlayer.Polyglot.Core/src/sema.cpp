#include "mintplayer/polyglot/sema.hpp"

#include <cerrno>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mintplayer::polyglot {

namespace {

// Semantic types are reused TypeRefs. An empty Named ("") is the "unconstrained / unknown" type — the
// checker stays lenient on it (and on generic params and std types we don't model yet) to avoid false
// errors, and only reports a mismatch/missing-member when both sides are fully known. The scalar rules
// are unchanged: they run on scalarTyOf() of the operand types, so conformance behaviour is preserved.

TypeRef tUnknown() { return TypeRef{}; }
TypeRef tNamed(std::string n) { return namedType(std::move(n)); }
TypeRef tNullableUnknown() { TypeRef t; t.nullable = true; return t; }

// A type every component of which is concrete (no empty-name / unconstrained hole anywhere). Used to
// decide whether a declaration's initializer conveyed enough to reconstruct the declared type on every
// target. A nullable-with-no-underlying-type (bare `null`) is NOT fully known.
bool typeFullyKnown(const TypeRef& t) {
    if (t.kind == TypeRef::Kind::Named && t.name.empty()) return false;
    for (const auto& a : t.args) if (!typeFullyKnown(a)) return false;
    for (const auto& r : t.ret)  if (!typeFullyKnown(r)) return false;
    return true;
}

// Does an *un-annotated* binding's initializer leave the type un-inferable — i.e. would the missing type
// reach a backend as a placeholder (C# `object` / TS evolving-`any`) rather than the author's intent?
// (issue #27 root cause). Precisely the forms that lose type information to emission — an empty/unknown-
// element list literal (`[]`), a bare `null`, and a lambda with un-annotated parameters. Deliberately NOT
// a bare unconstrained `tUnknown()` from an unmodeled generic/std call: the checker stays lenient there
// (see the note above `tUnknown`), so flagging it would be a false positive.
bool uninferableInit(const Expr& value, const TypeRef& inferred) {
    if (value.kind == ExprKind::Lambda) {           // a lambda's type is reconstructible iff every param is typed
        for (const auto& p : value.params) if (p.type.absent()) return true;
        return false;
    }
    if (inferred.kind == TypeRef::Kind::Named && inferred.name == "List" &&
        (inferred.args.empty() || !typeFullyKnown(inferred.args[0]))) return true;   // `[]` -> List<unknown>
    if (inferred.nullable && inferred.name.empty() && inferred.args.empty()) return true; // bare `null`
    return false;
}

// The fix-it example for the "add an explicit type" diagnostic, tailored to which un-inferable form it is.
std::string annotationHint(const std::string& name, const Expr& value) {
    if (value.kind == ExprKind::Lambda)
        return "annotate the lambda's parameter types, e.g. '(x: i32) => …'";
    if (value.kind == ExprKind::NullLit)
        return "add an explicit type annotation, e.g. 'var " + name + ": T? = null'";
    return "add an explicit type annotation, e.g. 'var " + name + ": List<i32> = []'";
}

bool isNumeric(Ty t) { return t == Ty::I32 || t == Ty::F64; }

// The MVP scalar lattice (Ty) only tracks i32/f64; the other numeric widths (i64/u64/i8/i16/u8/u16/u32/
// f32) are recognized by name so binary arithmetic keeps them typed — the backends need the exact width
// to lower faithfully (i64 -> BigInt, etc.).
bool isNumericTypeName(const TypeRef& t) {
    if (t.kind != TypeRef::Kind::Named || t.nullable || !t.args.empty()) return false;
    const std::string& n = t.name;
    return n == "i8" || n == "i16" || n == "i32" || n == "i64" || n == "u8" || n == "u16" ||
           n == "u32" || n == "u64" || n == "f32" || n == "f64";
}
bool sameNamedType(const TypeRef& a, const TypeRef& b) {
    return a.kind == TypeRef::Kind::Named && b.kind == TypeRef::Kind::Named &&
           a.name == b.name && !a.nullable && !b.nullable;
}

int numWidth(const std::string& n) {
    if (n == "i8" || n == "u8") return 8;
    if (n == "i16" || n == "u16") return 16;
    if (n == "i32" || n == "u32") return 32;
    if (n == "i64" || n == "u64") return 64;
    return 0;
}
bool isSignedInt(const std::string& n)   { return n == "i8" || n == "i16" || n == "i32" || n == "i64"; }
bool isUnsignedInt(const std::string& n) { return n == "u8" || n == "u16" || n == "u32" || n == "u64"; }
bool isFloatName(const std::string& n)   { return n == "f32" || n == "f64"; }

// Does an (unsuffixed) integer-literal text hold a value representable in i64/u64? Used by literal
// adoption in checkConvert (G3, wave 2). Base 0: decimal and 0x hex alike. The one edge left out is
// the exact INT64_MIN spelling (its magnitude overflows the positive parse) — that falls back to the
// widening-cast path.
bool literalFits64(const std::string& text, bool isUnsigned) {
    errno = 0;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(text.c_str(), &end, 0);
    if (errno == ERANGE || end == text.c_str() || *end != '\0') return false;
    return isUnsigned || v <= 9223372036854775807ull;
}

// Is `from -> to` an implicit, value-preserving widening (no precision loss, no sign surprise)? Everything
// else (narrowing, i64->f64, signed->unsigned) requires an explicit cast. (SPEC §3.)
bool isLosslessWiden(const TypeRef& from, const TypeRef& to) {
    if (!isNumericTypeName(from) || !isNumericTypeName(to) || from.name == to.name) return false;
    const std::string& f = from.name;
    const std::string& t = to.name;
    if (isFloatName(t)) // ints up to 32 bits (and f32) fit exactly in an f64 mantissa; ->f32 is lossy
        return t == "f64" && (f == "f32" || ((isSignedInt(f) || isUnsignedInt(f)) && numWidth(f) <= 32));
    if (isFloatName(f)) return false; // float -> int is never implicit
    if (isSignedInt(f) && isSignedInt(t))     return numWidth(t) > numWidth(f);
    if (isUnsignedInt(f) && isUnsignedInt(t)) return numWidth(t) > numWidth(f);
    if (isUnsignedInt(f) && isSignedInt(t))   return numWidth(t) > numWidth(f); // unsigned fits in wider signed
    return false; // signed -> unsigned never implicit
}

bool isBuiltinType(const std::string& n) {
    static const std::unordered_set<std::string> b = {
        "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64",
        "f32", "f64", "bool", "char", "string", "unit",
        // (`Error` and `Iterable` are NOT builtins — they are `extern class`es in the always-linked core
        //  prelude (compiler.cpp STD_CORE), so they resolve like any declared type.)
    };
    return b.count(n) != 0;
}

// An integer literal's type comes from its width suffix (`100u8`, `0i64`); a bare literal is i32.
std::string intLitType(const std::string& text) {
    static const char* suffixes[] = {"i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64"};
    for (const char* s : suffixes) {
        std::size_t n = std::char_traits<char>::length(s);
        if (text.size() > n && text.compare(text.size() - n, n, s) == 0) return s;
    }
    return "i32";
}

// A float literal's type comes from its suffix: `f`/`f32` => f32, `f64`/`d` => f64; a bare literal
// (`1.5`, `1e3`) is f64. Mirrors C#'s `f`/`d` shorthands so `310f` is single-precision as written.
std::string floatLitType(const std::string& text) {
    if (text.size() >= 3 && text.compare(text.size() - 3, 3, "f32") == 0) return "f32";
    if (text.size() >= 3 && text.compare(text.size() - 3, 3, "f64") == 0) return "f64";
    char last = text.back();
    if (last == 'f') return "f32";
    return "f64";
}

// A §3.B-refused construct named in type position. Returns a targeted "Polyglot refuses X because …"
// message (never a generic "unknown type"), or nullptr. The refused surface has no grammar, but a user
// reaching for a familiar platform type by name should get told *why* and *what to use instead* — the
// PRD §3.B "refuse out loud, never miscompile" rule.
const char* refusedReason(const std::string& n) {
    if (n == "Thread" || n == "Mutex" || n == "Monitor" || n == "Semaphore" || n == "Lock" ||
        n == "Interlocked" || n == "ReaderWriterLock" || n == "ThreadPool")
        return "Polyglot refuses threads and locks — it targets single-threaded async only (PRD §3.B)";
    if (n == "decimal" || n == "Decimal")
        return "Polyglot refuses 'decimal' — no portable cross-target decimal exists; use a fixed-point std type (PRD §3.B/§3.D)";
    if (n == "IntPtr" || n == "UIntPtr" || n == "nint" || n == "nuint" || n == "Span" ||
        n == "ReadOnlySpan" || n == "Pointer")
        return "Polyglot refuses pointers and unsafe memory (no 'unsafe', '*T', 'stackalloc', raw Span) (PRD §3.B)";
    if (n == "dynamic")
        return "Polyglot refuses 'dynamic' and runtime code generation (PRD §3.B)";
    if (n == "Expression")
        return "Polyglot refuses LINQ expression trees — a lambda is always an executable closure, never code-as-data (PRD §3.B)";
    if (n == "Activator")
        return "Polyglot refuses runtime reflection ('Activator', open-world 'Type.GetType') — compile-time metadata only (PRD §3.B)";
    return nullptr;
}

// Map an operator symbol to its overload method name (PRD §6.1 / SPEC §6.1).
std::string operatorMethod(const std::string& op) {
    if (op == "+") return "plus";
    if (op == "-") return "minus";
    if (op == "*") return "times";
    if (op == "/") return "div";
    if (op == "%") return "rem";
    if (op == "==") return "eq";
    if (op == "<") return "lt";
    if (op == "<=") return "le";
    if (op == ">") return "gt";
    if (op == ">=") return "ge";
    return "";
}

struct MemberInfo {
    std::string name;
    MemberKind kind;
    std::vector<TypeRef> params;  // Method/Operator/Init
    TypeRef type;                 // field/property type, or method/operator return type
    bool isStatic = false;        // a `static fn` member, called as `Type.method(...)`
    bool isOpen = false;          // `open`/`abstract` — overridable from a subclass (C#-convention `override`)
    bool isAsync = false;         // an `async` method — its call result is `Awaitable<ret>` (§4.7)
    std::size_t required = 0;     // leading params with no default — the minimum a call must supply
    std::vector<std::string> generics; // the method's own type-param names (for TypeArg inference)
    std::vector<std::string> numericParams; // subset of `generics` bounded by INumber (must infer to a number)
};
struct TypeInfo {
    std::vector<GenericParam> generics;
    std::vector<MemberInfo> members;
    std::vector<std::string> bases;  // named base class/interfaces — members are inherited from these
    std::vector<TypeRef> ctorParams; // record positional fields, or class init params
    std::size_t ctorRequired = 0;    // leading ctor params with no default
    bool hasCtor = false;
    bool complete = true;            // we fully know this type's members (record/class/interface)
};
struct FnSig {
    std::vector<TypeRef> params;
    std::size_t required = 0;        // leading params with no default
    TypeRef result = namedType("unit");
    std::vector<std::string> generics; // the function's type-param names (for TypeArg inference)
    std::vector<std::string> numericParams; // subset of `generics` bounded by INumber (must infer to a number)
    TypeRef receiver;                  // extensions only: the declared receiver type (for inference from it)
    bool isAsync = false;              // an `async fn` — its call result is `Awaitable<result>` (§4.7)
};

// The generic-param names bounded by the INumber marker — those whose inferred type argument must be numeric.
inline std::vector<std::string> numericBounded(const std::vector<GenericParam>& gs) {
    std::vector<std::string> out;
    for (const auto& g : gs)
        for (const auto& b : g.bounds)
            if (b.kind == TypeRef::Kind::Named && b.name == "INumber") { out.push_back(g.name); break; }
    return out;
}

// How many leading parameters a caller MUST supply: the count before the first defaulted one.
// (Defaults are trailing by convention; a non-defaulted param after a default is a latent error
// we don't diagnose here.)
std::size_t requiredCount(const std::vector<Param>& ps) {
    std::size_t n = 0;
    for (const auto& p : ps) { if (p.hasDefault) break; ++n; }
    return n;
}

// Render a TypeRef for a diagnostic (author-facing, loose): `name<args>?`, tuples parenthesized,
// function types elided. An empty name reads as `unit`.
std::string sigTypeName(const TypeRef& t) {
    if (t.kind == TypeRef::Kind::Tuple) {
        std::string s = "(";
        for (std::size_t i = 0; i < t.args.size(); ++i) { if (i) s += ", "; s += sigTypeName(t.args[i]); }
        return s + ")";
    }
    if (t.kind == TypeRef::Kind::Function) return "fn";
    std::string s = t.name.empty() ? "unit" : t.name;
    if (!t.args.empty()) {
        s += "<";
        for (std::size_t i = 0; i < t.args.size(); ++i) { if (i) s += ", "; s += sigTypeName(t.args[i]); }
        s += ">";
    }
    if (t.nullable) s += "?";
    return s;
}

// Two parameter lists denote the same overload signature (a true duplicate): same arity and each position
// the same named type / same scalar category.
bool sameParamList(const std::vector<TypeRef>& a, const std::vector<TypeRef>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (sameNamedType(a[i], b[i])) continue;
        Ty sa = scalarTyOf(a[i]);
        if (sa != Ty::Unknown && sa == scalarTyOf(b[i])) continue;
        return false;
    }
    return true;
}


// A function's per-target name. C# keeps the source name (native overloading); TS needs a distinct name
// per overload, so an overloaded function mangles its parameter types in: `describe$i32`, `add$i32_i32`.
std::string mangleFn(const std::string& name, const std::vector<TypeRef>& params, bool overloaded) {
    if (!overloaded) return name;
    std::string m = name + "$";
    if (params.empty()) return m + "unit";
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (i) m += "_";
        m += (params[i].kind == TypeRef::Kind::Named && !params[i].name.empty()) ? params[i].name : "x";
    }
    return m;
}

// Pick the best-matching overload for the given argument types: exact/scalar match (2 pts) beats an
// implicit widen (1 pt); a non-numeric (generic/user) parameter matches leniently. nullptr = no candidate.
const FnSig* resolveOverload(const std::vector<FnSig>& set, const std::vector<TypeRef>& args) {
    if (set.size() == 1) return &set[0]; // not overloaded — arity/type errors are reported by checkArgs
    const FnSig* best = nullptr;
    int bestScore = -1;
    for (const auto& sig : set) {
        if (sig.params.size() != args.size()) continue;
        int score = 0;
        bool ok = true;
        for (std::size_t i = 0; i < args.size(); ++i) {
            const TypeRef& p = sig.params[i];
            const TypeRef& a = args[i];
            Ty sa = scalarTyOf(a);
            if (sameNamedType(a, p) || (sa != Ty::Unknown && sa == scalarTyOf(p))) score += 2;
            else if (isLosslessWiden(a, p)) score += 1;
            else if (!isNumericTypeName(p)) score += 0; // generic/user/unknown parameter: lenient
            else { ok = false; break; }
        }
        if (ok && score > bestScore) { best = &sig; bestScore = score; }
    }
    return best;
}
struct Local {
    TypeRef type;
    bool isMutable;
    int defId = -1;  // index into the SemanticModel's defs when indexing is on (§4.8); -1 otherwise
};

class Checker {
public:
    Checker(DiagnosticBag& diags) : diags_(diags) {}
    Checker(DiagnosticBag& diags, SemanticModel* model, const SemanticRequest* req)
        : diags_(diags), model_(model), req_(req) {}

    void run(CompilationUnit& unit) {
        collectTypeNames(unit);
        liftExtensionGenerics(unit);
        normalizeOptionalGenerics(unit); // `T?` (generic T) -> Option<T>, before tables read the types
        buildTables(unit);
        resolveAllTypes(unit);
        checkImplements(unit); // issue #29: implements-conformance, override rule, interface-body shape

        // Pre-register file-local definitions (functions, types + members, values) so a reference from any
        // user body resolves to one. The linker APPENDS merged std/import decls, so indices [0, userX) are
        // the entry file's own. (First decl of a name wins; overload precision is a follow-up.)
        if (model_ && req_) registerUserSymbols(unit);

        for (std::size_t i = 0; i < unit.functions.size(); ++i) {
            auto& fn = unit.functions[i];
            recordModel_ = model_ && req_ && i < req_->userFunctions; // index the entry file's own bodies only
            currentReturn_ = fn.returnType;
            currentThis_ = tUnknown();
            inActual_ = !fn.actualTarget.empty(); // only an `actual` body is a target-gated region (§4.4)
            inAsync_ = fn.isAsync;
            scopeStart_ = fn.namePos; scopeEnd_ = fn.bodyEnd; // locals here are in scope only within this fn
            pushGenerics(fn.generics);
            pushScope();
            for (const auto& p : fn.params) declare(p.name, p.type, false, p.pos);
            checkBlock(fn.body);
            popScope();
            popGenerics(fn.generics);
            scopeStart_ = {}; scopeEnd_ = {};
            inActual_ = false;
            inAsync_ = false;
            recordModel_ = false;
        }
        for (std::size_t i = 0; i < unit.records.size(); ++i) {
            recordModel_ = model_ && req_ && i < req_->userRecords; // record refs inside user type bodies too
            checkTypeBody(unit.records[i].name, unit.records[i].generics, unit.records[i].members, &unit.records[i].fields);
            recordModel_ = false;
        }
        for (std::size_t i = 0; i < unit.classes.size(); ++i) {
            recordModel_ = model_ && req_ && i < req_->userClasses;
            checkTypeBody(unit.classes[i].name, unit.classes[i].generics, unit.classes[i].members, nullptr);
            checkFieldsInitialized(unit.classes[i]);
            recordModel_ = false;
        }
        for (auto& d : unit.extensions) { // `this` denotes the receiver inside an extension body
            currentReturn_ = d.returnType;
            currentThis_ = d.receiver;
            pushGenerics(d.generics);
            pushScope();
            for (const auto& p : d.params) declare(p.name, p.type, false, p.pos);
            if (d.exprBody) { checkExpr(*d.exprBody); checkConvert(d.exprBody, d.returnType, "extension body"); }
            else checkBlock(d.body);
            popScope();
            popGenerics(d.generics);
            currentThis_ = tUnknown();
        }
        for (auto& v : unit.values) { // type-check top-level const/let initializers (so their types propagate)
            if (!v.init) continue;
            TypeRef it = checkExpr(*v.init);
            if (v.hasType) checkConvert(v.init, v.type, "initializer of '" + v.name + "'");
            else if (uninferableInit(*v.init, it)) // module-level binding with an un-inferable initializer (issue #27)
                diags_.error(v.pos, "cannot infer the type of '" + v.name + "' from its initializer; " + annotationHint(v.name, *v.init));
        }
    }

private:
    DiagnosticBag& diags_;
    std::unordered_map<std::string, std::vector<FnSig>> fns_; // name -> overload set
    std::unordered_map<std::string, TypeInfo> types_;
    std::unordered_set<std::string> typeNames_;
    std::unordered_set<std::string> interfaceNames_;                       // declared `interface` names
    std::unordered_map<std::string, const InterfaceDecl*> interfaceDecls_; // for base type-args (conformance)
    std::unordered_set<std::string> enumNames_;
    std::unordered_map<std::string, TypeRef> values_;          // top-level const/let
    std::unordered_map<std::string, FnSig> unionCtors_;        // union case name -> ctor sig
    std::unordered_map<std::string, std::string> unionCaseOwner_; // case name -> union type
    std::unordered_map<std::string, std::vector<std::string>> unionAllCases_; // union -> all case names
    std::unordered_map<std::string, std::vector<std::string>> enumAllCases_;  // enum  -> all case names
    std::unordered_map<std::string, FnSig> extensions_;        // "RecvType.method" -> extension sig
    std::unordered_set<std::string> genericsInScope_;
    std::vector<std::unordered_map<std::string, Local>> scopes_;
    TypeRef currentReturn_ = namedType("unit");
    TypeRef currentThis_ = tUnknown();
    std::string currentClass_;  // enclosing type name while checking a member body; survives into static
                                // methods (where currentThis_ is Unknown) so its statics/consts resolve unqualified
    bool inActual_ = false; // checking a target-gated `actual` body — the only place `extern` is allowed
    bool inAsync_ = false;  // checking an `async fn`/method body — the only place `await` is allowed (§4.7)

    // ---- semantic model (LSP; §4.8). Populated only when model_ != nullptr, and only for file-local symbols.
    SemanticModel* model_ = nullptr;
    const SemanticRequest* req_ = nullptr;
    bool recordModel_ = false;                       // true only while checking a file-local (user) decl body
    SourcePos scopeStart_, scopeEnd_;                // enclosing fn/method extent; stamped on Local/Parameter defs (§4.8)
    std::unordered_map<std::string, int> fnDefId_;     // user function name -> its SymbolDef index (for call refs)
    std::unordered_map<std::string, int> typeDefId_;   // user type name -> SymbolDef index (construction refs)
    std::unordered_map<std::string, int> valueDefId_;  // user top-level const/let name -> SymbolDef index
    std::unordered_map<std::string, int> memberDefId_; // "Type.member" -> SymbolDef index (own members)

    int recordDef(SymbolKind kind, const std::string& name, SourcePos namePos, const TypeRef& type,
                  bool external = false, const std::string& owner = "") {
        if (!model_) return -1;
        SymbolDef d;
        d.kind = kind;
        d.name = name;
        d.nameSpan = {namePos, static_cast<int>(name.size())};
        d.type = type;
        d.external = external;
        d.owner = owner;
        if (kind == SymbolKind::Local || kind == SymbolKind::Parameter) { d.scopeStart = scopeStart_; d.scopeEnd = scopeEnd_; }
        model_->defs.push_back(std::move(d));
        return static_cast<int>(model_->defs.size()) - 1;
    }
    void recordRef(SourcePos pos, const std::string& name, int defId) {
        if (!model_ || !recordModel_ || defId < 0) return;
        model_->refs.push_back({{pos, static_cast<int>(name.size())}, defId});
    }

    // Register a file-local nominal type + its members as definitions (so a construction / member access
    // can resolve to them). Members are keyed "Type.member". Definition positions are the decl keyword for
    // now (jumps to the right line); precise name columns are a follow-up like FunctionDecl::namePos.
    void registerType(const std::string& name, SourcePos namePos, const std::vector<Member>& members, bool external) {
        typeDefId_.emplace(name, recordDef(SymbolKind::Type, name, namePos, tNamed(name), external));
        for (const auto& m : members) {
            SymbolKind k = (m.kind == MemberKind::Method || m.kind == MemberKind::Operator) ? SymbolKind::Method
                         : (m.kind == MemberKind::Property) ? SymbolKind::Method
                         : SymbolKind::Field;
            const TypeRef& t = (m.kind == MemberKind::Method || m.kind == MemberKind::Operator) ? m.returnType : m.type;
            memberDefId_.emplace(name + "." + m.name, recordDef(k, m.name, m.namePos, t, external, name));
        }
    }
    // Register EVERY merged declaration so references resolve — the entry file's own (indices [0, userX),
    // flagged file-local) and the appended std/import decls (flagged external, carrying their module's
    // fileId for cross-module go-to-definition). Entry decls are registered first, so they win in the
    // name->def maps (a user symbol shadows an external of the same name).
    void registerUserSymbols(const CompilationUnit& unit) {
        for (std::size_t i = 0; i < unit.functions.size(); ++i) {
            const auto& fn = unit.functions[i];
            fnDefId_.emplace(fn.name, recordDef(SymbolKind::Function, fn.name, fn.namePos, fn.returnType, i >= req_->userFunctions));
        }
        for (std::size_t i = 0; i < unit.records.size(); ++i) {
            const auto& r = unit.records[i];
            bool ext = i >= req_->userRecords;
            registerType(r.name, r.namePos, r.members, ext);
            for (const auto& f : r.fields) // positional record fields (Param.pos is already the name)
                memberDefId_.emplace(r.name + "." + f.name, recordDef(SymbolKind::Field, f.name, f.pos, f.type, ext, r.name));
        }
        for (std::size_t i = 0; i < unit.classes.size(); ++i)    registerType(unit.classes[i].name,    unit.classes[i].namePos,    unit.classes[i].members,    i >= req_->userClasses);
        for (std::size_t i = 0; i < unit.interfaces.size(); ++i) registerType(unit.interfaces[i].name, unit.interfaces[i].namePos, unit.interfaces[i].members, i >= req_->userInterfaces);
        for (std::size_t i = 0; i < unit.enums.size(); ++i)      typeDefId_.emplace(unit.enums[i].name,  recordDef(SymbolKind::Type, unit.enums[i].name,  unit.enums[i].namePos,  tNamed(unit.enums[i].name),  i >= req_->userEnums));
        for (std::size_t i = 0; i < unit.unions.size(); ++i)     typeDefId_.emplace(unit.unions[i].name, recordDef(SymbolKind::Type, unit.unions[i].name, unit.unions[i].namePos, tNamed(unit.unions[i].name), i >= req_->userUnions));
        for (std::size_t i = 0; i < unit.values.size(); ++i) {
            const auto& v = unit.values[i];
            valueDefId_.emplace(v.name, recordDef(SymbolKind::Value, v.name, v.namePos, v.hasType ? v.type : TypeRef{}, i >= req_->userValues));
        }
    }

    // ---- scopes ----
    void pushScope() { scopes_.emplace_back(); }
    void popScope() { scopes_.pop_back(); }
    void declare(const std::string& name, TypeRef type, bool isMutable, SourcePos pos) {
        auto& top = scopes_.back();
        if (top.count(name)) diags_.error(pos, "'" + name + "' is already declared in this scope");
        int defId = recordModel_ ? recordDef(SymbolKind::Local, name, pos, type) : -1;
        top[name] = {std::move(type), isMutable, defId};
    }
    const Local* lookup(const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) return &found->second;
        }
        return nullptr;
    }

    // ---- declaration tables ----
    // `externOnBuiltin`: an `extern class` naming a builtin scalar (std.core's `extern class i32 { static
    // fn parse … }`) is a MEMBER CARRIER, not a shadow — the scalar type stays authoritative, so the name
    // is not registered as a declared type; only its binding members matter (lower reads them by name).
    void declareType(const std::string& name, SourcePos pos, bool externOnBuiltin = false) {
        if (isBuiltinType(name)) {
            if (!externOnBuiltin) diags_.error(pos, "'" + name + "' shadows a builtin type");
            return;
        }
        if (!typeNames_.insert(name).second) diags_.error(pos, "duplicate type '" + name + "'");
    }
    void collectTypeNames(const CompilationUnit& u) {
        for (const auto& d : u.records)    declareType(d.name, d.pos);
        for (const auto& d : u.classes)    declareType(d.name, d.pos, d.isExtern);
        for (const auto& d : u.interfaces) declareType(d.name, d.pos);
        for (const auto& d : u.enums)      { declareType(d.name, d.pos); enumNames_.insert(d.name); }
        for (const auto& d : u.unions)     declareType(d.name, d.pos);
    }

    // An extension's type variables are written on the receiver — SPEC §6.3 `extension fn List<T>.m(): T?`.
    // The receiver is a *pattern*, so a type-arg leaf that names nothing known (not a builtin, not a declared
    // type, not already a generic) is a binding occurrence of a type parameter. Lift those into the
    // extension's generic list so every downstream pass (tables, resolution, body check, lower, both
    // emitters — all of which already read `generics`) treats them uniformly. Runs after collectTypeNames
    // so `typeNames_` is populated.
    void collectReceiverVars(const TypeRef& t, std::unordered_set<std::string>& have, std::vector<std::string>& out) const {
        for (const auto& a : t.args) {
            if (a.kind == TypeRef::Kind::Named && !a.name.empty() && a.args.empty() &&
                !isBuiltinType(a.name) && !typeNames_.count(a.name) && have.insert(a.name).second)
                out.push_back(a.name);
            collectReceiverVars(a, have, out);
        }
        for (const auto& r : t.ret) collectReceiverVars(r, have, out);
    }
    void liftExtensionGenerics(CompilationUnit& u) {
        for (auto& e : u.extensions) {
            std::unordered_set<std::string> have;
            for (const auto& g : e.generics) have.insert(g.name);
            std::vector<std::string> found;
            collectReceiverVars(e.receiver, have, found);
            for (const auto& n : found) { GenericParam g; g.name = n; e.generics.push_back(std::move(g)); }
        }
    }

    // `T?` where `T` is a type parameter in scope has no faithful native nullable (PRD §3.C): rewrite it to
    // the core `Option<T>` union. Concrete `T?` (`string?`, `i32?`, `List<T>?`) keeps the native nullable.
    // Runs before buildTables so every table + downstream pass sees the normalized type.
    static void normalizeOptional(TypeRef& t, const std::unordered_set<std::string>& gens) {
        for (auto& a : t.args) normalizeOptional(a, gens);
        for (auto& r : t.ret) normalizeOptional(r, gens);
        if (t.nullable && t.kind == TypeRef::Kind::Named && gens.count(t.name)) {
            TypeRef inner; inner.kind = TypeRef::Kind::Named; inner.name = t.name; // the bare `T`
            t.name = "Option"; t.nullable = false; t.args = { std::move(inner) };
        }
    }
    static void normalizeMembers(std::vector<Member>& ms, std::unordered_set<std::string> classGens) {
        for (auto& m : ms) {
            auto g = classGens; for (const auto& mg : m.generics) g.insert(mg.name);
            if (m.kind == MemberKind::Field || m.kind == MemberKind::Const || m.kind == MemberKind::Property) normalizeOptional(m.type, g);
            else { for (auto& p : m.params) normalizeOptional(p.type, g); normalizeOptional(m.returnType, g); }
        }
    }
    void normalizeOptionalGenerics(CompilationUnit& u) {
        auto setOf = [](const std::vector<GenericParam>& gs) { std::unordered_set<std::string> s; for (const auto& g : gs) s.insert(g.name); return s; };
        for (auto& fn : u.functions)  { auto g = setOf(fn.generics); for (auto& p : fn.params) normalizeOptional(p.type, g); normalizeOptional(fn.returnType, g); }
        for (auto& d : u.records)     { auto g = setOf(d.generics); for (auto& f : d.fields) normalizeOptional(f.type, g); normalizeMembers(d.members, g); }
        for (auto& d : u.classes)     { auto g = setOf(d.generics); normalizeMembers(d.members, g); }
        for (auto& d : u.interfaces)  { auto g = setOf(d.generics); normalizeMembers(d.members, g); }
        for (auto& d : u.unions)      { auto g = setOf(d.generics); for (auto& c : d.cases) for (auto& p : c.params) normalizeOptional(p.type, g); }
        for (auto& d : u.extensions)  { auto g = setOf(d.generics); for (auto& p : d.params) normalizeOptional(p.type, g); normalizeOptional(d.returnType, g); }
        for (auto& v : u.values)      if (v.hasType) { std::unordered_set<std::string> g; normalizeOptional(v.type, g); }
        // (local `let x: T?` declared types inside bodies are normalized lazily in checkStmt's resolve.)
    }

    static MemberInfo memberInfo(const Member& m) {
        MemberInfo mi;
        mi.name = m.name;
        mi.kind = m.kind;
        for (const auto& p : m.params) mi.params.push_back(p.type);
        for (const auto& g : m.generics) mi.generics.push_back(g.name);
        mi.numericParams = numericBounded(m.generics);
        mi.required = requiredCount(m.params);
        mi.type = (m.kind == MemberKind::Field || m.kind == MemberKind::Const ||
                   m.kind == MemberKind::Property) ? m.type : m.returnType;
        for (const auto& mod : m.modifiers) {
            if (mod == "static") mi.isStatic = true;
            if (mod == "open" || mod == "abstract") mi.isOpen = true;
        }
        mi.isAsync = m.isAsync;
        return mi;
    }
    void addMembers(TypeInfo& ti, const std::vector<Member>& members) {
        for (const auto& m : members) {
            ti.members.push_back(memberInfo(m));
            if (m.kind == MemberKind::Init) { ti.hasCtor = true; ti.ctorRequired = requiredCount(m.params); for (const auto& p : m.params) ti.ctorParams.push_back(p.type); }
        }
    }
    static void collectBaseNames(const std::vector<TypeRef>& bases, std::vector<std::string>& out) {
        for (const auto& b : bases) if (b.kind == TypeRef::Kind::Named && !b.name.empty()) out.push_back(b.name);
    }
    void buildTables(CompilationUnit& u) {
        for (const auto& d : u.records) {
            TypeInfo ti; ti.generics = d.generics; ti.hasCtor = true; ti.ctorRequired = requiredCount(d.fields);
            for (const auto& f : d.fields) { ti.ctorParams.push_back(f.type); ti.members.push_back({f.name, MemberKind::Field, {}, f.type}); }
            addMembers(ti, d.members); collectBaseNames(d.bases, ti.bases);
            types_[d.name] = std::move(ti);
        }
        for (const auto& d : u.classes)    { TypeInfo ti; ti.generics = d.generics; addMembers(ti, d.members); collectBaseNames(d.bases, ti.bases); types_[d.name] = std::move(ti); }
        for (const auto& d : u.interfaces) {
            TypeInfo ti; ti.generics = d.generics; addMembers(ti, d.members); collectBaseNames(d.bases, ti.bases);
            types_[d.name] = std::move(ti);
            interfaceNames_.insert(d.name);
            interfaceDecls_[d.name] = &d; // base TypeRefs with their generic args (TypeInfo.bases keeps names only)
        }
        for (const auto& d : u.unions) {
            for (const auto& c : d.cases) {
                // A union case name must be unique across the program (it's a global constructor). Two
                // cases sharing a name — within a union, across unions, or merged from two modules — is a
                // hard error, not a silent last-wins overwrite.
                if (unionCtors_.count(c.name))
                    diags_.error(c.pos, "duplicate union case '" + c.name + "'");
                // A generic union's case is a generic constructor: `Some(value: T): Option<T>`. Carrying the
                // union's type params (sig.generics) + a parameterized result lets `Some(5)` infer Option<i32>.
                FnSig sig; for (const auto& p : c.params) sig.params.push_back(p.type);
                for (const auto& g : d.generics) sig.generics.push_back(g.name);
                TypeRef res = tNamed(d.name);
                for (const auto& g : d.generics) res.args.push_back(namedType(g.name));
                sig.result = res;
                unionCtors_[c.name] = sig;
                unionCaseOwner_[c.name] = d.name;
                unionAllCases_[d.name].push_back(c.name);
            }
        }
        for (const auto& d : u.enums)
            for (const auto& c : d.cases) enumAllCases_[d.name].push_back(c.name);
        for (const auto& fn : u.functions) { // collect overload sets; an `actual` is an impl, not a signature
            if (!fn.actualTarget.empty()) continue;
            FnSig sig; for (const auto& p : fn.params) sig.params.push_back(p.type); sig.result = fn.returnType;
            sig.required = requiredCount(fn.params);
            sig.isAsync = fn.isAsync;
            for (const auto& g : fn.generics) sig.generics.push_back(g.name);
            sig.numericParams = numericBounded(fn.generics);
            auto& set = fns_[fn.name];
            for (const auto& existing : set)
                if (sameParamList(existing.params, sig.params))
                    diags_.error(fn.pos, "duplicate function '" + fn.name + "' with the same parameter types");
            set.push_back(std::move(sig));
        }
        for (auto& fn : u.functions) { // assign per-target names: overloaded functions mangle their params
            if (!fn.actualTarget.empty()) { fn.mangledName = fn.name; continue; } // actuals reuse the expect name
            std::vector<TypeRef> ps; for (const auto& p : fn.params) ps.push_back(p.type);
            fn.mangledName = mangleFn(fn.name, ps, fns_[fn.name].size() > 1);
        }
        for (const auto& v : u.values) {
            if (values_.count(v.name)) diags_.error(v.pos, "duplicate top-level '" + v.name + "'");
            values_[v.name] = v.hasType ? v.type : tUnknown();
        }
        for (const auto& e : u.extensions) {
            std::string key = e.receiver.name + "." + e.name;
            if (extensions_.count(key)) diags_.error(e.pos, "duplicate extension '" + key + "'");
            FnSig sig; for (const auto& p : e.params) sig.params.push_back(p.type); sig.result = e.returnType;
            sig.receiver = e.receiver;
            for (const auto& g : e.generics) sig.generics.push_back(g.name); // for inference from receiver/args
            extensions_[key] = std::move(sig); // keyed by receiver type + method
        }
    }

    const MemberInfo* findMember(const std::string& typeName, const std::string& name) const {
        // `Error.message` resolves here like any other member: `Error` is a core-prelude `extern class` whose
        // `message` property is in `types_`, reached directly or via a `: Error` base (the loop below).
        auto it = types_.find(typeName);
        if (it == types_.end()) return nullptr;
        for (const auto& m : it->second.members) if (m.name == name) return &m;
        for (const auto& base : it->second.bases) if (const MemberInfo* m = findMember(base, name)) return m; // inherited
        return nullptr;
    }
    bool knownType(const TypeRef& t) const {
        return t.kind == TypeRef::Kind::Named && !t.name.empty() && types_.count(t.name) != 0;
    }

    // ---- interface conformance + `override` validation (issue #29) ----
    // The contract behind `class X : IFoo` is checked at the SOURCE: every method the interface (and its
    // base interfaces) declares must be implemented with a matching signature, so a breach is a Polyglot
    // diagnostic — never an error surfacing later in one target's generated code. `override` follows C#
    // conventions: it marks only an override of an `open`/`abstract` base-CLASS member; implementing an
    // interface method takes no `override`.
    bool inheritsFrom(const std::string& sub, const std::string& super) const {
        auto it = types_.find(sub);
        if (it == types_.end()) return false;
        for (const auto& b : it->second.bases)
            if (b == super || inheritsFrom(b, super)) return true;
        return false;
    }
    bool interfaceHasMethod(const std::string& iface, const std::string& name) const {
        auto it = types_.find(iface);
        if (it == types_.end()) return false;
        for (const auto& m : it->second.members) if (m.name == name) return true;
        for (const auto& b : it->second.bases) if (interfaceHasMethod(b, name)) return true;
        return false;
    }
    // Methods named `name` on the type or its base-CLASS chain. Interface bases are excluded — an
    // interface's own signature must never satisfy (or be overridden as) a class member.
    void gatherMethods(const std::string& typeName, const std::string& name, std::vector<const MemberInfo*>& out) const {
        auto it = types_.find(typeName);
        if (it == types_.end()) return;
        for (const auto& m : it->second.members)
            if (m.name == name && (m.kind == MemberKind::Method || m.kind == MemberKind::Operator)) out.push_back(&m);
        for (const auto& b : it->second.bases)
            if (!interfaceNames_.count(b)) gatherMethods(b, name, out);
    }
    // Like sameNamedType but signature-position: nullability must AGREE (not be absent), so an interface's
    // `Node?` is satisfied by an implementing `Node?`.
    static bool sameSigType(const TypeRef& a, const TypeRef& b) {
        if (a.kind == TypeRef::Kind::Named && b.kind == TypeRef::Kind::Named &&
            a.name == b.name && a.nullable == b.nullable) return true;
        Ty sa = scalarTyOf(a);
        return sa != Ty::Unknown && sa == scalarTyOf(b);
    }
    static bool sameSigParams(const std::vector<TypeRef>& a, const std::vector<TypeRef>& b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) if (!sameSigType(a[i], b[i])) return false;
        return true;
    }
    static bool sameRetType(TypeRef a, TypeRef b) {
        if (a.kind == TypeRef::Kind::Named && a.name.empty()) a.name = "unit";
        if (b.kind == TypeRef::Kind::Named && b.name.empty()) b.name = "unit";
        return sameSigType(a, b);
    }
    struct RequiredMethod { std::string iface; std::string name; std::vector<TypeRef> params; TypeRef ret; };
    static std::string describeRequired(const RequiredMethod& r) {
        std::string s = "fn " + r.name + "(";
        for (std::size_t i = 0; i < r.params.size(); ++i) { if (i) s += ", "; s += sigTypeName(r.params[i]); }
        s += ")";
        std::string ret = sigTypeName(r.ret);
        if (ret != "unit") s += ": " + ret;
        return s;
    }
    // Every method the interface demands of an implementer, with the interface's type params substituted
    // from the implements-clause type args (`Comparable<Version>` demands `compareTo(Version)`), incl.
    // transitively inherited interface methods (args composed through each hop).
    void collectRequired(const std::string& iface, const std::vector<TypeRef>& args,
                         std::vector<RequiredMethod>& out, std::unordered_set<std::string>& seen) {
        if (!seen.insert(iface).second) return; // repeated/diamond base — first visit wins
        auto ti = types_.find(iface);
        if (ti == types_.end()) return;
        std::unordered_set<std::string> gen;
        std::unordered_map<std::string, TypeRef> binds;
        for (std::size_t i = 0; i < ti->second.generics.size(); ++i) {
            gen.insert(ti->second.generics[i].name);
            if (i < args.size()) binds[ti->second.generics[i].name] = args[i];
        }
        for (const auto& m : ti->second.members) {
            if (m.kind != MemberKind::Method) continue;
            RequiredMethod r{iface, m.name, {}, substGeneric(m.type, gen, binds)};
            for (const auto& p : m.params) r.params.push_back(substGeneric(p, gen, binds));
            out.push_back(std::move(r));
        }
        if (auto di = interfaceDecls_.find(iface); di != interfaceDecls_.end())
            for (const auto& b : di->second->bases) {
                if (b.kind != TypeRef::Kind::Named || !interfaceNames_.count(b.name)) continue;
                std::vector<TypeRef> subArgs;
                for (const auto& a : b.args) subArgs.push_back(substGeneric(a, gen, binds));
                collectRequired(b.name, subArgs, out, seen);
            }
    }
    void checkImplements(const CompilationUnit& u) {
        // Interface bodies: bodiless, non-static method signatures only (issue #29 v1 scope). Anything
        // else used to be accepted and silently DROPPED at lowering — refuse it instead.
        for (const auto& d : u.interfaces) {
            for (const auto& m : d.members) {
                const char* what = m.kind == MemberKind::Field ? "a field"
                                 : m.kind == MemberKind::Const ? "a const"
                                 : m.kind == MemberKind::Property ? "a property"
                                 : m.kind == MemberKind::Init ? "an initializer" : nullptr;
                if (what) {
                    diags_.error(m.pos, "an interface declares method signatures only — " + std::string(what) +
                                            " is not allowed in 'interface " + d.name + "' (express it as a method)");
                    continue;
                }
                if (m.kind == MemberKind::Operator) {
                    diags_.error(m.pos, "an interface cannot declare an operator — operator contracts are not "
                                        "portable across targets; declare a named method instead");
                    continue;
                }
                if (m.hasBody)
                    diags_.error(m.pos, "interface method '" + m.name + "' must be a bodiless signature "
                                        "(default method bodies are not supported)");
                for (const auto& mod : m.modifiers)
                    if (mod == "static")
                        diags_.error(m.pos, "interface method '" + m.name + "' cannot be static");
            }
        }
        auto checkType = [&](const std::string& name, SourcePos namePos, const std::vector<TypeRef>& bases,
                             const std::vector<Member>& members, const char* what) {
            for (const auto& b : bases) {
                if (b.kind != TypeRef::Kind::Named || !interfaceNames_.count(b.name)) continue;
                std::vector<RequiredMethod> reqs;
                std::unordered_set<std::string> seen;
                collectRequired(b.name, b.args, reqs, seen);
                for (const auto& r : reqs) {
                    std::vector<const MemberInfo*> cands;
                    gatherMethods(name, r.name, cands);
                    bool ok = false, nameExists = false;
                    for (const MemberInfo* c : cands) {
                        if (c->isStatic) continue;
                        nameExists = true;
                        if (sameSigParams(c->params, r.params) && sameRetType(c->type, r.ret)) { ok = true; break; }
                    }
                    if (ok) continue;
                    if (nameExists)
                        diags_.error(namePos, std::string(what) + " '" + name + "' implements '" + r.iface +
                                                  "' but its '" + r.name + "' does not match the interface signature '" +
                                                  describeRequired(r) + "'");
                    else
                        diags_.error(namePos, std::string(what) + " '" + name + "' does not implement '" +
                                                  describeRequired(r) + "' declared by interface '" + r.iface + "'");
                }
            }
            for (const auto& m : members) {
                bool hasOverride = false;
                for (const auto& mod : m.modifiers) if (mod == "override") hasOverride = true;
                if (!hasOverride) continue;
                bool inBaseClass = false, baseOpen = false, inInterface = false;
                for (const auto& b : bases) {
                    if (b.kind != TypeRef::Kind::Named || b.name.empty()) continue;
                    if (interfaceNames_.count(b.name)) {
                        if (interfaceHasMethod(b.name, m.name)) inInterface = true;
                    } else {
                        std::vector<const MemberInfo*> cands;
                        gatherMethods(b.name, m.name, cands);
                        for (const MemberInfo* c : cands) { inBaseClass = true; if (c->isOpen) baseOpen = true; }
                    }
                }
                if (inBaseClass) {
                    if (!baseOpen)
                        diags_.error(m.namePos, "'" + m.name + "' overrides a base-class member that is not "
                                                "'open'/'abstract'");
                } else if (inInterface) {
                    diags_.error(m.namePos, "implementing an interface method takes no 'override' — remove it "
                                            "('override' marks only an override of an open/abstract base-class member)");
                } else {
                    diags_.error(m.namePos, "'override' on '" + m.name + "' has no base-class member to override");
                }
            }
        };
        for (const auto& d : u.records) checkType(d.name, d.namePos, d.bases, d.members, "record");
        for (const auto& d : u.classes) if (!d.isExtern) checkType(d.name, d.namePos, d.bases, d.members, "class");
    }

    // `Awaitable<T>` — the type of an in-flight async result. Compile-time-only (§4.7): the author never
    // writes it (they write the unwrapped `T` and `await` to get it back); each backend synthesizes the real
    // wrapper (`Task<T>`/`Promise<T>`) from the callee's `isAsync` flag, so this name never reaches emission.
    static TypeRef awaitable(const TypeRef& inner) {
        TypeRef a; a.kind = TypeRef::Kind::Named; a.name = "Awaitable"; a.args.push_back(inner); return a;
    }
    static bool isAwaitable(const TypeRef& t) {
        return t.kind == TypeRef::Kind::Named && t.name == "Awaitable";
    }

    // ---- type resolution (P4-1) ----
    void pushGenerics(const std::vector<GenericParam>& gs) { for (const auto& g : gs) genericsInScope_.insert(g.name); }
    void popGenerics(const std::vector<GenericParam>& gs) { for (const auto& g : gs) genericsInScope_.erase(g.name); }
    void resolveTypeRef(const TypeRef& t, SourcePos pos) {
        switch (t.kind) {
            case TypeRef::Kind::Named:
                if (!t.name.empty()) {
                    if (const char* why = refusedReason(t.name)) diags_.error(pos, why);
                    else if (!isBuiltinType(t.name) && !genericsInScope_.count(t.name) && !typeNames_.count(t.name))
                        diags_.error(pos, "unknown type '" + t.name + "'");
                }
                for (const auto& a : t.args) resolveTypeRef(a, pos);
                break;
            case TypeRef::Kind::Tuple:
                for (const auto& a : t.args) resolveTypeRef(a, pos);
                break;
            case TypeRef::Kind::Function:
                for (const auto& a : t.args) resolveTypeRef(a, pos);
                for (const auto& r : t.ret) resolveTypeRef(r, pos);
                break;
        }
    }
    void resolveParams(const std::vector<Param>& ps, SourcePos fb) { for (const auto& p : ps) if (!p.type.absent()) resolveTypeRef(p.type, p.pos.line ? p.pos : fb); }
    void resolveBounds(const std::vector<GenericParam>& gs, SourcePos pos) { for (const auto& g : gs) for (const auto& b : g.bounds) resolveTypeRef(b, pos); }
    void resolveMembers(const std::vector<Member>& ms) {
        for (const auto& m : ms) {
            pushGenerics(m.generics); resolveBounds(m.generics, m.pos);
            if (m.kind == MemberKind::Field || m.kind == MemberKind::Const || m.kind == MemberKind::Property) {
                if (!m.type.absent()) resolveTypeRef(m.type, m.pos);
            } else { resolveParams(m.params, m.pos); resolveTypeRef(m.returnType, m.pos); }
            popGenerics(m.generics);
        }
    }
    void resolveAllTypes(const CompilationUnit& u) {
        for (const auto& fn : u.functions) { pushGenerics(fn.generics); resolveBounds(fn.generics, fn.pos); resolveParams(fn.params, fn.pos); resolveTypeRef(fn.returnType, fn.pos); popGenerics(fn.generics); }
        for (const auto& d : u.records)    { pushGenerics(d.generics); resolveBounds(d.generics, d.pos); resolveParams(d.fields, d.pos); for (const auto& b : d.bases) resolveTypeRef(b, d.pos); resolveMembers(d.members); popGenerics(d.generics); }
        for (const auto& d : u.classes)    { pushGenerics(d.generics); resolveBounds(d.generics, d.pos); for (const auto& b : d.bases) resolveTypeRef(b, d.pos); resolveMembers(d.members); popGenerics(d.generics); }
        for (const auto& d : u.interfaces) { pushGenerics(d.generics); resolveBounds(d.generics, d.pos); for (const auto& b : d.bases) resolveTypeRef(b, d.pos); resolveMembers(d.members); popGenerics(d.generics); }
        for (const auto& d : u.unions) { pushGenerics(d.generics); resolveBounds(d.generics, d.pos); for (const auto& c : d.cases) resolveParams(c.params, c.pos); popGenerics(d.generics); }
        for (const auto& d : u.extensions) { pushGenerics(d.generics); resolveBounds(d.generics, d.pos); resolveTypeRef(d.receiver, d.pos); resolveParams(d.params, d.pos); resolveTypeRef(d.returnType, d.pos); popGenerics(d.generics); }
        for (const auto& v : u.values)     if (v.hasType) resolveTypeRef(v.type, v.pos);
    }

    // G11 (wave 2): a class field with no initializer must be assigned in `init` — otherwise a read
    // observes four DIFFERENT defaults (C# 0, TS undefined, Python AttributeError, PHP null). Defining
    // the error out of existence: the checker demands the assignment, so no target ever reads one.
    // Any `this.<name> = …` anywhere in the ctor (including branches) counts — the guard targets
    // NEVER-assigned fields, not flow-sensitive definite assignment.
    static void collectThisAssigns(const std::vector<StmtPtr>& body, std::unordered_set<std::string>& out) {
        for (const auto& s : body) {
            if (!s) continue;
            if (s->kind == StmtKind::Assign && s->target && s->target->kind == ExprKind::Member &&
                s->target->lhs && s->target->lhs->kind == ExprKind::This)
                out.insert(s->target->text);
            collectThisAssigns(s->thenBody, out);
            collectThisAssigns(s->elseBody, out);
            collectThisAssigns(s->finallyBody, out);
            for (const auto& c : s->catches) collectThisAssigns(c.body, out);
        }
    }
    void checkFieldsInitialized(const ClassDecl& d) {
        if (d.isExtern) return;
        std::unordered_set<std::string> assigned;
        for (const auto& m : d.members)
            if (m.kind == MemberKind::Init) collectThisAssigns(m.body, assigned);
        for (const auto& m : d.members) {
            if (m.kind != MemberKind::Field || m.init || m.bindings.size()) continue;
            if (!assigned.count(m.name))
                diags_.error(m.pos, "field '" + m.name + "' of class '" + d.name +
                                        "' is never initialized — give it an initializer or assign it in init "
                                        "(uninitialized reads differ per target)");
        }
    }

    // ---- body checking ----
    void checkTypeBody(const std::string& typeName, const std::vector<GenericParam>& generics,
                       std::vector<Member>& members, std::vector<Param>* recordFields) {
        pushGenerics(generics);
        currentThis_ = tNamed(typeName);
        currentClass_ = typeName;
        for (auto& m : members) {
            pushGenerics(m.generics);
            if (m.kind == MemberKind::Method || m.kind == MemberKind::Operator || m.kind == MemberKind::Init) {
                bool isStatic = false;
                for (const auto& mod : m.modifiers) if (mod == "static") isStatic = true;
                currentThis_ = isStatic ? tUnknown() : tNamed(typeName); // no `this` inside a static method
                currentReturn_ = (m.kind == MemberKind::Init) ? namedType("unit") : m.returnType;
                inAsync_ = m.isAsync;
                scopeStart_ = m.namePos; scopeEnd_ = m.bodyEnd; // method locals scoped to this member body
                pushScope();
                if (recordFields) for (const auto& f : *recordFields) declare(f.name, f.type, false, f.pos);
                for (const auto& p : m.params) declare(p.name, p.type, false, p.pos);
                if (m.hasBody && m.exprBodied && m.exprBody) { checkExpr(*m.exprBody); if (m.kind != MemberKind::Init) checkConvert(m.exprBody, m.returnType, "method body"); }
                else if (m.hasBody) checkBlock(m.body);
                popScope();
                scopeStart_ = {}; scopeEnd_ = {};
                inAsync_ = false;
                currentThis_ = tNamed(typeName);
            } else if (m.kind == MemberKind::Property && (m.init || !m.body.empty() || m.hasSetter)) {
                currentReturn_ = m.type;
                pushScope();
                if (recordFields) for (const auto& f : *recordFields) declare(f.name, f.type, false, f.pos);
                if (m.init) { checkExpr(*m.init); checkConvert(m.init, m.type, "property getter"); }
                else if (!m.body.empty()) checkBlock(m.body); // block-bodied getter (accessor block, #39c)
                popScope();
                if (m.hasSetter) { // the setter body sees `this`, the record fields, and the value param
                    currentReturn_ = namedType("unit");
                    pushScope();
                    if (recordFields) for (const auto& f : *recordFields) declare(f.name, f.type, false, f.pos);
                    declare(m.setterParam, m.type, false, m.pos);
                    checkBlock(m.setterBody);
                    popScope();
                }
            }
            popGenerics(m.generics);
        }
        currentThis_ = tUnknown();
        currentClass_.clear();
        popGenerics(generics);
    }

    void checkBlock(std::vector<StmtPtr>& body) {
        pushScope();
        for (auto& s : body) checkStmt(*s);
        popScope();
    }

    // Wrap an expression in an implicit widening cast (mutates the AST so lowering/emit convert it).
    void coerce(ExprPtr& slot, const TypeRef& target) {
        auto cast = std::make_unique<Expr>();
        cast->kind = ExprKind::Cast;
        cast->pos = slot->pos;
        cast->castType = target;
        cast->type = target;
        cast->lhs = std::move(slot);
        slot = std::move(cast);
    }

    // A payload-free union case (e.g. `None`) is typed `U<unbound>` bottom-up, since a bare case carries no
    // value to infer the union's type args from. When it flows into a known `U<concrete>` slot, stamp the
    // concrete type so construction emits the right args (C# `new None<int>()`). Recurse through if/match so a
    // bare case in a branch instantiates too (`if b { Some(x) } else { None }` returned as Option<i32>).
    void instantiateBareCases(ExprPtr& slot, const TypeRef& expected) {
        if (!slot || expected.kind != TypeRef::Kind::Named || expected.args.empty()) return;
        switch (slot->kind) {
            case ExprKind::Name: {
                auto owner = unionCaseOwner_.find(slot->text);
                if (owner != unionCaseOwner_.end() && owner->second == expected.name) slot->type = expected;
                break;
            }
            case ExprKind::IfExpr:
                instantiateBareCases(slot->rhs, expected);   // then-branch
                instantiateBareCases(slot->extra, expected); // else-branch
                break;
            case ExprKind::Match:
                for (auto& arm : slot->arms) if (!arm.bodyIsBlock && arm.body) instantiateBareCases(arm.body, expected);
                break;
            default: break;
        }
    }

    // Synthesize a `Some(inner)` construction around `slot`, typed `opt` (lower turns the Call into a
    // MakeCase). Used to wrap a bare `T` value flowing into an `Option<T>` slot (the `T?` sugar).
    void wrapSome(ExprPtr& slot, const TypeRef& opt) {
        auto call = std::make_unique<Expr>();
        call->kind = ExprKind::Call; call->pos = slot->pos; call->type = opt;
        auto callee = std::make_unique<Expr>(); callee->kind = ExprKind::Name; callee->text = "Some"; callee->pos = slot->pos;
        call->lhs = std::move(callee);
        call->args.push_back(std::move(slot));
        slot = std::move(call);
    }
    // Coerce a value into an `Option<X>` slot (the `T?`-sugar leaf rules), recursing through if/match so each
    // branch is coerced: `null` -> `None`, a bare `None` -> typed None, an already-Option value -> instantiate,
    // a bare `X` value -> `Some(value)`.
    void coerceToOptional(ExprPtr& slot, const TypeRef& opt) {
        if (!slot) return;
        if (slot->kind == ExprKind::IfExpr) { coerceToOptional(slot->rhs, opt); coerceToOptional(slot->extra, opt); slot->type = opt; return; }
        if (slot->kind == ExprKind::Match)  { for (auto& a : slot->arms) if (!a.bodyIsBlock && a.body) coerceToOptional(a.body, opt); slot->type = opt; return; }
        if (slot->kind == ExprKind::NullLit) { auto none = std::make_unique<Expr>(); none->kind = ExprKind::Name; none->text = "None"; none->pos = slot->pos; none->type = opt; slot = std::move(none); return; }
        if (slot->type.kind == TypeRef::Kind::Named && slot->type.name == "Option") { instantiateBareCases(slot, opt); return; } // already optional (or a bare None)
        wrapSome(slot, opt); // a present value of the element type
    }

    // A value flowing into a typed slot (let/assign/return/argument): widen losslessly if possible, else
    // a numeric mismatch demands an explicit cast and any other known-scalar mismatch is a type error.
    void checkConvert(ExprPtr& slot, const TypeRef& target, const std::string& ctx) {
        if (!slot) return;
        // An un-awaited async result (`Awaitable<T>`) used where a plain value is expected — e.g. `return f()`
        // inside an async fn — is the "forgot to await" mistake. Refuse it (as C#/TS do), naming the fix. §4.7
        if (isAwaitable(slot->type) && !isAwaitable(target)) {
            diags_.error(slot->pos, "this is an async result (Awaitable) — add 'await' before using it (" + ctx + ") (PRD §4.7)");
            return;
        }
        if (target.kind == TypeRef::Kind::Named && target.name == "Option" && !target.args.empty()) { coerceToOptional(slot, target); return; }
        // A list literal takes its element type from the target — so `[]` is `List<i32>`, not `List<unknown>`
        // (which emits invalid C# `List<object>`), and elements widen to the target element type. The same
        // literal coerces to a fixed-size `Array<T>` target (`var a: i32[] = [1, 2]`), which the emitter then
        // spells as an array (C# `new int[]{…}`) instead of a List.
        if (target.kind == TypeRef::Kind::Named && (target.name == "List" || target.name == "Array") &&
            !target.args.empty() && slot->kind == ExprKind::ListLit) {
            for (auto& el : slot->args) checkConvert(el, target.args[0], ctx);
            slot->type = target;
            return;
        }
        instantiateBareCases(slot, target); // a contextually-typed bare union case (`None`) takes `target`
        // G3 (wave 2): an UNSUFFIXED integer literal flowing into an i64/u64 slot is retyped to the slot's
        // width — the literal IS a 64-bit value, so targets with a distinct 64-bit literal spelling emit it
        // exactly (TS `9007199254740993n`, C# `…L`). The widening-CAST path would instead emit a runtime
        // conversion of an already-parsed narrower value (TS `BigInt(9007199254740993)` rounds through an
        // f64 before BigInt ever sees it — a silent §3.C violation). Negative literals arrive as unary
        // minus over an IntLit; retype through the minus. A suffixed literal (`5i32`) keeps its width.
        if (target.kind == TypeRef::Kind::Named && (target.name == "i64" || target.name == "u64")) {
            Expr* lit = nullptr;
            if (slot->kind == ExprKind::IntLit) lit = slot.get();
            else if (slot->kind == ExprKind::Unary && slot->text == "-" && slot->lhs &&
                     slot->lhs->kind == ExprKind::IntLit && target.name == "i64")
                lit = slot->lhs.get();
            if (lit && lit->text.find_first_of("iu") == std::string::npos &&
                literalFits64(lit->text, target.name == "u64")) {
                lit->type = target;
                slot->type = target;
                return;
            }
        }
        TypeRef from = slot->type;
        if (isLosslessWiden(from, target)) { coerce(slot, target); return; }
        if (isNumericTypeName(from) && isNumericTypeName(target) && !sameNamedType(from, target)) {
            diags_.error(slot->pos, "cannot implicitly convert " + from.name + " to " + target.name +
                                        " (" + ctx + "); add an explicit cast '(" + target.name + ")'");
            return;
        }
        Ty a = scalarTyOf(target), i = scalarTyOf(from);
        if (a != Ty::Unknown && i != Ty::Unknown && a != i)
            diags_.error(slot->pos, std::string("type mismatch (") + ctx + "): expected " + tyName(a) + ", found " + tyName(i));
        // Nominal subtyping (issue #29): a known user type flowing into a differently-named known user-type
        // slot is valid only when the source extends/implements the target — previously ANY named-type
        // mismatch passed silently and failed (or dispatched wrongly) in the generated target code.
        // Conservative: both names must be declared types (scalars, unions, enums, generic params excluded);
        // an `Iterable` target stays open (every collection/iterator flows into it).
        if (a == Ty::Unknown && i == Ty::Unknown && nominalKnown(from) && nominalKnown(target) &&
            from.name != target.name && target.name != "Iterable" && !inheritsFrom(from.name, target.name)) {
            std::string why = interfaceNames_.count(target.name)
                                  ? " — '" + from.name + "' does not declare that it implements '" + target.name + "'"
                                  : "";
            diags_.error(slot->pos, "cannot convert '" + from.name + "' to '" + target.name + "' (" + ctx + ")" + why);
        }
    }

    // A named type declared in the program (class/record/interface/extern class) — the nominal-subtyping
    // domain. Scalars and generic type params are excluded even when a member-carrier extern class (e.g.
    // `extern class i32`) puts their name in types_.
    bool nominalKnown(const TypeRef& t) const {
        return t.kind == TypeRef::Kind::Named && !t.name.empty() && types_.count(t.name) != 0 &&
               scalarTyOf(t) == Ty::Unknown && !genericsInScope_.count(t.name);
    }

    // Reconcile two numeric binary operands, widening the narrower to the wider. Returns the common type
    // (true), or false if they aren't both numeric or neither widens to the other (caller decides).
    bool reconcileNumeric(Expr& e, const TypeRef& lt, const TypeRef& rt, TypeRef& common) {
        if (!isNumericTypeName(lt) || !isNumericTypeName(rt)) return false;
        if (sameNamedType(lt, rt)) { common = lt; return true; }
        if (isLosslessWiden(lt, rt)) { coerce(e.lhs, rt); common = rt; return true; }
        if (isLosslessWiden(rt, lt)) { coerce(e.rhs, lt); common = lt; return true; }
        return false;
    }

    // Declare a pattern's bindings, typed from the matched value `scrut`: a binding takes `scrut`; a ctor
    // pattern's sub-bindings take the case's field types with the union's generics substituted from `scrut`'s
    // args (so `match o: Option<i32> { Some(v) => … }` types `v` as i32, not Unknown); tuple sub-patterns
    // take the matched element types.
    void declarePattern(const Pattern& p, const TypeRef& scrut) {
        switch (p.kind) {
            case PatKind::Binding:
                // §3.B never-miscompile: a type-annotated binding LOOKS like a runtime type test, but no
                // test is lowered — the arm would match unconditionally on every target. Until real
                // type-test patterns exist, only the scrutinee's own static type may be (redundantly) named.
                if (p.hasType && !(p.type.kind == TypeRef::Kind::Named && scrut.kind == TypeRef::Kind::Named &&
                                   p.type.name == scrut.name && p.type.nullable == scrut.nullable))
                    diags_.error(p.pos, "a typed 'match' binding ('" + p.name + ": " + sigTypeName(p.type) +
                                            "') is not a runtime type test and would match unconditionally; " +
                                            "match on a union, or branch without type narrowing");
                declare(p.name, p.hasType ? p.type : scrut, false, p.pos);
                break;
            case PatKind::Ctor: {
                std::vector<TypeRef> fieldTypes;
                if (auto uc = unionCtors_.find(p.name); uc != unionCtors_.end()) {
                    std::unordered_set<std::string> gen(uc->second.generics.begin(), uc->second.generics.end());
                    std::unordered_map<std::string, TypeRef> binds;
                    for (std::size_t i = 0; i < uc->second.generics.size() && i < scrut.args.size(); ++i) binds[uc->second.generics[i]] = scrut.args[i];
                    for (const auto& pt : uc->second.params) fieldTypes.push_back(substGeneric(pt, gen, binds));
                }
                for (std::size_t i = 0; i < p.sub.size(); ++i) declarePattern(p.sub[i], i < fieldTypes.size() ? fieldTypes[i] : tUnknown());
                break;
            }
            case PatKind::Tuple:
                // §3.B never-miscompile: tuple patterns bind + type-check here but have no lowering yet, so a
                // `match` on a tuple would emit against undefined names. Refuse cleanly until it's a real
                // feature. (`for (a, b) in …` destructuring is a separate, supported path — see declareForBinding.)
                diags_.error(p.pos, "tuple patterns in 'match' are not yet supported; destructure the tuple after "
                                    "the match, or use a `for (a, b) in …` binding");
                for (std::size_t i = 0; i < p.sub.size(); ++i) declarePattern(p.sub[i], i < scrut.args.size() ? scrut.args[i] : tUnknown());
                break;
            default: break;
        }
    }

    // Declare a for-loop binding with the iterated element type: a single binding takes `elem`; a tuple
    // pattern `(a, b)` distributes a tuple element type across its sub-bindings (else they stay unknown).
    void declareForBinding(const Pattern& p, const TypeRef& elem) {
        if (p.kind == PatKind::Binding) {
            declare(p.name, p.hasType ? p.type : elem, false, p.pos);
        } else if (p.kind == PatKind::Tuple) {
            for (std::size_t i = 0; i < p.sub.size(); ++i) {
                TypeRef sub = (elem.kind == TypeRef::Kind::Tuple && i < elem.args.size()) ? elem.args[i] : tUnknown();
                declareForBinding(p.sub[i], sub);
            }
        } else {
            declarePattern(p, elem);
        }
    }

    void checkStmt(Stmt& s) {
        switch (s.kind) {
            case StmtKind::Let: {
                TypeRef init = checkExpr(*s.value);
                if (!s.tupleNames.empty()) { // `let (a, b) = t` — declare each name from the tuple's elements (#39b)
                    for (std::size_t i = 0; i < s.tupleNames.size(); ++i) {
                        TypeRef sub = (init.kind == TypeRef::Kind::Tuple && i < init.args.size()) ? init.args[i] : tUnknown();
                        declare(s.tupleNames[i], sub, s.isMutable, s.namePos);
                    }
                    break;
                }
                if (s.hasDeclType) { normalizeOptional(s.declType, genericsInScope_); resolveTypeRef(s.declType, s.pos); checkConvert(s.value, s.declType, "initializer of '" + s.name + "'"); }
                else if (s.value && uninferableInit(*s.value, init)) // no annotation + un-inferable init: no target can reconstruct the type (issue #27)
                    diags_.error(s.pos, "cannot infer the type of '" + s.name + "' from its initializer; " + annotationHint(s.name, *s.value));
                declare(s.name, s.hasDeclType ? s.declType : init, s.isMutable, s.namePos);
                break;
            }
            case StmtKind::Assign: {
                checkExpr(*s.value);
                if (s.target && s.target->kind == ExprKind::Name) {
                    const std::string& nm = s.target->text;
                    const Local* local = lookup(nm);
                    if (!local) diags_.error(s.pos, "assignment to undeclared '" + nm + "'");
                    else {
                        if (!local->isMutable) diags_.error(s.pos, "cannot assign to immutable '" + nm + "' (declared with 'let')");
                        checkConvert(s.value, local->type, "assignment to '" + nm + "'");
                    }
                } else if (s.target) checkExpr(*s.target);
                break;
            }
            case StmtKind::ExprStmt: checkExpr(*s.value); break;
            case StmtKind::Throw:    if (s.value) checkExpr(*s.value); break;
            case StmtKind::Yield:
                if (inAsync_)
                    diags_.error(s.pos, "Polyglot refuses async iterators — a function cannot be both 'async' "
                                        "and use 'yield' (PRD §4.7)");
                if (s.value) checkExpr(*s.value);
                break;
            case StmtKind::If: {
                requireBool(checkExpr(*s.value), s.pos, "'if' condition");
                checkBlock(s.thenBody);
                if (s.hasElse) checkBlock(s.elseBody);
                break;
            }
            case StmtKind::While:
                requireBool(checkExpr(*s.value), s.pos, "'while' condition");
                checkBlock(s.thenBody);
                break;
            case StmtKind::For: {
                TypeRef iter = s.value ? checkExpr(*s.value) : tUnknown();
                // A range yields its (integer) bound type; a collection yields its element type.
                TypeRef elem = (s.value && s.value->kind == ExprKind::Range) ? iter : elementType(iter);
                pushScope();
                declareForBinding(s.forBinding, elem);
                for (auto& st : s.thenBody) checkStmt(*st);
                popScope();
                break;
            }
            case StmtKind::Use:
                if (s.value) checkExpr(*s.value);
                if (s.hasDeclType) { normalizeOptional(s.declType, genericsInScope_); resolveTypeRef(s.declType, s.pos); }
                pushScope();
                declare(s.name, s.hasDeclType ? s.declType : tUnknown(), false, s.namePos);
                for (auto& st : s.thenBody) checkStmt(*st);
                popScope();
                break;
            case StmtKind::Try:
                checkBlock(s.thenBody);
                for (auto& c : s.catches) {
                    pushScope();
                    if (!c.type.absent()) resolveTypeRef(c.type, c.pos);
                    declare(c.name, c.type, false, c.pos);
                    if (c.guard) requireBool(checkExpr(*c.guard), c.pos, "'when' guard");
                    for (auto& st : c.body) checkStmt(*st);
                    popScope();
                }
                if (s.hasFinally) checkBlock(s.finallyBody);
                break;
            case StmtKind::Return: {
                if (s.value) {
                    checkExpr(*s.value);
                    if (isUnitT(currentReturn_)) diags_.error(s.pos, "this function returns unit; 'return' takes no value");
                    else checkConvert(s.value, currentReturn_, "return value");
                } else if (!isUnitT(currentReturn_)) {
                    diags_.error(s.pos, "this function must return a value");
                }
                break;
            }
            case StmtKind::Break: case StmtKind::Continue: break;
        }
    }
    static bool isUnitT(const TypeRef& t) { return t.kind == TypeRef::Kind::Named && t.name == "unit" && !t.nullable; }
    void requireBool(const TypeRef& t, SourcePos pos, const char* what) {
        Ty s = scalarTyOf(t);
        if (s != Ty::Unknown && s != Ty::Bool) diags_.error(pos, std::string(what) + " must be bool, found " + tyName(s));
    }

    // The element type of an iterable/collection: List<T> and Iterable<T> -> T; anything else -> unknown.
    // Used to type `lst[i]` and the `for x in lst` binding.
    TypeRef elementType(const TypeRef& t) {
        if (t.kind == TypeRef::Kind::Named && (t.name == "List" || t.name == "Array" || t.name == "Iterable") && !t.args.empty())
            return t.args[0];
        // A user type with an `operator fn get(index): R` indexer: `recv[i]` is R, with the type's generics
        // substituted from `t`'s args (e.g. RingBuffer<string>[i] -> string).
        if (t.kind == TypeRef::Kind::Named) {
            if (auto it = types_.find(t.name); it != types_.end())
                for (const auto& m : it->second.members)
                    if (m.kind == MemberKind::Operator && m.name == "get") {
                        std::vector<std::string> gnames; for (const auto& g : it->second.generics) gnames.push_back(g.name);
                        std::unordered_set<std::string> gen(gnames.begin(), gnames.end());
                        std::unordered_map<std::string, TypeRef> binds;
                        for (std::size_t i = 0; i < gnames.size() && i < t.args.size(); ++i) binds[gnames[i]] = t.args[i];
                        return substGeneric(m.type, gen, binds);
                    }
        }
        return tUnknown();
    }

    // Arity-check a subscript against its target's indexer (issue #42). Native collections take exactly one
    // index; a user `operator fn get(...)` takes as many as it declares. Lenient when the receiver type is
    // unknown or has no resolvable indexer (a bare `recv[]` degrading to `0` also passes).
    void checkIndexArity(const Expr& e, const TypeRef& recv) {
        if (e.args.empty() || recv.kind != TypeRef::Kind::Named) return;
        if (recv.name == "List" || recv.name == "Array" || recv.name == "Iterable") {
            if (e.args.size() != 1)
                diags_.error(e.pos, "indexing '" + recv.name + "' takes exactly one index, got " +
                                        std::to_string(e.args.size()));
            return;
        }
        if (auto it = types_.find(recv.name); it != types_.end())
            for (const auto& m : it->second.members)
                if (m.kind == MemberKind::Operator && m.name == "get") {
                    if (e.args.size() != m.params.size())
                        diags_.error(e.pos, "indexer 'get' on '" + recv.name + "' expects " +
                                                std::to_string(m.params.size()) + " index argument(s), got " +
                                                std::to_string(e.args.size()));
                    return;
                }
    }

    // ---- expression typing ----
    // checkExpr annotates each node with its resolved type (read later by IR lowering), then returns it.
    TypeRef checkExpr(Expr& e) {
        TypeRef t = computeType(e);
        e.type = t;
        return t;
    }
    TypeRef computeType(Expr& e) {
        switch (e.kind) {
            case ExprKind::IntLit:    return tNamed(intLitType(e.text));
            case ExprKind::FloatLit:  return tNamed(floatLitType(e.text));
            case ExprKind::CharLit:   return tNamed("char");
            case ExprKind::StringLit: return tNamed("string");
            case ExprKind::InterpString: for (auto& a : e.args) checkExpr(*a); return tNamed("string");
            case ExprKind::BoolLit:   return tNamed("bool");
            case ExprKind::NullLit:   return tNullableUnknown();
            case ExprKind::This:      return currentThis_;
            case ExprKind::Super:     return tUnknown();
            case ExprKind::Name:      return checkName(e);
            case ExprKind::Unary:     return checkUnary(e);
            case ExprKind::Await:     { // `await e` unwraps `Awaitable<T>` (an async call's result) to `T` (§4.7)
                TypeRef t = checkExpr(*e.lhs);
                if (!inAsync_) { // placement error takes precedence; still unwrap so downstream typing is sane
                    diags_.error(e.pos, "'await' is only allowed inside an 'async fn' (PRD §4.7)");
                    return isAwaitable(t) && !t.args.empty() ? t.args[0] : t;
                }
                if (isAwaitable(t)) return t.args.empty() ? tUnknown() : t.args[0];
                // The only awaitables are async-call results; awaiting a plain value is a mistake. Diagnose it
                // when the operand's type is definitely known (a scalar or a nominal type), lenient otherwise.
                if (scalarTyOf(t) != Ty::Unknown || knownType(t))
                    diags_.error(e.pos, "'await' expects the result of an async call — this value is not "
                                        "awaitable (did you await a non-async call?) (PRD §4.7)");
                return t;
            }
            case ExprKind::Binary:    return checkBinary(e);
            case ExprKind::Cast:      return checkCast(e);
            case ExprKind::Extern: // raw target code — type asserted by context (§4.4 FFI)
                if (!inActual_)
                    diags_.error(e.pos, "'extern' target code is only allowed in a target-gated 'actual' — "
                                        "portable code must stay target-neutral (PRD §4.4)");
                return tUnknown();
            case ExprKind::Range:     { TypeRef lo = checkExpr(*e.lhs); checkExpr(*e.rhs); return isNumericTypeName(lo) ? lo : tNamed("i32"); }
            case ExprKind::Call:      return checkCall(e);
            case ExprKind::Member:    return checkMember(e);
            case ExprKind::Index:     { TypeRef recv = checkExpr(*e.lhs); for (auto& a : e.args) checkExpr(*a); checkIndexArity(e, recv); return elementType(recv); }
            case ExprKind::NullAssert:{
                TypeRef t = checkExpr(*e.lhs);
                if (t.kind == TypeRef::Kind::Named && t.name == "Option") { // `x!` on an optional generic: not yet lowered
                    diags_.error(e.pos, "Polyglot doesn't support '!' on an optional generic yet — use '??' or 'match'");
                    return t.args.empty() ? tUnknown() : t.args[0];
                }
                t.nullable = false; return t;
            }
            case ExprKind::Lambda:    return checkLambda(e);
            case ExprKind::ListLit:   { // `[a, b, …]` -> List<T>, T inferred from the first element
                TypeRef elem = tUnknown();
                for (auto& a : e.args) { TypeRef t = checkExpr(*a); if (elem.name.empty() && t.kind == TypeRef::Kind::Named) elem = t; }
                TypeRef list; list.kind = TypeRef::Kind::Named; list.name = "List"; list.args.push_back(elem);
                return list;
            }
            case ExprKind::TupleLit:  { TypeRef t; t.kind = TypeRef::Kind::Tuple; for (auto& a : e.args) t.args.push_back(checkExpr(*a)); return t; }
            case ExprKind::With:      { TypeRef base = checkExpr(*e.lhs); for (auto& f : e.fields) checkExpr(*f.value); return base; }
            case ExprKind::IfExpr:    { requireBool(checkExpr(*e.lhs), e.pos, "'if' condition"); TypeRef t = checkExpr(*e.rhs); checkExpr(*e.extra); return t; }
            case ExprKind::Match:     return checkMatch(e);
        }
        return tUnknown();
    }

    // A member of the enclosing class reachable by its bare name (no `this`/`Type.` qualifier): a `const`
    // or any `static` field/property. Mirrors C#/the .pg surface where a class names its own statics plainly.
    const MemberInfo* enclosingStatic(const std::string& name) const {
        if (currentClass_.empty()) return nullptr;
        const MemberInfo* m = findMember(currentClass_, name);
        if (m && (m->kind == MemberKind::Const || m->isStatic) &&
            (m->kind == MemberKind::Const || m->kind == MemberKind::Field || m->kind == MemberKind::Property))
            return m;
        return nullptr;
    }

    TypeRef checkName(Expr& e) {
        if (const Local* l = lookup(e.text)) { recordRef(e.pos, e.text, l->defId); return l->type; }
        auto v = values_.find(e.text);
        if (v != values_.end()) {
            if (auto it = valueDefId_.find(e.text); it != valueDefId_.end()) recordRef(e.pos, e.text, it->second);
            return v->second;
        }
        auto uc = unionCtors_.find(e.text); if (uc != unionCtors_.end() && uc->second.params.empty()) return uc->second.result;
        if (const MemberInfo* m = enclosingStatic(e.text)) { e.staticOwner = currentClass_; return m->type; }
        if (fns_.count(e.text) || typeNames_.count(e.text) || unionCtors_.count(e.text)) return tUnknown();
        diags_.error(e.pos, "undeclared name '" + e.text + "'");
        return tUnknown();
    }

    TypeRef checkLambda(Expr& e) {
        pushScope();
        for (const auto& p : e.params) {
            if (!p.type.absent()) resolveTypeRef(p.type, p.pos);
            declare(p.name, p.type, false, p.pos);
        }
        if (e.flag) for (auto& st : e.block) checkStmt(*st);
        else if (e.lhs) checkExpr(*e.lhs);
        popScope();
        return tUnknown();
    }

    // An explicit numeric conversion `(T)expr`. v0.1 casts are numeric<->numeric; the result is the
    // target type. Narrowing/lossy conversions are allowed *because* they're explicit. String/bool/char
    // are not numerically castable: string->number is std parsing, and char->ordinal is
    // `s.codePointAt(i)` (std.strings) — a char is a native char only on C# but a string on TS/Python/PHP,
    // so `(i32)c` cannot be honored uniformly and must refuse instead of miscompile (issue #34).
    TypeRef checkCast(Expr& e) {
        TypeRef from = checkExpr(*e.lhs);
        resolveTypeRef(e.castType, e.pos);
        Ty fs = scalarTyOf(from);
        if (isNumericTypeName(e.castType) && (fs == Ty::Bool || fs == Ty::String))
            diags_.error(e.pos, std::string("cannot cast ") + tyName(fs) + " to numeric type '" + e.castType.name + "'");
        if (isNumericTypeName(e.castType) && from.kind == TypeRef::Kind::Named && from.name == "char")
            diags_.error(e.pos, "cannot cast char to numeric type '" + e.castType.name +
                                    "' — use s.codePointAt(i) from std.strings for the ordinal");
        return e.castType;
    }

    TypeRef checkUnary(Expr& e) {
        TypeRef ot = checkExpr(*e.lhs);
        Ty o = scalarTyOf(ot);
        if (e.text == "!") {
            if (o != Ty::Unknown && o != Ty::Bool) diags_.error(e.pos, std::string("'!' expects bool, found ") + tyName(o));
            return tNamed("bool");
        }
        if (e.text == "~") return ot;
        if (o != Ty::Unknown && !isNumeric(o)) diags_.error(e.pos, std::string("unary '-' expects a numeric operand, found ") + tyName(o));
        return ot;
    }

    TypeRef checkBinary(Expr& e) {
        TypeRef lt = checkExpr(*e.lhs), rt = checkExpr(*e.rhs);
        Ty l = scalarTyOf(lt), r = scalarTyOf(rt);
        const std::string& op = e.text;

        if (op == "&&" || op == "||") {
            if ((l != Ty::Unknown && l != Ty::Bool) || (r != Ty::Unknown && r != Ty::Bool)) diags_.error(e.pos, "'" + op + "' expects bool operands");
            return tNamed("bool");
        }
        if (op == "??") { // null-coalescing
            if (lt.kind == TypeRef::Kind::Named && lt.name == "Option" && !lt.args.empty()) {
                // optional-generic: `opt ?? d` yields the element type; the fallback must produce it too.
                TypeRef elem = lt.args[0];
                checkConvert(e.rhs, elem, "'??' fallback");
                return elem;
            }
            TypeRef res = lt; res.nullable = false; // native nullable: the left's type, made non-nullable
            if (res.name.empty() && res.kind == TypeRef::Kind::Named) res = rt;
            return res;
        }
        TypeRef common;
        if (op == "==" || op == "!=") {
            if (reconcileNumeric(e, lt, rt, common)) return tNamed("bool"); // widen the narrower, then compare
            if (l != Ty::Unknown && r != Ty::Unknown && l != r) diags_.error(e.pos, "'" + op + "' compares mismatched types " + tyName(l) + " and " + tyName(r));
            return tNamed("bool");
        }
        if (op == "<" || op == "<=" || op == ">" || op == ">=") {
            if (reconcileNumeric(e, lt, rt, common)) return tNamed("bool");
            if (l != Ty::Unknown && r != Ty::Unknown && (l != r || !isNumeric(l))) diags_.error(e.pos, "'" + op + "' expects matching numeric operands, found " + tyName(l) + " and " + tyName(r));
            return tNamed("bool");
        }
        // Shifts are asymmetric (the C# rule, adopted for every target): the COUNT is a plain i32 — it
        // never reconciles with the shiftee (C# has no `long << long` / `uint << uint` operator), and
        // the result takes the SHIFTEE's type. (G4, wave 2.)
        if (op == "<<" || op == ">>") {
            if (!(isSignedInt(lt.name) || isUnsignedInt(lt.name))) {
                diags_.error(e.pos, "'" + op + "' expects an integer left operand, found " + lt.name);
                return lt;
            }
            if (rt.name != "i32") {
                if (isSignedInt(rt.name) || isUnsignedInt(rt.name))
                    checkConvert(e.rhs, tNamed("i32"), "shift count");
                else
                    diags_.error(e.pos, "'" + op + "' expects an i32 shift count, found " + rt.name);
            }
            return lt;
        }
        // arithmetic / bitwise
        if (op == "+" && l == Ty::String && r == Ty::String) return tNamed("string");
        if (reconcileNumeric(e, lt, rt, common)) return common; // widen the narrower operand to the wider
        if (isNumericTypeName(lt) && isNumericTypeName(rt)) { // both numeric but neither widens -> needs a cast
            diags_.error(e.pos, "'" + op + "' has mismatched operand types " + lt.name + " and " + rt.name + "; add an explicit cast");
            return tUnknown();
        }
        if (l != Ty::Unknown && r != Ty::Unknown && (l != r || !isNumeric(l))) { diags_.error(e.pos, "'" + op + "' expects matching numeric operands, found " + tyName(l) + " and " + tyName(r)); return tUnknown(); }
        // a user type with an operator method (e.g. Vec2 + Vec2)
        if (knownType(lt)) { if (const MemberInfo* m = findMember(lt.name, operatorMethod(op))) return m->type; }
        return tUnknown();
    }

    // ---- TypeArg inference: bind a callable's type params from the argument types, then substitute them
    // into its return type (so `fn pick<T>(a:T,b:T):T` called `pick(10i64,20i64)` returns i64, not bare T).
    void unifyGeneric(const std::unordered_set<std::string>& gen, const TypeRef& p, const TypeRef& a,
                      std::unordered_map<std::string, TypeRef>& binds) {
        if (p.kind == TypeRef::Kind::Named && p.args.empty() && gen.count(p.name)) {
            // Bind T to the argument type (first binding wins); ignore an unknown arg so we don't bind T=unknown.
            if (!binds.count(p.name) && !(a.kind == TypeRef::Kind::Named && a.name.empty())) binds[p.name] = a;
            return;
        }
        for (std::size_t i = 0; i < p.args.size() && i < a.args.size(); ++i) unifyGeneric(gen, p.args[i], a.args[i], binds);
        for (std::size_t i = 0; i < p.ret.size() && i < a.ret.size(); ++i) unifyGeneric(gen, p.ret[i], a.ret[i], binds);
    }
    TypeRef substGeneric(const TypeRef& t, const std::unordered_set<std::string>& gen,
                         const std::unordered_map<std::string, TypeRef>& binds) {
        if (t.kind == TypeRef::Kind::Named && t.args.empty() && gen.count(t.name)) {
            auto it = binds.find(t.name);
            if (it == binds.end()) return t; // unbound -> leave the type-param name (lenient, as before)
            TypeRef r = it->second;
            if (t.nullable) r.nullable = true;
            return r;
        }
        TypeRef r = t;
        for (auto& a : r.args) a = substGeneric(a, gen, binds);
        for (auto& rt : r.ret) rt = substGeneric(rt, gen, binds);
        return r;
    }
    TypeRef inferResult(const std::vector<std::string>& generics, const std::vector<TypeRef>& params,
                        const std::vector<TypeRef>& args, const TypeRef& result) {
        if (generics.empty()) return result;
        std::unordered_set<std::string> gen(generics.begin(), generics.end());
        std::unordered_map<std::string, TypeRef> binds;
        for (std::size_t i = 0; i < params.size() && i < args.size(); ++i) unifyGeneric(gen, params[i], args[i], binds);
        return substGeneric(result, gen, binds);
    }
    // Like inferResult, but also binds type params from the extension receiver (`List<T>` vs the actual
    // `List<i32>`), so an extension method's return type substitutes correctly.
    TypeRef inferExtResult(const std::vector<std::string>& generics, const TypeRef& recvDecl, const TypeRef& recvActual,
                           const std::vector<TypeRef>& params, const std::vector<TypeRef>& args, const TypeRef& result) {
        if (generics.empty()) return result;
        std::unordered_set<std::string> gen(generics.begin(), generics.end());
        std::unordered_map<std::string, TypeRef> binds;
        unifyGeneric(gen, recvDecl, recvActual, binds);
        for (std::size_t i = 0; i < params.size() && i < args.size(); ++i) unifyGeneric(gen, params[i], args[i], binds);
        return substGeneric(result, gen, binds);
    }

    // INumber enforcement: a generic param bounded by INumber must infer to a numeric type. Reject a
    // non-numeric type argument (e.g. `Math.max("a", "b")`) at Polyglot compile time rather than deferring to
    // the target (a C# overload error, or — worse — a silent TS NaN). An uninferred T is left alone (a
    // separate arity/type diagnostic covers it); so is a still-generic T (calling one generic from another).
    void checkNumericBounds(const std::vector<std::string>& numericParams, const std::vector<std::string>& generics,
                            const std::vector<TypeRef>& params, const std::vector<TypeRef>& args,
                            const std::string& what, SourcePos pos) {
        if (numericParams.empty()) return;
        std::unordered_set<std::string> gen(generics.begin(), generics.end());
        std::unordered_map<std::string, TypeRef> binds;
        for (std::size_t i = 0; i < params.size() && i < args.size(); ++i) unifyGeneric(gen, params[i], args[i], binds);
        for (const auto& n : numericParams) {
            auto it = binds.find(n);
            if (it == binds.end()) continue;
            const TypeRef& t = it->second;
            if (gen.count(t.name)) continue; // still generic (T bound to another type param) — not yet concrete
            if (t.kind == TypeRef::Kind::Named && !t.name.empty() && !isNumericTypeName(t))
                diags_.error(pos, what + " requires a numeric argument (type parameter '" + n +
                                      "' is constrained to INumber), found " + t.name);
        }
    }

    TypeRef checkMember(Expr& e) {
        // Enum case access: `EnumName.Case`.
        if (e.lhs->kind == ExprKind::Name && enumNames_.count(e.lhs->text)) return tNamed(e.lhs->text);
        // Static member access `Type.MEMBER` (e.g. `Math.PI` on the std.math extern class): the LHS is a
        // type name, so resolve the member on the type rather than evaluating the LHS as a value.
        if (e.lhs->kind == ExprKind::Name && types_.count(e.lhs->text)) {
            if (const MemberInfo* m = findMember(e.lhs->text, e.text)) {
                if (auto it = memberDefId_.find(e.lhs->text + "." + e.text); it != memberDefId_.end()) recordRef(e.pos, e.text, it->second);
                TypeRef t = m->type; if (e.flag) t.nullable = true; return t;
            }
            diags_.error(e.pos, "type '" + e.lhs->text + "' has no member '" + e.text + "'");
            return tUnknown();
        }

        TypeRef recv = checkExpr(*e.lhs);
        TypeRef result = tUnknown();
        if (knownType(recv)) {
            if (const MemberInfo* m = findMember(recv.name, e.text)) {
                result = m->type;
                if (auto it = memberDefId_.find(recv.name + "." + e.text); it != memberDefId_.end()) recordRef(e.pos, e.text, it->second);
            } else diags_.error(e.pos, "type '" + recv.name + "' has no member '" + e.text + "'");
        }
        if (e.flag) result.nullable = true; // null-safe `?.`
        return result;
    }

    // `required` is the minimum arg count (params with no default); SIZE_MAX means "all params required".
    void checkArgs(const std::vector<TypeRef>& params, const std::vector<TypeRef>& argTypes,
                   std::vector<ExprPtr>& args, const std::string& what, SourcePos pos,
                   std::size_t required = static_cast<std::size_t>(-1)) {
        if (required == static_cast<std::size_t>(-1)) required = params.size();
        if (argTypes.size() < required || argTypes.size() > params.size()) {
            std::string want = required == params.size() ? std::to_string(params.size())
                                                         : std::to_string(required) + "-" + std::to_string(params.size());
            diags_.error(pos, what + " expects " + want + " argument(s), got " + std::to_string(argTypes.size()));
            return;
        }
        // Each supplied argument flows into its parameter slot: widen losslessly, else require an explicit cast.
        for (std::size_t i = 0; i < argTypes.size(); ++i)
            checkConvert(args[i], params[i], "argument " + std::to_string(i + 1) + " of " + what);
    }

    TypeRef checkCall(Expr& e) {
        std::vector<TypeRef> argTypes;
        for (auto& a : e.args) argTypes.push_back(checkExpr(*a));

        if (e.lhs->kind == ExprKind::Member) {
            // Method call `recv.method(args)`.
            const std::string& method = e.lhs->text;
            // Static call `Type.method(args)`: the receiver is a type name, not an evaluated value.
            if (e.lhs->lhs->kind == ExprKind::Name) {
                const std::string& typeName = e.lhs->lhs->text;
                // Builtin `Math` namespace. `min`/`max`/`abs` are type-preserving (like C#'s overloads:
                // (`Math` is no longer a builtin namespace — it's the std.math `extern class`, resolved by
                // the general static-method path below; min/max/abs are generic and TypeArg-inferred.)
                // Primitive static intrinsic: `i32.parse(s)` / `f64.parse(s)` — string -> that numeric type.
                if (isNumericTypeName(tNamed(typeName)) && method == "parse") {
                    Ty a = argTypes.size() == 1 ? scalarTyOf(argTypes[0]) : Ty::Unknown;
                    if (argTypes.size() != 1 || (a != Ty::Unknown && a != Ty::String))
                        diags_.error(e.pos, "'" + typeName + ".parse' expects a single string argument");
                    return tNamed(typeName);
                }
                if (types_.count(typeName)) {
                    if (const MemberInfo* m = findMember(typeName, method); m && m->isStatic) {
                        if (auto it = memberDefId_.find(typeName + "." + method); it != memberDefId_.end()) recordRef(e.lhs->pos, method, it->second);
                        std::string what = "static method '" + typeName + "." + method + "'";
                        checkArgs(m->params, argTypes, e.args, what, e.pos, m->required);
                        checkNumericBounds(m->numericParams, m->generics, m->params, argTypes, what, e.pos);
                        TypeRef r = inferResult(m->generics, m->params, argTypes, m->type);
                        return m->isAsync ? awaitable(r) : r;
                    }
                    diags_.error(e.pos, "type '" + typeName + "' has no static method '" + method + "'");
                    return tUnknown();
                }
            }
            TypeRef recv = checkExpr(*e.lhs->lhs);
            // An extension method augments any receiver type (scalar or nominal); check it first so a
            // `type 'T' has no method` error doesn't fire for a method an extension actually supplies.
            if (recv.kind == TypeRef::Kind::Named) {
                if (auto it = extensions_.find(recv.name + "." + method); it != extensions_.end()) {
                    checkArgs(it->second.params, argTypes, e.args, "extension '" + method + "'", e.pos);
                    // Infer the extension's type params from the receiver (List<i32> vs List<T> -> T=i32) and
                    // args, then substitute the result — so `xs.secondOrNull()` is Option<i32>, not Option<T>.
                    return inferExtResult(it->second.generics, it->second.receiver, recv, it->second.params, argTypes, it->second.result);
                }
            }
            if (knownType(recv)) {
                if (const MemberInfo* m = findMember(recv.name, method)) { if (auto it = memberDefId_.find(recv.name + "." + method); it != memberDefId_.end()) recordRef(e.lhs->pos, method, it->second); checkArgs(m->params, argTypes, e.args, "method '" + method + "'", e.pos, m->required); checkNumericBounds(m->numericParams, m->generics, m->params, argTypes, "method '" + method + "'", e.pos); TypeRef r = inferResult(m->generics, m->params, argTypes, m->type); return m->isAsync ? awaitable(r) : r; }
                diags_.error(e.pos, "type '" + recv.name + "' has no method '" + method + "'");
            }
            return tUnknown();
        }
        if (e.lhs->kind != ExprKind::Name) { checkExpr(*e.lhs); return tUnknown(); }

        const std::string& name = e.lhs->text;
        // Call-syntax primitive cast `i32(x)` / `f64(n)`: the numeric scalars are extern-class prelude types
        // (so `i32.parse` resolves), which would otherwise resolve `i32(x)` as construction -> `new i32(x)`,
        // invalid in every target (issue #9 Bug 1). Rewrite it into the identical `(i32)x` Cast node so it
        // reuses the existing, correct Cast lowering/emission. Also turns the previously-silent bad forms
        // (`i32()`, `i32(a,b)`, `i32(true)`) into diagnostics.
        if (isNumericTypeName(tNamed(name))) {
            if (e.args.size() != 1) {
                diags_.error(e.pos, "cast to '" + name + "' expects a single argument");
                return tNamed(name);
            }
            Ty fs = scalarTyOf(argTypes[0]);
            if (fs == Ty::Bool || fs == Ty::String)
                diags_.error(e.pos, std::string("cannot cast ") + tyName(fs) + " to numeric type '" + name + "'");
            if (argTypes[0].kind == TypeRef::Kind::Named && argTypes[0].name == "char")
                diags_.error(e.pos, "cannot cast char to numeric type '" + name +
                                        "' — use s.codePointAt(i) from std.strings for the ordinal");
            TypeRef target = tNamed(name);
            resolveTypeRef(target, e.pos);
            e.castType = target;
            e.type = target;
            e.lhs = std::move(e.args[0]); // replace the `i32` callee with the operand; `name` now dangles
            e.args.clear();
            e.kind = ExprKind::Cast;
            return target;
        }
        // `print` is the std.io `print<T>` function (import-only), but it keeps a printable-arg guard: its
        // generic signature accepts any T, yet a non-scalar would emit divergent output (C# ToString vs JS
        // String) — a §3 miscompile. Diagnose here, then fall through to normal resolution (so it's still
        // import-gated and emits as an ordinary call to std.io.print).
        if (name == "print" && argTypes.size() == 1) {
            if (isAwaitable(argTypes[0])) // an un-awaited async result would print a Task/Promise — a §3 divergence
                diags_.error(e.pos, "cannot print the result of an async call directly — did you forget 'await'? (PRD §4.7)");
            Ty a = scalarTyOf(argTypes[0]);
            if (a != Ty::Unknown && !(isNumeric(a) || a == Ty::Bool || a == Ty::String))
                diags_.error(e.pos, std::string("print cannot print a value of type ") + tyName(a));
        }
        if (const Local* l = lookup(name)) { // calling a function-valued local (lambda/delegate); lenient on args
            recordRef(e.lhs->pos, name, l->defId);
            return l->type.kind == TypeRef::Kind::Function && !l->type.ret.empty() ? l->type.ret[0] : tUnknown();
        }
        if (auto t = types_.find(name); t != types_.end()) { // construction
            if (auto it = typeDefId_.find(name); it != typeDefId_.end()) recordRef(e.lhs->pos, name, it->second);
            if (t->second.hasCtor) checkArgs(t->second.ctorParams, argTypes, e.args, "'" + name + "'", e.pos, t->second.ctorRequired);
            TypeRef r = tNamed(name); r.args = e.typeArgs; // carry `Box<i32>()`'s args so the result is Box<i32>
            return r;
        }
        if (auto uc = unionCtors_.find(name); uc != unionCtors_.end()) {
            checkArgs(uc->second.params, argTypes, e.args, "'" + name + "'", e.pos);
            return inferResult(uc->second.generics, uc->second.params, argTypes, uc->second.result); // Some(5) -> Option<i32>
        }
        if (auto f = fns_.find(name); f != fns_.end()) {
            if (auto it = fnDefId_.find(name); it != fnDefId_.end()) recordRef(e.lhs->pos, name, it->second);
            const FnSig* chosen = resolveOverload(f->second, argTypes);
            if (!chosen) { diags_.error(e.pos, "no overload of '" + name + "' matches the argument types"); return tUnknown(); }
            checkArgs(chosen->params, argTypes, e.args, "'" + name + "'", e.pos, chosen->required);
            checkNumericBounds(chosen->numericParams, chosen->generics, chosen->params, argTypes, "'" + name + "'", e.pos);
            e.overloadName = mangleFn(name, chosen->params, f->second.size() > 1); // == name unless overloaded
            TypeRef r = inferResult(chosen->generics, chosen->params, argTypes, chosen->result);
            return chosen->isAsync ? awaitable(r) : r;
        }
        // (`Error(msg)` construction is handled by the types_ branch above — Error is a core-prelude type.)
        // A bare call inside a class body may target one of its own static methods.
        if (!currentClass_.empty()) {
            if (const MemberInfo* m = findMember(currentClass_, name);
                m && m->isStatic && (m->kind == MemberKind::Method || m->kind == MemberKind::Operator)) {
                checkArgs(m->params, argTypes, e.args, "static method '" + currentClass_ + "." + name + "'", e.pos, m->required);
                e.staticOwner = currentClass_;
                TypeRef r = inferResult(m->generics, m->params, argTypes, m->type);
                return m->isAsync ? awaitable(r) : r;
            }
        }
        diags_.error(e.pos, "call to undeclared function '" + name + "'");
        return tUnknown();
    }

    bool isCaseOf(const std::vector<std::string>& cases, const std::string& name) const {
        for (const auto& c : cases) if (c == name) return true;
        return false;
    }

    TypeRef checkMatch(Expr& e) {
        TypeRef st = checkExpr(*e.lhs);
        const std::vector<std::string>* unionCases = nullptr;
        const std::vector<std::string>* enumCases = nullptr;
        if (st.kind == TypeRef::Kind::Named && !st.nullable) {
            if (auto u = unionAllCases_.find(st.name); u != unionAllCases_.end()) unionCases = &u->second;
            else if (auto en = enumAllCases_.find(st.name); en != enumAllCases_.end()) enumCases = &en->second;
        }

        bool catchAll = false, coveredTrue = false, coveredFalse = false;
        std::unordered_set<std::string> covered;
        TypeRef result = tUnknown();

        for (auto& arm : e.arms) {
            pushScope();
            declarePattern(arm.pattern, st);
            if (arm.guard) requireBool(checkExpr(*arm.guard), arm.pattern.pos, "'match' guard");
            if (arm.bodyIsBlock) {
                // §3.B: a match arm must be an EXPRESSION. Python/PHP lower `match` to an expression form
                // (a ternary/arrow fold) with no room for a statement block, and hoisting each block arm to
                // a named function is out of scope — so a block-bodied arm is refused loudly instead of the
                // old silent drop-to-`0` (issue #51). Extract the block into a function and call it.
                diags_.error(arm.pattern.pos,
                             "Polyglot refuses a block-bodied match arm — a match arm must be an expression "
                             "(extract the block into a function and call it from the arm) (PRD §3.B)");
                for (auto& s : arm.block) checkStmt(*s); // still check the body so nested errors surface too
            }
            else if (arm.body) result = checkExpr(*arm.body);
            popScope();

            // Only unguarded arms contribute to exhaustiveness (a guard may fail at run time).
            if (arm.guard) continue;
            const Pattern& p = arm.pattern;
            switch (p.kind) {
                case PatKind::Wildcard: catchAll = true; break;
                case PatKind::Ctor: covered.insert(p.name); break;
                case PatKind::Binding:
                    if ((unionCases && isCaseOf(*unionCases, p.name)) || (enumCases && isCaseOf(*enumCases, p.name)))
                        covered.insert(p.name);
                    else catchAll = true; // a plain variable binding matches anything
                    break;
                case PatKind::Literal:
                    if (p.literal && p.literal->kind == ExprKind::BoolLit) (p.literal->boolVal ? coveredTrue : coveredFalse) = true;
                    break;
                case PatKind::Tuple: break;
            }
        }

        if (!catchAll) reportNonExhaustive(e.pos, st, unionCases, enumCases, covered, coveredTrue, coveredFalse);
        return result;
    }

    void reportNonExhaustive(SourcePos pos, const TypeRef& st,
                             const std::vector<std::string>* unionCases, const std::vector<std::string>* enumCases,
                             const std::unordered_set<std::string>& covered, bool coveredTrue, bool coveredFalse) {
        auto missingOf = [&](const std::vector<std::string>& all) {
            std::string s;
            for (const auto& c : all) if (!covered.count(c)) { if (!s.empty()) s += ", "; s += c; }
            return s;
        };
        if (unionCases) { std::string m = missingOf(*unionCases); if (!m.empty()) diags_.error(pos, "non-exhaustive match: missing case(s) " + m); return; }
        if (enumCases)  { std::string m = missingOf(*enumCases);  if (!m.empty()) diags_.error(pos, "non-exhaustive match: missing case(s) " + m); return; }
        Ty s = scalarTyOf(st);
        if (s == Ty::Bool) { if (!(coveredTrue && coveredFalse)) diags_.error(pos, "non-exhaustive match: cover both true and false (or add '_')"); return; }
        if (s != Ty::Unknown) diags_.error(pos, "non-exhaustive match: add a '_' arm"); // a known, non-enumerable type
    }
};

} // namespace

void check(CompilationUnit& unit, DiagnosticBag& diags, SemanticModel* model, const SemanticRequest* req) {
    Checker checker(diags, model, req);
    checker.run(unit);
}

} // namespace mintplayer::polyglot
