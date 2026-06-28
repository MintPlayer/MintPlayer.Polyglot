#pragma once

#include <memory>
#include <string>
#include <vector>

#include "mintplayer/polyglot/diagnostics.hpp"

// The AST for the MVP subset. After semantic analysis (sema.hpp) every Expr carries a resolved `type`;
// that type-annotated tree IS the MVP's "typed tree IR" (PRD §4.2). P4 may split a distinct IR out of
// the AST once the surface is wide enough to need lowering/desugaring — for the walking skeleton the
// AST-with-types is the IR the backends consume.

namespace mintplayer::polyglot {

enum class Ty { Unknown, Unit, I32, F64, Bool, String };

inline const char* tyName(Ty t) {
    switch (t) {
        case Ty::Unit:   return "unit";
        case Ty::I32:    return "i32";
        case Ty::F64:    return "f64";
        case Ty::Bool:   return "bool";
        case Ty::String: return "string";
        default:         return "<unknown>";
    }
}

enum class ExprKind { IntLit, FloatLit, BoolLit, StringLit, Name, Unary, Binary, Call };

struct Expr {
    ExprKind kind;
    SourcePos pos;
    Ty type = Ty::Unknown;          // filled by sema

    std::string text;               // raw numeric text | string value | name | call callee | operator
    bool boolVal = false;           // BoolLit

    std::unique_ptr<Expr> lhs;      // Unary operand / Binary left
    std::unique_ptr<Expr> rhs;      // Binary right
    std::vector<std::unique_ptr<Expr>> args; // Call arguments
};
using ExprPtr = std::unique_ptr<Expr>;

enum class StmtKind { Let, Assign, ExprStmt, If, While, Return };

struct Stmt {
    StmtKind kind;
    SourcePos pos;

    std::string name;               // Let / Assign target
    bool isMutable = false;         // Let: `var` (true) vs `let` (false)
    Ty declType = Ty::Unknown;      // Let: optional explicit annotation

    ExprPtr value;                  // Let init | Assign value | ExprStmt | Return value | If/While cond

    std::vector<std::unique_ptr<Stmt>> thenBody; // If-then / While body
    std::vector<std::unique_ptr<Stmt>> elseBody; // If-else
    bool hasElse = false;
};
using StmtPtr = std::unique_ptr<Stmt>;

struct Param {
    std::string name;
    Ty type = Ty::Unknown;
    SourcePos pos;
};

struct FunctionDecl {
    std::string name;
    SourcePos pos;
    std::vector<Param> params;
    Ty returnType = Ty::Unit;
    std::vector<StmtPtr> body;
};

struct CompilationUnit {
    std::vector<FunctionDecl> functions;
};

} // namespace mintplayer::polyglot
