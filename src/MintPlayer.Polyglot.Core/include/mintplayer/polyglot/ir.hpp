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
enum class ExprKind { Int, Float, Bool, Str, Char, Null, Var, This, Unary, Await, Binary, Cast, Call, MethodCall, Member, New, MakeCase, Match, Lambda, Extern, Index, ListLit, Tuple, Bound, Interp, Cond, With, IsTest, AsCast };

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
    // Capture analysis (P25 §4.18): this reference is to a variable boxed into a shared cell (its declaration
    // is `needsCell`). A value-capture target (Python cell object) reads/writes through the cell; a
    // native-by-reference target (C#/TS) ignores it and emits the plain name. Node-local, like `lhsIsRecord`.
    bool throughCell = false;
    Var(SourcePos p, Type t, std::string n) : Expr(ExprKind::Var, p, std::move(t)), name(std::move(n)) {}
};
struct This : Expr {
    // Lowering-precomputed fact (P19): inside an operator method's body. Target-neutral; only the C#
    // static-operator declaration shape consumes it (`this` -> the `lhs` first operand).
    bool insideOperator = false;
    explicit This(SourcePos p, Type t) : Expr(ExprKind::This, p, std::move(t)) {}
};
struct Unary : Expr {
    std::string op;
    ExprPtr operand;
    Unary(SourcePos p, Type t, std::string o, ExprPtr e) : Expr(ExprKind::Unary, p, std::move(t)), op(std::move(o)), operand(std::move(e)) {}
};
struct Await : Expr { // `await e` — only inside an async fn; `type` is the unwrapped result (identity in v1)
    ExprPtr operand;
    Await(SourcePos p, Type t, ExprPtr e) : Expr(ExprKind::Await, p, std::move(t)), operand(std::move(e)) {}
};
struct Binary : Expr {
    std::string op;
    ExprPtr lhs, rhs;
    // Module facts precomputed in lowering (P19), keeping the emit layer node-local: the lhs type is a
    // user record (structural `==` -> `.equals` on TS) / any named non-scalar type (operator-overload
    // dispatch on targets without native operators).
    bool lhsIsRecord = false;
    bool lhsIsUserType = false;
    // Equality model (#49/#57): the lhs type is a union (structural `==` over its cases, like a record), and
    // the lhs type declares a user `operator fn eq` — which wins over the default structural/reference
    // equality on every target (TS/Py/PHP call `.eq(...)`; C# emits the user eq as `override Equals`).
    bool lhsIsUnion = false;
    bool lhsHasUserEq = false;
    Binary(SourcePos p, Type t, std::string o, ExprPtr l, ExprPtr r)
        : Expr(ExprKind::Binary, p, std::move(t)), op(std::move(o)), lhs(std::move(l)), rhs(std::move(r)) {}
};
struct Cast : Expr { // numeric conversion: the result type is `type`, the source is `operand->type`
    ExprPtr operand;
    Cast(SourcePos p, Type t, ExprPtr o) : Expr(ExprKind::Cast, p, std::move(t)), operand(std::move(o)) {}
};
// P37 B: `x is T` — a runtime type test over a class/record hierarchy; type bool. Pure predicate: an
// `if`-condition binding form is desugared in lowering (hoisted `AsCast` Let + null check), so every
// target emits its native test (C# `is`, TS/PHP `instanceof`, Python `isinstance`).
struct IsTest : Expr {
    ExprPtr operand;
    Type testType;
    IsTest(SourcePos p, Type t, ExprPtr o, Type tt)
        : Expr(ExprKind::IsTest, p, std::move(t)), operand(std::move(o)), testType(std::move(tt)) {}
};
// P37 B: `x as T` — checked conversion; `type` is T with `nullable=true`, null on failure, NEVER throws
// (M1: a runtime guard on TS/Python/PHP — never a bare TS `as` assertion). Targets that re-evaluate the
// operand in their guard bind `tempName` inline when the operand isn't a simple var (the `With` pattern):
// TS an IIFE parameter, Python a walrus, PHP an assignment expression; C# `as` never needs it.
struct AsCast : Expr {
    ExprPtr operand;
    Type castType;
    bool operandIsSimple = true;
    std::string tempName; // set iff !operandIsSimple
    AsCast(SourcePos p, Type t, ExprPtr o, Type ct)
        : Expr(ExprKind::AsCast, p, std::move(t)), operand(std::move(o)), castType(std::move(ct)) {}
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
    // The accessed member is a computed property (a getter). Native-property targets (C#/TS/Python) ignore
    // this and emit a plain field access; a target that emulates properties as methods (PHP) emits a call
    // `recv->field()`. A target-neutral fact stamped at lowering from the receiver type's members.
    bool isProperty = false;
    Member(SourcePos p, Type t, ExprPtr o, std::string f, bool ns)
        : Expr(ExprKind::Member, p, std::move(t)), object(std::move(o)), field(std::move(f)), nullSafe(ns) {}
};
struct New : Expr { // construction `Type(args)` or `Type<TypeArgs>(args)`
    std::string typeName;
    std::vector<Type> typeArgs; // explicit construction type args, e.g. `Box<i32>(7)` (empty = none)
    std::vector<ExprPtr> args;
    New(SourcePos p, Type t, std::string n) : Expr(ExprKind::New, p, std::move(t)), typeName(std::move(n)) {}
};

struct Index : Expr { // element access `receiver[a]` or multi-arg indexer `receiver[a, b]`
    ExprPtr receiver;
    std::vector<ExprPtr> indices; // 1+ subscript args in source order; single-arg (`List[i]`) is the hot path
    // Precomputed in lowering (P19): the receiver's type declares an `operator fn get` — a target without
    // `[]` overloading (TS) emits `recv.get(a, b)` instead of a subscription.
    bool receiverHasIndexer = false;
    Index(SourcePos p, Type t, ExprPtr r)
        : Expr(ExprKind::Index, p, std::move(t)), receiver(std::move(r)) {}
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
struct With : Expr { // record copy `base with { f = v, … }`: C# native `with`; TS/Python rebuild via the ctor
    ExprPtr base;
    std::vector<WithField> fields; // the overridden fields (others copied from base) — C#'s native form
    // The ctor-rebuild, precomputed in lowering (P19): the record's fields in declaration (ctor) order,
    // each the override expression or a `<base>.field` read. A non-simple base is bound once to `tempName`
    // for single evaluation (TS IIFE / Python lambda) and the reads reference it; a simple (Var) base is
    // read directly and `tempName` is empty.
    std::vector<ExprPtr> ctorArgs;
    std::string tempName;
    bool baseIsSimple = false;
    With(SourcePos p, Type t, ExprPtr b) : Expr(ExprKind::With, p, std::move(t)), base(std::move(b)) {}
};
// A bound call/access: a portable std method/property resolved to THE ACTIVE TARGET's FFI template
// (lowering knows the target and picks its arm — P19 slice 9; the per-target triple is gone). The backend
// substitutes `$this`->receiver and `$0`,`$1`,…->args. A `$this = …` template emits a receiver assignment
// (e.g. List.clear -> TS `xxx = []`). See ast::TargetBinding.
struct Bound : Expr {
    ExprPtr receiver;
    std::vector<ExprPtr> args;
    std::string tmpl; // the active target's template ("" only when the pre-lowering refusal was bypassed)
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
enum class PatternKind { Wildcard, Literal, Binding, EnumCase, Ctor, TypeTest };
struct Binder { // one sub-slot of a constructor pattern, in declaration order (issue #43)
    std::string field;    // the case field this slot matches (declared field name)
    std::string binding;  // Binding slot: the bound name; empty when this slot is a literal or wildcard
    ExprPtr literal;       // Literal slot: the constant this slot must equal (null otherwise) — `Leaf(0)`
    // A slot with neither binding nor literal is a wildcard (`Leaf(_)`): matches anything, binds nothing.
};
struct Pattern {
    PatternKind kind = PatternKind::Wildcard;
    std::string binding;            // Binding: the bound name
    ExprPtr literal;                // Literal: the constant
    std::string enumType, enumCase; // EnumCase: Type.Case
    std::string ctorCase;           // Ctor: the union case name
    std::vector<Binder> binders;    // Ctor: field -> binding (in declared order)
    Type testType;                  // TypeTest: the runtime type tested (`d: Disk` -> C# `Disk d`, else instanceof/isinstance) (#38)
};
struct MatchArm {
    Pattern pattern;
    ExprPtr guard; // optional
    ExprPtr body;
};
struct Match : Expr {
    ExprPtr scrutinee;
    std::vector<MatchArm> arms;
    // Precomputed in lowering (P19): some arm is an unguarded wildcard/binding. Sema guarantees
    // exhaustiveness, but C# can't prove it for enums — without a catch-all, the C# rule appends an
    // unreachable `_ => throw` default so the switch expression compiles without CS8524.
    bool hasCatchAll = false;
    // The match is the whole statement (its value is discarded), so its arms are void/side-effecting.
    // C# then needs a switch STATEMENT, not a switch expression (which can't have void arms and isn't a
    // statement — CS0201/CS0029); the other targets' IIFE works in either position (issue #52).
    bool isStatement = false;
    Match(SourcePos p, Type t, ExprPtr s) : Expr(ExprKind::Match, p, std::move(t)), scrutinee(std::move(s)) {}
};

// ---- statements ----
enum class StmtKind { Let, Assign, IndexAssign, ExprStmt, If, While, For, Return, Break, Continue, Yield, Throw, Try, Use, LocalFunc };

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
    // Capture analysis (P25 §4.18): this local is captured by a lambda, and `needsCell` means at least one
    // capture requires a shared mutable binding (mutated through the closure, or reassigned while a closure
    // holds it) — so a value-capture target allocates a cell here instead of a plain variable.
    bool captured = false;
    bool needsCell = false;
    // The source carried an explicit annotation (wave-2 G-library): the checker STAMPS the initializer's
    // type from the annotation (bare union cases, list literals), so comparing declared-vs-initializer
    // types can no longer tell that the annotation added information — this flag preserves the fact.
    bool declExplicit = false;
    // `let (a, b) = t` destructuring targets (empty = an ordinary single-name let). Emitted via the
    // per-target TupleLet rule: C# `var (a,b)=`, TS `const [a,b]=`, Python `a, b =`, PHP `[$a,$b]=` (#39b).
    std::vector<std::string> tupleNames;
    Let(SourcePos p, std::string n, bool m, Type t, ExprPtr i)
        : Stmt(StmtKind::Let, p), name(std::move(n)), isMutable(m), type(std::move(t)), init(std::move(i)) {}
};
struct Assign : Stmt {
    ExprPtr target;
    std::string op;
    ExprPtr value;
    // Capture analysis (P25 §4.18): the assignment target is a variable boxed into a shared cell — a
    // value-capture target writes through the cell (mirrors ir::Var.throughCell for the write side).
    bool targetThroughCell = false;
    Assign(SourcePos p, ExprPtr t, std::string o, ExprPtr v)
        : Stmt(StmtKind::Assign, p), target(std::move(t)), op(std::move(o)), value(std::move(v)) {}
};
// A write through a USER indexer `recv[a, b] = v` (issue #40). Kept distinct from Assign so each target
// renders it its own way: C# native `recv[a, b] = v` (paired into the merged `this[...]{get;set;}`),
// while targets that emulate operators as methods call `recv.set(a, b, v)`. A plain collection write
// (`xs[i] = v`, no user indexer) stays an ordinary Assign — only a user-indexer receiver lowers here.
struct IndexAssign : Stmt {
    ExprPtr receiver;
    std::vector<ExprPtr> indices;
    ExprPtr value;
    IndexAssign(SourcePos p) : Stmt(StmtKind::IndexAssign, p) {}
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
    bool isDoWhile = false; // a post-tested `do { … } while (cond)` loop (#39a)
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
    // Capture analysis (P25 §4.18): the loop binding is captured by a lambda created in the body. `needsCell`
    // means the cell is allocated **per iteration** (inside the loop body), so each closure closes over its
    // own binding — matching C#/JS `let`/`foreach` per-iteration semantics (`() => i` over 1..=3 → 1,2,3).
    bool captured = false;
    bool needsCell = false;
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
    // Capture analysis (P25 §4.18): this parameter is captured by a nested lambda; `needsCell` if that
    // capture needs a shared mutable binding (the param is assigned somewhere its closures can observe).
    bool captured = false;
    bool needsCell = false;
};
struct GenericParam {
    std::string name;
    std::vector<Type> bounds; // `T: A & B` — C# `where T : A, B`, TS `<T extends A & B>`
};

// A lambda / closure: `(params) => expr` or `=> { block }`. Both targets emit a native arrow function;
// a param's type is omitted from the emit when absent (the bare `x => …` form relies on target typing).
// Defined here (not with the other Expr nodes) because it needs Param + StmtPtr.
// One captured variable of a lambda (P25 §4.18), classified by the capture-analysis pass. `needsCell` is the
// authoritative decision backends consume (never re-derive): true = the variable must be a shared
// reference/cell (mutated through a closure, reassigned while a closure holds it, or self-referential);
// false = SNAPSHOT, a by-value copy is faithful. `mutatedInside`/`reassignedOutside` are provenance for the
// SHARED-RW vs SHARED-RO distinction some targets tune on. `isThis` never needs a cell (identity capture).
struct Capture {
    std::string name;
    Type declType;
    bool isThis = false;
    bool needsCell = false;
    bool mutatedInside = false;     // written through this (or a nested) lambda
    bool reassignedOutside = false; // the source variable is assigned somewhere outside this lambda
};
struct Lambda : Expr {
    std::vector<Param> params;
    bool exprBodied = true;     // `=> expr` vs `=> { block }`
    ExprPtr body;               // exprBodied
    std::vector<StmtPtr> block; // block body
    // Capture analysis (P25 §4.18): the classified free variables this lambda captures (deduped by binding,
    // first-use order); `capturesThis` forces the lexical-receiver form (TS arrow not `function`, PHP `$this`,
    // C++ `[this]`); `escapes` (the closure outlives its frame) is the load-bearing input for C++/Rust cell
    // decisions — conservatively true until precise escape analysis lands with those targets.
    std::vector<Capture> captures;
    bool capturesThis = false;
    bool escapes = true;
    Lambda(SourcePos p, Type t) : Expr(ExprKind::Lambda, p, std::move(t)) {}
};
// A hoisted nested function — the Python lowering of a **block lambda** (Python `lambda` is expression-only,
// so `(a) => { stmts }` can't stay inline). Emitted as `def <name>(<params>):` with a `nonlocal <names>`
// line for the captures it *mutates*. Created ONLY by the Python block-lambda hoist pass (§4.18); every other
// target emits block lambdas inline and never sees this node — hence it stays off the anti-silent-drop
// coverage table, and only the Python plugin needs a rule for it. Defined here (like `Lambda`) because it
// needs `Param` + `StmtPtr`.
struct LocalFunc : Stmt {
    std::string name;
    std::vector<Param> params;
    std::vector<std::string> nonlocals; // captured variables assigned inside the body (need Python `nonlocal`)
    std::vector<StmtPtr> body;
    LocalFunc(SourcePos p, std::string n) : Stmt(StmtKind::LocalFunc, p), name(std::move(n)) {}
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
    bool isAsync = false;    // `async fn` — C# `async Task<T>` / TS `async…Promise<T>` / Python `async def` (§4.7)
    bool isExtension = false; // an `extension fn T.m(...)`: params[0] is the receiver `self` (type T)
    std::string actualTarget; // `actual(<target>)` impl — emitted only by the matching backend; else empty
    bool exprBodied = false; // `=> expr` vs block (extensions; regular fns always use a block today)
    ExprPtr exprBody;        // exprBodied
    std::string originModule; // module linking (§4.5): "" = entry-own, a canonical path = imported user module, "<prelude>" = std/core/lib
    // Top-level module globals this function's body references (unshadowed), in first-use order. A
    // target-neutral fact computed at lowering; consumed only where module globals aren't visible in
    // function scope (PHP emits `global $x;`). Empty / ignored on C#/TS/Python.
    std::vector<std::string> globalRefs;
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
    bool isAsync = false;         // `async` method — return wrapped in Task/Promise/async def (§4.7)
    bool isVirtual = false;       // `open fn` — C# `virtual` (overridable); SPEC §grammar 210-213
    bool isOverride = false;      // `override fn` — C# `override` (replaces a base virtual)
    bool isIterator = false;      // body has a `yield` — C# `IEnumerable`, TS generator `*method()` (§4.x)
    ExprPtr exprBody;
    std::vector<StmtPtr> body;
    // Indexer pairing (issue #40): on a `get` indexer operator, a non-owning pointer to the sibling `set`
    // operator in the same class, so C# can MERGE them into one `this[...] { get; set; }` (the set method
    // still owns its own body — it also emits standalone as `.set(...)` on the method-emulation targets).
    // Stable: set after the class's `methods` vector is fully built and never reallocated afterwards.
    const Method* pairedSetter = nullptr;
    // Property accessor-block setter (#39c): a read-write computed property carries its setter here (the
    // getter is exprBody/body). C# emits `{ get …; set { … } }`, TS/Python native get/set accessors, PHP a
    // getter+setter method pair (property-set assignments route to the setter).
    bool propHasSetter = false;
    std::string setterParamName;         // the setter's value parameter (default `value`)
    std::vector<StmtPtr> setterBody;
    // Top-level module globals this member's body (incl. a property getter's expr) references — same
    // fact as ir::Function.globalRefs; consumed only by PHP (`global $x;`), empty/ignored elsewhere.
    std::vector<std::string> globalRefs;
};
struct Record { // an immutable data type (record)
    std::string name;
    std::vector<GenericParam> generics;
    std::vector<Type> bases;        // implemented interfaces
    std::vector<RecordField> fields;
    std::vector<Method> methods;
    std::string originModule; // module linking (§4.5): see ir::Function.originModule
};
struct EnumCase {
    std::string name;
    long long value;
};
struct Enum {
    std::string name;
    std::vector<EnumCase> cases;
    std::string originModule; // module linking (§4.5): see ir::Function.originModule
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
    std::string originModule; // module linking (§4.5): see ir::Function.originModule
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
    // Does an ANCESTOR have a real constructor to chain up to? True if a base (transitively) is a user
    // class with an `init`, or a native/extern base (Error→`\Exception`, …) — which always has a ctor.
    // False when the only bases are interfaces or user classes with no `init`. PHP gates its
    // `parent::__construct(...)` on this: unlike C#/TS/Python, PHP has no implicit default base ctor, so a
    // `super()` to an initless base is a fatal `Cannot call constructor` (issue #48).
    bool baseHasInit = false;
    std::vector<ExprPtr> superArgs;    // the base-constructor arguments (hoisted out of initBody)
    std::vector<StmtPtr> initBody;
    std::vector<Method> methods;
    std::string originModule; // module linking (§4.5): see ir::Function.originModule
};
struct Interface { // a contract: method signatures only (no bodies). C# `interface`, TS `interface`.
    std::string name;
    std::vector<GenericParam> generics;
    std::vector<Type> bases;     // extended interfaces
    std::vector<Method> methods; // signatures (body empty)
    std::string originModule; // module linking (§4.5): see ir::Function.originModule
};
struct Global { // a top-level `const`/`let` value
    std::string name;
    bool isConst = false;
    Type type;
    ExprPtr init;
    std::string originModule; // module linking (§4.5): see ir::Function.originModule
};
// A native-backed `extern class`: how its name spells on THE ACTIVE TARGET (`$0,$1,…` = the rendered type
// args) — lowering picks the target's arm (P19 slice 9). Construction (`Type(args)` with `$T`/`$0…` ctor
// templates) lowers through ir::Bound like any binding.
struct ExternType {
    std::string name;
    std::string typeTmpl; // the active target's type-spelling template; empty -> the bare name
};

// Module linking (§4.5): one cross-module reference this file emits as a target import. `path` is the
// imported module's emitted basename; `names` is the pre-joined selective group ("a, b as c"); for a
// `* as ns` import `isNamespace` is set and `ns` holds the alias; a bare `import "x"` has all three empty.
struct ModuleImport {
    std::string path;
    std::string names;
    bool isNamespace = false;
    std::string ns;
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
    std::vector<ModuleImport> imports;   // §4.5: this file's cross-module imports (empty for single-file builds)
    bool linked = false;                 // §4.5: this is one file of a multi-module build (drives C# `partial`)
    std::string access;                  // requested accessibility for emitted C# types ("public"/"internal"); "" = target default
};

// A stable, deterministic textual dump of the typed IR (for inspection and the P4 gate).
std::string dump(const Module& m);

} // namespace mintplayer::polyglot::ir
