#include "mintplayer/polyglot/flow.hpp"

#include <algorithm>
#include <iterator>
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
    if (e->kind == ExprKind::Name) { out = e->text; return true; }
    if (e->kind == ExprKind::This) { out = "this"; return true; }
    if (e->kind == ExprKind::Member && !e->flag && e->lhs) {
        std::string base;
        if (!pathText(e->lhs.get(), base)) return false;
        out = base + "." + e->text;
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
            block(e->block);
            loops_.swap(saved);
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

Assigned meet(const Assigned& a, const Assigned& b) {
    if (!a) return b;
    if (!b) return a;
    std::set<std::string> out;
    std::set_intersection(a->begin(), a->end(), b->begin(), b->end(), std::inserter(out, out.end()));
    return out;
}

class FieldInit {
public:
    FieldInit(DiagnosticBag& d, const ClassDecl& c, const std::vector<const Member*>& required)
        : d_(d), c_(c), required_(required) {}

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

    Assigned stmt(const Stmt& s, const std::set<std::string>& in) {
        switch (s.kind) {
            case StmtKind::Assign: {
                std::set<std::string> out = in;
                if (const char* n = thisField(s)) out.insert(n);
                return out;
            }
            case StmtKind::Return: {
                for (const Member* m : required_)
                    if (!in.count(m->name))
                        d_.error(s.pos, "`init` of class '" + c_.name + "' returns before field '" + m->name +
                                            "' is assigned — assign it before every early `return`");
                return std::nullopt;
            }
            case StmtKind::Throw:
            case StmtKind::Break:
            case StmtKind::Continue:
                return std::nullopt;
            case StmtKind::If: {
                Assigned t = list(s.thenBody, in);
                Assigned e = s.hasElse ? list(s.elseBody, in) : Assigned(in);
                return meet(t, e);
            }
            case StmtKind::While:
            case StmtKind::For:
                list(s.thenBody, in); // a loop body may run zero times; walked only for its early returns
                return in;
            case StmtKind::Use:
                return list(s.thenBody, in);
            case StmtKind::Try: {
                // A catch may enter from any point of the body, so it starts from the state before the try.
                Assigned out = list(s.thenBody, in);
                for (const auto& c : s.catches) out = meet(out, list(c.body, in));
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
};

void checkFieldsDefinitelyAssigned(const ClassDecl& c, DiagnosticBag& d) {
    if (c.isExtern) return;
    std::vector<const Member*> required;
    for (const auto& m : c.members)
        if (m.kind == MemberKind::Field && !m.init && m.bindings.empty()) required.push_back(&m);
    if (required.empty()) return;
    FieldInit fi(d, c, required);
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
    for (const auto& c : unit.classes) {
        if (c.isExtern) continue; // native-backed std carrier: its members are bindings, not .pg bodies
        checkMembers(flow, c.members, c.name, c.originModule == "<prelude>");
        checkFieldsDefinitelyAssigned(c, diags);
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
