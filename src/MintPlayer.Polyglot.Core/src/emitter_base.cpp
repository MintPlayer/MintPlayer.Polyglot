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
    }
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
    return targetBuiltin(name, args);
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
