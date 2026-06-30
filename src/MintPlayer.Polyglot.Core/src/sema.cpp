#include "mintplayer/polyglot/sema.hpp"

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
    std::size_t required = 0;     // leading params with no default — the minimum a call must supply
    std::vector<std::string> generics; // the method's own type-param names (for TypeArg inference)
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
};

// How many leading parameters a caller MUST supply: the count before the first defaulted one.
// (Defaults are trailing by convention; a non-defaulted param after a default is a latent error
// we don't diagnose here.)
std::size_t requiredCount(const std::vector<Param>& ps) {
    std::size_t n = 0;
    for (const auto& p : ps) { if (p.hasDefault) break; ++n; }
    return n;
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
};

class Checker {
public:
    Checker(DiagnosticBag& diags) : diags_(diags) {}

    void run(CompilationUnit& unit) {
        collectTypeNames(unit);
        liftExtensionGenerics(unit);
        buildTables(unit);
        resolveAllTypes(unit);

        for (auto& fn : unit.functions) {
            currentReturn_ = fn.returnType;
            currentThis_ = tUnknown();
            inActual_ = !fn.actualTarget.empty(); // only an `actual` body is a target-gated region (§4.4)
            pushGenerics(fn.generics);
            pushScope();
            for (const auto& p : fn.params) declare(p.name, p.type, false, p.pos);
            checkBlock(fn.body);
            popScope();
            popGenerics(fn.generics);
            inActual_ = false;
        }
        for (auto& d : unit.records) checkTypeBody(d.name, d.generics, d.members, &d.fields);
        for (auto& d : unit.classes) checkTypeBody(d.name, d.generics, d.members, nullptr);
        for (auto& d : unit.extensions) { // `this` denotes the receiver inside an extension body
            currentReturn_ = d.returnType;
            currentThis_ = d.receiver;
            pushGenerics(d.generics);
            pushScope();
            for (const auto& p : d.params) declare(p.name, p.type, false, p.pos);
            if (d.exprBody) checkExpr(*d.exprBody);
            else checkBlock(d.body);
            popScope();
            popGenerics(d.generics);
            currentThis_ = tUnknown();
        }
        for (auto& v : unit.values) { // type-check top-level const/let initializers (so their types propagate)
            if (!v.init) continue;
            checkExpr(*v.init);
            if (v.hasType) checkConvert(v.init, v.type, "initializer of '" + v.name + "'");
        }
    }

private:
    DiagnosticBag& diags_;
    std::unordered_map<std::string, std::vector<FnSig>> fns_; // name -> overload set
    std::unordered_map<std::string, TypeInfo> types_;
    std::unordered_set<std::string> typeNames_;
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

    // ---- scopes ----
    void pushScope() { scopes_.emplace_back(); }
    void popScope() { scopes_.pop_back(); }
    void declare(const std::string& name, TypeRef type, bool isMutable, SourcePos pos) {
        auto& top = scopes_.back();
        if (top.count(name)) diags_.error(pos, "'" + name + "' is already declared in this scope");
        top[name] = {std::move(type), isMutable};
    }
    const Local* lookup(const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) return &found->second;
        }
        return nullptr;
    }

    // ---- declaration tables ----
    void declareType(const std::string& name, SourcePos pos) {
        if (isBuiltinType(name)) diags_.error(pos, "'" + name + "' shadows a builtin type");
        else if (!typeNames_.insert(name).second) diags_.error(pos, "duplicate type '" + name + "'");
    }
    void collectTypeNames(const CompilationUnit& u) {
        for (const auto& d : u.records)    declareType(d.name, d.pos);
        for (const auto& d : u.classes)    declareType(d.name, d.pos);
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

    static MemberInfo memberInfo(const Member& m) {
        MemberInfo mi;
        mi.name = m.name;
        mi.kind = m.kind;
        for (const auto& p : m.params) mi.params.push_back(p.type);
        for (const auto& g : m.generics) mi.generics.push_back(g.name);
        mi.required = requiredCount(m.params);
        mi.type = (m.kind == MemberKind::Field || m.kind == MemberKind::Const ||
                   m.kind == MemberKind::Property) ? m.type : m.returnType;
        for (const auto& mod : m.modifiers) if (mod == "static") mi.isStatic = true;
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
        for (const auto& d : u.interfaces) { TypeInfo ti; ti.generics = d.generics; addMembers(ti, d.members); collectBaseNames(d.bases, ti.bases); types_[d.name] = std::move(ti); }
        for (const auto& d : u.unions) {
            for (const auto& c : d.cases) {
                // A union case name must be unique across the program (it's a global constructor). Two
                // cases sharing a name — within a union, across unions, or merged from two modules — is a
                // hard error, not a silent last-wins overwrite.
                if (unionCtors_.count(c.name))
                    diags_.error(c.pos, "duplicate union case '" + c.name + "'");
                FnSig sig; for (const auto& p : c.params) sig.params.push_back(p.type); sig.result = tNamed(d.name);
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
            for (const auto& g : fn.generics) sig.generics.push_back(g.name);
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
        for (const auto& d : u.unions)     for (const auto& c : d.cases) resolveParams(c.params, c.pos);
        for (const auto& d : u.extensions) { pushGenerics(d.generics); resolveBounds(d.generics, d.pos); resolveTypeRef(d.receiver, d.pos); resolveParams(d.params, d.pos); resolveTypeRef(d.returnType, d.pos); popGenerics(d.generics); }
        for (const auto& v : u.values)     if (v.hasType) resolveTypeRef(v.type, v.pos);
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
                pushScope();
                if (recordFields) for (const auto& f : *recordFields) declare(f.name, f.type, false, f.pos);
                for (const auto& p : m.params) declare(p.name, p.type, false, p.pos);
                if (m.hasBody && m.exprBodied && m.exprBody) checkExpr(*m.exprBody);
                else if (m.hasBody) checkBlock(m.body);
                popScope();
                currentThis_ = tNamed(typeName);
            } else if (m.kind == MemberKind::Property && m.init) {
                currentReturn_ = m.type;
                pushScope();
                if (recordFields) for (const auto& f : *recordFields) declare(f.name, f.type, false, f.pos);
                checkExpr(*m.init);
                popScope();
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

    // A value flowing into a typed slot (let/assign/return/argument): widen losslessly if possible, else
    // a numeric mismatch demands an explicit cast and any other known-scalar mismatch is a type error.
    void checkConvert(ExprPtr& slot, const TypeRef& target, const std::string& ctx) {
        if (!slot) return;
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

    void declarePattern(const Pattern& p) {
        switch (p.kind) {
            case PatKind::Binding: declare(p.name, p.hasType ? p.type : tUnknown(), false, p.pos); break;
            case PatKind::Ctor: case PatKind::Tuple: for (const auto& s : p.sub) declarePattern(s); break;
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
            declarePattern(p);
        }
    }

    void checkStmt(Stmt& s) {
        switch (s.kind) {
            case StmtKind::Let: {
                TypeRef init = checkExpr(*s.value);
                if (s.hasDeclType) { resolveTypeRef(s.declType, s.pos); checkConvert(s.value, s.declType, "initializer of '" + s.name + "'"); }
                declare(s.name, s.hasDeclType ? s.declType : init, s.isMutable, s.pos);
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
            case StmtKind::Yield:    if (s.value) checkExpr(*s.value); break;
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
                if (s.hasDeclType) resolveTypeRef(s.declType, s.pos);
                pushScope();
                declare(s.name, s.hasDeclType ? s.declType : tUnknown(), false, s.pos);
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
        if (t.kind == TypeRef::Kind::Named && (t.name == "List" || t.name == "Iterable") && !t.args.empty())
            return t.args[0];
        return tUnknown();
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
            case ExprKind::Index:     { TypeRef recv = checkExpr(*e.lhs); for (auto& a : e.args) checkExpr(*a); return elementType(recv); }
            case ExprKind::NullAssert:{ TypeRef t = checkExpr(*e.lhs); t.nullable = false; return t; }
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
        if (const Local* l = lookup(e.text)) return l->type;
        auto v = values_.find(e.text); if (v != values_.end()) return v->second;
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

    // An explicit numeric conversion `(T)expr`. v0.1 casts are numeric<->numeric (incl. char as a code
    // unit); the result is the target type. Narrowing/lossy conversions are allowed *because* they're
    // explicit. String/bool are not numerically castable (string->number is std parsing, a separate thing).
    TypeRef checkCast(Expr& e) {
        TypeRef from = checkExpr(*e.lhs);
        resolveTypeRef(e.castType, e.pos);
        Ty fs = scalarTyOf(from);
        if (isNumericTypeName(e.castType) && (fs == Ty::Bool || fs == Ty::String))
            diags_.error(e.pos, std::string("cannot cast ") + tyName(fs) + " to numeric type '" + e.castType.name + "'");
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
        if (op == "??") { // null-coalescing: the left's type, made non-nullable (falls back to the right)
            TypeRef res = lt; res.nullable = false;
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
        // arithmetic / bitwise / shift
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

    TypeRef checkMember(Expr& e) {
        // Enum case access: `EnumName.Case`.
        if (e.lhs->kind == ExprKind::Name && enumNames_.count(e.lhs->text)) return tNamed(e.lhs->text);
        // Static member access `Type.MEMBER` (e.g. `Math.PI` on the std.math extern class): the LHS is a
        // type name, so resolve the member on the type rather than evaluating the LHS as a value.
        if (e.lhs->kind == ExprKind::Name && types_.count(e.lhs->text)) {
            if (const MemberInfo* m = findMember(e.lhs->text, e.text)) { TypeRef t = m->type; if (e.flag) t.nullable = true; return t; }
            diags_.error(e.pos, "type '" + e.lhs->text + "' has no member '" + e.text + "'");
            return tUnknown();
        }

        TypeRef recv = checkExpr(*e.lhs);
        TypeRef result = tUnknown();
        if (knownType(recv)) {
            if (const MemberInfo* m = findMember(recv.name, e.text)) result = m->type;
            else diags_.error(e.pos, "type '" + recv.name + "' has no member '" + e.text + "'");
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
                        checkArgs(m->params, argTypes, e.args, "static method '" + typeName + "." + method + "'", e.pos, m->required);
                        return inferResult(m->generics, m->params, argTypes, m->type);
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
                    return it->second.result;
                }
            }
            if (knownType(recv)) {
                if (const MemberInfo* m = findMember(recv.name, method)) { checkArgs(m->params, argTypes, e.args, "method '" + method + "'", e.pos, m->required); return inferResult(m->generics, m->params, argTypes, m->type); }
                diags_.error(e.pos, "type '" + recv.name + "' has no method '" + method + "'");
            }
            return tUnknown();
        }
        if (e.lhs->kind != ExprKind::Name) { checkExpr(*e.lhs); return tUnknown(); }

        const std::string& name = e.lhs->text;
        // `print` is the std.io `print<T>` function (import-only), but it keeps a printable-arg guard: its
        // generic signature accepts any T, yet a non-scalar would emit divergent output (C# ToString vs JS
        // String) — a §3 miscompile. Diagnose here, then fall through to normal resolution (so it's still
        // import-gated and emits as an ordinary call to std.io.print).
        if (name == "print" && argTypes.size() == 1) {
            Ty a = scalarTyOf(argTypes[0]);
            if (a != Ty::Unknown && !(isNumeric(a) || a == Ty::Bool || a == Ty::String))
                diags_.error(e.pos, std::string("print cannot print a value of type ") + tyName(a));
        }
        if (const Local* l = lookup(name)) { // calling a function-valued local (lambda/delegate); lenient on args
            return l->type.kind == TypeRef::Kind::Function && !l->type.ret.empty() ? l->type.ret[0] : tUnknown();
        }
        if (auto t = types_.find(name); t != types_.end()) { // construction
            if (t->second.hasCtor) checkArgs(t->second.ctorParams, argTypes, e.args, "'" + name + "'", e.pos, t->second.ctorRequired);
            return tNamed(name);
        }
        if (auto uc = unionCtors_.find(name); uc != unionCtors_.end()) { checkArgs(uc->second.params, argTypes, e.args, "'" + name + "'", e.pos); return uc->second.result; }
        if (auto f = fns_.find(name); f != fns_.end()) {
            const FnSig* chosen = resolveOverload(f->second, argTypes);
            if (!chosen) { diags_.error(e.pos, "no overload of '" + name + "' matches the argument types"); return tUnknown(); }
            checkArgs(chosen->params, argTypes, e.args, "'" + name + "'", e.pos, chosen->required);
            e.overloadName = mangleFn(name, chosen->params, f->second.size() > 1); // == name unless overloaded
            return inferResult(chosen->generics, chosen->params, argTypes, chosen->result);
        }
        // (`Error(msg)` construction is handled by the types_ branch above — Error is a core-prelude type.)
        // A bare call inside a class body may target one of its own static methods.
        if (!currentClass_.empty()) {
            if (const MemberInfo* m = findMember(currentClass_, name);
                m && m->isStatic && (m->kind == MemberKind::Method || m->kind == MemberKind::Operator)) {
                checkArgs(m->params, argTypes, e.args, "static method '" + currentClass_ + "." + name + "'", e.pos, m->required);
                e.staticOwner = currentClass_;
                return inferResult(m->generics, m->params, argTypes, m->type);
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
            declarePattern(arm.pattern);
            if (arm.guard) requireBool(checkExpr(*arm.guard), arm.pattern.pos, "'match' guard");
            if (arm.bodyIsBlock) for (auto& s : arm.block) checkStmt(*s);
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

void check(CompilationUnit& unit, DiagnosticBag& diags) {
    Checker checker(diags);
    checker.run(unit);
}

} // namespace mintplayer::polyglot
