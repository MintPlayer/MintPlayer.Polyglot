#include "mintplayer/polyglot/emit.hpp"

#include <string>

// Hand-written IR -> C# pretty-printer. Walks the typed IR; wraps the program's free functions in a
// `static class Program`, maps the `print` intrinsic -> Console.WriteLine and the entry function -> Main.

namespace mintplayer::polyglot {

namespace {

std::string csType(const TypeRef& t) {
    if (t.kind == TypeRef::Kind::Named) {
        if (t.name == "unit")   return "void";
        if (t.name == "i32")    return "int";
        if (t.name == "f64")    return "double";
        if (t.name == "bool")   return "bool";
        if (t.name == "string") return "string";
        return t.name.empty() ? "object" : t.name; // user/generic type names refined in P5
    }
    return "object";
}

int prec(const std::string& op) {
    if (op == "||") return 1;
    if (op == "&&") return 2;
    if (op == "==" || op == "!=") return 3;
    if (op == "<" || op == "<=" || op == ">" || op == ">=") return 4;
    if (op == "+" || op == "-") return 5;
    if (op == "*" || op == "/" || op == "%") return 6;
    return 7;
}

class CSharpEmitter {
public:
    std::string emit(const ir::Module& m) {
        out_ = "using System;\n\nstatic class Program\n{\n";
        indent_ = 1;
        for (const auto& fn : m.functions) emitFunction(fn);
        for (const auto& fn : m.functions) {
            if (fn.isEntry) { line(""); line("static void Main() { main(); }"); break; }
        }
        out_ += "}\n";
        return out_;
    }

private:
    std::string out_;
    int indent_ = 0;

    void line(const std::string& s) {
        out_.append(static_cast<std::size_t>(indent_) * 4, ' ');
        out_ += s;
        out_ += '\n';
    }

    void emitFunction(const ir::Function& fn) {
        std::string sig = "static " + csType(fn.returnType) + " " + fn.name + "(";
        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            if (i) sig += ", ";
            sig += csType(fn.params[i].type) + " " + fn.params[i].name;
        }
        sig += ")";
        line(sig);
        line("{");
        ++indent_;
        for (const auto& s : fn.body) emitStmt(*s);
        --indent_;
        line("}");
    }

    void emitBlock(const std::vector<ir::StmtPtr>& body) {
        line("{");
        ++indent_;
        for (const auto& s : body) emitStmt(*s);
        --indent_;
        line("}");
    }

    void emitStmt(const ir::Stmt& s) {
        switch (s.kind) {
            case ir::StmtKind::Let: {
                const auto& l = static_cast<const ir::Let&>(s);
                line("var " + l.name + " = " + emitExpr(*l.init) + ";");
                break;
            }
            case ir::StmtKind::Assign: {
                const auto& a = static_cast<const ir::Assign&>(s);
                line(emitExpr(*a.target) + " " + a.op + " " + emitExpr(*a.value) + ";");
                break;
            }
            case ir::StmtKind::ExprStmt:
                line(emitExpr(*static_cast<const ir::ExprStmt&>(s).expr) + ";");
                break;
            case ir::StmtKind::Return: {
                const auto& r = static_cast<const ir::Return&>(s);
                line(r.value ? "return " + emitExpr(*r.value) + ";" : "return;");
                break;
            }
            case ir::StmtKind::If: {
                const auto& i = static_cast<const ir::If&>(s);
                line("if (" + emitExpr(*i.cond) + ")");
                emitBlock(i.thenBody);
                if (i.hasElse) { line("else"); emitBlock(i.elseBody); }
                break;
            }
            case ir::StmtKind::While: {
                const auto& w = static_cast<const ir::While&>(s);
                line("while (" + emitExpr(*w.cond) + ")");
                emitBlock(w.body);
                break;
            }
        }
    }

    std::string escape(const std::string& v) {
        std::string s = "\"";
        for (char c : v) {
            switch (c) {
                case '\\': s += "\\\\"; break;
                case '"':  s += "\\\""; break;
                case '\n': s += "\\n"; break;
                case '\t': s += "\\t"; break;
                case '\r': s += "\\r"; break;
                default:   s += c; break;
            }
        }
        return s + "\"";
    }

    std::string child(const ir::Expr& c, int parentPrec, bool isRight) {
        std::string inner = emitExpr(c);
        if (c.kind == ir::ExprKind::Binary) {
            int cp = prec(static_cast<const ir::Binary&>(c).op);
            if (isRight ? (cp <= parentPrec) : (cp < parentPrec)) return "(" + inner + ")";
        }
        return inner;
    }

    std::string emitExpr(const ir::Expr& e) {
        switch (e.kind) {
            case ir::ExprKind::Int:   return static_cast<const ir::IntLit&>(e).text;
            case ir::ExprKind::Float: return static_cast<const ir::FloatLit&>(e).text;
            case ir::ExprKind::Bool:  return static_cast<const ir::BoolLit&>(e).value ? "true" : "false";
            case ir::ExprKind::Str:   return escape(static_cast<const ir::StrLit&>(e).value);
            case ir::ExprKind::Var:   return static_cast<const ir::Var&>(e).name;
            case ir::ExprKind::Unary: {
                const auto& u = static_cast<const ir::Unary&>(e);
                std::string operand = u.operand->kind == ir::ExprKind::Binary ? "(" + emitExpr(*u.operand) + ")"
                                                                              : emitExpr(*u.operand);
                return u.op + operand;
            }
            case ir::ExprKind::Binary: {
                const auto& b = static_cast<const ir::Binary&>(e);
                int p = prec(b.op);
                return child(*b.lhs, p, false) + " " + b.op + " " + child(*b.rhs, p, true);
            }
            case ir::ExprKind::Call: {
                const auto& c = static_cast<const ir::Call&>(e);
                std::string s = (c.isPrint ? "Console.WriteLine" : c.callee) + "(";
                for (std::size_t i = 0; i < c.args.size(); ++i) { if (i) s += ", "; s += emitExpr(*c.args[i]); }
                return s + ")";
            }
        }
        return "";
    }
};

} // namespace

std::string emitCSharp(const ir::Module& module) {
    CSharpEmitter emitter;
    return emitter.emit(module);
}

} // namespace mintplayer::polyglot
