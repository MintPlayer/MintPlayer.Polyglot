#include "mintplayer/polyglot/emit.hpp"

#include <string>

// Hand-written IR -> TypeScript pretty-printer. Walks the typed IR; emits free `function`s, maps the
// `print` intrinsic -> console.log, and appends a top-level call to the entry function. Output stays
// plain enough to run under Node's type-stripping (the P2 differential conformance test relies on it).

namespace mintplayer::polyglot {

namespace {

std::string tsType(const TypeRef& t) {
    if (t.kind == TypeRef::Kind::Named) {
        if (t.name == "unit")                    return "void";
        if (t.name == "i32" || t.name == "f64")  return "number";
        if (t.name == "bool")                    return "boolean";
        if (t.name == "string")                  return "string";
        return t.name.empty() ? "unknown" : t.name; // user/generic type names refined in P5
    }
    return "unknown";
}

bool isScalarName(const std::string& n) {
    return n == "i8" || n == "i16" || n == "i32" || n == "i64" || n == "u8" || n == "u16" || n == "u32" ||
           n == "u64" || n == "f32" || n == "f64" || n == "bool" || n == "char" || n == "string" || n == "unit";
}
bool isUserType(const TypeRef& t) {
    return t.kind == TypeRef::Kind::Named && !t.name.empty() && !isScalarName(t.name);
}
// Operator symbol -> overload method name (TS has no operator overloading; call the method instead).
std::string opMethod(const std::string& op) {
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

int prec(const std::string& op) {
    if (op == "||") return 1;
    if (op == "&&") return 2;
    if (op == "==" || op == "!=") return 3;
    if (op == "<" || op == "<=" || op == ">" || op == ">=") return 4;
    if (op == "+" || op == "-") return 5;
    if (op == "*" || op == "/" || op == "%") return 6;
    return 7;
}

class TypeScriptEmitter {
public:
    std::string emit(const ir::Module& m) {
        out_.clear();
        indent_ = 0;
        for (const auto& e : m.enums) emitEnum(e);
        for (const auto& r : m.records) emitRecord(r);
        for (const auto& fn : m.functions) emitFunction(fn);
        for (const auto& fn : m.functions) {
            if (fn.isEntry) { line("main();"); break; }
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

    // Enum -> a type alias (stripped) + a const value object, both type-strippable (TS `enum` is not).
    void emitEnum(const ir::Enum& e) {
        line("type " + e.name + " = number;");
        std::string s = "const " + e.name + " = { ";
        for (std::size_t i = 0; i < e.cases.size(); ++i) { if (i) s += ", "; s += e.cases[i].name + ": " + std::to_string(e.cases[i].value); }
        line(s + " };");
    }

    // One arm of a match, lowered into the IIFE if-chain.
    std::string matchArm(const ir::MatchArm& a) {
        const ir::Pattern& p = a.pattern;
        std::string body = emitExpr(*a.body);
        if (p.kind == ir::PatternKind::Binding) {
            std::string s = "{ const " + p.binding + " = _m; ";
            s += a.guard ? "if (" + emitExpr(*a.guard) + ") return " + body + "; }" : "return " + body + "; }";
            return s + " ";
        }
        if (p.kind == ir::PatternKind::Wildcard)
            return (a.guard ? "if (" + emitExpr(*a.guard) + ") return " + body + "; " : "return " + body + "; ");
        std::string cond = "_m === " + (p.kind == ir::PatternKind::Literal ? emitExpr(*p.literal) : p.enumType + "." + p.enumCase);
        if (a.guard) cond += " && (" + emitExpr(*a.guard) + ")";
        return "if (" + cond + ") return " + body + "; ";
    }

    // Explicit fields + a constructor (not TS parameter-properties, which Node's type-stripping rejects).
    void emitRecord(const ir::Record& r) {
        line("class " + r.name + " {");
        ++indent_;
        for (const auto& f : r.fields) line(f.name + ": " + tsType(f.type) + ";");
        std::string ctor = "constructor(";
        for (std::size_t i = 0; i < r.fields.size(); ++i) { if (i) ctor += ", "; ctor += r.fields[i].name + ": " + tsType(r.fields[i].type); }
        line(ctor + ") {");
        ++indent_;
        for (const auto& f : r.fields) line("this." + f.name + " = " + f.name + ";");
        --indent_;
        line("}");
        for (const auto& m : r.methods) emitMethod(m);
        --indent_;
        line("}");
    }

    void emitMethod(const ir::Method& m) {
        if (m.kind == ir::MethodKind::Property) { // read-only computed property -> getter
            line("get " + m.name + "(): " + tsType(m.returnType) + " {");
            ++indent_;
            line("return " + emitExpr(*m.exprBody) + ";");
            --indent_;
            line("}");
            return;
        }
        // method and operator both become regular methods (a + b calls a.plus(b) at the use site)
        std::string sig = m.name + "(";
        for (std::size_t i = 0; i < m.params.size(); ++i) { if (i) sig += ", "; sig += m.params[i].name + ": " + tsType(m.params[i].type); }
        sig += "): " + tsType(m.returnType) + " {";
        line(sig);
        ++indent_;
        if (m.exprBodied) line("return " + emitExpr(*m.exprBody) + ";");
        else for (const auto& s : m.body) emitStmt(*s);
        --indent_;
        line("}");
    }

    std::string atom(const ir::Expr& e) {
        std::string s = emitExpr(e);
        if (e.kind == ir::ExprKind::Unary) return "(" + s + ")";
        // a scalar binary needs parens as a receiver; a user-type binary emits a (high-binding) method call
        if (e.kind == ir::ExprKind::Binary && !isUserType(static_cast<const ir::Binary&>(e).lhs->type)) return "(" + s + ")";
        return s;
    }

    void emitFunction(const ir::Function& fn) {
        std::string sig = "function " + fn.name + "(";
        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            if (i) sig += ", ";
            sig += fn.params[i].name + ": " + tsType(fn.params[i].type);
        }
        sig += "): " + tsType(fn.returnType) + " {";
        line(sig);
        ++indent_;
        for (const auto& s : fn.body) emitStmt(*s);
        --indent_;
        line("}");
    }

    void emitBlock(const std::vector<ir::StmtPtr>& body) {
        ++indent_;
        for (const auto& s : body) emitStmt(*s);
        --indent_;
    }

    void emitStmt(const ir::Stmt& s) {
        switch (s.kind) {
            case ir::StmtKind::Let: {
                const auto& l = static_cast<const ir::Let&>(s);
                line(std::string(l.isMutable ? "let " : "const ") + l.name + " = " + emitExpr(*l.init) + ";");
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
                line("if (" + emitExpr(*i.cond) + ") {");
                emitBlock(i.thenBody);
                if (i.hasElse) { line("} else {"); emitBlock(i.elseBody); line("}"); }
                else line("}");
                break;
            }
            case ir::StmtKind::While: {
                const auto& w = static_cast<const ir::While&>(s);
                line("while (" + emitExpr(*w.cond) + ") {");
                emitBlock(w.body);
                line("}");
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
            case ir::ExprKind::This:  return "this";
            case ir::ExprKind::Unary: {
                const auto& u = static_cast<const ir::Unary&>(e);
                std::string operand = u.operand->kind == ir::ExprKind::Binary ? "(" + emitExpr(*u.operand) + ")"
                                                                              : emitExpr(*u.operand);
                return u.op + operand;
            }
            case ir::ExprKind::Binary: {
                const auto& b = static_cast<const ir::Binary&>(e);
                if (isUserType(b.lhs->type)) { // operator overload -> method call (TS has no operators)
                    std::string method = opMethod(b.op);
                    if (!method.empty()) return atom(*b.lhs) + "." + method + "(" + emitExpr(*b.rhs) + ")";
                }
                int p = prec(b.op);
                return child(*b.lhs, p, false) + " " + b.op + " " + child(*b.rhs, p, true);
            }
            case ir::ExprKind::Call: {
                const auto& c = static_cast<const ir::Call&>(e);
                std::string s = (c.isPrint ? "console.log" : c.callee) + "(";
                for (std::size_t i = 0; i < c.args.size(); ++i) { if (i) s += ", "; s += emitExpr(*c.args[i]); }
                return s + ")";
            }
            case ir::ExprKind::Member: {
                const auto& m = static_cast<const ir::Member&>(e);
                return atom(*m.object) + (m.nullSafe ? "?." : ".") + m.field;
            }
            case ir::ExprKind::MethodCall: {
                const auto& mc = static_cast<const ir::MethodCall&>(e);
                std::string s = atom(*mc.object) + "." + mc.method + "(";
                for (std::size_t i = 0; i < mc.args.size(); ++i) { if (i) s += ", "; s += emitExpr(*mc.args[i]); }
                return s + ")";
            }
            case ir::ExprKind::New: {
                const auto& n = static_cast<const ir::New&>(e);
                std::string s = "new " + n.typeName + "(";
                for (std::size_t i = 0; i < n.args.size(); ++i) { if (i) s += ", "; s += emitExpr(*n.args[i]); }
                return s + ")";
            }
            case ir::ExprKind::Match: {
                const auto& m = static_cast<const ir::Match&>(e);
                std::string s = "(() => { const _m = " + emitExpr(*m.scrutinee) + "; ";
                for (const auto& arm : m.arms) s += matchArm(arm);
                return s + "})()";
            }
        }
        return "";
    }
};

} // namespace

std::string emitTypeScript(const ir::Module& module) {
    TypeScriptEmitter emitter;
    return emitter.emit(module);
}

} // namespace mintplayer::polyglot
