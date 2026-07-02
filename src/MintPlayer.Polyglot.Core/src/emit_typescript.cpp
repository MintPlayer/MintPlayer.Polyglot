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
        case ir::ExprKind::Binary:     return "Binary";
        default:                       return "";
    }
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
    std::unordered_set<std::string> recordNames_;    // backs emitRecordEquals' structural-== dispatch
    std::unordered_set<std::string> interfaceNames_; // names declared as interfaces (class implements vs extends)
    std::unordered_map<std::string, const ir::ExternType*> externMap_; // backs g_externTypes for this emit

    bool isRecordType(const TypeRef& t) const {
        return t.kind == TypeRef::Kind::Named && recordNames_.count(t.name) != 0;
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
        std::string sig = std::string(m.isStatic ? "static " : "") + (m.isAsync ? "async " : "") +
                          m.name + tsGenerics(m.generics) + "(";
        for (std::size_t i = 0; i < m.params.size(); ++i) { if (i) sig += ", "; sig += tsParam(m.params[i]); }
        sig += "): " + (m.isAsync ? tsAsyncReturn(m.returnType) : tsType(m.returnType)) + " {";
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
                TsExprCtx ctx(e, spec(), [this](const ir::Expr& c) { return emitExpr(c); });
                return engine::evalRule(it->second, ctx);
            }
        }
        switch (e.kind) {
            case ir::ExprKind::Bound:
                return substTemplate(static_cast<const ir::Bound&>(e).tsTemplate, static_cast<const ir::Bound&>(e));
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
