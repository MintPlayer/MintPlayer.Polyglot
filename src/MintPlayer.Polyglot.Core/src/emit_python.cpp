#include "mintplayer/polyglot/emit.hpp"

#include <string>
#include <vector>

#include "mintplayer/polyglot/backend_spec.hpp"
#include "mintplayer/polyglot/emitter_base.hpp"

// Hand-written IR -> Python pretty-printer — the THIRD backend, added to validate the P9 shared engine
// against a non-brace target (PRD §4.3; the engine was extracted from two brace-family backends, C#/TS).
// Python is colon+indent with no statement terminators, so it drives EmitterBase's BlockStyle::ColonIndent +
// stmtEnd "" (see emitter_base.hpp). Scope today: the walking-skeleton subset — free functions, arithmetic,
// let/var, if/while/for, return, calls, and `print` (std.io's python `actual`). Declarations beyond that
// (records/unions/classes/enums/extensions) and the faithfulness machinery (int overflow masking, etc.) are
// not emitted yet; the PythonBackend capability set (backend.cpp) gates the advanced §3.A surface off so any
// use targeting Python is refused, never miscompiled.

namespace mintplayer::polyglot {
namespace {

bool isIntType(const TypeRef& t) {
    if (t.kind != TypeRef::Kind::Named) return false;
    const std::string& n = t.name;
    return n == "i8" || n == "i16" || n == "i32" || n == "i64" ||
           n == "u8" || n == "u16" || n == "u32" || n == "u64";
}

class PythonEmitter : public EmitterBase {
public:
    std::string emit(const ir::Module& m) {
        out_.clear();
        indent_ = 0;
        for (const auto& g : m.globals)
            line(g.name + " = " + (g.init ? emitExpr(*g.init) : "None"));
        for (const auto& fn : m.functions) {
            if (!fn.actualTarget.empty() && fn.actualTarget != "python") continue; // other target's `actual`
            emitFunction(fn);
        }
        for (const auto& fn : m.functions)
            if (fn.isEntry) { line(fn.mangledName + "()"); break; } // top-level entry call
        return out_;
    }

private:
    BlockStyle blockStyle() const override { return BlockStyle::ColonIndent; }
    const char* stmtEnd() const override { return ""; }                                 // Python: no terminator
    std::string localDecl(const std::string& name, bool) override { return name; }       // bare `name = ...`
    std::string yieldStmt(const std::string& v, bool has) override { return has ? "yield " + v : "return"; }
    std::string rethrowStmt() override { return "raise"; } // unexercised: Exceptions gated off for Python

    void emitFunction(const ir::Function& fn) {
        std::string sig = "def " + fn.mangledName + "(";
        for (std::size_t i = 0; i < fn.params.size(); ++i) { if (i) sig += ", "; sig += fn.params[i].name; }
        sig += ")";
        if (fn.exprBodied) {
            openBlock(sig);
            ++indent_;
            bool unit = fn.returnType.kind == TypeRef::Kind::Named && (fn.returnType.name == "unit" || fn.returnType.name.empty());
            line(unit ? emitExpr(*fn.exprBody) : "return " + emitExpr(*fn.exprBody));
            --indent_;
        } else {
            headBlock(sig, fn.body);
        }
    }

    void emitStmtTarget(const ir::Stmt& s) override {
        // For is the only non-shared statement the skeleton needs (Try is gated off via Exceptions).
        if (s.kind == ir::StmtKind::For) {
            const auto& f = static_cast<const ir::For&>(s);
            if (f.isRange) {
                std::string hi = f.inclusive ? "(" + emitExpr(*f.rangeEnd) + ") + 1" : emitExpr(*f.rangeEnd);
                headBlock("for " + f.binding + " in range(" + emitExpr(*f.rangeStart) + ", " + hi + ")", f.body);
            } else if (!f.tupleBindings.empty()) {
                std::string names;
                for (std::size_t i = 0; i < f.tupleBindings.size(); ++i) { if (i) names += ", "; names += f.tupleBindings[i]; }
                headBlock("for " + names + " in " + emitExpr(*f.iterable), f.body);
            } else {
                headBlock("for " + f.binding + " in " + emitExpr(*f.iterable), f.body);
            }
            return;
        }
        line("# polyglot: statement kind not yet supported for the python target");
    }

    // Parenthesize a binary child that binds looser than its parent (Python precedence matches C/JS here).
    std::string child(const ir::Expr& c, int parentPrec, bool isRight) {
        std::string inner = emitExpr(c);
        if (c.kind == ir::ExprKind::Binary) {
            int cp = operatorPrecedence(static_cast<const ir::Binary&>(c).op);
            if (isRight ? (cp <= parentPrec) : (cp < parentPrec)) return "(" + inner + ")";
        }
        return inner;
    }

    // Python operator spelling: `and`/`or` for `&&`/`||`; integer `/` is `//` (Python `/` is always float).
    std::string pyOp(const std::string& op, const TypeRef& resultType) {
        if (op == "&&") return "and";
        if (op == "||") return "or";
        if (op == "/" && isIntType(resultType)) return "//";
        return op;
    }

    std::string emitExpr(const ir::Expr& e) override {
        switch (e.kind) {
            case ir::ExprKind::Int:    return static_cast<const ir::IntLit&>(e).text;
            case ir::ExprKind::Float:  return static_cast<const ir::FloatLit&>(e).text;
            case ir::ExprKind::Bool:   return static_cast<const ir::BoolLit&>(e).value ? "True" : "False";
            case ir::ExprKind::Null:   return "None";
            case ir::ExprKind::Str:    return escape(static_cast<const ir::StrLit&>(e).value);
            case ir::ExprKind::Var:    return static_cast<const ir::Var&>(e).name;
            case ir::ExprKind::This:   return "self";
            case ir::ExprKind::Extern: return static_cast<const ir::Extern&>(e).code; // raw Python verbatim
            case ir::ExprKind::Unary: {
                const auto& u = static_cast<const ir::Unary&>(e);
                std::string operand = u.operand->kind == ir::ExprKind::Binary ? "(" + emitExpr(*u.operand) + ")"
                                                                              : emitExpr(*u.operand);
                return (u.op == "!" ? "not " : u.op) + operand;
            }
            case ir::ExprKind::Binary: {
                const auto& b = static_cast<const ir::Binary&>(e);
                int p = operatorPrecedence(b.op);
                return child(*b.lhs, p, false) + " " + pyOp(b.op, e.type) + " " + child(*b.rhs, p, true);
            }
            case ir::ExprKind::Cond: { // Python ternary: `then if cond else els`
                const auto& c = static_cast<const ir::Cond&>(e);
                std::string cc = emitExpr(*c.cond), ct = emitExpr(*c.then), ce = emitExpr(*c.els);
                return "(" + ct + " if " + cc + " else " + ce + ")";
            }
            case ir::ExprKind::Call: {
                const auto& c = static_cast<const ir::Call&>(e);
                std::vector<std::string> args;
                for (const auto& a : c.args) args.push_back(emitExpr(*a));
                return c.mangledCallee + renderArgs(args);
            }
            default:
                return "__py_unsupported_expr__"; // fails loudly at runtime if a non-skeleton node reaches here
        }
    }

    std::string escape(const std::string& v) {
        std::string s = "\"";
        for (char c : v) {
            switch (c) {
                case '\\': s += "\\\\"; break;
                case '"':  s += "\\\""; break;
                case '\n': s += "\\n";  break;
                case '\t': s += "\\t";  break;
                case '\r': s += "\\r";  break;
                default:   s += c;      break;
            }
        }
        return s + "\"";
    }
};

} // namespace

std::string emitPython(const ir::Module& module) {
    PythonEmitter emitter;
    return emitter.emit(module);
}

} // namespace mintplayer::polyglot
