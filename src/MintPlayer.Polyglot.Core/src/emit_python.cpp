#include "mintplayer/polyglot/emit.hpp"

#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mintplayer/polyglot/backend_spec.hpp"
#include "mintplayer/polyglot/emitter_base.hpp"

// Hand-written IR -> Python pretty-printer — the THIRD backend, added to validate the P9 shared engine
// against a non-brace target (PRD §4.3; the engine was extracted from two brace-family backends, C#/TS).
// Python is colon+indent with no statement terminators, so it drives EmitterBase's BlockStyle::ColonIndent +
// stmtEnd "" (see emitter_base.hpp). The P9-V spike grew this from the walking skeleton to the FULL §3.A
// surface (run-python.ps1: 36/36 vs the C# oracle): functions/extensions/overloads, records/classes with
// const+static members and field initializers, closures, iterators (a `def` with `yield` is a generator),
// operators (real dunders) + computed properties (`@property`), enums (int class-attrs) + discriminated
// unions (tagged dicts) + match (a lambda-bound ternary chain), exceptions (raise / try-except-finally,
// Error->Exception), inheritance, disposal (use -> try/finally), nullability (`?.`/`??`/`!`), string
// interpolation, tuples, the std-module python arms (Math/List/strings via ir::Bound), and §3.C integer
// faithfulness (width-masked overflow + truncating div/rem). Declarations stay per-target by shape; the
// shared statement engine (emitter_base) serves all three targets.

namespace mintplayer::polyglot {
namespace {

// Python's declarative Spec — the engine consults it for block style, statement terminator, and throw
// keyword. The type/operator/bracket tables stay empty: Python emits no type annotations, spells operators
// through pyOp(), and brackets list/tuple literals inline — those are its imperative Hook tier, not data.
const BackendSpec& pythonSpec() {
    static const BackendSpec spec = {
        "python",
        {}, {}, {}, {},              // scalarType / intSuffix / binaryOp / delimited: unused by this backend
        BlockStyle::ColonIndent,     // colon+indent, no braces
        "",                          // no statement terminator
        "raise",                     // `throw v` -> `raise v`
    };
    return spec;
}

bool isIntType(const TypeRef& t) {
    if (t.kind != TypeRef::Kind::Named) return false;
    const std::string& n = t.name;
    return n == "i8" || n == "i16" || n == "i32" || n == "i64" ||
           n == "u8" || n == "u16" || n == "u32" || n == "u64";
}

bool isFloatType(const TypeRef& t) {
    return t.kind == TypeRef::Kind::Named && (t.name == "f32" || t.name == "f64");
}

// Operator symbol -> Python dunder. Python HAS operator overloading, so an `operator fn plus` emits a real
// `__add__` and `a + b` dispatches to it at the use site (no method-call rewrite as C#-less TS needs).
const char* opDunder(const std::string& sym) {
    if (sym == "+")  return "__add__";
    if (sym == "-")  return "__sub__";
    if (sym == "*")  return "__mul__";
    if (sym == "/")  return "__truediv__";
    if (sym == "%")  return "__mod__";
    if (sym == "==") return "__eq__";
    if (sym == "<")  return "__lt__";
    if (sym == "<=") return "__le__";
    if (sym == ">")  return "__gt__";
    if (sym == ">=") return "__ge__";
    return nullptr;
}

class PythonEmitter : public EmitterBase {
public:
    std::string emit(const ir::Module& m) {
        out_.clear();
        indent_ = 0;
        for (const auto& et : m.externTypes) externTypes_[et.name] = &et; // for Error->Exception etc. spellings
        for (const auto& e : m.enums) emitEnum(e);
        for (const auto& u : m.unions) emitUnion(u);
        for (const auto& r : m.records) emitRecord(r);
        for (const auto& c : m.classes) emitClass(c);
        for (const auto& g : m.globals)
            line(g.name + " = " + (g.init ? emitExpr(*g.init) : "None"));
        for (const auto& fn : m.functions) {
            if (!fn.actualTarget.empty() && fn.actualTarget != "python") continue; // other target's `actual`
            emitFunction(fn);
        }
        for (const auto& fn : m.extensions) emitFunction(fn); // extensions -> free fns; `x.m()` calls `m(x)`
        for (const auto& fn : m.functions)
            if (fn.isEntry) { line(pyName(fn.mangledName) + "()"); break; } // top-level entry call
        if (needsIdiv_) out_ = idivPrelude() + out_; // C#-faithful truncating int `/` and `%`
        return out_;
    }

    // Truncate-toward-zero integer division + remainder (Python `//`/`%` floor; .NET truncates). `_pg_irem`
    // is `a - (a/b)*b` so it matches C#'s sign-of-dividend remainder. Prepended only when used.
    static std::string idivPrelude() {
        return "def _pg_idiv(a, b):\n"
               "    q = a // b\n"
               "    return q + 1 if (q < 0 and q * b != a) else q\n"
               "def _pg_irem(a, b):\n"
               "    return a - _pg_idiv(a, b) * b\n";
    }

private:
    const BackendSpec& spec() const override { return pythonSpec(); }
    std::string localDecl(const std::string& name, bool) override { return pyId(name); }  // bare `name = ...`
    std::string yieldStmt(const std::string& v, bool has) override { return has ? "yield " + v : "return"; }
    std::string rethrowStmt() override { return "raise"; }            // bare `raise` re-raises the active exception

    std::unordered_map<std::string, const ir::ExternType*> externTypes_;
    int tmp_ = 0; // fresh-name counter for the walrus temporaries that keep `?.`/`??` single-evaluated

    // The Python spelling of a named type: an `extern class`'s pyType template (Error -> "Exception") if one
    // is registered, else the bare name (user types, which Python needs no annotation for). Used for class
    // bases, catch types, and ctor `$T` — the spots where a native-backed type must read as its target name.
    std::string pyTypeName(const TypeRef& t) {
        if (t.kind == TypeRef::Kind::Named) {
            if (auto it = externTypes_.find(t.name); it != externTypes_.end() && !it->second->pyType.empty())
                return substTypeTmpl(it->second->pyType, t.args);
        }
        return t.name;
    }

    // Substitute `$0,$1,…` in a type-spelling template with the rendered type args (none needed for Python's
    // erased generics today, but kept symmetric with the C#/TS type templates).
    std::string substTypeTmpl(const std::string& tmpl, const std::vector<TypeRef>& args) {
        std::string out;
        for (std::size_t i = 0; i < tmpl.size();) {
            if (tmpl[i] == '$' && i + 1 < tmpl.size() && std::isdigit(static_cast<unsigned char>(tmpl[i + 1]))) {
                std::size_t idx = static_cast<std::size_t>(tmpl[i + 1] - '0');
                if (idx < args.size()) out += pyTypeName(args[idx]);
                i += 2;
            } else out += tmpl[i++];
        }
        return out;
    }

    // An overloaded function's mangled name (`area$i32`) carries a `$` that's invalid in a Python identifier;
    // map it to `_` consistently at every def and call site so the overloads stay distinct and callable.
    static std::string pyName(const std::string& s) {
        std::string out = s;
        for (char& c : out) if (c == '$') c = '_';
        return out;
    }

    // Escape a source identifier that collides with a Python keyword (e.g. a local `def`/`class`/`in`) by
    // suffixing `_`. Applied uniformly to every binding site and reference so the renaming stays consistent.
    static std::string pyId(const std::string& s) {
        static const std::unordered_set<std::string> kw = {
            "False","None","True","and","as","assert","async","await","break","class","continue","def","del",
            "elif","else","except","finally","for","from","global","if","import","in","is","lambda","nonlocal",
            "not","or","pass","raise","return","try","while","with","yield","match","case"};
        return kw.count(s) ? s + "_" : s;
    }

    // A parameter: escaped name + optional `= default` (Python supports default args natively).
    std::string param(const ir::Param& p) {
        return pyId(p.name) + (p.defaultValue ? " = " + emitExpr(*p.defaultValue) : "");
    }

    void emitFunction(const ir::Function& fn) {
        // Extensions lower to plain free functions (`self` is the receiver param) with no mangledName.
        std::string sig = "def " + pyName(fn.mangledName.empty() ? fn.name : fn.mangledName) + "(";
        for (std::size_t i = 0; i < fn.params.size(); ++i) { if (i) sig += ", "; sig += param(fn.params[i]); }
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

    // An enum -> a class of int class-attributes, so `Color.Green` reads as the int (matching C#/TS, where
    // enums are ints). `match c { Green => … }` then compares `_m == Color.Green`.
    void emitEnum(const ir::Enum& e) {
        openBlock("class " + e.name);
        ++indent_;
        if (e.cases.empty()) line("pass");
        for (const auto& c : e.cases) line(c.name + " = " + std::to_string(c.value));
        --indent_;
    }

    // A discriminated union needs NO runtime declaration: each case is built as a tagged dict at the use site
    // (`{"tag": "Circle", "r": …}`) and matched on `_m["tag"]` — mirroring TS's erased type alias. The comment
    // documents the representation for a reader of the .py.
    void emitUnion(const ir::Union& u) {
        line("# union " + u.name + " -> tagged dicts: {\"tag\": <case>, <field>: <value>, ...}");
    }

    // A record -> a Python class with an __init__ (positional fields) and a structural __eq__. Python `==`
    // dispatches to __eq__, so equality is just field-wise `==` joined by `and` (nested records recurse via
    // their own __eq__ — no explicit recursion as the TS backend needs). Methods, if any, emit as defs.
    void emitRecord(const ir::Record& r) {
        openBlock("class " + r.name);
        ++indent_;
        std::string ctor = "def __init__(self";
        for (const auto& f : r.fields) ctor += ", " + pyId(f.name);
        openBlock(ctor + ")");
        ++indent_;
        if (r.fields.empty()) line("pass");
        for (const auto& f : r.fields) line("self." + f.name + " = " + pyId(f.name));
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

    // A class -> a Python class. `const`/`static` fields become class-level attributes (read as `Owner.name`).
    // Instance-field initializers run in __init__ (after super, before the init body); plain instance fields
    // with no initializer need no declaration (Python sets them on first assignment in the init body).
    void emitClass(const ir::Class& c) {
        std::string head = "class " + c.name;
        if (!c.bases.empty()) {
            head += "(";
            for (std::size_t i = 0; i < c.bases.size(); ++i) { if (i) head += ", "; head += pyTypeName(c.bases[i]); }
            head += ")";
        }
        openBlock(head);
        ++indent_;
        bool any = false;
        for (const auto& f : c.fields)
            if (f.isStatic && f.init) { line(f.name + " = " + emitExpr(*f.init)); any = true; } // class attribute

        bool hasFieldInit = false;
        for (const auto& f : c.fields) if (!f.isStatic && f.init) hasFieldInit = true;
        if (c.hasInit || hasFieldInit) {
            std::string sig = "def __init__(self";
            for (const auto& p : c.initParams) sig += ", " + param(p); // empty when synthesized for field inits
            openBlock(sig + ")");
            ++indent_;
            if (c.hasSuper) {
                std::vector<std::string> sa;
                for (const auto& a : c.superArgs) sa.push_back(emitExpr(*a));
                line("super().__init__" + renderArgs(sa));
            }
            for (const auto& f : c.fields)
                if (!f.isStatic && f.init) line("self." + f.name + " = " + emitExpr(*f.init));
            for (const auto& s : c.initBody) emitStmt(*s);
            if (!c.hasSuper && !hasFieldInit && c.initBody.empty()) line("pass");
            --indent_;
            any = true;
        }
        for (const auto& m : c.methods) { emitMethod(m); any = true; }
        if (!any) line("pass");
        --indent_;
    }

    // A record/class member -> a `def`. Most members take a leading `self`; four shapes map idiomatically:
    //   Method     -> `def name(self, ...)`
    //   static fn  -> `@staticmethod` + `def name(...)` (no self; called `Type.name(...)`)
    //   Operator   -> `def __add__(self, ...)` (Python dispatches `a + b` to the dunder natively)
    //   Property   -> `@property` + `def name(self)` (accessed as `a.prop`, no call)
    void emitMethod(const ir::Method& m) {
        std::string name = m.name;
        if (m.kind == ir::MethodKind::Operator) {
            if (const char* d = opDunder(m.opSymbol)) name = d;
        } else if (m.kind == ir::MethodKind::Property) {
            line("@property");
        }
        if (m.isStatic) line("@staticmethod");
        std::string sig = "def " + name + "(";
        bool first = true;
        if (!m.isStatic) { sig += "self"; first = false; } // static members take no receiver
        for (const auto& p : m.params) { if (!first) sig += ", "; first = false; sig += param(p); }
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
        // For and Try are the non-shared statements: their shape (not just spelling) diverges per target.
        if (s.kind == ir::StmtKind::For) {
            const auto& f = static_cast<const ir::For&>(s);
            if (f.isRange) {
                std::string hi = f.inclusive ? "(" + emitExpr(*f.rangeEnd) + ") + 1" : emitExpr(*f.rangeEnd);
                headBlock("for " + pyId(f.binding) + " in range(" + emitExpr(*f.rangeStart) + ", " + hi + ")", f.body);
            } else if (!f.tupleBindings.empty()) {
                std::string names;
                for (std::size_t i = 0; i < f.tupleBindings.size(); ++i) { if (i) names += ", "; names += pyId(f.tupleBindings[i]); }
                headBlock("for " + names + " in " + emitExpr(*f.iterable), f.body);
            } else {
                headBlock("for " + pyId(f.binding) + " in " + emitExpr(*f.iterable), f.body);
            }
            return;
        }
        if (s.kind == ir::StmtKind::Try) { emitTry(static_cast<const ir::Try&>(s)); return; }
        line("# polyglot: statement kind not yet supported for the python target");
    }

    // try/except/finally. Python has native typed `except Type as e:`, so a typed catch list maps directly
    // (C# needs the same; only TS lowers to a dispatch chain). An untyped catch-all -> `except Exception`;
    // a `when` guard re-raises when it fails (`if not (guard): raise`) so a later clause can handle it.
    void emitTry(const ir::Try& t) {
        openBlock("try");
        blockBody(t.body);
        for (const auto& c : t.catches) {
            std::string head = "except ";
            head += c.type.name.empty() ? "Exception" : pyTypeName(c.type);
            if (!c.binding.empty()) head += " as " + c.binding;
            openBlock(head);
            ++indent_;
            if (c.guard) line("if not (" + emitExpr(*c.guard) + "): raise");
            --indent_;
            blockBody(c.body);
        }
        if (t.hasFinally) { openBlock("finally"); blockBody(t.finallyBody); }
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

    // Python operator spelling: `and`/`or` for `&&`/`||`. (Fixed-width int arithmetic — wrap, trunc-div,
    // trunc-rem — is handled in the Binary case below, not here; non-int `/` stays float division.)
    std::string pyOp(const std::string& op) {
        if (op == "&&") return "and";
        if (op == "||") return "or";
        return op;
    }

    bool needsIdiv_ = false; // set when a fixed-width int `/`/`%` is emitted -> prepend the trunc helpers

    // §3.C integer faithfulness: wrap an int result to its width so it overflows like .NET (Python ints are
    // arbitrary-precision). Unsigned masks; signed masks then sign-extends via the (m ^ SIGN) - SIGN trick —
    // `x` appears once, so it's safe to inline without a temporary.
    std::string wrapInt(const TypeRef& t, const std::string& x) {
        const std::string& n = t.name;
        if (n == "u8")  return "((" + x + ") & 0xff)";
        if (n == "u16") return "((" + x + ") & 0xffff)";
        if (n == "u32") return "((" + x + ") & 0xffffffff)";
        if (n == "u64") return "((" + x + ") & 0xffffffffffffffff)";
        if (n == "i8")  return "((((" + x + ") & 0xff) ^ 0x80) - 0x80)";
        if (n == "i16") return "((((" + x + ") & 0xffff) ^ 0x8000) - 0x8000)";
        if (n == "i32") return "((((" + x + ") & 0xffffffff) ^ 0x80000000) - 0x80000000)";
        if (n == "i64") return "((((" + x + ") & 0xffffffffffffffff) ^ 0x8000000000000000) - 0x8000000000000000)";
        return x;
    }

    // ---- match -> a lambda-bound ternary chain --------------------------------------------------------
    // Python has no match expression (3.10's is a statement), so a match lowers to
    //   (lambda _m: <v0> if <c0> else (<v1> if <c1> else <v_last>))(scrutinee)
    // The scrutinee is bound once as `_m`; the LAST arm is unconditional (sema guarantees exhaustiveness),
    // supplying the ternary's required else. Pattern bindings come into scope via an inner lambda per arm
    // (`(lambda r: <body>)(_m["r"])`) — no statement-level binding needed.

    // (name, access-on-`_m`) pairs a pattern binds: Ctor fields, or the whole scrutinee for a bare binding.
    std::vector<std::pair<std::string, std::string>> armBinders(const ir::Pattern& p) {
        std::vector<std::pair<std::string, std::string>> bs;
        if (p.kind == ir::PatternKind::Ctor)
            for (const auto& b : p.binders) bs.push_back({b.binding, "_m[\"" + b.field + "\"]"});
        else if (p.kind == ir::PatternKind::Binding)
            bs.push_back({p.binding, "_m"});
        return bs;
    }

    // Wrap `inner` so the binders are in scope: `(lambda a, b: inner)(_m["x"], _m["y"])`; identity if none.
    std::string wrapBinders(const std::vector<std::pair<std::string, std::string>>& bs, const std::string& inner) {
        if (bs.empty()) return inner;
        std::string params, args;
        for (std::size_t i = 0; i < bs.size(); ++i) {
            if (i) { params += ", "; args += ", "; }
            params += bs[i].first;
            args += bs[i].second;
        }
        return "(lambda " + params + ": " + inner + ")(" + args + ")";
    }

    // The boolean test that selects this arm (binders already bound for a guard that references them).
    std::string armCond(const ir::MatchArm& a, const std::vector<std::pair<std::string, std::string>>& bs) {
        const ir::Pattern& p = a.pattern;
        std::string base;
        switch (p.kind) {
            case ir::PatternKind::Wildcard:
            case ir::PatternKind::Binding: base = "True"; break;
            case ir::PatternKind::Literal:  base = "_m == " + emitExpr(*p.literal); break;
            case ir::PatternKind::EnumCase: base = "_m == " + p.enumType + "." + p.enumCase; break;
            case ir::PatternKind::Ctor:     base = "_m[\"tag\"] == \"" + p.ctorCase + "\""; break;
        }
        if (!a.guard) return base;
        std::string g = wrapBinders(bs, emitExpr(*a.guard));
        return base == "True" ? g : base + " and " + g;
    }

    std::string matchChain(const std::vector<ir::MatchArm>& arms, std::size_t i) {
        const ir::MatchArm& a = arms[i];
        auto bs = armBinders(a.pattern);
        std::string value = wrapBinders(bs, emitExpr(*a.body));
        if (i + 1 == arms.size()) return value; // exhaustive: final arm is the unconditional else
        return value + " if " + armCond(a, bs) + " else " + matchChain(arms, i + 1);
    }

    // Substitute a bound FFI template's placeholders: `$this`->receiver, `$T`->the type name (ctor templates),
    // `$0`,`$1`,…->args. Mirrors the C#/TS backends so a std/plugin binding's python arm renders the same way.
    std::string substTemplate(const std::string& tmpl, const ir::Bound& b) {
        std::string out;
        for (std::size_t i = 0; i < tmpl.size();) {
            if (tmpl.compare(i, 5, "$this") == 0) { if (b.receiver) out += emitExpr(*b.receiver); i += 5; }
            else if (tmpl.compare(i, 2, "$T") == 0) { out += pyTypeName(b.type); i += 2; }
            else if (tmpl[i] == '$' && i + 1 < tmpl.size() && std::isdigit(static_cast<unsigned char>(tmpl[i + 1]))) {
                std::size_t idx = static_cast<std::size_t>(tmpl[i + 1] - '0');
                if (idx < b.args.size()) out += emitExpr(*b.args[idx]);
                i += 2;
            } else out += tmpl[i++];
        }
        return out;
    }

    std::string emitExpr(const ir::Expr& e) override {
        switch (e.kind) {
            case ir::ExprKind::Int:    return static_cast<const ir::IntLit&>(e).text;
            case ir::ExprKind::Float:  return static_cast<const ir::FloatLit&>(e).text;
            case ir::ExprKind::Bool:   return static_cast<const ir::BoolLit&>(e).value ? "True" : "False";
            case ir::ExprKind::Null:   return "None";
            case ir::ExprKind::Str:    return renderString(static_cast<const ir::StrLit&>(e).value);
            case ir::ExprKind::Var:    return pyId(static_cast<const ir::Var&>(e).name);
            case ir::ExprKind::This:   return "self";
            case ir::ExprKind::Extern: return static_cast<const ir::Extern&>(e).code; // raw Python verbatim
            case ir::ExprKind::Unary: {
                const auto& u = static_cast<const ir::Unary&>(e);
                std::string operand = u.operand->kind == ir::ExprKind::Binary ? "(" + emitExpr(*u.operand) + ")"
                                                                              : emitExpr(*u.operand);
                if (u.op == "-" && isIntType(e.type)) return wrapInt(e.type, "-" + operand); // negate can overflow (i8 min)
                return (u.op == "!" ? "not " : u.op) + operand;
            }
            case ir::ExprKind::Binary: {
                const auto& b = static_cast<const ir::Binary&>(e);
                if (b.op == "??") { // `a ?? d` -> `(t if (t := a) is not None else d)` (single eval of a)
                    std::string t = "__c" + std::to_string(tmp_++);
                    return "(" + t + " if (" + t + " := " + emitExpr(*b.lhs) + ") is not None else " + emitExpr(*b.rhs) + ")";
                }
                int p = operatorPrecedence(b.op);
                // §3.C: fixed-width int arithmetic wraps at each boundary; `/`/`%` truncate toward zero like
                // .NET (Python `//`/`%` floor), via the prelude helpers. Comparisons/logic (bool result) and
                // non-int `/` (float) fall through to the plain spelling.
                if (isIntType(e.type) && (b.op == "+" || b.op == "-" || b.op == "*" || b.op == "/" || b.op == "%")) {
                    std::string inner;
                    if (b.op == "/") { needsIdiv_ = true; inner = "_pg_idiv(" + emitExpr(*b.lhs) + ", " + emitExpr(*b.rhs) + ")"; }
                    else if (b.op == "%") { needsIdiv_ = true; inner = "_pg_irem(" + emitExpr(*b.lhs) + ", " + emitExpr(*b.rhs) + ")"; }
                    else inner = child(*b.lhs, p, false) + " " + b.op + " " + child(*b.rhs, p, true);
                    return wrapInt(e.type, inner);
                }
                return child(*b.lhs, p, false) + " " + pyOp(b.op) + " " + child(*b.rhs, p, true);
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
                return pyName(c.mangledCallee) + renderArgs(args);
            }
            case ir::ExprKind::Member: {
                const auto& m = static_cast<const ir::Member&>(e);
                if (m.nullSafe) { // `obj?.field` -> `(t.field if (t := obj) is not None else None)` (single eval)
                    std::string t = "__o" + std::to_string(tmp_++);
                    return "(" + t + "." + m.field + " if (" + t + " := " + emitExpr(*m.object) + ") is not None else None)";
                }
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
                if (mc.isExtension) { // free-function form `name(obj, args)` — `x.m()` cannot stay method syntax
                    std::vector<std::string> args;
                    args.push_back(emitExpr(*mc.object));
                    for (const auto& a : mc.args) args.push_back(emitExpr(*a));
                    return mc.method + renderArgs(args);
                }
                std::string recv = mc.staticType.empty() ? atom(*mc.object) : mc.staticType;
                std::vector<std::string> args;
                for (const auto& a : mc.args) args.push_back(emitExpr(*a));
                return recv + "." + mc.method + renderArgs(args);
            }
            case ir::ExprKind::Lambda: {
                const auto& l = static_cast<const ir::Lambda&>(e);
                std::string params;
                for (std::size_t i = 0; i < l.params.size(); ++i) { if (i) params += ", "; params += pyId(l.params[i].name); }
                // Python lambdas are single-expression; a statement-bodied lambda needs a nested def (later).
                if (l.exprBodied) return "lambda " + params + ": " + emitExpr(*l.body);
                return "__py_unsupported_block_lambda__";
            }
            case ir::ExprKind::Bound: // a portable std method/property resolved to its python FFI template
                return substTemplate(static_cast<const ir::Bound&>(e).pyTemplate, static_cast<const ir::Bound&>(e));
            case ir::ExprKind::ListLit: { // `[a, b, c]` -> a Python list (List<T> maps to the native list)
                const auto& l = static_cast<const ir::ListLit&>(e);
                std::vector<std::string> els;
                for (const auto& el : l.elements) els.push_back(emitExpr(*el));
                return renderDelimited({"[", ", ", "]"}, els);
            }
            case ir::ExprKind::Index: { // `recv[i]` -> Python subscription (lists index directly)
                const auto& ix = static_cast<const ir::Index&>(e);
                return atom(*ix.receiver) + "[" + emitExpr(*ix.index) + "]";
            }
            case ir::ExprKind::Tuple: { // `(a, b)` -> a Python tuple
                const auto& t = static_cast<const ir::Tuple&>(e);
                std::vector<std::string> els;
                for (const auto& el : t.elements) els.push_back(emitExpr(*el));
                std::string s = renderDelimited({"(", ", ", ")"}, els);
                return els.size() == 1 ? "(" + els[0] + ",)" : s; // 1-tuples need a trailing comma in Python
            }
            case ir::ExprKind::Interp: { // string interpolation -> `"lit" + str(hole) + …` (str() like C# ToString)
                const auto& in = static_cast<const ir::Interp&>(e);
                std::string s = renderString(in.chunks[0]);
                for (std::size_t i = 0; i < in.holes.size(); ++i)
                    s += " + str(" + emitExpr(*in.holes[i]) + ") + " + renderString(in.chunks[i + 1]);
                return s;
            }
            case ir::ExprKind::MakeCase: { // union-case construction -> a tagged dict
                const auto& mc = static_cast<const ir::MakeCase&>(e);
                std::string s = "{\"tag\": \"" + mc.caseName + "\"";
                for (const auto& f : mc.fields) s += ", \"" + f.name + "\": " + emitExpr(*f.value);
                return s + "}";
            }
            case ir::ExprKind::Match: {
                const auto& m = static_cast<const ir::Match&>(e);
                return "(lambda _m: " + matchChain(m.arms, 0) + ")(" + emitExpr(*m.scrutinee) + ")";
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

};

} // namespace

std::string emitPython(const ir::Module& module) {
    PythonEmitter emitter;
    return emitter.emit(module);
}

} // namespace mintplayer::polyglot
