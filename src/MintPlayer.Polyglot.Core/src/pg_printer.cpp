#include "mintplayer/polyglot/pg_printer.hpp"

#include <string>

namespace mintplayer::polyglot {

namespace {

// Binary precedence, loosest = lowest (mirrors the parser ladder). Non-binary expressions return a high
// value so they never get parenthesized as operands.
int prec(const std::string& op) {
    if (op == "??") return 1;
    if (op == "||") return 2;
    if (op == "&&") return 3;
    if (op == "==" || op == "!=") return 4;
    if (op == "<" || op == "<=" || op == ">" || op == ">=") return 5;
    if (op == "|") return 6;
    if (op == "^") return 7;
    if (op == "&") return 8;
    if (op == "<<" || op == ">>" || op == ">>>") return 9;
    if (op == "+" || op == "-") return 10;
    if (op == "*" || op == "/" || op == "%") return 11;
    return 100;
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
        std::string sig = "fn " + fn.name + "(" + params(fn.params) + ")";
        if (fn.returnType != Ty::Unit) sig += std::string(": ") + tyName(fn.returnType);
        line(sig + " {");
        ++indent_;
        for (const auto& s : fn.body) printStmt(*s);
        --indent_;
        line("}");
    }

    std::string params(const std::vector<Param>& ps) {
        std::string s;
        for (std::size_t i = 0; i < ps.size(); ++i) {
            if (i) s += ", ";
            s += ps[i].name;
            if (ps[i].type != Ty::Unknown) s += std::string(": ") + tyName(ps[i].type);
        }
        return s;
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
                line(head + " = " + expr(*s.value));
                break;
            }
            case StmtKind::Assign:   line(s.name + " = " + expr(*s.value)); break;
            case StmtKind::ExprStmt: line(expr(*s.value)); break;
            case StmtKind::Return:   line(s.value ? "return " + expr(*s.value) : "return"); break;
            case StmtKind::If:
                line("if " + expr(*s.value) + " {");
                printBlock(s.thenBody);
                if (s.hasElse) { line("} else {"); printBlock(s.elseBody); line("}"); }
                else line("}");
                break;
            case StmtKind::While:
                line("while " + expr(*s.value) + " {");
                printBlock(s.thenBody);
                line("}");
                break;
        }
    }

    std::string quote(const std::string& v, char q) {
        std::string s(1, q);
        for (char c : v) {
            switch (c) {
                case '\\': s += "\\\\"; break;
                case '\n': s += "\\n"; break;
                case '\t': s += "\\t"; break;
                case '\r': s += "\\r"; break;
                default:
                    if (c == q) { s += '\\'; s += q; } else s += c;
                    break;
            }
        }
        s += q;
        return s;
    }

    // Parenthesize a binary/range child where precedence (or low-precedence kind) requires it.
    std::string operand(const Expr& c, int parentPrec, bool isRight) {
        std::string inner = expr(c);
        bool wrap = false;
        if (c.kind == ExprKind::Binary) {
            int cp = prec(c.text);
            wrap = isRight ? (cp <= parentPrec) : (cp < parentPrec);
        } else if (c.kind == ExprKind::Range || c.kind == ExprKind::IfExpr || c.kind == ExprKind::Lambda) {
            wrap = true;
        }
        return wrap ? "(" + inner + ")" : inner;
    }

    // An expression used as the target of `.`/call/index/`!` — parenthesize anything non-atomic.
    std::string atom(const Expr& e) {
        switch (e.kind) {
            case ExprKind::Name: case ExprKind::This: case ExprKind::Super:
            case ExprKind::Call: case ExprKind::Member: case ExprKind::Index:
            case ExprKind::NullAssert: case ExprKind::IntLit: case ExprKind::FloatLit:
            case ExprKind::StringLit: case ExprKind::CharLit: case ExprKind::ListLit:
            case ExprKind::TupleLit:
                return expr(e);
            default:
                return "(" + expr(e) + ")";
        }
    }

    std::string exprList(const std::vector<ExprPtr>& xs) {
        std::string s;
        for (std::size_t i = 0; i < xs.size(); ++i) { if (i) s += ", "; s += expr(*xs[i]); }
        return s;
    }

    std::string expr(const Expr& e) {
        switch (e.kind) {
            case ExprKind::IntLit:
            case ExprKind::FloatLit:  return e.text;
            case ExprKind::CharLit:   return quote(e.text, '\'');
            case ExprKind::StringLit: return quote(e.text, '"');
            case ExprKind::BoolLit:   return e.boolVal ? "true" : "false";
            case ExprKind::NullLit:   return "null";
            case ExprKind::Name:      return e.text;
            case ExprKind::This:      return "this";
            case ExprKind::Super:     return "super";
            case ExprKind::Unary: {
                const Expr& o = *e.lhs;
                bool wrap = o.kind == ExprKind::Binary || o.kind == ExprKind::Range || o.kind == ExprKind::IfExpr;
                return e.text + (wrap ? "(" + expr(o) + ")" : expr(o));
            }
            case ExprKind::Binary: {
                int p = prec(e.text);
                return operand(*e.lhs, p, false) + " " + e.text + " " + operand(*e.rhs, p, true);
            }
            case ExprKind::Range: {
                std::string op = e.flag ? "..=" : "..";
                return operand(*e.lhs, 100, false) + op + operand(*e.rhs, 100, true);
            }
            case ExprKind::Call:      return atom(*e.lhs) + "(" + exprList(e.args) + ")";
            case ExprKind::Index:     return atom(*e.lhs) + "[" + exprList(e.args) + "]";
            case ExprKind::Member:    return atom(*e.lhs) + (e.flag ? "?." : ".") + e.text;
            case ExprKind::NullAssert:return atom(*e.lhs) + "!";
            case ExprKind::ListLit:   return "[" + exprList(e.args) + "]";
            case ExprKind::TupleLit:  return "(" + exprList(e.args) + ")";
            case ExprKind::Lambda: {
                std::string s = "(" + params(e.params) + ") => ";
                if (e.flag) {
                    s += "{ ";
                    for (const auto& st : e.block) s += stmtInline(*st) + "; ";
                    s += "}";
                } else {
                    s += expr(*e.lhs);
                }
                return s;
            }
            case ExprKind::With: {
                std::string s = atom(*e.lhs) + " with { ";
                for (std::size_t i = 0; i < e.fields.size(); ++i) {
                    if (i) s += ", ";
                    s += e.fields[i].name + " = " + expr(*e.fields[i].value);
                }
                return s + " }";
            }
            case ExprKind::IfExpr:
                return "if " + expr(*e.lhs) + " { " + expr(*e.rhs) + " } else { " + expr(*e.extra) + " }";
        }
        return "";
    }

    // Single-line statement form, used only inside lambda block bodies.
    std::string stmtInline(const Stmt& s) {
        switch (s.kind) {
            case StmtKind::Let: {
                std::string head = (s.isMutable ? "var " : "let ") + s.name;
                if (s.declType != Ty::Unknown) head += std::string(": ") + tyName(s.declType);
                return head + " = " + expr(*s.value);
            }
            case StmtKind::Assign:   return s.name + " = " + expr(*s.value);
            case StmtKind::ExprStmt: return expr(*s.value);
            case StmtKind::Return:   return s.value ? "return " + expr(*s.value) : "return";
            default:                 return expr(*s.value); // if/while not expected inline in v0.1 lambdas
        }
    }
};

} // namespace

std::string printSource(const CompilationUnit& unit) {
    PgPrinter printer;
    return printer.print(unit);
}

} // namespace mintplayer::polyglot
