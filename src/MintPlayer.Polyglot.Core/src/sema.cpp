#include "mintplayer/polyglot/sema.hpp"

#include <string>
#include <unordered_map>
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

class Checker {
public:
    Checker(DiagnosticBag& diags) : diags_(diags) {}

    void run(CompilationUnit& unit) {
        // Collect function signatures first so calls can resolve regardless of order.
        for (const auto& fn : unit.functions) {
            if (fns_.count(fn.name)) {
                diags_.error(fn.pos, "duplicate function '" + fn.name + "'");
            }
            FnSig sig;
            for (const auto& p : fn.params) sig.params.push_back(p.type);
            sig.result = fn.returnType;
            fns_[fn.name] = sig;
        }
        for (auto& fn : unit.functions) checkFunction(fn);
    }

private:
    DiagnosticBag& diags_;
    std::unordered_map<std::string, FnSig> fns_;
    std::vector<std::unordered_map<std::string, Local>> scopes_;
    Ty currentReturn_ = Ty::Unit;

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
        currentReturn_ = fn.returnType;
        pushScope();
        for (const auto& p : fn.params) declare(p.name, p.type, /*mutable*/ false, p.pos);
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
                if (s.declType != Ty::Unknown && init != Ty::Unknown && s.declType != init) {
                    diags_.error(s.pos, std::string("type mismatch: '") + s.name + "' is declared " +
                                            tyName(s.declType) + " but initialized with " + tyName(init));
                }
                Ty declared = s.declType != Ty::Unknown ? s.declType : init;
                declare(s.name, declared, s.isMutable, s.pos);
                break;
            }
            case StmtKind::Assign: {
                const Local* local = lookup(s.name);
                Ty value = checkExpr(*s.value);
                if (!local) {
                    diags_.error(s.pos, "assignment to undeclared '" + s.name + "'");
                } else {
                    if (!local->isMutable)
                        diags_.error(s.pos, "cannot assign to immutable '" + s.name + "' (declared with 'let')");
                    if (value != Ty::Unknown && local->type != Ty::Unknown && value != local->type)
                        diags_.error(s.pos, std::string("type mismatch assigning ") + tyName(value) +
                                                " to '" + s.name + "' of type " + tyName(local->type));
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

        if (e.text == "print") {
            if (e.args.size() != 1) {
                diags_.error(e.pos, "print expects exactly one argument");
            } else {
                Ty a = e.args[0]->type;
                if (a != Ty::Unknown && !(isNumeric(a) || a == Ty::Bool || a == Ty::String))
                    diags_.error(e.pos, std::string("print cannot print a value of type ") + tyName(a));
            }
            return Ty::Unit;
        }

        auto it = fns_.find(e.text);
        if (it == fns_.end()) {
            diags_.error(e.pos, "call to undeclared function '" + e.text + "'");
            return Ty::Unknown;
        }
        const FnSig& sig = it->second;
        if (sig.params.size() != e.args.size()) {
            diags_.error(e.pos, "'" + e.text + "' expects " + std::to_string(sig.params.size()) +
                                    " argument(s), got " + std::to_string(e.args.size()));
        } else {
            for (std::size_t i = 0; i < sig.params.size(); ++i) {
                Ty got = e.args[i]->type;
                if (got != Ty::Unknown && sig.params[i] != Ty::Unknown && got != sig.params[i])
                    diags_.error(e.args[i]->pos, std::string("argument ") + std::to_string(i + 1) + " of '" +
                                                     e.text + "' expects " + tyName(sig.params[i]) +
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
