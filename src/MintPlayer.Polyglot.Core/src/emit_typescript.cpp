#include "mintplayer/polyglot/emit.hpp"

#include <string>

// Hand-written IR -> TypeScript pretty-printer for the MVP subset. Emits free `function`s, maps `print`
// -> console.log, `let`/`var` -> const/let, and appends a top-level `main();` call when present. The
// output is plain enough to run under Node's type-stripping (no enums/decorators), which the P2
// differential conformance test relies on.

namespace mintplayer::polyglot {

namespace {

std::string tsType(const TypeRef& t) {
    if (t.kind == TypeRef::Kind::Named) {
        if (t.name == "unit")               return "void";
        if (t.name == "i32" || t.name == "f64") return "number";
        if (t.name == "bool")               return "boolean";
        if (t.name == "string")             return "string";
        return t.name.empty() ? "unknown" : t.name; // user/generic type names refined in P5
    }
    return "unknown";
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
    std::string emit(const CompilationUnit& unit) {
        out_.clear();
        indent_ = 0;
        for (const auto& fn : unit.functions) emitFunction(fn);
        for (const auto& fn : unit.functions) {
            if (fn.name == "main" && fn.params.empty()) { line("main();"); break; }
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

    void emitFunction(const FunctionDecl& fn) {
        std::string sig = "function " + fn.name + "(";
        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            if (i) sig += ", ";
            sig += fn.params[i].name + ": " + tsType(fn.params[i].type);
        }
        sig += "): ";
        sig += tsType(fn.returnType);
        sig += " {";
        line(sig);
        ++indent_;
        for (const auto& s : fn.body) emitStmt(*s);
        --indent_;
        line("}");
    }

    void emitBlock(const std::vector<StmtPtr>& body) {
        ++indent_;
        for (const auto& s : body) emitStmt(*s);
        --indent_;
    }

    void emitStmt(const Stmt& s) {
        switch (s.kind) {
            case StmtKind::Let:
                line(std::string(s.isMutable ? "let " : "const ") + s.name + " = " + emitExpr(*s.value) + ";");
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
                line("if (" + emitExpr(*s.value) + ") {");
                emitBlock(s.thenBody);
                if (s.hasElse) { line("} else {"); emitBlock(s.elseBody); line("}"); }
                else line("}");
                break;
            case StmtKind::While:
                line("while (" + emitExpr(*s.value) + ") {");
                emitBlock(s.thenBody);
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
                const Expr& callee = *e.lhs;
                std::string head = (callee.kind == ExprKind::Name && callee.text == "print")
                                       ? "console.log" : emitExpr(callee);
                std::string s = head + "(";
                for (std::size_t i = 0; i < e.args.size(); ++i) {
                    if (i) s += ", ";
                    s += emitExpr(*e.args[i]);
                }
                return s + ")";
            }
            default: return ""; // expression kinds beyond the MVP subset are emitted in P5
        }
        return "";
    }
};

} // namespace

std::string emitTypeScript(const CompilationUnit& unit) {
    TypeScriptEmitter emitter;
    return emitter.emit(unit);
}

} // namespace mintplayer::polyglot
