#include "mintplayer/polyglot/emitter_base.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

// The shared walk machinery for the hand-written backends — see emitter_base.hpp for the abstraction.

namespace mintplayer::polyglot {

// ---- IrExprCtx: the target-independent rule-interpreter seam over the IR ---------------------------------

namespace {
// An integer-width `.pg` type name (i8..u64) — the language's int family, target-independent.
bool isIntTypeName(const TypeRef& t) {
    if (t.kind != TypeRef::Kind::Named) return false;
    const std::string& n = t.name;
    return n == "i8" || n == "i16" || n == "i32" || n == "i64" ||
           n == "u8" || n == "u16" || n == "u32" || n == "u64";
}
bool isFloatTypeName(const TypeRef& t) {
    return t.kind == TypeRef::Kind::Named && (t.name == "f32" || t.name == "f64");
}
bool isInt64Name(const TypeRef& t) {
    return t.kind == TypeRef::Kind::Named && (t.name == "i64" || t.name == "u64");
}
bool isBoolTypeName(const TypeRef& t) {
    return t.kind == TypeRef::Kind::Named && t.name == "bool";
}
} // namespace

std::string IrExprCtx::get(const std::string& path) const {
    if (path == "node.type") return e_.type.name;
    if (path == "node.op") {
        if (e_.kind == ir::ExprKind::Binary) return static_cast<const ir::Binary&>(e_).op;
        if (e_.kind == ir::ExprKind::Unary)  return static_cast<const ir::Unary&>(e_).op;
        return targetGet(path);
    }
    if (path == "node.name" && e_.kind == ir::ExprKind::Var)    return static_cast<const ir::Var&>(e_).name;
    if (path == "node.code" && e_.kind == ir::ExprKind::Extern) return static_cast<const ir::Extern&>(e_).code;
    if (e_.kind == ir::ExprKind::Binary) { // module facts precomputed in lowering (P19)
        const auto& b = static_cast<const ir::Binary&>(e_);
        if (path == "node.lhsIsRecord")   return b.lhsIsRecord ? "true" : "false";
        if (path == "node.lhsIsUserType") return b.lhsIsUserType ? "true" : "false";
        if (path == "node.lhsIsUnion")    return b.lhsIsUnion ? "true" : "false";     // #57
        if (path == "node.lhsHasUserEq")  return b.lhsHasUserEq ? "true" : "false";   // #49
    }
    if (path == "node.receiverHasIndexer" && e_.kind == ir::ExprKind::Index)
        return static_cast<const ir::Index&>(e_).receiverHasIndexer ? "true" : "false";
    if (path == "node.insideOperator" && e_.kind == ir::ExprKind::This)
        return static_cast<const ir::This&>(e_).insideOperator ? "true" : "false";
    // Type-class predicates — which `.pg` types are ints/floats/64-bit is a LANGUAGE fact, not a target
    // fact; the per-target part is only which predicate a rule consults.
    if (path == "node.typeIsInt")      return isIntTypeName(e_.type) ? "true" : "false";
    if (path == "node.typeIsSmallInt") // 32-bit-or-narrower (i64/u64 ride BigInt on JS, not bitwise ops)
        return isIntTypeName(e_.type) && e_.type.name != "i64" && e_.type.name != "u64" ? "true" : "false";
    if (path == "node.typeIsFloat") return isFloatTypeName(e_.type) ? "true" : "false";
    if (path == "node.typeIsInt64") return isInt64Name(e_.type) ? "true" : "false";
    if (e_.kind == ir::ExprKind::Cast) { // the operand's (source) type classes, for conversion rules
        const TypeRef& from = static_cast<const ir::Cast&>(e_).operand->type;
        if (path == "node.castSame")
            return from.kind == TypeRef::Kind::Named && e_.type.kind == TypeRef::Kind::Named &&
                           from.name == e_.type.name
                       ? "true" : "false";
        if (path == "node.fromIsInt")   return isIntTypeName(from) ? "true" : "false";
        if (path == "node.fromIsFloat") return isFloatTypeName(from) ? "true" : "false";
        if (path == "node.fromIsInt64") return isInt64Name(from) ? "true" : "false";
    }
    if (path == "node.hasOpMethod" && e_.kind == ir::ExprKind::Binary) {
        // operator overload -> method call, on targets whose spec declares an opMethod table row
        const auto& b = static_cast<const ir::Binary&>(e_);
        return b.lhsIsUserType && !specTable(spec_, "opMethod", b.op).empty() ? "true" : "false";
    }
    if (e_.kind == ir::ExprKind::With) {
        const auto& w = static_cast<const ir::With&>(e_);
        if (path == "node.baseIsSimple")   return w.baseIsSimple ? "true" : "false";
        if (path == "node.tempName")       return w.tempName;
        if (path == "node.ctorArgs.count") return std::to_string(w.ctorArgs.size());
        if (path == "node.fields.count")   return std::to_string(w.fields.size());
        if (path.rfind("node.fields.", 0) == 0 && path.size() > 5 && path.rfind(".name") == path.size() - 5) {
            const std::size_t i = static_cast<std::size_t>(std::stoul(path.substr(12)));
            if (i < w.fields.size()) return w.fields[i].name;
        }
    }
    if (e_.kind == ir::ExprKind::Call) {
        const auto& c = static_cast<const ir::Call&>(e_);
        if (path == "node.callee")        return c.callee;
        if (path == "node.mangledCallee") return c.mangledCallee;
        if (path == "node.isFree")        return c.isFree ? "true" : "false";
        if (path == "node.args.count")    return std::to_string(c.args.size());
    }
    if (e_.kind == ir::ExprKind::Member) {
        const auto& m = static_cast<const ir::Member&>(e_);
        if (path == "node.staticType") return m.staticType;
        if (path == "node.field")      return m.field;
        if (path == "node.nullSafe")   return m.nullSafe ? "true" : "false";
        if (path == "node.isProperty") return m.isProperty ? "true" : "false";
    }
    if (e_.kind == ir::ExprKind::MethodCall) {
        const auto& mc = static_cast<const ir::MethodCall&>(e_);
        if (path == "node.method")      return mc.method;
        if (path == "node.staticType")  return mc.staticType;
        if (path == "node.isExtension") return mc.isExtension ? "true" : "false";
        if (path == "node.args.count")  return std::to_string(mc.args.size());
    }
    if (e_.kind == ir::ExprKind::Lambda) {
        const auto& l = static_cast<const ir::Lambda&>(e_);
        if (path == "node.exprBodied")   return l.exprBodied ? "true" : "false";
        if (path == "node.params.count") return std::to_string(l.params.size());
        // Capture facts (P25 §4.18) — for targets with explicit capture lists (PHP `use (…)`). Captures never
        // include `this` (that is `capturesThis`); `allCapturesByValue` is true when nothing needs a cell.
        if (path == "node.capturesThis")      return l.capturesThis ? "true" : "false";
        if (path == "node.allCapturesByValue") {
            for (const auto& c : l.captures) if (c.needsCell) return "false";
            return "true";
        }
        if (path == "node.captures.count")    return std::to_string(l.captures.size());
        if (path.rfind("node.captures.", 0) == 0) { // node.captures.<i>.{name|needsCell}
            const std::size_t i = static_cast<std::size_t>(std::stoul(path.substr(14)));
            const std::size_t dot = path.find('.', 14);
            if (i < l.captures.size() && dot != std::string::npos) {
                const std::string rest = path.substr(dot + 1);
                if (rest == "name")      return l.captures[i].name;
                if (rest == "needsCell") return l.captures[i].needsCell ? "true" : "false";
            }
        }
        if (path.rfind("node.params.", 0) == 0) { // node.params.<i>.{name|hasType}
            const std::size_t i = static_cast<std::size_t>(std::stoul(path.substr(12)));
            const std::size_t dot = path.find('.', 12);
            if (i < l.params.size() && dot != std::string::npos) {
                const std::string rest = path.substr(dot + 1);
                if (rest == "name")    return l.params[i].name;
                if (rest == "hasType") return l.params[i].type.absent() ? "false" : "true";
            }
        }
    }
    if (e_.kind == ir::ExprKind::New) {
        const auto& n = static_cast<const ir::New&>(e_);
        if (path == "node.typeName")   return n.typeName;
        if (path == "node.args.count") return std::to_string(n.args.size());
    }
    if (path == "node.typeArgs.count") { // the node's construction/result/scrutinee type args, per kind
        if (const std::vector<TypeRef>* ta = nodeTypeArgs()) return std::to_string(ta->size());
    }
    if (e_.kind == ir::ExprKind::MakeCase) {
        const auto& mc = static_cast<const ir::MakeCase&>(e_);
        if (path == "node.caseName")     return mc.caseName;
        if (path == "node.fields.count") return std::to_string(mc.fields.size());
        if (path.rfind("node.fields.", 0) == 0 && path.size() > 5 && path.rfind(".name") == path.size() - 5) {
            const std::size_t i = static_cast<std::size_t>(std::stoul(path.substr(12)));
            if (i < mc.fields.size()) return mc.fields[i].name;
        }
    }
    if (path == "node.elements.count") {
        if (e_.kind == ir::ExprKind::ListLit) return std::to_string(static_cast<const ir::ListLit&>(e_).elements.size());
        if (e_.kind == ir::ExprKind::Tuple)   return std::to_string(static_cast<const ir::Tuple&>(e_).elements.size());
    }
    if (path == "node.indices.count" && e_.kind == ir::ExprKind::Index)
        return std::to_string(static_cast<const ir::Index&>(e_).indices.size());
    if (path == "node.isArray") { // a list literal whose target type is the fixed-size Array<T> (vs growable List<T>)
        const bool isArray = e_.kind == ir::ExprKind::ListLit &&
                             static_cast<const ir::ListLit&>(e_).type.name == "Array";
        return isArray ? "true" : "false";
    }
    if (e_.kind == ir::ExprKind::Match) {
        const auto& m = static_cast<const ir::Match&>(e_);
        if (path == "node.arms.count")  return std::to_string(m.arms.size());
        if (path == "node.hasCatchAll") return m.hasCatchAll ? "true" : "false";
        if (path == "node.isStatement") return m.isStatement ? "true" : "false"; // #52
        if (path.rfind("node.arms.", 0) == 0) { // node.arms.<i>.<rest>
            const std::size_t i = static_cast<std::size_t>(std::stoul(path.substr(10)));
            const std::size_t dot = path.find('.', 10);
            if (i < m.arms.size() && dot != std::string::npos) {
                const ir::MatchArm& a = m.arms[i];
                const std::string rest = path.substr(dot + 1);
                if (rest == "hasGuard") return a.guard ? "true" : "false";
                if (rest == "pattern.kind") {
                    switch (a.pattern.kind) {
                        case ir::PatternKind::Wildcard: return "wildcard";
                        case ir::PatternKind::Literal:  return "literal";
                        case ir::PatternKind::Binding:  return "binding";
                        case ir::PatternKind::EnumCase: return "enumCase";
                        case ir::PatternKind::Ctor:     return "ctor";
                    }
                }
                if (rest == "pattern.binding")       return a.pattern.binding;
                if (rest == "pattern.enumType")      return a.pattern.enumType;
                if (rest == "pattern.enumCase")      return a.pattern.enumCase;
                if (rest == "pattern.ctorCase")      return a.pattern.ctorCase;
                if (rest == "pattern.binders.count") return std::to_string(a.pattern.binders.size());
                if (rest.rfind("pattern.binders.", 0) == 0) { // pattern.binders.<j>.<field>
                    const std::string sub = rest.substr(16);
                    const std::size_t j = static_cast<std::size_t>(std::stoul(sub));
                    const std::size_t jdot = sub.find('.');
                    if (j < a.pattern.binders.size() && jdot != std::string::npos) {
                        const std::string bf = sub.substr(jdot + 1);
                        if (bf == "binding")    return a.pattern.binders[j].binding;
                        if (bf == "field")      return a.pattern.binders[j].field;
                        if (bf == "isLiteral")  return a.pattern.binders[j].literal ? "true" : "false"; // #43
                        if (bf == "isWildcard") // neither a binding nor a literal slot
                            return (!a.pattern.binders[j].literal && a.pattern.binders[j].binding.empty()) ? "true" : "false";
                    }
                }
            }
        }
    }
    if (e_.kind == ir::ExprKind::Interp) { // chunks are scalar text; holes are child exprs (childExpr below)
        const auto& in = static_cast<const ir::Interp&>(e_);
        if (path == "node.chunks.count") return std::to_string(in.chunks.size());
        if (path == "node.holes.count")  return std::to_string(in.holes.size());
        if (path.rfind("node.chunks.", 0) == 0) {
            const std::size_t i = static_cast<std::size_t>(std::stoul(path.substr(12)));
            if (i < in.chunks.size()) return in.chunks[i];
        }
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
        if (e_.kind == ir::ExprKind::Char) return static_cast<const ir::CharLit&>(e_).value;
    }
    // Child type-class predicates: `<child-path>.typeIsBool` / `.typeIsFloat` / … answer the LANGUAGE
    // type-class of any child expression a rule can name — an interp hole type-gates its stringification
    // with these (a bool hole must spell "true"/"false" identically on every target, PRD §3.C).
    {
        static const std::pair<const char*, bool (*)(const TypeRef&)> kChildTypeFacts[] = {
            {".typeIsBool", isBoolTypeName},   {".typeIsFloat", isFloatTypeName},
            {".typeIsInt", isIntTypeName},     {".typeIsInt64", isInt64Name},
        };
        for (const auto& [suffix, pred] : kChildTypeFacts) {
            const std::size_t n = std::string(suffix).size();
            if (path.size() > n && path.compare(path.size() - n, n, suffix) == 0)
                if (const ir::Expr* child = childExpr(path.substr(0, path.size() - n)))
                    return pred(child->type) ? "true" : "false";
        }
    }
    return targetGet(path);
}

const ir::Expr* IrExprCtx::childExpr(const std::string& path) const {
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
        if (e_.kind == ir::ExprKind::MethodCall) {
            const auto& mc = static_cast<const ir::MethodCall&>(e_);
            if (i < mc.args.size()) return mc.args[i].get();
        }
    }
    if (e_.kind == ir::ExprKind::MethodCall && path == "node.object")
        return static_cast<const ir::MethodCall&>(e_).object.get();
    if (e_.kind == ir::ExprKind::MakeCase && path.rfind("node.fields.", 0) == 0) {
        // `node.fields.<i>` and `node.fields.<i>.value` both name the field's value expression.
        const auto& mc = static_cast<const ir::MakeCase&>(e_);
        const std::size_t i = static_cast<std::size_t>(std::stoul(path.substr(12)));
        if (i < mc.fields.size()) return mc.fields[i].value.get();
    }
    if (e_.kind == ir::ExprKind::Member && path == "node.object")
        return static_cast<const ir::Member&>(e_).object.get();
    if (e_.kind == ir::ExprKind::Index) {
        const auto& ix = static_cast<const ir::Index&>(e_);
        if (path == "node.receiver") return ix.receiver.get();
        if (path == "node.index")    return ix.indices.empty() ? nullptr : ix.indices[0].get(); // single-arg alias
        if (path.rfind("node.indices.", 0) == 0) { // indexed subscript `node.indices.<i>` (from a `map` rule)
            const std::size_t i = static_cast<std::size_t>(std::stoul(path.substr(13)));
            if (i < ix.indices.size()) return ix.indices[i].get();
        }
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
    if (path == "node.operand") {
        if (e_.kind == ir::ExprKind::Cast)  return static_cast<const ir::Cast&>(e_).operand.get();
        if (e_.kind == ir::ExprKind::Unary) return static_cast<const ir::Unary&>(e_).operand.get();
        if (e_.kind == ir::ExprKind::Await) return static_cast<const ir::Await&>(e_).operand.get();
    }
    if (e_.kind == ir::ExprKind::Lambda && path == "node.body")
        return static_cast<const ir::Lambda&>(e_).body.get();
    if (e_.kind == ir::ExprKind::Interp && path.rfind("node.holes.", 0) == 0) {
        const auto& in = static_cast<const ir::Interp&>(e_);
        const std::size_t i = static_cast<std::size_t>(std::stoul(path.substr(11)));
        if (i < in.holes.size()) return in.holes[i].get();
    }
    if (e_.kind == ir::ExprKind::Match) {
        const auto& m = static_cast<const ir::Match&>(e_);
        if (path == "node.scrutinee") return m.scrutinee.get();
        if (path.rfind("node.arms.", 0) == 0) { // node.arms.<i>.{body|guard|pattern.literal}
            const std::size_t i = static_cast<std::size_t>(std::stoul(path.substr(10)));
            const std::size_t dot = path.find('.', 10);
            if (i < m.arms.size() && dot != std::string::npos) {
                const ir::MatchArm& a = m.arms[i];
                const std::string rest = path.substr(dot + 1);
                if (rest == "body")            return a.body.get();
                if (rest == "guard")           return a.guard.get();
                if (rest == "pattern.literal") return a.pattern.literal.get();
                if (rest.rfind("pattern.binders.", 0) == 0) { // pattern.binders.<j>.literal (issue #43)
                    const std::string sub = rest.substr(16);
                    const std::size_t jdot = sub.find('.');
                    if (jdot != std::string::npos && sub.substr(jdot + 1) == "literal") {
                        const std::size_t j = static_cast<std::size_t>(std::stoul(sub.substr(0, jdot)));
                        if (j < a.pattern.binders.size()) return a.pattern.binders[j].literal.get();
                    }
                }
            }
        }
    }
    if (e_.kind == ir::ExprKind::With) {
        const auto& w = static_cast<const ir::With&>(e_);
        if (path == "node.base") return w.base.get();
        if (path.rfind("node.ctorArgs.", 0) == 0) { // indexed ctor-rebuild arg (from a `map` rule)
            const std::size_t i = static_cast<std::size_t>(std::stoul(path.substr(14)));
            if (i < w.ctorArgs.size()) return w.ctorArgs[i].get();
        }
        if (path.rfind("node.fields.", 0) == 0) { // `node.fields.<i>` / `.value`: the override's value expr
            const std::size_t i = static_cast<std::size_t>(std::stoul(path.substr(12)));
            if (i < w.fields.size()) return w.fields[i].value.get();
        }
    }
    return nullptr;
}

std::string IrExprCtx::emitChild(const std::string& path, const std::string& side) const {
    const ir::Expr* c = childExpr(path);
    if (!c) return "";
    std::string inner = emit_(*c);
    if (side == "l" || side == "r") { // binary operand: wrap by precedence + associativity (shared policy)
        // A widening cast can emit as pure identity on a target (f32->f64 is textless on TS/Python/PHP),
        // which would silently swallow the parens its operand needs — `(a + b) * x` becoming `a + b * x`
        // (wave-2 f32.pg catch). Judge precedence by the expression UNDER any casts; if the cast does
        // emit real text it self-brackets, so the extra parens are harmless.
        const ir::Expr* eff = c;
        while (eff->kind == ir::ExprKind::Cast && static_cast<const ir::Cast&>(*eff).operand)
            eff = static_cast<const ir::Cast&>(*eff).operand.get();
        if (e_.kind == ir::ExprKind::Binary && eff->kind == ir::ExprKind::Binary) {
            const std::string& po = static_cast<const ir::Binary&>(e_).op;
            const std::string& co = static_cast<const ir::Binary&>(*eff).op;
            // Two target quirks precedence levels can't express, both harmless as extra parens on the
            // other targets: JS makes bare `a && b ?? c` a SyntaxError (?? refuses to mix with &&/||
            // unparenthesized), and Python CHAINS comparisons (`a < b == c` means `a < b and b == c`),
            // so a comparison operand of a comparison keeps its parens even where C# wouldn't need them.
            auto logical = [](const std::string& o) { return o == "&&" || o == "||"; };
            auto comparison = [](const std::string& o) {
                return o == "==" || o == "!=" || o == "<" || o == "<=" || o == ">" || o == ">=";
            };
            if ((po == "??" && logical(co)) || (co == "??" && logical(po))) return "(" + inner + ")";
            if (comparison(po) && comparison(co)) return "(" + inner + ")";
            int pp = operatorPrecedence(po);
            int cp = operatorPrecedence(co);
            if (side == "r" ? cp <= pp : cp < pp) return "(" + inner + ")";
        }
        return inner;
    }
    if (!side.empty() && wrapAtom(*c, side)) return "(" + inner + ")"; // "recv"/"unary": per-target policy
    return inner;
}

std::string IrExprCtx::builtin(const std::string& name, const std::vector<std::string>& args) const {
    if (name == "intSuffix") {
        auto it = spec_.intSuffix.find(args.empty() ? std::string() : args[0]);
        return it == spec_.intSuffix.end() ? std::string() : it->second;
    }
    if (name == "escapeString") return renderString(args.empty() ? std::string() : args[0]);
    if (name == "opSpelling")   return spec_.binOp(args.empty() ? std::string() : args[0]);
    // Identifier repair — generic catalog entries parameterized by the spec's `identifiers` block (P19).
    if (name == "ident")      return specIdent(spec_, args.empty() ? std::string() : args[0]);
    if (name == "identFn")    return specIdentFn(spec_, args.empty() ? std::string() : args[0]);
    if (name == "mangleName") return specMangle(spec_, args.empty() ? std::string() : args[0]);
    // Named escape maps — {"fn":"escape","args":["<map>", <text>]} over the spec's `escapes` data (P19).
    if (name == "escape" && args.size() >= 2) return specEscape(spec_, args[0], args[1]);
    // §3.C integer wrap — {"fn":"wrap","args":[<width>, <expr>]} over the spec's `wrapInt` templates (P19).
    if (name == "wrap" && args.size() >= 2) return specWrapInt(spec_, args[0], args[1]);
    // Named table lookup — {"fn":"table","args":[<table>, <key>]} over the spec's `tables` data (P19).
    if (name == "table" && args.size() >= 2) return specTable(spec_, args[0], args[1]);
    // Table-template substitution — {"fn":"subst","args":[<table>, <key>, <expr>]}: look the template up,
    // fill its `$x` holes (identity when absent). The conversion rules' width-specific pieces live here.
    if (name == "subst" && args.size() >= 3) return specSubst(spec_, args[0], args[1], args[2]);
    // Prelude requirement — {"fn":"require","args":[<key>]} records the key and emits nothing; the
    // emitter prepends the matching prelude once after the walk (Python's `_pg_idiv` helpers).
    if (name == "require") {
        if (requires_ && !args.empty()) requires_->insert(args[0]);
        return "";
    }
    if (name == "inlineBlock" && inline_ && e_.kind == ir::ExprKind::Lambda)
        return inline_(static_cast<const ir::Lambda&>(e_).block); // statement-bodied lambda, on one line
    return targetBuiltin(name, args);
}

std::string IrExprCtx::renderType(const std::string& path) const {
    const TypeRef* t = typeRefAt(path);
    return t ? renderTypeRef(*t) : "";
}

bool IrExprCtx::wrapAtom(const ir::Expr& c, const std::string& side) const {
    const std::vector<std::string>& set = side == "recv" ? spec_.wrapAtomRecv : spec_.wrapAtomUnary;
    for (const std::string& k : set) {
        if (k == "binary" && c.kind == ir::ExprKind::Binary) return true;
        if (k == "binaryScalar" && c.kind == ir::ExprKind::Binary &&
            !static_cast<const ir::Binary&>(c).lhsIsUserType)
            return true;
        if (k == "unary" && c.kind == ir::ExprKind::Unary) return true;
        if (k == "cast" && c.kind == ir::ExprKind::Cast) return true;
        if (k == "cond" && c.kind == ir::ExprKind::Cond) return true;
    }
    return false;
}

const TypeRef* IrExprCtx::typeRefAt(const std::string& path) const {
    if (path == "node.type") return &e_.type;
    if (path == "node.elem" && e_.kind == ir::ExprKind::ListLit)
        return &static_cast<const ir::ListLit&>(e_).elem;
    if (path.rfind("node.typeArgs.", 0) == 0) { // indexed into the node's kind-dispatched type-arg list
        if (const std::vector<TypeRef>* ta = nodeTypeArgs()) {
            const std::size_t i = static_cast<std::size_t>(std::stoul(path.substr(14)));
            if (i < ta->size()) return &(*ta)[i];
        }
    }
    if (e_.kind == ir::ExprKind::Lambda && path.rfind("node.params.", 0) == 0 &&
        path.size() > 5 && path.rfind(".type") == path.size() - 5) {
        const auto& l = static_cast<const ir::Lambda&>(e_);
        const std::size_t i = static_cast<std::size_t>(std::stoul(path.substr(12)));
        if (i < l.params.size()) return &l.params[i].type;
    }
    return nullptr;
}

// The TypeRef list `node.typeArgs` names for this node kind: a New's construction type args, a MakeCase's
// result-type args, a Match's scrutinee type args (generic union case patterns need `Full<int>`).
const std::vector<TypeRef>* IrExprCtx::nodeTypeArgs() const {
    if (e_.kind == ir::ExprKind::New)      return &static_cast<const ir::New&>(e_).typeArgs;
    if (e_.kind == ir::ExprKind::MakeCase) return &e_.type.args;
    if (e_.kind == ir::ExprKind::Match)    return &static_cast<const ir::Match&>(e_).scrutinee->type.args;
    return nullptr;
}

// ---- TypeRefCtx ------------------------------------------------------------------------------------------

std::string TypeRefCtx::get(const std::string& path) const {
    if (path == "type.kind") {
        switch (t_.kind) {
            case TypeRef::Kind::Named:    return "named";
            case TypeRef::Kind::Function: return "function";
            case TypeRef::Kind::Tuple:    return "tuple";
        }
    }
    if (path == "type.name")       return t_.name;
    if (path == "type.nameEmpty")  return t_.name.empty() ? "true" : "false";
    if (path == "type.nullable")   return t_.nullable ? "true" : "false";
    if (path == "type.args.count") return std::to_string(t_.args.size());
    if (path == "type.hasRet")     return t_.ret.empty() ? "false" : "true";
    if (path == "type.returnsUnit")
        return (t_.ret.empty() || (t_.ret[0].kind == TypeRef::Kind::Named && t_.ret[0].name == "unit"))
                   ? "true" : "false";
    if (path == "type.scalar") {
        auto it = spec_.scalarType.find(t_.name);
        return it == spec_.scalarType.end() ? std::string() : it->second;
    }
    if (path == "type.isValueType") { // the value-scalar family (C# `T?` = Nullable<T> only for these)
        const std::string& n = t_.name;
        bool v = n == "i8" || n == "i16" || n == "i32" || n == "i64" || n == "u8" || n == "u16" ||
                 n == "u32" || n == "u64" || n == "f32" || n == "f64" || n == "bool" || n == "char";
        return v ? "true" : "false";
    }
    if (path == "type.externTemplate") return externTemplate();
    return targetGet(path);
}

std::string TypeRefCtx::renderType(const std::string& path) const {
    if (path == "type.base" && t_.nullable) return renderTypeRef(base_);
    if (path == "type.ret" && !t_.ret.empty()) return renderTypeRef(t_.ret[0]);
    if (path.rfind("type.args.", 0) == 0) {
        const std::size_t i = static_cast<std::size_t>(std::stoul(path.substr(10)));
        if (i < t_.args.size()) return renderTypeRef(t_.args[i]);
    }
    return "";
}

std::string TypeRefCtx::builtin(const std::string& name, const std::vector<std::string>& args) const {
    if (name == "ident") return specIdent(spec_, args.empty() ? std::string() : args[0]);
    if (name == "substExtern") { // substitute `$0,$1,…` in the extern-class spelling with rendered type args
        const std::string tmpl = externTemplate();
        std::string out;
        for (std::size_t i = 0; i < tmpl.size();) {
            if (tmpl[i] == '$' && i + 1 < tmpl.size() && tmpl[i + 1] >= '0' && tmpl[i + 1] <= '9') {
                const std::size_t idx = static_cast<std::size_t>(tmpl[i + 1] - '0');
                if (idx < t_.args.size()) out += renderTypeRef(t_.args[idx]);
                i += 2;
            } else out += tmpl[i++];
        }
        return out;
    }
    return "";
}

std::string EnumDeclCtx::get(const std::string& path) const {
    if (path == "decl.name")        return e_.name;
    if (path == "decl.cases.count") return std::to_string(e_.cases.size());
    if (path.rfind("decl.cases.", 0) == 0) { // decl.cases.<i>.{name|value}
        const std::size_t i = static_cast<std::size_t>(std::stoul(path.substr(11)));
        const std::size_t dot = path.find('.', 11);
        if (i < e_.cases.size() && dot != std::string::npos) {
            const std::string rest = path.substr(dot + 1);
            if (rest == "name")  return e_.cases[i].name;
            if (rest == "value") return std::to_string(e_.cases[i].value);
        }
    }
    return "";
}

std::string EnumDeclCtx::builtin(const std::string& name, const std::vector<std::string>& args) const {
    if (name == "ident") return hooks_.ident(args.empty() ? std::string() : args[0]);
    if (name == "access") return hooks_.accessPrefix();
    return "";
}

std::string UnionDeclCtx::get(const std::string& path) const {
    if (path == "decl.name")        return u_.name;
    if (path == "decl.cases.count") return std::to_string(u_.cases.size());
    if (path.rfind("decl.cases.", 0) == 0) { // decl.cases.<i>.{name | fields.count | fields.<j>.name}
        const std::size_t i = static_cast<std::size_t>(std::stoul(path.substr(11)));
        const std::size_t dot = path.find('.', 11);
        if (i < u_.cases.size() && dot != std::string::npos) {
            const auto& c = u_.cases[i];
            const std::string rest = path.substr(dot + 1);
            if (rest == "name")         return c.name;
            if (rest == "fields.count") return std::to_string(c.fields.size());
            if (rest.rfind("fields.", 0) == 0) {
                const std::string sub = rest.substr(7);
                const std::size_t j = static_cast<std::size_t>(std::stoul(sub));
                if (j < c.fields.size() && sub.find('.') != std::string::npos &&
                    sub.substr(sub.find('.') + 1) == "name")
                    return c.fields[j].name;
            }
        }
    }
    return "";
}

std::string UnionDeclCtx::builtin(const std::string& name, const std::vector<std::string>& args) const {
    if (name == "generics") return hooks_.generics(u_.generics);
    if (name == "ident")    return hooks_.ident(args.empty() ? std::string() : args[0]);
    if (name == "access")   return hooks_.accessPrefix();
    return "";
}

std::string UnionDeclCtx::renderType(const std::string& path) const {
    // decl.cases.<i>.fields.<j>.type — the only TypeRef a union rule renders.
    if (path.rfind("decl.cases.", 0) != 0) return "";
    const std::size_t i = static_cast<std::size_t>(std::stoul(path.substr(11)));
    const std::size_t dot = path.find('.', 11);
    if (i >= u_.cases.size() || dot == std::string::npos) return "";
    const std::string rest = path.substr(dot + 1);
    if (rest.rfind("fields.", 0) != 0) return "";
    const std::string sub = rest.substr(7);
    const std::size_t j = static_cast<std::size_t>(std::stoul(sub));
    if (j >= u_.cases[i].fields.size() || sub.find('.') == std::string::npos ||
        sub.substr(sub.find('.') + 1) != "type")
        return "";
    return hooks_.renderTypeRef(u_.cases[i].fields[j].type);
}

std::string DeclHooks::generics(const std::vector<ir::GenericParam>& gs) const {
    const BackendSpec& s = specFn_();
    if (s.genericsStyle.empty() || gs.empty()) return "";
    std::string out = "<";
    for (std::size_t i = 0; i < gs.size(); ++i) {
        if (i) out += ", ";
        out += gs[i].name;
        if (s.genericsStyle == "inlineBounds") {
            bool first = true;
            for (const auto& b : gs[i].bounds) {
                if (s.genericsErase.count(b.name)) continue; // compile-time-only marker (INumber)
                out += first ? s.genericsBoundsIntro : s.genericsBoundsSep;
                first = false;
                out += renderTypeRef(b);
            }
        }
    }
    return out + ">";
}

std::string DeclHooks::where(const std::vector<ir::GenericParam>& gs) const {
    const BackendSpec& s = specFn_();
    if (s.genericsStyle != "whereClauses") return "";
    std::string out;
    for (const auto& g : gs) {
        bool first = true;
        for (const auto& b : g.bounds) {
            if (s.genericsErase.count(b.name)) continue; // compile-time-only marker (INumber)
            out += first ? " where " + g.name + s.genericsBoundsIntro : s.genericsBoundsSep;
            first = false;
            out += renderTypeRef(b);
        }
    }
    return out;
}

std::string InterfaceDeclCtx::get(const std::string& path) const {
    if (path == "decl.name")          return it_.name;
    if (path == "decl.bases.count")   return std::to_string(it_.bases.size());
    if (path == "decl.methods.count") return std::to_string(it_.methods.size());
    if (path.rfind("decl.methods.", 0) == 0) { // decl.methods.<i>.{name | params.count | params.<j>.{name|hasDefault}}
        const std::string sub = path.substr(13);
        const std::size_t i = static_cast<std::size_t>(std::stoul(sub));
        const std::size_t dot = sub.find('.');
        if (i < it_.methods.size() && dot != std::string::npos) {
            const ir::Method& m = it_.methods[i];
            const std::string rest = sub.substr(dot + 1);
            if (rest == "name")         return m.name;
            if (rest == "params.count") return std::to_string(m.params.size());
            if (rest.rfind("params.", 0) == 0) {
                const std::string ps = rest.substr(7);
                const std::size_t j = static_cast<std::size_t>(std::stoul(ps));
                const std::size_t pdot = ps.find('.');
                if (j < m.params.size() && pdot != std::string::npos) {
                    const std::string pf = ps.substr(pdot + 1);
                    if (pf == "name")       return m.params[j].name;
                    if (pf == "hasDefault") return m.params[j].defaultValue ? "true" : "false";
                }
            }
        }
    }
    return "";
}

std::string InterfaceDeclCtx::builtin(const std::string& name, const std::vector<std::string>& args) const {
    if (name == "generics") {
        if (args.empty() || args[0].empty()) return hooks_.generics(it_.generics);
        const std::size_t i = static_cast<std::size_t>(std::stoul(args[0]));
        return i < it_.methods.size() ? hooks_.generics(it_.methods[i].generics) : "";
    }
    if (name == "access") return hooks_.accessPrefix();
    if (name == "ident") return hooks_.ident(args.empty() ? std::string() : args[0]);
    if (name == "where") return hooks_.where(it_.generics);
    return "";
}

std::string InterfaceDeclCtx::renderType(const std::string& path) const {
    if (path.rfind("decl.bases.", 0) == 0) { // decl.bases.<i>
        const std::size_t i = static_cast<std::size_t>(std::stoul(path.substr(11)));
        return i < it_.bases.size() ? hooks_.renderTypeRef(it_.bases[i]) : "";
    }
    if (path.rfind("decl.methods.", 0) != 0) return ""; // decl.methods.<i>.{returnType | params.<j>.type}
    const std::string sub = path.substr(13);
    const std::size_t i = static_cast<std::size_t>(std::stoul(sub));
    const std::size_t dot = sub.find('.');
    if (i >= it_.methods.size() || dot == std::string::npos) return "";
    const ir::Method& m = it_.methods[i];
    const std::string rest = sub.substr(dot + 1);
    if (rest == "returnType") return hooks_.renderTypeRef(m.returnType);
    if (rest.rfind("params.", 0) == 0) {
        const std::string ps = rest.substr(7);
        const std::size_t j = static_cast<std::size_t>(std::stoul(ps));
        if (j < m.params.size() && ps.find('.') != std::string::npos && ps.substr(ps.find('.') + 1) == "type")
            return hooks_.renderTypeRef(m.params[j].type);
    }
    return "";
}

std::string InterfaceDeclCtx::emitChild(const std::string& path, const std::string&) const {
    // decl.methods.<i>.params.<j>.default — a param default re-enters the expression walk.
    if (!emit_ || path.rfind("decl.methods.", 0) != 0) return "";
    const std::string sub = path.substr(13);
    const std::size_t i = static_cast<std::size_t>(std::stoul(sub));
    const std::size_t dot = sub.find('.');
    if (i >= it_.methods.size() || dot == std::string::npos) return "";
    const ir::Method& m = it_.methods[i];
    const std::string rest = sub.substr(dot + 1);
    if (rest.rfind("params.", 0) != 0) return "";
    const std::string ps = rest.substr(7);
    const std::size_t j = static_cast<std::size_t>(std::stoul(ps));
    if (j >= m.params.size() || ps.find('.') == std::string::npos || ps.substr(ps.find('.') + 1) != "default")
        return "";
    return m.params[j].defaultValue ? emit_(*m.params[j].defaultValue) : "";
}

std::string MethodDeclCtx::get(const std::string& path) const {
    if (path == "decl.kind") {
        switch (m_.kind) {
            case ir::MethodKind::Property: return "property";
            case ir::MethodKind::Operator: return "operator";
            case ir::MethodKind::Method:   return "method";
        }
    }
    if (path == "decl.name")       return m_.name;
    if (path == "decl.opSymbol")   return m_.opSymbol;
    if (path == "decl.owner")      return owner_;
    if (path == "decl.isStatic")   return m_.isStatic ? "true" : "false";
    if (path == "decl.isAsync")    return m_.isAsync ? "true" : "false";
    if (path == "decl.isVirtual")  return m_.isVirtual ? "true" : "false";
    if (path == "decl.isOverride") return m_.isOverride ? "true" : "false";
    if (path == "decl.isIterator") return m_.isIterator ? "true" : "false";
    if (path == "decl.hasSetter")  return m_.pairedSetter ? "true" : "false"; // paired indexer set (#40)
    if (path == "decl.setterValueParam")
        return m_.pairedSetter && !m_.pairedSetter->params.empty() ? m_.pairedSetter->params.back().name : "";
    if (path == "decl.setterBody.count")
        return std::to_string(m_.pairedSetter ? m_.pairedSetter->body.size() : 0);
    if (path == "decl.exprBodied") return m_.exprBodied ? "true" : "false";
    if (path == "decl.body.count") return std::to_string(m_.body.size());
    if (path == "decl.retName")    return m_.returnType.kind == TypeRef::Kind::Named ? m_.returnType.name : "";
    if (path == "decl.returnsUnit")
        return m_.returnType.kind == TypeRef::Kind::Named &&
                       (m_.returnType.name == "unit" || m_.returnType.name.empty())
                   ? "true" : "false";
    if (path == "decl.params.count") return std::to_string(m_.params.size());
    if (path.rfind("decl.params.", 0) == 0) { // decl.params.<i>.{name|hasDefault}
        const std::string sub = path.substr(12);
        const std::size_t i = static_cast<std::size_t>(std::stoul(sub));
        const std::size_t dot = sub.find('.');
        if (i < m_.params.size() && dot != std::string::npos) {
            const std::string rest = sub.substr(dot + 1);
            if (rest == "name")       return m_.params[i].name;
            if (rest == "hasDefault") return m_.params[i].defaultValue ? "true" : "false";
        }
    }
    if (path == "decl.globalRefs.count") return std::to_string(m_.globalRefs.size());
    if (path.rfind("decl.globalRefs.", 0) == 0) {
        const std::size_t gi = static_cast<std::size_t>(std::stoul(path.substr(16)));
        if (gi < m_.globalRefs.size()) return m_.globalRefs[gi];
    }
    return "";
}

std::string MethodDeclCtx::builtin(const std::string& name, const std::vector<std::string>& args) const {
    if (name == "generics") return hooks_.generics(m_.generics);
    if (name == "where")    return hooks_.where(m_.generics);
    if (name == "ident")    return hooks_.ident(args.empty() ? std::string() : args[0]);
    return "";
}

std::string MethodDeclCtx::renderType(const std::string& path) const {
    if (path == "decl.returnType") return hooks_.renderTypeRef(m_.returnType);
    if (path.rfind("decl.params.", 0) == 0) { // decl.params.<i>.type
        const std::string sub = path.substr(12);
        const std::size_t i = static_cast<std::size_t>(std::stoul(sub));
        if (i < m_.params.size() && sub.find('.') != std::string::npos && sub.substr(sub.find('.') + 1) == "type")
            return hooks_.renderTypeRef(m_.params[i].type);
    }
    return "";
}

std::string MethodDeclCtx::emitChild(const std::string& path, const std::string&) const {
    if (!emit_) return "";
    if (path == "decl.exprBody") return m_.exprBody ? emit_(*m_.exprBody) : "";
    if (path.rfind("decl.params.", 0) == 0) { // decl.params.<i>.default
        const std::string sub = path.substr(12);
        const std::size_t i = static_cast<std::size_t>(std::stoul(sub));
        if (i < m_.params.size() && sub.find('.') != std::string::npos &&
            sub.substr(sub.find('.') + 1) == "default")
            return m_.params[i].defaultValue ? emit_(*m_.params[i].defaultValue) : "";
    }
    return "";
}

const std::vector<ir::StmtPtr>* MethodDeclCtx::stmtList(const std::string& path) const {
    if (path == "decl.body") return &m_.body;
    if (path == "decl.setterBody" && m_.pairedSetter) return &m_.pairedSetter->body; // paired indexer set (#40)
    return nullptr;
}

std::string RecordDeclCtx::get(const std::string& path) const {
    if (path == "decl.name")          return r_.name;
    if (path == "decl.fields.count")  return std::to_string(r_.fields.size());
    if (path == "decl.bases.count")   return std::to_string(r_.bases.size());
    if (path == "decl.methods.count") return std::to_string(r_.methods.size());
    if (path.rfind("decl.fields.", 0) == 0) { // decl.fields.<i>.{name|typeIsRecord}
        const std::string sub = path.substr(12);
        const std::size_t i = static_cast<std::size_t>(std::stoul(sub));
        const std::size_t dot = sub.find('.');
        if (i < r_.fields.size() && dot != std::string::npos) {
            const std::string rest = sub.substr(dot + 1);
            if (rest == "name") return r_.fields[i].name;
            if (rest == "typeIsRecord")
                return isRecord_ && isRecord_(r_.fields[i].type) ? "true" : "false";
            if (rest == "typeIsList") { // List<T>/T[] fields compare by REFERENCE in record equality
                                        // (the C# oracle's default field comparer) — targets whose
                                        // native == is value-based (Python list) consult this to emit
                                        // an identity comparison instead.
                const TypeRef& t = r_.fields[i].type;
                return t.kind == TypeRef::Kind::Named && (t.name == "List" || t.name == "Array")
                           ? "true" : "false";
            }
        }
    }
    return "";
}

std::string RecordDeclCtx::builtin(const std::string& name, const std::vector<std::string>& args) const {
    if (name == "generics") return hooks_.generics(r_.generics);
    if (name == "where")    return hooks_.where(r_.generics);
    if (name == "ident")    return hooks_.ident(args.empty() ? std::string() : args[0]);
    if (name == "access")   return hooks_.accessPrefix();
    return "";
}

std::string RecordDeclCtx::renderType(const std::string& path) const {
    if (path.rfind("decl.bases.", 0) == 0) { // decl.bases.<i>
        const std::size_t i = static_cast<std::size_t>(std::stoul(path.substr(11)));
        return i < r_.bases.size() ? hooks_.renderTypeRef(r_.bases[i]) : "";
    }
    if (path.rfind("decl.fields.", 0) == 0) { // decl.fields.<i>.type
        const std::string sub = path.substr(12);
        const std::size_t i = static_cast<std::size_t>(std::stoul(sub));
        if (i < r_.fields.size() && sub.find('.') != std::string::npos && sub.substr(sub.find('.') + 1) == "type")
            return hooks_.renderTypeRef(r_.fields[i].type);
    }
    return "";
}

std::unique_ptr<IrDeclCtx> RecordDeclCtx::memberCtx(const std::string& path, std::size_t index) const {
    if (path == "decl.methods" && index < r_.methods.size())
        return std::make_unique<MethodDeclCtx>(r_.methods[index], r_.name, hooks_, emit_);
    return nullptr;
}

ClassDeclCtx::ClassDeclCtx(const ir::Class& c, const DeclHooks& hooks, EmitFn emit, TypePred isInterface)
    : c_(c), hooks_(hooks), emit_(std::move(emit)) {
    for (std::size_t i = 0; i < c_.fields.size(); ++i)
        if (c_.fields[i].init) (c_.fields[i].isStatic ? staticInit_ : instanceInit_).push_back(i);
    for (std::size_t i = 0; i < c_.bases.size(); ++i) {
        if (isInterface && isInterface(c_.bases[i])) ifaceBases_.push_back(i);
        else extBase_ = static_cast<int>(i);
    }
}

namespace {
// Split a `<list>.<i>.<field>` path tail into its index + trailing field ("" when the path is bare).
bool splitIndexed(const std::string& path, std::size_t prefixLen, std::size_t& idx, std::string& field) {
    const std::string sub = path.substr(prefixLen);
    if (sub.empty() || sub[0] < '0' || sub[0] > '9') return false;
    idx = static_cast<std::size_t>(std::stoul(sub));
    const std::size_t dot = sub.find('.');
    field = dot == std::string::npos ? std::string() : sub.substr(dot + 1);
    return true;
}
} // namespace

std::string ClassDeclCtx::get(const std::string& path) const {
    if (path == "decl.name")             return c_.name;
    if (path == "decl.bases.count")      return std::to_string(c_.bases.size());
    if (path == "decl.methods.count")    return std::to_string(c_.methods.size());
    if (path == "decl.fields.count")     return std::to_string(c_.fields.size());
    if (path == "decl.hasInit")          return c_.hasInit ? "true" : "false";
    if (path == "decl.hasSuper")         return c_.hasSuper ? "true" : "false";
    if (path == "decl.baseHasInit")      return c_.baseHasInit ? "true" : "false";
    if (path == "decl.needsCtor")        return c_.hasInit || !instanceInit_.empty() ? "true" : "false";
    if (path == "decl.initBody.count")   return std::to_string(c_.initBody.size());
    if (path == "decl.superArgs.count")  return std::to_string(c_.superArgs.size());
    if (path == "decl.initParams.count") return std::to_string(c_.initParams.size());
    if (path == "decl.hasExtBase")       return extBase_ >= 0 ? "true" : "false";
    if (path == "decl.ifaceBases.count")         return std::to_string(ifaceBases_.size());
    if (path == "decl.staticInitFields.count")   return std::to_string(staticInit_.size());
    if (path == "decl.instanceInitFields.count") return std::to_string(instanceInit_.size());
    std::size_t i = 0;
    std::string f;
    if (path.rfind("decl.fields.", 0) == 0 && splitIndexed(path, 12, i, f) && i < c_.fields.size()) {
        const ir::ClassField& fld = c_.fields[i];
        if (f == "name")      return fld.name;
        if (f == "isStatic")  return fld.isStatic ? "true" : "false";
        if (f == "isMutable") return fld.isMutable ? "true" : "false";
        if (f == "hasInit")   return fld.init ? "true" : "false";
    }
    if (path.rfind("decl.initParams.", 0) == 0 && splitIndexed(path, 16, i, f) && i < c_.initParams.size()) {
        if (f == "name")       return c_.initParams[i].name;
        if (f == "hasDefault") return c_.initParams[i].defaultValue ? "true" : "false";
    }
    if (path.rfind("decl.staticInitFields.", 0) == 0 && splitIndexed(path, 22, i, f) && i < staticInit_.size()) {
        if (f == "name") return c_.fields[staticInit_[i]].name;
    }
    if (path.rfind("decl.instanceInitFields.", 0) == 0 && splitIndexed(path, 24, i, f) && i < instanceInit_.size()) {
        if (f == "name") return c_.fields[instanceInit_[i]].name;
    }
    return "";
}

std::string ClassDeclCtx::builtin(const std::string& name, const std::vector<std::string>& args) const {
    if (name == "generics") return hooks_.generics(c_.generics);
    if (name == "where")    return hooks_.where(c_.generics);
    if (name == "ident")    return hooks_.ident(args.empty() ? std::string() : args[0]);
    if (name == "access")   return hooks_.accessPrefix();
    return "";
}

std::string ClassDeclCtx::renderType(const std::string& path) const {
    if (path == "decl.extBase")
        return extBase_ >= 0 ? hooks_.renderTypeRef(c_.bases[static_cast<std::size_t>(extBase_)]) : "";
    std::size_t i = 0;
    std::string f;
    if (path.rfind("decl.bases.", 0) == 0 && splitIndexed(path, 11, i, f) && i < c_.bases.size() && f.empty())
        return hooks_.renderTypeRef(c_.bases[i]);
    if (path.rfind("decl.ifaceBases.", 0) == 0 && splitIndexed(path, 16, i, f) && i < ifaceBases_.size() && f.empty())
        return hooks_.renderTypeRef(c_.bases[ifaceBases_[i]]);
    if (path.rfind("decl.fields.", 0) == 0 && splitIndexed(path, 12, i, f) && i < c_.fields.size() && f == "type")
        return hooks_.renderTypeRef(c_.fields[i].type);
    if (path.rfind("decl.initParams.", 0) == 0 && splitIndexed(path, 16, i, f) && i < c_.initParams.size() && f == "type")
        return hooks_.renderTypeRef(c_.initParams[i].type);
    return "";
}

std::string ClassDeclCtx::emitChild(const std::string& path, const std::string&) const {
    if (!emit_) return "";
    std::size_t i = 0;
    std::string f;
    if (path.rfind("decl.superArgs.", 0) == 0 && splitIndexed(path, 15, i, f) && i < c_.superArgs.size() && f.empty())
        return emit_(*c_.superArgs[i]);
    if (path.rfind("decl.fields.", 0) == 0 && splitIndexed(path, 12, i, f) && i < c_.fields.size() && f == "init")
        return c_.fields[i].init ? emit_(*c_.fields[i].init) : "";
    if (path.rfind("decl.initParams.", 0) == 0 && splitIndexed(path, 16, i, f) && i < c_.initParams.size() && f == "default")
        return c_.initParams[i].defaultValue ? emit_(*c_.initParams[i].defaultValue) : "";
    if (path.rfind("decl.staticInitFields.", 0) == 0 && splitIndexed(path, 22, i, f) && i < staticInit_.size() && f == "init")
        return emit_(*c_.fields[staticInit_[i]].init);
    if (path.rfind("decl.instanceInitFields.", 0) == 0 && splitIndexed(path, 24, i, f) && i < instanceInit_.size() && f == "init")
        return emit_(*c_.fields[instanceInit_[i]].init);
    return "";
}

const std::vector<ir::StmtPtr>* ClassDeclCtx::stmtList(const std::string& path) const {
    return path == "decl.initBody" ? &c_.initBody : nullptr;
}

std::unique_ptr<IrDeclCtx> ClassDeclCtx::memberCtx(const std::string& path, std::size_t index) const {
    if (path == "decl.methods" && index < c_.methods.size())
        return std::make_unique<MethodDeclCtx>(c_.methods[index], c_.name, hooks_, emit_);
    return nullptr;
}

std::string FnDeclCtx::get(const std::string& path) const {
    if (path == "decl.name")        return f_.name;
    if (path == "decl.mangledName") return f_.mangledName;
    if (path == "decl.emitName")    return f_.mangledName.empty() ? f_.name : f_.mangledName;
    // A std/core/lib prelude function (originModule "<prelude>"), emitted into EVERY module. On a target
    // with one global function namespace (PHP), that redeclares it across `require`d files — the plugin
    // guards such a fn (e.g. `function_exists`). Inert for namespaced targets (C#/TS/Python) that don't read it.
    if (path == "decl.isPrelude")   return f_.originModule == "<prelude>" ? "true" : "false";
    if (path == "decl.isAsync")     return f_.isAsync ? "true" : "false";
    if (path == "decl.isIterator")  return f_.isIterator ? "true" : "false";
    if (path == "decl.exprBodied")  return f_.exprBodied ? "true" : "false";
    if (path == "decl.body.count")  return std::to_string(f_.body.size());
    if (path == "decl.retName")     return f_.returnType.kind == TypeRef::Kind::Named ? f_.returnType.name : "";
    if (path == "decl.returnsUnit")
        return f_.returnType.kind == TypeRef::Kind::Named &&
                       (f_.returnType.name == "unit" || f_.returnType.name.empty())
                   ? "true" : "false";
    if (path == "decl.params.count")     return std::to_string(f_.params.size());
    if (path == "decl.paramsTail.count") return std::to_string(f_.params.empty() ? 0 : f_.params.size() - 1);
    std::size_t i = 0;
    std::string f;
    if (path.rfind("decl.params.", 0) == 0 && splitIndexed(path, 12, i, f) && i < f_.params.size()) {
        if (f == "name")       return f_.params[i].name;
        if (f == "hasDefault") return f_.params[i].defaultValue ? "true" : "false";
    }
    if (path.rfind("decl.paramsTail.", 0) == 0 && splitIndexed(path, 16, i, f) && i + 1 < f_.params.size()) {
        if (f == "name")       return f_.params[i + 1].name;
        if (f == "hasDefault") return f_.params[i + 1].defaultValue ? "true" : "false";
    }
    if (path == "decl.globalRefs.count") return std::to_string(f_.globalRefs.size());
    if (path.rfind("decl.globalRefs.", 0) == 0) {
        const std::size_t gi = static_cast<std::size_t>(std::stoul(path.substr(16)));
        if (gi < f_.globalRefs.size()) return f_.globalRefs[gi];
    }
    return "";
}

std::string FnDeclCtx::builtin(const std::string& name, const std::vector<std::string>& args) const {
    if (name == "generics") return hooks_.generics(f_.generics);
    if (name == "where")    return hooks_.where(f_.generics);
    if (name == "ident")    return hooks_.ident(args.empty() ? std::string() : args[0]);
    if (name == "identFn")  return hooks_.identFn(args.empty() ? std::string() : args[0]);
    if (name == "mangle")   return hooks_.mangle(args.empty() ? std::string() : args[0]);
    return "";
}

std::string FnDeclCtx::renderType(const std::string& path) const {
    if (path == "decl.returnType") return hooks_.renderTypeRef(f_.returnType);
    std::size_t i = 0;
    std::string f;
    if (path.rfind("decl.params.", 0) == 0 && splitIndexed(path, 12, i, f) && i < f_.params.size() && f == "type")
        return hooks_.renderTypeRef(f_.params[i].type);
    if (path.rfind("decl.paramsTail.", 0) == 0 && splitIndexed(path, 16, i, f) && i + 1 < f_.params.size() && f == "type")
        return hooks_.renderTypeRef(f_.params[i + 1].type);
    return "";
}

std::string FnDeclCtx::emitChild(const std::string& path, const std::string&) const {
    if (!emit_) return "";
    if (path == "decl.exprBody") return f_.exprBody ? emit_(*f_.exprBody) : "";
    std::size_t i = 0;
    std::string f;
    if (path.rfind("decl.params.", 0) == 0 && splitIndexed(path, 12, i, f) && i < f_.params.size() && f == "default")
        return f_.params[i].defaultValue ? emit_(*f_.params[i].defaultValue) : "";
    if (path.rfind("decl.paramsTail.", 0) == 0 && splitIndexed(path, 16, i, f) && i + 1 < f_.params.size() && f == "default")
        return f_.params[i + 1].defaultValue ? emit_(*f_.params[i + 1].defaultValue) : "";
    return "";
}

const std::vector<ir::StmtPtr>* FnDeclCtx::stmtList(const std::string& path) const {
    return path == "decl.body" ? &f_.body : nullptr;
}

ModuleDeclCtx::ModuleDeclCtx(const ir::Module& m, const DeclHooks& hooks, EmitFn emit,
                             const std::string& target, TypePred isRecordType, TypePred isInterface)
    : m_(m), hooks_(hooks), emit_(std::move(emit)),
      isRecord_(std::move(isRecordType)), isInterface_(std::move(isInterface)) {
    for (std::size_t i = 0; i < m_.functions.size(); ++i) {
        const ir::Function& f = m_.functions[i];
        if (f.actualTarget.empty() || f.actualTarget == target) fns_.push_back(i);
        if (entry_ < 0 && f.isEntry) entry_ = static_cast<int>(i);
    }
}

std::string ModuleDeclCtx::get(const std::string& path) const {
    if (path == "module.enums.count")      return std::to_string(m_.enums.size());
    if (path == "module.unions.count")     return std::to_string(m_.unions.size());
    if (path == "module.interfaces.count") return std::to_string(m_.interfaces.size());
    if (path == "module.records.count")    return std::to_string(m_.records.size());
    if (path == "module.classes.count")    return std::to_string(m_.classes.size());
    if (path == "module.extensions.count") return std::to_string(m_.extensions.size());
    if (path == "module.functions.count")  return std::to_string(fns_.size());
    if (path == "module.globals.count")    return std::to_string(m_.globals.size());
    if (path == "module.imports.count")    return std::to_string(m_.imports.size());
    if (path == "module.linked")           return m_.linked ? "true" : "false";
    if (path == "module.hasEntry")         return entry_ >= 0 ? "true" : "false";
    if (entry_ >= 0) {
        const ir::Function& e = m_.functions[static_cast<std::size_t>(entry_)];
        if (path == "module.entry.isAsync")     return e.isAsync ? "true" : "false";
        if (path == "module.entry.mangledName") return e.mangledName;
        if (path == "module.entry.name")        return e.name;
    }
    std::size_t i = 0;
    std::string f;
    if (path.rfind("module.globals.", 0) == 0 && splitIndexed(path, 15, i, f) && i < m_.globals.size()) {
        if (f == "name")    return m_.globals[i].name;
        if (f == "isConst") return m_.globals[i].isConst ? "true" : "false";
        if (f == "hasInit") return m_.globals[i].init ? "true" : "false";
    }
    if (path.rfind("module.imports.", 0) == 0 && splitIndexed(path, 15, i, f) && i < m_.imports.size()) {
        if (f == "path")        return m_.imports[i].path;
        if (f == "names")       return m_.imports[i].names;
        if (f == "isNamespace") return m_.imports[i].isNamespace ? "true" : "false";
        if (f == "ns")          return m_.imports[i].ns;
    }
    return "";
}

std::string ModuleDeclCtx::builtin(const std::string& name, const std::vector<std::string>& args) const {
    if (name == "ident")  return hooks_.ident(args.empty() ? std::string() : args[0]);
    if (name == "mangle") return hooks_.mangle(args.empty() ? std::string() : args[0]);
    if (name == "access") return hooks_.accessPrefix();
    return "";
}

std::string ModuleDeclCtx::renderType(const std::string& path) const {
    std::size_t i = 0;
    std::string f;
    if (path.rfind("module.globals.", 0) == 0 && splitIndexed(path, 15, i, f) && i < m_.globals.size() && f == "type")
        return hooks_.renderTypeRef(m_.globals[i].type);
    return "";
}

std::string ModuleDeclCtx::emitChild(const std::string& path, const std::string&) const {
    if (!emit_) return "";
    std::size_t i = 0;
    std::string f;
    if (path.rfind("module.globals.", 0) == 0 && splitIndexed(path, 15, i, f) && i < m_.globals.size() && f == "init")
        return m_.globals[i].init ? emit_(*m_.globals[i].init) : "";
    return "";
}

std::unique_ptr<IrDeclCtx> ModuleDeclCtx::memberCtx(const std::string& path, std::size_t index) const {
    if (path == "module.enums" && index < m_.enums.size())
        return std::make_unique<EnumDeclCtx>(m_.enums[index], hooks_);
    if (path == "module.unions" && index < m_.unions.size())
        return std::make_unique<UnionDeclCtx>(m_.unions[index], hooks_);
    if (path == "module.interfaces" && index < m_.interfaces.size())
        return std::make_unique<InterfaceDeclCtx>(m_.interfaces[index], hooks_, emit_);
    if (path == "module.records" && index < m_.records.size())
        return std::make_unique<RecordDeclCtx>(m_.records[index], hooks_, emit_, isRecord_);
    if (path == "module.classes" && index < m_.classes.size())
        return std::make_unique<ClassDeclCtx>(m_.classes[index], hooks_, emit_, isInterface_);
    if (path == "module.extensions" && index < m_.extensions.size())
        return std::make_unique<FnDeclCtx>(m_.extensions[index], hooks_, emit_);
    if (path == "module.functions" && index < fns_.size())
        return std::make_unique<FnDeclCtx>(m_.functions[fns_[index]], hooks_, emit_);
    return nullptr;
}

std::string StmtCtx::get(const std::string& path) const {
    if (s_.kind == ir::StmtKind::LocalFunc) { // P25 §4.18: a hoisted nested def (Python block-lambda lowering)
        const auto& lf = static_cast<const ir::LocalFunc&>(s_);
        if (path == "stmt.name")            return lf.name;
        if (path == "stmt.params.count")    return std::to_string(lf.params.size());
        if (path == "stmt.body.count")      return std::to_string(lf.body.size());
        if (path == "stmt.nonlocals.count") return std::to_string(lf.nonlocals.size());
        if (path.rfind("stmt.params.", 0) == 0) {
            std::size_t i = 0;
            std::string f;
            if (splitIndexed(path, 12, i, f) && i < lf.params.size() && f == "name") return lf.params[i].name;
        }
        if (path.rfind("stmt.nonlocals.", 0) == 0) {
            const std::size_t i = static_cast<std::size_t>(std::stoul(path.substr(15)));
            if (i < lf.nonlocals.size()) return lf.nonlocals[i];
        }
    }
    if (s_.kind == ir::StmtKind::IndexAssign) {
        const auto& ia = static_cast<const ir::IndexAssign&>(s_);
        if (path == "stmt.indices.count") return std::to_string(ia.indices.size());
    }
    if (s_.kind == ir::StmtKind::For) {
        const auto& f = static_cast<const ir::For&>(s_);
        if (path == "stmt.isRange")   return f.isRange ? "true" : "false";
        if (path == "stmt.inclusive") return f.inclusive ? "true" : "false";
        if (path == "stmt.binding")   return f.binding;
        if (path == "stmt.body.count") return std::to_string(f.body.size());
        if (path == "stmt.tupleBindings.count") return std::to_string(f.tupleBindings.size());
        if (path.rfind("stmt.tupleBindings.", 0) == 0) {
            const std::size_t i = static_cast<std::size_t>(std::stoul(path.substr(19)));
            if (i < f.tupleBindings.size()) return f.tupleBindings[i];
        }
    }
    if (s_.kind == ir::StmtKind::Try) {
        const auto& t = static_cast<const ir::Try&>(s_);
        if (path == "stmt.body.count")       return std::to_string(t.body.size());
        if (path == "stmt.hasFinally")       return t.hasFinally ? "true" : "false";
        if (path == "stmt.finallyBody.count") return std::to_string(t.finallyBody.size());
        if (path == "stmt.catches.count")    return std::to_string(t.catches.size());
        if (path == "stmt.hasCatchAll") { // an untyped, unguarded catch handles everything
            for (const auto& c : t.catches)
                if (c.type.name.empty() && !c.guard) return "true";
            return "false";
        }
        if (path == "stmt.catchesHaveGuard") { // any guarded catch forces the fall-through dispatch shape
            for (const auto& c : t.catches)
                if (c.guard) return "true";
            return "false";
        }
        std::size_t i = 0;
        std::string f;
        if (path.rfind("stmt.catches.", 0) == 0 && splitIndexed(path, 13, i, f) && i < t.catches.size()) {
            const auto& c = t.catches[i];
            if (f == "hasType")    return c.type.name.empty() ? "false" : "true";
            if (f == "hasBinding") return c.binding.empty() ? "false" : "true";
            if (f == "binding")    return c.binding;
            if (f == "hasGuard")   return c.guard ? "true" : "false";
            if (f == "body.count") return std::to_string(c.body.size());
        }
    }
    return "";
}

std::string StmtCtx::builtin(const std::string& name, const std::vector<std::string>& args) const {
    if (name == "ident")  return hooks_.ident(args.empty() ? std::string() : args[0]);
    if (name == "mangle") return hooks_.mangle(args.empty() ? std::string() : args[0]);
    return "";
}

std::string StmtCtx::renderType(const std::string& path) const {
    if (s_.kind == ir::StmtKind::Try && path.rfind("stmt.catches.", 0) == 0) { // stmt.catches.<i>.type
        const auto& t = static_cast<const ir::Try&>(s_);
        std::size_t i = 0;
        std::string f;
        if (splitIndexed(path, 13, i, f) && i < t.catches.size() && f == "type")
            return hooks_.renderTypeRef(t.catches[i].type);
    }
    return "";
}

std::string StmtCtx::emitChild(const std::string& path, const std::string&) const {
    if (!emit_) return "";
    if (s_.kind == ir::StmtKind::IndexAssign) {
        const auto& ia = static_cast<const ir::IndexAssign&>(s_);
        if (path == "stmt.receiver" && ia.receiver) return emit_(*ia.receiver);
        if (path == "stmt.value" && ia.value)       return emit_(*ia.value);
        if (path.rfind("stmt.indices.", 0) == 0) {
            const std::size_t i = static_cast<std::size_t>(std::stoul(path.substr(13)));
            if (i < ia.indices.size()) return emit_(*ia.indices[i]);
        }
    }
    if (s_.kind == ir::StmtKind::For) {
        const auto& f = static_cast<const ir::For&>(s_);
        if (path == "stmt.rangeStart" && f.rangeStart) return emit_(*f.rangeStart);
        if (path == "stmt.rangeEnd" && f.rangeEnd)     return emit_(*f.rangeEnd);
        if (path == "stmt.iterable" && f.iterable)     return emit_(*f.iterable);
    }
    if (s_.kind == ir::StmtKind::Try && path.rfind("stmt.catches.", 0) == 0) { // stmt.catches.<i>.guard
        const auto& t = static_cast<const ir::Try&>(s_);
        std::size_t i = 0;
        std::string f;
        if (splitIndexed(path, 13, i, f) && i < t.catches.size() && f == "guard" && t.catches[i].guard)
            return emit_(*t.catches[i].guard);
    }
    return "";
}

const std::vector<ir::StmtPtr>* StmtCtx::stmtList(const std::string& path) const {
    if (s_.kind == ir::StmtKind::LocalFunc && path == "stmt.body") // P25 §4.18
        return &static_cast<const ir::LocalFunc&>(s_).body;
    if (s_.kind == ir::StmtKind::For && path == "stmt.body")
        return &static_cast<const ir::For&>(s_).body;
    if (s_.kind == ir::StmtKind::Try) {
        const auto& t = static_cast<const ir::Try&>(s_);
        if (path == "stmt.body")        return &t.body;
        if (path == "stmt.finallyBody") return &t.finallyBody;
        if (path.rfind("stmt.catches.", 0) == 0) { // stmt.catches.<i>.body
            std::size_t i = 0;
            std::string f;
            if (splitIndexed(path, 13, i, f) && i < t.catches.size() && f == "body")
                return &t.catches[i].body;
        }
    }
    return nullptr;
}

void EmitterBase::runDeclRule(const engine::Rule& r, const engine::EvalContext& ctx, const IrDeclCtx& root,
                              const engine::RuleTable* helpers) {
    using K = engine::Rule::Kind;
    switch (r.kind) {
        case K::Line:
            line(engine::evalRule(r.parts[0], ctx, helpers));
            return;
        case K::Block: {
            openBlock(engine::evalRule(r.parts[0], ctx, helpers));
            ++indent_;
            for (std::size_t i = 1; i < r.parts.size(); ++i) runDeclRule(r.parts[i], ctx, root, helpers);
            --indent_;
            closeBlock();
            return;
        }
        case K::MapDecl: {
            int n = 0;
            for (char c : ctx.get(r.s + ".count")) { if (c < '0' || c > '9') { n = 0; break; } n = n * 10 + (c - '0'); }
            for (int i = 0; i < n; ++i) {
                engine::ItemCtx item(ctx, ctx.resolvePath(r.s) + "." + std::to_string(i), i);
                runDeclRule(r.parts[0], item, root, helpers);
            }
            return;
        }
        case K::Stmts: {
            if (const std::vector<ir::StmtPtr>* body = root.stmtList(ctx.resolvePath(r.s)))
                for (const auto& s : *body) emitStmt(*s);
            return;
        }
        case K::Seq:
            for (const auto& p : r.parts) runDeclRule(p, ctx, root, helpers);
            return;
        case K::Indent:
            ++indent_;
            for (const auto& p : r.parts) runDeclRule(p, ctx, root, helpers);
            --indent_;
            return;
        case K::MapMembers: {
            if (!helpers) return;
            auto it = helpers->find(r.s2);
            if (it == helpers->end()) return;
            int n = 0;
            for (char c : ctx.get(r.s + ".count")) { if (c < '0' || c > '9') { n = 0; break; } n = n * 10 + (c - '0'); }
            for (int i = 0; i < n; ++i)
                if (auto sub = root.memberCtx(ctx.resolvePath(r.s), static_cast<std::size_t>(i)))
                    runDeclRule(it->second, *sub, *sub, helpers);
            return;
        }
        case K::Case:
            for (const auto& arm : r.arms)
                if (engine::evalTest(arm.first, ctx)) { runDeclRule(arm.second, ctx, root, helpers); return; }
            if (!r.elseBody.empty()) runDeclRule(r.elseBody[0], ctx, root, helpers);
            return;
        case K::Call: {
            if (!helpers) return;
            auto it = helpers->find(r.s);
            if (it != helpers->end()) runDeclRule(it->second, ctx, root, helpers);
            return;
        }
        default: // a string-flavor rule at declaration position renders as one line
            line(engine::evalRule(r, ctx, helpers));
            return;
    }
}

// ---- InterpretedEmitter: the one data-parameterized backend (P19 slice 7d) ------------------------------

namespace {

// The rule-table key for an expression kind — one shared map (the three per-target switches were
// identical). "" routes to the fixed C++ path (Bound — the FFI template substitution).
const char* exprRuleKey(ir::ExprKind k) {
    switch (k) {
        case ir::ExprKind::Int:      return "Int";
        case ir::ExprKind::Float:    return "Float";
        case ir::ExprKind::Bool:     return "Bool";
        case ir::ExprKind::Null:     return "Null";
        case ir::ExprKind::Str:      return "Str";
        case ir::ExprKind::Char:     return "Char";
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
        case ir::ExprKind::Lambda:     return "Lambda";
        case ir::ExprKind::Binary:     return "Binary";
        default:                       return "";
    }
}

// The expression context: IrExprCtx whose type renderer routes back to the owning emitter.
class GenericExprCtx : public IrExprCtx {
public:
    GenericExprCtx(const ir::Expr& e, const BackendSpec& spec, EmitFn emit, InlineFn inlineBlock,
                   int* fresh, std::unordered_set<std::string>* preludeKeys, InterpretedEmitter& owner)
        : IrExprCtx(e, spec, std::move(emit), std::move(inlineBlock), fresh, preludeKeys), owner_(owner) {}

protected:
    std::string renderTypeRef(const TypeRef& t) const override { return owner_.renderType(t); }

private:
    InterpretedEmitter& owner_;
};

// The type context: extern-template pick from the module's registry (already target-resolved by
// lowering); recursion routes back to the owning emitter.
class GenericTypeCtx : public TypeRefCtx {
public:
    GenericTypeCtx(const TypeRef& t, const BackendSpec& spec,
                   const std::unordered_map<std::string, const ir::ExternType*>& externs,
                   InterpretedEmitter& owner)
        : TypeRefCtx(t, spec), externs_(externs), owner_(owner) {}

protected:
    std::string externTemplate() const override {
        if (t_.kind != TypeRef::Kind::Named) return "";
        auto it = externs_.find(t_.name);
        return it != externs_.end() ? it->second->typeTmpl : std::string();
    }
    std::string renderTypeRef(const TypeRef& t) const override { return owner_.renderType(t); }

private:
    const std::unordered_map<std::string, const ir::ExternType*>& externs_;
    InterpretedEmitter& owner_;
};

} // namespace

InterpretedEmitter::InterpretedEmitter(SpecFn spec, const engine::RuleTable& rules)
    : specFn_(spec), rules_(rules), hooks_(spec, *this) {}

std::string InterpretedEmitter::renderType(const TypeRef& t) {
    GenericTypeCtx ctx(t, spec(), externMap_, *this);
    return engine::evalRule(rules_.at("Type"), ctx, &rules_);
}

std::string InterpretedEmitter::emitExpr(const ir::Expr& e) {
    struct Depth { // see exprDepth_ — bail with one clean error, never a stack overflow (G45).
                   // 400 levels x ~30KB/frame (MSVC Debug worst case) stays inside the 16MB reserve.
        int& d;
        explicit Depth(int& depth) : d(++depth) {
            if (d > 400) throw std::runtime_error(
                "expression is too deeply nested to emit (more than 400 levels) — split it into locals");
        }
        ~Depth() { --d; }
    } depthGuard(exprDepth_);
    if (const char* key = exprRuleKey(e.kind); key[0] != '\0') {
        auto it = rules_.find(key);
        if (it != rules_.end()) {
            GenericExprCtx ctx(e, spec(), [this](const ir::Expr& c) { return emitExpr(c); },
                               [this](const std::vector<ir::StmtPtr>& b) { return inlineBlock(b); },
                               &tmp_, &requires_, *this);
            return engine::evalRule(it->second, ctx, &rules_);
        }
    }
    if (e.kind == ir::ExprKind::Bound) // the FFI template substitution — fixed machinery over plugin data
        return substBoundTemplate(static_cast<const ir::Bound&>(e).tmpl, static_cast<const ir::Bound&>(e));
    return "";
}

std::string InterpretedEmitter::emit(const ir::Module& m) {
    out_.clear();
    indent_ = 0;
    externMap_.clear();
    tmp_ = 0;
    requires_.clear();
    hooks_.access = m.access; // per-emit accessibility for the `{"fn":"access"}` decl builtin (§ access modifier)
    for (const auto& et : m.externTypes) externMap_[et.name] = &et;
    // The two module facts record/class rules read: which named types are records (TS's structural-equals
    // dispatch) and which bases are interfaces (TS's extends/implements split). Computed for every target;
    // a target whose rules never read them is unaffected.
    std::unordered_set<std::string> recordNames, interfaceNames;
    for (const auto& r : m.records) recordNames.insert(r.name);
    for (const auto& i : m.interfaces) interfaceNames.insert(i.name);
    ModuleDeclCtx ctx(m, hooks_, [this](const ir::Expr& e) { return emitExpr(e); }, spec().name,
                      [&recordNames](const TypeRef& t) {
                          return t.kind == TypeRef::Kind::Named && recordNames.count(t.name) != 0;
                      },
                      [&interfaceNames](const TypeRef& t) {
                          return t.kind == TypeRef::Kind::Named && interfaceNames.count(t.name) != 0;
                      });
    runDeclRule(rules_.at("Program"), ctx, ctx, &rules_);
    for (const auto& fn : m.functions)
        if (fn.isEntry) { // an async entry needs the target's event-loop prelude, when it declares one
            if (fn.isAsync) requires_.insert("asyncEntry");
            break;
        }
    std::vector<std::string> keys; // ascending order, each prepended => the last key ends up outermost
    for (const auto& k : requires_)
        if (spec().preludes.count(k)) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    for (const auto& k : keys) out_ = spec().preludes.at(k) + out_;
    return out_;
}

void EmitterBase::line(const std::string& s) {
    out_.append(static_cast<std::size_t>(indent_) * 4, ' ');
    out_ += s;
    out_ += '\n';
}

void EmitterBase::blockBody(const std::vector<ir::StmtPtr>& body) {
    ++indent_;
    if (body.empty() && spec().blockStyle == BlockStyle::ColonIndent) line("pass"); // Python has no empty block
    for (const auto& s : body) emitStmt(*s);
    --indent_;
}

void EmitterBase::openBlock(const std::string& head) {
    switch (spec().blockStyle) {
        case BlockStyle::BracesAllman: line(head); line("{"); break;
        case BlockStyle::BracesKnR:    line(head + " {");      break;
        case BlockStyle::ColonIndent:  line(head + ":");       break;
    }
}

void EmitterBase::closeBlock() {
    if (spec().blockStyle != BlockStyle::ColonIndent) line("}"); // indentation targets have no closer
}

void EmitterBase::headBlock(const std::string& head, const std::vector<ir::StmtPtr>& body) {
    openBlock(head);
    blockBody(body);
    closeBlock();
}

std::string EmitterBase::substBoundTemplate(const std::string& tmpl, const ir::Bound& b) {
    std::string out;
    for (std::size_t i = 0; i < tmpl.size();) {
        if (tmpl.compare(i, 5, "$this") == 0) { if (b.receiver) out += emitExpr(*b.receiver); i += 5; }
        else if (tmpl.compare(i, 2, "$T") == 0) { out += renderType(b.type); i += 2; } // ctor: the mapped type
        else if (tmpl[i] == '$' && i + 1 < tmpl.size() && std::isdigit(static_cast<unsigned char>(tmpl[i + 1]))) {
            std::size_t idx = static_cast<std::size_t>(tmpl[i + 1] - '0');
            if (idx < b.args.size()) out += emitExpr(*b.args[idx]);
            i += 2;
        } else out += tmpl[i++];
    }
    return out;
}

std::string EmitterBase::inlineBlock(const std::vector<ir::StmtPtr>& body) {
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

// A bound template that references neither the receiver (`$this`), the mapped type (`$T`), nor any argument
// (`$0`..`$9`) spells a fixed, type-blind value — e.g. TS/Python/PHP `List.init` is `[]`. A construction with
// such a template cannot convey its own type, so the enclosing declaration must carry it (see StmtKind::Let).
static bool boundTemplateIsConstant(const std::string& tmpl) {
    for (std::size_t i = 0; i + 1 < tmpl.size(); ++i) {
        if (tmpl[i] != '$') continue;
        const char c = tmpl[i + 1];
        if (c == 'T' || std::isdigit(static_cast<unsigned char>(c)) || tmpl.compare(i, 5, "$this") == 0)
            return false;
    }
    return true;
}

// Structural TypeRef identity, for "does the declared type add information over the initializer's own
// type" (StmtKind::Let). Everything that renders participates: kind, name, nullability, args, ret.
static bool sameTypeRef(const TypeRef& a, const TypeRef& b) {
    if (a.kind != b.kind || a.name != b.name || a.nullable != b.nullable ||
        a.args.size() != b.args.size() || a.ret.size() != b.ret.size())
        return false;
    for (std::size_t i = 0; i < a.args.size(); ++i)
        if (!sameTypeRef(a.args[i], b.args[i])) return false;
    for (std::size_t i = 0; i < a.ret.size(); ++i)
        if (!sameTypeRef(a.ret[i], b.ret[i])) return false;
    return true;
}

void EmitterBase::emitStmt(const ir::Stmt& s) {
    switch (s.kind) {
        case ir::StmtKind::Assign: {
            const auto& a = static_cast<const ir::Assign&>(s);
            line(emitExpr(*a.target) + " " + a.op + " " + emitExpr(*a.value) + spec().stmtEnd);
            return;
        }
        case ir::StmtKind::ExprStmt:
            line(emitExpr(*static_cast<const ir::ExprStmt&>(s).expr) + spec().stmtEnd);
            return;
        case ir::StmtKind::Let: {
            const auto& l = static_cast<const ir::Let&>(s);
            // Some initializers give the target no type to reconstruct, so the DECLARATION must carry it
            // (issue #27; C# `var x = null` is also CS0815, issue #9). Two such forms:
            //   • a bare `null` (no underlying type), and
            //   • a construction whose bound template is a bare constant — e.g. TS `List.init` is `[]`, which
            //     spells no element type (C#'s `new $T()` carries `$T`, so it is NOT constant and won't fire).
            // For these, emit the declared type via localDeclTyped. Only C#/TS declare a `localDeclTyped` row —
            // Python/PHP get "" back and fall through to `localDecl` (dynamic, byte-unchanged). An empty list
            // *literal* is handled separately by the ListLit rule's ` as T[]` suffix, so it is not listed here.
            std::string decl = localDecl(l.name, l.isMutable);
            const bool uninferable = l.init && (l.init->kind == ir::ExprKind::Null ||
                (l.init->kind == ir::ExprKind::Bound &&
                 boundTemplateIsConstant(static_cast<const ir::Bound&>(*l.init).tmpl)));
            // An EXPLICIT source annotation always emits — the checker often stamps the initializer's
            // type FROM the annotation (bare union cases, contextually-typed list literals), so the
            // annotation's information is invisible to a declared-vs-initializer comparison, yet the
            // target needs it: `var n: i32? = 0` must not become C# `var n = 0` (CS0037 on the later
            // `n = null`, issue #47's dropped-annotation half), and `let e: Box<i64> = Empty` /
            // `var xs: List<Tree> = [...]` need the contextual type in TS or literal `tag`s widen to
            // string. declDiffers additionally catches non-explicit divergence, belt-and-braces.
            const bool declDiffers = l.init && !l.type.absent() && !sameTypeRef(l.type, l.init->type);
            if (uninferable || declDiffers || (l.declExplicit && !l.type.absent())) {
                std::string ty = renderType(l.type);
                if (!ty.empty()) {
                    std::string typed = localDeclTyped(l.name, l.isMutable, ty);
                    if (!typed.empty()) decl = typed;
                }
            }
            line(decl + " = " + emitExpr(*l.init) + spec().stmtEnd);
            return;
        }

        case ir::StmtKind::Yield: {
            const auto& y = static_cast<const ir::Yield&>(s);
            std::string v = y.value ? emitExpr(*y.value) : std::string{};
            line(yieldStmt(v, static_cast<bool>(y.value))); // hook owns its own terminator
            return;
        }
        case ir::StmtKind::Throw: { // `throw v`/`raise v` — only the keyword differs (throwKeyword hook)
            const auto& t = static_cast<const ir::Throw&>(s);
            if (t.value) line(std::string(spec().throwKeyword) + " " + emitExpr(*t.value) + spec().stmtEnd);
            else line(rethrowStmt());
            return;
        }
        case ir::StmtKind::Use: { // `<decl> = init; try { body } finally { binding.dispose(); }`
            const auto& u = static_cast<const ir::Use&>(s);
            line(localDecl(u.binding, false) + " = " + emitExpr(*u.init) + spec().stmtEnd);
            openBlock("try");
            blockBody(u.body);
            switch (spec().blockStyle) { // close the try and open the finally — only the join differs per style
                case BlockStyle::BracesKnR:    line("} finally {");          break;
                case BlockStyle::BracesAllman: closeBlock(); openBlock("finally"); break;
                case BlockStyle::ColonIndent:  openBlock("finally");          break;
            }
            ++indent_;
            // Emit the receiver via the target's own Var rule ($x on PHP, x elsewhere) and the target's
            // member-access operator (`->` on PHP, `.` elsewhere) — not a hardcoded `.dispose()`. Byte-
            // identical for C#/TS/Python (Var -> `x`, memberOp defaults to `.`).
            ir::Var recv(u.pos, u.type, u.binding);
            line(emitExpr(recv) + spec().memberOp + "dispose()" + spec().stmtEnd);
            --indent_;
            closeBlock();
            return;
        }
        case ir::StmtKind::Return: {
            const auto& r = static_cast<const ir::Return&>(s);
            line(r.value ? "return " + emitExpr(*r.value) + spec().stmtEnd : std::string("return") + spec().stmtEnd);
            return;
        }
        case ir::StmtKind::Break:    line(std::string("break") + spec().stmtEnd);    return; // identical spelling, all targets
        case ir::StmtKind::Continue: line(std::string("continue") + spec().stmtEnd); return;
        case ir::StmtKind::While: { // `while (cond)` head is identical across targets; block form via headBlock
            const auto& w = static_cast<const ir::While&>(s);
            if (w.isDoWhile) {
                const std::string cond = emitExpr(*w.cond);
                if (spec().blockStyle == BlockStyle::ColonIndent) {
                    // Python has no do-while; a while-true emulation whose condition re-runs on the loop
                    // header, so a `continue` in the body re-tests it: a fresh flag makes the first pass
                    // unconditional, then every re-entry (incl. after continue) checks `cond` (#39a).
                    const std::string flag = "__dw" + std::to_string(doWhileSeq_++);
                    line(flag + " = " + std::string(spec().trueLit));
                    openBlock("while " + flag + " or (" + cond + ")");
                    ++indent_;
                    line(flag + " = " + std::string(spec().falseLit));
                    for (const auto& st : w.body) emitStmt(*st);
                    --indent_;
                    closeBlock();
                } else {
                    openBlock("do");
                    blockBody(w.body);
                    line("} while (" + cond + ")" + spec().stmtEnd);
                }
                return;
            }
            headBlock("while (" + emitExpr(*w.cond) + ")", w.body);
            return;
        }
        case ir::StmtKind::If: { // `if (cond)` head is identical; the else arm differs per block style
            const auto& i = static_cast<const ir::If&>(s);
            openBlock("if (" + emitExpr(*i.cond) + ")");
            blockBody(i.thenBody);
            if (!i.hasElse) { closeBlock(); return; }
            switch (spec().blockStyle) {
                case BlockStyle::BracesKnR:    line("} else {"); blockBody(i.elseBody); line("}"); break;
                case BlockStyle::BracesAllman: closeBlock(); openBlock("else"); blockBody(i.elseBody); closeBlock(); break;
                case BlockStyle::ColonIndent:  line("else:"); blockBody(i.elseBody); break;
            }
            return;
        }
        default: {
            // A per-kind statement RULE, when the backend's table declares one (P19 slice 7): the shape
            // is data; only a kind with no rule falls back to the imperative emitStmtTarget.
            const char* key = s.kind == ir::StmtKind::For ? "ForStmt"
                            : s.kind == ir::StmtKind::Try ? "TryStmt"
                            : s.kind == ir::StmtKind::IndexAssign ? "IndexAssign"
                            : s.kind == ir::StmtKind::LocalFunc ? "LocalFunc" : nullptr;
            if (key) {
                if (const engine::RuleTable* rules = ruleTable()) {
                    if (const DeclHooks* hooks = declHooks()) {
                        auto it = rules->find(key);
                        if (it != rules->end()) {
                            StmtCtx ctx(s, *hooks, [this](const ir::Expr& e) { return emitExpr(e); });
                            runDeclRule(it->second, ctx, ctx, rules);
                            return;
                        }
                    }
                }
            }
            emitStmtTarget(s);
            return;
        }
    }
}

} // namespace mintplayer::polyglot
