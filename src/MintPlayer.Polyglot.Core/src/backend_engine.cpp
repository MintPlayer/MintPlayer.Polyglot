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
    if (v.has("fn")) {
        r.kind = Rule::Kind::Fn;
        r.s = v["fn"].asString();
        for (const json::Value& a : v["args"].items()) r.parts.push_back(parseRule(a, ok, error));
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

// Scopes a `map` item template to one list element: `item`/`item.…` paths are rewritten onto the element's
// indexed path (`node.fields.<i>…`) before delegating, so the underlying context needs no new methods —
// an indexed child path is already first-class.
class ItemCtx : public EvalContext {
public:
    ItemCtx(const EvalContext& base, std::string prefix) : base_(base), prefix_(std::move(prefix)) {}

    std::string get(const std::string& p) const override { return base_.get(redirect(p)); }
    bool has(const std::string& p) const override { return base_.has(redirect(p)); }
    std::string emitChild(const std::string& p, const std::string& side) const override {
        return base_.emitChild(redirect(p), side);
    }
    std::string builtin(const std::string& name, const std::vector<std::string>& args) const override {
        return base_.builtin(name, args);
    }

private:
    std::string redirect(const std::string& p) const {
        if (p == "item") return prefix_;
        if (p.rfind("item.", 0) == 0) return prefix_ + p.substr(4); // "item.value" -> "<prefix>.value"
        return p;
    }

    const EvalContext& base_;
    std::string prefix_;
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

std::string evalRule(const Rule& r, const EvalContext& ctx) {
    switch (r.kind) {
        case Rule::Kind::Lit:  return r.s;
        case Rule::Kind::Get:  return ctx.get(r.s);
        case Rule::Kind::Emit: return ctx.emitChild(r.s, r.side);
        case Rule::Kind::Tmpl: {
            std::string out;
            for (const Rule& p : r.parts) out += evalRule(p, ctx);
            return out;
        }
        case Rule::Kind::Fn: {
            std::vector<std::string> args;
            args.reserve(r.parts.size());
            for (const Rule& a : r.parts) args.push_back(evalRule(a, ctx));
            return ctx.builtin(r.s, args);
        }
        case Rule::Kind::Case:
            for (const auto& arm : r.arms)
                if (evalTest(arm.first, ctx)) return evalRule(arm.second, ctx);
            return r.elseBody.empty() ? std::string() : evalRule(r.elseBody[0], ctx);
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
                else out += evalRule(r.parts[0], ItemCtx(ctx, elem));              // item template per element
            }
            return out;
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
                out += evalRule(r.parts[0], ItemCtx(ctx, r.s + "." + std::to_string(i)));
                if (i < nHoles) out += evalRule(r.parts[1], ItemCtx(ctx, r.s2 + "." + std::to_string(i)));
            }
            return out;
        }
    }
    return {};
}

} // namespace mintplayer::polyglot::engine
