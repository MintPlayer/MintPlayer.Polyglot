#include "mintplayer/polyglot/emit.hpp"

#include <string>

// Hand-written IR -> TypeScript pretty-printer. Walks the typed IR; emits free `function`s, maps the
// `print` intrinsic -> console.log, and appends a top-level call to the entry function. Output stays
// plain enough to run under Node's type-stripping (the P2 differential conformance test relies on it).

namespace mintplayer::polyglot {

namespace {

std::string tsType(const TypeRef& t) {
    if (t.kind == TypeRef::Kind::Named) {
        if (t.name == "unit")   return "void";
        if (t.name == "bool")   return "boolean";
        if (t.name == "string") return "string";
        if (t.name == "char")   return "string";
        if (t.name == "i64" || t.name == "u64") return "bigint"; // §3.C: 64-bit ints exceed 2^53 -> BigInt
        if (t.name == "i8" || t.name == "i16" || t.name == "i32" ||
            t.name == "u8" || t.name == "u16" || t.name == "u32" ||
            t.name == "f32" || t.name == "f64") return "number";
        if (t.name.empty()) return "unknown";
        std::string name = t.name; // Iterable<T> stays Iterable<T> (a generator is assignable to it)
        if (!t.args.empty()) {
            name += "<";
            for (std::size_t i = 0; i < t.args.size(); ++i) { if (i) name += ", "; name += tsType(t.args[i]); }
            name += ">";
        }
        return name;
    }
    if (t.kind == TypeRef::Kind::Function) { // arrow-function type: (arg0: T0, …) => Ret
        std::string s = "(";
        for (std::size_t i = 0; i < t.args.size(); ++i) { if (i) s += ", "; s += "arg" + std::to_string(i) + ": " + tsType(t.args[i]); }
        return s + ") => " + (t.ret.empty() ? "void" : tsType(t.ret[0]));
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
bool isI32(const TypeRef& t) { return t.kind == TypeRef::Kind::Named && t.name == "i32"; }
bool isI64(const TypeRef& t) { return t.kind == TypeRef::Kind::Named && (t.name == "i64" || t.name == "u64"); }
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

// TS carries bounds inline on each parameter: `<T extends A & B, U>`.
std::string tsGenerics(const std::vector<ir::GenericParam>& gs) {
    if (gs.empty()) return "";
    std::string s = "<";
    for (std::size_t i = 0; i < gs.size(); ++i) {
        if (i) s += ", ";
        s += gs[i].name;
        if (!gs[i].bounds.empty()) {
            s += " extends ";
            for (std::size_t j = 0; j < gs[i].bounds.size(); ++j) { if (j) s += " & "; s += tsType(gs[i].bounds[j]); }
        }
    }
    return s + ">";
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
        for (const auto& u : m.unions) emitUnion(u);
        for (const auto& r : m.records) emitRecord(r);
        for (const auto& c : m.classes) emitClass(c);
        for (const auto& f : m.extensions) emitExtension(f);
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

    // Union -> a discriminated-union type alias (stripped at runtime; construction makes {tag,...} objects).
    void emitUnion(const ir::Union& u) {
        std::string s = "type " + u.name + " = ";
        for (std::size_t i = 0; i < u.cases.size(); ++i) {
            if (i) s += " | ";
            s += "{ tag: \"" + u.cases[i].name + "\"";
            for (const auto& f : u.cases[i].fields) s += "; " + f.name + ": " + tsType(f.type);
            s += " }";
        }
        line(s + ";");
    }

    // One arm of a match, lowered into the IIFE if-chain.
    std::string matchArm(const ir::MatchArm& a) {
        const ir::Pattern& p = a.pattern;
        std::string body = emitExpr(*a.body);
        if (p.kind == ir::PatternKind::Ctor) {
            std::string s = "if (_m.tag === \"" + p.ctorCase + "\"";
            if (a.guard) s += " && (" + emitExpr(*a.guard) + ")";
            s += ") { ";
            for (const auto& b : p.binders) s += "const " + b.binding + " = _m." + b.field + "; ";
            return s + "return " + body + "; } ";
        }
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
        line("class " + r.name + tsGenerics(r.generics) + " {");
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

    // A mutable reference type: explicit fields, a constructor (`init`), and methods.
    void emitClass(const ir::Class& c) {
        std::string head = "class " + c.name + tsGenerics(c.generics);
        if (!c.bases.empty()) head += " extends " + tsType(c.bases[0]); // inheritance emission widens later
        line(head + " {");
        ++indent_;
        for (const auto& f : c.fields) {
            std::string decl = f.name + ": " + tsType(f.type);
            if (f.init) decl += " = " + emitExpr(*f.init);
            line(decl + ";");
        }
        if (c.hasInit) {
            std::string sig = "constructor(";
            for (std::size_t i = 0; i < c.initParams.size(); ++i) { if (i) sig += ", "; sig += c.initParams[i].name + ": " + tsType(c.initParams[i].type); }
            line(sig + ") {");
            ++indent_;
            if (c.hasSuper) {
                std::string call = "super(";
                for (std::size_t i = 0; i < c.superArgs.size(); ++i) { if (i) call += ", "; call += emitExpr(*c.superArgs[i]); }
                line(call + ");");
            }
            for (const auto& s : c.initBody) emitStmt(*s);
            --indent_;
            line("}");
        }
        for (const auto& m : c.methods) emitMethod(m);
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
        std::string sig = m.name + tsGenerics(m.generics) + "(";
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

    // An extension lowers to a plain free function whose first param is the receiver `self`; call sites
    // emit `name(obj, …)` (TS has no extension-method call syntax — the `x.m()` reading cannot survive).
    void emitExtension(const ir::Function& f) {
        std::string sig = "function " + f.name + tsGenerics(f.generics) + "(";
        for (std::size_t i = 0; i < f.params.size(); ++i) { if (i) sig += ", "; sig += f.params[i].name + ": " + tsType(f.params[i].type); }
        sig += "): " + tsType(f.returnType) + " {";
        line(sig);
        ++indent_;
        if (f.exprBodied) line("return " + emitExpr(*f.exprBody) + ";");
        else for (const auto& s : f.body) emitStmt(*s);
        --indent_;
        line("}");
    }

    void emitFunction(const ir::Function& fn) {
        std::string sig = std::string(fn.isIterator ? "function* " : "function ") + fn.name + tsGenerics(fn.generics) + "(";
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

    // TS has a single untyped `catch`, so a typed/guarded catch list becomes an instanceof/guard
    // dispatch chain. A `__handled` flag reproduces C#'s semantics: a clause whose type matches but
    // whose `when` guard fails falls through to the next clause; if none handle it, the error rethrows.
    void emitTry(const ir::Try& t) {
        line("try {");
        emitBlock(t.body);
        if (!t.catches.empty()) {
            line("} catch (__e) {");
            ++indent_;
            line("let __handled = false;");
            bool hasCatchAll = false;
            for (const auto& c : t.catches) {
                bool typed = !c.type.name.empty();
                std::string cond = "!__handled";
                if (typed) cond += " && __e instanceof " + tsType(c.type);
                if (!typed && !c.guard) hasCatchAll = true;
                line("if (" + cond + ") {");
                ++indent_;
                if (!c.binding.empty()) line("const " + c.binding + " = __e;");
                if (c.guard) {
                    line("if (" + emitExpr(*c.guard) + ") {");
                    ++indent_;
                    line("__handled = true;");
                    for (const auto& st : c.body) emitStmt(*st);
                    --indent_;
                    line("}");
                } else {
                    line("__handled = true;");
                    for (const auto& st : c.body) emitStmt(*st);
                }
                --indent_;
                line("}");
            }
            if (!hasCatchAll) line("if (!__handled) { throw __e; }");
            --indent_;
        }
        if (t.hasFinally) { line("} finally {"); emitBlock(t.finallyBody); }
        line("}");
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
            case ir::StmtKind::Yield: {
                const auto& y = static_cast<const ir::Yield&>(s);
                line(y.value ? "yield " + emitExpr(*y.value) + ";" : "return;");
                break;
            }
            case ir::StmtKind::Throw: {
                const auto& t = static_cast<const ir::Throw&>(s);
                line(t.value ? "throw " + emitExpr(*t.value) + ";" : "throw __e;");
                break;
            }
            case ir::StmtKind::Use: {
                const auto& u = static_cast<const ir::Use&>(s);
                line("const " + u.binding + " = " + emitExpr(*u.init) + ";");
                line("try {");
                emitBlock(u.body);
                line("} finally {");
                ++indent_;
                line(u.binding + ".dispose();");
                --indent_;
                line("}");
                break;
            }
            case ir::StmtKind::Try:
                emitTry(static_cast<const ir::Try&>(s));
                break;
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
            case ir::StmtKind::For: {
                const auto& f = static_cast<const ir::For&>(s);
                if (f.isRange) {
                    std::string cmp = f.inclusive ? " <= " : " < ";
                    line("for (let " + f.binding + " = " + emitExpr(*f.rangeStart) + "; " +
                         f.binding + cmp + emitExpr(*f.rangeEnd) + "; " + f.binding + "++) {");
                } else {
                    line("for (const " + f.binding + " of " + emitExpr(*f.iterable) + ") {");
                }
                emitBlock(f.body);
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
            case ir::ExprKind::Int: { // 64-bit ints are BigInt literals (`7n`)
                const std::string& text = static_cast<const ir::IntLit&>(e).text;
                return isI64(e.type) ? text + "n" : text;
            }
            case ir::ExprKind::Float: return static_cast<const ir::FloatLit&>(e).text;
            case ir::ExprKind::Bool:  return static_cast<const ir::BoolLit&>(e).value ? "true" : "false";
            case ir::ExprKind::Str:   return escape(static_cast<const ir::StrLit&>(e).value);
            case ir::ExprKind::Var:   return static_cast<const ir::Var&>(e).name;
            case ir::ExprKind::This:  return "this";
            case ir::ExprKind::Unary: {
                const auto& u = static_cast<const ir::Unary&>(e);
                std::string operand = u.operand->kind == ir::ExprKind::Binary ? "(" + emitExpr(*u.operand) + ")"
                                                                              : emitExpr(*u.operand);
                if (isI32(e.type) && u.op == "-") return "(-" + operand + " | 0)"; // wrap negate of INT_MIN
                return u.op + operand;
            }
            case ir::ExprKind::Binary: {
                const auto& b = static_cast<const ir::Binary&>(e);
                if (isUserType(b.lhs->type)) { // operator overload -> method call (TS has no operators)
                    std::string method = opMethod(b.op);
                    if (!method.empty()) return atom(*b.lhs) + "." + method + "(" + emitExpr(*b.rhs) + ")";
                }
                int p = prec(b.op);
                std::string lhs = child(*b.lhs, p, false), rhs = child(*b.rhs, p, true);
                // §3.C: 64-bit ints are BigInt; wrap to 64 bits at each boundary so they overflow like .NET
                // `long`/`ulong` (BigInt is otherwise arbitrary-precision). BigInt `/` already truncates.
                if (isI64(e.type)) {
                    const char* w = e.type.name == "u64" ? "BigInt.asUintN(64, " : "BigInt.asIntN(64, ";
                    return std::string(w) + lhs + " " + b.op + " " + rhs + ")";
                }
                // §3.C: i32 arithmetic must wrap on overflow and truncate on division like .NET — JS numbers
                // are f64. `| 0` coerces back to a signed 32-bit int at each operation boundary; `*` needs
                // Math.imul (a plain product can exceed 2^53 and lose the low 32 bits before `| 0`).
                if (isI32(e.type)) {
                    if (b.op == "*") return "Math.imul(" + emitExpr(*b.lhs) + ", " + emitExpr(*b.rhs) + ")";
                    if (b.op == "+" || b.op == "-" || b.op == "/" || b.op == "%")
                        return "(" + lhs + " " + b.op + " " + rhs + " | 0)";
                }
                return lhs + " " + b.op + " " + rhs;
            }
            case ir::ExprKind::Call: {
                const auto& c = static_cast<const ir::Call&>(e);
                // console.log prints a BigInt with a trailing `n` (util.inspect); String() drops it so the
                // output matches C#'s Console.WriteLine(long). (Template `${x}` already omits the `n`.)
                if (c.isPrint && c.args.size() == 1 && isI64(c.args[0]->type))
                    return "console.log(String(" + emitExpr(*c.args[0]) + "))";
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
                if (mc.isExtension) { // free-function form: name(obj, args)
                    std::string s = mc.method + "(" + emitExpr(*mc.object);
                    for (const auto& a : mc.args) s += ", " + emitExpr(*a);
                    return s + ")";
                }
                std::string s = atom(*mc.object) + "." + mc.method + "(";
                for (std::size_t i = 0; i < mc.args.size(); ++i) { if (i) s += ", "; s += emitExpr(*mc.args[i]); }
                return s + ")";
            }
            case ir::ExprKind::New: {
                const auto& n = static_cast<const ir::New&>(e);
                std::string ctor = n.typeName;
                if (!n.typeArgs.empty()) {
                    ctor += "<";
                    for (std::size_t i = 0; i < n.typeArgs.size(); ++i) { if (i) ctor += ", "; ctor += tsType(n.typeArgs[i]); }
                    ctor += ">";
                }
                std::string s = "new " + ctor + "(";
                for (std::size_t i = 0; i < n.args.size(); ++i) { if (i) s += ", "; s += emitExpr(*n.args[i]); }
                return s + ")";
            }
            case ir::ExprKind::MakeCase: {
                const auto& mc = static_cast<const ir::MakeCase&>(e);
                std::string s = "{ tag: \"" + mc.caseName + "\"";
                for (const auto& f : mc.fields) s += ", " + f.name + ": " + emitExpr(*f.value);
                return s + " }";
            }
            case ir::ExprKind::Lambda: {
                const auto& l = static_cast<const ir::Lambda&>(e);
                std::string s = "(";
                for (std::size_t i = 0; i < l.params.size(); ++i) {
                    if (i) s += ", ";
                    s += l.params[i].name;
                    if (!l.params[i].type.absent()) s += ": " + tsType(l.params[i].type);
                }
                s += ") => ";
                if (l.exprBodied) return s + emitExpr(*l.body);
                return s + "{ " + inlineBlock(l.block) + "}"; // statement-bodied lambda
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
