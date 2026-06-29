#include "mintplayer/polyglot/ir.hpp"

#include <string>

namespace mintplayer::polyglot::ir {

namespace {

std::string typeName(const TypeRef& t) {
    if (t.kind == TypeRef::Kind::Tuple) {
        std::string s = "(";
        for (std::size_t i = 0; i < t.args.size(); ++i) { if (i) s += ", "; s += typeName(t.args[i]); }
        return s + ")" + (t.nullable ? "?" : "");
    }
    if (t.kind == TypeRef::Kind::Function) {
        std::string s = "(";
        for (std::size_t i = 0; i < t.args.size(); ++i) { if (i) s += ", "; s += typeName(t.args[i]); }
        s += ") => " + (t.ret.empty() ? std::string("unit") : typeName(t.ret[0]));
        return s + (t.nullable ? "?" : "");
    }
    std::string s = t.name.empty() ? "?" : t.name;
    if (!t.args.empty()) {
        s += "<";
        for (std::size_t i = 0; i < t.args.size(); ++i) { if (i) s += ", "; s += typeName(t.args[i]); }
        s += ">";
    }
    return s + (t.nullable ? "?" : "");
}

class Dumper {
public:
    std::string run(const Module& m) {
        for (const auto& r : m.records) record(r);
        for (const auto& fn : m.functions) function(fn);
        return out_;
    }

    void record(const Record& r) {
        std::string s = "record " + r.name + "(";
        for (std::size_t i = 0; i < r.fields.size(); ++i) { if (i) s += ", "; s += r.fields[i].name + ": " + typeName(r.fields[i].type); }
        line(s + ")");
    }

private:
    std::string out_;
    int indent_ = 0;

    void line(const std::string& s) {
        out_.append(static_cast<std::size_t>(indent_) * 2, ' ');
        out_ += s;
        out_ += '\n';
    }

    void function(const Function& fn) {
        std::string sig = "fn " + fn.name + "(";
        for (std::size_t i = 0; i < fn.params.size(); ++i) { if (i) sig += ", "; sig += fn.params[i].name + ": " + typeName(fn.params[i].type); }
        sig += "): " + typeName(fn.returnType);
        if (fn.isEntry) sig += " [entry]";
        line(sig + " {");
        ++indent_;
        for (const auto& s : fn.body) stmt(*s);
        --indent_;
        line("}");
    }

    void block(const std::vector<StmtPtr>& body) {
        ++indent_;
        for (const auto& s : body) stmt(*s);
        --indent_;
    }

    void stmt(const Stmt& s) {
        switch (s.kind) {
            case StmtKind::Let: {
                const auto& l = static_cast<const Let&>(s);
                line(std::string(l.isMutable ? "var " : "let ") + l.name + ": " + typeName(l.type) + " = " + expr(*l.init));
                break;
            }
            case StmtKind::Assign: {
                const auto& a = static_cast<const Assign&>(s);
                line(expr(*a.target) + " " + a.op + " " + expr(*a.value));
                break;
            }
            case StmtKind::ExprStmt:
                line(expr(*static_cast<const ExprStmt&>(s).expr));
                break;
            case StmtKind::If: {
                const auto& i = static_cast<const If&>(s);
                line("if " + expr(*i.cond) + " {");
                block(i.thenBody);
                if (i.hasElse) { line("} else {"); block(i.elseBody); }
                line("}");
                break;
            }
            case StmtKind::While: {
                const auto& w = static_cast<const While&>(s);
                line("while " + expr(*w.cond) + " {");
                block(w.body);
                line("}");
                break;
            }
            case StmtKind::Return: {
                const auto& r = static_cast<const Return&>(s);
                line(r.value ? "return " + expr(*r.value) : "return");
                break;
            }
        }
    }

    // Each expression is annotated with its resolved type: `<repr>:<type>`.
    std::string expr(const Expr& e) {
        std::string repr;
        switch (e.kind) {
            case ExprKind::Int:   repr = static_cast<const IntLit&>(e).text; break;
            case ExprKind::Float: repr = static_cast<const FloatLit&>(e).text; break;
            case ExprKind::Bool:  repr = static_cast<const BoolLit&>(e).value ? "true" : "false"; break;
            case ExprKind::Str:   repr = "\"" + static_cast<const StrLit&>(e).value + "\""; break;
            case ExprKind::Var:   repr = static_cast<const Var&>(e).name; break;
            case ExprKind::This:  repr = "this"; break;
            case ExprKind::MethodCall: {
                const auto& mc = static_cast<const MethodCall&>(e);
                repr = expr(*mc.object) + "." + mc.method + "(";
                for (std::size_t i = 0; i < mc.args.size(); ++i) { if (i) repr += ", "; repr += expr(*mc.args[i]); }
                repr += ")";
                break;
            }
            case ExprKind::Unary: { const auto& u = static_cast<const Unary&>(e); repr = u.op + expr(*u.operand); break; }
            case ExprKind::Binary: {
                const auto& b = static_cast<const Binary&>(e);
                repr = "(" + expr(*b.lhs) + " " + b.op + " " + expr(*b.rhs) + ")";
                break;
            }
            case ExprKind::Call: {
                const auto& c = static_cast<const Call&>(e);
                repr = (c.isPrint ? "print" : c.callee) + "(";
                for (std::size_t i = 0; i < c.args.size(); ++i) { if (i) repr += ", "; repr += expr(*c.args[i]); }
                repr += ")";
                break;
            }
            case ExprKind::Member: {
                const auto& m = static_cast<const Member&>(e);
                repr = expr(*m.object) + (m.nullSafe ? "?." : ".") + m.field;
                break;
            }
            case ExprKind::New: {
                const auto& n = static_cast<const New&>(e);
                repr = "new " + n.typeName + "(";
                for (std::size_t i = 0; i < n.args.size(); ++i) { if (i) repr += ", "; repr += expr(*n.args[i]); }
                repr += ")";
                break;
            }
        }
        return repr + ":" + typeName(e.type);
    }
};

} // namespace

std::string dump(const Module& m) {
    Dumper d;
    return d.run(m);
}

} // namespace mintplayer::polyglot::ir
