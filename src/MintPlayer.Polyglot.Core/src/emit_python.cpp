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
  "wrapInt": { "u8": "(($x) & 0xff)", "u16": "(($x) & 0xffff)", "u32": "(($x) & 0xffffffff)",
               "u64": "(($x) & 0xffffffffffffffff)",
               "i8": "(((($x) & 0xff) ^ 0x80) - 0x80)", "i16": "(((($x) & 0xffff) ^ 0x8000) - 0x8000)",
               "i32": "(((($x) & 0xffffffff) ^ 0x80000000) - 0x80000000)",
               "i64": "(((($x) & 0xffffffffffffffff) ^ 0x8000000000000000) - 0x8000000000000000)" },
  "wrapAtom": { "recv": ["binary", "unary", "cond", "cast"], "unary": ["binary"] },
  "rethrow": "raise",
  "preludes": { "asyncEntry": "import asyncio\n",
                "idiv": "def _pg_idiv(a, b):\n    q = a // b\n    return q + 1 if (q < 0 and q * b != a) else q\ndef _pg_irem(a, b):\n    return a - _pg_idiv(a, b) * b\n" },
  "tables": { "localDecl": { "mutable": "$x", "const": "$x" },
              "yield": { "value": "yield $x", "empty": "return" } },
  "blockStyle": "colonIndent",
  "stmtEnd": "",
  "throwKeyword": "raise",
  "trueLit": "True", "falseLit": "False", "nullLit": "None",
  "identifiers": {
    "keywords": ["False","None","True","and","as","assert","async","await","break","class","continue","def",
      "del","elif","else","except","finally","for","from","global","if","import","in","is","lambda","nonlocal",
      "not","or","pass","raise","return","try","while","with","yield","match","case"],
    "escape": { "strategy": "suffix", "with": "_" },
    "mangle": { "replace": "$", "with": "_" }
  }
})JSON";

const BackendSpec& pythonSpec() {
    static const BackendSpec spec = [] {
        SpecLoadResult r = loadBackendSpec(PY_SPEC_JSON);
        assert(r.ok && "embedded Python backend spec must parse");
        return r.spec;
    }();
    return spec;
}

// (Keyword suffixing `name_` and the overload-mangle `$`->`_` are the spec's `identifiers` block now; the
// generic specIdent/specMangle catalog entries serve every read — def sites, call sites, decl hooks.)

// (§3.C width wrapping — unsigned masks; signed mask + the (m ^ SIGN) - SIGN sign-extension trick — is the
// spec's `wrapInt` template table now, applied by the generic `wrap` catalog entry.)

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
  "Member": { "case": { "when": [ [ {"eq":["node.nullSafe","true"]},
      {"fresh":{"prefix":"__o","as":"t","in":
        {"tmpl":["(",{"get":"t"},".",{"get":"node.field"}," if (",{"get":"t"}," := ",{"emit":"node.object"},
                 ") is not None else None)"]}}} ] ],
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
  "Lambda": { "tmpl": [ "lambda ",
      {"map":"node.params","sep":", ","item":{"fn":"ident","args":[{"get":"item.name"}]}},
      ": ", {"emit":"node.body"} ] },
  "Type": { "case": { "when": [ [ {"has":"type.externTemplate"}, {"fn":"substExtern"} ] ],
            "else": {"get":"type.name"} } },
  "EnumDecl": { "block": { "head": { "tmpl": [ "class ", {"get":"decl.name"} ] }, "body": [
      { "case": { "when": [ [ {"eq":["decl.cases.count","0"]}, {"line":"pass"} ] ] } },
      { "mapDecl": "decl.cases",
        "each": { "line": { "tmpl": [ {"get":"item.name"}, " = ", {"get":"item.value"} ] } } } ] } },
  "UnionDecl": { "line": { "tmpl": [ "# union ", {"get":"decl.name"},
      " -> tagged dicts: {\"tag\": <case>, <field>: <value>, ...}" ] } },
  "RecordDecl": {"block":{"head":{"tmpl":["class ",{"get":"decl.name"}]},"body":[
      {"block":{"head":{"tmpl":["def __init__(self",
          {"map":"decl.fields","sep":"","item":{"tmpl":[", ",{"fn":"ident","args":[{"get":"item.name"}]}]}},")"]},
        "body":[
          {"case":{"when":[[{"eq":["decl.fields.count","0"]},{"line":"pass"}]]}},
          {"mapDecl":"decl.fields","each":{"line":{"tmpl":["self.",{"get":"item.name"}," = ",
              {"fn":"ident","args":[{"get":"item.name"}]}]}}}]}},
      {"block":{"head":"def __eq__(self, other)","body":[
          {"case":{"when":[[{"eq":["decl.fields.count","0"]},{"line":"return True"}]],
            "else":{"line":{"tmpl":["return ",
              {"map":"decl.fields","sep":" and ","item":
                {"tmpl":["self.",{"get":"item.name"}," == other.",{"get":"item.name"}]}}]}}}}]}},
      {"mapMembers":"decl.methods","rule":"MethodDecl"}]}},
  "pyStmtBody": {"case":{"when":[[{"eq":["stmt.body.count","0"]},{"line":"pass"}]],"else":{"stmts":"stmt.body"}}},
  "pyItemBody": {"case":{"when":[[{"eq":["item.body.count","0"]},{"line":"pass"}]],"else":{"stmts":"item.body"}}},
  "TryStmt": {"seq":[
      {"block":{"head":"try","body":[{"call":"pyStmtBody"}]}},
      {"mapDecl":"stmt.catches","each":
        {"block":{"head":{"tmpl":["except ",
            {"case":{"when":[[{"eq":["item.hasType","true"]},{"type":"item.type"}]],"else":"Exception"}},
            {"case":{"when":[[{"eq":["item.hasBinding","true"]},{"tmpl":[" as ",{"get":"item.binding"}]}]]}}]},
          "body":[
            {"case":{"when":[[{"eq":["item.hasGuard","true"]},
              {"line":{"tmpl":["if not (",{"emit":"item.guard"},"): raise"]}}]]}},
            {"call":"pyItemBody"}]}}},
      {"case":{"when":[[{"eq":["stmt.hasFinally","true"]},
        {"block":{"head":"finally","body":[
          {"case":{"when":[[{"eq":["stmt.finallyBody.count","0"]},{"line":"pass"}]],
            "else":{"stmts":"stmt.finallyBody"}}}]}}]]}}]},
  "ForStmt": {"case":{"when":[
      [{"eq":["stmt.isRange","true"]},
        {"block":{"head":{"tmpl":["for ",{"fn":"ident","args":[{"get":"stmt.binding"}]}," in range(",
            {"emit":"stmt.rangeStart"},", ",
            {"case":{"when":[[{"eq":["stmt.inclusive","true"]},{"tmpl":["(",{"emit":"stmt.rangeEnd"},") + 1"]}]],
              "else":{"emit":"stmt.rangeEnd"}}},")"]},
          "body":[{"call":"pyStmtBody"}]}}],
      [{"not":{"eq":["stmt.tupleBindings.count","0"]}},
        {"block":{"head":{"tmpl":["for ",
            {"map":"stmt.tupleBindings","sep":", ","item":{"fn":"ident","args":[{"get":"item"}]}}," in ",
            {"emit":"stmt.iterable"}]},
          "body":[{"call":"pyStmtBody"}]}}]],
      "else":{"block":{"head":{"tmpl":["for ",{"fn":"ident","args":[{"get":"stmt.binding"}]}," in ",
            {"emit":"stmt.iterable"}]},
          "body":[{"call":"pyStmtBody"}]}}}},
  "Program": {"seq":[
      {"mapMembers":"module.enums","rule":"EnumDecl"},
      {"mapMembers":"module.unions","rule":"UnionDecl"},
      {"mapMembers":"module.records","rule":"RecordDecl"},
      {"mapMembers":"module.classes","rule":"ClassDecl"},
      {"mapDecl":"module.globals","each":{"line":{"tmpl":[{"get":"item.name"}," = ",
          {"case":{"when":[[{"eq":["item.hasInit","true"]},{"emit":"item.init"}]],"else":"None"}}]}}},
      {"mapMembers":"module.functions","rule":"FunctionDecl"},
      {"mapMembers":"module.extensions","rule":"FunctionDecl"},
      {"case":{"when":[[{"eq":["module.hasEntry","true"]},
        {"case":{"when":[[{"eq":["module.entry.isAsync","true"]},
          {"line":{"tmpl":["asyncio.run(",{"fn":"mangle","args":[{"get":"module.entry.mangledName"}]},"())"]}}]],
          "else":{"line":{"tmpl":[{"fn":"mangle","args":[{"get":"module.entry.mangledName"}]},"()"]}}}}]]}}]},
  "pyFnSig": {"tmpl":[
      {"case":{"when":[[{"eq":["decl.isAsync","true"]},"async def "]],"else":"def "}},
      {"fn":"mangle","args":[{"get":"decl.emitName"}]},"(",
      {"map":"decl.params","sep":", ","item":{"call":"pyParam"}},")"]},
  "FunctionDecl": {"case":{"when":[
      [{"eq":["decl.exprBodied","true"]},
        {"block":{"head":{"call":"pyFnSig"},"body":[
          {"case":{"when":[[{"eq":["decl.returnsUnit","true"]},{"line":{"emit":"decl.exprBody"}}]],
            "else":{"line":{"tmpl":["return ",{"emit":"decl.exprBody"}]}}}}]}}]],
      "else":{"block":{"head":{"call":"pyFnSig"},"body":[
        {"case":{"when":[[{"eq":["decl.body.count","0"]},{"line":"pass"}]],
          "else":{"stmts":"decl.body"}}}]}}}},
  "pyClassHead": {"tmpl":["class ",{"get":"decl.name"},
      {"case":{"when":[[{"eq":["decl.bases.count","0"]},""]],
        "else":{"tmpl":["(",{"map":"decl.bases","sep":", ","item":{"type":"item"}},")"]}}}]},
  "ClassDecl": {"block":{"head":{"call":"pyClassHead"},"body":[
      {"mapDecl":"decl.staticInitFields","each":{"line":{"tmpl":[{"get":"item.name"}," = ",{"emit":"item.init"}]}}},
      {"case":{"when":[[{"eq":["decl.needsCtor","true"]},
        {"block":{"head":{"tmpl":["def __init__(self",
            {"map":"decl.initParams","sep":"","item":{"tmpl":[", ",{"call":"pyParam"}]}},")"]},
          "body":[
            {"case":{"when":[[{"eq":["decl.hasSuper","true"]},
              {"line":{"tmpl":["super().__init__(",{"map":"decl.superArgs","sep":", ","item":{"emit":"item"}},")"]}}]]}},
            {"mapDecl":"decl.instanceInitFields","each":{"line":{"tmpl":["self.",{"get":"item.name"}," = ",{"emit":"item.init"}]}}},
            {"stmts":"decl.initBody"},
            {"case":{"when":[[{"and":[{"eq":["decl.hasSuper","false"]},{"eq":["decl.instanceInitFields.count","0"]},{"eq":["decl.initBody.count","0"]}]},
              {"line":"pass"}]]}}]}}]]}},
      {"mapMembers":"decl.methods","rule":"MethodDecl"},
      {"case":{"when":[[{"and":[{"eq":["decl.staticInitFields.count","0"]},{"eq":["decl.needsCtor","false"]},{"eq":["decl.methods.count","0"]}]},
        {"line":"pass"}]]}}]}},
  "pyParam": {"tmpl":[{"fn":"ident","args":[{"get":"item.name"}]},
      {"case":{"when":[[{"eq":["item.hasDefault","true"]},{"tmpl":[" = ",{"emit":"item.default"}]}]]}}]},
  "pyDunder": {"case":{"when":[
      [{"eq":["decl.opSymbol","+"]},"__add__"],
      [{"eq":["decl.opSymbol","-"]},"__sub__"],
      [{"eq":["decl.opSymbol","*"]},"__mul__"],
      [{"eq":["decl.opSymbol","/"]},"__truediv__"],
      [{"eq":["decl.opSymbol","%"]},"__mod__"],
      [{"eq":["decl.opSymbol","=="]},"__eq__"],
      [{"eq":["decl.opSymbol","<"]},"__lt__"],
      [{"eq":["decl.opSymbol","<="]},"__le__"],
      [{"eq":["decl.opSymbol",">"]},"__gt__"],
      [{"eq":["decl.opSymbol",">="]},"__ge__"]],
      "else":{"get":"decl.name"}}},
  "pyMethodSig": {"tmpl":[
      {"case":{"when":[[{"eq":["decl.isAsync","true"]},"async def "]],"else":"def "}},
      {"case":{"when":[[{"eq":["decl.kind","operator"]},{"call":"pyDunder"}]],"else":{"get":"decl.name"}}},
      "(",
      {"case":{"when":[[{"eq":["decl.isStatic","true"]},""]],
        "else":{"tmpl":["self",{"case":{"when":[[{"eq":["decl.params.count","0"]},""]],"else":", "}}]}}},
      {"map":"decl.params","sep":", ","item":{"call":"pyParam"}},")"]},
  "MethodDecl": {"seq":[
      {"case":{"when":[[{"eq":["decl.kind","property"]},{"line":"@property"}]]}},
      {"case":{"when":[[{"eq":["decl.isStatic","true"]},{"line":"@staticmethod"}]]}},
      {"case":{"when":[
        [{"eq":["decl.exprBodied","true"]},
          {"block":{"head":{"call":"pyMethodSig"},"body":[
            {"case":{"when":[[{"eq":["decl.returnsUnit","true"]},{"line":{"emit":"decl.exprBody"}}]],
              "else":{"line":{"tmpl":["return ",{"emit":"decl.exprBody"}]}}}} ]}}]],
        "else":{"block":{"head":{"call":"pyMethodSig"},"body":[
            {"case":{"when":[[{"eq":["decl.body.count","0"]},{"line":"pass"}]],
              "else":{"stmts":"decl.body"}}} ]}}}} ]},
  "MakeCase": { "tmpl": [ "{\"tag\": ", {"fn":"escapeString","args":[{"get":"node.caseName"}]},
                          {"map":"node.fields","sep":"",
                           "item":{"tmpl":[", ",{"fn":"escapeString","args":[{"get":"item.name"}]},": ",
                                           {"emit":"item.value"}]}}, "}" ] },
  "Unary": { "case": { "when": [
               [ {"and":[{"eq":["node.op","-"]},{"eq":["node.typeIsInt","true"]}]},
                 {"fn":"wrap","args":[{"get":"node.type"},
                   {"tmpl":["-",{"emitChild":"node.operand","side":"unary"}]}]} ],
               [ {"eq":["node.op","!"]}, {"tmpl":["not ",{"emitChild":"node.operand","side":"unary"}]} ] ],
             "else": {"tmpl":[{"get":"node.op"},{"emitChild":"node.operand","side":"unary"}]} } },
  "Cast": { "case": { "when": [
      [ {"and":[{"eq":["node.typeIsFloat","true"]},{"eq":["node.fromIsInt","true"]}]},
        {"tmpl":["float(",{"emit":"node.operand"},")"]} ],
      [ {"eq":["node.typeIsFloat","true"]}, {"emit":"node.operand"} ],
      [ {"and":[{"eq":["node.typeIsInt","true"]},{"eq":["node.fromIsFloat","true"]}]},
        {"tmpl":["int(",{"emit":"node.operand"},")"]} ] ],
    "else": {"emit":"node.operand"} } },
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
      [ {"eq":["node.op","??"]},
        {"fresh":{"prefix":"__c","as":"t","in":
          {"tmpl":["(",{"get":"t"}," if (",{"get":"t"}," := ",{"emit":"node.lhs"},") is not None else ",
                   {"emit":"node.rhs"},")"]}}} ],
      [ {"and":[{"eq":["node.typeIsInt","true"]},
                {"or":[{"eq":["node.op","+"]},{"eq":["node.op","-"]},{"eq":["node.op","*"]},
                       {"eq":["node.op","/"]},{"eq":["node.op","%"]}]}]},
        {"fn":"wrap","args":[{"get":"node.type"},
          { "case": { "when": [
              [ {"eq":["node.op","/"]}, {"tmpl":[{"fn":"require","args":["idiv"]},
                  "_pg_idiv(",{"emit":"node.lhs"},", ",{"emit":"node.rhs"},")"]} ],
              [ {"eq":["node.op","%"]}, {"tmpl":[{"fn":"require","args":["idiv"]},
                  "_pg_irem(",{"emit":"node.lhs"},", ",{"emit":"node.rhs"},")"]} ] ],
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

} // namespace

// The Python backend is DATA: the spec + rule JSON above, interpreted by the one shared emitter. (The
// ExternType/Bound member picks collapse into std overlays at P19 slice 9.)
std::string emitPython(const ir::Module& module) {
    InterpretedEmitter emitter(&pythonSpec, pyExprRules(), &ir::ExternType::pyType, &ir::Bound::pyTemplate);
    return emitter.emit(module);
}

} // namespace mintplayer::polyglot
