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
    for (const auto& s : body) emitStmt(*s);
    --indent_;
}

void EmitterBase::headBlock(const std::string& head, const std::vector<ir::StmtPtr>& body) {
    if (bracesOnHeadLine()) line(head + " {");
    else { line(head); line("{"); }
    blockBody(body);
    line("}");
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
            line(emitExpr(*a.target) + " " + a.op + " " + emitExpr(*a.value) + ";");
            return;
        }
        case ir::StmtKind::ExprStmt:
            line(emitExpr(*static_cast<const ir::ExprStmt&>(s).expr) + ";");
            return;
        case ir::StmtKind::Return: {
            const auto& r = static_cast<const ir::Return&>(s);
            line(r.value ? "return " + emitExpr(*r.value) + ";" : "return;");
            return;
        }
        case ir::StmtKind::While: { // `while (cond)` head is identical across targets; braces via headBlock
            const auto& w = static_cast<const ir::While&>(s);
            headBlock("while (" + emitExpr(*w.cond) + ")", w.body);
            return;
        }
        case ir::StmtKind::If: { // `if (cond)` head is identical; the else arm merges the brace in K&R
            const auto& i = static_cast<const ir::If&>(s);
            std::string head = "if (" + emitExpr(*i.cond) + ")";
            if (bracesOnHeadLine()) line(head + " {");
            else { line(head); line("{"); }
            blockBody(i.thenBody);
            if (!i.hasElse) { line("}"); return; }
            if (bracesOnHeadLine()) { line("} else {"); blockBody(i.elseBody); line("}"); }
            else { line("}"); line("else"); line("{"); blockBody(i.elseBody); line("}"); }
            return;
        }
        default:
            emitStmtTarget(s);
            return;
    }
}

} // namespace mintplayer::polyglot
