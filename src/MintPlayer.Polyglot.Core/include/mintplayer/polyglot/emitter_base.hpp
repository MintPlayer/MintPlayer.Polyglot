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

// How a target delimits a block. The brace family differs only in where `{` sits; an indentation target
// (Python) has no braces at all — `head:` then an indented body, no closer. headBlock() and the If/While
// statements consult this so the shared statement walk serves all three.
enum class BlockStyle {
    BracesAllman, // `head` ⏎ `{` … `}`   (C#)
    BracesKnR,    // `head {` … `}`        (TypeScript)
    ColonIndent,  // `head:` … (indent only, no closer)  (Python)
};

class EmitterBase {
protected:
    std::string out_;
    int indent_ = 0;

    void line(const std::string& s);

    // Emit a block body: the statements, indented one level, with no braces of their own.
    void blockBody(const std::vector<ir::StmtPtr>& body);

    // Open a block after `head` / close it, per the target's BlockStyle (braces vs colon+indent). headBlock
    // is the common open+body+close; If/While/Use call openBlock/closeBlock directly where the else/finally
    // arms need finer control.
    void openBlock(const std::string& head);
    void closeBlock();
    void headBlock(const std::string& head, const std::vector<ir::StmtPtr>& body);

    // Render statements onto a single line (for statement-bodied lambdas, which live mid-expression).
    std::string inlineBlock(const std::vector<ir::StmtPtr>& body);

    // Statement dispatch. The statements whose spelling is identical across targets are rendered here;
    // every other kind (declarations, and the target-specific Let/Yield/Throw/Use/Try) routes to the
    // concrete backend via emitStmtTarget.
    void emitStmt(const ir::Stmt& s);

    // ---- Backend hook surface (the contract between this shared engine and a concrete backend) ----------
    // This is the realized "Hook tier" of P9's {spec data + hooks} split. It comes in two granularities:
    //
    //   (a) Wholesale per-target walks. The engine owns the statement layer but delegates the whole
    //       expression walk and the two irreducibly per-target statements (For's head, Try) to the backend,
    //       because their *shapes* diverge, not just their spelling. All the imperative codegen the design
    //       note calls the "30%" lives behind these — numeric narrowing/BigInt boundaries, Cast/tsConvert,
    //       Match, Try, string interpolation, operator-method dispatch, and every declaration emitter
    //       (enum/union/record/class/function/extension), which are likewise per-target by shape.
    virtual std::string emitExpr(const ir::Expr& e) = 0;     // the entire expression walk
    virtual void emitStmtTarget(const ir::Stmt& s) = 0;      // the statements emitStmt does not handle (For, Try)
    //
    //   (b) Fine-grained spelling hooks. The engine owns the *structure* of these statements; only a short
    //       spelling differs per target, so the backend supplies just that string.
    virtual BlockStyle blockStyle() const = 0;                                  // braces (C#/TS) vs colon+indent (Py)
    virtual const char* stmtEnd() const { return ";"; }                         // statement terminator; Python: ""
    virtual const char* throwKeyword() const { return "throw"; }                // `throw v`; Python: `raise v`
    virtual std::string localDecl(const std::string& name, bool isMutable) = 0; // `var x`/`let|const x`/`x` (Let, Use)
    virtual std::string yieldStmt(const std::string& value, bool hasValue) = 0; // `yield return v;`/`yield v;` …
    virtual std::string rethrowStmt() = 0;                                      // value-less `throw;`/`throw __e;`

public:
    virtual ~EmitterBase() = default;
};

} // namespace mintplayer::polyglot
