#include "mintplayer/polyglot/emit.hpp"

#include <string>

// Hand-written IR -> C# pretty-printer. Walks the typed IR; wraps the program's free functions in a
// `static class Program`, maps the `print` intrinsic -> Console.WriteLine and the entry function -> Main.

namespace mintplayer::polyglot {

namespace {

std::string csType(const TypeRef& t) {
    if (t.kind == TypeRef::Kind::Named) {
        if (t.name == "unit")   return "void";
        if (t.name == "i8")     return "sbyte";
        if (t.name == "i16")    return "short";
        if (t.name == "i32")    return "int";
        if (t.name == "i64")    return "long";
        if (t.name == "u8")     return "byte";
        if (t.name == "u16")    return "ushort";
        if (t.name == "u32")    return "uint";
        if (t.name == "u64")    return "ulong";
        if (t.name == "f32")    return "float";
        if (t.name == "f64")    return "double";
        if (t.name == "bool")   return "bool";
        if (t.name == "string") return "string";
        if (t.name.empty())     return "object";
        if (t.name == "Error")  return "System.Exception";
        std::string name = (t.name == "Iterable") ? "System.Collections.Generic.IEnumerable" : t.name;
        if (!t.args.empty()) {
            name += "<";
            for (std::size_t i = 0; i < t.args.size(); ++i) { if (i) name += ", "; name += csType(t.args[i]); }
            name += ">";
        }
        return name;
    }
    if (t.kind == TypeRef::Kind::Function) { // C# delegate: Action for unit return, else Func<args, ret>
        bool returnsUnit = t.ret.empty() || (t.ret[0].kind == TypeRef::Kind::Named && t.ret[0].name == "unit");
        if (returnsUnit) {
            if (t.args.empty()) return "System.Action";
            std::string s = "System.Action<";
            for (std::size_t i = 0; i < t.args.size(); ++i) { if (i) s += ", "; s += csType(t.args[i]); }
            return s + ">";
        }
        std::string s = "System.Func<";
        for (const auto& a : t.args) s += csType(a) + ", ";
        return s + csType(t.ret[0]) + ">";
    }
    return "object";
}

std::string csGenerics(const std::vector<ir::GenericParam>& gs) {
    if (gs.empty()) return "";
    std::string s = "<";
    for (std::size_t i = 0; i < gs.size(); ++i) { if (i) s += ", "; s += gs[i].name; }
    return s + ">";
}
// C# puts bounds in trailing `where T : A, B` clauses (one per bounded parameter).
std::string csWhere(const std::vector<ir::GenericParam>& gs) {
    std::string s;
    for (const auto& g : gs) {
        if (g.bounds.empty()) continue;
        s += " where " + g.name + " : ";
        for (std::size_t i = 0; i < g.bounds.size(); ++i) { if (i) s += ", "; s += csType(g.bounds[i]); }
    }
    return s;
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
        out_ = "using System;\n\n";
        indent_ = 0;
        for (const auto& e : m.enums) emitEnum(e);
        for (const auto& u : m.unions) emitUnion(u);
        for (const auto& r : m.records) emitRecord(r);
        for (const auto& c : m.classes) emitClass(c);
        if (!m.extensions.empty()) { // extension methods live in a top-level static class (global namespace)
            line("static class Extensions");
            line("{");
            ++indent_;
            for (const auto& f : m.extensions) emitExtension(f);
            --indent_;
            line("}");
        }
        if (!m.enums.empty() || !m.unions.empty() || !m.records.empty() || !m.classes.empty() || !m.extensions.empty()) out_ += "\n";
        out_ += "static class Program\n{\n";
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
    std::string thisAlias_; // non-empty inside a static operator body: `this` is emitted as this name

    void line(const std::string& s) {
        out_.append(static_cast<std::size_t>(indent_) * 4, ' ');
        out_ += s;
        out_ += '\n';
    }

    void emitEnum(const ir::Enum& e) {
        std::string s = "enum " + e.name + " { ";
        for (std::size_t i = 0; i < e.cases.size(); ++i) { if (i) s += ", "; s += e.cases[i].name + " = " + std::to_string(e.cases[i].value); }
        line(s + " }");
    }

    void emitUnion(const ir::Union& u) {
        line("abstract record " + u.name + ";");
        for (const auto& c : u.cases) {
            std::string s = "sealed record " + c.name + "(";
            for (std::size_t i = 0; i < c.fields.size(); ++i) { if (i) s += ", "; s += csType(c.fields[i].type) + " " + c.fields[i].name; }
            line(s + ") : " + u.name + ";");
        }
    }

    std::string patternCs(const ir::Pattern& p) {
        switch (p.kind) {
            case ir::PatternKind::Wildcard: return "_";
            case ir::PatternKind::Literal:  return emitExpr(*p.literal);
            case ir::PatternKind::Binding:  return "var " + p.binding;
            case ir::PatternKind::EnumCase: return p.enumType + "." + p.enumCase;
            case ir::PatternKind::Ctor: {
                if (p.binders.empty()) return p.ctorCase + " _"; // type pattern (payload-free)
                std::string s = p.ctorCase + "(";
                for (std::size_t i = 0; i < p.binders.size(); ++i) { if (i) s += ", "; s += "var " + p.binders[i].binding; }
                return s + ")";
            }
        }
        return "_";
    }

    void emitRecord(const ir::Record& r) {
        std::string head = "record " + r.name + csGenerics(r.generics) + "(";
        for (std::size_t i = 0; i < r.fields.size(); ++i) { if (i) head += ", "; head += csType(r.fields[i].type) + " " + r.fields[i].name; }
        head += ")" + csWhere(r.generics);
        if (r.methods.empty()) { line(head + ";"); return; }
        line(head);
        line("{");
        ++indent_;
        for (const auto& m : r.methods) emitMethod(r.name, m);
        --indent_;
        line("}");
    }

    // A mutable reference type: fields, a constructor (`init`), and methods.
    void emitClass(const ir::Class& c) {
        std::string head = "class " + c.name + csGenerics(c.generics);
        if (!c.bases.empty()) {
            head += " : ";
            for (std::size_t i = 0; i < c.bases.size(); ++i) { if (i) head += ", "; head += csType(c.bases[i]); }
        }
        head += csWhere(c.generics);
        line(head);
        line("{");
        ++indent_;
        for (const auto& f : c.fields) {
            std::string decl = "public " + std::string(f.isMutable ? "" : "readonly ") + csType(f.type) + " " + f.name;
            if (f.init) decl += " = " + emitExpr(*f.init);
            line(decl + ";");
        }
        if (c.hasInit) {
            std::string sig = "public " + c.name + "(";
            for (std::size_t i = 0; i < c.initParams.size(); ++i) { if (i) sig += ", "; sig += csType(c.initParams[i].type) + " " + c.initParams[i].name; }
            sig += ")";
            if (c.hasSuper) {
                sig += " : base(";
                for (std::size_t i = 0; i < c.superArgs.size(); ++i) { if (i) sig += ", "; sig += emitExpr(*c.superArgs[i]); }
                sig += ")";
            }
            line(sig);
            emitBlock(c.initBody);
        }
        for (const auto& m : c.methods) emitMethod(c.name, m);
        --indent_;
        line("}");
    }

    void emitMethod(const std::string& recordName, const ir::Method& m) {
        if (m.kind == ir::MethodKind::Property) { // expression-bodied property
            line("public " + csType(m.returnType) + " " + m.name + " => " + emitExpr(*m.exprBody) + ";");
            return;
        }
        if (m.kind == ir::MethodKind::Operator) { // real C# static operator; `this` -> the first operand
            std::string sig = "public static " + csType(m.returnType) + " operator " + m.opSymbol + "(" + recordName + " lhs";
            for (const auto& p : m.params) sig += ", " + csType(p.type) + " " + p.name;
            sig += ")";
            thisAlias_ = "lhs";
            if (m.exprBodied) line(sig + " => " + emitExpr(*m.exprBody) + ";");
            else { line(sig); emitBlock(m.body); }
            thisAlias_.clear();
            return;
        }
        std::string sig = "public " + csType(m.returnType) + " " + m.name + csGenerics(m.generics) + "(";
        for (std::size_t i = 0; i < m.params.size(); ++i) { if (i) sig += ", "; sig += csType(m.params[i].type) + " " + m.params[i].name; }
        sig += ")" + csWhere(m.generics);
        if (m.exprBodied) line(sig + " => " + emitExpr(*m.exprBody) + ";");
        else { line(sig); emitBlock(m.body); }
    }

    // Parenthesize a receiver that would otherwise mis-bind against `.`/call.
    std::string atom(const ir::Expr& e) {
        std::string s = emitExpr(e);
        return (e.kind == ir::ExprKind::Binary || e.kind == ir::ExprKind::Unary) ? "(" + s + ")" : s;
    }

    // `public static R name(this T self, …)` — the leading `self` param carries the C# `this` modifier.
    void emitExtension(const ir::Function& f) {
        std::string sig = "public static " + csType(f.returnType) + " " + f.name + csGenerics(f.generics) + "(this " +
                          csType(f.params[0].type) + " " + f.params[0].name;
        for (std::size_t i = 1; i < f.params.size(); ++i) sig += ", " + csType(f.params[i].type) + " " + f.params[i].name;
        sig += ")" + csWhere(f.generics);
        if (f.exprBodied) line(sig + " => " + emitExpr(*f.exprBody) + ";");
        else { line(sig); emitBlock(f.body); }
    }

    void emitFunction(const ir::Function& fn) {
        std::string sig = "static " + csType(fn.returnType) + " " + fn.name + csGenerics(fn.generics) + "(";
        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            if (i) sig += ", ";
            sig += csType(fn.params[i].type) + " " + fn.params[i].name;
        }
        sig += ")" + csWhere(fn.generics);
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

    // Render statements onto a single line (for statement-bodied lambdas, which live mid-expression).
    std::string inlineBlock(const std::vector<ir::StmtPtr>& body) {
        std::string saved = std::move(out_);
        int savedIndent = indent_;
        out_.clear();
        indent_ = 0;
        for (const auto& s : body) emitStmt(*s);
        std::string rendered = std::move(out_);
        out_ = std::move(saved);
        indent_ = savedIndent;
        std::string flat;
        for (char c : rendered) flat += (c == '\n') ? ' ' : c;
        return flat;
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
            case ir::StmtKind::Yield: {
                const auto& y = static_cast<const ir::Yield&>(s);
                line(y.value ? "yield return " + emitExpr(*y.value) + ";" : "yield break;");
                break;
            }
            case ir::StmtKind::Throw: {
                const auto& t = static_cast<const ir::Throw&>(s);
                line(t.value ? "throw " + emitExpr(*t.value) + ";" : "throw;");
                break;
            }
            case ir::StmtKind::Use: {
                const auto& u = static_cast<const ir::Use&>(s);
                line("var " + u.binding + " = " + emitExpr(*u.init) + ";");
                line("try");
                emitBlock(u.body);
                line("finally");
                line("{");
                ++indent_;
                line(u.binding + ".dispose();");
                --indent_;
                line("}");
                break;
            }
            case ir::StmtKind::Try: {
                const auto& t = static_cast<const ir::Try&>(s);
                line("try");
                emitBlock(t.body);
                for (const auto& c : t.catches) {
                    std::string head = "catch";
                    if (!c.type.name.empty()) {
                        head += " (" + csType(c.type);
                        if (!c.binding.empty()) head += " " + c.binding;
                        head += ")";
                    }
                    if (c.guard) head += " when (" + emitExpr(*c.guard) + ")";
                    line(head);
                    emitBlock(c.body);
                }
                if (t.hasFinally) { line("finally"); emitBlock(t.finallyBody); }
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
            case ir::StmtKind::For: {
                const auto& f = static_cast<const ir::For&>(s);
                if (f.isRange) {
                    std::string cmp = f.inclusive ? " <= " : " < ";
                    line("for (var " + f.binding + " = " + emitExpr(*f.rangeStart) + "; " +
                         f.binding + cmp + emitExpr(*f.rangeEnd) + "; " + f.binding + "++)");
                } else {
                    line("foreach (var " + f.binding + " in " + emitExpr(*f.iterable) + ")");
                }
                emitBlock(f.body);
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
            case ir::ExprKind::This:  return thisAlias_.empty() ? "this" : thisAlias_;
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
                std::string ctor = (n.typeName == "Error") ? "System.Exception" : n.typeName;
                if (!n.typeArgs.empty()) {
                    ctor += "<";
                    for (std::size_t i = 0; i < n.typeArgs.size(); ++i) { if (i) ctor += ", "; ctor += csType(n.typeArgs[i]); }
                    ctor += ">";
                }
                std::string s = "new " + ctor + "(";
                for (std::size_t i = 0; i < n.args.size(); ++i) { if (i) s += ", "; s += emitExpr(*n.args[i]); }
                return s + ")";
            }
            case ir::ExprKind::MakeCase: {
                const auto& mc = static_cast<const ir::MakeCase&>(e);
                std::string s = "new " + mc.caseName + "(";
                for (std::size_t i = 0; i < mc.fields.size(); ++i) { if (i) s += ", "; s += emitExpr(*mc.fields[i].value); }
                return s + ")";
            }
            case ir::ExprKind::Lambda: {
                const auto& l = static_cast<const ir::Lambda&>(e);
                std::string s = "(";
                // A typed param emits its type; an untyped (bare) param relies on the target type.
                for (std::size_t i = 0; i < l.params.size(); ++i) {
                    if (i) s += ", ";
                    if (!l.params[i].type.absent()) s += csType(l.params[i].type) + " ";
                    s += l.params[i].name;
                }
                s += ") => ";
                if (l.exprBodied) return s + emitExpr(*l.body);
                return s + "{ " + inlineBlock(l.block) + "}"; // statement-bodied lambda
            }
            case ir::ExprKind::Match: {
                const auto& m = static_cast<const ir::Match&>(e);
                std::string s = atom(*m.scrutinee) + " switch { ";
                bool hasCatchAll = false;
                for (std::size_t i = 0; i < m.arms.size(); ++i) {
                    if (i) s += ", ";
                    const ir::Pattern& p = m.arms[i].pattern;
                    if (!m.arms[i].guard && (p.kind == ir::PatternKind::Wildcard || p.kind == ir::PatternKind::Binding)) hasCatchAll = true;
                    s += patternCs(p);
                    if (m.arms[i].guard) s += " when " + emitExpr(*m.arms[i].guard);
                    s += " => " + emitExpr(*m.arms[i].body);
                }
                // Sema guarantees exhaustiveness, but C# can't prove it for enums — add an unreachable
                // default so the switch expression compiles without CS8524.
                if (!hasCatchAll) s += ", _ => throw new System.InvalidOperationException()";
                return s + " }";
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
