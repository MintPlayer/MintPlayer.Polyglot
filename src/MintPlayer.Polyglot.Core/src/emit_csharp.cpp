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

// P18 slices 4-10: emitter HOOKS migrated from imperative C++ to declarative JSON Rules. emitExpr looks up the
// rule for the node kind and interprets it; kinds without a rule still run the C++ switch. Byte-identical (the
// differential + golden gates are the oracle). Covered: the leaf literals (Int/Float/Bool/Null/Str/Var/Extern),
// Binary + Unary + Cast (child recursion + precedence via `emitChild`), Call (arg lists via the `map`
// primitive), the scalar-child family Member/Index/Cond, and the delimited-list family Tuple/ListLit/New/
// MakeCase (`map` + affixes). A free function lives in `static class Program`, so a free call is qualified
// `Program.f(...)`; a function-valued local (closure param) is called bare — the `case` on `node.isFree` picks
// between them. Builtins: `ident` (escape a C#-keyword name -> `@base`), `elemType` (a ListLit's element type),
// `castType` (a Cast's target type), `typeArgsSuffix` ("" or `<T, U>` — New's own type args, or a MakeCase's
// result-type args, since a generic union's case record is itself generic: `Option<i32>` -> `new Some<int>(…)`).
// Tuple's brackets read from the spec's `delimited` table; ListLit's container is the inherent BCL `List<T>`
// (list literals are built-in syntax like TS `[…]`, container fixed regardless of imports). NOTE: an extern
// class with a bound ctor (List, Error, …) routes through `ir::Bound` in lower, so a plain `ir::New` is always a
// user record/class here. Still imperative (genuinely per-target shape or stateful): Interp/Char (string
// building), This (operator rebinds `this`), Await, MethodCall, With, Bound, Lambda, Match.
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
                          "(", {"map":"node.fields","sep":", "}, ")" ] },
  "Var":    { "fn": "ident", "args": [ {"get":"node.name"} ] },
  "Extern": { "get": "node.code" },
  "Await":  { "tmpl": [ "await ", {"emitChild":"node.operand","side":"recv"} ] },
  "With": { "tmpl": [ {"emitChild":"node.base","side":"recv"}, " with { ",
              {"map":"node.fields","sep":", ",
               "item":{"tmpl":[{"fn":"ident","args":[{"get":"item.name"}]}," = ",{"emit":"item.value"}]}},
              " }" ] },
  "Cast":   { "tmpl": [ "(", {"fn":"castType"}, ")(", {"emit":"node.operand"}, ")" ] },
  "Unary":  { "tmpl": [ {"get":"node.op"}, {"emitChild":"node.operand","side":"unary"} ] },
  "Interp": { "tmpl": [ "$\"", {"interleave":{"lits":"node.chunks","holes":"node.holes",
                "lit":{"fn":"interpEscape","args":[{"get":"item"}]},
                "hole":{"tmpl":["{",{"emit":"item"},"}"]}}}, "\"" ] },
  "Char": { "fn": "charLit", "args": [ {"get":"node.value"} ] },
  "This": { "case": { "when": [ [ {"has":"ctx.thisAlias"}, {"get":"ctx.thisAlias"} ] ], "else": "this" } },
  "MethodCall": { "tmpl": [
      { "case": { "when": [ [ {"has":"node.staticType"}, {"get":"node.staticType"} ] ],
                  "else": {"emitChild":"node.object","side":"recv"} } },
      ".", {"get":"node.method"}, "(", {"map":"node.args","sep":", "}, ")" ] },
  "Match": { "tmpl": [
      {"emitChild":"node.scrutinee","side":"recv"}, " switch { ",
      {"map":"node.arms","sep":", ","item":{"tmpl":[
        { "case": { "when": [
            [ {"eq":["item.pattern.kind","wildcard"]}, "_" ],
            [ {"eq":["item.pattern.kind","literal"]}, {"emit":"item.pattern.literal"} ],
            [ {"eq":["item.pattern.kind","binding"]},
              {"tmpl":["var ",{"fn":"ident","args":[{"get":"item.pattern.binding"}]}]} ],
            [ {"eq":["item.pattern.kind","enumCase"]},
              {"tmpl":[{"get":"item.pattern.enumType"},".",{"get":"item.pattern.enumCase"}]} ],
            [ {"eq":["item.pattern.binders.count","0"]},
              {"tmpl":[{"get":"item.pattern.ctorCase"},{"fn":"genArgs"}," _"]} ] ],
          "else": {"tmpl":[{"get":"item.pattern.ctorCase"},{"fn":"genArgs"},"(",
            {"map":"item.pattern.binders","sep":", ",
             "item":{"tmpl":["var ",{"fn":"ident","args":[{"get":"item.binding"}]}]}},")"]} } },
        { "case": { "when": [ [ {"eq":["item.hasGuard","true"]},
            {"tmpl":[" when ",{"emit":"item.guard"}]} ] ], "else": "" } },
        " => ", {"emit":"item.body"} ]}},
      { "case": { "when": [ [ {"eq":["node.hasCatchAll","true"]}, "" ] ],
        "else": ", _ => throw new global::System.InvalidOperationException()" } },
      " }" ] },
  "Lambda": { "tmpl": [ "(",
      {"map":"node.params","sep":", ","item":{"case":{"when":[[{"eq":["item.hasType","true"]},
        {"tmpl":[{"type":"item.type"}," ",{"get":"item.name"}]}]],"else":{"get":"item.name"}}}},
      ") => ",
      {"case":{"when":[[{"eq":["node.exprBodied","true"]},{"emit":"node.body"}]],
        "else":{"tmpl":["{ ",{"fn":"inlineBlock"},"}"]}}} ] },
  "EnumDecl": { "line": { "tmpl": [ "enum ", {"get":"decl.name"}, " { ",
      {"map":"decl.cases","sep":", ","item":{"tmpl":[{"get":"item.name"}," = ",{"get":"item.value"}]}},
      " }" ] } },
  "csParam": {"tmpl":[{"type":"item.type"}," ",{"fn":"ident","args":[{"get":"item.name"}]},
      {"case":{"when":[[{"eq":["item.hasDefault","true"]},{"tmpl":[" = ",{"emit":"item.default"}]}]]}}]},
  "csIndexerSig": {"tmpl":["public ",{"type":"decl.returnType"}," this[",
      {"map":"decl.params","sep":", ","item":{"call":"csParam"}},"]"]},
  "csOperatorSig": {"tmpl":["public static ",{"type":"decl.returnType"}," operator ",{"get":"decl.opSymbol"},
      "(",{"get":"decl.owner"}," lhs",
      {"map":"decl.params","sep":"","item":{"tmpl":[", ",{"type":"item.type"}," ",{"get":"item.name"}]}},")"]},
  "csAsyncRet": {"case":{"when":[[{"eq":["decl.retName","unit"]},"global::System.Threading.Tasks.Task"]],
      "else":{"tmpl":["global::System.Threading.Tasks.Task<",{"type":"decl.returnType"},">"]}}},
  "csMethodSig": {"tmpl":["public ",
      {"case":{"when":[[{"eq":["decl.isStatic","true"]},"static "]]}},
      {"case":{"when":[[{"eq":["decl.isAsync","true"]},"async "]]}},
      {"case":{"when":[[{"eq":["decl.isAsync","true"]},{"call":"csAsyncRet"}]],"else":{"type":"decl.returnType"}}},
      " ",{"get":"decl.name"},{"fn":"generics"},"(",
      {"map":"decl.params","sep":", ","item":{"call":"csParam"}},")",{"fn":"where"}]},
  "MethodDecl": {"case":{"when":[
      [ {"eq":["decl.kind","property"]},
        {"line":{"tmpl":["public ",{"type":"decl.returnType"}," ",{"get":"decl.name"}," => ",{"emit":"decl.exprBody"},";"]}} ],
      [ {"and":[{"eq":["decl.kind","operator"]},{"eq":["decl.opSymbol","get"]}]},
        {"case":{"when":[
          [ {"eq":["decl.exprBodied","true"]},
            {"line":{"tmpl":[{"call":"csIndexerSig"}," => ",{"emit":"decl.exprBody"},";"]}} ]],
          "else":{"seq":[
            {"block":{"head":{"tmpl":[{"call":"csIndexerSig"}," { get"]},"body":[{"stmts":"decl.body"}]}},
            {"line":"}"} ]} }} ],
      [ {"eq":["decl.kind","operator"]},
        {"case":{"when":[
          [ {"eq":["decl.exprBodied","true"]},
            {"line":{"tmpl":[{"call":"csOperatorSig"}," => ",{"emit":"decl.exprBody"},";"]}} ]],
          "else":{"block":{"head":{"call":"csOperatorSig"},"body":[{"stmts":"decl.body"}]}} }} ]],
      "else":{"case":{"when":[
          [ {"eq":["decl.exprBodied","true"]},
            {"line":{"tmpl":[{"call":"csMethodSig"}," => ",{"emit":"decl.exprBody"},";"]}} ]],
          "else":{"block":{"head":{"call":"csMethodSig"},"body":[{"stmts":"decl.body"}]}} }} }},
  "InterfaceDecl": { "block": {
      "head": {"tmpl": [ "interface ", {"get":"decl.name"}, {"fn":"generics"},
        {"case":{"when":[[{"eq":["decl.bases.count","0"]},""]],
          "else":{"tmpl":[" : ",{"map":"decl.bases","sep":", ","item":{"type":"item"}}]}}},
        {"fn":"where"} ] },
      "body": [
        { "mapDecl": "decl.methods", "each": { "line": { "tmpl": [
            {"type":"item.returnType"}, " ", {"fn":"ident","args":[{"get":"item.name"}]},
            {"fn":"generics","args":[{"get":"item.#"}]}, "(",
            {"map":"item.params","sep":", ","item":{"tmpl":[
                {"type":"item.type"}," ",{"fn":"ident","args":[{"get":"item.name"}]},
                {"case":{"when":[[{"eq":["item.hasDefault","true"]},
                  {"tmpl":[" = ",{"emit":"item.default"}]}]]}} ]}},
            ");" ] } } } ] } },
  "UnionDecl": { "seq": [
      { "line": { "tmpl": [ "abstract record ", {"get":"decl.name"}, {"fn":"generics"}, ";" ] } },
      { "mapDecl": "decl.cases", "each": { "line": { "tmpl": [
          "sealed record ", {"get":"item.name"}, {"fn":"generics"}, "(",
          {"map":"item.fields","sep":", ",
           "item":{"tmpl":[{"type":"item.type"}," ",{"fn":"ident","args":[{"get":"item.name"}]}]}},
          ") : ", {"get":"decl.name"}, {"fn":"generics"}, ";" ] } } } ] },
  "Type": { "case": { "when": [
      [ {"eq":["type.kind","function"]},
        { "case": { "when": [
            [ {"and":[{"eq":["type.returnsUnit","true"]},{"eq":["type.args.count","0"]}]}, "global::System.Action" ],
            [ {"eq":["type.returnsUnit","true"]},
              {"tmpl":["global::System.Action<",{"map":"type.args","sep":", ","item":{"type":"item"}},">"]} ] ],
          "else": {"tmpl":["global::System.Func<",
            {"map":"type.args","sep":"","item":{"tmpl":[{"type":"item"},", "]}},{"type":"type.ret"},">"]} } } ],
      [ {"eq":["type.kind","tuple"]}, {"tmpl":["(",{"map":"type.args","sep":", ","item":{"type":"item"}},")"]} ],
      [ {"and":[{"eq":["type.nullable","true"]},{"eq":["type.isValueType","true"]}]},
        {"tmpl":[{"type":"type.base"},"?"]} ],
      [ {"eq":["type.nullable","true"]}, {"type":"type.base"} ],
      [ {"has":"type.scalar"}, {"get":"type.scalar"} ],
      [ {"eq":["type.nameEmpty","true"]}, "object" ],
      [ {"has":"type.externTemplate"}, {"fn":"substExtern"} ] ],
    "else": {"tmpl":[{"get":"type.name"},
      {"case":{"when":[[{"eq":["type.args.count","0"]},""]],
        "else":{"tmpl":["<",{"map":"type.args","sep":", ","item":{"type":"item"}},">"]}}}]} } }
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
        case ir::ExprKind::Var:      return "Var";
        case ir::ExprKind::Extern:   return "Extern";
        case ir::ExprKind::Await:    return "Await";
        case ir::ExprKind::With:     return "With";
        case ir::ExprKind::Cast:     return "Cast";
        case ir::ExprKind::Unary:    return "Unary";
        case ir::ExprKind::Interp:     return "Interp";
        case ir::ExprKind::Char:       return "Char";
        case ir::ExprKind::This:       return "This";
        case ir::ExprKind::MethodCall: return "MethodCall";
        case ir::ExprKind::Match:      return "Match";
        case ir::ExprKind::Lambda:     return "Lambda";
        default:                       return "";
    }
}

std::string csIdent(const std::string& n); // defined below; the `ident` builtin escapes C# keyword fields
std::string csType(const TypeRef& t);      // defined below; the `elemType` builtin renders a list element type

// The C# rule-interpreter seam: only what differs from the shared IrExprCtx — the C# builtins (keyword
// escaping, C# type rendering, sub-word wrap-back, interpolation/char escaping) and the C# atom-wrapping
// policy. `thisAlias` is the emitter's operator-body rebind (`this` -> `lhs`) — a C# declaration-shape
// consequence (a C# operator is a static method), so it stays a C#-only context scalar the This rule reads.
class CsExprCtx : public IrExprCtx {
public:
    CsExprCtx(const ir::Expr& e, const BackendSpec& spec, EmitFn emit, InlineFn inlineBlock,
              const std::string& thisAlias)
        : IrExprCtx(e, spec, std::move(emit), std::move(inlineBlock)), thisAlias_(thisAlias) {}

protected:
    std::string targetGet(const std::string& path) const override {
        if (path == "ctx.thisAlias") return thisAlias_;
        return "";
    }

    std::string renderTypeRef(const TypeRef& t) const override { return csType(t); }

    bool wrapAtom(const ir::Expr& c, const std::string& side) const override {
        if (side == "recv") // atom(): wrap a binary/unary/cast receiver
            return c.kind == ir::ExprKind::Binary || c.kind == ir::ExprKind::Unary || c.kind == ir::ExprKind::Cast;
        // "unary": wrap only a binary operand (so `-(a + b)`, but `-x`/`-(-x)` stay bare)
        return c.kind == ir::ExprKind::Binary;
    }

    std::string targetBuiltin(const std::string& name, const std::vector<std::string>& args) const override {
        if (name == "ident")    return csIdent(args.empty() ? std::string() : args[0]);
        if (name == "elemType") return e_.kind == ir::ExprKind::ListLit ? csType(static_cast<const ir::ListLit&>(e_).elem) : "";
        if (name == "castType") return csType(e_.type); // the Cast's target type
        if (name == "genArgs") { // a generic union scrutinee's case patterns need `Full<int>`, not `Full`
            if (e_.kind != ir::ExprKind::Match) return "";
            const auto& ta = static_cast<const ir::Match&>(e_).scrutinee->type.args;
            if (ta.empty()) return "";
            std::string s = "<";
            for (std::size_t i = 0; i < ta.size(); ++i) { if (i) s += ", "; s += csType(ta[i]); }
            return s + ">";
        }
        if (name == "typeArgsSuffix") { // "" or "<T, U>" — New's own type args / a MakeCase's result-type args
            const std::vector<TypeRef>* ta = e_.kind == ir::ExprKind::New ? &static_cast<const ir::New&>(e_).typeArgs
                                           : e_.kind == ir::ExprKind::MakeCase ? &e_.type.args : nullptr;
            if (!ta || ta->empty()) return "";
            std::string s = "<";
            for (std::size_t i = 0; i < ta->size(); ++i) { if (i) s += ", "; s += csType((*ta)[i]); }
            return s + ">";
        }
        if (name == "subWordWrap") { // C# sub-32 arithmetic promotes to int; cast back to wrap at 8/16 bits
            const std::string& tn = args.size() > 0 ? args[0] : std::string();
            const std::string& inner = args.size() > 1 ? args[1] : std::string();
            const char* cast = tn == "i8" ? "sbyte" : tn == "i16" ? "short"
                             : tn == "u8" ? "byte"  : tn == "u16" ? "ushort" : nullptr;
            return cast ? "(" + std::string(cast) + ")(" + inner + ")" : inner;
        }
        if (name == "interpEscape") { // escape a chunk for a C# interpolated-string literal `$"…"`
            std::string s;
            for (char c : args.empty() ? std::string() : args[0]) {
                if (c == '"' || c == '\\') { s += '\\'; s += c; }
                else if (c == '\n') s += "\\n"; // a raw newline/tab/CR would break the `$"…"` literal
                else if (c == '\t') s += "\\t";
                else if (c == '\r') s += "\\r";
                else if (c == '{') s += "{{";
                else if (c == '}') s += "}}";
                else s += c;
            }
            return s;
        }
        if (name == "charLit") { // C# char literal `'x'` (escape `'`, `\`, control chars)
            std::string s = "'";
            for (char c : args.empty() ? std::string() : args[0]) {
                if (c == '\'' || c == '\\') { s += '\\'; s += c; }
                else if (c == '\n') s += "\\n";
                else if (c == '\t') s += "\\t";
                else if (c == '\r') s += "\\r";
                else s += c;
            }
            return s + "'";
        }
        return "";
    }

private:
    const std::string& thisAlias_;
};

// The current module's `extern class` type map (name -> spelling/ctor templates), set at the top of each
// emit() so the free `csType` can consult declared spellings without threading the map through every call
// site. Single-threaded compile; this emitter is the only writer; rebuilt and re-pointed each emit().
const std::unordered_map<std::string, const ir::ExternType*>* g_externTypes = nullptr;

// The C# type-scoped rule context: the `isValueType` predicate (C# `T?` is Nullable<T> for value types
// only) + the extern-class C# spelling + recursion back into csType. The rendering LOGIC lives in the
// "Type" rule (data); this supplies only the reads.
class CsTypeCtx : public TypeRefCtx {
public:
    using TypeRefCtx::TypeRefCtx;

protected:
    std::string targetGet(const std::string& path) const override {
        if (path == "type.isValueType") {
            const std::string& n = t_.name;
            bool v = n == "i8" || n == "i16" || n == "i32" || n == "i64" || n == "u8" || n == "u16" ||
                     n == "u32" || n == "u64" || n == "f32" || n == "f64" || n == "bool" || n == "char";
            return v ? "true" : "false";
        }
        return "";
    }
    std::string externTemplate() const override {
        if (t_.kind != TypeRef::Kind::Named || !g_externTypes) return "";
        auto it = g_externTypes->find(t_.name);
        return it != g_externTypes->end() ? it->second->csType : "";
    }
    std::string renderTypeRef(const TypeRef& t) const override { return csType(t); }
};

// The C# type renderer — a thin wrapper evaluating the "Type" rule, so every caller (expressions via the
// `type` primitive AND the still-imperative declaration emitters) goes through the same data.
std::string csType(const TypeRef& t) {
    CsTypeCtx ctx(t, csharpSpec());
    return engine::evalRule(csharpExprRules().at("Type"), ctx, &csharpExprRules());
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

// The C# declaration hooks — the one per-backend object every decl context reads through.
class CsDeclHooks : public DeclHooks {
public:
    std::string renderTypeRef(const TypeRef& t) const override { return csType(t); }
    std::string ident(const std::string& n) const override { return csIdent(n); }
    std::string generics(const std::vector<ir::GenericParam>& gs) const override { return csGenerics(gs); }
    std::string where(const std::vector<ir::GenericParam>& gs) const override { return csWhere(gs); }
};
const CsDeclHooks kCsDeclHooks;

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
        for (const auto& e : m.enums) { // P19: declarations migrate to decl rules, one kind at a time
            EnumDeclCtx ctx(e);
            runDeclRule(csharpExprRules().at("EnumDecl"), ctx, ctx, &csharpExprRules());
        }
        for (const auto& i : m.interfaces) {
            InterfaceDeclCtx ctx(i, kCsDeclHooks, [this](const ir::Expr& e) { return emitExpr(e); });
            runDeclRule(csharpExprRules().at("InterfaceDecl"), ctx, ctx, &csharpExprRules());
        }
        for (const auto& u : m.unions) {
            UnionDeclCtx ctx(u, kCsDeclHooks);
            runDeclRule(csharpExprRules().at("UnionDecl"), ctx, ctx, &csharpExprRules());
        }
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
        for (const auto& m : r.methods) runMethodRule(r.name, m);
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
        for (const auto& m : c.methods) runMethodRule(c.name, m);
        --indent_;
        line("}");
    }

    // `T name` or `T name = default` — a parameter declaration with its optional default value.
    std::string csParam(const ir::Param& p) {
        std::string s = csType(p.type) + " " + csIdent(p.name);
        if (p.defaultValue) s += " = " + emitExpr(*p.defaultValue);
        return s;
    }

    // Run the MethodDecl rule for one member. The real-static-operator `this`->`lhs` rebind is a C#
    // declaration-shape consequence, so the alias scopes around the rule run (not shared lowering).
    void runMethodRule(const std::string& owner, const ir::Method& m) {
        const bool opAlias = m.kind == ir::MethodKind::Operator && m.opSymbol != "get";
        if (opAlias) thisAlias_ = "lhs";
        MethodDeclCtx ctx(m, owner, kCsDeclHooks, [this](const ir::Expr& e) { return emitExpr(e); });
        runDeclRule(csharpExprRules().at("MethodDecl"), ctx, ctx, &csharpExprRules());
        if (opAlias) thisAlias_.clear();
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
        // A migrated node kind (see csExprRuleKey / CSHARP_EXPR_RULES_JSON) is interpreted from its JSON Rule
        // here; the C++ switch below handles only the kinds whose shape is still imperative (P18 slices 4-10).
        if (const char* key = csExprRuleKey(e.kind); key[0] != '\0') {
            const auto& rules = csharpExprRules();
            auto it = rules.find(key);
            if (it != rules.end()) {
                CsExprCtx ctx(e, spec(), [this](const ir::Expr& c) { return emitExpr(c); },
                              [this](const std::vector<ir::StmtPtr>& b) { return inlineBlock(b); }, thisAlias_);
                return engine::evalRule(it->second, ctx, &rules);
            }
        }
        switch (e.kind) {
            case ir::ExprKind::Bound:
                return substTemplate(static_cast<const ir::Bound&>(e).csTemplate, static_cast<const ir::Bound&>(e));
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
