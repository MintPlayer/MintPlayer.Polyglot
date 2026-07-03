#include "mintplayer/polyglot/emit.hpp"

#include <cctype>
#include <string>
#include <unordered_set>

#include "mintplayer/polyglot/backend_spec.hpp"
#include "mintplayer/polyglot/backend_spec_json.hpp"
#include "mintplayer/polyglot/emitter_base.hpp"

#include <cassert>

// Hand-written IR -> TypeScript pretty-printer. Walks the typed IR; emits free `function`s, maps the
// `print` intrinsic -> console.log, and appends a top-level call to the entry function. Output stays
// plain enough to run under Node's type-stripping (the P2 differential conformance test relies on it).

namespace mintplayer::polyglot {

namespace {

// The TS backend's declarative data, now a JSON spec (P18: the tabular ~70% loads from data — PRD §4.10).
// Unlike C#, TS maps `char` -> `string` and the 64-bit ints -> `bigint`; structural cases
// (List/tuple/function/nullable) stay in tsType. Only the Spec's source moved; output is byte-identical.
const char* TS_SPEC_JSON = R"JSON({
  "name": "typescript",
  "scalarType": { "unit": "void", "bool": "boolean", "string": "string", "char": "string",
                  "i64": "bigint", "u64": "bigint",
                  "i8": "number", "i16": "number", "i32": "number", "u8": "number", "u16": "number",
                  "u32": "number", "f32": "number", "f64": "number" },
  "intSuffix": { "i64": "n", "u64": "n" },
  "binaryOp": { "==": "===", "!=": "!==" },
  "delimited": { "tuple": { "open": "[", "sep": ", ", "close": "]" },
                 "list":  { "open": "[", "sep": ", ", "close": "]" } },
  "blockStyle": "bracesKnR",
  "stmtEnd": ";",
  "throwKeyword": "throw",
  "trueLit": "true", "falseLit": "false", "nullLit": "null"
})JSON";

const BackendSpec& typescriptSpec() {
    static const BackendSpec spec = [] {
        SpecLoadResult r = loadBackendSpec(TS_SPEC_JSON);
        assert(r.ok && "embedded TypeScript backend spec must parse");
        return r.spec;
    }();
    return spec;
}

// P18: the TS expression walk as declarative JSON Rules — the same interpreter + primitives that drive the
// C# rules, proving the DSL isn't shaped around C#. Per-target differences live in the DATA: `Char` is a
// string (TS has no char), `Var` emits verbatim (no keyword escaping), `Call` uses the overload-mangled
// callee, `Index` dispatches on `node.receiverHasIndexer` (TS has no `[]` overload -> `.get(i)`), `This`
// is a plain literal (only C# rebinds `this` in operator bodies), Tuple/ListLit brackets come from the
// spec's `delimited` table.
const char* TS_EXPR_RULES_JSON = R"JSON({
  "Int":   { "tmpl": [ {"get":"node.text"}, {"fn":"intSuffix","args":[{"get":"node.type"}]} ] },
  "Float": { "get": "node.text" },
  "Bool":  { "case": { "when": [ [ {"eq":["node.value","true"]}, {"get":"spec.trueLit"} ] ],
                       "else": {"get":"spec.falseLit"} } },
  "Null":  { "get": "spec.nullLit" },
  "Str":   { "fn": "escapeString", "args": [ {"get":"node.value"} ] },
  "Char":  { "fn": "escapeString", "args": [ {"get":"node.value"} ] },
  "Var":    { "get": "node.name" },
  "This":   "this",
  "Extern": { "get": "node.code" },
  "Await":  { "tmpl": [ "await ", {"emitChild":"node.operand","side":"recv"} ] },
  "Call": { "tmpl": [ {"get":"node.mangledCallee"}, "(", {"map":"node.args","sep":", "}, ")" ] },
  "Member": { "tmpl": [
                { "case": { "when": [ [ {"has":"node.staticType"}, {"get":"node.staticType"} ] ],
                            "else": {"emitChild":"node.object","side":"recv"} } },
                { "case": { "when": [ [ {"eq":["node.nullSafe","true"]}, "?." ] ], "else": "." } },
                {"get":"node.field"} ] },
  "Index": { "case": { "when": [ [ {"eq":["node.receiverHasIndexer","true"]},
                { "tmpl": [ {"emitChild":"node.receiver","side":"recv"}, ".get(", {"emit":"node.index"}, ")" ] } ] ],
              "else": { "tmpl": [ {"emitChild":"node.receiver","side":"recv"}, "[", {"emit":"node.index"}, "]" ] } } },
  "Cond":  { "tmpl": [ "(", {"emit":"node.cond"}, " ? ", {"emit":"node.then"},
                       " : ", {"emit":"node.els"}, ")" ] },
  "Tuple": { "tmpl": [ {"get":"spec.delimited.tuple.open"}, {"map":"node.elements","sep":", "},
                       {"get":"spec.delimited.tuple.close"} ] },
  "ListLit": { "tmpl": [ {"get":"spec.delimited.list.open"}, {"map":"node.elements","sep":", "},
                         {"get":"spec.delimited.list.close"} ] },
  "New": { "tmpl": [ "new ", {"get":"node.typeName"}, {"fn":"typeArgsSuffix"},
                     "(", {"map":"node.args","sep":", "}, ")" ] },
  "MakeCase": { "tmpl": [ "{ tag: ", {"fn":"escapeString","args":[{"get":"node.caseName"}]},
                          {"map":"node.fields","sep":"",
                           "item":{"tmpl":[", ",{"get":"item.name"},": ",{"emit":"item.value"}]}}, " }" ] },
  "Unary": { "case": { "when": [
               [ {"and":[{"eq":["node.typeIsSmallInt","true"]},{"eq":["node.op","-"]}]},
                 {"fn":"narrowWrap","args":[{"get":"node.type"},
                   {"tmpl":["-",{"emitChild":"node.operand","side":"unary"}]}]} ] ],
             "else": {"tmpl":[{"get":"node.op"},{"emitChild":"node.operand","side":"unary"}]} } },
  "Cast": { "fn": "convert", "args": [ {"emit":"node.operand"} ] },
  "With": { "case": { "when": [ [ {"eq":["node.baseIsSimple","true"]},
              {"tmpl":["new ",{"get":"node.type"},"(",{"map":"node.ctorArgs","sep":", "},")"]} ] ],
            "else": {"tmpl":["(() => { const ",{"get":"node.tempName"}," = ",{"emit":"node.base"},"; return new ",
                             {"get":"node.type"},"(",{"map":"node.ctorArgs","sep":", "},"); })()"]} } },
  "Interp": { "tmpl": [ "`", {"interleave":{"lits":"node.chunks","holes":"node.holes",
                "lit":{"fn":"interpEscape","args":[{"get":"item"}]},
                "hole":{"tmpl":["${",{"emit":"item"},"}"]}}}, "`" ] },
  "MethodCall": { "case": { "when": [ [ {"eq":["node.isExtension","true"]},
      {"tmpl":[{"get":"node.method"},"(",{"emit":"node.object"},
        {"case":{"when":[[{"eq":["node.args.count","0"]},""]],
                 "else":{"tmpl":[", ",{"map":"node.args","sep":", "}]}}},")"]} ] ],
    "else": {"tmpl":[
      { "case": { "when": [ [ {"has":"node.staticType"}, {"get":"node.staticType"} ] ],
                  "else": {"emitChild":"node.object","side":"recv"} } },
      ".",{"get":"node.method"},"(",{"map":"node.args","sep":", "},")"]} } },
  "Match": { "tmpl": [ "(() => { const _m = ", {"emit":"node.scrutinee"}, "; ",
    {"map":"node.arms","sep":"","item":{ "case": { "when": [
      [ {"eq":["item.pattern.kind","ctor"]},
        {"tmpl":["if (_m.tag === ",{"fn":"escapeString","args":[{"get":"item.pattern.ctorCase"}]},
          {"case":{"when":[[{"eq":["item.hasGuard","true"]},{"tmpl":[" && (",{"emit":"item.guard"},")"]}]],"else":""}},
          ") { ",
          {"map":"item.pattern.binders","sep":"",
           "item":{"tmpl":["const ",{"get":"item.binding"}," = _m.",{"get":"item.field"},"; "]}},
          "return ",{"emit":"item.body"},"; } "]} ],
      [ {"eq":["item.pattern.kind","binding"]},
        {"tmpl":["{ const ",{"get":"item.pattern.binding"}," = _m; ",
          {"case":{"when":[[{"eq":["item.hasGuard","true"]},
             {"tmpl":["if (",{"emit":"item.guard"},") return ",{"emit":"item.body"},"; }"]}]],
            "else":{"tmpl":["return ",{"emit":"item.body"},"; }"]}}}," "]} ],
      [ {"eq":["item.pattern.kind","wildcard"]},
        {"case":{"when":[[{"eq":["item.hasGuard","true"]},
           {"tmpl":["if (",{"emit":"item.guard"},") return ",{"emit":"item.body"},"; "]}]],
          "else":{"tmpl":["return ",{"emit":"item.body"},"; "]}}} ] ],
    "else": {"tmpl":["if (_m === ",
        {"case":{"when":[[{"eq":["item.pattern.kind","literal"]},{"emit":"item.pattern.literal"}]],
          "else":{"tmpl":[{"get":"item.pattern.enumType"},".",{"get":"item.pattern.enumCase"}]}}},
        {"case":{"when":[[{"eq":["item.hasGuard","true"]},{"tmpl":[" && (",{"emit":"item.guard"},")"]}]],"else":""}},
        ") return ",{"emit":"item.body"},"; "]} } }},
    "})()" ] },
  "Lambda": { "tmpl": [ "(",
      {"map":"node.params","sep":", ","item":{"tmpl":[{"get":"item.name"},
        {"case":{"when":[[{"eq":["item.hasType","true"]},{"tmpl":[": ",{"type":"item.type"}]}]],"else":""}}]}},
      ") => ",
      {"case":{"when":[[{"eq":["node.exprBodied","true"]},{"emit":"node.body"}]],
        "else":{"tmpl":["{ ",{"fn":"inlineBlock"},"}"]}}} ] },
  "EnumDecl": { "seq": [
      { "line": { "tmpl": [ "type ", {"get":"decl.name"}, " = number;" ] } },
      { "line": { "tmpl": [ "const ", {"get":"decl.name"}, " = { ",
          {"map":"decl.cases","sep":", ","item":{"tmpl":[{"get":"item.name"},": ",{"get":"item.value"}]}},
          " };" ] } } ] },
  "tsParam": {"tmpl":[{"get":"item.name"},": ",{"type":"item.type"},
      {"case":{"when":[[{"eq":["item.hasDefault","true"]},{"tmpl":[" = ",{"emit":"item.default"}]}]]}}]},
  "tsMethodSig": {"tmpl":[
      {"case":{"when":[[{"eq":["decl.isStatic","true"]},"static "]]}},
      {"case":{"when":[[{"eq":["decl.isAsync","true"]},"async "]]}},
      {"get":"decl.name"},{"fn":"generics"},"(",
      {"map":"decl.params","sep":", ","item":{"call":"tsParam"}},"): ",
      {"case":{"when":[[{"eq":["decl.isAsync","true"]},{"tmpl":["Promise<",{"type":"decl.returnType"},">"]}]],
        "else":{"type":"decl.returnType"}}}]},
  "MethodDecl": {"case":{"when":[
      [ {"eq":["decl.kind","property"]},
        {"block":{"head":{"tmpl":["get ",{"get":"decl.name"},"(): ",{"type":"decl.returnType"}]},
          "body":[{"line":{"tmpl":["return ",{"emit":"decl.exprBody"},";"]}}]}} ]],
      "else":{"block":{"head":{"call":"tsMethodSig"},"body":[
          {"case":{"when":[[{"eq":["decl.exprBodied","true"]},
              {"line":{"tmpl":["return ",{"emit":"decl.exprBody"},";"]}}]],
            "else":{"stmts":"decl.body"}}} ]}} }},
  "InterfaceDecl": { "block": {
      "head": {"tmpl": [ "interface ", {"get":"decl.name"}, {"fn":"generics"},
        {"case":{"when":[[{"eq":["decl.bases.count","0"]},""]],
          "else":{"tmpl":[" extends ",{"map":"decl.bases","sep":", ","item":{"type":"item"}}]}}} ] },
      "body": [
        { "mapDecl": "decl.methods", "each": { "line": { "tmpl": [
            {"get":"item.name"}, {"fn":"generics","args":[{"get":"item.#"}]}, "(",
            {"map":"item.params","sep":", ","item":{"tmpl":[
                {"get":"item.name"},": ",{"type":"item.type"},
                {"case":{"when":[[{"eq":["item.hasDefault","true"]},
                  {"tmpl":[" = ",{"emit":"item.default"}]}]]}} ]}},
            "): ", {"type":"item.returnType"}, ";" ] } } } ] } },
  "UnionDecl": { "line": { "tmpl": [ "type ", {"get":"decl.name"}, {"fn":"generics"}, " = ",
      {"map":"decl.cases","sep":" | ","item":{"tmpl":["{ tag: \"",{"get":"item.name"},"\"",
        {"map":"item.fields","sep":"","item":{"tmpl":["; ",{"get":"item.name"},": ",{"type":"item.type"}]}},
        " }"]}},
      ";" ] } },
  "Type": { "case": { "when": [
      [ {"eq":["type.kind","function"]},
        {"tmpl":["(",{"map":"type.args","sep":", ","item":{"tmpl":["arg",{"get":"item.#"},": ",{"type":"item"}]}},
          ") => ",{"case":{"when":[[{"eq":["type.hasRet","true"]},{"type":"type.ret"}]],"else":"void"}}]} ],
      [ {"eq":["type.kind","tuple"]}, {"tmpl":["[",{"map":"type.args","sep":", ","item":{"type":"item"}},"]"]} ],
      [ {"eq":["type.nullable","true"]}, {"tmpl":[{"type":"type.base"}," | null"]} ],
      [ {"has":"type.scalar"}, {"get":"type.scalar"} ],
      [ {"eq":["type.nameEmpty","true"]}, "unknown" ],
      [ {"has":"type.externTemplate"}, {"fn":"substExtern"} ] ],
    "else": {"tmpl":[{"get":"type.name"},
      {"case":{"when":[[{"eq":["type.args.count","0"]},""]],
        "else":{"tmpl":["<",{"map":"type.args","sep":", ","item":{"type":"item"}},">"]}}}]} } },
  "Binary": { "case": { "when": [
      [ {"and":[{"or":[{"eq":["node.op","=="]},{"eq":["node.op","!="]}]},{"eq":["node.lhsIsRecord","true"]}]},
        { "case": { "when": [ [ {"eq":["node.op","=="]},
            {"tmpl":[{"emitChild":"node.lhs","side":"recv"},".equals(",{"emit":"node.rhs"},")"]} ] ],
          "else": {"tmpl":["!",{"emitChild":"node.lhs","side":"recv"},".equals(",{"emit":"node.rhs"},")"]} } } ],
      [ {"or":[{"eq":["node.op","=="]},{"eq":["node.op","!="]}]},
        {"tmpl":[{"emitChild":"node.lhs","side":"l"}," ",{"fn":"opSpelling","args":[{"get":"node.op"}]}," ",
                 {"emitChild":"node.rhs","side":"r"}]} ],
      [ {"eq":["node.hasOpMethod","true"]},
        {"tmpl":[{"emitChild":"node.lhs","side":"recv"},".",{"fn":"opMethod","args":[{"get":"node.op"}]},
                 "(",{"emit":"node.rhs"},")"]} ],
      [ {"or":[{"eq":["node.type","i64"]},{"eq":["node.type","u64"]}]},
        {"fn":"i64Wrap","args":[{"get":"node.type"},
          {"tmpl":[{"emitChild":"node.lhs","side":"l"}," ",{"get":"node.op"}," ",{"emitChild":"node.rhs","side":"r"}]}]} ],
      [ {"and":[{"eq":["node.typeIsSmallInt","true"]},{"eq":["node.op","*"]}]},
        {"fn":"imul","args":[{"get":"node.type"},{"emit":"node.lhs"},{"emit":"node.rhs"}]} ],
      [ {"and":[{"eq":["node.typeIsSmallInt","true"]},
                {"or":[{"eq":["node.op","+"]},{"eq":["node.op","-"]},{"eq":["node.op","/"]},{"eq":["node.op","%"]}]}]},
        {"fn":"narrowWrap","args":[{"get":"node.type"},
          {"tmpl":[{"emitChild":"node.lhs","side":"l"}," ",{"get":"node.op"}," ",{"emitChild":"node.rhs","side":"r"}]}]} ] ],
    "else": {"tmpl":[{"emitChild":"node.lhs","side":"l"}," ",{"fn":"opSpelling","args":[{"get":"node.op"}]}," ",
                     {"emitChild":"node.rhs","side":"r"}]} } }
})JSON";

const std::unordered_map<std::string, engine::Rule>& tsExprRules() {
    static const std::unordered_map<std::string, engine::Rule> rules = [] {
        std::unordered_map<std::string, engine::Rule> m;
        json::Value doc = json::parse(TS_EXPR_RULES_JSON);
        for (const auto& kv : doc.members) {
            bool ok = true;
            std::string err;
            engine::Rule r = engine::parseRule(kv.second, ok, err);
            assert(ok && "embedded TS expr rule must parse");
            m.emplace(kv.first, std::move(r));
        }
        return m;
    }();
    return rules;
}

// The ExprKind name keying the TS rule table ("" routes to the C++ switch).
const char* tsExprRuleKey(ir::ExprKind k) {
    switch (k) {
        case ir::ExprKind::Int:     return "Int";
        case ir::ExprKind::Float:   return "Float";
        case ir::ExprKind::Bool:    return "Bool";
        case ir::ExprKind::Null:    return "Null";
        case ir::ExprKind::Str:     return "Str";
        case ir::ExprKind::Char:    return "Char";
        case ir::ExprKind::Var:     return "Var";
        case ir::ExprKind::This:    return "This";
        case ir::ExprKind::Extern:  return "Extern";
        case ir::ExprKind::Await:   return "Await";
        case ir::ExprKind::Call:    return "Call";
        case ir::ExprKind::Member:  return "Member";
        case ir::ExprKind::Index:   return "Index";
        case ir::ExprKind::Cond:    return "Cond";
        case ir::ExprKind::Tuple:    return "Tuple";
        case ir::ExprKind::ListLit:  return "ListLit";
        case ir::ExprKind::New:      return "New";
        case ir::ExprKind::MakeCase: return "MakeCase";
        case ir::ExprKind::Unary:    return "Unary";
        case ir::ExprKind::Cast:     return "Cast";
        case ir::ExprKind::With:       return "With";
        case ir::ExprKind::Interp:     return "Interp";
        case ir::ExprKind::MethodCall: return "MethodCall";
        case ir::ExprKind::Match:      return "Match";
        case ir::ExprKind::Lambda:     return "Lambda";
        case ir::ExprKind::Binary:     return "Binary";
        default:                       return "";
    }
}

// The current module's `extern class` type map; see the identical note in emit_csharp.cpp. Set per emit().
const std::unordered_map<std::string, const ir::ExternType*>* g_externTypes = nullptr;

std::string tsType(const TypeRef& t);

// The TS type-scoped rule context: the extern-class TS spelling + recursion back into tsType. The
// rendering LOGIC lives in the "Type" rule (data); this supplies only the reads.
class TsTypeCtx : public TypeRefCtx {
public:
    using TypeRefCtx::TypeRefCtx;

protected:
    std::string externTemplate() const override {
        if (t_.kind != TypeRef::Kind::Named || !g_externTypes) return "";
        auto it = g_externTypes->find(t_.name);
        return it != g_externTypes->end() ? it->second->tsType : "";
    }
    std::string renderTypeRef(const TypeRef& t) const override { return tsType(t); }
};

// The TS type renderer — a thin wrapper evaluating the "Type" rule, so every caller (expressions via the
// `type` primitive AND the still-imperative declaration emitters) goes through the same data.
std::string tsType(const TypeRef& t) {
    TsTypeCtx ctx(t, typescriptSpec());
    return engine::evalRule(tsExprRules().at("Type"), ctx, &tsExprRules());
}

bool isScalarName(const std::string& n) {
    return n == "i8" || n == "i16" || n == "i32" || n == "i64" || n == "u8" || n == "u16" || n == "u32" ||
           n == "u64" || n == "f32" || n == "f64" || n == "bool" || n == "char" || n == "string" || n == "unit";
}
bool isUserType(const TypeRef& t) {
    return t.kind == TypeRef::Kind::Named && !t.name.empty() && !isScalarName(t.name);
}
bool isI64(const TypeRef& t) { return t.kind == TypeRef::Kind::Named && (t.name == "i64" || t.name == "u64"); }
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

// TS carries bounds inline on each parameter: `<T extends A & B, U>`. The `INumber` marker is a Polyglot
// compile-time-only numeric constraint (no TS equivalent), so it is erased here.
std::string tsGenerics(const std::vector<ir::GenericParam>& gs) {
    if (gs.empty()) return "";
    std::string s = "<";
    for (std::size_t i = 0; i < gs.size(); ++i) {
        if (i) s += ", ";
        s += gs[i].name;
        std::vector<const TypeRef*> bounds;
        for (const auto& b : gs[i].bounds) if (b.name != "INumber") bounds.push_back(&b);
        if (!bounds.empty()) {
            s += " extends ";
            for (std::size_t j = 0; j < bounds.size(); ++j) { if (j) s += " & "; s += tsType(*bounds[j]); }
        }
    }
    return s + ">";
}

// The TS declaration hooks — the one per-backend object every decl context reads through.
class TsDeclHooks : public DeclHooks {
public:
    std::string renderTypeRef(const TypeRef& t) const override { return tsType(t); }
    std::string generics(const std::vector<ir::GenericParam>& gs) const override { return tsGenerics(gs); }
};
const TsDeclHooks kTsDeclHooks;

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

// The TS rule-interpreter seam: only what differs from the shared IrExprCtx — the TS builtins, the TS
// atom-wrapping policy, and the TS-specific predicates (the module facts themselves — lhsIsRecord,
// receiverHasIndexer — are lowering-precomputed IR bits the shared IrExprCtx reads).
class TsExprCtx : public IrExprCtx {
public:
    using IrExprCtx::IrExprCtx;

protected:
    bool wrapAtom(const ir::Expr& c, const std::string& side) const override {
        if (side == "recv") { // TS atom(): wrap a unary; wrap a binary only when it stays an operator
            if (c.kind == ir::ExprKind::Unary) return true;
            // a scalar binary needs parens as a receiver; a user-type binary emits a (high-binding) method call
            return c.kind == ir::ExprKind::Binary && !static_cast<const ir::Binary&>(c).lhsIsUserType;
        }
        // "unary": wrap only a binary operand
        return c.kind == ir::ExprKind::Binary;
    }

    std::string targetGet(const std::string& path) const override {
        if (path == "node.typeIsSmallInt") return isSmallInt(e_.type) ? "true" : "false";
        if (path == "node.hasOpMethod" && e_.kind == ir::ExprKind::Binary) {
            const auto& b = static_cast<const ir::Binary&>(e_); // operator overload -> method call (no TS operators)
            return (b.lhsIsUserType && !opMethod(b.op).empty()) ? "true" : "false";
        }
        return "";
    }

    std::string renderTypeRef(const TypeRef& t) const override { return tsType(t); }

    std::string targetBuiltin(const std::string& name, const std::vector<std::string>& args) const override {
        if (name == "typeArgsSuffix") { // "" or "<T, U>" — a New's construction type args
            if (e_.kind != ir::ExprKind::New) return "";
            const auto& ta = static_cast<const ir::New&>(e_).typeArgs;
            if (ta.empty()) return "";
            std::string s = "<";
            for (std::size_t i = 0; i < ta.size(); ++i) { if (i) s += ", "; s += tsType(ta[i]); }
            return s + ">";
        }
        // §3.C numeric faithfulness — fixed Core primitives the rules select among, never author.
        if (name == "narrowWrap") // re-narrow a 32-bit-or-narrower int result to its value range
            return narrowTs(args.size() > 0 ? args[0] : std::string(), args.size() > 1 ? args[1] : std::string());
        if (name == "i64Wrap") {  // wrap BigInt arithmetic to 64 bits so it overflows like .NET long/ulong
            const std::string& tn = args.size() > 0 ? args[0] : std::string();
            const std::string& inner = args.size() > 1 ? args[1] : std::string();
            return std::string(tn == "u64" ? "BigInt.asUintN(64, " : "BigInt.asIntN(64, ") + inner + ")";
        }
        if (name == "imul") {     // small-int `*`: a plain product can exceed 2^53 and lose low bits first
            const std::string& tn = args.size() > 0 ? args[0] : std::string();
            std::string prod = "Math.imul(" + (args.size() > 1 ? args[1] : std::string()) + ", " +
                               (args.size() > 2 ? args[2] : std::string()) + ")";
            return tn == "i32" ? prod : narrowTs(tn, prod); // Math.imul already yields i32
        }
        if (name == "opMethod") return opMethod(args.empty() ? std::string() : args[0]);
        if (name == "convert") { // the numeric-conversion algorithm (BigInt boundaries etc.) stays fixed C++
            if (e_.kind != ir::ExprKind::Cast) return "";
            return tsConvert(static_cast<const ir::Cast&>(e_).operand->type, e_.type,
                             args.empty() ? std::string() : args[0]);
        }
        if (name == "interpEscape") { // escape a chunk for a TS template literal `` `…` ``
            const std::string& chunk = args.empty() ? std::string() : args[0];
            std::string s;
            for (std::size_t j = 0; j < chunk.size(); ++j) {
                char c = chunk[j];
                if (c == '`' || c == '\\') { s += '\\'; s += c; }
                else if (c == '$' && j + 1 < chunk.size() && chunk[j + 1] == '{') s += "\\$";
                else s += c;
            }
            return s;
        }
        return "";
    }
};

class TypeScriptEmitter : public EmitterBase {
public:
    std::string emit(const ir::Module& m) {
        out_.clear();
        indent_ = 0;
        recordNames_.clear();
        interfaceNames_.clear();
        for (const auto& i : m.interfaces) interfaceNames_.insert(i.name); // a class `implements` these, `extends` a class
        externMap_.clear();
        for (const auto& et : m.externTypes) externMap_[et.name] = &et;
        g_externTypes = &externMap_;
        // records compare structurally (§3.C); a TS record is a class (the set backs emitRecordEquals —
        // expression-level record/indexer facts are lowering-precomputed IR bits now)
        for (const auto& r : m.records) recordNames_.insert(r.name);
        for (const auto& e : m.enums) { // P19: declarations migrate to decl rules, one kind at a time
            EnumDeclCtx ctx(e);
            runDeclRule(tsExprRules().at("EnumDecl"), ctx, ctx, &tsExprRules());
        }
        for (const auto& i : m.interfaces) {
            InterfaceDeclCtx ctx(i, kTsDeclHooks, [this](const ir::Expr& e) { return emitExpr(e); });
            runDeclRule(tsExprRules().at("InterfaceDecl"), ctx, ctx, &tsExprRules());
        }
        for (const auto& u : m.unions) {
            UnionDeclCtx ctx(u, kTsDeclHooks);
            runDeclRule(tsExprRules().at("UnionDecl"), ctx, ctx, &tsExprRules());
        }
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
    std::unordered_set<std::string> recordNames_;    // backs emitRecordEquals' structural-== dispatch
    std::unordered_set<std::string> interfaceNames_; // names declared as interfaces (class implements vs extends)
    std::unordered_map<std::string, const ir::ExternType*> externMap_; // backs g_externTypes for this emit

    bool isRecordType(const TypeRef& t) const {
        return t.kind == TypeRef::Kind::Named && recordNames_.count(t.name) != 0;
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
        for (const auto& m : r.methods) runMethodRule(m);
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
        for (const auto& m : c.methods) runMethodRule(m);
        --indent_;
        line("}");
    }

    // `name: T` or `name: T = default` — a parameter declaration with its optional default value.
    std::string tsParam(const ir::Param& p) {
        std::string s = p.name + ": " + tsType(p.type);
        if (p.defaultValue) s += " = " + emitExpr(*p.defaultValue);
        return s;
    }

    void runMethodRule(const ir::Method& m) {
        MethodDeclCtx ctx(m, "", kTsDeclHooks, [this](const ir::Expr& e) { return emitExpr(e); });
        runDeclRule(tsExprRules().at("MethodDecl"), ctx, ctx, &tsExprRules());
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

    // `async fn`: author writes the unwrapped `T`; TS needs `Promise<T>` (`Promise<void>` for unit). §4.7
    std::string tsAsyncReturn(const TypeRef& ret) { return "Promise<" + tsType(ret) + ">"; }

    void emitFunction(const ir::Function& fn) {
        std::string kw = fn.isAsync ? "async function " : (fn.isIterator ? "function* " : "function ");
        std::string sig = kw + fn.mangledName + tsGenerics(fn.generics) + "(";
        for (std::size_t i = 0; i < fn.params.size(); ++i) { if (i) sig += ", "; sig += tsParam(fn.params[i]); }
        sig += "): " + (fn.isAsync ? tsAsyncReturn(fn.returnType) : tsType(fn.returnType));
        headBlock(sig, fn.body);
    }

    // TS has a single untyped `catch`, so a typed/guarded catch list becomes an instanceof/guard
    // dispatch chain. A `__handled` flag reproduces C#'s semantics: a clause whose type matches but
    // whose `when` guard fails falls through to the next clause; if none handle it, the error rethrows.
    void emitTry(const ir::Try& t) {
        line("try {");
        blockBody(t.body);
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
        if (t.hasFinally) { line("} finally {"); blockBody(t.finallyBody); }
        line("}");
    }

    const BackendSpec& spec() const override { return typescriptSpec(); }

    std::string localDecl(const std::string& name, bool isMutable) override { return std::string(isMutable ? "let " : "const ") + name; }
    std::string yieldStmt(const std::string& v, bool hasValue) override { return hasValue ? "yield " + v + ";" : "return;"; }
    std::string rethrowStmt() override { return "throw __e;"; }

    void emitStmtTarget(const ir::Stmt& s) override {
        switch (s.kind) {
            case ir::StmtKind::Try:
                emitTry(static_cast<const ir::Try&>(s));
                break;
            case ir::StmtKind::For: {
                const auto& f = static_cast<const ir::For&>(s);
                std::string head;
                if (f.isRange) {
                    std::string cmp = f.inclusive ? " <= " : " < ";
                    head = "for (let " + f.binding + " = " + emitExpr(*f.rangeStart) + "; " +
                           f.binding + cmp + emitExpr(*f.rangeEnd) + "; " + f.binding + "++)";
                } else if (!f.tupleBindings.empty()) { // `for (const [a, b] of seq)`
                    std::string names;
                    for (std::size_t i = 0; i < f.tupleBindings.size(); ++i) { if (i) names += ", "; names += f.tupleBindings[i]; }
                    head = "for (const [" + names + "] of " + emitExpr(*f.iterable) + ")";
                } else {
                    head = "for (const " + f.binding + " of " + emitExpr(*f.iterable) + ")";
                }
                headBlock(head, f.body);
                break;
            }
        }
    }

    std::string emitExpr(const ir::Expr& e) override {
        // A migrated node kind (see tsExprRuleKey / TS_EXPR_RULES_JSON) is interpreted from its JSON Rule
        // here; the C++ switch below handles only the kinds whose shape is still imperative.
        if (const char* key = tsExprRuleKey(e.kind); key[0] != '\0') {
            const auto& rules = tsExprRules();
            auto it = rules.find(key);
            if (it != rules.end()) {
                TsExprCtx ctx(e, spec(), [this](const ir::Expr& c) { return emitExpr(c); },
                              [this](const std::vector<ir::StmtPtr>& b) { return inlineBlock(b); });
                return engine::evalRule(it->second, ctx, &rules);
            }
        }
        switch (e.kind) {
            case ir::ExprKind::Bound:
                return substTemplate(static_cast<const ir::Bound&>(e).tsTemplate, static_cast<const ir::Bound&>(e));
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
