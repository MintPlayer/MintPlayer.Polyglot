#include "mintplayer/polyglot/emitter_base.hpp"

// The shared walk machinery for the hand-written backends — see emitter_base.hpp for the abstraction.

namespace mintplayer::polyglot {

// ---- IrExprCtx: the target-independent rule-interpreter seam over the IR ---------------------------------

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
    }
    if (path == "node.receiverHasIndexer" && e_.kind == ir::ExprKind::Index)
        return static_cast<const ir::Index&>(e_).receiverHasIndexer ? "true" : "false";
    if (path == "node.insideOperator" && e_.kind == ir::ExprKind::This)
        return static_cast<const ir::This&>(e_).insideOperator ? "true" : "false";
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
    if (e_.kind == ir::ExprKind::Match) {
        const auto& m = static_cast<const ir::Match&>(e_);
        if (path == "node.arms.count")  return std::to_string(m.arms.size());
        if (path == "node.hasCatchAll") return m.hasCatchAll ? "true" : "false";
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
                        if (bf == "binding") return a.pattern.binders[j].binding;
                        if (bf == "field")   return a.pattern.binders[j].field;
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
        if (e_.kind == ir::ExprKind::Binary && c->kind == ir::ExprKind::Binary) {
            int pp = operatorPrecedence(static_cast<const ir::Binary&>(e_).op);
            int cp = operatorPrecedence(static_cast<const ir::Binary&>(*c).op);
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
    if (name == "inlineBlock" && inline_ && e_.kind == ir::ExprKind::Lambda)
        return inline_(static_cast<const ir::Lambda&>(e_).block); // statement-bodied lambda, on one line
    return targetBuiltin(name, args);
}

std::string IrExprCtx::renderType(const std::string& path) const {
    const TypeRef* t = typeRefAt(path);
    return t ? renderTypeRef(*t) : "";
}

const TypeRef* IrExprCtx::typeRefAt(const std::string& path) const {
    if (path == "node.type") return &e_.type;
    if (e_.kind == ir::ExprKind::Lambda && path.rfind("node.params.", 0) == 0 &&
        path.size() > 5 && path.rfind(".type") == path.size() - 5) {
        const auto& l = static_cast<const ir::Lambda&>(e_);
        const std::size_t i = static_cast<std::size_t>(std::stoul(path.substr(12)));
        if (i < l.params.size()) return &l.params[i].type;
    }
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

std::string TypeRefCtx::builtin(const std::string& name, const std::vector<std::string>& /*args*/) const {
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
    return path == "decl.body" ? &m_.body : nullptr;
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
        }
    }
    return "";
}

std::string RecordDeclCtx::builtin(const std::string& name, const std::vector<std::string>& args) const {
    if (name == "generics") return hooks_.generics(r_.generics);
    if (name == "where")    return hooks_.where(r_.generics);
    if (name == "ident")    return hooks_.ident(args.empty() ? std::string() : args[0]);
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
            line(localDecl(l.name, l.isMutable) + " = " + emitExpr(*l.init) + spec().stmtEnd);
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
            line(u.binding + ".dispose()" + spec().stmtEnd);
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
        default:
            emitStmtTarget(s);
            return;
    }
}

} // namespace mintplayer::polyglot
