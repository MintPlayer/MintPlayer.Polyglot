#include "mintplayer/polyglot/emit.hpp"

#include <string>

// Hand-written IR -> C# pretty-printer for the MVP subset. Wraps the program's free functions in a
// `static class Program`, mapping `print` -> Console.WriteLine and a `fn main()` -> the `Main` entry.

namespace mintplayer::polyglot {

namespace {

const char* csType(Ty t) {
    switch (t) {
        case Ty::Unit:   return "void";
        case Ty::I32:    return "int";
        case Ty::F64:    return "double";
        case Ty::Bool:   return "bool";
        case Ty::String: return "string";
        default:         return "object";
    }
}

// Binary-operator precedence (higher binds tighter); mirrors the parser. Used to parenthesize minimally.
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
    std::string emit(const CompilationUnit& unit) {
        out_ = "using System;\n\nstatic class Program\n{\n";
        indent_ = 1;
        for (const auto& fn : unit.functions) emitFunction(fn);
        if (const FunctionDecl* m = findMain(unit)) {
            (void)m;
            line("");
            line("static void Main() { main(); }");
        }
        out_ += "}\n";
        return out_;
    }

private:
    std::string out_;
    int indent_ = 0;

    const FunctionDecl* findMain(const CompilationUnit& unit) {
        for (const auto& fn : unit.functions)
            if (fn.name == "main" && fn.params.empty()) return &fn;
        return nullptr;
    }

    void line(const std::string& s) {
        out_.append(static_cast<std::size_t>(indent_) * 4, ' ');
        out_ += s;
        out_ += '\n';
    }

    void emitFunction(const FunctionDecl& fn) {
        std::string sig = std::string("static ") + csType(fn.returnType) + " " + fn.name + "(";
        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            if (i) sig += ", ";
            sig += std::string(csType(fn.params[i].type)) + " " + fn.params[i].name;
        }
        sig += ")";
        line(sig);
        line("{");
        ++indent_;
        for (const auto& s : fn.body) emitStmt(*s);
        --indent_;
        line("}");
    }

    void emitBlock(const std::vector<StmtPtr>& body) {
        line("{");
        ++indent_;
        for (const auto& s : body) emitStmt(*s);
        --indent_;
        line("}");
    }

    void emitStmt(const Stmt& s) {
        switch (s.kind) {
            case StmtKind::Let:
                line("var " + s.name + " = " + emitExpr(*s.value) + ";");
                break;
            case StmtKind::Assign:
                line(s.name + " = " + emitExpr(*s.value) + ";");
                break;
            case StmtKind::ExprStmt:
                line(emitExpr(*s.value) + ";");
                break;
            case StmtKind::Return:
                line(s.value ? "return " + emitExpr(*s.value) + ";" : "return;");
                break;
            case StmtKind::If:
                line("if (" + emitExpr(*s.value) + ")");
                emitBlock(s.thenBody);
                if (s.hasElse) { line("else"); emitBlock(s.elseBody); }
                break;
            case StmtKind::While:
                line("while (" + emitExpr(*s.value) + ")");
                emitBlock(s.thenBody);
                break;
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
        s += "\"";
        return s;
    }

    std::string child(const Expr& c, int parentPrec, bool isRight) {
        std::string inner = emitExpr(c);
        if (c.kind == ExprKind::Binary) {
            int cp = prec(c.text);
            bool wrap = isRight ? (cp <= parentPrec) : (cp < parentPrec);
            if (wrap) return "(" + inner + ")";
        }
        return inner;
    }

    std::string emitExpr(const Expr& e) {
        switch (e.kind) {
            case ExprKind::IntLit:    return e.text;
            case ExprKind::FloatLit:  return e.text;
            case ExprKind::BoolLit:   return e.boolVal ? "true" : "false";
            case ExprKind::StringLit: return escape(e.text);
            case ExprKind::Name:      return e.text;
            case ExprKind::Unary: {
                std::string operand = e.lhs->kind == ExprKind::Binary ? "(" + emitExpr(*e.lhs) + ")"
                                                                      : emitExpr(*e.lhs);
                return e.text + operand;
            }
            case ExprKind::Binary: {
                int p = prec(e.text);
                return child(*e.lhs, p, false) + " " + e.text + " " + child(*e.rhs, p, true);
            }
            case ExprKind::Call: {
                std::string callee = e.text == "print" ? "Console.WriteLine" : e.text;
                std::string s = callee + "(";
                for (std::size_t i = 0; i < e.args.size(); ++i) {
                    if (i) s += ", ";
                    s += emitExpr(*e.args[i]);
                }
                s += ")";
                return s;
            }
        }
        return "";
    }
};

} // namespace

std::string emitCSharp(const CompilationUnit& unit) {
    CSharpEmitter emitter;
    return emitter.emit(unit);
}

} // namespace mintplayer::polyglot
