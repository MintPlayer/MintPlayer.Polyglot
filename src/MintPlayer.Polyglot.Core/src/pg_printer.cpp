#include "mintplayer/polyglot/pg_printer.hpp"

#include <string>

namespace mintplayer::polyglot {

namespace {

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

bool isUnit(const TypeRef& t) {
    return t.kind == TypeRef::Kind::Named && t.name == "unit" && t.args.empty() && !t.nullable;
}

class PgPrinter {
public:
    std::string print(const CompilationUnit& unit) {
        out_.clear();
        bool first = true;
        auto sep = [&]() { if (!first) out_ += "\n"; first = false; };
        for (const auto& e : unit.enums)     { sep(); printEnum(e); }
        for (const auto& u : unit.unions)    { sep(); printUnion(u); }
        for (const auto& fn : unit.functions){ sep(); printFunction(fn); }
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

    std::string typeStr(const TypeRef& t) {
        std::string s;
        switch (t.kind) {
            case TypeRef::Kind::Named:
                s = t.name;
                if (!t.args.empty()) s += "<" + typeList(t.args) + ">";
                break;
            case TypeRef::Kind::Tuple:
                s = "(" + typeList(t.args) + ")";
                break;
            case TypeRef::Kind::Function:
                s = "(" + typeList(t.args) + ") => " + (t.ret.empty() ? "unit" : typeStr(t.ret[0]));
                break;
        }
        if (t.nullable) s += "?";
        return s;
    }
    std::string typeList(const std::vector<TypeRef>& ts) {
        std::string s;
        for (std::size_t i = 0; i < ts.size(); ++i) { if (i) s += ", "; s += typeStr(ts[i]); }
        return s;
    }

    std::string params(const std::vector<Param>& ps) {
        std::string s;
        for (std::size_t i = 0; i < ps.size(); ++i) {
            if (i) s += ", ";
            s += ps[i].name;
            if (!ps[i].type.absent()) s += ": " + typeStr(ps[i].type);
        }
        return s;
    }

    void printEnum(const EnumDecl& d) {
        line("enum " + d.name + " {");
        ++indent_;
        for (const auto& c : d.cases)
            line(c.name + (c.hasValue ? " = " + std::to_string(c.value) : "") + ",");
        --indent_;
        line("}");
    }

    void printUnion(const UnionDecl& d) {
        line("union " + d.name + " {");
        ++indent_;
        for (const auto& c : d.cases) {
            std::string s = c.name;
            if (!c.params.empty()) s += "(" + params(c.params) + ")";
            line(s + ",");
        }
        --indent_;
        line("}");
    }

    void printFunction(const FunctionDecl& fn) {
        std::string sig = "fn " + fn.name + "(" + params(fn.params) + ")";
        if (!isUnit(fn.returnType)) sig += ": " + typeStr(fn.returnType);
        line(sig + " {");
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
                if (s.hasDeclType) head += ": " + typeStr(s.declType);
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

    std::string operand(const Expr& c, int parentPrec, bool isRight) {
        std::string inner = expr(c);
        bool wrap = false;
        if (c.kind == ExprKind::Binary) {
            int cp = prec(c.text);
            wrap = isRight ? (cp <= parentPrec) : (cp < parentPrec);
        } else if (c.kind == ExprKind::Range || c.kind == ExprKind::IfExpr ||
                   c.kind == ExprKind::Lambda || c.kind == ExprKind::Match) {
            wrap = true;
        }
        return wrap ? "(" + inner + ")" : inner;
    }

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

    std::string blockInline(const std::vector<StmtPtr>& body) {
        std::string s = "{ ";
        for (const auto& st : body) s += stmtInline(*st) + "; ";
        return s + "}";
    }

    std::string patternStr(const Pattern& p) {
        switch (p.kind) {
            case PatKind::Wildcard: return "_";
            case PatKind::Literal:  return expr(*p.literal);
            case PatKind::Binding:  return p.name + (p.hasType ? ": " + typeStr(p.type) : "");
            case PatKind::Ctor: {
                std::string s = p.name + "(";
                for (std::size_t i = 0; i < p.sub.size(); ++i) { if (i) s += ", "; s += patternStr(p.sub[i]); }
                return s + ")";
            }
            case PatKind::Tuple: {
                std::string s = "(";
                for (std::size_t i = 0; i < p.sub.size(); ++i) { if (i) s += ", "; s += patternStr(p.sub[i]); }
                return s + ")";
            }
        }
        return "_";
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
            case ExprKind::Call:       return atom(*e.lhs) + "(" + exprList(e.args) + ")";
            case ExprKind::Index:      return atom(*e.lhs) + "[" + exprList(e.args) + "]";
            case ExprKind::Member:     return atom(*e.lhs) + (e.flag ? "?." : ".") + e.text;
            case ExprKind::NullAssert: return atom(*e.lhs) + "!";
            case ExprKind::ListLit:    return "[" + exprList(e.args) + "]";
            case ExprKind::TupleLit:   return "(" + exprList(e.args) + ")";
            case ExprKind::Lambda: {
                std::string s = "(" + params(e.params) + ") => ";
                return s + (e.flag ? blockInline(e.block) : expr(*e.lhs));
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
            case ExprKind::Match: {
                std::string s = "match " + expr(*e.lhs) + " { ";
                for (std::size_t i = 0; i < e.arms.size(); ++i) {
                    if (i) s += ", ";
                    const MatchArm& a = e.arms[i];
                    s += patternStr(a.pattern);
                    if (a.guard) s += " if " + expr(*a.guard);
                    s += " => ";
                    s += a.bodyIsBlock ? blockInline(a.block) : expr(*a.body);
                }
                return s + " }";
            }
        }
        return "";
    }

    std::string stmtInline(const Stmt& s) {
        switch (s.kind) {
            case StmtKind::Let: {
                std::string head = (s.isMutable ? "var " : "let ") + s.name;
                if (s.hasDeclType) head += ": " + typeStr(s.declType);
                return head + " = " + expr(*s.value);
            }
            case StmtKind::Assign:   return s.name + " = " + expr(*s.value);
            case StmtKind::ExprStmt: return expr(*s.value);
            case StmtKind::Return:   return s.value ? "return " + expr(*s.value) : "return";
            default:                 return expr(*s.value);
        }
    }
};

} // namespace

std::string printSource(const CompilationUnit& unit) {
    PgPrinter printer;
    return printer.print(unit);
}

} // namespace mintplayer::polyglot
