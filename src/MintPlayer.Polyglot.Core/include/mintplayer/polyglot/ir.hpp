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
enum class ExprKind { Int, Float, Bool, Str, Char, Null, Var, This, Unary, Binary, Cast, Call, MethodCall, Member, New, MakeCase, Match, Lambda, Extern, Index, ListLit, Tuple, Bound, Interp, Cond, With };

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
struct CharLit : Expr { // a char literal `'A'`: C# `'A'` (char), TS `"A"` (string — TS has no char type)
    std::string value;
    CharLit(SourcePos p, Type t, std::string v) : Expr(ExprKind::Char, p, std::move(t)), value(std::move(v)) {}
};
struct NullLit : Expr { // the `null` literal (C# `null` / TS `null`)
    explicit NullLit(SourcePos p, Type t) : Expr(ExprKind::Null, p, std::move(t)) {}
};
struct Interp : Expr { // interpolated string: `chunks` (N+1 literal pieces) interleaved with `holes` (N exprs)
    std::vector<std::string> chunks;
    std::vector<ExprPtr> holes;
    Interp(SourcePos p, Type t) : Expr(ExprKind::Interp, p, std::move(t)) {}
};
struct Cond : Expr { // `if c { a } else { b }` as a value -> ternary `c ? a : b`
    ExprPtr cond, then, els;
    Cond(SourcePos p, Type t, ExprPtr c, ExprPtr a, ExprPtr b)
        : Expr(ExprKind::Cond, p, std::move(t)), cond(std::move(c)), then(std::move(a)), els(std::move(b)) {}
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
struct Cast : Expr { // numeric conversion: the result type is `type`, the source is `operand->type`
    ExprPtr operand;
    Cast(SourcePos p, Type t, ExprPtr o) : Expr(ExprKind::Cast, p, std::move(t)), operand(std::move(o)) {}
};
struct Extern : Expr { // `extern("…")`: raw target code emitted verbatim (the FFI hatch)
    std::string code;
    Extern(SourcePos p, Type t, std::string c) : Expr(ExprKind::Extern, p, std::move(t)), code(std::move(c)) {}
};
struct Call : Expr { // resolved direct call; `isPrint` marks the `print` intrinsic
    std::string callee;        // source name — C# uses this (native overloading)
    std::string mangledCallee; // resolved overload's per-target name — TS uses this (== callee if unique)
    bool isPrint = false;
    bool isFree = false;       // a top-level free function (C# qualifies as `Program.callee`); not a local value
    std::vector<ExprPtr> args;
    Call(SourcePos p, Type t, std::string c, bool print) : Expr(ExprKind::Call, p, std::move(t)), callee(std::move(c)), isPrint(print) {}
};
struct MethodCall : Expr { // `object.method(args)` — or a static call `Type.method(args)` when staticType set
    ExprPtr object;            // null for a static call
    std::string method;
    std::string staticType;    // non-empty = static call `staticType.method(args)` (object is null)
    std::vector<ExprPtr> args;
    bool isExtension = false; // resolved to an extension fn: C# keeps `obj.m(args)`, TS emits `m(obj, args)`
    MethodCall(SourcePos p, Type t, ExprPtr o, std::string m)
        : Expr(ExprKind::MethodCall, p, std::move(t)), object(std::move(o)), method(std::move(m)) {}
};
struct Member : Expr { // field / property access `object.field` — or a static member `staticType.field`
    ExprPtr object;            // null for a static member access
    std::string field;
    std::string staticType;    // non-empty = static access `staticType.field` (object is null)
    bool nullSafe = false;
    Member(SourcePos p, Type t, ExprPtr o, std::string f, bool ns)
        : Expr(ExprKind::Member, p, std::move(t)), object(std::move(o)), field(std::move(f)), nullSafe(ns) {}
};
struct New : Expr { // construction `Type(args)` or `Type<TypeArgs>(args)`
    std::string typeName;
    std::vector<Type> typeArgs; // explicit construction type args, e.g. `Box<i32>(7)` (empty = none)
    std::vector<ExprPtr> args;
    New(SourcePos p, Type t, std::string n) : Expr(ExprKind::New, p, std::move(t)), typeName(std::move(n)) {}
};

struct Index : Expr { // element access `receiver[index]`
    ExprPtr receiver;
    ExprPtr index;
    Index(SourcePos p, Type t, ExprPtr r, ExprPtr i)
        : Expr(ExprKind::Index, p, std::move(t)), receiver(std::move(r)), index(std::move(i)) {}
};
struct ListLit : Expr { // list literal `[a, b, c]`; `elem` is the element type T (type is List<T>)
    Type elem;
    std::vector<ExprPtr> elements;
    ListLit(SourcePos p, Type t, Type e) : Expr(ExprKind::ListLit, p, std::move(t)), elem(std::move(e)) {}
};
struct Tuple : Expr { // tuple literal `(a, b)`: C# ValueTuple, TS array `[a, b]`
    std::vector<ExprPtr> elements;
    Tuple(SourcePos p, Type t) : Expr(ExprKind::Tuple, p, std::move(t)) {}
};
struct WithField { std::string name; ExprPtr value; }; // one `name = value` override of a `with`-copy
struct With : Expr { // record copy `base with { f = v, … }`: C# native `with`; TS rebuilds via the ctor
    ExprPtr base;
    std::vector<WithField> fields; // the overridden fields (others copied from base)
    With(SourcePos p, Type t, ExprPtr b) : Expr(ExprKind::With, p, std::move(t)), base(std::move(b)) {}
};
// A bound call/access: a portable std method/property resolved to a per-target FFI template. Each backend
// substitutes `$this`->receiver and `$0`,`$1`,…->args into its own template. A `$this = …` template emits
// a receiver assignment (e.g. List.clear -> TS `xxx = []`). See ast::TargetBinding.
struct Bound : Expr {
    ExprPtr receiver;
    std::vector<ExprPtr> args;
    std::string csTemplate;
    std::string tsTemplate;
    std::string pyTemplate;
    Bound(SourcePos p, Type t) : Expr(ExprKind::Bound, p, std::move(t)) {}
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
enum class StmtKind { Let, Assign, ExprStmt, If, While, For, Return, Break, Continue, Yield, Throw, Try, Use };

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
    std::vector<std::string> tupleBindings; // non-empty: `for (a, b) in …` destructuring (binding unused)
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
struct Break : Stmt { explicit Break(SourcePos p) : Stmt(StmtKind::Break, p) {} };
struct Continue : Stmt { explicit Continue(SourcePos p) : Stmt(StmtKind::Continue, p) {} };
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
    ExprPtr defaultValue;   // optional `= expr` default; null = required parameter
};
struct GenericParam {
    std::string name;
    std::vector<Type> bounds; // `T: A & B` — C# `where T : A, B`, TS `<T extends A & B>`
};

// A lambda / closure: `(params) => expr` or `=> { block }`. Both targets emit a native arrow function;
// a param's type is omitted from the emit when absent (the bare `x => …` form relies on target typing).
// Defined here (not with the other Expr nodes) because it needs Param + StmtPtr.
struct Lambda : Expr {
    std::vector<Param> params;
    bool exprBodied = true;     // `=> expr` vs `=> { block }`
    ExprPtr body;               // exprBodied
    std::vector<StmtPtr> block; // block body
    Lambda(SourcePos p, Type t) : Expr(ExprKind::Lambda, p, std::move(t)) {}
};
struct Function {
    std::string name;
    std::string mangledName; // per-target emitted name (TS); == name unless overloaded. C# uses `name`.
    std::vector<GenericParam> generics;
    std::vector<Param> params;
    Type returnType;
    std::vector<StmtPtr> body;
    bool isEntry = false;    // a `fn main()` with no params — the program entry point
    bool isIterator = false; // body contains `yield` — C# `IEnumerable<T>`+`yield return`, TS `function*`
    bool isExtension = false; // an `extension fn T.m(...)`: params[0] is the receiver `self` (type T)
    std::string actualTarget; // `actual(<target>)` impl — emitted only by the matching backend; else empty
    bool exprBodied = false; // `=> expr` vs block (extensions; regular fns always use a block today)
    ExprPtr exprBody;        // exprBodied
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
    std::vector<GenericParam> generics;
    std::vector<Param> params;
    Type returnType;
    bool exprBodied = false;      // `=> expr` vs block
    bool isStatic = false;        // a `static fn` member — called as `Type.method(...)`, no `this`
    ExprPtr exprBody;
    std::vector<StmtPtr> body;
};
struct Record { // an immutable data type (record)
    std::string name;
    std::vector<GenericParam> generics;
    std::vector<Type> bases;        // implemented interfaces
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
    std::vector<GenericParam> generics; // `union Option<T> { … }`
    std::vector<UnionCase> cases;
};
struct ClassField {
    std::string name;
    bool isMutable = false;
    bool isStatic = false;  // a `const`/`static` member — accessed `Owner.name`, emitted `static`/`readonly`
    Type type;
    ExprPtr init;   // optional field initializer (may be null)
};
struct Class { // a mutable reference type
    std::string name;
    std::vector<GenericParam> generics;
    std::vector<Type> bases;        // base class and/or interfaces (single base drives `extends`/`: Base`)
    std::vector<ClassField> fields;
    bool hasInit = false;
    std::vector<Param> initParams;
    bool hasSuper = false;             // init calls `super(...)`: C# `: base(args)`, TS `super(args);`
    std::vector<ExprPtr> superArgs;    // the base-constructor arguments (hoisted out of initBody)
    std::vector<StmtPtr> initBody;
    std::vector<Method> methods;
};
struct Interface { // a contract: method signatures only (no bodies). C# `interface`, TS `interface`.
    std::string name;
    std::vector<GenericParam> generics;
    std::vector<Type> bases;     // extended interfaces
    std::vector<Method> methods; // signatures (body empty)
};
struct Global { // a top-level `const`/`let` value
    std::string name;
    bool isConst = false;
    Type type;
    ExprPtr init;
};
// A native-backed `extern class`: how its name spells per target (`$0,$1,…` = the rendered type args) and
// (optionally) how `Type(args)` constructs (`$T` = the spelled type, `$0,…` = ctor args). This is the data
// that replaced the emitters' hardcoded List/Iterable/Error mappings — a backend consults it from `csType`/
// `tsType` (spelling) and at construction (ctor), so a user plugin class maps + constructs the same way.
struct ExternType {
    std::string name;
    std::string csType, tsType, pyType;   // type-spelling templates; empty -> emitter falls back to the bare name
    std::string csCtor, tsCtor, pyCtor;   // ctor templates; empty -> no bound constructor (use a plain `new Name(…)`)
};

struct Module {
    std::vector<Enum> enums;
    std::vector<Union> unions;
    std::vector<Record> records;
    std::vector<Class> classes;
    std::vector<Interface> interfaces;
    std::vector<Global> globals;
    std::vector<Function> extensions; // `extension fn T.m(...)` — each isExtension, params[0] is `self`
    std::vector<Function> functions;
    std::vector<ExternType> externTypes; // native-backed `extern class` type/ctor spellings (see ExternType)
};

// A stable, deterministic textual dump of the typed IR (for inspection and the P4 gate).
std::string dump(const Module& m);

} // namespace mintplayer::polyglot::ir
