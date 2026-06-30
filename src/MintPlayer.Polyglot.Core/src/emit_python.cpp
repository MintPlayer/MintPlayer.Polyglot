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

bool isFloatType(const TypeRef& t) {
    return t.kind == TypeRef::Kind::Named && (t.name == "f32" || t.name == "f64");
}

class PythonEmitter : public EmitterBase {
public:
    std::string emit(const ir::Module& m) {
        out_.clear();
        indent_ = 0;
        for (const auto& r : m.records) emitRecord(r);
        for (const auto& c : m.classes) emitClass(c);
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

    // A record -> a Python class with an __init__ (positional fields) and a structural __eq__. Python `==`
    // dispatches to __eq__, so equality is just field-wise `==` joined by `and` (nested records recurse via
    // their own __eq__ — no explicit recursion as the TS backend needs). Methods, if any, emit as defs.
    void emitRecord(const ir::Record& r) {
        openBlock("class " + r.name);
        ++indent_;
        std::string ctor = "def __init__(self";
        for (const auto& f : r.fields) ctor += ", " + f.name;
        openBlock(ctor + ")");
        ++indent_;
        if (r.fields.empty()) line("pass");
        for (const auto& f : r.fields) line("self." + f.name + " = " + f.name);
        --indent_;
        openBlock("def __eq__(self, other)");
        ++indent_;
        if (r.fields.empty()) line("return True");
        else {
            std::string cond;
            for (std::size_t i = 0; i < r.fields.size(); ++i) {
                if (i) cond += " and ";
                cond += "self." + r.fields[i].name + " == other." + r.fields[i].name;
            }
            line("return " + cond);
        }
        --indent_;
        for (const auto& m : r.methods) emitMethod(m);
        --indent_;
    }

    // A class -> a Python class: `__init__` (with super()), methods. Instance fields are set in __init__ (no
    // separate declaration); static fields and field initializers outside init are a later slice.
    void emitClass(const ir::Class& c) {
        std::string head = "class " + c.name;
        if (!c.bases.empty()) {
            head += "(";
            for (std::size_t i = 0; i < c.bases.size(); ++i) { if (i) head += ", "; head += c.bases[i].name; }
            head += ")";
        }
        openBlock(head);
        ++indent_;
        bool any = false;
        if (c.hasInit) {
            std::string sig = "def __init__(self";
            for (const auto& p : c.initParams) sig += ", " + p.name;
            openBlock(sig + ")");
            ++indent_;
            if (c.hasSuper) {
                std::vector<std::string> sa;
                for (const auto& a : c.superArgs) sa.push_back(emitExpr(*a));
                line("super().__init__" + renderArgs(sa));
            }
            for (const auto& s : c.initBody) emitStmt(*s);
            if (!c.hasSuper && c.initBody.empty()) line("pass");
            --indent_;
            any = true;
        }
        for (const auto& m : c.methods) { emitMethod(m); any = true; }
        if (!any) line("pass");
        --indent_;
    }

    // A record/class method -> a `def` with a leading `self`. Only plain methods reach here: a Property or
    // Operator member would trip Python's capability gating (both off) and be refused before emit.
    void emitMethod(const ir::Method& m) {
        std::string sig = "def " + m.name + "(self";
        for (const auto& p : m.params) sig += ", " + p.name;
        sig += ")";
        if (m.exprBodied) {
            openBlock(sig);
            ++indent_;
            bool unit = m.returnType.kind == TypeRef::Kind::Named && (m.returnType.name == "unit" || m.returnType.name.empty());
            line(unit ? emitExpr(*m.exprBody) : "return " + emitExpr(*m.exprBody));
            --indent_;
        } else {
            headBlock(sig, m.body);
        }
    }

    // Parenthesize a receiver that would otherwise mis-bind against `.`/call.
    std::string atom(const ir::Expr& e) {
        std::string s = emitExpr(e);
        return (e.kind == ir::ExprKind::Binary || e.kind == ir::ExprKind::Unary ||
                e.kind == ir::ExprKind::Cond || e.kind == ir::ExprKind::Cast) ? "(" + s + ")" : s;
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
            case ir::ExprKind::Member: {
                const auto& m = static_cast<const ir::Member&>(e);
                std::string base = m.staticType.empty() ? atom(*m.object) : m.staticType;
                return base + "." + m.field; // `this.f` -> `self.f` (This emits "self")
            }
            case ir::ExprKind::MethodCall: {
                const auto& mc = static_cast<const ir::MethodCall&>(e);
                const std::string& st = mc.staticType;
                bool prim = st == "i8" || st == "i16" || st == "i32" || st == "i64" || st == "u8" ||
                            st == "u16" || st == "u32" || st == "u64" || st == "f32" || st == "f64";
                if (prim && mc.method == "parse") { // i32.parse(s)/f64.parse(s) -> int(s)/float(s)
                    std::string arg = emitExpr(*mc.args[0]);
                    return (st == "f32" || st == "f64" ? "float(" : "int(") + arg + ")";
                }
                std::string recv = mc.staticType.empty() ? atom(*mc.object) : mc.staticType;
                std::vector<std::string> args;
                for (const auto& a : mc.args) args.push_back(emitExpr(*a));
                return recv + "." + mc.method + renderArgs(args);
            }
            case ir::ExprKind::Lambda: {
                const auto& l = static_cast<const ir::Lambda&>(e);
                std::string params;
                for (std::size_t i = 0; i < l.params.size(); ++i) { if (i) params += ", "; params += l.params[i].name; }
                // Python lambdas are single-expression; a statement-bodied lambda needs a nested def (later).
                if (l.exprBodied) return "lambda " + params + ": " + emitExpr(*l.body);
                return "__py_unsupported_block_lambda__";
            }
            case ir::ExprKind::New: { // user record/class construction — Python has no `new`, no type args
                const auto& n = static_cast<const ir::New&>(e);
                std::vector<std::string> args;
                for (const auto& a : n.args) args.push_back(emitExpr(*a));
                return n.typeName + renderArgs(args);
            }
            case ir::ExprKind::Cast: {
                const auto& c = static_cast<const ir::Cast&>(e);
                std::string x = emitExpr(*c.operand);
                const TypeRef& to = e.type;
                bool fromFloat = isFloatType(c.operand->type);
                if (isFloatType(to)) return isIntType(c.operand->type) ? "float(" + x + ")" : x; // int->float
                if (isIntType(to) && fromFloat) return "int(" + x + ")"; // float->int truncates toward zero (matches C#)
                return x; // int<->int: Python ints are arbitrary-precision (overflow masking is a later slice)
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
