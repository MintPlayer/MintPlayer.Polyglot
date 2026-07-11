#!/usr/bin/env python3
# Reproducible scaffold for the Kotlin target plugin (P26 slice 2), built from the C# plugin as the base
# (JVM shares real overloading / exceptions / ADTs / statics with .NET). Re-runnable: `python
# scripts/scaffold-kotlin.py`. Adapts the spec + the rules that diverge for Kotlin; other rules ride the C#
# base until a conformance program forces them. This is the source of truth for the Kotlin plugin's
# structure — hand-edits should be folded back here so it stays reproducible (survives a `git clean`).
import json, os

d = json.load(open('plugins/csharp/polyglot-plugin.json'))
d['name'] = 'kotlin'
d['fileExtension'] = '.kt'
d['capabilities'] = {"disposal": "emulated", "async": "emulated"}

s = d['spec']
s['name'] = 'kotlin'
s['scalarType'] = {"unit": "Unit", "i8": "Byte", "i16": "Short", "i32": "Int", "i64": "Long",
                   "u8": "UByte", "u16": "UShort", "u32": "UInt", "u64": "ULong",
                   "f32": "Float", "f64": "Double", "bool": "Boolean", "string": "String"}
s['intSuffix'] = {"i64": "L", "u64": "UL", "u32": "U", "u16": "U", "u8": "U"}
s['wrapInt'] = {"i8": "($x).toByte()", "i16": "($x).toShort()", "u8": "($x).toUByte()", "u16": "($x).toUShort()"}
s['rethrow'] = "throw __e"
s['tables']['localDecl'] = {"mutable": "var $x", "const": "val $x"}
s['tables']['localDeclTyped'] = {"mutable": "var $x: $T", "const": "val $x: $T"}
s['tables']['yield'] = {"value": "yield($x)", "empty": "return@sequence"}
s['blockStyle'] = 'bracesKnR'
s['stmtEnd'] = ''

def ident(p): return {"fn": "ident", "args": [{"get": p}]}

r = d['rules']
r['csParam'] = {"tmpl": [ident("item.name"), ": ", {"type": "item.type"},
    {"case": {"when": [[{"eq": ["item.hasDefault", "true"]}, {"tmpl": [" = ", {"emit": "item.default"}]}]]}}]}
r['csFnSig'] = {"tmpl": [
    {"case": {"when": [[{"eq": ["decl.isAsync", "true"]}, "suspend "]]}},
    "fun ", {"fn": "generics"}, " ", ident("decl.name"), "(",
    {"map": "decl.params", "sep": ", ", "item": {"call": "csParam"}}, ")",
    {"case": {"when": [[{"eq": ["decl.returnsUnit", "true"]}, ""]], "else": {"tmpl": [": ", {"type": "decl.returnType"}]}}}]}
r['FunctionDecl'] = {"block": {"head": {"call": "csFnSig"}, "body": [{"stmts": "decl.body"}]}}
r['Call'] = {"tmpl": [ident("node.callee"), "(", {"map": "node.args", "sep": ", "}, ")"]}
# Kotlin has no C-style casts; every scalar conversion is a `to<TypeName>()` method (Int->toInt, Double->
# toDouble, String->toString). The Type rule renders the target name, so `.to`+<name>+`()` is uniform.
r['Cast'] = {"tmpl": ["(", {"emit": "node.operand"}, ").to", {"type": "node.type"}, "()"]}
# Construction: Kotlin has no `new` (Type(args)); union-case construction is the case constructor.
r['New'] = {"tmpl": [ident("node.typeName"), {"call": "typeArgsSuffix"}, "(", {"map": "node.args", "sep": ", "}, ")"]}
r['MakeCase'] = {"tmpl": [{"get": "node.caseName"}, {"call": "typeArgsSuffix"}, "(", {"map": "node.fields", "sep": ", "}, ")"]}
# List literal -> mutableListOf(...).
r['ListLit'] = {"tmpl": ["mutableListOf(", {"map": "node.elements", "sep": ", "}, ")"]}
# Type: Kotlin function types `(A,B)->R`; tuples -> Pair (2)/Triple (best-effort); `T?`; scalars; generics.
r['Type'] = {"case": {"when": [
    [{"eq": ["type.kind", "function"]},
     {"tmpl": ["(", {"map": "type.args", "sep": ", ", "item": {"type": "item"}}, ") -> ",
               {"case": {"when": [[{"eq": ["type.returnsUnit", "true"]}, "Unit"]], "else": {"type": "type.ret"}}}]}],
    [{"eq": ["type.kind", "tuple"]},
     {"tmpl": ["Pair<", {"map": "type.args", "sep": ", ", "item": {"type": "item"}}, ">"]}],
    [{"eq": ["type.nullable", "true"]}, {"tmpl": [{"type": "type.base"}, "?"]}],
    [{"has": "type.scalar"}, {"get": "type.scalar"}],
    [{"eq": ["type.nameEmpty", "true"]}, "Any"],
    [{"has": "type.externTemplate"}, {"fn": "substExtern"}]],
  "else": {"tmpl": [ident("type.name"),
    {"case": {"when": [[{"eq": ["type.args.count", "0"]}, ""]],
      "else": {"tmpl": ["<", {"map": "type.args", "sep": ", ", "item": {"type": "item"}}, ">"]}}}]}}}
r['Program'] = {"seq": [
    {"mapMembers": "module.enums", "rule": "EnumDecl"},
    {"mapMembers": "module.interfaces", "rule": "InterfaceDecl"},
    {"mapMembers": "module.unions", "rule": "UnionDecl"},
    {"mapMembers": "module.records", "rule": "RecordDecl"},
    {"mapMembers": "module.classes", "rule": "ClassDecl"},
    {"mapMembers": "module.extensions", "rule": "ExtensionDecl"},
    {"mapDecl": "module.globals", "each": {"line": {"tmpl": [
        "val ", ident("item.name"), ": ", {"type": "item.type"},
        {"case": {"when": [[{"eq": ["item.hasInit", "true"]}, {"tmpl": [" = ", {"emit": "item.init"}]}]], "else": " = TODO()"}}]}}},
    {"mapMembers": "module.functions", "rule": "FunctionDecl"}]}
r['csRecordHead'] = {"tmpl": ["data class ", ident("decl.name"), {"fn": "generics"}, "(",
    {"map": "decl.fields", "sep": ", ", "item": {"tmpl": ["val ", ident("item.name"), ": ", {"type": "item.type"}]}}, ")",
    {"case": {"when": [[{"eq": ["decl.bases.count", "0"]}, ""]],
      "else": {"tmpl": [" : ", {"map": "decl.bases", "sep": ", ", "item": {"type": "item"}}]}}}]}
r['RecordDecl'] = {"case": {"when": [[{"eq": ["decl.methods.count", "0"]}, {"line": {"call": "csRecordHead"}}]],
    "else": {"block": {"head": {"call": "csRecordHead"}, "body": [{"mapMembers": "decl.methods", "rule": "MethodDecl"}]}}}}
# Lambda -> Kotlin `{ p1, p2 -> body }` (no-arg: `{ body }`); typed param `{ x: Int -> ... }`.
r['Lambda'] = {"tmpl": ["{ ",
    {"case": {"when": [[{"eq": ["node.params.count", "0"]}, ""]], "else": {"tmpl": [
        {"map": "node.params", "sep": ", ", "item": {"case": {"when": [[{"eq": ["item.hasType", "true"]},
            {"tmpl": [ident("item.name"), ": ", {"type": "item.type"}]}]], "else": ident("item.name")}}}, " -> "]}}},
    {"case": {"when": [[{"eq": ["node.exprBodied", "true"]}, {"emit": "node.body"}]], "else": {"fn": "inlineBlock"}}},
    " }"]}
r['UnionDecl'] = {"seq": [
    {"line": {"tmpl": ["sealed class ", ident("decl.name"), {"fn": "generics"}]}},
    {"mapDecl": "decl.cases", "each": {"line": {"tmpl": [
        {"case": {"when": [[{"eq": ["item.fields.count", "0"]}, {"tmpl": ["class ", ident("item.name"), {"fn": "generics"}]}]],
          "else": {"tmpl": ["data class ", ident("item.name"), {"fn": "generics"}, "(",
            {"map": "item.fields", "sep": ", ", "item": {"tmpl": ["val ", ident("item.name"), ": ", {"type": "item.type"}]}}, ")"]}}},
        " : ", ident("decl.name"), {"fn": "generics"}, "()"]}}}]}

d['std'] = {
  "std.collections": {"List.type": "MutableList<$0>", "List.init": "mutableListOf()", "List.count": "$this.size",
    "List.add": "$this.add($0)", "List.clear": "$this.clear()",
    "List.removeAll": "$this.removeAll { __e -> ($0)(__e) }", "List.removeAt": "$this.removeAt($0)"},
  "std.io": {"print": "println(x)"},
  "std.math": {"Math.PI": "kotlin.math.PI", "Math.E": "kotlin.math.E", "Math.sqrt": "kotlin.math.sqrt($0)",
    "Math.ln": "kotlin.math.ln($0)", "Math.log": "kotlin.math.ln($0)", "Math.log2": "kotlin.math.log2($0)",
    "Math.log10": "kotlin.math.log10($0)", "Math.exp": "kotlin.math.exp($0)", "Math.pow": "$0.pow($1)",
    "Math.sin": "kotlin.math.sin($0)", "Math.cos": "kotlin.math.cos($0)", "Math.tan": "kotlin.math.tan($0)",
    "Math.asin": "kotlin.math.asin($0)", "Math.acos": "kotlin.math.acos($0)", "Math.atan": "kotlin.math.atan($0)",
    "Math.atan2": "kotlin.math.atan2($0, $1)", "Math.sinh": "kotlin.math.sinh($0)", "Math.cosh": "kotlin.math.cosh($0)",
    "Math.tanh": "kotlin.math.tanh($0)", "Math.trunc": "kotlin.math.truncate($0)", "Math.floor": "kotlin.math.floor($0)",
    "Math.ceil": "kotlin.math.ceil($0)", "Math.min": "kotlin.math.min($0, $1)", "Math.max": "kotlin.math.max($0, $1)",
    "Math.abs": "kotlin.math.abs($0)", "Math.round": "kotlin.math.round($0)"},
  "std.strings": {"string.isEmpty": "$this.isEmpty()", "string.len": "$this.length", "string.toUpper": "$this.uppercase()",
    "string.toLower": "$this.lowercase()", "string.charAt": "$this[$0].toString()", "string.codePoints": "$this.toList()",
    "string.toI32": "$this.toInt()"},
  "std.core": {"Error.type": "Exception", "Error.init": "$T($0)", "Error.message": "($this.message ?: \"\")",
    "Iterable.type": "Iterable<$0>",
    "i8.parse": "$0.toByte()", "i16.parse": "$0.toShort()", "i32.parse": "$0.toInt()", "i64.parse": "$0.toLong()",
    "u8.parse": "$0.toUByte()", "u16.parse": "$0.toUShort()", "u32.parse": "$0.toUInt()", "u64.parse": "$0.toULong()",
    "f32.parse": "$0.toFloat()", "f64.parse": "$0.toDouble()"}
}

os.makedirs('plugins/kotlin', exist_ok=True)
json.dump(d, open('plugins/kotlin/polyglot-plugin.json', 'w'), indent=2)
print("scaffolded plugins/kotlin/polyglot-plugin.json (name=%s ext=%s)" % (d['name'], d['fileExtension']))
