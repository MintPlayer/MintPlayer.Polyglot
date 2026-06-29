#pragma once

#include <memory>
#include <string>
#include <vector>

#include "mintplayer/polyglot/ast.hpp" // SourcePos, TypeRef

// The high-level, typed, tree-shaped IR (PRD §4.2). This is the contract the backends consume — produced
// from the AST by the lowering pass (lower.hpp) after semantic analysis. It is deliberately NOT SSA and
// NOT a lowest-common-denominator form: it keeps loops, names and structure so backends can emit readable
// code, and it carries resolved decisions the parser/AST don't (every expression's type; whether a call is
// the `print` intrinsic; whether a function is the program entry point). Sugar is desugared into a small
// set of core nodes here so each backend handles fewer shapes.
//
// Nodes are a tagged class hierarchy: a `kind` discriminator for fast dispatch, with each concrete node
// holding exactly its own fields. Consumers switch on `kind` and static_cast. The IR widens with the
// surface in P5 (records/classes/match/...); this is the MVP-subset core.

namespace mintplayer::polyglot::ir {

using Type = TypeRef; // the IR reuses the resolved semantic type

// ---- expressions ----
enum class ExprKind { Int, Float, Bool, Str, Var, Unary, Binary, Call, Member, New };

struct Expr {
    ExprKind kind;
    SourcePos pos;
    Type type;
    virtual ~Expr() = default;
protected:
    Expr(ExprKind k, SourcePos p, Type t) : kind(k), pos(p), type(std::move(t)) {}
};
using ExprPtr = std::unique_ptr<Expr>;

struct IntLit : Expr {
    std::string text;
    IntLit(SourcePos p, Type t, std::string s) : Expr(ExprKind::Int, p, std::move(t)), text(std::move(s)) {}
};
struct FloatLit : Expr {
    std::string text;
    FloatLit(SourcePos p, Type t, std::string s) : Expr(ExprKind::Float, p, std::move(t)), text(std::move(s)) {}
};
struct BoolLit : Expr {
    bool value;
    BoolLit(SourcePos p, Type t, bool v) : Expr(ExprKind::Bool, p, std::move(t)), value(v) {}
};
struct StrLit : Expr {
    std::string value;
    StrLit(SourcePos p, Type t, std::string v) : Expr(ExprKind::Str, p, std::move(t)), value(std::move(v)) {}
};
struct Var : Expr { // reference to a local or parameter
    std::string name;
    Var(SourcePos p, Type t, std::string n) : Expr(ExprKind::Var, p, std::move(t)), name(std::move(n)) {}
};
struct Unary : Expr {
    std::string op;
    ExprPtr operand;
    Unary(SourcePos p, Type t, std::string o, ExprPtr e) : Expr(ExprKind::Unary, p, std::move(t)), op(std::move(o)), operand(std::move(e)) {}
};
struct Binary : Expr {
    std::string op;
    ExprPtr lhs, rhs;
    Binary(SourcePos p, Type t, std::string o, ExprPtr l, ExprPtr r)
        : Expr(ExprKind::Binary, p, std::move(t)), op(std::move(o)), lhs(std::move(l)), rhs(std::move(r)) {}
};
struct Call : Expr { // resolved direct call; `isPrint` marks the `print` intrinsic
    std::string callee;
    bool isPrint = false;
    std::vector<ExprPtr> args;
    Call(SourcePos p, Type t, std::string c, bool print) : Expr(ExprKind::Call, p, std::move(t)), callee(std::move(c)), isPrint(print) {}
};
struct Member : Expr { // field access `object.field`
    ExprPtr object;
    std::string field;
    bool nullSafe = false;
    Member(SourcePos p, Type t, ExprPtr o, std::string f, bool ns)
        : Expr(ExprKind::Member, p, std::move(t)), object(std::move(o)), field(std::move(f)), nullSafe(ns) {}
};
struct New : Expr { // construction `Type(args)`
    std::string typeName;
    std::vector<ExprPtr> args;
    New(SourcePos p, Type t, std::string n) : Expr(ExprKind::New, p, std::move(t)), typeName(std::move(n)) {}
};

// ---- statements ----
enum class StmtKind { Let, Assign, ExprStmt, If, While, Return };

struct Stmt {
    StmtKind kind;
    SourcePos pos;
    virtual ~Stmt() = default;
protected:
    Stmt(StmtKind k, SourcePos p) : kind(k), pos(p) {}
};
using StmtPtr = std::unique_ptr<Stmt>;

struct Let : Stmt {
    std::string name;
    bool isMutable;
    Type type;
    ExprPtr init;
    Let(SourcePos p, std::string n, bool m, Type t, ExprPtr i)
        : Stmt(StmtKind::Let, p), name(std::move(n)), isMutable(m), type(std::move(t)), init(std::move(i)) {}
};
struct Assign : Stmt {
    ExprPtr target;
    std::string op;
    ExprPtr value;
    Assign(SourcePos p, ExprPtr t, std::string o, ExprPtr v)
        : Stmt(StmtKind::Assign, p), target(std::move(t)), op(std::move(o)), value(std::move(v)) {}
};
struct ExprStmt : Stmt {
    ExprPtr expr;
    ExprStmt(SourcePos p, ExprPtr e) : Stmt(StmtKind::ExprStmt, p), expr(std::move(e)) {}
};
struct If : Stmt {
    ExprPtr cond;
    std::vector<StmtPtr> thenBody, elseBody;
    bool hasElse = false;
    If(SourcePos p, ExprPtr c) : Stmt(StmtKind::If, p), cond(std::move(c)) {}
};
struct While : Stmt {
    ExprPtr cond;
    std::vector<StmtPtr> body;
    While(SourcePos p, ExprPtr c) : Stmt(StmtKind::While, p), cond(std::move(c)) {}
};
struct Return : Stmt {
    ExprPtr value; // may be null
    Return(SourcePos p, ExprPtr v) : Stmt(StmtKind::Return, p), value(std::move(v)) {}
};

// ---- declarations ----
struct Param {
    std::string name;
    Type type;
};
struct Function {
    std::string name;
    std::vector<Param> params;
    Type returnType;
    std::vector<StmtPtr> body;
    bool isEntry = false; // a `fn main()` with no params — the program entry point
};
struct RecordField {
    std::string name;
    Type type;
};
struct Record { // an immutable data type (record) — P5 lowers fields; methods/operators widen later
    std::string name;
    std::vector<RecordField> fields;
};
struct Module {
    std::vector<Record> records;
    std::vector<Function> functions;
};

// A stable, deterministic textual dump of the typed IR (for inspection and the P4 gate).
std::string dump(const Module& m);

} // namespace mintplayer::polyglot::ir
