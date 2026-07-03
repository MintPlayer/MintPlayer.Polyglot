#include "mintplayer/polyglot/capability.hpp"

#include <array>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace mintplayer::polyglot {

namespace {

// Walks the AST accumulating the set of capability-flagged features the program uses, recording the first
// source position of each. Declarations expose features structurally (an `extension`, an `operator`/
// `property` member, a class with a base); statements/expressions expose them by node kind (`yield`,
// try/throw, `use`, `match`, lambda).
class Collector {
public:
    std::vector<FeatureUse> uses;
    std::vector<std::pair<std::string, SourcePos>> calls; // free-function call sites: (callee name, pos)
    // Member/construction use sites, keyed "<TypeOrOwner>.<member>" (both the receiver's semantic type and,
    // for a bare-name receiver, its spelling — statics like `Math.sqrt` resolve by the latter). The bound-
    // member arm check (P19 slice 9b follow-up) matches these against binding-shaped declarations.
    std::vector<std::pair<std::vector<std::string>, SourcePos>> memberUses;

    void run(const CompilationUnit& u) {
        for (const auto& e : u.extensions) {
            mark(Feature::ExtensionMethods, e.pos);
            for (const auto& s : e.body) stmt(s.get());
            expr(e.exprBody.get());
        }
        for (const auto& f : u.functions) {
            if (f.isAsync) mark(Feature::Async, f.pos);
            for (const auto& s : f.body) stmt(s.get());
        }
        for (const auto& d : u.records)    typeBody(d.members);
        for (const auto& d : u.interfaces) typeBody(d.members);
        for (const auto& d : u.classes) {
            // An `extern class` (e.g. the core prelude's Error/Iterable) is native-backed and not emitted —
            // its members are bound templates, not native properties/operators — so it requires no backend
            // feature support. Skipping it keeps an always-linked prelude from tripping a target's gating.
            if (d.isExtern) continue;
            if (!d.bases.empty()) mark(Feature::Inheritance, d.pos);
            typeBody(d.members);
        }
    }

private:
    std::array<bool, 32> seen_{}; // indexed by (int)Feature — first use of each is the one reported

    void mark(Feature f, SourcePos pos) {
        auto i = static_cast<std::size_t>(f);
        if (seen_[i]) return;
        seen_[i] = true;
        uses.push_back({f, pos});
    }

    void typeBody(const std::vector<Member>& members) {
        for (const auto& m : members) {
            if (m.kind == MemberKind::Operator) mark(Feature::OperatorOverloading, m.pos);
            if (m.kind == MemberKind::Property) mark(Feature::Properties, m.pos);
            if (m.isAsync) mark(Feature::Async, m.pos);
            for (const auto& s : m.body) stmt(s.get());
            expr(m.exprBody.get());
            expr(m.init.get());
        }
    }

    void stmt(const Stmt* s) {
        if (!s) return;
        switch (s->kind) {
            case StmtKind::Yield: mark(Feature::Iterators, s->pos); break;
            case StmtKind::Throw:
            case StmtKind::Try:   mark(Feature::Exceptions, s->pos); break;
            case StmtKind::Use:   mark(Feature::Disposal, s->pos); break;
            default: break;
        }
        expr(s->value.get());
        expr(s->target.get());
        for (const auto& st : s->thenBody) stmt(st.get());
        for (const auto& st : s->elseBody) stmt(st.get());
        for (const auto& c : s->catches) { expr(c.guard.get()); for (const auto& st : c.body) stmt(st.get()); }
        for (const auto& st : s->finallyBody) stmt(st.get());
    }

    void expr(const Expr* e) {
        if (!e) return;
        if (e->kind == ExprKind::Match)  mark(Feature::PatternMatching, e->pos);
        if (e->kind == ExprKind::Lambda) {
            mark(Feature::Closures, e->pos);
            if (e->flag) mark(Feature::BlockLambdas, e->pos); // flag = Lambda has a block (statement) body
        }
        if (e->kind == ExprKind::With)   mark(Feature::WithExpressions, e->pos);
        if (e->kind == ExprKind::Await)  mark(Feature::Async, e->pos);
        if (e->kind == ExprKind::Call && e->lhs && e->lhs->kind == ExprKind::Name) {
            calls.push_back({e->lhs->text, e->pos}); // a free-function call `name(...)`
            memberUses.push_back({{e->lhs->text + ".init"}, e->pos}); // or a construction `Type(args)`
        }
        if (e->kind == ExprKind::Member && e->lhs) {
            std::vector<std::string> keys;
            if (e->lhs->type.kind == TypeRef::Kind::Named && !e->lhs->type.name.empty())
                keys.push_back(e->lhs->type.name + "." + e->text);
            if (e->lhs->kind == ExprKind::Name) keys.push_back(e->lhs->text + "." + e->text);
            if (!keys.empty()) memberUses.push_back({std::move(keys), e->pos});
        }
        expr(e->lhs.get());
        expr(e->rhs.get());
        expr(e->extra.get());
        for (const auto& a : e->args) expr(a.get());
        for (const auto& st : e->block) stmt(st.get());
        for (const auto& f : e->fields) expr(f.value.get());
        for (const auto& arm : e->arms) {
            expr(arm.guard.get());
            expr(arm.body.get());
            for (const auto& st : arm.block) stmt(st.get());
        }
    }
};

} // namespace

std::vector<FeatureUse> collectFeatureUses(const CompilationUnit& unit) {
    Collector c;
    c.run(unit);
    return std::move(c.uses);
}

void checkCapabilities(const CompilationUnit& unit, const Backend& backend, DiagnosticBag& diags) {
    Collector c;
    c.run(unit);
    for (const auto& use : c.uses) {
        if (!backend.supports(use.feature))
            diags.error(use.pos, std::string("target '") + backend.name() + "' does not support " +
                                      featureName(use.feature) + "; remove it or drop that target");
    }

    // §3.B / §4.4: a target-gated portable function (one with `actual` impls) must have an `actual` for THIS
    // target at every call site — else the call would emit against a function the backend never defines (a
    // silent broken program). Keyed on *calls*, so an unused portable fn missing this target's arm is fine
    // (e.g. std.io's readText has no python arm, but a python program that never calls it is unaffected).
    std::unordered_map<std::string, std::unordered_set<std::string>> actualTargets; // fn name -> targets with an actual
    for (const auto& f : unit.functions) {
        // An `expect` fn is portable even with ZERO actuals in the unit (P19 slice 9b: the arms live in
        // plugin overlays and only the active target's are injected) — the empty entry makes a call to an
        // un-overlaid expect refuse below instead of slipping through as "not portable".
        if (f.isExpect) actualTargets[f.name];
        if (!f.actualTarget.empty()) actualTargets[f.name].insert(f.actualTarget);
    }
    for (const auto& call : c.calls) {
        auto it = actualTargets.find(call.first);
        if (it != actualTargets.end() && !it->second.count(backend.name()))
            diags.error(call.second, std::string("portable function '") + call.first +
                                          "' has no 'actual' implementation for target '" + backend.name() +
                                          "'; add `actual(" + backend.name() + ")` or drop that target");
    }

    // P19 slice 9b follow-up: a BOUND member (an extern-class member/type/ctor, or a bound extension) used
    // without this target's arm refuses at the use site — the member-level twin of the portable-fn check
    // above. Binding-shaped = declares a binding block (possibly the empty skeleton form awaiting overlay).
    std::unordered_map<std::string, bool> boundMembers; // "Type.member" -> has an arm for this target
    auto hasArm = [&](const std::vector<TargetBinding>& bs) {
        for (const auto& b : bs)
            if (b.target == backend.name()) return true;
        return false;
    };
    for (const auto& d : unit.classes) {
        if (!d.isExtern) continue;
        for (const auto& m : d.members) {
            if (!(m.hasBody && m.body.empty() && !m.exprBody)) continue; // binding-shaped members only
            const std::string member = m.kind == MemberKind::Init ? "init" : m.name;
            boundMembers[d.name + "." + member] = hasArm(m.bindings);
        }
    }
    for (const auto& e : unit.extensions) {
        const bool boundShaped = !e.bindings.empty() || (e.body.empty() && !e.exprBody);
        if (boundShaped) boundMembers[e.receiver.name + "." + e.name] = hasArm(e.bindings);
    }
    for (const auto& use : c.memberUses) {
        for (const std::string& key : use.first) {
            auto it = boundMembers.find(key);
            if (it != boundMembers.end() && !it->second)
                diags.error(use.second, "'" + key + "' has no binding for target '" + backend.name() +
                                            "'; add an `actual(" + backend.name() +
                                            ")` arm (or a plugin std overlay) or drop that target");
        }
    }
}

} // namespace mintplayer::polyglot
