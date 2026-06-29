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

bool isBuiltinType(const std::string& n) {
    static const std::unordered_set<std::string> b = {
        "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64",
        "f32", "f64", "bool", "char", "string", "unit",
    };
    return b.count(n) != 0;
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
            }
        }
        for (const auto& fn : u.functions) {
            if (fns_.count(fn.name)) diags_.error(fn.pos, "duplicate function '" + fn.name + "'");
            FnSig sig; for (const auto& p : fn.params) sig.params.push_back(p.type); sig.result = fn.returnType;
            fns_[fn.name] = std::move(sig);
        }
        for (const auto& v : u.values) values_[v.name] = v.hasType ? v.type : tUnknown();
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
                if (!t.name.empty() && !isBuiltinType(t.name) &&
                    !genericsInScope_.count(t.name) && !typeNames_.count(t.name))
                    diags_.error(pos, "unknown type '" + t.name + "'");
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
    TypeRef checkExpr(Expr& e) {
        switch (e.kind) {
            case ExprKind::IntLit:    return tNamed("i32");
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
        if (auto t = types_.find(name); t != types_.end()) { // construction
            if (t->second.hasCtor) checkArgs(t->second.ctorParams, argTypes, e.args, "'" + name + "'", e.pos);
            return tNamed(name);
        }
        if (auto uc = unionCtors_.find(name); uc != unionCtors_.end()) { checkArgs(uc->second.params, argTypes, e.args, "'" + name + "'", e.pos); return uc->second.result; }
        if (auto f = fns_.find(name); f != fns_.end()) { checkArgs(f->second.params, argTypes, e.args, "'" + name + "'", e.pos); return f->second.result; }
        diags_.error(e.pos, "call to undeclared function '" + name + "'");
        return tUnknown();
    }

    TypeRef checkMatch(Expr& e) {
        checkExpr(*e.lhs);
        TypeRef result = tUnknown();
        for (auto& arm : e.arms) {
            pushScope();
            declarePattern(arm.pattern);
            if (arm.guard) requireBool(checkExpr(*arm.guard), arm.pattern.pos, "'match' guard");
            if (arm.bodyIsBlock) for (auto& st : arm.block) checkStmt(*st);
            else if (arm.body) result = checkExpr(*arm.body);
            popScope();
        }
        return result;
    }
};

} // namespace

void check(CompilationUnit& unit, DiagnosticBag& diags) {
    Checker checker(diags);
    checker.run(unit);
}

} // namespace mintplayer::polyglot
