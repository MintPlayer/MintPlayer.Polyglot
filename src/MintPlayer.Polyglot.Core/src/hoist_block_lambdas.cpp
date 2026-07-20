#include "mintplayer/polyglot/hoist_block_lambdas.hpp"

// Global anonymous namespace so unqualified node/enum names resolve to `ir::` (the AST defines same-named
// types in `mintplayer::polyglot`, which would otherwise shadow them — see capture_analysis.cpp).
namespace {

using namespace mintplayer::polyglot::ir;

// Rewrites block-lambda expressions to hoisted `LocalFunc` defs. A block lambda anywhere in a statement's
// expression subtree is replaced by a `Var` referencing a fresh def, and the def is inserted immediately
// before that statement. Nested statement lists (if/for/…) hoist internally, so a def lands in the same
// block as its use — preserving per-iteration/scope semantics.
class Hoister {
public:
    void run(Module& m) {
        for (auto& f : m.functions)  if (!f.exprBodied) hoistStmts(f.body);
        for (auto& f : m.extensions) if (!f.exprBodied) hoistStmts(f.body);
        for (auto& r : m.records) for (auto& mm : r.methods) if (!mm.exprBodied) hoistStmts(mm.body);
        for (auto& c : m.classes) {
            for (auto& mm : c.methods) if (!mm.exprBodied) hoistStmts(mm.body);
            if (c.hasInit) hoistStmts(c.initBody);
        }
    }

private:
    int counter_ = 0;
    // The `__`-prefix matches the existing lowering gensym convention (`__opt`/`__w`) the codebase reserves.
    std::string fresh() { return "__pg_lam" + std::to_string(counter_++); }

    void hoistStmts(std::vector<StmtPtr>& stmts) {
        std::vector<StmtPtr> out;
        out.reserve(stmts.size());
        for (auto& s : stmts) {
            std::vector<StmtPtr> defs;
            hoistStmt(*s, defs);
            for (auto& d : defs) out.push_back(std::move(d));
            out.push_back(std::move(s));
        }
        stmts = std::move(out);
    }

    void hoistStmt(Stmt& s, std::vector<StmtPtr>& defs) {
        switch (s.kind) {
        case StmtKind::Let:      hoistExpr(static_cast<Let&>(s).init, defs); break;
        case StmtKind::Assign: { auto& a = static_cast<Assign&>(s); hoistExpr(a.target, defs); hoistExpr(a.value, defs); break; }
        case StmtKind::IndexAssign: { auto& ia = static_cast<IndexAssign&>(s); hoistExpr(ia.receiver, defs); for (auto& ix : ia.indices) hoistExpr(ix, defs); hoistExpr(ia.value, defs); break; }
        case StmtKind::ExprStmt: hoistExpr(static_cast<ExprStmt&>(s).expr, defs); break;
        case StmtKind::If:     { auto& i = static_cast<If&>(s); hoistExpr(i.cond, defs); hoistStmts(i.thenBody); hoistStmts(i.elseBody); break; }
        case StmtKind::While:  { auto& w = static_cast<While&>(s); hoistExpr(w.cond, defs); hoistStmts(w.body); break; }
        case StmtKind::For:    { auto& f = static_cast<For&>(s);
                                 if (f.rangeStart) hoistExpr(f.rangeStart, defs);
                                 if (f.rangeEnd) hoistExpr(f.rangeEnd, defs);
                                 if (f.iterable) hoistExpr(f.iterable, defs);
                                 hoistStmts(f.body); break; }
        case StmtKind::Return: { auto& r = static_cast<Return&>(s); if (r.value) hoistExpr(r.value, defs); break; }
        case StmtKind::Yield:  { auto& y = static_cast<Yield&>(s);  if (y.value) hoistExpr(y.value, defs); break; }
        case StmtKind::Throw:  { auto& t = static_cast<Throw&>(s);  if (t.value) hoistExpr(t.value, defs); break; }
        case StmtKind::Use:    { auto& u = static_cast<Use&>(s); if (u.init) hoistExpr(u.init, defs); hoistStmts(u.body); break; }
        case StmtKind::Try:    { auto& t = static_cast<Try&>(s);
                                 hoistStmts(t.body);
                                 for (auto& c : t.catches) { if (c.guard) hoistExpr(c.guard, defs); hoistStmts(c.body); }
                                 hoistStmts(t.finallyBody); break; }
        case StmtKind::LocalFunc: hoistStmts(static_cast<LocalFunc&>(s).body); break; // not in input; safe
        case StmtKind::Break:
        case StmtKind::Continue: break;
        }
    }

    // Rewrite block lambdas in `slot` (and its subtree), appending their defs to `defs`.
    void hoistExpr(ExprPtr& slot, std::vector<StmtPtr>& defs) {
        if (!slot) return;
        if (slot->kind == ExprKind::Lambda) {
            auto& lam = static_cast<Lambda&>(*slot);
            if (!lam.exprBodied) {
                hoistStmts(lam.block); // hoist nested block lambdas into this def's body first
                const auto pos = lam.pos;
                Type type = slot->type;
                auto lf = std::make_unique<LocalFunc>(pos, fresh());
                lf->params.swap(lam.params);
                for (const auto& c : lam.captures) if (c.mutatedInside) lf->nonlocals.push_back(c.name);
                lf->body.swap(lam.block);
                std::string name = lf->name;
                defs.push_back(std::move(lf));
                slot = std::make_unique<Var>(pos, std::move(type), name); // destroys the old Lambda
                return;
            }
            hoistExpr(lam.body, defs); // expr-bodied lambda: recurse into its body
            return;
        }
        switch (slot->kind) {
        case ExprKind::Unary:  hoistExpr(static_cast<Unary&>(*slot).operand, defs); break;
        case ExprKind::Await:  hoistExpr(static_cast<Await&>(*slot).operand, defs); break;
        case ExprKind::Cast:   hoistExpr(static_cast<Cast&>(*slot).operand, defs); break;
        case ExprKind::Binary: { auto& b = static_cast<Binary&>(*slot); hoistExpr(b.lhs, defs); hoistExpr(b.rhs, defs); break; }
        case ExprKind::Cond:   { auto& c = static_cast<Cond&>(*slot); hoistExpr(c.cond, defs); hoistExpr(c.then, defs); hoistExpr(c.els, defs); break; }
        case ExprKind::Call:   for (auto& a : static_cast<Call&>(*slot).args) hoistExpr(a, defs); break;
        case ExprKind::MethodCall: { auto& mc = static_cast<MethodCall&>(*slot); if (mc.object) hoistExpr(mc.object, defs); for (auto& a : mc.args) hoistExpr(a, defs); break; }
        case ExprKind::Member: { auto& m = static_cast<Member&>(*slot); if (m.object) hoistExpr(m.object, defs); break; }
        case ExprKind::New:    for (auto& a : static_cast<New&>(*slot).args) hoistExpr(a, defs); break;
        case ExprKind::Index:  { auto& i = static_cast<Index&>(*slot); hoistExpr(i.receiver, defs); for (auto& ix : i.indices) hoistExpr(ix, defs); break; }
        case ExprKind::ListLit: for (auto& x : static_cast<ListLit&>(*slot).elements) hoistExpr(x, defs); break;
        case ExprKind::Tuple:  for (auto& x : static_cast<Tuple&>(*slot).elements) hoistExpr(x, defs); break;
        case ExprKind::Bound:  { auto& b = static_cast<Bound&>(*slot); if (b.receiver) hoistExpr(b.receiver, defs); for (auto& a : b.args) hoistExpr(a, defs); break; }
        case ExprKind::MakeCase: for (auto& f : static_cast<MakeCase&>(*slot).fields) hoistExpr(f.value, defs); break;
        case ExprKind::Interp: for (auto& h : static_cast<Interp&>(*slot).holes) hoistExpr(h, defs); break;
        case ExprKind::With:   { auto& w = static_cast<With&>(*slot); hoistExpr(w.base, defs); for (auto& f : w.fields) hoistExpr(f.value, defs); break; }
        case ExprKind::Match:  { auto& m = static_cast<Match&>(*slot);
                                 hoistExpr(m.scrutinee, defs);
                                 for (auto& arm : m.arms) { if (arm.guard) hoistExpr(arm.guard, defs); hoistExpr(arm.body, defs); } break; }
        default: break; // literals, Var, This, Extern
        }
    }
};

} // anonymous namespace

namespace mintplayer::polyglot {

void hoistBlockLambdas(ir::Module& m) {
    Hoister h;
    h.run(m);
}

} // namespace mintplayer::polyglot
