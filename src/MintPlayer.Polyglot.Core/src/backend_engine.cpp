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
    error = "unknown rule (expected string/tmpl/get/fn/case)";
    return r;
}

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
        case Rule::Kind::Lit: return r.s;
        case Rule::Kind::Get: return ctx.get(r.s);
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
    }
    return {};
}

} // namespace mintplayer::polyglot::engine
