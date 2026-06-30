#pragma once

#include <string>
#include <vector>

#include "mintplayer/polyglot/ir.hpp"

// Shared walk machinery for the hand-written backends — the seed of the P9 `SpecEmitter` engine (see
// docs/design/backend-spec.md). It owns the byte-identical output buffer + indentation state and the
// statement dispatch whose spelling is the same across C# and TS; everything target-specific is reached
// through the pure-virtual hooks the concrete emitters override. As later P9 slices lift more shared
// structure up here, the two emitters shrink toward `{spec data + a handful of hooks}`.
//
// Implementation in emitter_base.cpp (this repo keeps logic in .cpp; headers stay declaration-only).

namespace mintplayer::polyglot {

class EmitterBase {
protected:
    std::string out_;
    int indent_ = 0;

    void line(const std::string& s);

    // Emit a block body: the statements, indented one level, with no braces of their own.
    void blockBody(const std::vector<ir::StmtPtr>& body);

    // Emit `head` followed by a brace-delimited block, per the target's brace style — the one real
    // divergence in block control flow: K&R puts `{` on the head line (TS), Allman on its own line (C#).
    void headBlock(const std::string& head, const std::vector<ir::StmtPtr>& body);

    // Render statements onto a single line (for statement-bodied lambdas, which live mid-expression).
    std::string inlineBlock(const std::vector<ir::StmtPtr>& body);

    // Statement dispatch. The statements whose spelling is identical across targets are rendered here;
    // every other kind (declarations, and the target-specific Let/Yield/Throw/Use/Try) routes to the
    // concrete backend via emitStmtTarget.
    void emitStmt(const ir::Stmt& s);

    // Hooks the concrete backend implements.
    virtual std::string emitExpr(const ir::Expr& e) = 0;
    virtual void emitStmtTarget(const ir::Stmt& s) = 0;
    virtual bool bracesOnHeadLine() const = 0; // K&R (TS) vs Allman (C#) — see headBlock()
    // Spelling hooks for the statement tail (the structure lives in emitStmt; only these spellings diverge):
    virtual std::string localDecl(const std::string& name, bool isMutable) = 0; // `var x`/`let|const x` (Let, Use)
    virtual std::string yieldStmt(const std::string& value, bool hasValue) = 0; // `yield return v;`/`yield v;` …
    virtual std::string rethrowStmt() = 0;                                      // value-less `throw;`/`throw __e;`

public:
    virtual ~EmitterBase() = default;
};

} // namespace mintplayer::polyglot
