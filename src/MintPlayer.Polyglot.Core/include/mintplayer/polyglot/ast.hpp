#pragma once

#include <memory>
#include <string>
#include <vector>

#include "mintplayer/polyglot/diagnostics.hpp"

// The AST. After sema (sema.hpp) every Expr carries a resolved `type`; that type-annotated tree is the
// MVP's typed tree IR (PRD §4.2). Nodes are a single discriminated (kind-tagged) struct per category —
// consumers switch on `kind`. P3 widens the parser/printer to the full P1 expression grammar; statements,
// declarations, rich types, and string interpolation widen in later P3 increments.

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

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;
struct Stmt;
using StmtPtr = std::unique_ptr<Stmt>;

struct Param {
    std::string name;
    Ty type = Ty::Unknown;   // Unknown = inferred (e.g. an untyped lambda parameter)
    SourcePos pos;
};

struct FieldInit {           // `name = value` inside a `with { ... }` expression
    std::string name;
    ExprPtr value;
};

enum class ExprKind {
    IntLit, FloatLit, CharLit, StringLit, BoolLit, NullLit,
    Name, This, Super,
    Unary, Binary, Range,
    Call, Member, Index, NullAssert,
    Lambda, ListLit, TupleLit, With, IfExpr,
};

// One struct, many shapes. Field usage by kind (unused fields stay default):
//   IntLit/FloatLit/CharLit/StringLit : text = literal value/text
//   BoolLit                            : boolVal
//   Name/Member                        : text = name (Member also: lhs = object, flag = null-safe `?.`)
//   Unary/NullAssert                   : text = op, lhs = operand
//   Binary                             : text = op, lhs/rhs = operands
//   Range                              : lhs/rhs = endpoints, flag = inclusive (`..=`)
//   Call                               : lhs = callee, args = arguments
//   Index                              : lhs = object, args = indices
//   ListLit/TupleLit                   : args = elements
//   Lambda                             : params; flag ? block : lhs (expression body)
//   With                               : lhs = base, fields = field inits
//   IfExpr                             : lhs = cond, rhs = then-expr, extra = else-expr
struct Expr {
    ExprKind kind;
    SourcePos pos;
    Ty type = Ty::Unknown;

    std::string text;
    bool boolVal = false;
    bool flag = false;          // Member null-safe | Range inclusive | Lambda has block body

    ExprPtr lhs;
    ExprPtr rhs;
    ExprPtr extra;
    std::vector<ExprPtr> args;
    std::vector<Param> params;
    std::vector<StmtPtr> block;
    std::vector<FieldInit> fields;
};

enum class StmtKind { Let, Assign, ExprStmt, If, While, Return };

struct Stmt {
    StmtKind kind;
    SourcePos pos;

    std::string name;               // Let / Assign target
    bool isMutable = false;         // Let: `var` (true) vs `let` (false)
    Ty declType = Ty::Unknown;      // Let: optional explicit annotation

    ExprPtr value;                  // Let init | Assign value | ExprStmt | Return value | If/While cond

    std::vector<StmtPtr> thenBody;  // If-then / While body
    std::vector<StmtPtr> elseBody;  // If-else
    bool hasElse = false;
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
