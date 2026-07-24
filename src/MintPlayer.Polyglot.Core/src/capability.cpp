#include "mintplayer/polyglot/capability.hpp"

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
    std::vector<const AttrUse*> tier1Uses_; // P37 D12: every pass-through attribute attachment
    std::vector<std::pair<std::string, SourcePos>> calls; // free-function call sites: (callee name, pos)
    // Member/construction use sites, keyed "<TypeOrOwner>.<member>" (both the receiver's semantic type and,
    // for a bare-name receiver, its spelling — statics like `Math.sqrt` resolve by the latter). The bound-
    // member arm check (P19 slice 9b follow-up) matches these against binding-shaped declarations.
    std::vector<std::pair<std::vector<std::string>, SourcePos>> memberUses;

    void run(const CompilationUnit& u) {
        for (const auto& i : u.interfaces) interfaces_.insert(i.name); // prescan: interface-typed runtime tests (B4)
        // P37 D Tier 1: a pass-through attribute marks its ATTACHMENT POINT per target — Python has class/
        // method/function decorators but TS has no function decorators, so the point matters, not just
        // "attributes". Tier 2 marks nothing (the compiler both writes and reads the data).
        for (const auto& ea : u.externAttrs) externAttrNames_.insert(ea.name);
        auto markAttrs = [&](const std::vector<AttrUse>& attrs, const char* point) {
            for (const auto& a : attrs)
                if (externAttrNames_.count(a.name)) {
                    mark(std::string("attributes:target.") + point, a.pos);
                    tier1Uses_.push_back(&a);
                }
        };
        auto memberPoint = [](const Member& m) {
            return m.kind == MemberKind::Field ? "field" : "method"; // properties ride the method shape
        };
        for (const auto& r : u.records) {
            markAttrs(r.attributes, "type");
            for (const auto& mem : r.members) {
                markAttrs(mem.attributes, memberPoint(mem));
                for (const auto& p : mem.params) markAttrs(p.attributes, "param");
            }
        }
        for (const auto& c : u.classes) {
            markAttrs(c.attributes, "type");
            for (const auto& mem : c.members) {
                markAttrs(mem.attributes, memberPoint(mem));
                for (const auto& p : mem.params) markAttrs(p.attributes, "param");
            }
        }
        for (const auto& f : u.functions) {
            markAttrs(f.attributes, "function");
            for (const auto& p : f.params) markAttrs(p.attributes, "param");
        }
        for (const auto& e : u.extensions) {
            mark(Feature::ExtensionMethods, e.pos);
            typeRef(e.receiver, e.pos);
            params(e.params);
            typeRef(e.returnType, e.pos);
            for (const auto& s : e.body) stmt(s.get());
            expr(e.exprBody.get());
        }
        for (const auto& f : u.functions) {
            if (f.isAsync) mark(Feature::Async, f.pos);
            params(f.params);
            typeRef(f.returnType, f.pos);
            for (const auto& s : f.body) stmt(s.get());
        }
        for (const auto& v : u.values) typeRef(v.type, v.pos);
        for (const auto& d : u.records) { params(d.fields); typeBody(d.members); }
        for (const auto& d : u.interfaces) typeBody(d.members);
        for (const auto& d : u.classes) {
            // An `extern class` (e.g. the core prelude's Error/Iterable) is native-backed and not emitted —
            // its members are bound templates, not native properties/operators — so it requires no backend
            // feature support. Skipping it keeps an always-linked prelude from tripping a target's gating.
            if (d.isExtern) continue;
            if (!d.bases.empty()) mark(Feature::Inheritance, d.pos);
            // A mutable reference class = a non-extern class with a `var` field/property: assignable in place,
            // observable identity. The functional-subset targets (Haskell/Elixir) can't model it, so they gate
            // it via §3.E; C#/TS/Python/PHP support it, so this mark is inert for every current target.
            for (const auto& m : d.members)
                if ((m.kind == MemberKind::Field || m.kind == MemberKind::Property) && m.isMutable) {
                    mark(Feature::MutableRefClasses, d.pos);
                    break;
                }
            typeBody(d.members);
        }
    }

private:
    std::unordered_set<std::string> seen_; // capability keys — first use of each is the one reported
    std::unordered_set<std::string> interfaces_; // declared interface names (prescanned)
    std::unordered_set<std::string> externAttrNames_; // Tier 1 extern attribute names (prescanned)

    void mark(std::string key, SourcePos pos) {
        if (!seen_.insert(key).second) return;
        uses.push_back({std::move(key), pos});
    }
    void mark(Feature f, SourcePos pos) { mark(std::string(featureName(f)), pos); }

    // Fixed-width/unsigned integers and `char`/UTF-16 strings survive as syntactic TypeRef names even though
    // the MVP scalar lattice (Ty) collapses them — so type annotations are where these two axes are detected.
    // i64 is excluded (it's a full 64-bit width, native on the single-int targets that only emulate the rest).
    void typeRef(const TypeRef& t, SourcePos pos) {
        if (t.kind == TypeRef::Kind::Named) {
            const std::string& n = t.name;
            if (n == "i8" || n == "i16" || n == "i32" || n == "u8" || n == "u16" || n == "u32" || n == "u64")
                mark(Feature::FixedWidthIntegers, pos);
            else if (n == "char")
                mark(Feature::Utf16Strings, pos);
        }
        for (const auto& a : t.args) typeRef(a, pos);
        for (const auto& r : t.ret) typeRef(r, pos);
    }
    void params(const std::vector<Param>& ps) {
        for (const auto& p : ps) typeRef(p.type, p.pos);
    }

    // P37 C6: operator support is graded per sub-capability, not one blanket flag — PHP supports
    // `:eq` (structural equality) and `:indexers` (get/set) while refusing `:arithmetic`/`:comparison`/
    // `:conversion`. The bare `operatorOverloading` manifest stance umbrella-covers undeclared sub-keys.
    static std::string operatorCategory(const std::string& method) {
        if (method == "eq") return "eq";
        if (method == "lt" || method == "le" || method == "gt" || method == "ge") return "comparison";
        if (method == "get" || method == "set") return "indexers";
        if (method == "explicit" || method == "implicit") return "conversion";
        return "arithmetic"; // plus/minus/times/div/rem/band/bor/bxor/shl/shr/neg/bnot
    }

    void typeBody(const std::vector<Member>& members) {
        for (const auto& m : members) {
            if (m.kind == MemberKind::Operator)
                mark("operatorOverloading:" + operatorCategory(m.name), m.pos);
            if (m.kind == MemberKind::Property) mark(Feature::Properties, m.pos);
            if (m.kind == MemberKind::Property && m.hasSetter) mark(Feature::PropertySetters, m.pos); // #39c
            if (m.isAsync) mark(Feature::Async, m.pos);
            typeRef(m.type, m.pos);
            typeRef(m.returnType, m.pos);
            params(m.params);
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
        if (s->hasDeclType) typeRef(s->declType, s->pos); // `let x: u8` / `use r: T`
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
        if (e->kind == ExprKind::CharLit) mark(Feature::Utf16Strings, e->pos);
        if (e->kind == ExprKind::Cast)   typeRef(e->castType, e->pos);
        // P37 B4: an interface-typed runtime test gates per-target (TS interfaces erase — no instanceof).
        if ((e->kind == ExprKind::Is || e->kind == ExprKind::As) &&
            e->castType.kind == TypeRef::Kind::Named && interfaces_.count(e->castType.name))
            mark("interfaces:runtimeIdentity", e->pos);
        for (const auto& ta : e->typeArgs) typeRef(ta, e->pos); // explicit generic args, e.g. List<u32>()
        if (e->kind == ExprKind::Call && e->lhs && e->lhs->kind == ExprKind::Name) {
            calls.push_back({e->lhs->text, e->pos}); // a free-function call `name(...)`
            memberUses.push_back({{e->lhs->text + ".constructor"}, e->pos}); // or a construction `Type(args)`
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
            pattern(arm.pattern); // typed-arm interface tests (B4)
            expr(arm.guard.get());
            expr(arm.body.get());
            for (const auto& st : arm.block) stmt(st.get());
        }
    }

    void pattern(const Pattern& p) {
        if (p.hasType && p.type.kind == TypeRef::Kind::Named && interfaces_.count(p.type.name))
            mark("interfaces:runtimeIdentity", p.pos);
        for (const auto& s : p.sub) pattern(s);
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
        const std::string stance = backend.capabilityStance(use.key);
        if (stance == "false")
            diags.error(use.pos, std::string("target '") + backend.name() + "' does not support " +
                                      use.key + "; remove it or drop that target");
        else if (stance == "emulated")
            diags.warn(use.pos, std::string("target '") + backend.name() + "' emulates " +
                                     use.key + " (runtime behavior is faithful, but the emitted "
                                     "shape differs from a native construct)");
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
            const std::string member = m.kind == MemberKind::Constructor ? "constructor" : m.name;
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

    // P37 D12 (non-negotiable): a used pass-through attribute must have a non-refuse arm for THIS target —
    // the backstop that makes emit-only honest (no arm would silently drop the annotation).
    std::unordered_map<std::string, const ExternAttrDecl*> externAttrs;
    for (const auto& ea : unit.externAttrs) externAttrs[ea.name] = &ea;
    for (const AttrUse* a : c.tier1Uses_) {
        auto it = externAttrs.find(a->name);
        if (it == externAttrs.end()) continue;
        const ExternAttrArm* arm = nullptr;
        bool refused = false;
        for (const auto& cand : it->second->arms)
            if (cand.target == backend.name()) { arm = &cand; refused = cand.refuse; }
        if (!arm)
            diags.error(a->pos, "attribute '" + a->name + "' has no binding for target '" + backend.name() +
                                    "'; add an `actual(" + backend.name() +
                                    ")` arm (or an explicit `refuse`) or drop that target (D12)");
        else if (refused)
            diags.error(a->pos, "attribute '" + a->name + "' is refused on target '" + backend.name() +
                                    "' (its binding declares `refuse`); remove the attribute or drop that "
                                    "target");
    }
}

namespace {

// Every identifier the USER declares (names only — string literals, comments, and extern("…") templates
// are never visited, by construction: this walks declaration sites, P19 §7's hard invariant).
class NameCollector {
public:
    std::vector<std::pair<std::string, SourcePos>> names;

    void run(const CompilationUnit& u) {
        for (const auto& f : u.functions) {
            add(f.name, f.namePos);
            params(f.params);
            for (const auto& s : f.body) stmt(s.get());
        }
        for (const auto& v : u.values) add(v.name, v.namePos);
        for (const auto& e : u.enums) {
            add(e.name, e.pos);
            for (const auto& c : e.cases) add(c.name, e.pos);
        }
        for (const auto& un : u.unions) {
            add(un.name, un.pos);
            for (const auto& c : un.cases) {
                add(c.name, un.pos);
                for (const auto& p : c.params) add(p.name, un.pos);
            }
        }
        for (const auto& r : u.records) {
            add(r.name, r.pos);
            for (const auto& f : r.fields) add(f.name, r.pos);
            members(r.members);
        }
        for (const auto& d : u.classes) {
            if (d.isExtern) continue; // native-backed carrier — its names live on the target side
            add(d.name, d.pos);
            members(d.members);
        }
        for (const auto& d : u.interfaces) {
            add(d.name, d.pos);
            members(d.members);
        }
        for (const auto& e : u.extensions) {
            add(e.name, e.pos);
            params(e.params);
            for (const auto& s : e.body) stmt(s.get());
            expr(e.exprBody.get());
        }
    }

private:
    void add(const std::string& n, SourcePos pos) {
        if (!n.empty()) names.push_back({n, pos});
    }
    void params(const std::vector<Param>& ps) {
        for (const auto& p : ps) add(p.name, {});
    }
    void members(const std::vector<Member>& ms) {
        for (const auto& m : ms) {
            if (m.kind != MemberKind::Constructor) add(m.name, m.namePos);
            params(m.params);
            for (const auto& s : m.body) stmt(s.get());
            expr(m.exprBody.get());
            expr(m.init.get());
        }
    }
    void pattern(const Pattern& p) {
        if (p.kind == PatKind::Binding) add(p.name, p.pos);
        for (const auto& s : p.sub) pattern(s);
    }
    void stmt(const Stmt* s) {
        if (!s) return;
        if (s->kind == StmtKind::Let || s->kind == StmtKind::Use) add(s->name, s->namePos);
        if (s->kind == StmtKind::For) pattern(s->forBinding);
        expr(s->value.get());
        expr(s->target.get());
        for (const auto& st : s->thenBody) stmt(st.get());
        for (const auto& st : s->elseBody) stmt(st.get());
        for (const auto& c : s->catches) {
            add(c.name, c.pos);
            expr(c.guard.get());
            for (const auto& st : c.body) stmt(st.get());
        }
        for (const auto& st : s->finallyBody) stmt(st.get());
    }
    void expr(const Expr* e) {
        if (!e) return;
        for (const auto& p : e->params) add(p.name, e->pos); // lambda params
        for (const auto& arm : e->arms) {                     // match binders
            pattern(arm.pattern);
            expr(arm.guard.get());
            expr(arm.body.get());
            for (const auto& st : arm.block) stmt(st.get());
        }
        expr(e->lhs.get());
        expr(e->rhs.get());
        expr(e->extra.get());
        for (const auto& a : e->args) expr(a.get());
        for (const auto& st : e->block) stmt(st.get());
        for (const auto& f : e->fields) expr(f.value.get());
    }
};

// A reserved entry matches exactly, or as a prefix family when it ends in `*` (the lowering temps `__w*`).
bool matchesReserved(const std::string& entry, const std::string& name) {
    if (!entry.empty() && entry.back() == '*') return name.rfind(entry.substr(0, entry.size() - 1), 0) == 0;
    return entry == name;
}

} // namespace

void checkReservedNames(const CompilationUnit& unit, const Backend& backend,
                        const std::vector<std::pair<std::string, std::string>>& forbidden,
                        DiagnosticBag& diags) {
    NameCollector nc;
    nc.run(unit);
    for (const auto& [name, pos] : nc.names) {
        for (const auto& r : backend.reservedIdentifiers())
            if (matchesReserved(r, name))
                diags.error(pos, "identifier '" + name + "' is reserved by target '" + backend.name() +
                                     "' (its generated code uses it); rename it or drop that target");
        for (const auto& g : backend.globalIdentifiers())
            if (g == name)
                diags.error(pos, "identifier '" + name + "' shadows a runtime global of target '" +
                                     backend.name() + "'; rename it or drop that target");
        for (const auto& [tgt, fname] : forbidden)
            if ((tgt == "*" || tgt == backend.name()) && fname == name)
                diags.error(pos, "identifier '" + name + "' is forbidden by pgconfig for target '" +
                                     backend.name() + "'");
    }
}

} // namespace mintplayer::polyglot
