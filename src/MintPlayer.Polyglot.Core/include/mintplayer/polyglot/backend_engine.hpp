#pragma once

#include <string>
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
    // Recurse into a child IR node at `path`, emitting it. `side` selects precedence parenthesization computed
    // by the CONTEXT (not the plugin): "" = plain, "l"/"r" = a binary operand (wrap by precedence+associativity),
    // "recv" = an atom receiver (wrap a binary/unary/cast). Keeping the paren algorithm in the context (fixed
    // C++) means plugins declare only the precedence table, never author paren logic.
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

// A parsed emission Rule (the scalar spine — see the header note for the deferred recursive forms).
struct Rule {
    enum class Kind { Lit, Tmpl, Get, Fn, Case, Emit } kind = Kind::Lit;
    std::string s;                              // Lit: text | Get/Emit: path | Fn: builtin name
    std::string side;                           // Emit: precedence side ("" / "l" / "r" / "recv")
    std::vector<Rule> parts;                    // Tmpl: parts | Fn: args
    std::vector<std::pair<Test, Rule>> arms;    // Case: [test, body] pairs, first match wins
    std::vector<Rule> elseBody;                 // Case: 0-or-1 else rule
};

// Parse a JSON value into a Rule / Test. On a malformed rule, sets ok=false + a message in `error` and
// returns a benign empty Rule — a spec never silently misparses (the anti-silent-drop rule).
Rule parseRule(const json::Value& v, bool& ok, std::string& error);
Test parseTest(const json::Value& v, bool& ok, std::string& error);

// Evaluate a Rule / Test against a context.
std::string evalRule(const Rule& r, const EvalContext& ctx);
bool evalTest(const Test& t, const EvalContext& ctx);

} // namespace mintplayer::polyglot::engine
