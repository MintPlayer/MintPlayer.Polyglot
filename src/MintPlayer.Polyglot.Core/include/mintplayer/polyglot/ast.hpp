#pragma once

#include <memory>
#include <string>
#include <vector>

#include "mintplayer/polyglot/diagnostics.hpp"

// The AST. Type annotations are syntactic `TypeRef`s (name + generic args / tuple / function + nullable);
// the MVP scalar checker still reasons in the `Ty` enum and converts at the boundary via scalarTyOf().
// P3 widens the parser/printer to the full P1 grammar; sema/emitters still only walk the compiled MVP
// subset (the round-trip fidelity gate is parser+printer only — full semantics is P4, emission P5).

namespace mintplayer::polyglot {

// Scalar type lattice used by the MVP type checker (sema). Expr::type stays a Ty.
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

// A syntactic type reference as written in source.
struct TypeRef {
    enum class Kind { Named, Tuple, Function };
    Kind kind = Kind::Named;
    std::string name;             // Named: type name; empty Named = absent (inferred, e.g. lambda param)
    std::vector<TypeRef> args;    // Named: generic args | Tuple: elements | Function: parameter types
    std::vector<TypeRef> ret;     // Function: the single return type (size 1)
    bool nullable = false;        // trailing '?'

    bool absent() const { return kind == Kind::Named && name.empty() && args.empty() && !nullable; }
};

inline TypeRef namedType(std::string n) {
    TypeRef t;
    t.name = std::move(n);
    return t;
}

// Collapse a TypeRef to the scalar lattice for the MVP checker; non-scalar/named types are Unknown.
inline Ty scalarTyOf(const TypeRef& t) {
    if (t.kind != TypeRef::Kind::Named || t.nullable || !t.args.empty()) return Ty::Unknown;
    if (t.name == "i32") return Ty::I32;
    if (t.name == "f64") return Ty::F64;
    if (t.name == "bool") return Ty::Bool;
    if (t.name == "string") return Ty::String;
    if (t.name == "unit") return Ty::Unit;
    return Ty::Unknown;
}

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;
struct Stmt;
using StmtPtr = std::unique_ptr<Stmt>;

struct Param {
    std::string name;
    TypeRef type;            // absent() = inferred
    SourcePos pos;
    ExprPtr defaultValue;    // optional `= expr`
    bool hasDefault = false;
};

struct GenericParam {
    std::string name;
    std::vector<TypeRef> bounds;   // `T: A & B`
};

struct FieldInit {           // `name = value` inside a `with { ... }` expression
    std::string name;
    ExprPtr value;
};

// ---- patterns (for `match`) ----
enum class PatKind { Wildcard, Literal, Binding, Ctor, Tuple };
struct Pattern {
    PatKind kind = PatKind::Wildcard;
    SourcePos pos;
    std::string name;            // Binding / Ctor name
    ExprPtr literal;             // Literal pattern's value
    std::vector<Pattern> sub;    // Ctor arguments / Tuple elements
    TypeRef type;                // typed binding `x: T`
    bool hasType = false;
};

struct MatchArm {
    Pattern pattern;
    ExprPtr guard;               // optional `if <expr>`
    ExprPtr body;                // when !bodyIsBlock
    bool bodyIsBlock = false;
    std::vector<StmtPtr> block;  // when bodyIsBlock
};

enum class ExprKind {
    IntLit, FloatLit, CharLit, StringLit, InterpString, BoolLit, NullLit,
    Name, This, Super,
    Unary, Binary, Range, Cast, Extern,
    Call, Member, Index, NullAssert,
    Lambda, ListLit, TupleLit, With, IfExpr, Match,
};

struct Expr {
    ExprKind kind;
    SourcePos pos;
    TypeRef type;                   // resolved semantic type, filled by sema (P4); empty = unknown

    std::string text;
    bool boolVal = false;
    bool flag = false;          // Member null-safe | Range inclusive | Lambda has block body

    ExprPtr lhs;                // operand/object/left/callee/base/cond/scrutinee
    ExprPtr rhs;                // Binary/Range right | IfExpr then
    ExprPtr extra;              // IfExpr else
    std::vector<ExprPtr> args;  // Call/Index args | List/Tuple elements
    std::vector<Param> params;  // Lambda params
    std::vector<StmtPtr> block; // Lambda block body
    std::vector<FieldInit> fields; // With field inits
    std::vector<MatchArm> arms; // Match arms
    std::vector<TypeRef> typeArgs; // Call: explicit generic args, e.g. List<i32>()
    std::vector<std::string> chunks; // InterpString: N+1 literal chunks around N holes (in `args`)
    TypeRef castType;              // Cast: the target type in `(T)expr` (operand is `lhs`)
    std::string overloadName;      // Call: the resolved overload's mangled name (set by sema); empty = none
    std::string staticOwner;       // Name/Call: resolves to a static/const member of this enclosing class
                                   // (set by sema so lowering can qualify it as `Owner.member`); empty = none
};

struct CatchClause {
    std::string name;            // bound exception variable
    TypeRef type;                // caught type
    ExprPtr guard;               // optional `when (expr)`
    std::vector<StmtPtr> body;
    SourcePos pos;
};

enum class StmtKind { Let, Assign, ExprStmt, If, While, For, Return, Break, Continue, Throw, Yield, Use, Try };

struct Stmt {
    StmtKind kind;
    SourcePos pos;

    std::string name;               // Let / Use binding name
    bool isMutable = false;         // Let: `var` (true) vs `let` (false)
    TypeRef declType;               // Let / Use: explicit annotation (when hasDeclType)
    bool hasDeclType = false;

    ExprPtr target;                 // Assign: lvalue
    std::string op;                 // Assign: "=", "+=", ...
    ExprPtr value;                  // Let/Use init | Assign RHS | ExprStmt | Return/Throw/Yield value | If/While/For cond/iterable

    Pattern forBinding;             // For: the loop binding (Binding or Tuple)
    std::vector<StmtPtr> thenBody;  // If-then / While / For / Use / Try body
    std::vector<StmtPtr> elseBody;  // If-else
    bool hasElse = false;

    std::vector<CatchClause> catches;  // Try
    std::vector<StmtPtr> finallyBody;  // Try
    bool hasFinally = false;           // Try
};

struct FunctionDecl {
    std::string name;
    std::string mangledName;       // per-target name for overloads (set by sema); == name when not overloaded
    SourcePos pos;
    std::vector<GenericParam> generics;
    std::vector<Param> params;
    TypeRef returnType = namedType("unit");
    std::vector<StmtPtr> body;
    bool isExpect = false;         // `expect fn` — a capability signature with no body (§4.4)
    std::string actualTarget;      // `actual(<target>) fn` — the per-target implementation; empty otherwise
};

// A member of a record/class/interface body.
enum class MemberKind { Field, Const, Init, Method, Operator, Property };
struct Member {
    MemberKind kind;
    SourcePos pos;
    std::vector<std::string> modifiers;  // abstract/open/override/static/private/async
    std::string name;                    // field/const/method/operator/property name (Init: "init")
    bool isMutable = false;              // Field/Property: var vs let
    TypeRef type;                        // Field/Const/Property type
    ExprPtr init;                        // Field/Const initializer | Property read-only getter (`=> expr`)
    std::vector<GenericParam> generics;  // Method/Operator
    std::vector<Param> params;           // Method/Operator/Init
    TypeRef returnType;                  // Method/Operator
    std::vector<StmtPtr> body;           // block body
    ExprPtr exprBody;                    // `=> expr` body
    bool hasBody = false;                // false = stub (interface method `;`)
    bool exprBodied = false;             // true = `=> expr`, false = block
};

struct RecordDecl {
    std::string name;
    SourcePos pos;
    std::vector<GenericParam> generics;
    std::vector<Param> fields;           // positional fields
    std::vector<TypeRef> bases;          // implemented interfaces
    std::vector<Member> members;         // optional body
};
struct ClassDecl {
    std::vector<std::string> modifiers;
    std::string name;
    SourcePos pos;
    std::vector<GenericParam> generics;
    std::vector<TypeRef> bases;          // base class and/or interfaces
    std::vector<Member> members;
};
struct InterfaceDecl {
    std::string name;
    SourcePos pos;
    std::vector<GenericParam> generics;
    std::vector<TypeRef> bases;
    std::vector<Member> members;
};
struct ExtensionDecl {                   // `extension fn Receiver.name<...>(...): ret body`
    TypeRef receiver;
    std::string name;
    SourcePos pos;
    std::vector<GenericParam> generics;
    std::vector<Param> params;
    TypeRef returnType = namedType("unit");
    std::vector<StmtPtr> body;
    ExprPtr exprBody;
    bool exprBodied = false;
};
struct ValueDecl {                       // top-level `const` / `let`
    bool isConst = false;
    std::string name;
    SourcePos pos;
    TypeRef type;
    bool hasType = false;
    ExprPtr init;
};

struct EnumCase {
    std::string name;
    bool hasValue = false;
    long long value = 0;
    SourcePos pos;
};
struct EnumDecl {
    std::string name;
    SourcePos pos;
    std::vector<EnumCase> cases;
};

struct UnionCase {
    std::string name;
    std::vector<Param> params;   // empty = payload-free case
    SourcePos pos;
};
struct UnionDecl {
    std::string name;
    SourcePos pos;
    std::vector<UnionCase> cases;
};

struct ImportDecl {
    std::string path;                // dotted module path, e.g. "std.io"
    std::vector<std::string> names;  // selective group `{ a, b }` (empty if none)
    std::string alias;               // `as X` (empty if none)
    SourcePos pos;
};

struct CompilationUnit {
    std::vector<ImportDecl> imports;
    std::vector<EnumDecl> enums;
    std::vector<UnionDecl> unions;
    std::vector<RecordDecl> records;
    std::vector<ClassDecl> classes;
    std::vector<InterfaceDecl> interfaces;
    std::vector<ExtensionDecl> extensions;
    std::vector<ValueDecl> values;
    std::vector<FunctionDecl> functions;
};

} // namespace mintplayer::polyglot
