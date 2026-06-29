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
enum class ExprKind { Int, Float, Bool, Str, Var, This, Unary, Binary, Call, MethodCall, Member, New, MakeCase, Match };

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
struct This : Expr {
    explicit This(SourcePos p, Type t) : Expr(ExprKind::This, p, std::move(t)) {}
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
struct MethodCall : Expr { // `object.method(args)`
    ExprPtr object;
    std::string method;
    std::vector<ExprPtr> args;
    MethodCall(SourcePos p, Type t, ExprPtr o, std::string m)
        : Expr(ExprKind::MethodCall, p, std::move(t)), object(std::move(o)), method(std::move(m)) {}
};
struct Member : Expr { // field / property access `object.field`
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

struct MakeCaseField { // a field of a union-case construction
    std::string name;
    ExprPtr value;
};
struct MakeCase : Expr { // union-case construction: C# `new Case(...)`, TS `{ tag: "Case", ... }`
    std::string unionName;
    std::string caseName;
    std::vector<MakeCaseField> fields;
    MakeCase(SourcePos p, Type t, std::string u, std::string c)
        : Expr(ExprKind::MakeCase, p, std::move(t)), unionName(std::move(u)), caseName(std::move(c)) {}
};

// match patterns
enum class PatternKind { Wildcard, Literal, Binding, EnumCase, Ctor };
struct Binder { // a sub-binding of a constructor pattern: bind `binding` from field `field`
    std::string field;
    std::string binding;
};
struct Pattern {
    PatternKind kind = PatternKind::Wildcard;
    std::string binding;            // Binding: the bound name
    ExprPtr literal;                // Literal: the constant
    std::string enumType, enumCase; // EnumCase: Type.Case
    std::string ctorCase;           // Ctor: the union case name
    std::vector<Binder> binders;    // Ctor: field -> binding (in declared order)
};
struct MatchArm {
    Pattern pattern;
    ExprPtr guard; // optional
    ExprPtr body;
};
struct Match : Expr {
    ExprPtr scrutinee;
    std::vector<MatchArm> arms;
    Match(SourcePos p, Type t, ExprPtr s) : Expr(ExprKind::Match, p, std::move(t)), scrutinee(std::move(s)) {}
};

// ---- statements ----
enum class StmtKind { Let, Assign, ExprStmt, If, While, For, Return, Yield, Throw, Try, Use };

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
struct For : Stmt { // `for binding in <seq>`: either an integer range or an arbitrary iterable
    std::string binding;
    bool isRange = false;
    ExprPtr rangeStart, rangeEnd; // range form: bounds
    bool inclusive = false;       // range form: `..=` (true) vs `..` (false)
    ExprPtr iterable;             // iterable form: the sequence expression
    std::vector<StmtPtr> body;
    For(SourcePos p, std::string b) : Stmt(StmtKind::For, p), binding(std::move(b)) {}
};
struct Return : Stmt {
    ExprPtr value; // may be null
    Return(SourcePos p, ExprPtr v) : Stmt(StmtKind::Return, p), value(std::move(v)) {}
};
struct Yield : Stmt { // `yield <value>` inside an iterator: C# `yield return`, TS generator `yield`
    ExprPtr value;
    Yield(SourcePos p, ExprPtr v) : Stmt(StmtKind::Yield, p), value(std::move(v)) {}
};
struct Throw : Stmt {
    ExprPtr value;
    Throw(SourcePos p, ExprPtr v) : Stmt(StmtKind::Throw, p), value(std::move(v)) {}
};
// `use x = init { body }`: deterministic, scoped disposal. Lowers to a try/finally that calls
// `x.dispose()` at block end (even on throw) — exact on both targets and the SPEC's TS lowering.
struct Use : Stmt {
    std::string binding;
    Type type; // optional annotation (may be empty)
    ExprPtr init;
    std::vector<StmtPtr> body;
    Use(SourcePos p, std::string b, ExprPtr i) : Stmt(StmtKind::Use, p), binding(std::move(b)), init(std::move(i)) {}
};
struct Catch { // one `catch (binding: type) when (guard)` clause; type/guard optional
    Type type;                    // empty name = untyped catch-all
    std::string binding;          // bound exception variable (may be empty)
    ExprPtr guard;                // optional `when` guard
    std::vector<StmtPtr> body;
};
// C# has typed catches + `when` natively; TS has one untyped catch, so its backend lowers the catch
// list into an `instanceof`/guard dispatch chain. The IR keeps the structured clauses; each backend
// renders them in its own idiom (different layer, different abstraction).
struct Try : Stmt {
    std::vector<StmtPtr> body;
    std::vector<Catch> catches;
    std::vector<StmtPtr> finallyBody;
    bool hasFinally = false;
    explicit Try(SourcePos p) : Stmt(StmtKind::Try, p) {}
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
    bool isEntry = false;    // a `fn main()` with no params — the program entry point
    bool isIterator = false; // body contains `yield` — C# `IEnumerable<T>`+`yield return`, TS `function*`
};
struct RecordField {
    std::string name;
    Type type;
};
enum class MethodKind { Method, Operator, Property };
struct Method {
    MethodKind kind = MethodKind::Method;
    std::string name;             // method/property name; operator method name (e.g. "plus")
    std::string opSymbol;         // Operator: the source symbol ("+", "-", ...); else empty
    std::vector<Param> params;
    Type returnType;
    bool exprBodied = false;      // `=> expr` vs block
    ExprPtr exprBody;
    std::vector<StmtPtr> body;
};
struct Record { // an immutable data type (record)
    std::string name;
    std::vector<RecordField> fields;
    std::vector<Method> methods;
};
struct EnumCase {
    std::string name;
    long long value;
};
struct Enum {
    std::string name;
    std::vector<EnumCase> cases;
};
struct UnionCaseField {
    std::string name;
    Type type;
};
struct UnionCase {
    std::string name;
    std::vector<UnionCaseField> fields; // empty = payload-free case
};
struct Union {
    std::string name;
    std::vector<UnionCase> cases;
};
struct ClassField {
    std::string name;
    bool isMutable = false;
    Type type;
    ExprPtr init;   // optional field initializer (may be null)
};
struct Class { // a mutable reference type
    std::string name;
    std::vector<Type> bases;        // base class and/or interfaces (single base drives `extends`/`: Base`)
    std::vector<ClassField> fields;
    bool hasInit = false;
    std::vector<Param> initParams;
    bool hasSuper = false;             // init calls `super(...)`: C# `: base(args)`, TS `super(args);`
    std::vector<ExprPtr> superArgs;    // the base-constructor arguments (hoisted out of initBody)
    std::vector<StmtPtr> initBody;
    std::vector<Method> methods;
};
struct Module {
    std::vector<Enum> enums;
    std::vector<Union> unions;
    std::vector<Record> records;
    std::vector<Class> classes;
    std::vector<Function> functions;
};

// A stable, deterministic textual dump of the typed IR (for inspection and the P4 gate).
std::string dump(const Module& m);

} // namespace mintplayer::polyglot::ir
