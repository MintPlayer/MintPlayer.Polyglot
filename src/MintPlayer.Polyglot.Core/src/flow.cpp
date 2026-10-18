#include "mintplayer/polyglot/flow.hpp"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

// P40 review (A9/B11) — see flow.hpp for the contract.

namespace mintplayer::polyglot {

namespace {

bool isUnit(const TypeRef& t) {
    return t.absent() || (t.kind == TypeRef::Kind::Named && t.name == "unit" && !t.nullable);
}

// A `yield` directly in this body (not in a nested lambda) makes it an iterator: its end is a normal finish.
bool containsYield(const std::vector<StmtPtr>& body) {
    for (const auto& s : body) {
        if (!s) continue;
        if (s->kind == StmtKind::Yield) return true;
        if (containsYield(s->thenBody) || containsYield(s->elseBody) || containsYield(s->finallyBody)) return true;
        for (const auto& c : s->catches) if (containsYield(c.body)) return true;
    }
    return false;
}

// A side-effect-free access path (`x`, `this`, `a.b.c`) — the only operand shape a self-comparison is judged on.
bool pathText(const Expr* e, std::string& out) {
    if (!e) return false;
    if (e->kind == ExprKind::Name) { // a bare static resolves to its owner (`count` inside K is `K.count`)
        out = e->staticOwner.empty() ? e->text : e->staticOwner + "." + e->text;
        return true;
    }
    if (e->kind == ExprKind::This) { out = "this"; return true; }
    if (e->kind == ExprKind::Member && !e->flag && e->lhs) {
        std::string base;
        if (!pathText(e->lhs.get(), base)) return false;
        out = base + "." + e->text;
        return true;
    }
    return false;
}

// A condition C# folds to a compile-time constant (P40 review r3, part 2): bool literals, `!`, `&&`/`||` over
// constants, and a comparison of two numeric literals. C# then reports the dead branch (CS0162) — a warning a
// warnings-as-errors consumer cannot build — so a constant `if`/`while` condition is refused at the source.
bool constBool(const Expr* e, bool& v) {
    if (!e) return false;
    if (e->kind == ExprKind::BoolLit) { v = e->boolVal; return true; }
    if (e->kind == ExprKind::Unary && e->text == "!") { bool x; if (!constBool(e->lhs.get(), x)) return false; v = !x; return true; }
    if (e->kind == ExprKind::Binary && (e->text == "&&" || e->text == "||")) {
        bool a, b;
        if (!constBool(e->lhs.get(), a) || !constBool(e->rhs.get(), b)) return false;
        v = e->text == "&&" ? (a && b) : (a || b);
        return true;
    }
    if (e->kind == ExprKind::Binary && e->lhs && e->rhs &&
        (e->lhs->kind == ExprKind::IntLit || e->lhs->kind == ExprKind::FloatLit) &&
        (e->rhs->kind == ExprKind::IntLit || e->rhs->kind == ExprKind::FloatLit)) {
        const std::string& op = e->text;
        if (op != "==" && op != "!=" && op != "<" && op != "<=" && op != ">" && op != ">=") return false;
        const double a = std::strtod(e->lhs->text.c_str(), nullptr), b = std::strtod(e->rhs->text.c_str(), nullptr);
        v = op == "==" ? a == b : op == "!=" ? a != b : op == "<" ? a < b : op == "<=" ? a <= b : op == ">" ? a > b : a >= b;
        return true;
    }
    return false;
}

class Flow {
public:
    explicit Flow(DiagnosticBag& d) : d_(d) {}

    // A declaration body: reports unreachable statements and, when `needsReturn`, a reachable end.
    void body(const std::vector<StmtPtr>& b, bool needsReturn, SourcePos at, const std::string& what) {
        std::vector<bool> saved;
        saved.swap(loops_);
        const bool completes = block(b);
        loops_.swap(saved);
        if (needsReturn && completes && !containsYield(b))
            d_.error(at, what + " can reach the end of its body without returning a value — every path must "
                                "`return` a value or `throw`");
    }

    // Every expression of a declaration that is not a statement body (initializers, `=> expr` bodies, defaults).
    void expr(const Expr* e) {
        if (!e) return;
        if (e->kind == ExprKind::Binary && isComparison(e->text)) {
            std::string l, r;
            if (pathText(e->lhs.get(), l) && pathText(e->rhs.get(), r) && l == r)
                d_.error(e->pos, "'" + l + " " + e->text + " " + r + "' compares a value with itself — the result "
                                 "is constant (only a NaN differs from itself: test that with Math.isNaN)");
        }
        if (e->kind == ExprKind::Lambda && e->flag) { // a block-bodied lambda: its own reachability scope
            std::vector<bool> saved;
            saved.swap(loops_);
            const bool completes = block(e->block);
            loops_.swap(saved);
            // P40 follow-up: a block lambda whose result is a value (sema: lambdaNeedsValue) must not fall off its
            // end — C# refuses it (CS1643) and TS/Python/PHP would silently produce undefined/None/null.
            if (e->lambdaNeedsValue && completes && !containsYield(e->block))
                d_.error(e->pos, "this lambda can reach the end of its body without returning a value — every "
                                 "path must `return` a value or `throw`");
        } else {
            for (const auto& s : e->block) stmt(*s);
        }
        expr(e->lhs.get()); expr(e->rhs.get()); expr(e->extra.get());
        for (const auto& a : e->args) expr(a.get());
        for (const auto& f : e->fields) expr(f.value.get());
        for (const auto& p : e->params) expr(p.defaultValue.get());
        for (const auto& a : e->arms) {
            expr(a.guard.get());
            expr(a.body.get());
            if (a.bodyIsBlock) block(a.block);
        }
    }

private:
    static bool isComparison(const std::string& op) {
        return op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=";
    }

    // Can the list complete normally? Reports the first unreachable statement of the list (once).
    bool block(const std::vector<StmtPtr>& b) {
        bool live = true;
        bool reported = false;
        for (const auto& s : b) {
            if (!s) continue;
            if (!live && !reported) {
                d_.error(s->pos, "unreachable code — every path before this statement already left the block "
                                 "(`return`, `throw`, `break` or `continue`); remove it");
                reported = true;
            }
            const bool c = stmt(*s);
            if (live) live = c;
        }
        return live;
    }

    bool stmt(const Stmt& s) {
        expr(s.target.get());
        expr(s.value.get());
        if (s.kind == StmtKind::Assign && s.op == "=") { // `x = x` (C# CS1717): a no-op, almost always a typo
            std::string l, r;
            if (pathText(s.target.get(), l) && pathText(s.value.get(), r) && l == r)
                d_.error(s.pos, "'" + l + " = " + r + "' assigns a value to itself — it has no effect");
        }
        bool cv = false;
        if (s.kind == StmtKind::If && constBool(s.value.get(), cv))
            d_.error(s.value->pos, std::string("this 'if' condition is always ") + (cv ? "true" : "false") +
                                   " — remove the dead branch (C# reports it as unreachable code)");
        if (s.kind == StmtKind::While && !s.isDoWhile && constBool(s.value.get(), cv) &&
            !(s.value->kind == ExprKind::BoolLit && cv))
            d_.error(s.value->pos, std::string("this 'while' condition is always ") + (cv ? "true" : "false") +
                                   (cv ? " — write 'while true'" : " — the loop never runs; remove it"));
        switch (s.kind) {
            case StmtKind::Return:
            case StmtKind::Throw:
                return false;
            case StmtKind::Break:
                if (!loops_.empty()) loops_.back() = true;
                return false;
            case StmtKind::Continue:
                return false;
            case StmtKind::If: {
                const bool t = block(s.thenBody);
                const bool e = s.hasElse ? block(s.elseBody) : true;
                return t || e;
            }
            case StmtKind::While: {
                const bool infinite = s.value && s.value->kind == ExprKind::BoolLit && s.value->boolVal;
                loops_.push_back(false);
                const bool bodyCompletes = block(s.thenBody);
                const bool broke = loops_.back();
                loops_.pop_back();
                if (broke) return true;
                if (infinite) return false;
                // a `do { … } while (c)` whose body never completes never reaches its condition
                return s.isDoWhile ? bodyCompletes || containsContinue(s.thenBody) : true;
            }
            case StmtKind::For:
                loops_.push_back(false);
                block(s.thenBody);
                loops_.pop_back();
                return true;
            case StmtKind::Use:
                return block(s.thenBody);
            case StmtKind::Try: {
                bool any = block(s.thenBody);
                for (const auto& c : s.catches) {
                    expr(c.guard.get());
                    if (block(c.body)) any = true;
                }
                if (s.hasFinally && !block(s.finallyBody)) return false;
                return any;
            }
            default:
                return true;
        }
    }

    // A `continue` that targets THIS loop (not a nested one) re-reaches a do-while's condition.
    static bool containsContinue(const std::vector<StmtPtr>& b) {
        for (const auto& s : b) {
            if (!s) continue;
            if (s->kind == StmtKind::Continue) return true;
            if (s->kind == StmtKind::While || s->kind == StmtKind::For) continue;
            if (containsContinue(s->thenBody) || containsContinue(s->elseBody) || containsContinue(s->finallyBody))
                return true;
            for (const auto& c : s->catches) if (containsContinue(c.body)) return true;
        }
        return false;
    }

    DiagnosticBag& d_;
    std::vector<bool> loops_; // per enclosing loop: a `break` reaches past it
};

// ---- definite assignment of class fields through `init` ----
// A state is the set of fields assigned on every path so far; nullopt = the path never gets here (it
// returned or threw), which is the identity of the meet.
using Assigned = std::optional<std::set<std::string>>;

// Does `this` occur anywhere in the expression (lambdas and statement-bodied arms included)?
bool mentionsThis(const Expr* e);
bool mentionsThis(const std::vector<StmtPtr>& b) {
    for (const auto& s : b) {
        if (!s) continue;
        if (mentionsThis(s->target.get()) || mentionsThis(s->value.get()) || mentionsThis(s->thenBody) ||
            mentionsThis(s->elseBody) || mentionsThis(s->finallyBody))
            return true;
        for (const auto& c : s->catches) if (mentionsThis(c.guard.get()) || mentionsThis(c.body)) return true;
    }
    return false;
}
bool mentionsThis(const Expr* e) {
    if (!e) return false;
    if (e->kind == ExprKind::This) return true;
    if (mentionsThis(e->lhs.get()) || mentionsThis(e->rhs.get()) || mentionsThis(e->extra.get()) || mentionsThis(e->block))
        return true;
    for (const auto& a : e->args) if (mentionsThis(a.get())) return true;
    for (const auto& f : e->fields) if (mentionsThis(f.value.get())) return true;
    for (const auto& a : e->arms)
        if (mentionsThis(a.guard.get()) || mentionsThis(a.body.get()) || mentionsThis(a.block)) return true;
    return false;
}

Assigned meet(const Assigned& a, const Assigned& b) {
    if (!a) return b;
    if (!b) return a;
    std::set<std::string> out;
    std::set_intersection(a->begin(), a->end(), b->begin(), b->end(), std::inserter(out, out.end()));
    return out;
}

class FieldInit {
public:
    FieldInit(DiagnosticBag& d, const ClassDecl& c, const std::vector<const Member*>& required,
              const std::set<std::string>& inherited)
        : d_(d), c_(c), required_(required), inherited_(inherited) {}

    Assigned list(const std::vector<StmtPtr>& b, Assigned in) {
        for (const auto& s : b) {
            if (!in) break; // the rest is unreachable (reported by the reachability walk)
            in = stmt(*s, *in);
        }
        return in;
    }

    void anywhere(const std::vector<StmtPtr>& b, std::set<std::string>& out) const {
        for (const auto& s : b) {
            if (!s) continue;
            if (const char* n = thisField(*s)) out.insert(n);
            anywhere(s->thenBody, out); anywhere(s->elseBody, out); anywhere(s->finallyBody, out);
            for (const auto& c : s->catches) anywhere(c.body, out);
        }
    }

private:
    static const char* thisField(const Stmt& s) {
        if (s.kind == StmtKind::Assign && s.op == "=" && s.target && s.target->kind == ExprKind::Member &&
            s.target->lhs && s.target->lhs->kind == ExprKind::This)
            return s.target->text.c_str();
        return nullptr;
    }

    // P40 review r3 2(g): until every field is assigned, `init` may only ASSIGN fields — a read of an unassigned
    // field printed null/undefined (or raised AttributeError on Python), and a method call, a lambda capturing
    // `this` or passing `this` along can read one indirectly (Swift's two-phase rule).
    bool allAssigned(const std::set<std::string>& in) const {
        for (const Member* m : required_) if (!in.count(m->name)) return false;
        return true;
    }
    bool isRequired(const std::string& n) const {
        for (const Member* m : required_) if (m->name == n) return true;
        return false;
    }
    // A field of this class, or an instance field it inherits: the base constructor (written `super()` or
    // implicit) runs before any statement of this one on every target, so an inherited field is assigned
    // (review r4 #4 — `super(); this.y = this.x + 10` was refused).
    bool isField(const std::string& n) const {
        for (const auto& m : c_.members) if (m.kind == MemberKind::Field && m.name == n) return true;
        return inherited_.count(n) != 0;
    }
    void earlyThis(SourcePos at, const std::set<std::string>& in) {
        std::string pending;
        for (const Member* m : required_) if (!in.count(m->name)) { pending = m->name; break; }
        if (!quiet_) d_.error(at, "`init` of class '" + c_.name + "' uses 'this' before field '" + pending + "' is assigned — "
                     "until every field is assigned, init may only assign them (no reads, method calls, "
                     "lambdas capturing 'this', or passing 'this')");
    }
    void use(const Expr* e, const std::set<std::string>& in) {
        if (!e) return;
        if (e->kind == ExprKind::Member && e->lhs && e->lhs->kind == ExprKind::This) {
            if (isRequired(e->text)) {
                if (!in.count(e->text))
                    if (!quiet_) d_.error(e->pos, "`init` of class '" + c_.name + "' reads field '" + e->text +
                                         "' before assigning it — the targets read null, undefined or nothing");
            } else if (!isField(e->text) && !allAssigned(in)) {
                earlyThis(e->pos, in);
            }
            return;
        }
        if (e->kind == ExprKind::This) { if (!allAssigned(in)) earlyThis(e->pos, in); return; }
        if (e->kind == ExprKind::Lambda) { if (!allAssigned(in) && mentionsThis(e)) earlyThis(e->pos, in); return; }
        use(e->lhs.get(), in); use(e->rhs.get(), in); use(e->extra.get(), in);
        for (const auto& a : e->args) use(a.get(), in);
        for (const auto& f : e->fields) use(f.value.get(), in);
        for (const auto& a : e->arms) { use(a.guard.get(), in); use(a.body.get(), in); useBlock(a.block, in); }
    }
    void useBlock(const std::vector<StmtPtr>& b, const std::set<std::string>& in) {
        for (const auto& s : b) {
            if (!s) continue;
            use(s->target.get(), in); use(s->value.get(), in);
            useBlock(s->thenBody, in); useBlock(s->elseBody, in); useBlock(s->finallyBody, in);
            for (const auto& c : s->catches) { use(c.guard.get(), in); useBlock(c.body, in); }
        }
    }

    Assigned stmt(const Stmt& s, const std::set<std::string>& in) {
        if (s.kind == StmtKind::Assign && thisField(s)) use(s.value.get(), in); // the target is a write
        else { use(s.target.get(), in); use(s.value.get(), in); }
        switch (s.kind) {
            case StmtKind::Assign: {
                std::set<std::string> out = in;
                if (const char* n = thisField(s)) out.insert(n);
                return out;
            }
            case StmtKind::Return: {
                // P40 review r3: an enclosing `finally` runs before the return completes — what it assigns counts.
                std::set<std::string> have = in;
                for (const auto& f : finallyAssigned_) have.insert(f.begin(), f.end());
                for (const Member* m : required_)
                    if (!have.count(m->name) && !quiet_)
                        d_.error(s.pos, "`init` of class '" + c_.name + "' returns before field '" + m->name +
                                            "' is assigned — assign it before every early `return`");
                return std::nullopt;
            }
            case StmtKind::Throw:
                return std::nullopt;
            case StmtKind::Break: // the state flows to the end of the innermost loop
                if (!breaks_.empty()) breaks_.back() = breaks_.back().has_value() ? meet(breaks_.back(), Assigned(in)) : Assigned(in);
                return std::nullopt;
            case StmtKind::Continue: // …or back to its condition
                if (!continues_.empty()) continues_.back() = continues_.back().has_value() ? meet(continues_.back(), Assigned(in)) : Assigned(in);
                return std::nullopt;
            case StmtKind::If: {
                Assigned t = list(s.thenBody, in);
                Assigned e = s.hasElse ? list(s.elseBody, in) : Assigned(in);
                return meet(t, e);
            }
            case StmtKind::While:
            case StmtKind::For: {
                // P40 review r3: a `while true` loop is left only through a `break` (the meet of their states);
                // a do-while body runs at least once. Any other loop body may run zero times.
                breaks_.emplace_back(std::nullopt);
                continues_.emplace_back(std::nullopt);
                Assigned bodyEnd = list(s.thenBody, in);
                Assigned brk = breaks_.back(), cont = continues_.back();
                breaks_.pop_back();
                continues_.pop_back();
                const bool infinite = s.kind == StmtKind::While && !s.isDoWhile && s.value &&
                                      s.value->kind == ExprKind::BoolLit && s.value->boolVal;
                if (infinite) return brk;
                if (s.kind == StmtKind::While && s.isDoWhile) return meet(meet(bodyEnd, cont), brk);
                return in;
            }
            case StmtKind::Use:
                return list(s.thenBody, in);
            case StmtKind::Try: {
                std::set<std::string> fin; // what the finally definitely assigns on its own
                if (s.hasFinally) {
                    const bool q = quiet_;
                    quiet_ = true;
                    Assigned f = list(s.finallyBody, std::set<std::string>{});
                    quiet_ = q;
                    if (f) fin = *f;
                }
                finallyAssigned_.push_back(fin);
                // A catch may enter from any point of the body, so it starts from the state before the try.
                Assigned out = list(s.thenBody, in);
                for (const auto& c : s.catches) out = meet(out, list(c.body, in));
                finallyAssigned_.pop_back();
                if (s.hasFinally) {
                    Assigned f = list(s.finallyBody, in);
                    if (!f) return std::nullopt;
                    if (!out) return std::nullopt;
                    std::set<std::string> u = *out;
                    u.insert(f->begin(), f->end());
                    return u;
                }
                return out;
            }
            default:
                return in;
        }
    }

    DiagnosticBag& d_;
    const ClassDecl& c_;
    const std::vector<const Member*>& required_;
    const std::set<std::string>& inherited_;               // instance fields declared by a base class
    bool quiet_ = false;                                   // computing a finally's own assignments: no reports
    std::vector<Assigned> breaks_, continues_;              // per enclosing loop: the meet of break/continue states
    std::vector<std::set<std::string>> finallyAssigned_;    // per enclosing try: what its finally assigns
};

void checkFieldsDefinitelyAssigned(const ClassDecl& c, DiagnosticBag& d, const std::set<std::string>& inherited) {
    if (c.isExtern) return;
    // P40 review r3 2(g): an instance field initializer runs before the constructor body on some targets and
    // inside it on others (Python/PHP), so `this` there reads fields in a target-dependent state (`let b =
    // this.a + 1` printed 0 on TS, 1 on PHP, and was C# CS0027). Refused.
    for (const auto& m : c.members) {
        if (m.kind != MemberKind::Field || !m.init) continue;
        bool isStatic = false;
        for (const auto& mod : m.modifiers) if (mod == "static") isStatic = true;
        if (!isStatic && mentionsThis(m.init.get()))
            d.error(m.init->pos, "the initializer of field '" + m.name + "' uses 'this' — field initializers run "
                                 "before or inside the constructor depending on the target; assign it in init instead");
    }
    std::vector<const Member*> required;
    for (const auto& m : c.members)
        if (m.kind == MemberKind::Field && !m.init && m.bindings.empty()) required.push_back(&m);
    if (required.empty()) return;
    FieldInit fi(d, c, required, inherited);
    std::set<std::string> somewhere;
    std::set<std::string> missing; // fields some completing init leaves unassigned
    bool anyCtor = false;
    for (const auto& m : c.members) {
        if (m.kind != MemberKind::Constructor) continue;
        anyCtor = true;
        fi.anywhere(m.body, somewhere);
        Assigned end = fi.list(m.body, std::set<std::string>{});
        if (!end) continue; // every path returns (checked at each return) or throws
        for (const Member* f : required) if (!end->count(f->name)) missing.insert(f->name);
    }
    for (const Member* f : required) {
        if (!somewhere.count(f->name))
            d.error(f->pos, "field '" + f->name + "' of class '" + c.name +
                                "' is never initialized — give it an initializer or assign it in init "
                                "(uninitialized reads differ per target)");
        else if (anyCtor && missing.count(f->name))
            d.error(f->pos, "field '" + f->name + "' of class '" + c.name +
                                "' is not assigned on every path through init — assign it on each branch "
                                "(a loop body may run zero times), or give it an initializer");
    }
}

// Every expression in a statement list / expression tree (pre-order); `f` returns false to skip a subtree.
template <class F> void eachExpr(const Expr* e, F& f);
template <class F> void eachExpr(const std::vector<StmtPtr>& b, F& f) {
    for (const auto& s : b) {
        if (!s) continue;
        eachExpr(s->target.get(), f); eachExpr(s->value.get(), f);
        eachExpr(s->thenBody, f); eachExpr(s->elseBody, f); eachExpr(s->finallyBody, f);
        for (const auto& c : s->catches) { eachExpr(c.guard.get(), f); eachExpr(c.body, f); }
    }
}
template <class F> void eachExpr(const Expr* e, F& f) {
    if (!e || !f(*e)) return;
    eachExpr(e->lhs.get(), f); eachExpr(e->rhs.get(), f); eachExpr(e->extra.get(), f); eachExpr(e->block, f);
    for (const auto& a : e->args) eachExpr(a.get(), f);
    for (const auto& fv : e->fields) eachExpr(fv.value.get(), f);
    for (const auto& a : e->arms) { eachExpr(a.guard.get(), f); eachExpr(a.body.get(), f); eachExpr(a.block, f); }
}

bool hasModifier(const Member& m, const char* mod) {
    for (const auto& x : m.modifiers) if (x == mod) return true;
    return false;
}

// P40 review r4 #5: a constructor that calls an overridable method on `this` runs a subclass override before
// the subclass's own fields are initialized — C# reads default(T), TS undefined, Python raises AttributeError,
// PHP reads null. Refused: no constructor may reach (directly, or through non-overridable methods of its own
// class chain called on `this`) a call to an `open`/`abstract`/`override` method on `this`.
void checkCtorVirtualCalls(const ClassDecl& c, const std::map<std::string, const ClassDecl*>& classes,
                           DiagnosticBag& d) {
    // The method `name` as `this` of class `c` sees it: the nearest declaration up the base chain.
    auto lookup = [&](const std::string& name) -> const Member* {
        std::set<std::string> seen;
        const ClassDecl* k = &c;
        while (k && seen.insert(k->name).second) {
            for (const auto& m : k->members) if (m.kind == MemberKind::Method && m.name == name) return &m;
            const ClassDecl* next = nullptr;
            for (const auto& b : k->bases) {
                auto it = classes.find(b.name);
                if (it != classes.end()) { next = it->second; break; }
            }
            k = next;
        }
        return nullptr;
    };
    for (const auto& ctor : c.members) {
        if (ctor.kind != MemberKind::Constructor) continue;
        std::set<const Member*> visited;
        std::vector<std::pair<const Member*, std::string>> work; // (method body to scan, the chain so far)
        bool reported = false;
        auto scan = [&](const std::vector<StmtPtr>& body, const Expr* exprBody, const std::string& via) {
            auto f = [&](const Expr& e) -> bool {
                if (reported) return false;
                if (e.kind == ExprKind::Lambda) return false; // deferred: runs whenever it is called
                if (e.kind != ExprKind::Call || !e.lhs || e.lhs->kind != ExprKind::Member || !e.lhs->lhs ||
                    e.lhs->lhs->kind != ExprKind::This)
                    return true;
                const Member* m = lookup(e.lhs->text);
                if (!m) return true;
                if (hasModifier(*m, "open") || hasModifier(*m, "abstract") || hasModifier(*m, "override")) {
                    d.error(e.pos, "the constructor of class '" + c.name + "' calls the overridable method '" +
                                       e.lhs->text + "' on 'this'" + via + " — a subclass override would run "
                                       "before the subclass's fields are initialized (C# reads defaults, TS "
                                       "undefined, Python raises AttributeError); call it after construction, or "
                                       "make the method non-open");
                    reported = true;
                    return false;
                }
                if (visited.insert(m).second)
                    work.emplace_back(m, via.empty() ? " (through '" + m->name + "')"
                                                     : via.substr(0, via.size() - 1) + " -> '" + m->name + "')");
                return true;
            };
            eachExpr(body, f);
            eachExpr(exprBody, f);
        };
        scan(ctor.body, ctor.exprBody.get(), "");
        while (!work.empty() && !reported) {
            auto [m, via] = work.back();
            work.pop_back();
            scan(m->body, m->exprBody.get(), via);
        }
    }
}

void checkMembers(Flow& flow, const std::vector<Member>& members, const std::string& owner, bool prelude) {
    for (const auto& m : members) {
        if (!m.bindings.empty()) continue; // a bound std member: target text, no .pg body
        if (prelude && m.body.empty() && !m.exprBody && !m.init) continue; // a manifest-bound std stub
        flow.expr(m.init.get());
        flow.expr(m.exprBody.get());
        for (const auto& p : m.params) flow.expr(p.defaultValue.get());
        const std::string what = "'" + owner + "." + m.name + "'";
        switch (m.kind) {
            case MemberKind::Method:
            case MemberKind::Operator:
                if (m.hasBody && !m.exprBodied) flow.body(m.body, !isUnit(m.returnType), m.pos, what);
                break;
            case MemberKind::Constructor:
                if (m.hasBody && !m.exprBodied) flow.body(m.body, false, m.pos, what);
                break;
            case MemberKind::Property:
                if (!m.init && !m.body.empty()) flow.body(m.body, true, m.pos, "the getter of " + what);
                if (m.hasSetter) flow.body(m.setterBody, false, m.pos, "the setter of " + what);
                break;
            default:
                break;
        }
    }
}

} // namespace

void checkFlow(const CompilationUnit& unit, DiagnosticBag& diags) {
    Flow flow(diags);
    // A prelude (std) declaration with an empty body is a signature the plugin manifest binds per target.
    auto boundStub = [](const std::string& origin, const std::vector<StmtPtr>& body) {
        return body.empty() && origin == "<prelude>";
    };
    for (const auto& fn : unit.functions) {
        if (fn.isExpect || boundStub(fn.originModule, fn.body)) continue;
        for (const auto& p : fn.params) flow.expr(p.defaultValue.get());
        flow.body(fn.body, !isUnit(fn.returnType), fn.pos, "function '" + fn.name + "'");
    }
    for (const auto& r : unit.records) checkMembers(flow, r.members, r.name, r.originModule == "<prelude>");
    std::map<std::string, const ClassDecl*> classes;
    for (const auto& c : unit.classes) classes[c.name] = &c;
    for (const auto& c : unit.classes) {
        if (c.isExtern) continue; // native-backed std carrier: its members are bindings, not .pg bodies
        checkMembers(flow, c.members, c.name, c.originModule == "<prelude>");
        std::set<std::string> inherited, seen{c.name};
        std::vector<const ClassDecl*> work{&c};
        while (!work.empty()) { // every user base class, transitively (the cycle guard is `seen`)
            const ClassDecl* k = work.back();
            work.pop_back();
            for (const auto& b : k->bases) {
                auto it = classes.find(b.name);
                if (it == classes.end() || !seen.insert(b.name).second) continue;
                for (const auto& m : it->second->members) {
                    bool isStatic = false;
                    for (const auto& mod : m.modifiers) if (mod == "static") isStatic = true;
                    if (m.kind == MemberKind::Field && !isStatic) inherited.insert(m.name);
                }
                work.push_back(it->second);
            }
        }
        checkFieldsDefinitelyAssigned(c, diags, inherited);
        checkCtorVirtualCalls(c, classes, diags);
    }
    for (const auto& i : unit.interfaces) checkMembers(flow, i.members, i.name, i.originModule == "<prelude>");
    for (const auto& e : unit.extensions) {
        if (!e.bindings.empty() || (!e.exprBody && boundStub(e.originModule, e.body))) continue;
        if (e.exprBody) flow.expr(e.exprBody.get());
        else flow.body(e.body, !isUnit(e.returnType), e.pos, "extension '" + e.receiver.name + "." + e.name + "'");
    }
    for (const auto& v : unit.values) flow.expr(v.init.get());
}

} // namespace mintplayer::polyglot
