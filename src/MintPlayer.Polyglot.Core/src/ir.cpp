#include "mintplayer/polyglot/ir.hpp"

#include <string>

namespace mintplayer::polyglot::ir {

namespace {

std::string typeName(const TypeRef& t) {
    if (t.kind == TypeRef::Kind::Tuple) {
        std::string s = "(";
        for (std::size_t i = 0; i < t.args.size(); ++i) { if (i) s += ", "; s += typeName(t.args[i]); }
        return s + ")" + (t.nullable ? "?" : "");
    }
    if (t.kind == TypeRef::Kind::Function) {
        std::string s = "(";
        for (std::size_t i = 0; i < t.args.size(); ++i) { if (i) s += ", "; s += typeName(t.args[i]); }
        s += ") => " + (t.ret.empty() ? std::string("unit") : typeName(t.ret[0]));
        return s + (t.nullable ? "?" : "");
    }
    std::string s = t.name.empty() ? "?" : t.name;
    if (!t.args.empty()) {
        s += "<";
        for (std::size_t i = 0; i < t.args.size(); ++i) { if (i) s += ", "; s += typeName(t.args[i]); }
        s += ">";
    }
    return s + (t.nullable ? "?" : "");
}

std::string gen(const std::vector<GenericParam>& gs) {
    if (gs.empty()) return "";
    std::string s = "<";
    for (std::size_t i = 0; i < gs.size(); ++i) { if (i) s += ", "; s += gs[i].name; }
    return s + ">";
}

class Dumper {
public:
    std::string run(const Module& m) {
        for (const auto& e : m.enums) {
            std::string s = "enum " + e.name + " { ";
            for (std::size_t i = 0; i < e.cases.size(); ++i) { if (i) s += ", "; s += e.cases[i].name + " = " + std::to_string(e.cases[i].value); }
            line(s + " }");
        }
        for (const auto& r : m.records) record(r);
        for (const auto& c : m.classes) klass(c);
        for (const auto& fn : m.extensions) function(fn);
        for (const auto& fn : m.functions) function(fn);
        return out_;
    }

    void record(const Record& r) {
        std::string s = "record " + r.name + gen(r.generics) + "(";
        for (std::size_t i = 0; i < r.fields.size(); ++i) { if (i) s += ", "; s += r.fields[i].name + ": " + typeName(r.fields[i].type); }
        line(s + ")");
    }

    void klass(const Class& c) {
        std::string head = "class " + c.name + gen(c.generics);
        if (!c.bases.empty()) {
            head += " : ";
            for (std::size_t i = 0; i < c.bases.size(); ++i) { if (i) head += ", "; head += typeName(c.bases[i]); }
        }
        line(head + " {");
        ++indent_;
        for (const auto& f : c.fields)
            line(std::string(f.isMutable ? "var " : "let ") + f.name + ": " + typeName(f.type) + (f.init ? " = " + expr(*f.init) : ""));
        if (c.hasInit) {
            std::string s = "constructor(";
            for (std::size_t i = 0; i < c.initParams.size(); ++i) { if (i) s += ", "; s += c.initParams[i].name + ": " + typeName(c.initParams[i].type); }
            line(s + ") {");
            ++indent_;
            if (c.hasSuper) {
                std::string sup = "super(";
                for (std::size_t i = 0; i < c.superArgs.size(); ++i) { if (i) sup += ", "; sup += expr(*c.superArgs[i]); }
                line(sup + ")");
            }
            for (const auto& st : c.initBody) stmt(*st);
            --indent_;
            line("}");
        }
        --indent_;
        line("}");
    }

private:
    std::string out_;
    int indent_ = 0;

    void line(const std::string& s) {
        out_.append(static_cast<std::size_t>(indent_) * 2, ' ');
        out_ += s;
        out_ += '\n';
    }

    void function(const Function& fn) {
        std::string sig = "fn " + fn.name + gen(fn.generics) + "(";
        for (std::size_t i = 0; i < fn.params.size(); ++i) { if (i) sig += ", "; sig += fn.params[i].name + ": " + typeName(fn.params[i].type); }
        sig += "): " + typeName(fn.returnType);
        if (fn.isEntry) sig += " [entry]";
        if (fn.isIterator) sig += " [iterator]";
        if (fn.isExtension) sig += " [extension]";
        if (fn.exprBodied) { line(sig + " => " + expr(*fn.exprBody)); return; }
        line(sig + " {");
        ++indent_;
        for (const auto& s : fn.body) stmt(*s);
        --indent_;
        line("}");
    }

    void block(const std::vector<StmtPtr>& body) {
        ++indent_;
        for (const auto& s : body) stmt(*s);
        --indent_;
    }

    void stmt(const Stmt& s) {
        switch (s.kind) {
            case StmtKind::Let: {
                const auto& l = static_cast<const Let&>(s);
                std::string suffix = l.needsCell ? " [cell]" : (l.captured ? " [captured]" : ""); // P25 §4.18
                line(std::string(l.isMutable ? "var " : "let ") + l.name + ": " + typeName(l.type) + " = " + expr(*l.init) + suffix);
                break;
            }
            case StmtKind::Assign: {
                const auto& a = static_cast<const Assign&>(s);
                line(expr(*a.target) + " " + a.op + " " + expr(*a.value));
                break;
            }
            case StmtKind::IndexAssign: {
                const auto& ia = static_cast<const IndexAssign&>(s);
                std::string idx;
                for (std::size_t i = 0; i < ia.indices.size(); ++i) { if (i) idx += ", "; idx += expr(*ia.indices[i]); }
                line(expr(*ia.receiver) + "[" + idx + "] = " + expr(*ia.value));
                break;
            }
            case StmtKind::ExprStmt:
                line(expr(*static_cast<const ExprStmt&>(s).expr));
                break;
            case StmtKind::If: {
                const auto& i = static_cast<const If&>(s);
                line("if " + expr(*i.cond) + " {");
                block(i.thenBody);
                if (i.hasElse) { line("} else {"); block(i.elseBody); }
                line("}");
                break;
            }
            case StmtKind::While: {
                const auto& w = static_cast<const While&>(s);
                line("while " + expr(*w.cond) + " {");
                block(w.body);
                line("}");
                break;
            }
            case StmtKind::For: {
                const auto& f = static_cast<const For&>(s);
                std::string seq = f.isRange
                    ? expr(*f.rangeStart) + (f.inclusive ? "..=" : "..") + expr(*f.rangeEnd)
                    : expr(*f.iterable);
                line("for " + f.binding + " in " + seq + " {");
                block(f.body);
                line("}");
                break;
            }
            case StmtKind::Return: {
                const auto& r = static_cast<const Return&>(s);
                line(r.value ? "return " + expr(*r.value) : "return");
                break;
            }
            case StmtKind::Yield: {
                const auto& y = static_cast<const Yield&>(s);
                line(y.value ? "yield " + expr(*y.value) : "yield");
                break;
            }
            case StmtKind::Throw: {
                const auto& t = static_cast<const Throw&>(s);
                line(t.value ? "throw " + expr(*t.value) : "throw");
                break;
            }
            case StmtKind::Use: {
                const auto& u = static_cast<const Use&>(s);
                line("use " + u.binding + " = " + expr(*u.init) + " {");
                block(u.body);
                line("}");
                break;
            }
            case StmtKind::Try: {
                const auto& t = static_cast<const Try&>(s);
                line("try {");
                block(t.body);
                for (const auto& c : t.catches) {
                    std::string head = "} catch";
                    if (!c.type.name.empty()) head += " (" + c.binding + ": " + typeName(c.type) + ")";
                    if (c.guard) head += " when " + expr(*c.guard);
                    line(head + " {");
                    block(c.body);
                }
                if (t.hasFinally) { line("} finally {"); block(t.finallyBody); }
                line("}");
                break;
            }
            case StmtKind::LocalFunc: { // P25 §4.18: a hoisted nested def (Python block-lambda lowering)
                const auto& lf = static_cast<const LocalFunc&>(s);
                std::string sig = "def " + lf.name + "(";
                for (std::size_t i = 0; i < lf.params.size(); ++i) { if (i) sig += ", "; sig += lf.params[i].name; }
                sig += ")";
                for (std::size_t i = 0; i < lf.nonlocals.size(); ++i) sig += (i ? ", " : " nonlocal ") + lf.nonlocals[i];
                line(sig + " {");
                block(lf.body);
                line("}");
                break;
            }
        }
    }

    // Each expression is annotated with its resolved type: `<repr>:<type>`.
    std::string expr(const Expr& e) {
        std::string repr;
        switch (e.kind) {
            case ExprKind::Int:   repr = static_cast<const IntLit&>(e).text; break;
            case ExprKind::Float: repr = static_cast<const FloatLit&>(e).text; break;
            case ExprKind::Bool:  repr = static_cast<const BoolLit&>(e).value ? "true" : "false"; break;
            case ExprKind::Str:   repr = "\"" + static_cast<const StrLit&>(e).value + "\""; break;
            case ExprKind::Var:   repr = static_cast<const Var&>(e).name; break;
            case ExprKind::This:  repr = "this"; break;
            case ExprKind::MethodCall: {
                const auto& mc = static_cast<const MethodCall&>(e);
                repr = (mc.object ? expr(*mc.object) : mc.staticType) + "." + mc.method + "(";
                for (std::size_t i = 0; i < mc.args.size(); ++i) { if (i) repr += ", "; repr += expr(*mc.args[i]); }
                repr += ")";
                break;
            }
            case ExprKind::Unary: { const auto& u = static_cast<const Unary&>(e); repr = u.op + expr(*u.operand); break; }
            case ExprKind::Await: { const auto& a = static_cast<const Await&>(e); repr = "await " + expr(*a.operand); break; }
            case ExprKind::Cast: { const auto& c = static_cast<const Cast&>(e); repr = "(" + typeName(e.type) + ")" + expr(*c.operand); break; }
            case ExprKind::IsTest: { const auto& t = static_cast<const IsTest&>(e); repr = expr(*t.operand) + " is " + typeName(t.testType); break; }
            case ExprKind::AsCast: { const auto& a = static_cast<const AsCast&>(e); repr = expr(*a.operand) + " as " + typeName(a.castType); break; }
            case ExprKind::Extern: { repr = "extern(\"" + static_cast<const Extern&>(e).code + "\")"; break; }
            case ExprKind::Binary: {
                const auto& b = static_cast<const Binary&>(e);
                repr = "(" + expr(*b.lhs) + " " + b.op + " " + expr(*b.rhs) + ")";
                break;
            }
            case ExprKind::Call: {
                const auto& c = static_cast<const Call&>(e);
                repr = (c.isPrint ? "print" : c.callee) + "(";
                for (std::size_t i = 0; i < c.args.size(); ++i) { if (i) repr += ", "; repr += expr(*c.args[i]); }
                repr += ")";
                break;
            }
            case ExprKind::Member: {
                const auto& m = static_cast<const Member&>(e);
                repr = expr(*m.object) + (m.nullSafe ? "?." : ".") + m.field;
                break;
            }
            case ExprKind::New: {
                const auto& n = static_cast<const New&>(e);
                repr = "new " + n.typeName + "(";
                for (std::size_t i = 0; i < n.args.size(); ++i) { if (i) repr += ", "; repr += expr(*n.args[i]); }
                repr += ")";
                break;
            }
            case ExprKind::MakeCase: {
                const auto& mc = static_cast<const MakeCase&>(e);
                repr = mc.caseName + "{";
                for (std::size_t i = 0; i < mc.fields.size(); ++i) { if (i) repr += ", "; repr += mc.fields[i].name + ": " + expr(*mc.fields[i].value); }
                repr += "}";
                break;
            }
            case ExprKind::Lambda: {
                const auto& l = static_cast<const Lambda&>(e);
                repr = "(";
                for (std::size_t i = 0; i < l.params.size(); ++i) {
                    if (i) repr += ", ";
                    repr += l.params[i].name;
                    if (!l.params[i].type.absent()) repr += ": " + typeName(l.params[i].type);
                }
                repr += ")";
                if (!l.captures.empty() || l.capturesThis) { // P25 §4.18 capture facts
                    repr += " [caps";
                    for (const auto& c : l.captures) repr += " " + c.name + (c.needsCell ? "(cell)" : "");
                    if (l.capturesThis) repr += " this";
                    repr += "]";
                }
                repr += " => ";
                repr += l.exprBodied ? expr(*l.body) : "{ ... }";
                break;
            }
            case ExprKind::Match: {
                const auto& m = static_cast<const Match&>(e);
                repr = "match " + expr(*m.scrutinee) + " { ";
                for (const auto& a : m.arms) {
                    switch (a.pattern.kind) {
                        case PatternKind::Wildcard: repr += "_"; break;
                        case PatternKind::Literal:  repr += expr(*a.pattern.literal); break;
                        case PatternKind::Binding:  repr += a.pattern.binding; break;
                        case PatternKind::EnumCase: repr += a.pattern.enumType + "." + a.pattern.enumCase; break;
                        case PatternKind::Ctor: {
                            repr += a.pattern.ctorCase + "(";
                            for (std::size_t i = 0; i < a.pattern.binders.size(); ++i) { if (i) repr += ", "; repr += a.pattern.binders[i].binding; }
                            repr += ")";
                            break;
                        }
                    }
                    if (a.guard) repr += " if " + expr(*a.guard);
                    repr += " => " + expr(*a.body) + ", ";
                }
                repr += "}";
                break;
            }
        }
        return repr + ":" + typeName(e.type);
    }
};

} // namespace

std::string dump(const Module& m) {
    Dumper d;
    return d.run(m);
}

} // namespace mintplayer::polyglot::ir
