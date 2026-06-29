#include "mintplayer/polyglot/pg_printer.hpp"

#include <string>

namespace mintplayer::polyglot {

namespace {

int prec(const std::string& op) {
    if (op == "||") return 1;
    if (op == "&&") return 2;
    if (op == "==" || op == "!=") return 3;
    if (op == "<" || op == "<=" || op == ">" || op == ">=") return 4;
    if (op == "+" || op == "-") return 5;
    if (op == "*" || op == "/" || op == "%") return 6;
    return 7;
}

class PgPrinter {
public:
    std::string print(const CompilationUnit& unit) {
        out_.clear();
        for (std::size_t i = 0; i < unit.functions.size(); ++i) {
            if (i) out_ += "\n";
            printFunction(unit.functions[i]);
        }
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

    void printFunction(const FunctionDecl& fn) {
        std::string sig = "fn " + fn.name + "(";
        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            if (i) sig += ", ";
            sig += fn.params[i].name + ": " + tyName(fn.params[i].type);
        }
        sig += ")";
        if (fn.returnType != Ty::Unit) sig += std::string(": ") + tyName(fn.returnType);
        sig += " {";
        line(sig);
        ++indent_;
        for (const auto& s : fn.body) printStmt(*s);
        --indent_;
        line("}");
    }

    void printBlock(const std::vector<StmtPtr>& body) {
        ++indent_;
        for (const auto& s : body) printStmt(*s);
        --indent_;
    }

    void printStmt(const Stmt& s) {
        switch (s.kind) {
            case StmtKind::Let: {
                std::string head = (s.isMutable ? "var " : "let ") + s.name;
                if (s.declType != Ty::Unknown) head += std::string(": ") + tyName(s.declType);
                line(head + " = " + printExpr(*s.value));
                break;
            }
            case StmtKind::Assign:
                line(s.name + " = " + printExpr(*s.value));
                break;
            case StmtKind::ExprStmt:
                line(printExpr(*s.value));
                break;
            case StmtKind::Return:
                line(s.value ? "return " + printExpr(*s.value) : "return");
                break;
            case StmtKind::If:
                line("if " + printExpr(*s.value) + " {");
                printBlock(s.thenBody);
                if (s.hasElse) { line("} else {"); printBlock(s.elseBody); line("}"); }
                else line("}");
                break;
            case StmtKind::While:
                line("while " + printExpr(*s.value) + " {");
                printBlock(s.thenBody);
                line("}");
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
        std::string inner = printExpr(c);
        if (c.kind == ExprKind::Binary) {
            int cp = prec(c.text);
            if (isRight ? (cp <= parentPrec) : (cp < parentPrec)) return "(" + inner + ")";
        }
        return inner;
    }

    std::string printExpr(const Expr& e) {
        switch (e.kind) {
            case ExprKind::IntLit:    return e.text;
            case ExprKind::FloatLit:  return e.text;
            case ExprKind::BoolLit:   return e.boolVal ? "true" : "false";
            case ExprKind::StringLit: return escape(e.text);
            case ExprKind::Name:      return e.text;
            case ExprKind::Unary: {
                std::string operand = e.lhs->kind == ExprKind::Binary ? "(" + printExpr(*e.lhs) + ")"
                                                                      : printExpr(*e.lhs);
                return e.text + operand;
            }
            case ExprKind::Binary: {
                int p = prec(e.text);
                return child(*e.lhs, p, false) + " " + e.text + " " + child(*e.rhs, p, true);
            }
            case ExprKind::Call: {
                std::string s = e.text + "(";
                for (std::size_t i = 0; i < e.args.size(); ++i) {
                    if (i) s += ", ";
                    s += printExpr(*e.args[i]);
                }
                return s + ")";
            }
        }
        return "";
    }
};

} // namespace

std::string printSource(const CompilationUnit& unit) {
    PgPrinter printer;
    return printer.print(unit);
}

} // namespace mintplayer::polyglot
