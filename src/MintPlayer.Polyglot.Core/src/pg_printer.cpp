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
        for (const auto& im : unit.imports) line(importStr(im));
        bool first = unit.imports.empty(); // a blank line separates imports from the first declaration
        auto sep = [&]() { if (!first) out_ += "\n"; first = false; };
        for (const auto& e : unit.enums)       { sep(); printEnum(e); }
        for (const auto& u : unit.unions)      { sep(); printUnion(u); }
        for (const auto& r : unit.records)     { sep(); printRecord(r); }
        for (const auto& c : unit.classes)     { sep(); printClass(c); }
        for (const auto& i : unit.interfaces)  { sep(); printInterface(i); }
        for (const auto& x : unit.extensions)  { sep(); printExtension(x); }
        for (const auto& v : unit.values)      { sep(); printValue(v); }
        for (const auto& fn : unit.functions)  { sep(); printFunction(fn); }
        return out_;
    }

    std::string importStr(const ImportDecl& im) {
        std::string spec = quote(im.path, '"');
        if (im.isNamespace) return "import * as " + im.nsAlias + " from " + spec;
        if (im.names.empty()) return "import " + spec; // bare
        std::string s = "import { ";
        for (std::size_t i = 0; i < im.names.size(); ++i) {
            if (i) s += ", ";
            s += im.names[i].name;
            if (!im.names[i].alias.empty()) s += " as " + im.names[i].alias;
        }
        return s + " } from " + spec;
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
            if (ps[i].hasDefault) s += " = " + expr(*ps[i].defaultValue);
        }
        return s;
    }

    std::string generics(const std::vector<GenericParam>& gs) {
        if (gs.empty()) return "";
        std::string s = "<";
        for (std::size_t i = 0; i < gs.size(); ++i) {
            if (i) s += ", ";
            s += gs[i].name;
            for (std::size_t b = 0; b < gs[i].bounds.size(); ++b)
                s += (b == 0 ? ": " : " & ") + typeStr(gs[i].bounds[b]);
        }
        return s + ">";
    }

    std::string modifiers(const std::vector<std::string>& mods) {
        std::string s;
        for (const auto& m : mods) s += m + " ";
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
        line("union " + d.name + generics(d.generics) + " {");
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
        std::string sig = std::string(fn.isAsync ? "async " : "") + "fn " + fn.name +
                          generics(fn.generics) + "(" + params(fn.params) + ")";
        if (!isUnit(fn.returnType)) sig += ": " + typeStr(fn.returnType);
        line(sig + " {");
        ++indent_;
        for (const auto& s : fn.body) printStmt(*s);
        --indent_;
        line("}");
    }

    void printMember(const Member& m) {
        std::string mods = modifiers(m.modifiers);
        switch (m.kind) {
            case MemberKind::Field:
                line(mods + (m.isMutable ? "var " : "let ") + m.name + ": " + typeStr(m.type) +
                     (m.init ? " = " + expr(*m.init) : ""));
                break;
            case MemberKind::Const:
                line(mods + "const " + m.name + ": " + typeStr(m.type) + " = " + expr(*m.init));
                break;
            case MemberKind::Property:
                line(mods + (m.isMutable ? "var " : "let ") + m.name + ": " + typeStr(m.type) +
                     " => " + expr(*m.init));
                break;
            case MemberKind::Init:
                line(mods + "init(" + params(m.params) + ") {");
                printBlock(m.body);
                line("}");
                break;
            case MemberKind::Method:
            case MemberKind::Operator: {
                std::string head = mods + (m.isAsync ? "async " : "") +
                                   (m.kind == MemberKind::Operator ? "operator fn " : "fn ") +
                                   m.name + generics(m.generics) + "(" + params(m.params) + ")";
                if (!isUnit(m.returnType)) head += ": " + typeStr(m.returnType);
                if (!m.hasBody) line(head);                          // interface stub
                else if (m.exprBodied) line(head + " => " + expr(*m.exprBody));
                else { line(head + " {"); printBlock(m.body); line("}"); }
                break;
            }
        }
    }

    void printBody(const std::vector<Member>& members) {
        ++indent_;
        for (const auto& m : members) printMember(m);
        --indent_;
    }

    void printRecord(const RecordDecl& d) {
        std::string head = "record " + d.name + generics(d.generics) + "(" + params(d.fields) + ")";
        if (!d.bases.empty()) head += " : " + typeList(d.bases);
        if (d.members.empty()) { line(head); return; }
        line(head + " {");
        printBody(d.members);
        line("}");
    }

    void printClass(const ClassDecl& d) {
        std::string head = modifiers(d.modifiers) + "class " + d.name + generics(d.generics);
        if (!d.bases.empty()) head += " : " + typeList(d.bases);
        line(head + " {");
        printBody(d.members);
        line("}");
    }

    void printInterface(const InterfaceDecl& d) {
        std::string head = "interface " + d.name + generics(d.generics);
        if (!d.bases.empty()) head += " : " + typeList(d.bases);
        line(head + " {");
        printBody(d.members);
        line("}");
    }

    void printExtension(const ExtensionDecl& d) {
        std::string head = "extension fn " + typeStr(d.receiver) + "." + d.name +
                           generics(d.generics) + "(" + params(d.params) + ")";
        if (!isUnit(d.returnType)) head += ": " + typeStr(d.returnType);
        if (d.exprBodied) line(head + " => " + expr(*d.exprBody));
        else { line(head + " {"); printBlock(d.body); line("}"); }
    }

    void printValue(const ValueDecl& d) {
        std::string s = (d.isConst ? "const " : "let ") + d.name;
        if (d.hasType) s += ": " + typeStr(d.type);
        line(s + " = " + expr(*d.init));
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
            case StmtKind::Assign:   line(expr(*s.target) + " " + s.op + " " + expr(*s.value)); break;
            case StmtKind::ExprStmt: line(expr(*s.value)); break;
            case StmtKind::Throw:    line("throw " + expr(*s.value)); break;
            case StmtKind::Yield:    line("yield " + expr(*s.value)); break;
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
            case StmtKind::For:
                line("for " + patternStr(s.forBinding) + " in " + expr(*s.value) + " {");
                printBlock(s.thenBody);
                line("}");
                break;
            case StmtKind::Break:    line("break"); break;
            case StmtKind::Continue: line("continue"); break;
            case StmtKind::Use: {
                std::string head = "use " + s.name;
                if (s.hasDeclType) head += ": " + typeStr(s.declType);
                line(head + " = " + expr(*s.value) + " {");
                printBlock(s.thenBody);
                line("}");
                break;
            }
            case StmtKind::Try:
                line("try {");
                printBlock(s.thenBody);
                for (const auto& c : s.catches) {
                    std::string h = "} catch (" + c.name + ": " + typeStr(c.type) + ")";
                    if (c.guard) h += " when (" + expr(*c.guard) + ")";
                    line(h + " {");
                    printBlock(c.body);
                }
                if (s.hasFinally) { line("} finally {"); printBlock(s.finallyBody); }
                line("}");
                break;
        }
    }

    std::string escapeBody(const std::string& v, char q) {
        std::string s;
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
        return s;
    }
    std::string quote(const std::string& v, char q) {
        return std::string(1, q) + escapeBody(v, q) + std::string(1, q);
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
            case ExprKind::InterpString: {
                std::string s = "\"";
                for (std::size_t i = 0; i < e.chunks.size(); ++i) {
                    s += escapeBody(e.chunks[i], '"');
                    if (i < e.args.size()) s += "${" + expr(*e.args[i]) + "}";
                }
                return s + "\"";
            }
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
            case ExprKind::Await: {
                const Expr& o = *e.lhs;
                bool wrap = o.kind == ExprKind::Binary || o.kind == ExprKind::Range || o.kind == ExprKind::IfExpr;
                return "await " + (wrap ? "(" + expr(o) + ")" : expr(o));
            }
            case ExprKind::Binary: {
                int p = prec(e.text);
                return operand(*e.lhs, p, false) + " " + e.text + " " + operand(*e.rhs, p, true);
            }
            case ExprKind::Range: {
                std::string op = e.flag ? "..=" : "..";
                return operand(*e.lhs, 100, false) + op + operand(*e.rhs, 100, true);
            }
            case ExprKind::Call:       return atom(*e.lhs) +
                                              (e.typeArgs.empty() ? "" : "<" + typeList(e.typeArgs) + ">") +
                                              "(" + exprList(e.args) + ")";
            case ExprKind::Index:      return atom(*e.lhs) + "[" + exprList(e.args) + "]";
            case ExprKind::Member:     return atom(*e.lhs) + (e.flag ? "?." : ".") + e.text;
            case ExprKind::NullAssert: return atom(*e.lhs) + "!";
            case ExprKind::ListLit:    return "[" + exprList(e.args) + "]";
            case ExprKind::TupleLit:   return "(" + exprList(e.args) + ")";
            case ExprKind::Cast:
                return "(" + typeStr(e.castType) + ")" + atom(*e.lhs);
            case ExprKind::Extern:
                return "extern(\"" + e.text + "\")";
            case ExprKind::Lambda: {
                // Canonical: a single untyped parameter prints bare (`x => …`); anything else keeps parens.
                bool bare = e.params.size() == 1 && e.params[0].type.absent() && !e.params[0].hasDefault;
                std::string s = (bare ? e.params[0].name : "(" + params(e.params) + ")") + " => ";
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
            case StmtKind::Assign:   return expr(*s.target) + " " + s.op + " " + expr(*s.value);
            case StmtKind::ExprStmt: return expr(*s.value);
            case StmtKind::Return:   return s.value ? "return " + expr(*s.value) : "return";
            case StmtKind::Throw:    return "throw " + expr(*s.value);
            case StmtKind::Yield:    return "yield " + expr(*s.value);
            case StmtKind::Break:    return "break";
            case StmtKind::Continue: return "continue";
            case StmtKind::For:      return "for " + patternStr(s.forBinding) + " in " +
                                            expr(*s.value) + " " + blockInline(s.thenBody);
            default:                 return s.value ? expr(*s.value) : "";
        }
    }
};

} // namespace

std::string printSource(const CompilationUnit& unit) {
    PgPrinter printer;
    return printer.print(unit);
}

} // namespace mintplayer::polyglot
