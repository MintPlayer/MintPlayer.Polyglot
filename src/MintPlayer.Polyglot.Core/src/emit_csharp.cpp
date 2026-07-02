#include "mintplayer/polyglot/emit.hpp"

#include <cctype>
#include <string>
#include <unordered_set>

#include "mintplayer/polyglot/backend_engine.hpp"
#include "mintplayer/polyglot/backend_spec.hpp"
#include "mintplayer/polyglot/backend_spec_json.hpp"
#include "mintplayer/polyglot/emitter_base.hpp"

#include <cassert>
#include <functional>
#include <unordered_map>

// Hand-written IR -> C# pretty-printer. Walks the typed IR; wraps the program's free functions in a
// `static class Program`, maps the `print` intrinsic -> global::System.Console.WriteLine and the entry
// function -> Main. Every BCL reference is `global::`-qualified (no `using`) so generated code can't
// collide with a user-defined type/namespace named System/Console/Math/etc.

namespace mintplayer::polyglot {

namespace {

// The C# backend's declarative data, now a JSON spec (P18 slice 1: the tabular ~70% loads from data instead
// of a compiled-in struct — PRD §4.10). `char` is absent on purpose — C# lets it fall through to the named-type
// path (-> `char`); the structural cases stay in csType. The imperative Hooks below are unchanged; only the
// Spec's source moved to JSON. Output is byte-identical (the differential/golden gates enforce it).
const char* CSHARP_SPEC_JSON = R"JSON({
  "name": "csharp",
  "scalarType": { "unit": "void", "i8": "sbyte", "i16": "short", "i32": "int", "i64": "long",
                  "u8": "byte", "u16": "ushort", "u32": "uint", "u64": "ulong",
                  "f32": "float", "f64": "double", "bool": "bool", "string": "string" },
  "intSuffix": { "i64": "L", "u64": "UL", "u32": "U" },
  "binaryOp": {},
  "delimited": { "tuple": { "open": "(", "sep": ", ", "close": ")" } },
  "blockStyle": "bracesAllman",
  "stmtEnd": ";",
  "throwKeyword": "throw",
  "trueLit": "true", "falseLit": "false", "nullLit": "null"
})JSON";

const BackendSpec& csharpSpec() {
    static const BackendSpec spec = [] {
        SpecLoadResult r = loadBackendSpec(CSHARP_SPEC_JSON);
        assert(r.ok && "embedded C# backend spec must parse"); // our own spec: a parse failure is a build bug
        return r.spec;
    }();
    return spec;
}

// P18 slices 4-9: emitter HOOKS migrated from imperative C++ to declarative JSON Rules. emitExpr looks up the
// rule for the node kind and interprets it; kinds without a rule still run the C++ switch. Byte-identical (the
// differential + golden gates are the oracle). Covered so far: the leaf literals (Int/Float/Bool/Null/Str),
// Binary (child recursion + precedence via `emitChild`), Call (arg lists via the `map` primitive), the
// scalar-child family Member/Index/Cond, and the delimited-list family Tuple/ListLit/New/MakeCase (`map` +
// affixes). A free function lives in `static class Program`, so a free call is qualified `Program.f(...)`; a
// function-valued local (closure param) is called bare — the `case` on `node.isFree` picks between them.
// Builtins: `ident` (escape a C#-keyword name -> `@base`), `elemType` (a ListLit's element type),
// `typeArgsSuffix` ("" or `<T, U>` — New's own type args, or a MakeCase's result-type args, since a generic
// union's case record is itself generic: `Option<i32>` -> `new Some<int>(…)`). Tuple's brackets read from the
// spec's `delimited` table; ListLit's container is the inherent BCL `List<T>` (list literals are built-in
// syntax like TS `[…]`, container fixed regardless of imports). NOTE: an extern class with a bound ctor (List,
// Error, …) routes through `ir::Bound` in lower, so a plain `ir::New` is always a user record/class here.
const char* CSHARP_EXPR_RULES_JSON = R"JSON({
  "Int":   { "tmpl": [ {"get":"node.text"}, {"fn":"intSuffix","args":[{"get":"node.type"}]} ] },
  "Float": { "get": "node.text" },
  "Bool":  { "case": { "when": [ [ {"eq":["node.value","true"]}, {"get":"spec.trueLit"} ] ],
                       "else": {"get":"spec.falseLit"} } },
  "Null":  { "get": "spec.nullLit" },
  "Str":   { "fn": "escapeString", "args": [ {"get":"node.value"} ] },
  "Binary": { "fn": "subWordWrap", "args": [ {"get":"node.type"},
                { "tmpl": [ {"emitChild":"node.lhs","side":"l"}, " ",
                            {"fn":"opSpelling","args":[{"get":"node.op"}]}, " ",
                            {"emitChild":"node.rhs","side":"r"} ] } ] },
  "Call": { "tmpl": [
              { "case": { "when": [ [ {"eq":["node.isFree","true"]},
                                      {"tmpl":["Program.", {"get":"node.callee"}]} ] ],
                          "else": {"get":"node.callee"} } },
              "(", { "map": "node.args", "sep": ", " }, ")" ] },
  "Member": { "tmpl": [
                { "case": { "when": [ [ {"has":"node.staticType"}, {"get":"node.staticType"} ] ],
                            "else": {"emitChild":"node.object","side":"recv"} } },
                { "case": { "when": [ [ {"eq":["node.nullSafe","true"]}, "?." ] ], "else": "." } },
                { "fn": "ident", "args": [ {"get":"node.field"} ] } ] },
  "Index": { "tmpl": [ {"emitChild":"node.receiver","side":"recv"}, "[", {"emit":"node.index"}, "]" ] },
  "Cond":  { "tmpl": [ "(", {"emit":"node.cond"}, " ? ", {"emit":"node.then"},
                       " : ", {"emit":"node.els"}, ")" ] },
  "Tuple": { "tmpl": [ {"get":"spec.delimited.tuple.open"}, {"map":"node.elements","sep":", "},
                       {"get":"spec.delimited.tuple.close"} ] },
  "ListLit": { "tmpl": [ "new global::System.Collections.Generic.List<", {"fn":"elemType"}, "> { ",
                         {"map":"node.elements","sep":", "}, " }" ] },
  "New": { "tmpl": [ "new ", {"get":"node.typeName"}, {"fn":"typeArgsSuffix"},
                     "(", {"map":"node.args","sep":", "}, ")" ] },
  "MakeCase": { "tmpl": [ "new ", {"get":"node.caseName"}, {"fn":"typeArgsSuffix"},
                          "(", {"map":"node.fields","sep":", "}, ")" ] }
})JSON";

const std::unordered_map<std::string, engine::Rule>& csharpExprRules() {
    static const std::unordered_map<std::string, engine::Rule> rules = [] {
        std::unordered_map<std::string, engine::Rule> m;
        json::Value doc = json::parse(CSHARP_EXPR_RULES_JSON);
        for (const auto& kv : doc.members) {
            bool ok = true;
            std::string err;
            engine::Rule r = engine::parseRule(kv.second, ok, err);
            assert(ok && "embedded C# expr rule must parse");
            m.emplace(kv.first, std::move(r));
        }
        return m;
    }();
    return rules;
}

// The ExprKind name used to key the rule table (only the migrated leaf kinds; "" routes to the C++ switch).
const char* csExprRuleKey(ir::ExprKind k) {
    switch (k) {
        case ir::ExprKind::Int:   return "Int";
        case ir::ExprKind::Float: return "Float";
        case ir::ExprKind::Bool:  return "Bool";
        case ir::ExprKind::Null:   return "Null";
        case ir::ExprKind::Str:    return "Str";
        case ir::ExprKind::Binary: return "Binary";
        case ir::ExprKind::Call:   return "Call";
        case ir::ExprKind::Member:  return "Member";
        case ir::ExprKind::Index:   return "Index";
        case ir::ExprKind::Cond:    return "Cond";
        case ir::ExprKind::Tuple:    return "Tuple";
        case ir::ExprKind::ListLit:  return "ListLit";
        case ir::ExprKind::New:      return "New";
        case ir::ExprKind::MakeCase: return "MakeCase";
        default:                     return "";
    }
}

std::string csIdent(const std::string& n); // defined below; the `ident` builtin escapes C# keyword fields
std::string csType(const TypeRef& t);      // defined below; the `elemType` builtin renders a list element type

// EvalContext over an ir::Expr + the backend spec — the seam that plugs the interpreter into the real IR.
// Exposes leaf fields (`node.text`/`node.value`/`node.type`), spec literals (`spec.*`), and the fixed builtins
// the rules invoke (intSuffix lookup, string escaping). Recursive child access is added when the recursive
// families migrate.
class CsExprCtx : public engine::EvalContext {
public:
    using EmitFn = std::function<std::string(const ir::Expr&)>;
    CsExprCtx(const ir::Expr& e, const BackendSpec& spec, EmitFn emit)
        : e_(e), spec_(spec), emit_(std::move(emit)) {}

    std::string get(const std::string& path) const override {
        if (path == "node.type")     return e_.type.name;
        if (path == "node.op")       return e_.kind == ir::ExprKind::Binary ? static_cast<const ir::Binary&>(e_).op : "";
        if (e_.kind == ir::ExprKind::Call) {
            const auto& c = static_cast<const ir::Call&>(e_);
            if (path == "node.callee")     return c.callee;
            if (path == "node.isFree")     return c.isFree ? "true" : "false";
            if (path == "node.args.count") return std::to_string(c.args.size());
        }
        if (e_.kind == ir::ExprKind::Member) {
            const auto& m = static_cast<const ir::Member&>(e_);
            if (path == "node.staticType") return m.staticType;
            if (path == "node.field")      return m.field;
            if (path == "node.nullSafe")   return m.nullSafe ? "true" : "false";
        }
        if (e_.kind == ir::ExprKind::New) {
            const auto& n = static_cast<const ir::New&>(e_);
            if (path == "node.typeName")   return n.typeName;
            if (path == "node.args.count") return std::to_string(n.args.size());
        }
        if (e_.kind == ir::ExprKind::MakeCase) {
            const auto& mc = static_cast<const ir::MakeCase&>(e_);
            if (path == "node.caseName")     return mc.caseName;
            if (path == "node.fields.count") return std::to_string(mc.fields.size());
        }
        if (path == "node.elements.count") {
            if (e_.kind == ir::ExprKind::ListLit) return std::to_string(static_cast<const ir::ListLit&>(e_).elements.size());
            if (e_.kind == ir::ExprKind::Tuple)   return std::to_string(static_cast<const ir::Tuple&>(e_).elements.size());
        }
        if (path.rfind("spec.delimited.", 0) == 0) { // spec.delimited.<key>.<open|sep|close>
            const std::string rest = path.substr(15);
            const std::size_t dot = rest.rfind('.');
            if (dot != std::string::npos) {
                auto it = spec_.delimited.find(rest.substr(0, dot));
                if (it != spec_.delimited.end()) {
                    const std::string field = rest.substr(dot + 1);
                    if (field == "open")  return it->second.open;
                    if (field == "sep")   return it->second.sep;
                    if (field == "close") return it->second.close;
                }
            }
            return "";
        }
        if (path == "spec.nullLit")  return spec_.nullLit;
        if (path == "spec.trueLit")  return spec_.trueLit;
        if (path == "spec.falseLit") return spec_.falseLit;
        if (path == "node.text") {
            if (e_.kind == ir::ExprKind::Int)   return static_cast<const ir::IntLit&>(e_).text;
            if (e_.kind == ir::ExprKind::Float) return static_cast<const ir::FloatLit&>(e_).text;
        }
        if (path == "node.value") {
            if (e_.kind == ir::ExprKind::Bool) return static_cast<const ir::BoolLit&>(e_).value ? "true" : "false";
            if (e_.kind == ir::ExprKind::Str)  return static_cast<const ir::StrLit&>(e_).value;
        }
        return "";
    }
    bool has(const std::string& path) const override { return !get(path).empty(); }

    // Recurse into a child expr, applying the fixed C# parenthesization rules the emitter's child()/atom() used.
    std::string emitChild(const std::string& path, const std::string& side) const override {
        const ir::Expr* c = childExpr(path);
        if (!c) return "";
        std::string inner = emit_(*c);
        if (side == "l" || side == "r") { // binary operand: wrap by precedence + associativity
            if (e_.kind == ir::ExprKind::Binary && c->kind == ir::ExprKind::Binary) {
                int pp = operatorPrecedence(static_cast<const ir::Binary&>(e_).op);
                int cp = operatorPrecedence(static_cast<const ir::Binary&>(*c).op);
                if (side == "r" ? cp <= pp : cp < pp) return "(" + inner + ")";
            }
            return inner;
        }
        if (side == "recv") { // atom(): wrap a binary/unary/cast receiver
            if (c->kind == ir::ExprKind::Binary || c->kind == ir::ExprKind::Unary || c->kind == ir::ExprKind::Cast)
                return "(" + inner + ")";
        }
        return inner;
    }

    std::string builtin(const std::string& name, const std::vector<std::string>& args) const override {
        if (name == "intSuffix") {
            auto it = spec_.intSuffix.find(args.empty() ? std::string() : args[0]);
            return it == spec_.intSuffix.end() ? std::string() : it->second;
        }
        if (name == "escapeString") return renderString(args.empty() ? std::string() : args[0]);
        if (name == "ident")        return csIdent(args.empty() ? std::string() : args[0]);
        if (name == "elemType")     return e_.kind == ir::ExprKind::ListLit ? csType(static_cast<const ir::ListLit&>(e_).elem) : "";
        if (name == "typeArgsSuffix") { // "" or "<T, U>" — New's own type args / a MakeCase's result-type args
            const std::vector<TypeRef>* ta = e_.kind == ir::ExprKind::New ? &static_cast<const ir::New&>(e_).typeArgs
                                           : e_.kind == ir::ExprKind::MakeCase ? &e_.type.args : nullptr;
            if (!ta || ta->empty()) return "";
            std::string s = "<";
            for (std::size_t i = 0; i < ta->size(); ++i) { if (i) s += ", "; s += csType((*ta)[i]); }
            return s + ">";
        }
        if (name == "opSpelling")   return spec_.binOp(args.empty() ? std::string() : args[0]);
        if (name == "subWordWrap") { // C# sub-32 arithmetic promotes to int; cast back to wrap at 8/16 bits
            const std::string& tn = args.size() > 0 ? args[0] : std::string();
            const std::string& inner = args.size() > 1 ? args[1] : std::string();
            const char* cast = tn == "i8" ? "sbyte" : tn == "i16" ? "short"
                             : tn == "u8" ? "byte"  : tn == "u16" ? "ushort" : nullptr;
            return cast ? "(" + std::string(cast) + ")(" + inner + ")" : inner;
        }
        return "";
    }

private:
    const ir::Expr* childExpr(const std::string& path) const {
        if (e_.kind == ir::ExprKind::Binary) {
            const auto& b = static_cast<const ir::Binary&>(e_);
            if (path == "node.lhs") return b.lhs.get();
            if (path == "node.rhs") return b.rhs.get();
        }
        if (path.rfind("node.args.", 0) == 0) { // indexed arg path `node.args.<i>` (from a `map` rule)
            const std::size_t i = static_cast<std::size_t>(std::stoul(path.substr(10)));
            if (e_.kind == ir::ExprKind::Call) {
                const auto& c = static_cast<const ir::Call&>(e_);
                if (i < c.args.size()) return c.args[i].get();
            }
            if (e_.kind == ir::ExprKind::New) {
                const auto& n = static_cast<const ir::New&>(e_);
                if (i < n.args.size()) return n.args[i].get();
            }
        }
        if (e_.kind == ir::ExprKind::MakeCase && path.rfind("node.fields.", 0) == 0) {
            const auto& mc = static_cast<const ir::MakeCase&>(e_);
            const std::size_t i = static_cast<std::size_t>(std::stoul(path.substr(12)));
            if (i < mc.fields.size()) return mc.fields[i].value.get();
        }
        if (e_.kind == ir::ExprKind::Member && path == "node.object")
            return static_cast<const ir::Member&>(e_).object.get();
        if (e_.kind == ir::ExprKind::Index) {
            const auto& ix = static_cast<const ir::Index&>(e_);
            if (path == "node.receiver") return ix.receiver.get();
            if (path == "node.index")    return ix.index.get();
        }
        if (e_.kind == ir::ExprKind::Cond) {
            const auto& c = static_cast<const ir::Cond&>(e_);
            if (path == "node.cond") return c.cond.get();
            if (path == "node.then") return c.then.get();
            if (path == "node.els")  return c.els.get();
        }
        if (path.rfind("node.elements.", 0) == 0) { // indexed element path `node.elements.<i>` (from a `map` rule)
            const std::size_t i = static_cast<std::size_t>(std::stoul(path.substr(14)));
            if (e_.kind == ir::ExprKind::ListLit) {
                const auto& l = static_cast<const ir::ListLit&>(e_);
                if (i < l.elements.size()) return l.elements[i].get();
            }
            if (e_.kind == ir::ExprKind::Tuple) {
                const auto& t = static_cast<const ir::Tuple&>(e_);
                if (i < t.elements.size()) return t.elements[i].get();
            }
        }
        return nullptr;
    }

    const ir::Expr& e_;
    const BackendSpec& spec_;
    EmitFn emit_;
};

// The current module's `extern class` type map (name -> spelling/ctor templates), set at the top of each
// emit() so the free `csType` can consult declared spellings without threading the map through every call
// site. Single-threaded compile; this emitter is the only writer; rebuilt and re-pointed each emit().
const std::unordered_map<std::string, const ir::ExternType*>* g_externTypes = nullptr;

// Substitute a type-spelling template's `$0,$1,…` with the rendered type args (e.g. List's
// "…List<$0>" with [i32] -> "…List<int>").
std::string substTypeTmpl(const std::string& tmpl, const std::vector<TypeRef>& args) {
    std::string out;
    for (std::size_t i = 0; i < tmpl.size();) {
        if (tmpl[i] == '$' && i + 1 < tmpl.size() && std::isdigit(static_cast<unsigned char>(tmpl[i + 1]))) {
            std::size_t idx = static_cast<std::size_t>(tmpl[i + 1] - '0');
            if (idx < args.size()) out += csType(args[idx]);
            i += 2;
        } else out += tmpl[i++];
    }
    return out;
}

std::string csType(const TypeRef& t) {
    if (t.kind == TypeRef::Kind::Named) {
        // A nullable value type needs C# `T?` (Nullable<T>); a nullable reference type accepts null as-is
        // (the generated project disables nullable reference annotations), so it renders without the `?`.
        if (t.nullable) {
            TypeRef base = t; base.nullable = false;
            const std::string& n = t.name;
            bool valueType = n == "i8" || n == "i16" || n == "i32" || n == "i64" || n == "u8" || n == "u16" ||
                             n == "u32" || n == "u64" || n == "f32" || n == "f64" || n == "bool" || n == "char";
            return valueType ? csType(base) + "?" : csType(base);
        }
        if (auto it = csharpSpec().scalarType.find(t.name); it != csharpSpec().scalarType.end()) return it->second;
        if (t.name.empty())     return "object";
        // A native-backed `extern class` (e.g. List) declares its spelling via a `type { … }` block.
        if (g_externTypes) {
            if (auto it = g_externTypes->find(t.name); it != g_externTypes->end() && !it->second->csType.empty())
                return substTypeTmpl(it->second->csType, t.args);
        }
        std::string name = t.name; // Error/Iterable now spell via the core-prelude extern-class mapping above
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
            if (t.args.empty()) return "global::System.Action";
            std::string s = "global::System.Action<";
            for (std::size_t i = 0; i < t.args.size(); ++i) { if (i) s += ", "; s += csType(t.args[i]); }
            return s + ">";
        }
        std::string s = "global::System.Func<";
        for (const auto& a : t.args) s += csType(a) + ", ";
        return s + csType(t.ret[0]) + ">";
    }
    if (t.kind == TypeRef::Kind::Tuple) { // C# value tuple `(A, B)`
        std::string s = "(";
        for (std::size_t i = 0; i < t.args.size(); ++i) { if (i) s += ", "; s += csType(t.args[i]); }
        return s + ")";
    }
    return "object";
}

// A `.pg` identifier that collides with a C# reserved keyword (field/param/local/member named `base`,
// `default`, `class`, …) is emitted as a `@`-verbatim identifier so the C# compiles. (TS has no such escape,
// but the colliding set differs and `base` etc. are legal TS identifiers, so TS names emit verbatim.)
std::string csIdent(const std::string& n) {
    static const std::unordered_set<std::string> kw = {
        "abstract","as","base","bool","break","byte","case","catch","char","checked","class","const","continue",
        "decimal","default","delegate","do","double","else","enum","event","explicit","extern","false","finally",
        "fixed","float","for","foreach","goto","if","implicit","in","int","interface","internal","is","lock","long",
        "namespace","new","null","object","operator","out","override","params","private","protected","public",
        "readonly","ref","return","sbyte","sealed","short","sizeof","stackalloc","static","string","struct","switch",
        "this","throw","true","try","typeof","uint","ulong","unchecked","unsafe","ushort","using","virtual","void",
        "volatile","while",
    };
    return kw.count(n) ? "@" + n : n;
}

std::string csGenerics(const std::vector<ir::GenericParam>& gs) {
    if (gs.empty()) return "";
    std::string s = "<";
    for (std::size_t i = 0; i < gs.size(); ++i) { if (i) s += ", "; s += gs[i].name; }
    return s + ">";
}
// C# puts bounds in trailing `where T : A, B` clauses (one per bounded parameter). The `INumber` marker is
// a Polyglot compile-time-only numeric constraint (no C# equivalent), so it is erased here.
std::string csWhere(const std::vector<ir::GenericParam>& gs) {
    std::string s;
    for (const auto& g : gs) {
        std::vector<const TypeRef*> bounds;
        for (const auto& b : g.bounds) if (b.name != "INumber") bounds.push_back(&b);
        if (bounds.empty()) continue;
        s += " where " + g.name + " : ";
        for (std::size_t i = 0; i < bounds.size(); ++i) { if (i) s += ", "; s += csType(*bounds[i]); }
    }
    return s;
}

bool isPrimNumeric(const std::string& n) {
    return n == "i8" || n == "i16" || n == "i32" || n == "i64" || n == "u8" || n == "u16" ||
           n == "u32" || n == "u64" || n == "f32" || n == "f64";
}


class CSharpEmitter : public EmitterBase {
public:
    std::string emit(const ir::Module& m) {
        // No `using` directives: every BCL reference is `global::`-qualified, so emitted code can't collide
        // with a user type/namespace named System/Console/Math/etc.
        out_.clear();
        indent_ = 0;
        externMap_.clear();
        for (const auto& et : m.externTypes) externMap_[et.name] = &et;
        g_externTypes = &externMap_;
        for (const auto& e : m.enums) emitEnum(e);
        for (const auto& i : m.interfaces) emitInterface(i);
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
        for (const auto& g : m.globals) // top-level const/let -> static readonly field
            line("static readonly " + csType(g.type) + " " + csIdent(g.name) + (g.init ? " = " + emitExpr(*g.init) : "") + ";");
        for (const auto& fn : m.functions) {
            if (!fn.actualTarget.empty() && fn.actualTarget != "csharp") continue; // other target's `actual`
            emitFunction(fn);
        }
        for (const auto& fn : m.functions) {
            if (fn.isEntry) {
                line("");
                // Pin the invariant culture so number formatting (WriteLine/interpolation) matches JS — C#
                // is culture-dependent (e.g. "12,5" on a comma-locale machine), JS always uses '.'. §3.
                // An `async fn main` returns a Task; block on it synchronously so `Main` stays `void`. §4.7
                std::string call = fn.isAsync ? "main().GetAwaiter().GetResult();" : "main();";
                line("static void Main() { global::System.Globalization.CultureInfo.CurrentCulture = global::System.Globalization.CultureInfo.InvariantCulture; " + call + " }");
                break;
            }
        }
        out_ += "}\n";
        return out_;
    }

private:
    std::string thisAlias_; // non-empty inside a static operator body: `this` is emitted as this name
    std::unordered_map<std::string, const ir::ExternType*> externMap_; // backs g_externTypes for this emit

    void emitEnum(const ir::Enum& e) {
        std::string s = "enum " + e.name + " { ";
        for (std::size_t i = 0; i < e.cases.size(); ++i) { if (i) s += ", "; s += e.cases[i].name + " = " + std::to_string(e.cases[i].value); }
        line(s + " }");
    }

    void emitInterface(const ir::Interface& it) {
        std::string head = "interface " + it.name + csGenerics(it.generics);
        if (!it.bases.empty()) {
            head += " : ";
            for (std::size_t i = 0; i < it.bases.size(); ++i) { if (i) head += ", "; head += csType(it.bases[i]); }
        }
        line(head + csWhere(it.generics));
        line("{");
        ++indent_;
        for (const auto& m : it.methods) { // implicitly public abstract; signature only
            std::string sig = csType(m.returnType) + " " + csIdent(m.name) + csGenerics(m.generics) + "(";
            for (std::size_t i = 0; i < m.params.size(); ++i) { if (i) sig += ", "; sig += csParam(m.params[i]); }
            line(sig + ");");
        }
        --indent_;
        line("}");
    }

    void emitUnion(const ir::Union& u) {
        std::string g = csGenerics(u.generics); // "" or "<T>"; the base reference reuses the same param names
        line("abstract record " + u.name + g + ";");
        for (const auto& c : u.cases) {
            std::string s = "sealed record " + c.name + g + "(";
            for (std::size_t i = 0; i < c.fields.size(); ++i) { if (i) s += ", "; s += csType(c.fields[i].type) + " " + csIdent(c.fields[i].name); }
            line(s + ") : " + u.name + g + ";");
        }
    }

    // `genArgs` is the scrutinee union's type-arg suffix ("" or "<int>"): a generic union's case records are
    // generic, so the C# pattern must name `Full<int>` / `Empty<int>`, not the open `Full`/`Empty`.
    std::string patternCs(const ir::Pattern& p, const std::string& genArgs = "") {
        switch (p.kind) {
            case ir::PatternKind::Wildcard: return "_";
            case ir::PatternKind::Literal:  return emitExpr(*p.literal);
            case ir::PatternKind::Binding:  return "var " + csIdent(p.binding);
            case ir::PatternKind::EnumCase: return p.enumType + "." + p.enumCase;
            case ir::PatternKind::Ctor: {
                if (p.binders.empty()) return p.ctorCase + genArgs + " _"; // type pattern (payload-free)
                std::string s = p.ctorCase + genArgs + "(";
                for (std::size_t i = 0; i < p.binders.size(); ++i) { if (i) s += ", "; s += "var " + csIdent(p.binders[i].binding); }
                return s + ")";
            }
        }
        return "_";
    }

    void emitRecord(const ir::Record& r) {
        std::string head = "record " + r.name + csGenerics(r.generics) + "(";
        for (std::size_t i = 0; i < r.fields.size(); ++i) { if (i) head += ", "; head += csType(r.fields[i].type) + " " + csIdent(r.fields[i].name); }
        head += ")";
        if (!r.bases.empty()) { // implemented interfaces
            head += " : ";
            for (std::size_t i = 0; i < r.bases.size(); ++i) { if (i) head += ", "; head += csType(r.bases[i]); }
        }
        head += csWhere(r.generics);
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
            // `static readonly` (not `const`) for immutable statics: it accepts any initializer expression,
            // not just compile-time constants, and reads identically as `Owner.Name`.
            std::string mods = f.isStatic ? (f.isMutable ? "static " : "static readonly ") : (f.isMutable ? "" : "readonly ");
            std::string decl = "public " + mods + csType(f.type) + " " + csIdent(f.name);
            if (f.init) decl += " = " + emitExpr(*f.init);
            line(decl + ";");
        }
        if (c.hasInit) {
            std::string sig = "public " + c.name + "(";
            for (std::size_t i = 0; i < c.initParams.size(); ++i) { if (i) sig += ", "; sig += csParam(c.initParams[i]); }
            sig += ")";
            if (c.hasSuper) {
                sig += " : base(";
                for (std::size_t i = 0; i < c.superArgs.size(); ++i) { if (i) sig += ", "; sig += emitExpr(*c.superArgs[i]); }
                sig += ")";
            }
            headBlock(sig, c.initBody);
        }
        for (const auto& m : c.methods) emitMethod(c.name, m);
        --indent_;
        line("}");
    }

    // `T name` or `T name = default` — a parameter declaration with its optional default value.
    std::string csParam(const ir::Param& p) {
        std::string s = csType(p.type) + " " + csIdent(p.name);
        if (p.defaultValue) s += " = " + emitExpr(*p.defaultValue);
        return s;
    }

    void emitMethod(const std::string& recordName, const ir::Method& m) {
        if (m.kind == ir::MethodKind::Property) { // expression-bodied property
            line("public " + csType(m.returnType) + " " + m.name + " => " + emitExpr(*m.exprBody) + ";");
            return;
        }
        if (m.kind == ir::MethodKind::Operator && m.opSymbol == "get") { // `operator fn get` -> a C# indexer
            std::string sig = "public " + csType(m.returnType) + " this[";
            for (std::size_t i = 0; i < m.params.size(); ++i) { if (i) sig += ", "; sig += csParam(m.params[i]); }
            sig += "]";
            if (m.exprBodied) line(sig + " => " + emitExpr(*m.exprBody) + ";");
            else { line(sig + " { get"); line("{"); blockBody(m.body); line("}"); line("}"); }
            return;
        }
        if (m.kind == ir::MethodKind::Operator) { // real C# static operator; `this` -> the first operand
            std::string sig = "public static " + csType(m.returnType) + " operator " + m.opSymbol + "(" + recordName + " lhs";
            for (const auto& p : m.params) sig += ", " + csType(p.type) + " " + p.name;
            sig += ")";
            thisAlias_ = "lhs";
            if (m.exprBodied) line(sig + " => " + emitExpr(*m.exprBody) + ";");
            else headBlock(sig, m.body);
            thisAlias_.clear();
            return;
        }
        std::string ret = m.isAsync ? csAsyncReturn(m.returnType) : csType(m.returnType);
        std::string sig = std::string("public ") + (m.isStatic ? "static " : "") + (m.isAsync ? "async " : "") +
                          ret + " " + m.name + csGenerics(m.generics) + "(";
        for (std::size_t i = 0; i < m.params.size(); ++i) { if (i) sig += ", "; sig += csParam(m.params[i]); }
        sig += ")" + csWhere(m.generics);
        if (m.exprBodied) line(sig + " => " + emitExpr(*m.exprBody) + ";");
        else headBlock(sig, m.body);
    }

    // Parenthesize a receiver that would otherwise mis-bind against `.`/call.
    std::string atom(const ir::Expr& e) {
        std::string s = emitExpr(e);
        return (e.kind == ir::ExprKind::Binary || e.kind == ir::ExprKind::Unary ||
                e.kind == ir::ExprKind::Cast) ? "(" + s + ")" : s;
    }

    // Substitute a binding template: `$this` -> receiver, `$0`,`$1`,… -> args, each rendered as C#.
    std::string substTemplate(const std::string& tmpl, const ir::Bound& b) {
        std::string out;
        for (std::size_t i = 0; i < tmpl.size();) {
            if (tmpl[i] == '$' && tmpl.compare(i, 5, "$this") == 0) { if (b.receiver) out += emitExpr(*b.receiver); i += 5; }
            else if (tmpl[i] == '$' && tmpl.compare(i, 2, "$T") == 0) { out += csType(b.type); i += 2; } // ctor: the mapped type
            else if (tmpl[i] == '$' && i + 1 < tmpl.size() && std::isdigit(static_cast<unsigned char>(tmpl[i + 1]))) {
                std::size_t idx = static_cast<std::size_t>(tmpl[i + 1] - '0');
                if (idx < b.args.size()) out += emitExpr(*b.args[idx]);
                i += 2;
            } else out += tmpl[i++];
        }
        return out;
    }

    // `public static R name(this T self, …)` — the leading `self` param carries the C# `this` modifier.
    void emitExtension(const ir::Function& f) {
        std::string sig = "public static " + csType(f.returnType) + " " + f.name + csGenerics(f.generics) + "(this " +
                          csType(f.params[0].type) + " " + f.params[0].name;
        for (std::size_t i = 1; i < f.params.size(); ++i) sig += ", " + csType(f.params[i].type) + " " + f.params[i].name;
        sig += ")" + csWhere(f.generics);
        if (f.exprBodied) line(sig + " => " + emitExpr(*f.exprBody) + ";");
        else headBlock(sig, f.body);
    }

    // `async fn`: the author writes the unwrapped `T`; C# needs `Task<T>` (or a bare `Task` for unit). §4.7
    static bool isUnitType(const TypeRef& t) { return t.kind == TypeRef::Kind::Named && t.name == "unit"; }
    std::string csAsyncReturn(const TypeRef& ret) {
        return isUnitType(ret) ? "global::System.Threading.Tasks.Task"
                               : "global::System.Threading.Tasks.Task<" + csType(ret) + ">";
    }

    void emitFunction(const ir::Function& fn) {
        // `public` so calls qualified as `Program.fn(...)` from emitted classes resolve (free fns live here).
        std::string ret = fn.isAsync ? csAsyncReturn(fn.returnType) : csType(fn.returnType);
        std::string sig = std::string("public static ") + (fn.isAsync ? "async " : "") + ret + " " +
                          fn.name + csGenerics(fn.generics) + "(";
        for (std::size_t i = 0; i < fn.params.size(); ++i) { if (i) sig += ", "; sig += csParam(fn.params[i]); }
        sig += ")" + csWhere(fn.generics);
        headBlock(sig, fn.body);
    }

    const BackendSpec& spec() const override { return csharpSpec(); }

    std::string localDecl(const std::string& name, bool /*isMutable*/) override { return "var " + csIdent(name); }
    std::string yieldStmt(const std::string& v, bool hasValue) override { return hasValue ? "yield return " + v + ";" : "yield break;"; }
    std::string rethrowStmt() override { return "throw;"; }

    void emitStmtTarget(const ir::Stmt& s) override {
        switch (s.kind) {
            case ir::StmtKind::Try: {
                const auto& t = static_cast<const ir::Try&>(s);
                headBlock("try", t.body);
                for (const auto& c : t.catches) {
                    std::string head = "catch";
                    if (!c.type.name.empty()) {
                        head += " (" + csType(c.type);
                        if (!c.binding.empty()) head += " " + c.binding;
                        head += ")";
                    }
                    if (c.guard) head += " when (" + emitExpr(*c.guard) + ")";
                    headBlock(head, c.body);
                }
                if (t.hasFinally) headBlock("finally", t.finallyBody);
                break;
            }
            case ir::StmtKind::For: {
                const auto& f = static_cast<const ir::For&>(s);
                std::string head;
                if (f.isRange) {
                    std::string cmp = f.inclusive ? " <= " : " < ";
                    head = "for (var " + f.binding + " = " + emitExpr(*f.rangeStart) + "; " +
                           f.binding + cmp + emitExpr(*f.rangeEnd) + "; " + f.binding + "++)";
                } else if (!f.tupleBindings.empty()) { // `foreach (var (a, b) in seq)`
                    std::string names;
                    for (std::size_t i = 0; i < f.tupleBindings.size(); ++i) { if (i) names += ", "; names += f.tupleBindings[i]; }
                    head = "foreach (var (" + names + ") in " + emitExpr(*f.iterable) + ")";
                } else {
                    head = "foreach (var " + f.binding + " in " + emitExpr(*f.iterable) + ")";
                }
                headBlock(head, f.body);
                break;
            }
        }
    }

    std::string emitExpr(const ir::Expr& e) override {
        // Migrated leaf literals (Int/Float/Bool/Null/Str) are now JSON Rules interpreted here; every other
        // kind still runs the C++ switch below. (P18 slice 4.)
        if (const char* key = csExprRuleKey(e.kind); key[0] != '\0') {
            const auto& rules = csharpExprRules();
            auto it = rules.find(key);
            if (it != rules.end()) {
                CsExprCtx ctx(e, spec(), [this](const ir::Expr& c) { return emitExpr(c); });
                return engine::evalRule(it->second, ctx);
            }
        }
        switch (e.kind) {
            case ir::ExprKind::Interp: { // C# interpolated string `$"…{expr}…"`
                const auto& in = static_cast<const ir::Interp&>(e);
                std::string s = "$\"";
                for (std::size_t i = 0; i < in.chunks.size(); ++i) {
                    for (char c : in.chunks[i]) { // escape for an interpolated-string literal
                        if (c == '"' || c == '\\') { s += '\\'; s += c; }
                        else if (c == '\n') s += "\\n"; // a raw newline/tab/CR would break the `$"…"` literal
                        else if (c == '\t') s += "\\t";
                        else if (c == '\r') s += "\\r";
                        else if (c == '{') s += "{{";
                        else if (c == '}') s += "}}";
                        else s += c;
                    }
                    if (i < in.holes.size()) s += "{" + emitExpr(*in.holes[i]) + "}";
                }
                return s + "\"";
            }
            case ir::ExprKind::Char: { // C# char literal `'x'` (escape `'`, `\`, control chars)
                std::string s = "'";
                for (char c : static_cast<const ir::CharLit&>(e).value) {
                    if (c == '\'' || c == '\\') { s += '\\'; s += c; }
                    else if (c == '\n') s += "\\n";
                    else if (c == '\t') s += "\\t";
                    else if (c == '\r') s += "\\r";
                    else s += c;
                }
                return s + "'";
            }
            case ir::ExprKind::Var:   return csIdent(static_cast<const ir::Var&>(e).name);
            case ir::ExprKind::This:  return thisAlias_.empty() ? "this" : thisAlias_;
            case ir::ExprKind::Unary: {
                const auto& u = static_cast<const ir::Unary&>(e);
                std::string operand = u.operand->kind == ir::ExprKind::Binary ? "(" + emitExpr(*u.operand) + ")"
                                                                              : emitExpr(*u.operand);
                return u.op + operand;
            }
            case ir::ExprKind::Await: {
                const auto& a = static_cast<const ir::Await&>(e);
                return "await " + atom(*a.operand);
            }
            case ir::ExprKind::Cast: { // C# handles every numeric conversion natively
                const auto& c = static_cast<const ir::Cast&>(e);
                return "(" + csType(e.type) + ")(" + emitExpr(*c.operand) + ")";
            }
            case ir::ExprKind::Extern: return static_cast<const ir::Extern&>(e).code; // raw C# verbatim
            case ir::ExprKind::MethodCall: {
                const auto& mc = static_cast<const ir::MethodCall&>(e);
                if (isPrimNumeric(mc.staticType) && mc.method == "parse") { // i32.parse(s) -> int.Parse(s)
                    bool flt = mc.staticType == "f32" || mc.staticType == "f64";
                    return csType(namedType(mc.staticType)) + ".Parse(" + emitExpr(*mc.args[0]) +
                           (flt ? ", global::System.Globalization.CultureInfo.InvariantCulture" : "") + ")";
                }
                std::string recv = mc.staticType.empty() ? atom(*mc.object) : mc.staticType;
                std::vector<std::string> args;
                for (const auto& a : mc.args) args.push_back(emitExpr(*a));
                return recv + "." + mc.method + renderArgs(args);
            }
            case ir::ExprKind::With: { // C# records support `with` natively
                const auto& w = static_cast<const ir::With&>(e);
                std::string s = atom(*w.base) + " with { ";
                for (std::size_t i = 0; i < w.fields.size(); ++i) { if (i) s += ", "; s += csIdent(w.fields[i].name) + " = " + emitExpr(*w.fields[i].value); }
                return s + " }";
            }
            case ir::ExprKind::Bound:
                return substTemplate(static_cast<const ir::Bound&>(e).csTemplate, static_cast<const ir::Bound&>(e));
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
                // A generic union scrutinee (`Box<int>`) needs its case patterns spelled `Full<int>` etc.
                std::string genArgs;
                if (!m.scrutinee->type.args.empty()) {
                    genArgs = "<";
                    for (std::size_t i = 0; i < m.scrutinee->type.args.size(); ++i) { if (i) genArgs += ", "; genArgs += csType(m.scrutinee->type.args[i]); }
                    genArgs += ">";
                }
                bool hasCatchAll = false;
                for (std::size_t i = 0; i < m.arms.size(); ++i) {
                    if (i) s += ", ";
                    const ir::Pattern& p = m.arms[i].pattern;
                    if (!m.arms[i].guard && (p.kind == ir::PatternKind::Wildcard || p.kind == ir::PatternKind::Binding)) hasCatchAll = true;
                    s += patternCs(p, genArgs);
                    if (m.arms[i].guard) s += " when " + emitExpr(*m.arms[i].guard);
                    s += " => " + emitExpr(*m.arms[i].body);
                }
                // Sema guarantees exhaustiveness, but C# can't prove it for enums — add an unreachable
                // default so the switch expression compiles without CS8524.
                if (!hasCatchAll) s += ", _ => throw new global::System.InvalidOperationException()";
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
