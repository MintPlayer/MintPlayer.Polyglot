#pragma once

#include <string>
#include <unordered_map>
#include <vector>

// Declarative, per-target emission data — the "70% tabular" half of a backend, extracted from the
// hand-written emitters (see docs/design/backend-spec.md, P9). This grows slice by slice; the shared emit
// engine + the imperative "Hook" tier land in later P9 slices. For now each emitter consults its own spec
// for the data already extracted, with output identical to before (the differential/golden gates enforce it).

namespace mintplayer::polyglot {

struct BackendSpec {
    std::string name; // "csharp" / "typescript"

    // A leaf/scalar `.pg` type name -> its target-language spelling. Captures the per-target leaf mapping
    // (e.g. "i64" -> "long" / "bigint"; TS maps "char" -> "string", C# does not). Structural types
    // (List / Iterable / Error / tuple / function / nullable / generics) are still built in the emitter.
    std::unordered_map<std::string, std::string> scalarType;

    // An integer-literal type name -> the suffix its literals carry (C# {i64:"L", u64:"UL", u32:"U"};
    // TS {i64:"n", u64:"n"}). A type not in the map takes no suffix.
    std::unordered_map<std::string, std::string> intSuffix;

    // A binary-operator symbol -> its target spelling, for operators that diverge from the source symbol
    // (TS {"==":"===", "!=":"!=="} — never JS loose ==/!=; C# emits every operator verbatim). A missing
    // entry means "emit the source symbol unchanged"; see binOp(). Numeric-overflow wrapping and operator
    // overloading are imperative Hooks, not table entries — this is only the bare spelling.
    std::unordered_map<std::string, std::string> binaryOp;

    // The target spelling of a binary operator (default: the source symbol verbatim).
    std::string binOp(const std::string& op) const {
        auto it = binaryOp.find(op);
        return it == binaryOp.end() ? op : it->second;
    }

    // Per-node bracketing for the fixed-shape "delimited list of rendered children" node family — the affix
    // that differs per target (a tuple is `(a, b)` in C#, `[a, b]` in TS). The engine renders the children
    // and wraps them via renderDelimited(); the IR node kind keys the map (e.g. "tuple"). The argument-list
    // affix ("(", ", ", ")") is identical across targets, so it stays a literal in the engine, not here.
    struct DelimitedTemplate { std::string open, sep, close; };
    std::unordered_map<std::string, DelimitedTemplate> delimited;

    // (`print` and `Math` used to live here as naming rules; they are now real std modules — std.io's
    // generic `print<T>` and the std.math `extern class` — bound per target via templates, so no naming
    // data lives in the backend spec anymore. It carries only type/literal/template tables now.)
};

// --- Shared emit-engine primitives (the seed of the P9 SpecEmitter) -------------------------------------
// These render a node from already-emitted child strings. Identical-across-targets shapes live here as
// shared rules; per-target shapes take their affix from a BackendSpec table. The caller must emit the
// children in a defined left-to-right order before calling (C++ leaves operator+/argument evaluation order
// unspecified, and a child may bump a per-emitter counter).

// A delimited list of children: open + children joined by sep + close (tuple, arg list, …).
inline std::string renderDelimited(const BackendSpec::DelimitedTemplate& t,
                                   const std::vector<std::string>& children) {
    std::string s = t.open;
    for (std::size_t i = 0; i < children.size(); ++i) { if (i) s += t.sep; s += children[i]; }
    return s + t.close;
}

// A conditional expression `(c ? t : e)` — identical spelling in C# and TS, so a shared rule.
inline std::string renderCond(const std::string& c, const std::string& t, const std::string& e) {
    return "(" + c + " ? " + t + " : " + e + ")";
}

// An argument list `(a, b, c)` — the affix is identical across C# and TS (unlike tuple brackets), so it's a
// shared engine constant rather than per-backend spec data. Callers prepend the callee/`new T`/`recv.m`.
inline std::string renderArgs(const std::vector<std::string>& children) {
    return renderDelimited({"(", ", ", ")"}, children);
}

// Binary-operator precedence (higher binds tighter), used by the engine for parenthesization. Identical
// across C# and TS — a shared engine concern, not per-backend data — so it lives here, not in a BackendSpec.
inline int operatorPrecedence(const std::string& op) {
    if (op == "||") return 1;
    if (op == "&&") return 2;
    if (op == "==" || op == "!=") return 3;
    if (op == "<" || op == "<=" || op == ">" || op == ">=") return 4;
    if (op == "+" || op == "-") return 5;
    if (op == "*" || op == "/" || op == "%") return 6;
    return 7;
}

} // namespace mintplayer::polyglot
