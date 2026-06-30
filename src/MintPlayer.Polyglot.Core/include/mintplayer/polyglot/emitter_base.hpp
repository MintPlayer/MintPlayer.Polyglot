#pragma once

#include <string>
#include <vector>

#include "mintplayer/polyglot/ir.hpp"

// Shared walk machinery for the hand-written backends — the seed of the P9 `SpecEmitter` engine (see
// docs/design/backend-spec.md). It owns the byte-identical output buffer + indentation state and the
// statement dispatch whose spelling is the same across C# and TS; everything target-specific is reached
// through the pure-virtual hooks the concrete emitters override. As later P9 slices lift more shared
// structure up here, the two emitters shrink toward `{spec data + a handful of hooks}`.

namespace mintplayer::polyglot {

class EmitterBase {
protected:
    std::string out_;
    int indent_ = 0;

    void line(const std::string& s) {
        out_.append(static_cast<std::size_t>(indent_) * 4, ' ');
        out_ += s;
        out_ += '\n';
    }

    // Emit a block body: the statements, indented one level, with no braces of their own.
    void blockBody(const std::vector<ir::StmtPtr>& body) {
        ++indent_;
        for (const auto& s : body) emitStmt(*s);
        --indent_;
    }

    // Emit `head` followed by a brace-delimited block, per the target's brace style — the one real
    // divergence in block control flow: K&R puts `{` on the head line (TS), Allman on its own line (C#).
    void headBlock(const std::string& head, const std::vector<ir::StmtPtr>& body) {
        if (bracesOnHeadLine()) line(head + " {");
        else { line(head); line("{"); }
        blockBody(body);
        line("}");
    }

    // Render statements onto a single line (for statement-bodied lambdas, which live mid-expression).
    std::string inlineBlock(const std::vector<ir::StmtPtr>& body) {
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

    // Statement dispatch. The leaf statements whose spelling is identical across targets are rendered here;
    // every other kind (declarations, brace-style-sensitive control flow, and the target-specific
    // Let/Yield/Throw/Use/Try) routes to the concrete backend via emitStmtTarget.
    void emitStmt(const ir::Stmt& s) {
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
            default:
                emitStmtTarget(s);
                return;
        }
    }

    // Hooks the concrete backend implements.
    virtual std::string emitExpr(const ir::Expr& e) = 0;
    virtual void emitStmtTarget(const ir::Stmt& s) = 0;
    virtual bool bracesOnHeadLine() const = 0; // K&R (TS) vs Allman (C#) — see headBlock()

public:
    virtual ~EmitterBase() = default;
};

} // namespace mintplayer::polyglot
