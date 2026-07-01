#include "mintplayer/polyglot/emitter_base.hpp"

// The shared walk machinery for the hand-written backends — see emitter_base.hpp for the abstraction.

namespace mintplayer::polyglot {

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
