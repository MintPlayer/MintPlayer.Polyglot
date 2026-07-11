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
# Kotlin has no `?:` ternary; `if` is an expression.
r['Cond'] = {"tmpl": ["(if (", {"emit": "node.cond"}, ") ", {"emit": "node.then"}, " else ", {"emit": "node.els"}, ")"]}
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
# EnumDecl -> Kotlin `enum class` (enums.pg matches cases; the int value is unused there).
r['EnumDecl'] = {"line": {"tmpl": ["enum class ", ident("decl.name"), " { ",
    {"map": "decl.cases", "sep": ", ", "item": ident("item.name")}, " }"]}}
# Match -> `(scrutinee).let { _m -> when { <cond> -> <value> ... else -> throw } }`. `.let` types _m from
# the scrutinee (numeric guards work); subject-less `when` allows boolean guards; `is` smart-casts _m.
r['ktArmBase'] = {"case": {"when": [
    [{"eq": ["item.pattern.kind", "literal"]}, {"tmpl": ["_m == ", {"emit": "item.pattern.literal"}]}],
    [{"eq": ["item.pattern.kind", "enumCase"]}, {"tmpl": ["_m == ", {"get": "item.pattern.enumType"}, ".", {"get": "item.pattern.enumCase"}]}],
    [{"eq": ["item.pattern.kind", "ctor"]}, {"tmpl": ["_m is ", {"get": "item.pattern.ctorCase"}]}]],
  "else": "true"}}
r['ktArmGuard'] = {"case": {"when": [
    [{"eq": ["item.pattern.kind", "binding"]}, {"tmpl": ["run { val ", ident("item.pattern.binding"), " = _m; ", {"emit": "item.guard"}, " }"]}],
    [{"and": [{"eq": ["item.pattern.kind", "ctor"]}, {"not": {"eq": ["item.pattern.binders.count", "0"]}}]},
     {"tmpl": ["run { ", {"map": "item.pattern.binders", "sep": "", "item": {"tmpl": ["val ", ident("item.binding"), " = _m.", {"get": "item.field"}, "; "]}}, {"emit": "item.guard"}, " }"]}]],
  "else": {"emit": "item.guard"}}}
r['ktArmValue'] = {"case": {"when": [
    [{"eq": ["item.pattern.kind", "binding"]}, {"tmpl": ["run { val ", ident("item.pattern.binding"), " = _m; ", {"emit": "item.body"}, " }"]}],
    [{"and": [{"eq": ["item.pattern.kind", "ctor"]}, {"not": {"eq": ["item.pattern.binders.count", "0"]}}]},
     {"tmpl": ["run { ", {"map": "item.pattern.binders", "sep": "", "item": {"tmpl": ["val ", ident("item.binding"), " = _m.", {"get": "item.field"}, "; "]}}, {"emit": "item.body"}, " }"]}]],
  "else": {"emit": "item.body"}}}
r['ktArmCond'] = {"case": {"when": [
    [{"eq": ["item.hasGuard", "false"]}, {"call": "ktArmBase"}],
    [{"or": [{"eq": ["item.pattern.kind", "wildcard"]}, {"eq": ["item.pattern.kind", "binding"]}]}, {"call": "ktArmGuard"}]],
  "else": {"tmpl": ["(", {"call": "ktArmBase"}, ") && (", {"call": "ktArmGuard"}, ")"]}}}
r['Match'] = {"tmpl": ["(", {"emit": "node.scrutinee"}, ").let { _m -> when {\n",
    {"map": "node.arms", "sep": "", "item": {"tmpl": ["  ", {"call": "ktArmCond"}, " -> ", {"call": "ktArmValue"}, "\n"]}},
    "  else -> throw RuntimeException()\n} }"]}
r['UnionDecl'] = {"seq": [
    {"line": {"tmpl": ["sealed class ", ident("decl.name"), {"fn": "generics"}]}},
    {"mapDecl": "decl.cases", "each": {"line": {"tmpl": [
        {"case": {"when": [[{"eq": ["item.fields.count", "0"]}, {"tmpl": ["class ", ident("item.name"), {"fn": "generics"}]}]],
          "else": {"tmpl": ["data class ", ident("item.name"), {"fn": "generics"}, "(",
            {"map": "item.fields", "sep": ", ", "item": {"tmpl": ["val ", ident("item.name"), ": ", {"type": "item.type"}]}}, ")"]}}},
        " : ", ident("decl.name"), {"fn": "generics"}, "()"]}}}]}

# ---- P26 slice 2 batch 2: control flow, exceptions, interpolation, class/method families ----
s['binaryOp'] = {"<<": "shl", ">>": "shr", ">>>": "ushr", "&": "and", "|": "or", "^": "xor"}  # Kotlin infix bit ops
# Kotlin keywords (hard + soft that bite as identifiers); escape with backticks.
s['identifiers']['keywords'] = ["as", "break", "class", "continue", "do", "else", "false", "for", "fun", "if",
    "in", "interface", "is", "null", "object", "package", "return", "super", "this", "throw", "true", "try",
    "typealias", "typeof", "val", "var", "when", "while", "by", "catch", "constructor", "delegate", "dynamic",
    "field", "file", "finally", "get", "import", "init", "param", "property", "receiver", "set", "setparam",
    "value", "where", "it"]
s['identifiers']['escape'] = {"strategy": "wrap", "with": "`"}

r['Await'] = {"emit": "node.operand"}  # Kotlin suspend calls return the value directly; no `await` keyword
r['With'] = {"tmpl": [{"emitChild": "node.base", "side": "recv"}, ".copy(",
    {"map": "node.fields", "sep": ", ", "item": {"tmpl": [ident("item.name"), " = ", {"emit": "item.value"}]}}, ")"]}
r['Interp'] = {"tmpl": ["\"", {"interleave": {"lits": "node.chunks", "holes": "node.holes",
    "lit": {"fn": "escape", "args": ["interp", {"get": "item"}]},
    "hole": {"case": {"when": [[{"eq": ["item.typeIsBool", "true"]}, {"tmpl": ["${if (", {"emit": "item"}, ") \"true\" else \"false\"}"]}]],
        "else": {"tmpl": ["${", {"emit": "item"}, "}"]}}}}}, "\""]}
r['ForStmt'] = {"case": {"when": [
    [{"eq": ["stmt.isRange", "true"]}, {"block": {"head": {"tmpl": ["for (", ident("stmt.binding"), " in ",
        {"emit": "stmt.rangeStart"}, {"case": {"when": [[{"eq": ["stmt.inclusive", "true"]}, ".."]], "else": " until "}},
        {"emit": "stmt.rangeEnd"}, ")"]}, "body": [{"stmts": "stmt.body"}]}}],
    [{"not": {"eq": ["stmt.tupleBindings.count", "0"]}}, {"block": {"head": {"tmpl": ["for ((",
        {"map": "stmt.tupleBindings", "sep": ", ", "item": ident("item")}, ") in ", {"emit": "stmt.iterable"}, ")"]},
        "body": [{"stmts": "stmt.body"}]}}]],
  "else": {"block": {"head": {"tmpl": ["for (", ident("stmt.binding"), " in ", {"emit": "stmt.iterable"}, ")"]},
    "body": [{"stmts": "stmt.body"}]}}}}
# try/catch: Kotlin `catch (e: Type)`; a `when` guard lowers to `if (!(guard)) throw <e>` (no native catch guard).
r['TryStmt'] = {"seq": [
    {"block": {"head": "try", "body": [{"stmts": "stmt.body"}]}},
    {"mapDecl": "stmt.catches", "each": {"block": {
        "head": {"tmpl": ["catch (", {"case": {"when": [[{"eq": ["item.hasBinding", "true"]}, ident("item.binding")]], "else": "__e"}},
            ": ", {"case": {"when": [[{"eq": ["item.hasType", "true"]}, {"type": "item.type"}]], "else": "Throwable"}}, ")"]},
        "body": [
            {"case": {"when": [[{"eq": ["item.hasGuard", "true"]}, {"line": {"tmpl": ["if (!(", {"emit": "item.guard"}, ")) throw ",
                {"case": {"when": [[{"eq": ["item.hasBinding", "true"]}, ident("item.binding")]], "else": "__e"}}]}}]]}},
            {"stmts": "item.body"}]}}},
    {"case": {"when": [[{"eq": ["stmt.hasFinally", "true"]}, {"block": {"head": "finally", "body": [{"stmts": "stmt.finallyBody"}]}}]]}}]}
# operator fun name mapping (Kotlin uses named operator functions).
r['ktOpName'] = {"case": {"when": [
    [{"eq": ["decl.opSymbol", "+"]}, "plus"], [{"eq": ["decl.opSymbol", "-"]}, "minus"],
    [{"eq": ["decl.opSymbol", "*"]}, "times"], [{"eq": ["decl.opSymbol", "/"]}, "div"],
    [{"eq": ["decl.opSymbol", "%"]}, "rem"], [{"eq": ["decl.opSymbol", "get"]}, "get"], [{"eq": ["decl.opSymbol", "set"]}, "set"]],
  "else": {"get": "decl.opSymbol"}}}
r['csMethodSig'] = {"tmpl": [{"case": {"when": [[{"eq": ["decl.isAsync", "true"]}, "suspend "]]}}, "fun ", {"fn": "generics"}, " ",
    ident("decl.name"), "(", {"map": "decl.params", "sep": ", ", "item": {"call": "csParam"}}, ")",
    {"case": {"when": [[{"eq": ["decl.returnsUnit", "true"]}, ""]], "else": {"tmpl": [": ", {"type": "decl.returnType"}]}}}]}
r['csOperatorSig'] = {"tmpl": ["operator fun ", {"call": "ktOpName"}, "(", {"map": "decl.params", "sep": ", ", "item": {"call": "csParam"}}, ")",
    {"case": {"when": [[{"eq": ["decl.returnsUnit", "true"]}, ""]], "else": {"tmpl": [": ", {"type": "decl.returnType"}]}}}]}
def ktBody(sig):
    return {"case": {"when": [[{"eq": ["decl.exprBodied", "true"]}, {"line": {"tmpl": [{"call": sig}, " = ", {"emit": "decl.exprBody"}]}}]],
        "else": {"block": {"head": {"call": sig}, "body": [{"stmts": "decl.body"}]}}}}
r['MethodDecl'] = {"case": {"when": [
    [{"eq": ["decl.kind", "property"]}, {"seq": [
        {"line": {"tmpl": ["val ", ident("decl.name"), ": ", {"type": "decl.returnType"}]}},
        {"line": {"tmpl": ["    get() = ", {"emit": "decl.exprBody"}]}}]}],
    [{"eq": ["decl.kind", "operator"]}, ktBody("csOperatorSig")]],
  "else": ktBody("csMethodSig")}}
# Extension: Kotlin `fun Recv.name(...)`; the receiver becomes the extension-fn receiver (params[0] is `self`).
r['csExtSig'] = {"tmpl": ["fun ", {"fn": "generics"}, " ", {"type": "decl.params.0.type"}, ".", ident("decl.name"), "(",
    {"map": "decl.paramsTail", "sep": ", ", "item": {"tmpl": [ident("item.name"), ": ", {"type": "item.type"}]}}, ")",
    {"case": {"when": [[{"eq": ["decl.returnsUnit", "true"]}, ""]], "else": {"tmpl": [": ", {"type": "decl.returnType"}]}}}]}
r['ExtensionDecl'] = {"case": {"when": [[{"eq": ["decl.exprBodied", "true"]},
    {"line": {"tmpl": [{"call": "csExtSig"}, " = ", {"emit": "decl.exprBody"}]}}]],
  "else": {"block": {"head": {"call": "csExtSig"}, "body": [{"stmts": "decl.body"}]}}}}
# But extension bodies reference `this` (the receiver); in a Kotlin extension fn, `this` IS the receiver, so
# `self` must map to `this`. Core rewrites this->self and names params[0] `self`; emit `self` as a value that
# equals the receiver by aliasing at the top. Simplest: the extension fn takes no explicit self param and the
# body uses `this`; but Core prepends `self`. So keep params[0]=self as the receiver TYPE (above) and alias
# `val self = this` — handled by emitting the body after a self alias is unnecessary since we drop params[0].
# ClassDecl: Kotlin `class Name[ : Base(super)][, Iface...] { fields; init/ctor; methods; companion { statics } }`.
r['csClassHead'] = {"tmpl": ["class ", ident("decl.name"), {"fn": "generics"},
    {"case": {"when": [[{"eq": ["decl.hasExtBase", "true"]}, {"tmpl": [" : ", {"type": "decl.extBase"},
        {"case": {"when": [[{"eq": ["decl.hasSuper", "true"]}, {"tmpl": ["(", {"map": "decl.superArgs", "sep": ", ", "item": {"emit": "item"}}, ")"]}]], "else": "()"}}]}]]}},
    {"case": {"when": [[{"not": {"eq": ["decl.ifaceBases.count", "0"]}}, {"tmpl": [
        {"case": {"when": [[{"eq": ["decl.hasExtBase", "true"]}, ", "]], "else": " : "}},
        {"map": "decl.ifaceBases", "sep": ", ", "item": {"type": "item"}}]}]]}}]}
_ktFieldInit = {"case": {"when": [[{"eq": ["item.hasInit", "true"]}, {"tmpl": [" = ", {"emit": "item.init"}]}]], "else": " = TODO()"}}
_ktMut = {"case": {"when": [[{"eq": ["item.isMutable", "true"]}, "var "]], "else": "val "}}
_ktInstField = {"tmpl": [_ktMut, ident("item.name"), ": ", {"type": "item.type"}, _ktFieldInit]}
_ktField = {"line": {"case": {"when": [[{"eq": ["item.isStatic", "true"]}, ""]], "else": _ktInstField}}}
_ktCtor = {"case": {"when": [[{"eq": ["decl.hasInit", "true"]}, {"block": {
    "head": {"tmpl": ["constructor(", {"map": "decl.initParams", "sep": ", ", "item": {"call": "csParam"}}, ")"]},
    "body": [{"stmts": "decl.initBody"}]}}]]}}
r['ClassDecl'] = {"block": {"head": {"call": "csClassHead"}, "body": [
    {"mapDecl": "decl.fields", "each": _ktField},
    _ktCtor,
    {"mapMembers": "decl.methods", "rule": "MethodDecl"}]}}

d['std'] = {
  "std.collections": {"List.type": "MutableList<$0>", "List.init": "mutableListOf<$0>()", "List.count": "$this.size",
    "List.add": "$this.add($0)", "List.clear": "$this.clear()",
    "List.removeAll": "$this.removeAll { __e -> ($0)(__e) }", "List.removeAt": "$this.removeAt($0)"},
  "std.io": {"print": "println(x)"},
  "std.math": {"Math.PI": "kotlin.math.PI", "Math.E": "kotlin.math.E", "Math.sqrt": "kotlin.math.sqrt($0)",
    "Math.ln": "kotlin.math.ln($0)", "Math.log": "kotlin.math.ln($0)", "Math.log2": "kotlin.math.log2($0)",
    "Math.log10": "kotlin.math.log10($0)", "Math.exp": "kotlin.math.exp($0)", "Math.pow": "kotlin.math.pow($0, $1)",
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
