#include "mintplayer/polyglot/backend_engine.hpp"

namespace mintplayer::polyglot::engine {

Test parseTest(const json::Value& v, bool& ok, std::string& error) {
    Test t;
    if (v.kind != json::Value::Kind::Object) {
        ok = false;
        error = "test must be a JSON object";
        return t;
    }
    if (v.has("eq")) {
        const std::vector<json::Value>& a = v["eq"].items();
        if (a.size() != 2 || a[0].kind != json::Value::Kind::String) {
            ok = false;
            error = "test 'eq' must be [path, value]";
            return t;
        }
        t.kind = Test::Kind::Eq;
        t.path = a[0].asString();
        t.value = a[1].asString();
        return t;
    }
    if (v.has("has")) {
        t.kind = Test::Kind::Has;
        t.path = v["has"].asString();
        return t;
    }
    if (v.has("and") || v.has("or")) {
        t.kind = v.has("and") ? Test::Kind::And : Test::Kind::Or;
        for (const json::Value& sub : v[v.has("and") ? "and" : "or"].items())
            t.subs.push_back(parseTest(sub, ok, error));
        return t;
    }
    if (v.has("not")) {
        t.kind = Test::Kind::Not;
        t.subs.push_back(parseTest(v["not"], ok, error));
        return t;
    }
    ok = false;
    error = "unknown test (expected eq/has/and/or/not)";
    return t;
}

Rule parseRule(const json::Value& v, bool& ok, std::string& error) {
    Rule r;
    if (v.kind == json::Value::Kind::String) {
        r.kind = Rule::Kind::Lit;
        r.s = v.asString();
        return r;
    }
    if (v.kind != json::Value::Kind::Object) {
        ok = false;
        error = "rule must be a string or object";
        return r;
    }
    if (v.has("tmpl")) {
        r.kind = Rule::Kind::Tmpl;
        for (const json::Value& p : v["tmpl"].items()) r.parts.push_back(parseRule(p, ok, error));
        return r;
    }
    if (v.has("get")) {
        r.kind = Rule::Kind::Get;
        r.s = v["get"].asString();
        return r;
    }
    if (v.has("emit")) {
        r.kind = Rule::Kind::Emit;
        r.s = v["emit"].asString(); // plain recurse (no precedence wrapping)
        return r;
    }
    if (v.has("emitChild")) {
        r.kind = Rule::Kind::Emit;
        r.s = v["emitChild"].asString();
        r.side = v["side"].asString(); // "l"/"r"/"recv" — the context computes the parenthesization
        return r;
    }
    if (v.has("map")) { // emit each child in the list at `map`, joined by `sep` (optional `side` per element)
        r.kind = Rule::Kind::Map;
        r.s = v["map"].asString();
        r.sep = v.has("sep") ? v["sep"].asString() : std::string();
        r.side = v.has("side") ? v["side"].asString() : std::string();
        // Optional per-element template: rendered once per element with `item.…` paths resolving against
        // that element (e.g. `{"tmpl":[{"get":"item.name"},": ",{"emit":"item.value"}]}`). Without it, each
        // element is emitted directly (the plain child-expr case).
        if (v.has("item")) r.parts.push_back(parseRule(v["item"], ok, error));
        return r;
    }
    if (v.has("interleave")) { // zip a literal list with a hole list: lit0 hole0 lit1 … litN (interpolation)
        const json::Value& iv = v["interleave"];
        r.kind = Rule::Kind::Interleave;
        r.s = iv["lits"].asString();
        r.s2 = iv["holes"].asString();
        r.parts.push_back(parseRule(iv["lit"], ok, error));   // per-literal template (`item` = the chunk)
        r.parts.push_back(parseRule(iv["hole"], ok, error));  // per-hole template (`item` = the hole expr)
        return r;
    }
    if (v.has("fold")) { // right-fold over a child list (Python's match ternary chain): the LAST element
        const json::Value& fv = v["fold"]; // renders via `seed`; earlier ones via `each`, which reads the
        r.kind = Rule::Kind::Fold;         // accumulated tail as {"get":"acc"}. Depth = list length (bounded).
        r.s = fv["list"].asString();
        r.parts.push_back(parseRule(fv["each"], ok, error));
        r.parts.push_back(parseRule(fv["seed"], ok, error));
        return r;
    }
    if (v.has("fn")) {
        r.kind = Rule::Kind::Fn;
        r.s = v["fn"].asString();
        for (const json::Value& a : v["args"].items()) r.parts.push_back(parseRule(a, ok, error));
        return r;
    }
    if (v.has("call")) { // a named helper sub-rule in the same table (depth-capped at eval)
        r.kind = Rule::Kind::Call;
        r.s = v["call"].asString();
        return r;
    }
    if (v.has("type")) { // render the TypeRef at `path` through the plugin's type rules
        r.kind = Rule::Kind::Type;
        r.s = v["type"].asString();
        return r;
    }
    // ---- decl flavor (interpreted by EmitterBase::runDeclRule — writes lines, returns nothing) ----------
    if (v.has("line")) { // one indented line; the payload is a string-flavor rule
        r.kind = Rule::Kind::Line;
        r.parts.push_back(parseRule(v["line"], ok, error));
        return r;
    }
    if (v.has("block")) { // open a block with `head` (string rule), body = decl rules, close per blockStyle
        const json::Value& bv = v["block"];
        r.kind = Rule::Kind::Block;
        r.parts.push_back(parseRule(bv["head"], ok, error));
        for (const json::Value& b : bv["body"].items()) r.parts.push_back(parseRule(b, ok, error));
        return r;
    }
    if (v.has("mapDecl")) { // run the `each` decl rule once per element of the list at `mapDecl`
        r.kind = Rule::Kind::MapDecl;
        r.s = v["mapDecl"].asString();
        r.parts.push_back(parseRule(v["each"], ok, error));
        return r;
    }
    if (v.has("fresh")) { // mint a single-eval temp: {"fresh":{"prefix":…,"as":…,"in":rule}}
        const json::Value& fv = v["fresh"];
        r.kind = Rule::Kind::Fresh;
        r.s = fv["prefix"].asString();
        r.s2 = fv["as"].asString();
        r.parts.push_back(parseRule(fv["in"], ok, error));
        return r;
    }
    if (v.has("mapMembers")) { // run the named decl rule once per member of the list at `path`, each
        r.kind = Rule::Kind::MapMembers; // against its own member-scoped root context (a member is a decl)
        r.s = v["mapMembers"].asString();
        r.s2 = v["rule"].asString();
        return r;
    }
    if (v.has("stmts")) { // emit the ir statement list at `path` through the shared statement walk
        r.kind = Rule::Kind::Stmts;
        r.s = v["stmts"].asString();
        return r;
    }
    if (v.has("seq")) { // run decl rules in order
        r.kind = Rule::Kind::Seq;
        for (const json::Value& p : v["seq"].items()) r.parts.push_back(parseRule(p, ok, error));
        return r;
    }
    if (v.has("indent")) { // run decl rules one level deeper (Block minus the head/close — manual joins)
        r.kind = Rule::Kind::Indent;
        for (const json::Value& p : v["indent"].items()) r.parts.push_back(parseRule(p, ok, error));
        return r;
    }
    if (v.has("case")) {
        r.kind = Rule::Kind::Case;
        for (const json::Value& arm : v["case"]["when"].items()) {
            const std::vector<json::Value>& pair = arm.items();
            if (pair.size() != 2) {
                ok = false;
                error = "case 'when' entry must be [test, rule]";
                return r;
            }
            r.arms.emplace_back(parseTest(pair[0], ok, error), parseRule(pair[1], ok, error));
        }
        if (v["case"].has("else")) r.elseBody.push_back(parseRule(v["case"]["else"], ok, error));
        return r;
    }
    ok = false;
    error = "unknown rule (expected string/tmpl/get/fn/case/emit/emitChild/map)";
    return r;
}

namespace {

// (ItemCtx moved to the header — the decl-rule interpreter in emitter_base reuses it.)

// Exposes a fold's accumulated tail as the scalar `acc`; everything else delegates.
class AccCtx : public EvalContext {
public:
    AccCtx(const EvalContext& base, const std::string& acc) : base_(base), acc_(acc) {}

    std::string get(const std::string& p) const override { return p == "acc" ? acc_ : base_.get(p); }
    bool has(const std::string& p) const override { return p == "acc" ? !acc_.empty() : base_.has(p); }
    std::string emitChild(const std::string& p, const std::string& side) const override {
        return base_.emitChild(p, side);
    }
    std::string builtin(const std::string& name, const std::vector<std::string>& args) const override {
        return base_.builtin(name, args);
    }
    std::string renderType(const std::string& p) const override { return base_.renderType(p); }
    std::string freshName(const std::string& p) const override { return base_.freshName(p); }

private:
    const EvalContext& base_;
    const std::string& acc_;
};

// Exposes a `{"fresh":…}` rule's minted temporary under its declared alias; everything else delegates.
class FreshCtx : public EvalContext {
public:
    FreshCtx(const EvalContext& base, const std::string& alias, std::string name)
        : base_(base), alias_(alias), name_(std::move(name)) {}

    std::string get(const std::string& p) const override { return p == alias_ ? name_ : base_.get(p); }
    bool has(const std::string& p) const override { return p == alias_ ? !name_.empty() : base_.has(p); }
    std::string emitChild(const std::string& p, const std::string& side) const override {
        return base_.emitChild(p, side);
    }
    std::string builtin(const std::string& name, const std::vector<std::string>& args) const override {
        return base_.builtin(name, args);
    }
    std::string renderType(const std::string& p) const override { return base_.renderType(p); }
    std::string resolvePath(const std::string& p) const override { return base_.resolvePath(p); }
    std::string freshName(const std::string& p) const override { return base_.freshName(p); }

private:
    const EvalContext& base_;
    const std::string& alias_;
    std::string name_;
};

} // namespace

bool evalTest(const Test& t, const EvalContext& ctx) {
    switch (t.kind) {
        case Test::Kind::Eq:  return ctx.get(t.path) == t.value;
        case Test::Kind::Has: return ctx.has(t.path);
        case Test::Kind::Not: return t.subs.empty() ? true : !evalTest(t.subs[0], ctx);
        case Test::Kind::And:
            for (const Test& s : t.subs) if (!evalTest(s, ctx)) return false;
            return true;
        case Test::Kind::Or:
            for (const Test& s : t.subs) if (evalTest(s, ctx)) return true;
            return false;
    }
    return false;
}

std::string evalRule(const Rule& r, const EvalContext& ctx, const RuleTable* helpers, int depth) {
    switch (r.kind) {
        case Rule::Kind::Lit:  return r.s;
        case Rule::Kind::Get:  return ctx.get(r.s);
        case Rule::Kind::Emit: return ctx.emitChild(r.s, r.side);
        case Rule::Kind::Tmpl: {
            std::string out;
            for (const Rule& p : r.parts) out += evalRule(p, ctx, helpers, depth);
            return out;
        }
        case Rule::Kind::Fn: {
            std::vector<std::string> args;
            args.reserve(r.parts.size());
            for (const Rule& a : r.parts) args.push_back(evalRule(a, ctx, helpers, depth));
            return ctx.builtin(r.s, args);
        }
        case Rule::Kind::Case:
            for (const auto& arm : r.arms)
                if (evalTest(arm.first, ctx)) return evalRule(arm.second, ctx, helpers, depth);
            return r.elseBody.empty() ? std::string() : evalRule(r.elseBody[0], ctx, helpers, depth);
        case Rule::Kind::Map: {
            // The list length is a context scalar (`<path>.count`); each element is an indexed child path.
            int n = 0;
            const std::string count = ctx.get(r.s + ".count");
            for (char c : count) { if (c < '0' || c > '9') { n = 0; break; } n = n * 10 + (c - '0'); }
            std::string out;
            for (int i = 0; i < n; ++i) {
                if (i) out += r.sep;
                const std::string elem = r.s + "." + std::to_string(i);
                if (r.parts.empty()) out += ctx.emitChild(elem, r.side);           // plain: emit the element
                else out += evalRule(r.parts[0], ItemCtx(ctx, elem, i), helpers, depth);           // item template per element
            }
            return out;
        }
        case Rule::Kind::Fold: {
            // Right-fold: the last element is the seed; earlier elements render via `each` with `acc` bound
            // to the already-rendered tail (Python's match `v0 if c0 else (v1 if c1 else vLast)`).
            int n = 0;
            for (char c : ctx.get(r.s + ".count")) { if (c < '0' || c > '9') { n = 0; break; } n = n * 10 + (c - '0'); }
            if (n == 0) return "";
            std::string acc = evalRule(r.parts[1], ItemCtx(ctx, r.s + "." + std::to_string(n - 1), n - 1), helpers, depth);
            for (int i = n - 2; i >= 0; --i) {
                ItemCtx item(ctx, r.s + "." + std::to_string(i), i);
                acc = evalRule(r.parts[0], AccCtx(item, acc), helpers, depth);
            }
            return acc;
        }
        case Rule::Kind::Interleave: {
            // lit0 hole0 lit1 hole1 … litN — string interpolation's chunks/holes zip. Counts come from the
            // context (`<path>.count`); each element renders through its template with `item` scoped to it.
            auto count = [&](const std::string& path) {
                int n = 0;
                for (char c : ctx.get(path + ".count")) { if (c < '0' || c > '9') return 0; n = n * 10 + (c - '0'); }
                return n;
            };
            const int nLits = count(r.s), nHoles = count(r.s2);
            std::string out;
            for (int i = 0; i < nLits; ++i) {
                out += evalRule(r.parts[0], ItemCtx(ctx, r.s + "." + std::to_string(i), i), helpers, depth);
                if (i < nHoles) out += evalRule(r.parts[1], ItemCtx(ctx, r.s2 + "." + std::to_string(i), i), helpers, depth);
            }
            return out;
        }
        case Rule::Kind::Type: return ctx.renderType(r.s);
        case Rule::Kind::Fresh: { // mint once, expose under the alias for every read in the body
            FreshCtx f(ctx, r.s2, ctx.freshName(r.s));
            return evalRule(r.parts[0], f, helpers, depth);
        }
        case Rule::Kind::Call: {
            // A named helper sub-rule from the plugin's own table. Depth-capped so a helper cycle bottoms
            // out loudly instead of looping — the DSL stays non-Turing-complete. (Load-time validation
            // rejects unknown call targets; the poison strings are the belt-and-braces runtime guard.)
            if (!helpers || depth > 64) return "<call-depth-exceeded:" + r.s + ">";
            auto it = helpers->find(r.s);
            if (it == helpers->end()) return "<unknown-helper:" + r.s + ">";
            return evalRule(it->second, ctx, helpers, depth + 1);
        }
    }
    return {};
}

} // namespace mintplayer::polyglot::engine
