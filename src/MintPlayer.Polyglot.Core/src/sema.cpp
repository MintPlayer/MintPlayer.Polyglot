#include "mintplayer/polyglot/sema.hpp"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mintplayer::polyglot {

namespace {

struct FnSig {
    std::vector<Ty> params;
    Ty result = Ty::Unit;
};

struct Local {
    Ty type;
    bool isMutable;
};

bool isNumeric(Ty t) { return t == Ty::I32 || t == Ty::F64; }

bool isBuiltinType(const std::string& n) {
    static const std::unordered_set<std::string> b = {
        "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64",
        "f32", "f64", "bool", "char", "string", "unit",
    };
    return b.count(n) != 0;
}

class Checker {
public:
    Checker(DiagnosticBag& diags) : diags_(diags) {}

    void run(CompilationUnit& unit) {
        collectTypeNames(unit);

        // Collect function signatures first so calls can resolve regardless of order.
        for (const auto& fn : unit.functions) {
            if (fns_.count(fn.name)) {
                diags_.error(fn.pos, "duplicate function '" + fn.name + "'");
            }
            FnSig sig;
            for (const auto& p : fn.params) sig.params.push_back(scalarTyOf(p.type));
            sig.result = scalarTyOf(fn.returnType);
            fns_[fn.name] = sig;
        }

        resolveAllTypes(unit);
        for (auto& fn : unit.functions) checkFunction(fn);
    }

private:
    DiagnosticBag& diags_;
    std::unordered_map<std::string, FnSig> fns_;
    std::unordered_set<std::string> typeNames_;       // all declared type names
    std::unordered_set<std::string> genericsInScope_; // type parameters of the enclosing declaration
    std::vector<std::unordered_map<std::string, Local>> scopes_;
    Ty currentReturn_ = Ty::Unit;

    // ---- name & type resolution (P4) ----

    void declareType(const std::string& name, SourcePos pos) {
        if (isBuiltinType(name)) diags_.error(pos, "'" + name + "' shadows a builtin type");
        else if (!typeNames_.insert(name).second) diags_.error(pos, "duplicate type '" + name + "'");
    }
    void collectTypeNames(const CompilationUnit& u) {
        for (const auto& d : u.records)    declareType(d.name, d.pos);
        for (const auto& d : u.classes)    declareType(d.name, d.pos);
        for (const auto& d : u.interfaces) declareType(d.name, d.pos);
        for (const auto& d : u.enums)      declareType(d.name, d.pos);
        for (const auto& d : u.unions)     declareType(d.name, d.pos);
    }

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
    void resolveParams(const std::vector<Param>& ps, SourcePos fallback) {
        for (const auto& p : ps) if (!p.type.absent()) resolveTypeRef(p.type, p.pos.line ? p.pos : fallback);
    }
    void resolveBounds(const std::vector<GenericParam>& gs, SourcePos pos) {
        for (const auto& g : gs) for (const auto& b : g.bounds) resolveTypeRef(b, pos);
    }
    void resolveMembers(const std::vector<Member>& ms) {
        for (const auto& m : ms) {
            pushGenerics(m.generics);
            resolveBounds(m.generics, m.pos);
            if (m.kind == MemberKind::Field || m.kind == MemberKind::Const || m.kind == MemberKind::Property) {
                if (!m.type.absent()) resolveTypeRef(m.type, m.pos);
            } else {
                resolveParams(m.params, m.pos);
                resolveTypeRef(m.returnType, m.pos);
            }
            popGenerics(m.generics);
        }
    }
    void resolveAllTypes(const CompilationUnit& u) {
        for (const auto& fn : u.functions) {
            pushGenerics(fn.generics);
            resolveBounds(fn.generics, fn.pos);
            resolveParams(fn.params, fn.pos);
            resolveTypeRef(fn.returnType, fn.pos);
            popGenerics(fn.generics);
        }
        for (const auto& d : u.records) {
            pushGenerics(d.generics); resolveBounds(d.generics, d.pos);
            resolveParams(d.fields, d.pos);
            for (const auto& b : d.bases) resolveTypeRef(b, d.pos);
            resolveMembers(d.members);
            popGenerics(d.generics);
        }
        for (const auto& d : u.classes) {
            pushGenerics(d.generics); resolveBounds(d.generics, d.pos);
            for (const auto& b : d.bases) resolveTypeRef(b, d.pos);
            resolveMembers(d.members);
            popGenerics(d.generics);
        }
        for (const auto& d : u.interfaces) {
            pushGenerics(d.generics); resolveBounds(d.generics, d.pos);
            for (const auto& b : d.bases) resolveTypeRef(b, d.pos);
            resolveMembers(d.members);
            popGenerics(d.generics);
        }
        for (const auto& d : u.unions)
            for (const auto& c : d.cases) resolveParams(c.params, c.pos);
        for (const auto& d : u.extensions) {
            pushGenerics(d.generics); resolveBounds(d.generics, d.pos);
            resolveTypeRef(d.receiver, d.pos);
            resolveParams(d.params, d.pos);
            resolveTypeRef(d.returnType, d.pos);
            popGenerics(d.generics);
        }
        for (const auto& v : u.values) if (v.hasType) resolveTypeRef(v.type, v.pos);
    }

    void pushScope() { scopes_.emplace_back(); }
    void popScope() { scopes_.pop_back(); }

    void declare(const std::string& name, Ty type, bool isMutable, SourcePos pos) {
        auto& top = scopes_.back();
        if (top.count(name)) diags_.error(pos, "'" + name + "' is already declared in this scope");
        top[name] = {type, isMutable};
    }
    const Local* lookup(const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) return &found->second;
        }
        return nullptr;
    }

    void checkFunction(FunctionDecl& fn) {
        currentReturn_ = scalarTyOf(fn.returnType);
        pushScope();
        for (const auto& p : fn.params) declare(p.name, scalarTyOf(p.type), /*mutable*/ false, p.pos);
        checkBlock(fn.body);
        popScope();
    }

    void checkBlock(std::vector<StmtPtr>& body) {
        pushScope();
        for (auto& s : body) checkStmt(*s);
        popScope();
    }

    void checkStmt(Stmt& s) {
        switch (s.kind) {
            case StmtKind::Let: {
                Ty init = checkExpr(*s.value);
                Ty annotated = s.hasDeclType ? scalarTyOf(s.declType) : Ty::Unknown;
                if (s.hasDeclType && annotated != Ty::Unknown && init != Ty::Unknown && annotated != init) {
                    diags_.error(s.pos, std::string("type mismatch: '") + s.name + "' is declared " +
                                            tyName(annotated) + " but initialized with " + tyName(init));
                }
                Ty declared = s.hasDeclType ? annotated : init;
                declare(s.name, declared, s.isMutable, s.pos);
                break;
            }
            case StmtKind::Assign: {
                Ty value = checkExpr(*s.value);
                // The MVP checker validates assignments to a bare name; member/index lvalues are P4.
                if (s.target && s.target->kind == ExprKind::Name) {
                    const std::string& nm = s.target->text;
                    const Local* local = lookup(nm);
                    if (!local) {
                        diags_.error(s.pos, "assignment to undeclared '" + nm + "'");
                    } else {
                        if (!local->isMutable)
                            diags_.error(s.pos, "cannot assign to immutable '" + nm + "' (declared with 'let')");
                        if (value != Ty::Unknown && local->type != Ty::Unknown && value != local->type)
                            diags_.error(s.pos, std::string("type mismatch assigning ") + tyName(value) +
                                                    " to '" + nm + "' of type " + tyName(local->type));
                    }
                } else if (s.target) {
                    checkExpr(*s.target);
                }
                break;
            }
            case StmtKind::ExprStmt:
                checkExpr(*s.value);
                break;
            case StmtKind::If: {
                Ty cond = checkExpr(*s.value);
                if (cond != Ty::Unknown && cond != Ty::Bool)
                    diags_.error(s.pos, std::string("'if' condition must be bool, found ") + tyName(cond));
                checkBlock(s.thenBody);
                if (s.hasElse) checkBlock(s.elseBody);
                break;
            }
            case StmtKind::While: {
                Ty cond = checkExpr(*s.value);
                if (cond != Ty::Unknown && cond != Ty::Bool)
                    diags_.error(s.pos, std::string("'while' condition must be bool, found ") + tyName(cond));
                checkBlock(s.thenBody);
                break;
            }
            case StmtKind::Return: {
                if (s.value) {
                    Ty got = checkExpr(*s.value);
                    if (currentReturn_ == Ty::Unit)
                        diags_.error(s.pos, "this function returns unit; 'return' takes no value");
                    else if (got != Ty::Unknown && got != currentReturn_)
                        diags_.error(s.pos, std::string("return type mismatch: expected ") +
                                                tyName(currentReturn_) + ", found " + tyName(got));
                } else if (currentReturn_ != Ty::Unit) {
                    diags_.error(s.pos, std::string("this function must return ") + tyName(currentReturn_));
                }
                break;
            }
        }
    }

    Ty checkExpr(Expr& e) {
        switch (e.kind) {
            case ExprKind::IntLit:    e.type = Ty::I32;    break;
            case ExprKind::FloatLit:  e.type = Ty::F64;    break;
            case ExprKind::BoolLit:   e.type = Ty::Bool;   break;
            case ExprKind::StringLit: e.type = Ty::String; break;
            case ExprKind::Name: {
                const Local* local = lookup(e.text);
                if (!local) { diags_.error(e.pos, "undeclared name '" + e.text + "'"); e.type = Ty::Unknown; }
                else e.type = local->type;
                break;
            }
            case ExprKind::Unary:   e.type = checkUnary(e);  break;
            case ExprKind::Binary:  e.type = checkBinary(e); break;
            case ExprKind::Call:    e.type = checkCall(e);   break;
        }
        return e.type;
    }

    Ty checkUnary(Expr& e) {
        Ty operand = checkExpr(*e.lhs);
        if (e.text == "!") {
            if (operand != Ty::Unknown && operand != Ty::Bool)
                diags_.error(e.pos, std::string("'!' expects bool, found ") + tyName(operand));
            return Ty::Bool;
        }
        // unary '-'
        if (operand != Ty::Unknown && !isNumeric(operand))
            diags_.error(e.pos, std::string("unary '-' expects a numeric operand, found ") + tyName(operand));
        return operand;
    }

    Ty checkBinary(Expr& e) {
        Ty l = checkExpr(*e.lhs);
        Ty r = checkExpr(*e.rhs);
        const std::string& op = e.text;

        if (op == "&&" || op == "||") {
            if ((l != Ty::Unknown && l != Ty::Bool) || (r != Ty::Unknown && r != Ty::Bool))
                diags_.error(e.pos, "'" + op + "' expects bool operands");
            return Ty::Bool;
        }
        if (op == "==" || op == "!=") {
            if (l != Ty::Unknown && r != Ty::Unknown && l != r)
                diags_.error(e.pos, std::string("'") + op + "' compares mismatched types " +
                                        tyName(l) + " and " + tyName(r));
            return Ty::Bool;
        }
        if (op == "<" || op == "<=" || op == ">" || op == ">=") {
            if (l != Ty::Unknown && r != Ty::Unknown) {
                if (l != r || !isNumeric(l))
                    diags_.error(e.pos, std::string("'") + op + "' expects matching numeric operands, found " +
                                            tyName(l) + " and " + tyName(r));
            }
            return Ty::Bool;
        }
        // arithmetic: + - * / %
        if (op == "+" && l == Ty::String && r == Ty::String) return Ty::String; // string concatenation
        if (l != Ty::Unknown && r != Ty::Unknown) {
            if (l != r || !isNumeric(l)) {
                diags_.error(e.pos, std::string("'") + op + "' expects matching numeric operands, found " +
                                        tyName(l) + " and " + tyName(r));
                return Ty::Unknown;
            }
        }
        return l != Ty::Unknown ? l : r;
    }

    Ty checkCall(Expr& e) {
        for (auto& a : e.args) checkExpr(*a);

        // The MVP type checker resolves only direct calls `name(...)`. Method/other callees (Member etc.)
        // get full support in P4; here they type as Unknown rather than error.
        const std::string name = (e.lhs && e.lhs->kind == ExprKind::Name) ? e.lhs->text : "";
        if (name.empty()) return Ty::Unknown;

        if (name == "print") {
            if (e.args.size() != 1) {
                diags_.error(e.pos, "print expects exactly one argument");
            } else {
                Ty a = e.args[0]->type;
                if (a != Ty::Unknown && !(isNumeric(a) || a == Ty::Bool || a == Ty::String))
                    diags_.error(e.pos, std::string("print cannot print a value of type ") + tyName(a));
            }
            return Ty::Unit;
        }

        auto it = fns_.find(name);
        if (it == fns_.end()) {
            diags_.error(e.pos, "call to undeclared function '" + name + "'");
            return Ty::Unknown;
        }
        const FnSig& sig = it->second;
        if (sig.params.size() != e.args.size()) {
            diags_.error(e.pos, "'" + name + "' expects " + std::to_string(sig.params.size()) +
                                    " argument(s), got " + std::to_string(e.args.size()));
        } else {
            for (std::size_t i = 0; i < sig.params.size(); ++i) {
                Ty got = e.args[i]->type;
                if (got != Ty::Unknown && sig.params[i] != Ty::Unknown && got != sig.params[i])
                    diags_.error(e.args[i]->pos, std::string("argument ") + std::to_string(i + 1) + " of '" +
                                                     name + "' expects " + tyName(sig.params[i]) +
                                                     ", got " + tyName(got));
            }
        }
        return sig.result;
    }
};

} // namespace

void check(CompilationUnit& unit, DiagnosticBag& diags) {
    Checker checker(diags);
    checker.run(unit);
}

} // namespace mintplayer::polyglot
