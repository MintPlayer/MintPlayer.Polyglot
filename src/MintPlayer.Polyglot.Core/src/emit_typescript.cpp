#include "mintplayer/polyglot/emit.hpp"

#include <cctype>
#include <string>
#include <unordered_set>

#include "mintplayer/polyglot/backend_spec.hpp"

// Hand-written IR -> TypeScript pretty-printer. Walks the typed IR; emits free `function`s, maps the
// `print` intrinsic -> console.log, and appends a top-level call to the entry function. Output stays
// plain enough to run under Node's type-stripping (the P2 differential conformance test relies on it).

namespace mintplayer::polyglot {

namespace {

// The TS backend's declarative data (P9 slice 1: the scalar type-leaf table). Unlike C#, TS maps `char` ->
// `string` and the 64-bit ints -> `bigint`; structural cases (List/tuple/function/nullable) stay in tsType.
const BackendSpec& typescriptSpec() {
    static const BackendSpec spec = {
        "typescript",
        {{"unit", "void"}, {"bool", "boolean"}, {"string", "string"}, {"char", "string"},
         {"i64", "bigint"}, {"u64", "bigint"},
         {"i8", "number"}, {"i16", "number"}, {"i32", "number"}, {"u8", "number"}, {"u16", "number"},
         {"u32", "number"}, {"f32", "number"}, {"f64", "number"}},
        {{"i64", "n"}, {"u64", "n"}}, // intSuffix: 64-bit ints are BigInt literals (`7n`)
        {{"==", "==="}, {"!=", "!=="}}, // binaryOp: always strict equality, never JS loose ==/!=
        {{"tuple", {"[", ", ", "]"}}}, // delimited: TS tuple `[a, b]`
    };
    return spec;
}

// The current module's `extern class` type map; see the identical note in emit_csharp.cpp. Set per emit().
const std::unordered_map<std::string, const ir::ExternType*>* g_externTypes = nullptr;

std::string tsType(const TypeRef& t);

// Substitute a type-spelling template's `$0,$1,…` with the rendered type args (List's "$0[]" with [i32]
// -> "number[]").
std::string substTypeTmpl(const std::string& tmpl, const std::vector<TypeRef>& args) {
    std::string out;
    for (std::size_t i = 0; i < tmpl.size();) {
        if (tmpl[i] == '$' && i + 1 < tmpl.size() && std::isdigit(static_cast<unsigned char>(tmpl[i + 1]))) {
            std::size_t idx = static_cast<std::size_t>(tmpl[i + 1] - '0');
            if (idx < args.size()) out += tsType(args[idx]);
            i += 2;
        } else out += tmpl[i++];
    }
    return out;
}

std::string tsType(const TypeRef& t) {
    if (t.kind == TypeRef::Kind::Named) {
        if (t.nullable) { TypeRef base = t; base.nullable = false; return tsType(base) + " | null"; }
        if (auto it = typescriptSpec().scalarType.find(t.name); it != typescriptSpec().scalarType.end()) return it->second;
        if (t.name.empty()) return "unknown";
        // A native-backed `extern class` (e.g. List -> `$0[]`) declares its spelling via a `type { … }` block.
        if (g_externTypes) {
            if (auto it = g_externTypes->find(t.name); it != g_externTypes->end() && !it->second->tsType.empty())
                return substTypeTmpl(it->second->tsType, t.args);
        }
        std::string name = t.name; // user nominal types; Error/Iterable spell via the core-prelude mapping above
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
    if (t.kind == TypeRef::Kind::Tuple) { // tuple type `[A, B]`
        std::string s = "[";
        for (std::size_t i = 0; i < t.args.size(); ++i) { if (i) s += ", "; s += tsType(t.args[i]); }
        return s + "]";
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
bool isI64(const TypeRef& t) { return t.kind == TypeRef::Kind::Named && (t.name == "i64" || t.name == "u64"); }
bool isPrimNumeric(const std::string& n) {
    return n == "i8" || n == "i16" || n == "i32" || n == "i64" || n == "u8" || n == "u16" ||
           n == "u32" || n == "u64" || n == "f32" || n == "f64";
}
// A 32-bit-or-narrower integer type, normalized with JS bitwise ops at each operation boundary.
bool isSmallInt(const TypeRef& t) {
    if (t.kind != TypeRef::Kind::Named) return false;
    const std::string& n = t.name;
    return n == "i8" || n == "i16" || n == "i32" || n == "u8" || n == "u16" || n == "u32";
}
// Coerce a JS number back into the value range of a 32-bit-or-narrower int type (§3.C overflow masking).
std::string narrowTs(const std::string& n, const std::string& inner) {
    if (n == "i8")  return "(" + inner + " << 24 >> 24)";
    if (n == "i16") return "(" + inner + " << 16 >> 16)";
    if (n == "u8")  return "(" + inner + " & 0xff)";
    if (n == "u16") return "(" + inner + " & 0xffff)";
    if (n == "u32") return "(" + inner + " >>> 0)";
    return "(" + inner + " | 0)"; // i32
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

// A numeric conversion, lowered per source/target representation. The hard boundary is BigInt (i64/u64):
// crossing into it needs BigInt(); crossing out needs a width-exact BigInt.asIntN/asUintN before Number()
// (a plain Number() of a >2^53 BigInt would lose the low bits a C# `(int)long` keeps).
std::string tsConvert(const TypeRef& from, const TypeRef& to, const std::string& x) {
    bool toBig = isI64(to), fromBig = isI64(from);
    bool toFloat = to.kind == TypeRef::Kind::Named && (to.name == "f32" || to.name == "f64");
    bool fromFloat = from.kind == TypeRef::Kind::Named && (from.name == "f32" || from.name == "f64");
    if (from.kind == TypeRef::Kind::Named && to.kind == TypeRef::Kind::Named && from.name == to.name) return x;
    if (toBig) {
        if (fromBig)   return std::string(to.name == "u64" ? "BigInt.asUintN(64, " : "BigInt.asIntN(64, ") + x + ")";
        if (fromFloat) return "BigInt(Math.trunc(" + x + "))";
        return "BigInt(" + x + ")";
    }
    if (toFloat) return fromBig ? "Number(" + x + ")" : x; // number<->number is a no-op (f32 rides f64)
    if (fromBig) { // BigInt -> 32-bit-or-narrower int: narrow the BigInt exactly, then to a number
        int w = (to.name == "i8" || to.name == "u8") ? 8 : (to.name == "i16" || to.name == "u16") ? 16 : 32;
        const char* fn = to.name[0] == 'u' ? "asUintN" : "asIntN";
        return "Number(BigInt." + std::string(fn) + "(" + std::to_string(w) + ", " + x + "))";
    }
    if (fromFloat) return narrowTs(to.name, "Math.trunc(" + x + ")");
    return narrowTs(to.name, x);
}

class TypeScriptEmitter {
public:
    std::string emit(const ir::Module& m) {
        out_.clear();
        indent_ = 0;
        recordNames_.clear();
        recordFields_.clear();
        indexerTypes_.clear();
        interfaceNames_.clear();
        for (const auto& i : m.interfaces) interfaceNames_.insert(i.name); // a class `implements` these, `extends` a class
        externMap_.clear();
        // Types with an `operator fn get` indexer: TS has no `[]` overload, so `recv[i]` becomes `recv.get(i)`.
        auto noteIndexer = [&](const std::string& name, const std::vector<ir::Method>& ms) {
            for (const auto& m : ms) if (m.kind == ir::MethodKind::Operator && m.name == "get") indexerTypes_.insert(name);
        };
        for (const auto& c : m.classes) noteIndexer(c.name, c.methods);
        for (const auto& r : m.records) noteIndexer(r.name, r.methods);
        for (const auto& et : m.externTypes) externMap_[et.name] = &et;
        g_externTypes = &externMap_;
        for (const auto& r : m.records) { // records compare structurally (§3.C); a TS record is a class
            recordNames_.insert(r.name);
            for (const auto& f : r.fields) recordFields_[r.name].push_back(f.name); // ctor arg order, for `with`
        }
        for (const auto& e : m.enums) emitEnum(e);
        for (const auto& i : m.interfaces) emitInterface(i);
        for (const auto& u : m.unions) emitUnion(u);
        for (const auto& r : m.records) emitRecord(r);
        for (const auto& c : m.classes) emitClass(c);
        for (const auto& g : m.globals) // top-level const/let
            line(std::string(g.isConst ? "const " : "let ") + g.name + ": " + tsType(g.type) + (g.init ? " = " + emitExpr(*g.init) : "") + ";");
        for (const auto& f : m.extensions) emitExtension(f);
        for (const auto& fn : m.functions) {
            if (!fn.actualTarget.empty() && fn.actualTarget != "typescript") continue; // other target's `actual`
            emitFunction(fn);
        }
        for (const auto& fn : m.functions) {
            if (fn.isEntry) { line(fn.mangledName + "();"); break; }
        }
        return out_;
    }

private:
    std::string out_;
    int indent_ = 0;
    std::unordered_set<std::string> recordNames_;
    std::unordered_map<std::string, std::vector<std::string>> recordFields_; // record name -> ctor field order
    std::unordered_set<std::string> indexerTypes_; // types with an `operator get` -> `recv[i]` is `recv.get(i)`
    std::unordered_set<std::string> interfaceNames_; // names declared as interfaces (class implements vs extends)
    std::unordered_map<std::string, const ir::ExternType*> externMap_; // backs g_externTypes for this emit
    int tmp_ = 0; // fresh-name counter (e.g. the `with`-copy IIFE binder)

    bool isRecordType(const TypeRef& t) const {
        return t.kind == TypeRef::Kind::Named && recordNames_.count(t.name) != 0;
    }

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

    void emitInterface(const ir::Interface& it) {
        std::string head = "interface " + it.name + tsGenerics(it.generics);
        if (!it.bases.empty()) {
            head += " extends ";
            for (std::size_t i = 0; i < it.bases.size(); ++i) { if (i) head += ", "; head += tsType(it.bases[i]); }
        }
        line(head + " {");
        ++indent_;
        for (const auto& m : it.methods) { // signature only
            std::string sig = m.name + tsGenerics(m.generics) + "(";
            for (std::size_t i = 0; i < m.params.size(); ++i) { if (i) sig += ", "; sig += tsParam(m.params[i]); }
            line(sig + "): " + tsType(m.returnType) + ";");
        }
        --indent_;
        line("}");
    }

    // Union -> a discriminated-union type alias (stripped at runtime; construction makes {tag,...} objects).
    void emitUnion(const ir::Union& u) {
        std::string s = "type " + u.name + tsGenerics(u.generics) + " = "; // `type Option<T> = …`
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
        std::string head = "class " + r.name + tsGenerics(r.generics);
        if (!r.bases.empty()) { // a record implements interfaces
            head += " implements ";
            for (std::size_t i = 0; i < r.bases.size(); ++i) { if (i) head += ", "; head += tsType(r.bases[i]); }
        }
        line(head + " {");
        ++indent_;
        for (const auto& f : r.fields) line(f.name + ": " + tsType(f.type) + ";");
        std::string ctor = "constructor(";
        for (std::size_t i = 0; i < r.fields.size(); ++i) { if (i) ctor += ", "; ctor += r.fields[i].name + ": " + tsType(r.fields[i].type); }
        line(ctor + ") {");
        ++indent_;
        for (const auto& f : r.fields) line("this." + f.name + " = " + f.name + ";");
        --indent_;
        line("}");
        emitRecordEquals(r);
        for (const auto& m : r.methods) emitMethod(m);
        --indent_;
        line("}");
    }

    // Structural value equality (§3.C): records are equal when every field is — record fields compare
    // recursively via their own .equals(); scalar fields via ===. (C# records synthesize this natively.)
    void emitRecordEquals(const ir::Record& r) {
        line("equals(other: " + r.name + "): boolean {");
        ++indent_;
        if (r.fields.empty()) {
            line("return true;");
        } else {
            std::string cond;
            for (std::size_t i = 0; i < r.fields.size(); ++i) {
                if (i) cond += " && ";
                const std::string& f = r.fields[i].name;
                cond += isRecordType(r.fields[i].type) ? "this." + f + ".equals(other." + f + ")"
                                                       : "this." + f + " === other." + f;
            }
            line("return " + cond + ";");
        }
        --indent_;
        line("}");
    }

    // A mutable reference type: explicit fields, a constructor (`init`), and methods.
    void emitClass(const ir::Class& c) {
        std::string head = "class " + c.name + tsGenerics(c.generics);
        // A class `extends` its (single) base class and `implements` its interface bases — distinct in TS.
        std::string ext, impl;
        for (const auto& b : c.bases) {
            if (interfaceNames_.count(b.name)) { if (!impl.empty()) impl += ", "; impl += tsType(b); }
            else ext = tsType(b);
        }
        if (!ext.empty()) head += " extends " + ext;
        if (!impl.empty()) head += " implements " + impl;
        line(head + " {");
        ++indent_;
        for (const auto& f : c.fields) {
            std::string mods = f.isStatic ? (f.isMutable ? "static " : "static readonly ") : "";
            std::string decl = mods + f.name + ": " + tsType(f.type);
            if (f.init) decl += " = " + emitExpr(*f.init);
            line(decl + ";");
        }
        if (c.hasInit) {
            std::string sig = "constructor(";
            for (std::size_t i = 0; i < c.initParams.size(); ++i) { if (i) sig += ", "; sig += tsParam(c.initParams[i]); }
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

    // `name: T` or `name: T = default` — a parameter declaration with its optional default value.
    std::string tsParam(const ir::Param& p) {
        std::string s = p.name + ": " + tsType(p.type);
        if (p.defaultValue) s += " = " + emitExpr(*p.defaultValue);
        return s;
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
        std::string sig = std::string(m.isStatic ? "static " : "") + m.name + tsGenerics(m.generics) + "(";
        for (std::size_t i = 0; i < m.params.size(); ++i) { if (i) sig += ", "; sig += tsParam(m.params[i]); }
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

    // Substitute a binding template: `$this` -> receiver, `$0`,`$1`,… -> args, each rendered as TS.
    std::string substTemplate(const std::string& tmpl, const ir::Bound& b) {
        std::string out;
        for (std::size_t i = 0; i < tmpl.size();) {
            if (tmpl[i] == '$' && tmpl.compare(i, 5, "$this") == 0) { if (b.receiver) out += emitExpr(*b.receiver); i += 5; }
            else if (tmpl[i] == '$' && tmpl.compare(i, 2, "$T") == 0) { out += tsType(b.type); i += 2; } // ctor: the mapped type
            else if (tmpl[i] == '$' && i + 1 < tmpl.size() && std::isdigit(static_cast<unsigned char>(tmpl[i + 1]))) {
                std::size_t idx = static_cast<std::size_t>(tmpl[i + 1] - '0');
                if (idx < b.args.size()) out += emitExpr(*b.args[idx]);
                i += 2;
            } else out += tmpl[i++];
        }
        return out;
    }

    // An extension lowers to a plain free function whose first param is the receiver `self`; call sites
    // emit `name(obj, …)` (TS has no extension-method call syntax — the `x.m()` reading cannot survive).
    void emitExtension(const ir::Function& f) {
        std::string sig = "function " + f.name + tsGenerics(f.generics) + "(";
        for (std::size_t i = 0; i < f.params.size(); ++i) { if (i) sig += ", "; sig += tsParam(f.params[i]); }
        sig += "): " + tsType(f.returnType) + " {";
        line(sig);
        ++indent_;
        if (f.exprBodied) line("return " + emitExpr(*f.exprBody) + ";");
        else for (const auto& s : f.body) emitStmt(*s);
        --indent_;
        line("}");
    }

    void emitFunction(const ir::Function& fn) {
        std::string sig = std::string(fn.isIterator ? "function* " : "function ") + fn.mangledName + tsGenerics(fn.generics) + "(";
        for (std::size_t i = 0; i < fn.params.size(); ++i) { if (i) sig += ", "; sig += tsParam(fn.params[i]); }
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
                } else if (!f.tupleBindings.empty()) { // `for (const [a, b] of seq)`
                    std::string names;
                    for (std::size_t i = 0; i < f.tupleBindings.size(); ++i) { if (i) names += ", "; names += f.tupleBindings[i]; }
                    line("for (const [" + names + "] of " + emitExpr(*f.iterable) + ") {");
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
            int cp = operatorPrecedence(static_cast<const ir::Binary&>(c).op);
            if (isRight ? (cp <= parentPrec) : (cp < parentPrec)) return "(" + inner + ")";
        }
        return inner;
    }

    std::string emitExpr(const ir::Expr& e) {
        switch (e.kind) {
            case ir::ExprKind::Int: {
                const std::string& text = static_cast<const ir::IntLit&>(e).text;
                auto it = typescriptSpec().intSuffix.find(e.type.name);
                return it == typescriptSpec().intSuffix.end() ? text : text + it->second;
            }
            case ir::ExprKind::Float: return static_cast<const ir::FloatLit&>(e).text;
            case ir::ExprKind::Bool:  return static_cast<const ir::BoolLit&>(e).value ? "true" : "false";
            case ir::ExprKind::Null:  return "null";
            case ir::ExprKind::Interp: { // TS template literal `` `…${expr}…` ``
                const auto& in = static_cast<const ir::Interp&>(e);
                std::string s = "`";
                for (std::size_t i = 0; i < in.chunks.size(); ++i) {
                    for (std::size_t j = 0; j < in.chunks[i].size(); ++j) {
                        char c = in.chunks[i][j];
                        if (c == '`' || c == '\\') { s += '\\'; s += c; }
                        else if (c == '$' && j + 1 < in.chunks[i].size() && in.chunks[i][j + 1] == '{') s += "\\$";
                        else s += c;
                    }
                    if (i < in.holes.size()) s += "${" + emitExpr(*in.holes[i]) + "}";
                }
                return s + "`";
            }
            case ir::ExprKind::Str:   return escape(static_cast<const ir::StrLit&>(e).value);
            case ir::ExprKind::Char:  return escape(static_cast<const ir::CharLit&>(e).value); // TS has no char -> string
            case ir::ExprKind::Var:   return static_cast<const ir::Var&>(e).name;
            case ir::ExprKind::This:  return "this";
            case ir::ExprKind::Unary: {
                const auto& u = static_cast<const ir::Unary&>(e);
                std::string operand = u.operand->kind == ir::ExprKind::Binary ? "(" + emitExpr(*u.operand) + ")"
                                                                              : emitExpr(*u.operand);
                if (isSmallInt(e.type) && u.op == "-") return narrowTs(e.type.name, "-" + operand); // wrap negate
                return u.op + operand;
            }
            case ir::ExprKind::Cast: {
                const auto& c = static_cast<const ir::Cast&>(e);
                return tsConvert(c.operand->type, e.type, emitExpr(*c.operand));
            }
            case ir::ExprKind::Extern: return static_cast<const ir::Extern&>(e).code; // raw TS verbatim
            case ir::ExprKind::Binary: {
                const auto& b = static_cast<const ir::Binary&>(e);
                if (b.op == "==" || b.op == "!=") {
                    // §3.C: record equality is structural (.equals); otherwise strict ===/!== (never JS loose ==).
                    if (isRecordType(b.lhs->type)) {
                        std::string call = atom(*b.lhs) + ".equals(" + emitExpr(*b.rhs) + ")";
                        return b.op == "==" ? call : "!" + call;
                    }
                    int pe = operatorPrecedence(b.op);
                    return child(*b.lhs, pe, false) + " " + typescriptSpec().binOp(b.op) + " " + child(*b.rhs, pe, true);
                }
                if (isUserType(b.lhs->type)) { // operator overload -> method call (TS has no operators)
                    std::string method = opMethod(b.op);
                    if (!method.empty()) return atom(*b.lhs) + "." + method + "(" + emitExpr(*b.rhs) + ")";
                }
                int p = operatorPrecedence(b.op);
                std::string lhs = child(*b.lhs, p, false), rhs = child(*b.rhs, p, true);
                // §3.C: 64-bit ints are BigInt; wrap to 64 bits at each boundary so they overflow like .NET
                // `long`/`ulong` (BigInt is otherwise arbitrary-precision). BigInt `/` already truncates.
                if (isI64(e.type)) {
                    const char* w = e.type.name == "u64" ? "BigInt.asUintN(64, " : "BigInt.asIntN(64, ";
                    return std::string(w) + lhs + " " + b.op + " " + rhs + ")";
                }
                // §3.C: 32-bit-or-narrower int arithmetic must wrap on overflow and truncate on division
                // like .NET — JS numbers are f64. Each operation boundary is re-narrowed to the type's
                // range; `*` needs Math.imul (a plain product can exceed 2^53 and lose the low bits first).
                if (isSmallInt(e.type)) {
                    const std::string& n = e.type.name;
                    if (b.op == "*") {
                        std::string prod = "Math.imul(" + emitExpr(*b.lhs) + ", " + emitExpr(*b.rhs) + ")";
                        return n == "i32" ? prod : narrowTs(n, prod); // Math.imul already yields i32
                    }
                    if (b.op == "+" || b.op == "-" || b.op == "/" || b.op == "%")
                        return narrowTs(n, lhs + " " + b.op + " " + rhs);
                }
                return lhs + " " + typescriptSpec().binOp(b.op) + " " + rhs;
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
                return base + (m.nullSafe ? "?." : ".") + m.field;
            }
            case ir::ExprKind::MethodCall: {
                const auto& mc = static_cast<const ir::MethodCall&>(e);
                if (isPrimNumeric(mc.staticType) && mc.method == "parse") { // i32.parse(s) per-target idiom
                    std::string arg = emitExpr(*mc.args[0]);
                    if (mc.staticType == "i64" || mc.staticType == "u64") return "BigInt(" + arg + ")";
                    if (mc.staticType == "f32" || mc.staticType == "f64") return "Number(" + arg + ")";
                    return narrowTs(mc.staticType, "Number.parseInt(" + arg + ", 10)"); // i8..u32
                }
                if (mc.isExtension) { // free-function form: name(obj, args)
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
            case ir::ExprKind::Cond: {
                const auto& c = static_cast<const ir::Cond&>(e);
                std::string cc = emitExpr(*c.cond), ct = emitExpr(*c.then), ce = emitExpr(*c.els);
                return renderCond(cc, ct, ce);
            }
            case ir::ExprKind::Index: {
                const auto& ix = static_cast<const ir::Index&>(e);
                // A user type's `operator get` is a `get(i)` method in TS (no `[]` overload); arrays index directly.
                if (ix.receiver->type.kind == TypeRef::Kind::Named && indexerTypes_.count(ix.receiver->type.name))
                    return atom(*ix.receiver) + ".get(" + emitExpr(*ix.index) + ")";
                return atom(*ix.receiver) + "[" + emitExpr(*ix.index) + "]";
            }
            case ir::ExprKind::ListLit: {
                const auto& l = static_cast<const ir::ListLit&>(e);
                std::string s = "[";
                for (std::size_t i = 0; i < l.elements.size(); ++i) { if (i) s += ", "; s += emitExpr(*l.elements[i]); }
                return s + "]";
            }
            case ir::ExprKind::Tuple: {
                const auto& t = static_cast<const ir::Tuple&>(e);
                std::vector<std::string> parts;
                for (const auto& el : t.elements) parts.push_back(emitExpr(*el));
                return renderDelimited(typescriptSpec().delimited.at("tuple"), parts);
            }
            case ir::ExprKind::With: {
                // A TS record is a class, so rebuild via its ctor (preserving the prototype/methods): each
                // field is the override or copied from the base. Bind the base to a temp (IIFE) unless it's a
                // simple var, so it's evaluated once — matching C#'s `expr with { … }`.
                const auto& w = static_cast<const ir::With&>(e);
                const auto fit = recordFields_.find(e.type.name);
                std::unordered_map<std::string, std::string> overrides;
                for (const auto& f : w.fields) overrides[f.name] = emitExpr(*f.value);
                bool simple = w.base->kind == ir::ExprKind::Var;
                std::string baseRef = simple ? emitExpr(*w.base) : "__w" + std::to_string(tmp_++);
                std::string ctor = "new " + e.type.name + "(";
                if (fit != recordFields_.end())
                    for (std::size_t i = 0; i < fit->second.size(); ++i) {
                        if (i) ctor += ", ";
                        auto o = overrides.find(fit->second[i]);
                        ctor += o != overrides.end() ? o->second : baseRef + "." + fit->second[i];
                    }
                ctor += ")";
                if (simple) return ctor;
                return "(() => { const " + baseRef + " = " + emitExpr(*w.base) + "; return " + ctor + "; })()";
            }
            case ir::ExprKind::Bound:
                return substTemplate(static_cast<const ir::Bound&>(e).tsTemplate, static_cast<const ir::Bound&>(e));
            case ir::ExprKind::New: {
                const auto& n = static_cast<const ir::New&>(e);
                // List (and any extern class with a bound ctor) routes through ir::Bound in lower; a plain
                // ir::New here is a user record/class (or Error, the last hardcoded core type).
                std::string ctor = n.typeName;
                if (!n.typeArgs.empty()) {
                    ctor += "<";
                    for (std::size_t i = 0; i < n.typeArgs.size(); ++i) { if (i) ctor += ", "; ctor += tsType(n.typeArgs[i]); }
                    ctor += ">";
                }
                std::vector<std::string> args;
                for (const auto& a : n.args) args.push_back(emitExpr(*a));
                return "new " + ctor + renderArgs(args);
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
