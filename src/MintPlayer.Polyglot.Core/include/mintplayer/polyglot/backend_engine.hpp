#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mintplayer/polyglot/json.hpp"

// The JSON emission-DSL interpreter (Design A — PRD §4.10 / docs/design/backend-spec.md §6). A backend's
// imperative "Hook" tier becomes a table of declarative `Rule`s, one per IR node kind, evaluated by this
// fixed, non-Turing-complete interpreter. The interpreter has no primitive that runs plugin-supplied code —
// only selection-among + substitution-into the plugin's own templates — so it is RCE-safe by construction.
//
// This first slice is the interpreter SPINE: the scalar rule forms (literal / tmpl / get / case+Test / fn),
// evaluated against an EvalContext. The recursive child-emission primitives (`emit`/`emitChild`/`map`/`fold`)
// are added when the interpreter is wired to the real `ir::Expr` walk (a later P18 slice), where their shape
// is grounded in the IR rather than guessed — the note's "extract from working backends, never guess" rule.

namespace mintplayer::polyglot::engine {

// How a Rule reads scalar node fields, tests presence, and invokes the fixed builtin set. The concrete
// backend implements this over `ir::` (a later slice); tests implement a lightweight stand-in. The builtin
// vocabulary is fixed and in-Core (ident/casing/escape/opSpelling/wrap/…) — a plugin selects among them,
// never adds one (adding a builtin is a trusted Core change).
class EvalContext {
public:
    virtual ~EvalContext() = default;
    virtual std::string get(const std::string& path) const = 0;   // {"get":"node.op"} — "" when absent
    virtual bool has(const std::string& path) const = 0;          // Test {"has":"node.guard"}
    virtual std::string builtin(const std::string& name, const std::vector<std::string>& args) const = 0;
    // {"type":path} — render the TypeRef at `path` through the plugin's type rules (the "Type" table
    // entry, evaluated against a type-scoped context). "" when the path names no type.
    virtual std::string renderType(const std::string& /*path*/) const { return ""; }
    // Resolve a rule path through any item-scoping wrappers to its absolute form (`item.body` inside a
    // `mapDecl` element -> `decl.methods.3.body`) — the decl interpreter's `stmts` resolves lists this way.
    virtual std::string resolvePath(const std::string& p) const { return p; }
    // Recurse into a child IR node at `path`, emitting it. `side` selects precedence parenthesization computed
    // by the CONTEXT (not the plugin): "" = plain, "l"/"r" = a binary operand (wrap by precedence+associativity),
    // "recv" = an atom receiver (wrap a binary/unary/cast). Keeping the paren algorithm in the context (fixed
    // C++) means plugins declare only the precedence table, never author paren logic. A `map` rule walks a
    // child *list* by asking `get("<path>.count")` for its length, then `emitChild("<path>.<i>", side)` for each
    // element — so an indexed path (`node.args.0`) is a first-class child path, no new context method needed.
    virtual std::string emitChild(const std::string& path, const std::string& side) const = 0;
};

// A boolean test over context values — deliberately weak (equality / presence / combinators; no arithmetic,
// no plugin loops) so the interpreter stays bounded and non-Turing-complete.
struct Test {
    enum class Kind { Eq, Has, And, Or, Not } kind = Kind::Has;
    std::string path;         // Eq / Has: the field path
    std::string value;        // Eq: the expected value
    std::vector<Test> subs;   // And / Or (n) / Not (1)
};

// A parsed emission Rule. String-flavor kinds evaluate to a string (expressions/heads); DECL-flavor kinds
// (Line/Block/MapDecl/Stmts/Seq) write indented lines into the emitter and are interpreted by
// EmitterBase::runDeclRule (declaration shapes need block structure, not a return string).
struct Rule {
    enum class Kind {
        Lit, Tmpl, Get, Fn, Case, Emit, Map, Interleave, Fold, Call, Type, // string flavor
        Line, Block, MapDecl, Stmts, Seq,                                  // decl flavor
    } kind = Kind::Lit;
    std::string s;          // Lit: text | Get/Emit/Map/Fold/Type/MapDecl/Stmts: path | Fn/Call: name | Interleave: lits
    std::string s2;                             // Interleave: holes path
    std::string side;                           // Emit/Map: precedence side ("" / "l" / "r" / "recv")
    std::string sep;                            // Map: separator between rendered children
    std::vector<Rule> parts;   // Tmpl: parts | Fn: args | Map/MapDecl: item | Interleave/Fold: 2 | Line: 1 | Block: head+body | Seq: steps
    std::vector<std::pair<Test, Rule>> arms;    // Case: [test, body] pairs, first match wins
    std::vector<Rule> elseBody;                 // Case: 0-or-1 else rule
};

// Scopes a `map`/`mapDecl` item template to one list element: `item`/`item.…` paths are rewritten onto the
// element's indexed path before delegating; `item.#` answers the element index. Public so the decl-rule
// interpreter (emitter_base) can reuse it.
class ItemCtx : public EvalContext {
public:
    ItemCtx(const EvalContext& base, std::string prefix, int index = 0)
        : base_(base), prefix_(std::move(prefix)), index_(index) {}

    std::string get(const std::string& p) const override {
        if (p == "item.#") return std::to_string(index_);
        return base_.get(redirect(p));
    }
    bool has(const std::string& p) const override { return base_.has(redirect(p)); }
    std::string emitChild(const std::string& p, const std::string& side) const override {
        return base_.emitChild(redirect(p), side);
    }
    std::string builtin(const std::string& name, const std::vector<std::string>& args) const override {
        return base_.builtin(name, args);
    }
    std::string renderType(const std::string& p) const override { return base_.renderType(redirect(p)); }
    std::string resolvePath(const std::string& p) const override { return base_.resolvePath(redirect(p)); }
    std::string redirect(const std::string& p) const {
        if (p == "item") return prefix_;
        if (p.rfind("item.", 0) == 0) return prefix_ + p.substr(4); // "item.value" -> "<prefix>.value"
        return p;
    }

private:
    const EvalContext& base_;
    std::string prefix_;
    int index_;
};

using RuleTable = std::unordered_map<std::string, Rule>;

// Parse a JSON value into a Rule / Test. On a malformed rule, sets ok=false + a message in `error` and
// returns a benign empty Rule — a spec never silently misparses (the anti-silent-drop rule).
Rule parseRule(const json::Value& v, bool& ok, std::string& error);
Test parseTest(const json::Value& v, bool& ok, std::string& error);

// Evaluate a Rule / Test against a context. `helpers` resolves {"call":"name"} sub-rules (a plugin's own
// named helpers in the same table, keyed by non-node names); recursion is depth-capped — the DSL stays
// non-Turing-complete (a helper cycle bottoms out instead of looping).
std::string evalRule(const Rule& r, const EvalContext& ctx, const RuleTable* helpers = nullptr, int depth = 0);
bool evalTest(const Test& t, const EvalContext& ctx);

} // namespace mintplayer::polyglot::engine
