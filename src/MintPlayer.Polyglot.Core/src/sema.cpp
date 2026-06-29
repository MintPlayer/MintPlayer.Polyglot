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

bool isBuiltinType(const std::string& n) {
    static const std::unordered_set<std::string> b = {
        "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64",
        "f32", "f64", "bool", "char", "string", "unit",
        "Iterable", // the core iterator contract (`fn …(): Iterable<T>` + `yield`); not std
        "Error",    // the core exception root (`throw`/typed `catch`); System.Exception / JS Error
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
};
struct TypeInfo {
    std::vector<GenericParam> generics;
    std::vector<MemberInfo> members;
    std::vector<TypeRef> ctorParams; // record positional fields, or class init params
    bool hasCtor = false;
    bool complete = true;            // we fully know this type's members (record/class/interface)
};
struct FnSig {
    std::vector<TypeRef> params;
    TypeRef result = namedType("unit");
};
struct Local {
    TypeRef type;
    bool isMutable;
};

class Checker {
public:
    Checker(DiagnosticBag& diags) : diags_(diags) {}

    void run(CompilationUnit& unit) {
        collectTypeNames(unit);
        buildTables(unit);
        resolveAllTypes(unit);

        for (auto& fn : unit.functions) {
            currentReturn_ = fn.returnType;
            currentThis_ = tUnknown();
            pushGenerics(fn.generics);
            pushScope();
            for (const auto& p : fn.params) declare(p.name, p.type, false, p.pos);
            checkBlock(fn.body);
            popScope();
            popGenerics(fn.generics);
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
    }

private:
    DiagnosticBag& diags_;
    std::unordered_map<std::string, FnSig> fns_;
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

    static MemberInfo memberInfo(const Member& m) {
        MemberInfo mi;
        mi.name = m.name;
        mi.kind = m.kind;
        for (const auto& p : m.params) mi.params.push_back(p.type);
        mi.type = (m.kind == MemberKind::Field || m.kind == MemberKind::Const ||
                   m.kind == MemberKind::Property) ? m.type : m.returnType;
        return mi;
    }
    void addMembers(TypeInfo& ti, const std::vector<Member>& members) {
        for (const auto& m : members) {
            ti.members.push_back(memberInfo(m));
            if (m.kind == MemberKind::Init) { ti.hasCtor = true; for (const auto& p : m.params) ti.ctorParams.push_back(p.type); }
        }
    }
    void buildTables(const CompilationUnit& u) {
        for (const auto& d : u.records) {
            TypeInfo ti; ti.generics = d.generics; ti.hasCtor = true;
            for (const auto& f : d.fields) { ti.ctorParams.push_back(f.type); ti.members.push_back({f.name, MemberKind::Field, {}, f.type}); }
            addMembers(ti, d.members);
            types_[d.name] = std::move(ti);
        }
        for (const auto& d : u.classes)    { TypeInfo ti; ti.generics = d.generics; addMembers(ti, d.members); types_[d.name] = std::move(ti); }
        for (const auto& d : u.interfaces) { TypeInfo ti; ti.generics = d.generics; addMembers(ti, d.members); types_[d.name] = std::move(ti); }
        for (const auto& d : u.unions) {
            for (const auto& c : d.cases) {
                FnSig sig; for (const auto& p : c.params) sig.params.push_back(p.type); sig.result = tNamed(d.name);
                unionCtors_[c.name] = sig;
                unionCaseOwner_[c.name] = d.name;
                unionAllCases_[d.name].push_back(c.name);
            }
        }
        for (const auto& d : u.enums)
            for (const auto& c : d.cases) enumAllCases_[d.name].push_back(c.name);
        for (const auto& fn : u.functions) {
            if (fns_.count(fn.name)) diags_.error(fn.pos, "duplicate function '" + fn.name + "'");
            FnSig sig; for (const auto& p : fn.params) sig.params.push_back(p.type); sig.result = fn.returnType;
            fns_[fn.name] = std::move(sig);
        }
        for (const auto& v : u.values) values_[v.name] = v.hasType ? v.type : tUnknown();
        for (const auto& e : u.extensions) {
            FnSig sig; for (const auto& p : e.params) sig.params.push_back(p.type); sig.result = e.returnType;
            extensions_[e.receiver.name + "." + e.name] = std::move(sig); // keyed by receiver type + method
        }
    }

    const MemberInfo* findMember(const std::string& typeName, const std::string& name) const {
        auto it = types_.find(typeName);
        if (it == types_.end()) return nullptr;
        for (const auto& m : it->second.members) if (m.name == name) return &m;
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
        for (auto& m : members) {
            pushGenerics(m.generics);
            if (m.kind == MemberKind::Method || m.kind == MemberKind::Operator || m.kind == MemberKind::Init) {
                currentReturn_ = (m.kind == MemberKind::Init) ? namedType("unit") : m.returnType;
                pushScope();
                if (recordFields) for (const auto& f : *recordFields) declare(f.name, f.type, false, f.pos);
                for (const auto& p : m.params) declare(p.name, p.type, false, p.pos);
                if (m.hasBody && m.exprBodied && m.exprBody) checkExpr(*m.exprBody);
                else if (m.hasBody) checkBlock(m.body);
                popScope();
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
        popGenerics(generics);
    }

    void checkBlock(std::vector<StmtPtr>& body) {
        pushScope();
        for (auto& s : body) checkStmt(*s);
        popScope();
    }

    void declarePattern(const Pattern& p) {
        switch (p.kind) {
            case PatKind::Binding: declare(p.name, p.hasType ? p.type : tUnknown(), false, p.pos); break;
            case PatKind::Ctor: case PatKind::Tuple: for (const auto& s : p.sub) declarePattern(s); break;
            default: break;
        }
    }

    void checkStmt(Stmt& s) {
        switch (s.kind) {
            case StmtKind::Let: {
                TypeRef init = checkExpr(*s.value);
                if (s.hasDeclType) {
                    Ty a = scalarTyOf(s.declType), i = scalarTyOf(init);
                    if (a != Ty::Unknown && i != Ty::Unknown && a != i)
                        diags_.error(s.pos, std::string("type mismatch: '") + s.name + "' is declared " +
                                                tyName(a) + " but initialized with " + tyName(i));
                }
                declare(s.name, s.hasDeclType ? s.declType : init, s.isMutable, s.pos);
                break;
            }
            case StmtKind::Assign: {
                TypeRef value = checkExpr(*s.value);
                if (s.target && s.target->kind == ExprKind::Name) {
                    const std::string& nm = s.target->text;
                    const Local* local = lookup(nm);
                    if (!local) diags_.error(s.pos, "assignment to undeclared '" + nm + "'");
                    else {
                        if (!local->isMutable) diags_.error(s.pos, "cannot assign to immutable '" + nm + "' (declared with 'let')");
                        Ty vt = scalarTyOf(value), lt = scalarTyOf(local->type);
                        if (vt != Ty::Unknown && lt != Ty::Unknown && vt != lt)
                            diags_.error(s.pos, std::string("type mismatch assigning ") + tyName(vt) + " to '" + nm + "' of type " + tyName(lt));
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
            case StmtKind::For:
                if (s.value) checkExpr(*s.value);  // iterable (std Iterable/Range — lenient)
                pushScope();
                declarePattern(s.forBinding);
                for (auto& st : s.thenBody) checkStmt(*st);
                popScope();
                break;
            case StmtKind::Use:
                if (s.value) checkExpr(*s.value);
                pushScope();
                declare(s.name, s.hasDeclType ? s.declType : tUnknown(), false, s.pos);
                for (auto& st : s.thenBody) checkStmt(*st);
                popScope();
                break;
            case StmtKind::Try:
                checkBlock(s.thenBody);
                for (auto& c : s.catches) {
                    pushScope();
                    declare(c.name, c.type, false, c.pos);
                    if (c.guard) requireBool(checkExpr(*c.guard), c.pos, "'when' guard");
                    for (auto& st : c.body) checkStmt(*st);
                    popScope();
                }
                if (s.hasFinally) checkBlock(s.finallyBody);
                break;
            case StmtKind::Return: {
                if (s.value) {
                    TypeRef got = checkExpr(*s.value);
                    Ty g = scalarTyOf(got), r = scalarTyOf(currentReturn_);
                    if (isUnitT(currentReturn_)) diags_.error(s.pos, "this function returns unit; 'return' takes no value");
                    else if (g != Ty::Unknown && r != Ty::Unknown && g != r)
                        diags_.error(s.pos, std::string("return type mismatch: expected ") + tyName(r) + ", found " + tyName(g));
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
            case ExprKind::FloatLit:  return tNamed("f64");
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
            case ExprKind::Range:     checkExpr(*e.lhs); checkExpr(*e.rhs); return tUnknown();
            case ExprKind::Call:      return checkCall(e);
            case ExprKind::Member:    return checkMember(e);
            case ExprKind::Index:     { checkExpr(*e.lhs); for (auto& a : e.args) checkExpr(*a); return tUnknown(); }
            case ExprKind::NullAssert:{ TypeRef t = checkExpr(*e.lhs); t.nullable = false; return t; }
            case ExprKind::Lambda:    return checkLambda(e);
            case ExprKind::ListLit:   for (auto& a : e.args) checkExpr(*a); return tUnknown();
            case ExprKind::TupleLit:  { TypeRef t; t.kind = TypeRef::Kind::Tuple; for (auto& a : e.args) t.args.push_back(checkExpr(*a)); return t; }
            case ExprKind::With:      { TypeRef base = checkExpr(*e.lhs); for (auto& f : e.fields) checkExpr(*f.value); return base; }
            case ExprKind::IfExpr:    { requireBool(checkExpr(*e.lhs), e.pos, "'if' condition"); TypeRef t = checkExpr(*e.rhs); checkExpr(*e.extra); return t; }
            case ExprKind::Match:     return checkMatch(e);
        }
        return tUnknown();
    }

    TypeRef checkName(Expr& e) {
        if (const Local* l = lookup(e.text)) return l->type;
        auto v = values_.find(e.text); if (v != values_.end()) return v->second;
        auto uc = unionCtors_.find(e.text); if (uc != unionCtors_.end() && uc->second.params.empty()) return uc->second.result;
        if (fns_.count(e.text) || typeNames_.count(e.text) || unionCtors_.count(e.text)) return tUnknown();
        diags_.error(e.pos, "undeclared name '" + e.text + "'");
        return tUnknown();
    }

    TypeRef checkLambda(Expr& e) {
        pushScope();
        for (const auto& p : e.params) declare(p.name, p.type, false, p.pos);
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
        if (op == "==" || op == "!=") {
            if (l != Ty::Unknown && r != Ty::Unknown && l != r) diags_.error(e.pos, "'" + op + "' compares mismatched types " + tyName(l) + " and " + tyName(r));
            return tNamed("bool");
        }
        if (op == "<" || op == "<=" || op == ">" || op == ">=") {
            if (l != Ty::Unknown && r != Ty::Unknown && (l != r || !isNumeric(l))) diags_.error(e.pos, "'" + op + "' expects matching numeric operands, found " + tyName(l) + " and " + tyName(r));
            return tNamed("bool");
        }
        // arithmetic / bitwise / shift
        if (op == "+" && l == Ty::String && r == Ty::String) return tNamed("string");
        if (l != Ty::Unknown && r != Ty::Unknown) {
            if (l != r || !isNumeric(l)) { diags_.error(e.pos, "'" + op + "' expects matching numeric operands, found " + tyName(l) + " and " + tyName(r)); return tUnknown(); }
            return lt;
        }
        // numeric widths the scalar lattice doesn't track (i64/u64/i8/…): same named numeric -> that type
        if (isNumericTypeName(lt) && sameNamedType(lt, rt)) return lt;
        // a user type with an operator method (e.g. Vec2 + Vec2)
        if (knownType(lt)) { if (const MemberInfo* m = findMember(lt.name, operatorMethod(op))) return m->type; }
        return tUnknown();
    }

    TypeRef checkMember(Expr& e) {
        // Enum case access: `EnumName.Case`.
        if (e.lhs->kind == ExprKind::Name && enumNames_.count(e.lhs->text)) return tNamed(e.lhs->text);

        TypeRef recv = checkExpr(*e.lhs);
        TypeRef result = tUnknown();
        if (knownType(recv)) {
            if (const MemberInfo* m = findMember(recv.name, e.text)) result = m->type;
            else diags_.error(e.pos, "type '" + recv.name + "' has no member '" + e.text + "'");
        }
        if (e.flag) result.nullable = true; // null-safe `?.`
        return result;
    }

    void checkArgs(const std::vector<TypeRef>& params, const std::vector<TypeRef>& argTypes,
                   const std::vector<ExprPtr>& args, const std::string& what, SourcePos pos) {
        if (params.size() != argTypes.size()) {
            diags_.error(pos, what + " expects " + std::to_string(params.size()) + " argument(s), got " + std::to_string(argTypes.size()));
            return;
        }
        for (std::size_t i = 0; i < params.size(); ++i) {
            Ty want = scalarTyOf(params[i]), got = scalarTyOf(argTypes[i]);
            if (want != Ty::Unknown && got != Ty::Unknown && want != got)
                diags_.error(args[i]->pos, "argument " + std::to_string(i + 1) + " of " + what + " expects " + tyName(want) + ", got " + tyName(got));
        }
    }

    TypeRef checkCall(Expr& e) {
        std::vector<TypeRef> argTypes;
        for (auto& a : e.args) argTypes.push_back(checkExpr(*a));

        if (e.lhs->kind == ExprKind::Member) {
            // Method call `recv.method(args)`.
            const std::string& method = e.lhs->text;
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
                if (const MemberInfo* m = findMember(recv.name, method)) { checkArgs(m->params, argTypes, e.args, "method '" + method + "'", e.pos); return m->type; }
                diags_.error(e.pos, "type '" + recv.name + "' has no method '" + method + "'");
            }
            return tUnknown();
        }
        if (e.lhs->kind != ExprKind::Name) { checkExpr(*e.lhs); return tUnknown(); }

        const std::string& name = e.lhs->text;
        if (name == "print") {
            if (argTypes.size() != 1) diags_.error(e.pos, "print expects exactly one argument");
            else { Ty a = scalarTyOf(argTypes[0]); if (a != Ty::Unknown && !(isNumeric(a) || a == Ty::Bool || a == Ty::String)) diags_.error(e.pos, std::string("print cannot print a value of type ") + tyName(a)); }
            return namedType("unit");
        }
        if (const Local* l = lookup(name)) { // calling a function-valued local (lambda/delegate); lenient on args
            return l->type.kind == TypeRef::Kind::Function && !l->type.ret.empty() ? l->type.ret[0] : tUnknown();
        }
        if (auto t = types_.find(name); t != types_.end()) { // construction
            if (t->second.hasCtor) checkArgs(t->second.ctorParams, argTypes, e.args, "'" + name + "'", e.pos);
            return tNamed(name);
        }
        if (auto uc = unionCtors_.find(name); uc != unionCtors_.end()) { checkArgs(uc->second.params, argTypes, e.args, "'" + name + "'", e.pos); return uc->second.result; }
        if (auto f = fns_.find(name); f != fns_.end()) { checkArgs(f->second.params, argTypes, e.args, "'" + name + "'", e.pos); return f->second.result; }
        if (name == "Error") return tNamed("Error"); // core exception root construction (message arg lenient)
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
