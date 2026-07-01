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
        if (e->kind == ExprKind::Lambda) mark(Feature::Closures, e->pos);
        if (e->kind == ExprKind::Await)  mark(Feature::Async, e->pos);
        if (e->kind == ExprKind::Call && e->lhs && e->lhs->kind == ExprKind::Name)
            calls.push_back({e->lhs->text, e->pos}); // a free-function call `name(...)`
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
    for (const auto& f : unit.functions)
        if (!f.actualTarget.empty()) actualTargets[f.name].insert(f.actualTarget);
    for (const auto& call : c.calls) {
        auto it = actualTargets.find(call.first);
        if (it != actualTargets.end() && !it->second.count(backend.name()))
            diags.error(call.second, std::string("portable function '") + call.first +
                                          "' has no 'actual' implementation for target '" + backend.name() +
                                          "'; add `actual(" + backend.name() + ")` or drop that target");
    }
}

} // namespace mintplayer::polyglot
