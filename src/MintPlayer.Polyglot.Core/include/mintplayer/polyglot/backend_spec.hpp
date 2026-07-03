#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Declarative, per-target emission data — the "70% tabular" half of a backend, extracted from the
// hand-written emitters (see docs/design/backend-spec.md, P9). This grows slice by slice; the shared emit
// engine + the imperative "Hook" tier land in later P9 slices. For now each emitter consults its own spec
// for the data already extracted, with output identical to before (the differential/golden gates enforce it).

namespace mintplayer::polyglot {

// How a target delimits a block. The brace family differs only in where `{` sits; an indentation target
// (Python) has no braces at all — `head:` then an indented body, no closer. The shared statement engine
// (EmitterBase) consults this so one statement walk serves all three targets.
enum class BlockStyle {
    BracesAllman, // `head` ⏎ `{` … `}`   (C#)
    BracesKnR,    // `head {` … `}`        (TypeScript)
    ColonIndent,  // `head:` … (indent only, no closer)  (Python)
};

struct BackendSpec {
    std::string name; // "csharp" / "typescript" / "python"

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

    // Statement-shape data the shared engine consults (previously constant-returning virtual hooks — pure
    // per-target *data* misfiled as behavior; the three-backend spike confirmed these are the only such
    // knobs). `blockStyle` picks brace-family vs colon+indent; `stmtEnd` is the terminator (";" / "" for
    // Python); `throwKeyword` spells a value `throw`/`raise`. Defaults suit the brace family (C#/TS).
    BlockStyle blockStyle = BlockStyle::BracesKnR;
    std::string stmtEnd = ";";
    std::string throwKeyword = "throw";
    // The value-less rethrow statement, terminator included (`throw;` / `throw __e;` / `raise`). The other
    // two statement spellings — the local-declaration keyword and the yield forms — are `tables` rows
    // (`localDecl`: mutable/const, `yield`: value/empty) applied by the shared statement walk.
    std::string rethrow = "throw;";

    // Boolean / null literal spellings (C#/TS `true`/`false`/`null`; Python `True`/`False`/`None`). Read by
    // each backend's expression walk. (`this`/`self` is NOT here — C# rebinds it in a `with`-copy IIFE, so it
    // carries behavior, not just a spelling.)
    std::string trueLit = "true";
    std::string falseLit = "false";
    std::string nullLit = "null";

    // (`print` and `Math` used to live here as naming rules; they are now real std modules — std.io's
    // generic `print<T>` and the std.math `extern class` — bound per target via templates, so no naming
    // data lives in the backend spec anymore. It carries only type/literal/template tables now.)

    // Generic-parameter spelling strategy. `genericsStyle`: "inlineBounds" carries bounds on each
    // parameter (TS `<T extends A & B, U>`), "whereClauses" spells bare names and puts bounds in trailing
    // clauses (C# `<T, U> where T : A, B`), "" emits nothing (Python). `genericsErase` lists compile-time-
    // only marker bounds with no target spelling (INumber) — erased, and a param whose bounds all erase is
    // spelled bare.
    std::string genericsStyle;
    std::string genericsBoundsIntro; // between the name and its bounds (TS " extends ", C# where " : ")
    std::string genericsBoundsSep;   // between two bounds (TS " & ", C# ", ")
    std::unordered_set<std::string> genericsErase;

    // Atom-parenthesization policy: which child node kinds need parens as a member/call receiver ("recv")
    // or a unary operand ("unary"). Vocabulary: "binary", "unary", "cast", "cond", and "binaryScalar" (a
    // binary whose lhs is NOT a user type — TS rewrites user-type binaries to high-binding method calls,
    // so only scalar binaries keep operator form and need receiver parens).
    std::vector<std::string> wrapAtomRecv, wrapAtomUnary;

    // Named lookup tables — the generic `{"fn":"table","args":[name, key]}` catalog entry (a missing key
    // answers ""). First occupant: TS's operator-overload method names (`"+"` -> `"plus"` — no TS operator
    // overloading, so `a + b` on a user type calls the method). The `hasOpMethod` node read consults it.
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> tables;

    // §3.C integer-overflow faithfulness: per-width wrap templates (`$x` = the expression). Masks on an
    // arbitrary-precision target (Python), bitwise re-narrowing + BigInt.asIntN/asUintN on JS, a cast-back
    // on C# (whose sub-32 arithmetic promotes to int). A width with no row emits unwrapped.
    std::unordered_map<std::string, std::string> wrapInt;

    // Named escape maps for the target's string-ish literal contexts (source sequence -> replacement):
    // C# `$"…"` interpolation chunks double braces, TS template literals escape backtick + `${`, C# char
    // literals escape `'`. Applied by the generic specEscape catalog entry — per-target escape CODE is gone.
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> escapes;

    // Identifier repair (the spec half of P19 §7's `identifiers` manifest block): the target's reserved
    // words and how a colliding `.pg` identifier escapes — "prefix" (C# `@name`) / "suffix" (Python
    // `name_`) / "" = emit verbatim (TS escapes nothing). `mangle` repairs emitted names whose characters
    // the target forbids (Python: the overload marker `$` -> `_`, applied at every def and call site).
    std::unordered_set<std::string> keywords;
    std::string escapeStrategy; // "prefix" | "suffix" | "" (none)
    std::string escapeWith;
    std::string mangleFrom, mangleTo; // single-char emitted-name replacement ("" = none)
};

// A `.pg` identifier spelled for the target: keyword collisions escape per the spec's declared strategy.
inline std::string specIdent(const BackendSpec& s, const std::string& n) {
    if (s.keywords.count(n) == 0) return n;
    if (s.escapeStrategy == "prefix") return s.escapeWith + n;
    if (s.escapeStrategy == "suffix") return n + s.escapeWith;
    return n;
}

// Look up `key` in the spec's named table ("" when the table or key is absent).
inline std::string specTable(const BackendSpec& s, const std::string& table, const std::string& key) {
    auto t = s.tables.find(table);
    if (t == s.tables.end()) return "";
    auto it = t->second.find(key);
    return it == t->second.end() ? std::string() : it->second;
}

// Substitute `x` into a template's `$x` holes.
inline std::string substX(const std::string& t, const std::string& x) {
    std::string out;
    for (std::size_t i = 0; i < t.size();) {
        if (t[i] == '$' && i + 1 < t.size() && t[i + 1] == 'x') { out += x; i += 2; }
        else out += t[i++];
    }
    return out;
}

// Wrap an int-typed expression to its declared width via the spec's template (`$x` = the expression);
// identity when the spec declares no row for the width.
inline std::string specWrapInt(const BackendSpec& s, const std::string& tn, const std::string& x) {
    auto it = s.wrapInt.find(tn);
    return it == s.wrapInt.end() ? x : substX(it->second, x);
}

// Look up a template in a named spec table and substitute `x` into its `$x` holes; identity (returns `x`)
// when the table or key is absent.
inline std::string specSubst(const BackendSpec& s, const std::string& table, const std::string& key,
                             const std::string& x) {
    auto t = s.tables.find(table);
    if (t == s.tables.end()) return x;
    auto it = t->second.find(key);
    return it == t->second.end() ? x : substX(it->second, x);
}

// Apply the named escape map to `text`: at each position the longest declared source sequence wins
// (2 chars, then 1); unmatched characters pass through. An unknown map name is the identity transform.
inline std::string specEscape(const BackendSpec& s, const std::string& mapName, const std::string& text) {
    auto mit = s.escapes.find(mapName);
    if (mit == s.escapes.end()) return text;
    const auto& m = mit->second;
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        if (i + 1 < text.size()) {
            auto it2 = m.find(text.substr(i, 2));
            if (it2 != m.end()) { out += it2->second; i += 2; continue; }
        }
        auto it1 = m.find(std::string(1, text[i]));
        if (it1 != m.end()) { out += it1->second; ++i; continue; }
        out += text[i++];
    }
    return out;
}

// An emitted name with the spec's forbidden-character replacement applied (identity when none declared).
inline std::string specMangle(const BackendSpec& s, const std::string& n) {
    if (s.mangleFrom.empty() || s.mangleTo.empty()) return n;
    std::string out = n;
    for (char& c : out)
        if (c == s.mangleFrom[0]) c = s.mangleTo[0];
    return out;
}

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

// A conditional expression `(c ? t : e)` — the C#/TS spelling (Python's `t if c else e` is its own hook).
inline std::string renderCond(const std::string& c, const std::string& t, const std::string& e) {
    return "(" + c + " ? " + t + " : " + e + ")";
}

// A double-quoted string literal with the target-independent escape set. The escape rules (\\ \" \n \t \r)
// and the `"` delimiter are byte-identical across C#, TS, and Python — the three-backend spike confirmed the
// per-target `escape()` copies were the same primitive — so string escaping is a shared engine rule, not
// per-target Spec data.
inline std::string renderString(const std::string& v) {
    std::string s = "\"";
    for (char c : v) {
        switch (c) {
            case '\\': s += "\\\\"; break;
            case '"':  s += "\\\""; break;
            case '\n': s += "\\n";  break;
            case '\t': s += "\\t";  break;
            case '\r': s += "\\r";  break;
            default:   s += c;      break;
        }
    }
    return s + "\"";
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
