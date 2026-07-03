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
  "wrapInt": { "i8": "(sbyte)($x)", "i16": "(short)($x)", "u8": "(byte)($x)", "u16": "(ushort)($x)" },
  "wrapAtom": { "recv": ["binary", "unary", "cast"], "unary": ["binary"] },
  "rethrow": "throw;",
  "tables": { "localDecl": { "mutable": "var $x", "const": "var $x" },
              "yield": { "value": "yield return $x;", "empty": "yield break;" } },
  "generics": { "style": "whereClauses", "boundsIntro": " : ", "boundsSep": ", ", "erase": ["INumber"] },
  "delimited": { "tuple": { "open": "(", "sep": ", ", "close": ")" } },
  "blockStyle": "bracesAllman",
  "stmtEnd": ";",
  "throwKeyword": "throw",
  "trueLit": "true", "falseLit": "false", "nullLit": "null",
  "escapes": {
    "interp": { "\"": "\\\"", "\\": "\\\\", "\n": "\\n", "\t": "\\t", "\r": "\\r", "{": "{{", "}": "}}" },
    "char":   { "'": "\\'", "\\": "\\\\", "\n": "\\n", "\t": "\\t", "\r": "\\r" }
  },
  "identifiers": {
    "keywords": ["abstract","as","base","bool","break","byte","case","catch","char","checked","class","const",
      "continue","decimal","default","delegate","do","double","else","enum","event","explicit","extern","false",
      "finally","fixed","float","for","foreach","goto","if","implicit","in","int","interface","internal","is",
      "lock","long","namespace","new","null","object","operator","out","override","params","private","protected",
      "public","readonly","ref","return","sbyte","sealed","short","sizeof","stackalloc","static","string","struct",
      "switch","this","throw","true","try","typeof","uint","ulong","unchecked","unsafe","ushort","using","virtual",
      "void","volatile","while"],
    "escape": { "strategy": "prefix", "with": "@" }
  }
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
  "Binary": { "fn": "wrap", "args": [ {"get":"node.type"},
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
  "typeArgsSuffix": {"case":{"when":[[{"eq":["node.typeArgs.count","0"]},""]],
      "else":{"tmpl":["<",{"map":"node.typeArgs","sep":", ","item":{"type":"item"}},">"]}}},
  "ListLit": { "tmpl": [ "new global::System.Collections.Generic.List<", {"type":"node.elem"}, "> { ",
                         {"map":"node.elements","sep":", "}, " }" ] },
  "New": { "tmpl": [ "new ", {"get":"node.typeName"}, {"call":"typeArgsSuffix"},
                     "(", {"map":"node.args","sep":", "}, ")" ] },
  "MakeCase": { "tmpl": [ "new ", {"get":"node.caseName"}, {"call":"typeArgsSuffix"},
                          "(", {"map":"node.fields","sep":", "}, ")" ] },
  "Var":    { "fn": "ident", "args": [ {"get":"node.name"} ] },
  "Extern": { "get": "node.code" },
  "Await":  { "tmpl": [ "await ", {"emitChild":"node.operand","side":"recv"} ] },
  "With": { "tmpl": [ {"emitChild":"node.base","side":"recv"}, " with { ",
              {"map":"node.fields","sep":", ",
               "item":{"tmpl":[{"fn":"ident","args":[{"get":"item.name"}]}," = ",{"emit":"item.value"}]}},
              " }" ] },
  "Cast":   { "tmpl": [ "(", {"type":"node.type"}, ")(", {"emit":"node.operand"}, ")" ] },
  "Unary":  { "tmpl": [ {"get":"node.op"}, {"emitChild":"node.operand","side":"unary"} ] },
  "Interp": { "tmpl": [ "$\"", {"interleave":{"lits":"node.chunks","holes":"node.holes",
                "lit":{"fn":"escape","args":["interp",{"get":"item"}]},
                "hole":{"tmpl":["{",{"emit":"item"},"}"]}}}, "\"" ] },
  "Char": { "tmpl": [ "'", {"fn":"escape","args":["char",{"get":"node.value"}]}, "'" ] },
  "This": { "case": { "when": [ [ {"eq":["node.insideOperator","true"]}, "lhs" ] ], "else": "this" } },
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
              {"tmpl":[{"get":"item.pattern.ctorCase"},{"call":"typeArgsSuffix"}," _"]} ] ],
          "else": {"tmpl":[{"get":"item.pattern.ctorCase"},{"call":"typeArgsSuffix"},"(",
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
  "csRecordHead": {"tmpl":["record ",{"get":"decl.name"},{"fn":"generics"},"(",
      {"map":"decl.fields","sep":", ","item":{"tmpl":[{"type":"item.type"}," ",{"fn":"ident","args":[{"get":"item.name"}]}]}},
      ")",
      {"case":{"when":[[{"eq":["decl.bases.count","0"]},""]],
        "else":{"tmpl":[" : ",{"map":"decl.bases","sep":", ","item":{"type":"item"}}]}}},
      {"fn":"where"}]},
  "RecordDecl": {"case":{"when":[
      [{"eq":["decl.methods.count","0"]},{"line":{"tmpl":[{"call":"csRecordHead"},";"]}}]],
      "else":{"block":{"head":{"call":"csRecordHead"},
        "body":[{"mapMembers":"decl.methods","rule":"MethodDecl"}]}}}},
  "TryStmt": {"seq":[
      {"block":{"head":"try","body":[{"stmts":"stmt.body"}]}},
      {"mapDecl":"stmt.catches","each":
        {"block":{"head":{"tmpl":["catch",
            {"case":{"when":[[{"eq":["item.hasType","true"]},
              {"tmpl":[" (",{"type":"item.type"},
                {"case":{"when":[[{"eq":["item.hasBinding","true"]},{"tmpl":[" ",{"get":"item.binding"}]}]]}},
                ")"]}]]}},
            {"case":{"when":[[{"eq":["item.hasGuard","true"]},{"tmpl":[" when (",{"emit":"item.guard"},")"]}]]}}]},
          "body":[{"stmts":"item.body"}]}}},
      {"case":{"when":[[{"eq":["stmt.hasFinally","true"]},
        {"block":{"head":"finally","body":[{"stmts":"stmt.finallyBody"}]}}]]}}]},
  "ForStmt": {"case":{"when":[
      [{"eq":["stmt.isRange","true"]},
        {"block":{"head":{"tmpl":["for (var ",{"get":"stmt.binding"}," = ",{"emit":"stmt.rangeStart"},"; ",
            {"get":"stmt.binding"},
            {"case":{"when":[[{"eq":["stmt.inclusive","true"]}," <= "]],"else":" < "}},
            {"emit":"stmt.rangeEnd"},"; ",{"get":"stmt.binding"},"++)"]},
          "body":[{"stmts":"stmt.body"}]}}],
      [{"not":{"eq":["stmt.tupleBindings.count","0"]}},
        {"block":{"head":{"tmpl":["foreach (var (",
            {"map":"stmt.tupleBindings","sep":", ","item":{"get":"item"}},") in ",{"emit":"stmt.iterable"},")"]},
          "body":[{"stmts":"stmt.body"}]}}]],
      "else":{"block":{"head":{"tmpl":["foreach (var ",{"get":"stmt.binding"}," in ",{"emit":"stmt.iterable"},")"]},
        "body":[{"stmts":"stmt.body"}]}}}},
  "Program": {"seq":[
      {"mapMembers":"module.enums","rule":"EnumDecl"},
      {"mapMembers":"module.interfaces","rule":"InterfaceDecl"},
      {"mapMembers":"module.unions","rule":"UnionDecl"},
      {"mapMembers":"module.records","rule":"RecordDecl"},
      {"mapMembers":"module.classes","rule":"ClassDecl"},
      {"case":{"when":[[{"not":{"eq":["module.extensions.count","0"]}},
        {"block":{"head":"static class Extensions","body":[
          {"mapMembers":"module.extensions","rule":"ExtensionDecl"}]}}]]}},
      {"case":{"when":[[{"or":[{"not":{"eq":["module.enums.count","0"]}},
                               {"not":{"eq":["module.unions.count","0"]}},
                               {"not":{"eq":["module.records.count","0"]}},
                               {"not":{"eq":["module.classes.count","0"]}},
                               {"not":{"eq":["module.extensions.count","0"]}}]},
        {"line":""}]]}},
      {"block":{"head":"static class Program","body":[
        {"mapDecl":"module.globals","each":{"line":{"tmpl":["static readonly ",
            {"type":"item.type"}," ",{"fn":"ident","args":[{"get":"item.name"}]},
            {"case":{"when":[[{"eq":["item.hasInit","true"]},{"tmpl":[" = ",{"emit":"item.init"}]}]]}},";"]}}},
        {"mapMembers":"module.functions","rule":"FunctionDecl"},
        {"case":{"when":[[{"eq":["module.hasEntry","true"]},{"seq":[
          {"line":""},
          {"case":{"when":[[{"eq":["module.entry.isAsync","true"]},
            {"line":"static void Main() { global::System.Globalization.CultureInfo.CurrentCulture = global::System.Globalization.CultureInfo.InvariantCulture; main().GetAwaiter().GetResult(); }"}]],
            "else":{"line":"static void Main() { global::System.Globalization.CultureInfo.CurrentCulture = global::System.Globalization.CultureInfo.InvariantCulture; main(); }"}}}]}]]}}]}}]},
  "csFnSig": {"tmpl":["public static ",
      {"case":{"when":[[{"eq":["decl.isAsync","true"]},"async "]]}},
      {"case":{"when":[[{"eq":["decl.isAsync","true"]},{"call":"csAsyncRet"}]],"else":{"type":"decl.returnType"}}},
      " ",{"get":"decl.name"},{"fn":"generics"},"(",
      {"map":"decl.params","sep":", ","item":{"call":"csParam"}},")",{"fn":"where"}]},
  "FunctionDecl": {"block":{"head":{"call":"csFnSig"},"body":[{"stmts":"decl.body"}]}},
  "csExtSig": {"tmpl":["public static ",{"type":"decl.returnType"}," ",{"get":"decl.name"},{"fn":"generics"},
      "(this ",{"type":"decl.params.0.type"}," ",{"get":"decl.params.0.name"},
      {"map":"decl.paramsTail","sep":"","item":{"tmpl":[", ",{"type":"item.type"}," ",{"get":"item.name"}]}},
      ")",{"fn":"where"}]},
  "ExtensionDecl": {"case":{"when":[
      [{"eq":["decl.exprBodied","true"]},
        {"line":{"tmpl":[{"call":"csExtSig"}," => ",{"emit":"decl.exprBody"},";"]}}]],
      "else":{"block":{"head":{"call":"csExtSig"},"body":[{"stmts":"decl.body"}]}}}},
  "csClassHead": {"tmpl":["class ",{"get":"decl.name"},{"fn":"generics"},
      {"case":{"when":[[{"eq":["decl.bases.count","0"]},""]],
        "else":{"tmpl":[" : ",{"map":"decl.bases","sep":", ","item":{"type":"item"}}]}}},
      {"fn":"where"}]},
  "ClassDecl": {"block":{"head":{"call":"csClassHead"},"body":[
      {"mapDecl":"decl.fields","each":{"line":{"tmpl":["public ",
          {"case":{"when":[
            [{"and":[{"eq":["item.isStatic","true"]},{"eq":["item.isMutable","true"]}]},"static "],
            [{"eq":["item.isStatic","true"]},"static readonly "],
            [{"eq":["item.isMutable","true"]},""]],
            "else":"readonly "}},
          {"type":"item.type"}," ",{"fn":"ident","args":[{"get":"item.name"}]},
          {"case":{"when":[[{"eq":["item.hasInit","true"]},{"tmpl":[" = ",{"emit":"item.init"}]}]]}},
          ";"]}}},
      {"case":{"when":[[{"eq":["decl.hasInit","true"]},
        {"block":{"head":{"tmpl":["public ",{"get":"decl.name"},"(",
            {"map":"decl.initParams","sep":", ","item":{"call":"csParam"}},")",
            {"case":{"when":[[{"eq":["decl.hasSuper","true"]},
              {"tmpl":[" : base(",{"map":"decl.superArgs","sep":", ","item":{"emit":"item"}},")"]}]]}}]},
          "body":[{"stmts":"decl.initBody"}]}}]]}},
      {"mapMembers":"decl.methods","rule":"MethodDecl"}]}},
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

std::string csType(const TypeRef& t);      // defined below; the `elemType` builtin renders a list element type

// The C# rule-interpreter seam: only what differs from the shared IrExprCtx — the C# builtins (keyword
// escaping, C# type rendering, sub-word wrap-back, interpolation/char escaping) and the C# atom-wrapping
// policy. (The operator-body `this`->`lhs` rebind is the lowering-precomputed `This.insideOperator` fact
// now — the This rule branches on it; no emitter state.)
class CsExprCtx : public IrExprCtx {
public:
    using IrExprCtx::IrExprCtx;

protected:
    std::string renderTypeRef(const TypeRef& t) const override { return csType(t); }
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

// (Keyword escaping — `@`-verbatim identifiers — is the spec's `identifiers` block now; the generic
// specIdent catalog entry serves every read. TS escapes nothing: its spec declares no keywords.)

// The C# declaration hooks — the one per-backend object every decl context reads through. (Generics/where
// spelling — bare names + trailing `where T : A, B` clauses, INumber erased — is the spec's `generics`
// strategy now, spelled by the DeclHooks base.)
class CsDeclHooks : public DeclHooks {
public:
    CsDeclHooks() : DeclHooks(&csharpSpec) {}
    std::string renderTypeRef(const TypeRef& t) const override { return csType(t); }
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
        // The whole module shape (decl order, the Extensions/Program wrappers, globals, Main synthesis with
        // its invariant-culture pin + async blocking) is the "Program" scaffold rule — data, not code.
        ModuleDeclCtx ctx(m, kCsDeclHooks, [this](const ir::Expr& e) { return emitExpr(e); }, "csharp");
        runDeclRule(csharpExprRules().at("Program"), ctx, ctx, &csharpExprRules());
        return out_;
    }

private:
    std::unordered_map<std::string, const ir::ExternType*> externMap_; // backs g_externTypes for this emit

    const BackendSpec& spec() const override { return csharpSpec(); }
    std::string renderType(const TypeRef& t) override { return csType(t); }
    const engine::RuleTable* ruleTable() const override { return &csharpExprRules(); }
    const DeclHooks* declHooks() const override { return &kCsDeclHooks; }

    std::string emitExpr(const ir::Expr& e) override {
        // A migrated node kind (see csExprRuleKey / CSHARP_EXPR_RULES_JSON) is interpreted from its JSON Rule
        // here; the C++ switch below handles only the kinds whose shape is still imperative (P18 slices 4-10).
        if (const char* key = csExprRuleKey(e.kind); key[0] != '\0') {
            const auto& rules = csharpExprRules();
            auto it = rules.find(key);
            if (it != rules.end()) {
                CsExprCtx ctx(e, spec(), [this](const ir::Expr& c) { return emitExpr(c); },
                              [this](const std::vector<ir::StmtPtr>& b) { return inlineBlock(b); });
                return engine::evalRule(it->second, ctx, &rules);
            }
        }
        switch (e.kind) {
            case ir::ExprKind::Bound:
                return substBoundTemplate(static_cast<const ir::Bound&>(e).csTemplate, static_cast<const ir::Bound&>(e));
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
