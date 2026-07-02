#include "mintplayer/polyglot/emit.hpp"

#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mintplayer/polyglot/backend_spec.hpp"
#include "mintplayer/polyglot/backend_spec_json.hpp"
#include "mintplayer/polyglot/emitter_base.hpp"

#include <cassert>

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

// Python's declarative Spec, now a JSON spec (P18 — PRD §4.10). The engine consults it for block style,
// statement terminator, throw keyword, and operator spellings (`&&`/`||` -> `and`/`or`). The type table
// stays empty: Python emits no type annotations. Only the Spec's source moved; output is byte-identical.
const char* PY_SPEC_JSON = R"JSON({
  "name": "python",
  "scalarType": {}, "intSuffix": {}, "delimited": {},
  "binaryOp": { "&&": "and", "||": "or" },
  "blockStyle": "colonIndent",
  "stmtEnd": "",
  "throwKeyword": "raise",
  "trueLit": "True", "falseLit": "False", "nullLit": "None"
})JSON";

const BackendSpec& pythonSpec() {
    static const BackendSpec spec = [] {
        SpecLoadResult r = loadBackendSpec(PY_SPEC_JSON);
        assert(r.ok && "embedded Python backend spec must parse");
        return r.spec;
    }();
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

// An overloaded function's mangled name (`area$i32`) carries a `$` that's invalid in a Python identifier;
// map it to `_` consistently at every def and call site so the overloads stay distinct and callable.
std::string pyName(const std::string& s) {
    std::string out = s;
    for (char& c : out) if (c == '$') c = '_';
    return out;
}

// Escape a source identifier that collides with a Python keyword (e.g. a local `def`/`class`/`in`) by
// suffixing `_`. Applied uniformly to every binding site and reference so the renaming stays consistent.
std::string pyId(const std::string& s) {
    static const std::unordered_set<std::string> kw = {
        "False","None","True","and","as","assert","async","await","break","class","continue","def","del",
        "elif","else","except","finally","for","from","global","if","import","in","is","lambda","nonlocal",
        "not","or","pass","raise","return","try","while","with","yield","match","case"};
    return kw.count(s) ? s + "_" : s;
}

// §3.C integer faithfulness: wrap an int result to its width so it overflows like .NET (Python ints are
// arbitrary-precision). Unsigned masks; signed masks then sign-extends via the (m ^ SIGN) - SIGN trick —
// `x` appears once, so it's safe to inline without a temporary.
std::string wrapInt(const std::string& n, const std::string& x) {
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

// P18: the Python expression walk as declarative JSON Rules — the third consumer of the same interpreter,
// and the non-sibling stress test (colon+indent, `not`/`and`/`or`, walrus null-safety, no `new`). Per-target
// shape lives in the DATA: `This` is the literal `self`, `Cond` is `then if cond else els`, a 1-`Tuple`
// needs the trailing comma, `MakeCase` is a tagged dict with QUOTED field keys, `New` has no keyword and no
// type args. The stateful pieces (walrus temporaries for `?.`/`??`, the `_pg_idiv` prelude flag) are fixed
// builtins that reach the emitter's counters by reference — a plugin selects them, never authors them.
const char* PY_EXPR_RULES_JSON = R"JSON({
  "Int":   { "tmpl": [ {"get":"node.text"}, {"fn":"intSuffix","args":[{"get":"node.type"}]} ] },
  "Float": { "get": "node.text" },
  "Bool":  { "case": { "when": [ [ {"eq":["node.value","true"]}, {"get":"spec.trueLit"} ] ],
                       "else": {"get":"spec.falseLit"} } },
  "Null":  { "get": "spec.nullLit" },
  "Str":   { "fn": "escapeString", "args": [ {"get":"node.value"} ] },
  "Var":    { "fn": "ident", "args": [ {"get":"node.name"} ] },
  "This":   "self",
  "Extern": { "get": "node.code" },
  "Await":  { "tmpl": [ "await ", {"emitChild":"node.operand","side":"recv"} ] },
  "Call": { "tmpl": [ {"fn":"mangleName","args":[{"get":"node.mangledCallee"}]},
                      "(", {"map":"node.args","sep":", "}, ")" ] },
  "Member": { "case": { "when": [ [ {"eq":["node.nullSafe","true"]}, {"fn":"nullSafeMember"} ] ],
              "else": { "tmpl": [
                { "case": { "when": [ [ {"has":"node.staticType"}, {"get":"node.staticType"} ] ],
                            "else": {"emitChild":"node.object","side":"recv"} } },
                ".", {"get":"node.field"} ] } } },
  "Index": { "tmpl": [ {"emitChild":"node.receiver","side":"recv"}, "[", {"emit":"node.index"}, "]" ] },
  "Cond":  { "tmpl": [ "(", {"emit":"node.then"}, " if ", {"emit":"node.cond"},
                       " else ", {"emit":"node.els"}, ")" ] },
  "ListLit": { "tmpl": [ "[", {"map":"node.elements","sep":", "}, "]" ] },
  "Tuple": { "case": { "when": [ [ {"eq":["node.elements.count","1"]},
               {"tmpl":["(",{"emit":"node.elements.0"},",)"]} ] ],
             "else": {"tmpl":["(",{"map":"node.elements","sep":", "},")"]} } },
  "New": { "tmpl": [ {"get":"node.typeName"}, "(", {"map":"node.args","sep":", "}, ")" ] },
  "MakeCase": { "tmpl": [ "{\"tag\": ", {"fn":"escapeString","args":[{"get":"node.caseName"}]},
                          {"map":"node.fields","sep":"",
                           "item":{"tmpl":[", ",{"fn":"escapeString","args":[{"get":"item.name"}]},": ",
                                           {"emit":"item.value"}]}}, "}" ] },
  "Unary": { "case": { "when": [
               [ {"and":[{"eq":["node.op","-"]},{"eq":["node.typeIsInt","true"]}]},
                 {"fn":"wrapInt","args":[{"get":"node.type"},
                   {"tmpl":["-",{"emitChild":"node.operand","side":"unary"}]}]} ],
               [ {"eq":["node.op","!"]}, {"tmpl":["not ",{"emitChild":"node.operand","side":"unary"}]} ] ],
             "else": {"tmpl":[{"get":"node.op"},{"emitChild":"node.operand","side":"unary"}]} } },
  "Cast": { "fn": "convert", "args": [ {"emit":"node.operand"} ] },
  "With": { "case": { "when": [ [ {"eq":["node.baseIsSimple","true"]},
              {"tmpl":[{"get":"node.type"},"(",{"map":"node.ctorArgs","sep":", "},")"]} ] ],
            "else": {"tmpl":["(lambda ",{"get":"node.tempName"},": ",{"get":"node.type"},
                             "(",{"map":"node.ctorArgs","sep":", "},"))(",{"emit":"node.base"},")"]} } },
  "Interp": { "interleave": { "lits":"node.chunks", "holes":"node.holes",
              "lit": {"fn":"escapeString","args":[{"get":"item"}]},
              "hole": {"tmpl":[" + str(",{"emit":"item"},") + "]} } },
  "MethodCall": { "case": { "when": [ [ {"eq":["node.isExtension","true"]},
      {"tmpl":[{"get":"node.method"},"(",{"emit":"node.object"},
        {"case":{"when":[[{"eq":["node.args.count","0"]},""]],
                 "else":{"tmpl":[", ",{"map":"node.args","sep":", "}]}}},")"]} ] ],
    "else": {"tmpl":[
      { "case": { "when": [ [ {"has":"node.staticType"}, {"get":"node.staticType"} ] ],
                  "else": {"emitChild":"node.object","side":"recv"} } },
      ".",{"get":"node.method"},"(",{"map":"node.args","sep":", "},")"]} } },
  "Match": { "tmpl": [ "(lambda _m: ",
    {"fold":{"list":"node.arms",
      "seed": {"call":"pyArmValue"},
      "each": {"tmpl":[ {"call":"pyArmValue"}, " if ", {"call":"pyArmCond"}, " else ", {"get":"acc"} ]}}},
    ")(", {"emit":"node.scrutinee"}, ")" ] },
  "pyArmValue": { "case": { "when": [
      [ {"eq":["item.pattern.kind","binding"]},
        {"tmpl":["(lambda ",{"get":"item.pattern.binding"},": ",{"emit":"item.body"},")(_m)"]} ],
      [ {"and":[{"eq":["item.pattern.kind","ctor"]},{"not":{"eq":["item.pattern.binders.count","0"]}}]},
        {"tmpl":["(lambda ",
          {"map":"item.pattern.binders","sep":", ","item":{"get":"item.binding"}},": ",{"emit":"item.body"},")(",
          {"map":"item.pattern.binders","sep":", ","item":{"tmpl":["_m[\"",{"get":"item.field"},"\"]"]}},")"]} ] ],
    "else": {"emit":"item.body"} } },
  "pyArmGuard": { "case": { "when": [
      [ {"eq":["item.pattern.kind","binding"]},
        {"tmpl":["(lambda ",{"get":"item.pattern.binding"},": ",{"emit":"item.guard"},")(_m)"]} ],
      [ {"and":[{"eq":["item.pattern.kind","ctor"]},{"not":{"eq":["item.pattern.binders.count","0"]}}]},
        {"tmpl":["(lambda ",
          {"map":"item.pattern.binders","sep":", ","item":{"get":"item.binding"}},": ",{"emit":"item.guard"},")(",
          {"map":"item.pattern.binders","sep":", ","item":{"tmpl":["_m[\"",{"get":"item.field"},"\"]"]}},")"]} ] ],
    "else": {"emit":"item.guard"} } },
  "pyArmBase": { "case": { "when": [
      [ {"eq":["item.pattern.kind","literal"]},  {"tmpl":["_m == ",{"emit":"item.pattern.literal"}]} ],
      [ {"eq":["item.pattern.kind","enumCase"]}, {"tmpl":["_m == ",{"get":"item.pattern.enumType"},".",{"get":"item.pattern.enumCase"}]} ],
      [ {"eq":["item.pattern.kind","ctor"]},
        {"tmpl":["_m[\"tag\"] == ",{"fn":"escapeString","args":[{"get":"item.pattern.ctorCase"}]}]} ] ],
    "else": "True" } },
  "pyArmCond": { "case": { "when": [
      [ {"eq":["item.hasGuard","false"]}, {"call":"pyArmBase"} ],
      [ {"or":[{"eq":["item.pattern.kind","wildcard"]},{"eq":["item.pattern.kind","binding"]}]},
        {"call":"pyArmGuard"} ] ],
    "else": {"tmpl":[{"call":"pyArmBase"}," and ",{"call":"pyArmGuard"}]} } },
  "Binary": { "case": { "when": [
      [ {"eq":["node.op","??"]}, {"fn":"nullCoalesce"} ],
      [ {"and":[{"eq":["node.typeIsInt","true"]},
                {"or":[{"eq":["node.op","+"]},{"eq":["node.op","-"]},{"eq":["node.op","*"]},
                       {"eq":["node.op","/"]},{"eq":["node.op","%"]}]}]},
        {"fn":"wrapInt","args":[{"get":"node.type"},
          { "case": { "when": [
              [ {"eq":["node.op","/"]}, {"fn":"idiv","args":[{"emit":"node.lhs"},{"emit":"node.rhs"}]} ],
              [ {"eq":["node.op","%"]}, {"fn":"irem","args":[{"emit":"node.lhs"},{"emit":"node.rhs"}]} ] ],
            "else": {"tmpl":[{"emitChild":"node.lhs","side":"l"}," ",{"get":"node.op"}," ",
                             {"emitChild":"node.rhs","side":"r"}]} } } ]} ] ],
    "else": {"tmpl":[{"emitChild":"node.lhs","side":"l"}," ",{"fn":"opSpelling","args":[{"get":"node.op"}]}," ",
                     {"emitChild":"node.rhs","side":"r"}]} } }
})JSON";

const std::unordered_map<std::string, engine::Rule>& pyExprRules() {
    static const std::unordered_map<std::string, engine::Rule> rules = [] {
        std::unordered_map<std::string, engine::Rule> m;
        json::Value doc = json::parse(PY_EXPR_RULES_JSON);
        for (const auto& kv : doc.members) {
            bool ok = true;
            std::string err;
            engine::Rule r = engine::parseRule(kv.second, ok, err);
            assert(ok && "embedded Python expr rule must parse");
            m.emplace(kv.first, std::move(r));
        }
        return m;
    }();
    return rules;
}

// The ExprKind name keying the Python rule table ("" routes to the C++ switch).
const char* pyExprRuleKey(ir::ExprKind k) {
    switch (k) {
        case ir::ExprKind::Int:      return "Int";
        case ir::ExprKind::Float:    return "Float";
        case ir::ExprKind::Bool:     return "Bool";
        case ir::ExprKind::Null:     return "Null";
        case ir::ExprKind::Str:      return "Str";
        case ir::ExprKind::Var:      return "Var";
        case ir::ExprKind::This:     return "This";
        case ir::ExprKind::Extern:   return "Extern";
        case ir::ExprKind::Await:    return "Await";
        case ir::ExprKind::Call:     return "Call";
        case ir::ExprKind::Member:   return "Member";
        case ir::ExprKind::Index:    return "Index";
        case ir::ExprKind::Cond:     return "Cond";
        case ir::ExprKind::ListLit:  return "ListLit";
        case ir::ExprKind::Tuple:    return "Tuple";
        case ir::ExprKind::New:      return "New";
        case ir::ExprKind::MakeCase: return "MakeCase";
        case ir::ExprKind::Unary:    return "Unary";
        case ir::ExprKind::Cast:     return "Cast";
        case ir::ExprKind::With:       return "With";
        case ir::ExprKind::Interp:     return "Interp";
        case ir::ExprKind::MethodCall: return "MethodCall";
        case ir::ExprKind::Match:      return "Match";
        case ir::ExprKind::Binary:     return "Binary";
        default:                       return "";
    }
}

// The Python rule-interpreter seam. `tmp`/`needsIdiv` are the EMITTER's counters, reached by reference:
// the walrus builtins (`?.`/`??` single-evaluation temporaries) mint fresh names, and `idiv`/`irem` flag
// that the `_pg_idiv` prelude must be prepended. The stateful machinery stays fixed C++; the rules only
// select it.
class PyExprCtx : public IrExprCtx {
public:
    PyExprCtx(const ir::Expr& e, const BackendSpec& spec, EmitFn emit, int& tmp, bool& needsIdiv)
        : IrExprCtx(e, spec, std::move(emit)), tmp_(tmp), needsIdiv_(needsIdiv) {}

protected:
    bool wrapAtom(const ir::Expr& c, const std::string& side) const override {
        if (side == "recv") // Python atom(): wrap anything that would mis-bind against `.`/call/subscription
            return c.kind == ir::ExprKind::Binary || c.kind == ir::ExprKind::Unary ||
                   c.kind == ir::ExprKind::Cond || c.kind == ir::ExprKind::Cast;
        // "unary": wrap only a binary operand
        return c.kind == ir::ExprKind::Binary;
    }

    std::string targetGet(const std::string& path) const override {
        if (path == "node.typeIsInt") return isIntType(e_.type) ? "true" : "false";
        return "";
    }

    std::string targetBuiltin(const std::string& name, const std::vector<std::string>& args) const override {
        if (name == "ident")      return pyId(args.empty() ? std::string() : args[0]);
        if (name == "mangleName") return pyName(args.empty() ? std::string() : args[0]);
        if (name == "wrapInt")
            return wrapInt(args.size() > 0 ? args[0] : std::string(), args.size() > 1 ? args[1] : std::string());
        if (name == "idiv" || name == "irem") { // truncating int / and % need the _pg_idiv prelude
            needsIdiv_ = true;
            return std::string(name == "idiv" ? "_pg_idiv(" : "_pg_irem(") +
                   (args.size() > 0 ? args[0] : std::string()) + ", " +
                   (args.size() > 1 ? args[1] : std::string()) + ")";
        }
        if (name == "nullCoalesce" && e_.kind == ir::ExprKind::Binary) {
            // `a ?? d` -> `(t if (t := a) is not None else d)` (single eval of a)
            const auto& b = static_cast<const ir::Binary&>(e_);
            std::string t = "__c" + std::to_string(tmp_++);
            return "(" + t + " if (" + t + " := " + emit_(*b.lhs) + ") is not None else " + emit_(*b.rhs) + ")";
        }
        if (name == "nullSafeMember" && e_.kind == ir::ExprKind::Member) {
            // `obj?.field` -> `(t.field if (t := obj) is not None else None)` (single eval)
            const auto& m = static_cast<const ir::Member&>(e_);
            std::string t = "__o" + std::to_string(tmp_++);
            return "(" + t + "." + m.field + " if (" + t + " := " + emit_(*m.object) + ") is not None else None)";
        }
        if (name == "convert") { // numeric casts: int->float / float->int truncation; int<->int passes through
            if (e_.kind != ir::ExprKind::Cast) return "";
            const auto& c = static_cast<const ir::Cast&>(e_);
            const std::string& x = args.empty() ? std::string() : args[0];
            if (isFloatType(e_.type)) return isIntType(c.operand->type) ? "float(" + x + ")" : x;
            if (isIntType(e_.type) && isFloatType(c.operand->type)) return "int(" + x + ")"; // truncates toward zero (matches C#)
            return x; // int<->int: Python ints are arbitrary-precision (overflow masking is a later slice)
        }
        return "";
    }

private:
    int& tmp_;
    bool& needsIdiv_;
};

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
            if (fn.isEntry) { // an `async fn main` must be driven by the event loop; else a plain call
                line(fn.isAsync ? "asyncio.run(" + pyName(fn.mangledName) + "())" : pyName(fn.mangledName) + "()");
                if (fn.isAsync) needsAsyncio_ = true;
                break;
            }
        if (needsAsyncio_) out_ = "import asyncio\n" + out_; // `asyncio.run(main())` needs the module
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

    // A parameter: escaped name + optional `= default` (Python supports default args natively).
    std::string param(const ir::Param& p) {
        return pyId(p.name) + (p.defaultValue ? " = " + emitExpr(*p.defaultValue) : "");
    }

    void emitFunction(const ir::Function& fn) {
        // Extensions lower to plain free functions (`self` is the receiver param) with no mangledName.
        std::string sig = std::string(fn.isAsync ? "async def " : "def ") +
                          pyName(fn.mangledName.empty() ? fn.name : fn.mangledName) + "(";
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
        std::string sig = std::string(m.isAsync ? "async def " : "def ") + name + "(";
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

    bool needsIdiv_ = false; // set when a fixed-width int `/`/`%` is emitted -> prepend the trunc helpers
    bool needsAsyncio_ = false; // set when an `async fn main` entry is emitted -> prepend `import asyncio`

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
        // A migrated node kind (see pyExprRuleKey / PY_EXPR_RULES_JSON) is interpreted from its JSON Rule
        // here; the C++ switch below handles only the kinds whose shape is still imperative.
        if (const char* key = pyExprRuleKey(e.kind); key[0] != '\0') {
            const auto& rules = pyExprRules();
            auto it = rules.find(key);
            if (it != rules.end()) {
                PyExprCtx ctx(e, spec(), [this](const ir::Expr& c) { return emitExpr(c); }, tmp_, needsIdiv_);
                return engine::evalRule(it->second, ctx, &rules);
            }
        }
        switch (e.kind) {
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
