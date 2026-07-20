#include "mintplayer/polyglot/capture_analysis.hpp"

#include <unordered_map>
#include <vector>

// The analysis lives in the global anonymous namespace (not inside `mintplayer::polyglot`) so that
// unqualified `Stmt`/`Expr`/`StmtKind`/`ExprKind`/node names resolve to the `ir::` versions via the
// using-directive below — the AST defines same-named types in `mintplayer::polyglot`, which would otherwise
// shadow them.
namespace {

using namespace mintplayer::polyglot::ir;

// A local binding currently in scope: a param / let / for-binding (which can carry a cell), or an
// "opaque" binder (use/catch/match binding — tracked only so it shadows correctly and isn't mistaken for
// a global). `id` is a stable per-binder identity used to aggregate facts across the whole function walk.
struct Binder {
    std::string name;
    int id;
    Let* let = nullptr;
    For* forN = nullptr;
    Param* param = nullptr;
};

// Facts aggregated per binder over the whole function walk (assignment status is only final at the end).
struct BinderInfo {
    std::string name;
    Type declType;
    bool assigned = false;   // is an assignment target somewhere in the function
    bool selfRef = false;    // captured by a lambda that is (part of) this binder's own initializer
    Let* let = nullptr;
    For* forN = nullptr;
    Param* param = nullptr;
    std::vector<Var*> reads;      // every plain read of this binder (stamped throughCell if celled)
    std::vector<Assign*> writes;  // every assignment whose target is this binder (stamped targetThroughCell)
};

// One capture of a lambda, accumulated while walking its body (keyed by binder id inside the frame).
struct CapRec {
    int binderId;
    bool mutatedInside = false; // written through this (or a nested) lambda
};
struct LamRec {
    Lambda* lam;
    int initBinder;             // binder id whose initializer this lambda sits in (self-ref detection), or -1
    std::vector<CapRec> caps;
};

class Analyzer {
public:
    ~Analyzer() { for (auto* r : lamRecs_) delete r; }
    void run(Module& m) {
        for (auto& f : m.functions) analyzeFn(f.params, &f.body, nullptr);
        for (auto& f : m.extensions) analyzeFn(f.params, f.exprBodied ? nullptr : &f.body, f.exprBodied ? f.exprBody.get() : nullptr);
        for (auto& r : m.records) for (auto& mm : r.methods) analyzeMethod(mm);
        for (auto& c : m.classes) {
            for (auto& mm : c.methods) analyzeMethod(mm);
            if (c.hasInit) analyzeFn(c.initParams, &c.initBody, nullptr);
            for (auto& fld : c.fields) if (fld.init) analyzeFn({}, nullptr, fld.init.get());
        }
        for (auto& g : m.globals) if (g.init) analyzeFn({}, nullptr, g.init.get());
    }

private:
    // ---- per-function state ----
    std::vector<Binder> scopes_;                 // active binder stack; nearest = back
    std::unordered_map<int, BinderInfo> info_;   // binder id -> aggregated facts
    std::vector<LamRec*> lamRecs_;               // every lambda seen this function (owns the records)
    std::vector<std::pair<LamRec*, std::size_t>> lamStack_; // active lambdas + scope depth at entry
    int nextId_ = 0;
    int curInitBinder_ = -1;                     // binder id whose init we are inside (for self-ref)

    void reset() {
        scopes_.clear();
        info_.clear();
        for (auto* r : lamRecs_) delete r;
        lamRecs_.clear();
        lamStack_.clear();
        nextId_ = 0;
        curInitBinder_ = -1;
    }

    // A function-like body: params + a statement block and/or a single expression body.
    void analyzeFn(const std::vector<Param>& cparams, std::vector<StmtPtr>* body, Expr* exprBody) {
        reset();
        // Params are binders (const_cast: the pass stamps `captured`/`needsCell` back onto them).
        for (const auto& p : cparams) pushBinder(p.name, nullptr, nullptr, const_cast<Param*>(&p));
        if (body) walkBlock(*body);
        if (exprBody) walkExpr(exprBody);
        finalize();
    }
    void analyzeMethod(Method& mm) {
        if (mm.exprBodied) analyzeFn(mm.params, nullptr, mm.exprBody.get());
        else analyzeFn(mm.params, &mm.body, nullptr);
    }

    int pushBinder(const std::string& name, Let* l, For* f, Param* p) {
        int id = nextId_++;
        scopes_.push_back({name, id, l, f, p});
        auto& bi = info_[id];
        bi.name = name;
        bi.let = l; bi.forN = f; bi.param = p;
        if (l) bi.declType = l->type;
        else if (p) bi.declType = p->type;
        return id;
    }

    // Resolve a name to the nearest enclosing binder; returns its index in `scopes_` or -1 (a global/free).
    int resolve(const std::string& name) const {
        for (int i = (int)scopes_.size() - 1; i >= 0; --i)
            if (scopes_[i].name == name) return i;
        return -1;
    }

    // Record a read/write of the binder at `scopes_[idx]`, adding a capture to every enclosing lambda.
    void noteAccess(int idx, Var* read, Assign* write) {
        const Binder& b = scopes_[idx];
        BinderInfo& bi = info_[b.id];
        if (write) { bi.assigned = true; bi.writes.push_back(write); }
        else if (read) bi.reads.push_back(read);
        for (auto& [rec, depth] : lamStack_) {
            if ((std::size_t)idx >= depth) continue; // declared inside this lambda -> not a capture
            CapRec* cr = nullptr;
            for (auto& c : rec->caps) if (c.binderId == b.id) { cr = &c; break; }
            if (!cr) { rec->caps.push_back({b.id, false}); cr = &rec->caps.back(); }
            if (write) cr->mutatedInside = true;
            if (rec->initBinder == b.id) bi.selfRef = true; // captured inside its own initializer
        }
    }

    // ---- the walk ----
    void walkBlock(std::vector<StmtPtr>& body) {
        std::size_t mark = scopes_.size();
        for (auto& s : body) walkStmt(*s);
        scopes_.resize(mark); // pop block-local binders
    }

    void walkStmt(Stmt& s) {
        switch (s.kind) {
        case StmtKind::Let: {
            auto& l = static_cast<Let&>(s);
            int save = curInitBinder_;
            // The binder is visible in its own initializer (recursive `let f = () => f(...)`); push first so a
            // self-capture resolves, and remember it for self-ref detection.
            int id = pushBinder(l.name, &l, nullptr, nullptr);
            curInitBinder_ = id;
            if (l.init) walkExpr(l.init.get());
            curInitBinder_ = save;
            break;
        }
        case StmtKind::Assign: {
            auto& a = static_cast<Assign&>(s);
            if (a.target->kind == ExprKind::Var) {
                int idx = resolve(static_cast<Var&>(*a.target).name);
                if (idx >= 0) noteAccess(idx, nullptr, &a); // the target is a captured/celled write
                else walkExpr(a.target.get()); // unresolved (global): still descend for completeness
            } else {
                walkExpr(a.target.get());
            }
            walkExpr(a.value.get());
            break;
        }
        case StmtKind::IndexAssign: {
            auto& ia = static_cast<IndexAssign&>(s);
            walkExpr(ia.receiver.get());
            for (auto& ix : ia.indices) walkExpr(ix.get());
            walkExpr(ia.value.get());
            break;
        }
        case StmtKind::ExprStmt: walkExpr(static_cast<ExprStmt&>(s).expr.get()); break;
        case StmtKind::If: {
            auto& i = static_cast<If&>(s);
            walkExpr(i.cond.get());
            walkBlock(i.thenBody);
            walkBlock(i.elseBody);
            break;
        }
        case StmtKind::While: {
            auto& w = static_cast<While&>(s);
            walkExpr(w.cond.get());
            walkBlock(w.body);
            break;
        }
        case StmtKind::For: {
            auto& f = static_cast<For&>(s);
            if (f.isRange) { if (f.rangeStart) walkExpr(f.rangeStart.get()); if (f.rangeEnd) walkExpr(f.rangeEnd.get()); }
            else if (f.iterable) walkExpr(f.iterable.get());
            std::size_t mark = scopes_.size();
            if (!f.tupleBindings.empty()) for (const auto& n : f.tupleBindings) pushBinder(n, nullptr, &f, nullptr);
            else pushBinder(f.binding, nullptr, &f, nullptr);
            for (auto& st : f.body) walkStmt(*st);
            scopes_.resize(mark);
            break;
        }
        case StmtKind::Return: { auto& r = static_cast<Return&>(s); if (r.value) walkExpr(r.value.get()); break; }
        case StmtKind::Yield:  { auto& y = static_cast<Yield&>(s);  if (y.value) walkExpr(y.value.get()); break; }
        case StmtKind::Throw:  { auto& t = static_cast<Throw&>(s);  if (t.value) walkExpr(t.value.get()); break; }
        case StmtKind::Break:
        case StmtKind::Continue: break;
        case StmtKind::Use: {
            auto& u = static_cast<Use&>(s);
            if (u.init) walkExpr(u.init.get());
            std::size_t mark = scopes_.size();
            pushBinder(u.binding, nullptr, nullptr, nullptr);
            for (auto& st : u.body) walkStmt(*st);
            scopes_.resize(mark);
            break;
        }
        case StmtKind::Try: {
            auto& t = static_cast<Try&>(s);
            walkBlock(t.body);
            for (auto& c : t.catches) {
                if (c.guard) walkExpr(c.guard.get());
                std::size_t mark = scopes_.size();
                if (!c.binding.empty()) pushBinder(c.binding, nullptr, nullptr, nullptr);
                for (auto& st : c.body) walkStmt(*st);
                scopes_.resize(mark);
            }
            walkBlock(t.finallyBody);
            break;
        }
        }
    }

    void walkExpr(Expr* e) {
        if (!e) return;
        switch (e->kind) {
        case ExprKind::Var: {
            int idx = resolve(static_cast<Var&>(*e).name);
            if (idx >= 0) noteAccess(idx, static_cast<Var*>(e), nullptr);
            break;
        }
        case ExprKind::This:
            for (auto& [rec, depth] : lamStack_) rec->lam->capturesThis = true;
            break;
        case ExprKind::Lambda: {
            auto& lam = static_cast<Lambda&>(*e);
            auto* rec = new LamRec{&lam, curInitBinder_, {}};
            lamRecs_.push_back(rec);
            std::size_t mark = scopes_.size();
            lamStack_.push_back({rec, mark}); // params + inner binders count as declared inside
            int save = curInitBinder_; curInitBinder_ = -1; // params/inner lets aren't the enclosing let's init
            for (auto& p : lam.params) pushBinder(p.name, nullptr, nullptr, &p);
            if (lam.exprBodied) walkExpr(lam.body.get());
            else for (auto& st : lam.block) walkStmt(*st);
            curInitBinder_ = save;
            lamStack_.pop_back();
            scopes_.resize(mark);
            break;
        }
        case ExprKind::Unary: walkExpr(static_cast<Unary&>(*e).operand.get()); break;
        case ExprKind::Await: walkExpr(static_cast<Await&>(*e).operand.get()); break;
        case ExprKind::Cast:  walkExpr(static_cast<Cast&>(*e).operand.get()); break;
        case ExprKind::Binary: { auto& b = static_cast<Binary&>(*e); walkExpr(b.lhs.get()); walkExpr(b.rhs.get()); break; }
        case ExprKind::Cond:  { auto& c = static_cast<Cond&>(*e); walkExpr(c.cond.get()); walkExpr(c.then.get()); walkExpr(c.els.get()); break; }
        case ExprKind::Call:  for (auto& a : static_cast<Call&>(*e).args) walkExpr(a.get()); break;
        case ExprKind::MethodCall: { auto& mc = static_cast<MethodCall&>(*e); if (mc.object) walkExpr(mc.object.get()); for (auto& a : mc.args) walkExpr(a.get()); break; }
        case ExprKind::Member: { auto& m = static_cast<Member&>(*e); if (m.object) walkExpr(m.object.get()); break; }
        case ExprKind::New:   for (auto& a : static_cast<New&>(*e).args) walkExpr(a.get()); break;
        case ExprKind::Index: { auto& i = static_cast<Index&>(*e); walkExpr(i.receiver.get()); for (auto& ix : i.indices) walkExpr(ix.get()); break; }
        case ExprKind::ListLit: for (auto& x : static_cast<ListLit&>(*e).elements) walkExpr(x.get()); break;
        case ExprKind::Tuple: for (auto& x : static_cast<Tuple&>(*e).elements) walkExpr(x.get()); break;
        case ExprKind::Bound: { auto& b = static_cast<Bound&>(*e); if (b.receiver) walkExpr(b.receiver.get()); for (auto& a : b.args) walkExpr(a.get()); break; }
        case ExprKind::MakeCase: for (auto& f : static_cast<MakeCase&>(*e).fields) walkExpr(f.value.get()); break;
        case ExprKind::Interp: for (auto& h : static_cast<Interp&>(*e).holes) walkExpr(h.get()); break;
        case ExprKind::With: {
            auto& w = static_cast<With&>(*e);
            walkExpr(w.base.get());
            for (auto& f : w.fields) walkExpr(f.value.get());
            break;
        }
        case ExprKind::Match: {
            auto& mt = static_cast<Match&>(*e);
            walkExpr(mt.scrutinee.get());
            for (auto& arm : mt.arms) {
                std::size_t mark = scopes_.size();
                if (arm.pattern.kind == PatternKind::Binding && !arm.pattern.binding.empty())
                    pushBinder(arm.pattern.binding, nullptr, nullptr, nullptr);
                for (const auto& bd : arm.pattern.binders)
                    if (!bd.binding.empty()) pushBinder(bd.binding, nullptr, nullptr, nullptr);
                if (arm.guard) walkExpr(arm.guard.get());
                walkExpr(arm.body.get());
                scopes_.resize(mark);
            }
            break;
        }
        default: break; // literals and leaves (Int/Float/Bool/Str/Char/Null/Extern)
        }
    }

    // ---- finalize: decide needsCell, stamp declarations + accesses + lambda capture lists ----
    void finalize() {
        for (auto& [id, bi] : info_) {
            bool needsCell = bi.assigned || bi.selfRef;
            bool captured = false;
            for (auto* rec : lamRecs_) for (auto& c : rec->caps) if (c.binderId == id) captured = true;
            if (!captured) continue;
            if (bi.let)   { bi.let->captured = true;   bi.let->needsCell = needsCell; }
            if (bi.param) { bi.param->captured = true; bi.param->needsCell = needsCell; }
            if (bi.forN)  { bi.forN->captured = true;  if (needsCell) bi.forN->needsCell = true; }
            if (needsCell) {
                for (auto* v : bi.reads) v->throughCell = true;
                for (auto* a : bi.writes) a->targetThroughCell = true;
            }
        }
        for (auto* rec : lamRecs_) {
            for (auto& c : rec->caps) {
                const BinderInfo& bi = info_[c.binderId];
                bool needsCell = bi.assigned || bi.selfRef;
                Capture cap;
                cap.name = bi.name;
                cap.declType = bi.declType;
                cap.needsCell = needsCell;
                cap.mutatedInside = c.mutatedInside;
                cap.reassignedOutside = bi.assigned && !c.mutatedInside;
                rec->lam->captures.push_back(std::move(cap));
            }
        }
    }
};

} // anonymous namespace

namespace mintplayer::polyglot {

void analyzeCaptures(ir::Module& m) {
    Analyzer a;
    a.run(m);
}

} // namespace mintplayer::polyglot
