#pragma once

#include <functional>
#include <string>
#include <vector>

#include "mintplayer/polyglot/backend_engine.hpp"
#include "mintplayer/polyglot/backend_spec.hpp"
#include "mintplayer/polyglot/ir.hpp"

// Shared walk machinery for the hand-written backends — the seed of the P9 `SpecEmitter` engine (see
// docs/design/backend-spec.md). It owns the byte-identical output buffer + indentation state and the
// statement dispatch whose spelling is the same across C# and TS; everything target-specific is reached
// through the pure-virtual hooks the concrete emitters override. As later P9 slices lift more shared
// structure up here, the two emitters shrink toward `{spec data + a handful of hooks}`.
//
// Implementation in emitter_base.cpp (this repo keeps logic in .cpp; headers stay declaration-only).

namespace mintplayer::polyglot {

// BlockStyle now lives in backend_spec.hpp — it is per-target Spec data (see BackendSpec::blockStyle).

// The shared EvalContext over an ir::Expr + a BackendSpec — the seam that plugs the P18 rule interpreter
// into the real IR. Everything target-INDEPENDENT lives here: the path->IR mapping (`node.lhs`,
// `node.args.<i>`, `node.fields.<i>.name`, `spec.delimited.…`, …), the binary-operand precedence policy
// for `side:"l"/"r"`, and the spec-driven builtins (intSuffix / escapeString / opSpelling). A concrete
// backend subclasses with only what genuinely differs per target: extra scalar reads (semantic predicates
// its rules test), its own builtins (keyword escaping, type rendering, numeric faithfulness), and its
// atom-wrapping policy (which `recv`/`unary` children need parens).
class IrExprCtx : public engine::EvalContext {
public:
    using EmitFn = std::function<std::string(const ir::Expr&)>;
    using InlineFn = std::function<std::string(const std::vector<ir::StmtPtr>&)>;
    IrExprCtx(const ir::Expr& e, const BackendSpec& spec, EmitFn emit, InlineFn inlineBlock = {})
        : e_(e), spec_(spec), emit_(std::move(emit)), inline_(std::move(inlineBlock)) {}

    std::string get(const std::string& path) const override;
    bool has(const std::string& path) const override { return !get(path).empty(); }
    std::string emitChild(const std::string& path, const std::string& side) const override;
    std::string builtin(const std::string& name, const std::vector<std::string>& args) const override;
    // {"type":path}: resolve the TypeRef the path names, render it via the backend's (rule-driven) type
    // renderer. The shared part is path->TypeRef; the rendering is the per-target renderTypeRef.
    std::string renderType(const std::string& path) const override;

protected:
    virtual std::string targetGet(const std::string& /*path*/) const { return ""; } // per-target scalar reads
    virtual std::string targetBuiltin(const std::string& name, const std::vector<std::string>& args) const = 0;
    virtual bool wrapAtom(const ir::Expr& child, const std::string& side) const = 0; // "recv"/"unary" parens
    virtual std::string renderTypeRef(const TypeRef& t) const = 0;                   // the target's type renderer

    // The child ir::Expr a rule path names (`node.lhs`, indexed `node.args.<i>`, …); nullptr when the
    // path doesn't name a child of this node kind.
    const ir::Expr* childExpr(const std::string& path) const;
    // The TypeRef a rule path names (`node.type`, `node.params.<i>.type`); nullptr when it names none.
    const TypeRef* typeRefAt(const std::string& path) const;

    const ir::Expr& e_;
    const BackendSpec& spec_;
    EmitFn emit_;
    InlineFn inline_;
};

// The decl-scoped context a DECLARATION rule evaluates against: path reads over one IR declaration
// (`decl.name`, `decl.cases.<i>.value`, …) plus the statement lists a `{"stmts":path}` rule renders.
// Backends subclass per declaration kind; target-independent decls (Enum) share one context.
class IrDeclCtx : public engine::EvalContext {
public:
    std::string get(const std::string&) const override { return ""; }
    bool has(const std::string& path) const override { return !get(path).empty(); }
    std::string emitChild(const std::string&, const std::string&) const override { return ""; }
    std::string builtin(const std::string&, const std::vector<std::string>&) const override { return ""; }
    // The ir statement list a `{"stmts":path}` decl rule renders; nullptr when the path names none.
    virtual const std::vector<ir::StmtPtr>* stmtList(const std::string&) const { return nullptr; }
};

// Shared decl context for enums — pure name/value data, identical for every target.
class EnumDeclCtx : public IrDeclCtx {
public:
    explicit EnumDeclCtx(const ir::Enum& e) : e_(e) {}
    std::string get(const std::string& path) const override;

private:
    const ir::Enum& e_;
};

// The per-target hooks a declaration context reads through — ONE instance per backend serves every decl
// kind (type rendering, keyword escaping, the generics/bounds spellings). These are the declaration
// layer's fixed builtins; the shapes around them are rule data.
class DeclHooks {
public:
    virtual ~DeclHooks() = default;
    virtual std::string renderTypeRef(const TypeRef& t) const = 0;
    virtual std::string ident(const std::string& n) const { return n; }
    virtual std::string generics(const std::vector<ir::GenericParam>& /*gs*/) const { return ""; }
    virtual std::string where(const std::vector<ir::GenericParam>& /*gs*/) const { return ""; }
};

// Shared decl context for unions: name/case/field reads; `generics`/`ident` builtins + field types go
// through the hooks. (A union's case records reuse the union's generic params, so `{"fn":"generics"}` is
// scope-independent — it always spells the union's list.)
class UnionDeclCtx : public IrDeclCtx {
public:
    UnionDeclCtx(const ir::Union& u, const DeclHooks& hooks) : u_(u), hooks_(hooks) {}
    std::string get(const std::string& path) const override;
    std::string builtin(const std::string& name, const std::vector<std::string>& args) const override;
    std::string renderType(const std::string& path) const override;

private:
    const ir::Union& u_;
    const DeclHooks& hooks_;
};

// Shared decl context for interfaces: name/method/param reads; base/return/param types render through the
// hooks. `generics` spells the interface's list with no args, a method's with the method index as its arg
// (`{"fn":"generics","args":[{"get":"item.#"}]}`); `where` spells the interface's bounds. A param default
// re-enters the expression walk via `{"emit":"item.default"}`.
class InterfaceDeclCtx : public IrDeclCtx {
public:
    using EmitFn = std::function<std::string(const ir::Expr&)>;
    InterfaceDeclCtx(const ir::Interface& it, const DeclHooks& hooks, EmitFn emit)
        : it_(it), hooks_(hooks), emit_(std::move(emit)) {}
    std::string get(const std::string& path) const override;
    std::string builtin(const std::string& name, const std::vector<std::string>& args) const override;
    std::string renderType(const std::string& path) const override;
    std::string emitChild(const std::string& path, const std::string& side) const override;

private:
    const ir::Interface& it_;
    const DeclHooks& hooks_;
    EmitFn emit_;
};

// Shared decl context for one record/class member: kind/name/flag/param reads; return/param types render
// through the hooks; `generics`/`where` spell the METHOD's own list; the expression body and param
// defaults re-enter the expression walk; `{"stmts":"decl.body"}` renders the block body. `decl.owner` is
// the containing type's name (the C# operator shape needs it for its `(Owner lhs, …)` first operand).
class MethodDeclCtx : public IrDeclCtx {
public:
    using EmitFn = std::function<std::string(const ir::Expr&)>;
    MethodDeclCtx(const ir::Method& m, std::string owner, const DeclHooks& hooks, EmitFn emit)
        : m_(m), owner_(std::move(owner)), hooks_(hooks), emit_(std::move(emit)) {}
    std::string get(const std::string& path) const override;
    std::string builtin(const std::string& name, const std::vector<std::string>& args) const override;
    std::string renderType(const std::string& path) const override;
    std::string emitChild(const std::string& path, const std::string& side) const override;
    const std::vector<ir::StmtPtr>* stmtList(const std::string& path) const override;

private:
    const ir::Method& m_;
    std::string owner_;
    const DeclHooks& hooks_;
    EmitFn emit_;
};

// The type-scoped context the "Type" rule evaluates against: shared TypeRef reads (`type.kind`/`.name`/
// `.nullable`/`.scalar`/`.args.count`/`.returnsUnit`…) + child-type recursion (`type.args.<i>`, `type.base`,
// `type.ret` re-enter the target's renderer). Per-target: extra predicates (`type.isValueType`), the extern
// type-template selection, and the recursion target. `substExtern` (the `$0`/`$1` substitution into an
// `extern class`'s declared spelling) is a fixed builtin implemented here over the virtuals.
class TypeRefCtx : public engine::EvalContext {
public:
    TypeRefCtx(const TypeRef& t, const BackendSpec& spec) : t_(t), spec_(spec) {
        if (t_.nullable) { base_ = t_; base_.nullable = false; }
    }

    std::string get(const std::string& path) const override;
    bool has(const std::string& path) const override { return !get(path).empty(); }
    std::string emitChild(const std::string&, const std::string&) const override { return ""; } // no expr children
    std::string builtin(const std::string& name, const std::vector<std::string>& args) const override;
    std::string renderType(const std::string& path) const override;

protected:
    virtual std::string targetGet(const std::string& /*path*/) const { return ""; }
    virtual std::string externTemplate() const { return ""; }        // the extern-class spelling, "" if none
    virtual std::string renderTypeRef(const TypeRef& t) const = 0;   // recursion -> the target's renderer

    const TypeRef& t_;
    const BackendSpec& spec_;
    TypeRef base_; // the nullable-stripped copy `type.base` recurses into (valid only when t_.nullable)
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

    // Interpret a DECLARATION rule (P19): the decl-flavor kinds (`line`/`block`/`mapDecl`/`stmts`/`seq` +
    // `case`/`call`/bare string rules as lines) write indented lines through this emitter's own
    // line/openBlock machinery; string-flavor payloads (heads, line content) evaluate via evalRule against
    // `ctx`. `root` resolves `{"stmts":path}` statement lists (through any `mapDecl` item scoping, via
    // resolvePath).
    void runDeclRule(const engine::Rule& r, const engine::EvalContext& ctx, const IrDeclCtx& root,
                     const engine::RuleTable* helpers);

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
    //   (b) The Spec — all per-target *data* the engine consults (block style, statement terminator, throw
    //       keyword, plus the type/literal/operator/bracket tables the concrete emitters read). One accessor
    //       replaces the former blockStyle()/stmtEnd()/throwKeyword() constant-hooks: those were data, and
    //       data belongs in the Spec.
    virtual const BackendSpec& spec() const = 0;
    //
    //   (c) Fine-grained spelling hooks with real per-target logic (not constants): the local-declaration
    //       keyword, the yield form, and the value-less rethrow.
    virtual std::string localDecl(const std::string& name, bool isMutable) = 0; // `var x`/`let|const x`/`x` (Let, Use)
    virtual std::string yieldStmt(const std::string& value, bool hasValue) = 0; // `yield return v;`/`yield v;` …
    virtual std::string rethrowStmt() = 0;                                      // value-less `throw;`/`throw __e;`

public:
    virtual ~EmitterBase() = default;
};

} // namespace mintplayer::polyglot
