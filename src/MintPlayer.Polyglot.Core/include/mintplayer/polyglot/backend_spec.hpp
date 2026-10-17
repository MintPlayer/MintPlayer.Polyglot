#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mintplayer/polyglot/unicode.hpp"

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

// P38/issue #69 — how a target records the origin of each emitted line, as DATA (top-level plugin manifest
// key `originMapping`; it lives in this header, rather than backend.hpp, because the emit engine needs it
// and this header is the dependency-free home of per-target emission data).
//
// Two sinks exist because the two ecosystems disagree about where origin info lives: C# writes `#line`
// pragmas INTO the source (Roslyn records them as PDB sequence points, so .NET coverage tools report
// against the `.pg` with no post-processing at all), while TypeScript writes a Source Map v3 BESIDE it.
//
// `Style` is a closed, load-validated vocabulary — an unknown value is a plugin LOAD ERROR, never a silent
// default (the `blockStyle` precedent directly above). That is what keeps version skew loud: a manifest
// asking for a style an older CLI does not implement refuses instead of quietly emitting nothing.
//
// A target that declares nothing gets `Style::None` and emits byte-for-byte as it always has.
struct OriginMapping {
    enum class Style {
        None,        // no origin info (the default; Python and PHP today)
        Directive,   // an in-source pragma per emitted line (C# `#line`)
        SourceMapV3, // a sidecar .map file beside the emitted source (TypeScript)
    };
    Style style = Style::None;
    // Directive: the per-line templates. `$n` = the 1-based source line, `$f` = the source path.
    std::string line;             // e.g. `#line $n "$f"`
    std::string hidden;           // e.g. `#line hidden` — emitted for a line with no known origin
    // Directive: the column the pragma is written at. C# `#line` is conventionally at column 0, which is
    // NOT the emitter's current indent — so this is data, not a hardcoded choice.
    int column = 0;
    // SourceMapV3: the suffix APPENDED to the emitted file's name to name its sidecar — ".map" turns
    // `solver.ts` into `solver.ts.map`, which is the convention TS tooling looks for — and the footer that
    // points the emitted file at it (`$f` = the sidecar's file name).
    std::string sidecarExtension;
    std::string footer;

    bool emitsDirectives() const { return style == Style::Directive && !line.empty(); }
    bool emitsSourceMap() const { return style == Style::SourceMapV3; }
    // Does this target want per-line origins recorded at all? Both sinks are fed by the same hook in
    // EmitterBase::line(); only where the result GOES differs.
    bool recordsOrigins() const { return style != Style::None; }
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
    // Instance member-access operator (`.` for C#/TS/Python; `->` for PHP). Used by the shared statement
    // walk where it must synthesize a member access itself (the `use`-disposal `x.dispose()` call); ordinary
    // member/method expressions carry their own operator in the plugin's Member/MethodCall rules.
    std::string memberOp = ".";

    // Boolean / null literal spellings (C#/TS `true`/`false`/`null`; Python `True`/`False`/`None`). Read by
    // each backend's expression walk. (`this`/`self` is NOT here — C# rebinds it in a `with`-copy IIFE, so it
    // carries behavior, not just a spelling.)
    std::string trueLit = "true";
    std::string falseLit = "false";
    std::string nullLit = "null";
    // P37 D Tier 1: how this target spells the compile-time constants lowering bakes into pre-rendered
    // attribute text — the enum-member accessor (`Color.Red` vs PHP `Color::Red`) and the array-literal
    // delimiters (`[…]` vs C# `new[] {…}`). Data, like every other spelling here: the Core never
    // compares target names (it cannot know what languages exist).
    std::string enumMemberOp = ".";
    std::string constArrayOpen = "[";
    std::string constArrayClose = "]";
    // Target trait flags the Core's lowering/linking consult — DATA, never target-name comparisons
    // (the Core cannot know what languages exist):
    bool linksWithoutImports = false;  // modules link by compilation into one unit (no import statements)
    bool forbidsShadowedLocals = false; // a nested-scope local may not reuse an outer local's name (CS0136)
    bool expressionOnlyLambdas = false; // lambdas are expression-only; block lambdas hoist to local fns

    // (`print` and `Math` used to live here as naming rules; they are now real std modules — std.io's
    // generic `print<T>` and the std.math `extern class` — bound per target via templates, so no naming
    // data lives in the backend spec anymore. It carries only type/literal/template tables now.)

    // Prelude fragments prepended (each at most once) when their key is required during the walk — by a
    // `{"fn":"require"}` read in a rule (`idiv`) or by the fixed entry fact (`asyncEntry`, recorded when
    // the program's entry fn is async). Prepended in ascending key order, innermost-first, so with both
    // recorded the output reads `idiv` text, then `asyncEntry` text, then the program.
    std::unordered_map<std::string, std::string> preludes;

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

    // P40 A1: how the target spells a literal, per named context (`string`, `interp`, `char`). The Core decides
    // only WHICH characters need an escape (`unicode::mustEscape`, fixed and language-agnostic, plus the
    // context's `map` keys); this data decides how each is written:
    //   quote     — the context's delimiter (the `quoted` builtin wraps with it; the loader checks the map
    //               escapes it and the backslash);
    //   map       — source sequence -> replacement, longest match first (`"` -> `\"`, PHP `$` -> `\$`, C# interp
    //               `{` -> `{{`, TS interp `${` -> `\${`); wins over the code-point templates;
    //   codepoint — a BMP code point the Core must escape: `$H4` (4 hex digits), `$H8` (8) or `$H` (minimal);
    //   astral    — a supplementary code point: a template (`$H8`/`$H`) or "surrogates" (two `codepoint`
    //               escapes of the UTF-16 halves; the default).
    struct LiteralContext {
        std::string quote;
        std::unordered_map<std::string, std::string> map;
        std::string codepoint;
        std::string astral; // "" = "surrogates"
    };
    std::unordered_map<std::string, LiteralContext> literals;

    // Identifier repair (the spec half of P19 §7's `identifiers` manifest block): the target's reserved
    // words and how a colliding `.pg` identifier escapes — "prefix" (C# `@name`) / "suffix" (Python
    // `name_`) / "" = emit verbatim (TS escapes nothing). `mangle` repairs emitted names whose characters
    // the target forbids (Python: the overload marker `$` -> `_`, applied at every def and call site).
    std::unordered_set<std::string> keywords;
    std::string escapeStrategy; // "prefix" | "suffix" | "" (none)
    std::string escapeWith;
    std::string mangleFrom, mangleTo; // single-char emitted-name replacement ("" = none)
    // Names the target's GENERATED code claims (scaffolding like C#'s `Program`/`Main`, synthesized temps
    // like `__handled`, the union discriminant `tag`; a trailing `*` reserves a prefix family like the
    // lowering temps `__w*`) and the runtime globals user code must not shadow (`console`, `str`). A user
    // identifier colliding with either is REFUSED with a per-target diagnostic (`checkReservedNames`,
    // P19 §7) — the silent-collision miscompiles the 2026-07-02 investigation found, made loud.
    std::vector<std::string> reservedNames;
    std::vector<std::string> globalNames;
    // Names the target treats as pre-existing FREE FUNCTIONS (PHP builtins like `explode`/`count`) that a
    // user `fn` of the same name would fatally redeclare. Unlike `reservedNames` (which refuse in EVERY
    // identifier position), these bite only at function-declaration/call position, so a suffix-mangle here
    // repairs the one real collision without touching legal params/fields/locals of the same spelling
    // (issue #55). Empty on targets where free-function names never collide (C#/TS/Python).
    std::unordered_set<std::string> fnBuiltins;
    std::string fnBuiltinSuffix; // disambiguating suffix appended to a colliding function name ("" = none)
};

// A `.pg` identifier spelled for the target: keyword collisions escape per the spec's declared strategy.
inline std::string specIdent(const BackendSpec& s, const std::string& n) {
    if (s.keywords.count(n) == 0) return n;
    if (s.escapeStrategy == "prefix") return s.escapeWith + n;
    if (s.escapeStrategy == "suffix") return n + s.escapeWith;
    return n;
}

// A free-function name spelled for the target: a collision with a target builtin (PHP `explode`, …) takes
// the disambiguating suffix; every other name passes through. Applied ONLY at function-declaration and
// free-call position — never to params/fields/locals — so it is a no-op on targets with no `fnBuiltins`.
inline std::string specIdentFn(const BackendSpec& s, const std::string& n) {
    if (s.fnBuiltins.count(n) == 0) return n;
    return n + s.fnBuiltinSuffix;
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

// Substitute `type` into `$T` holes and `x` into `$x` holes (two-hole variant of substX).
inline std::string substTX(const std::string& t, const std::string& type, const std::string& x) {
    std::string out;
    for (std::size_t i = 0; i < t.size();) {
        if (t[i] == '$' && i + 1 < t.size() && t[i + 1] == 'T') { out += type; i += 2; }
        else if (t[i] == '$' && i + 1 < t.size() && t[i + 1] == 'x') { out += x; i += 2; }
        else out += t[i++];
    }
    return out;
}

// Two-hole variant of specSubst (`$T`=type, `$x`=x). Returns "" when the table or key is absent, so the
// caller can fall back (a typed local decl has no single-string identity to return).
inline std::string specSubstTX(const BackendSpec& s, const std::string& table, const std::string& key,
                               const std::string& type, const std::string& x) {
    auto t = s.tables.find(table);
    if (t == s.tables.end()) return "";
    auto it = t->second.find(key);
    return it == t->second.end() ? std::string() : substTX(it->second, type, x);
}

// P40 A1: one code point through a context's `codepoint`/`astral` template. `$H8`/`$H4` pad to 8/4 uppercase
// hex digits, `$H` is the minimal spelling. A supplementary code point with no astral template (or
// "surrogates") is written as its two UTF-16 halves through `codepoint`.
inline std::string specCodepointEscape(const BackendSpec::LiteralContext& c, char32_t cp) {
    auto fill = [](const std::string& tmpl, char32_t v) {
        static const char* hex = "0123456789ABCDEF";
        auto digits = [&](int width) {
            std::string d;
            for (char32_t x = v; x != 0; x >>= 4) d.insert(d.begin(), hex[x & 0xF]);
            if (d.empty()) d = "0";
            if (static_cast<int>(d.size()) < width) d.insert(0, static_cast<std::size_t>(width) - d.size(), '0');
            return d;
        };
        std::string out;
        for (std::size_t i = 0; i < tmpl.size(); ++i) {
            if (tmpl[i] == '$' && i + 1 < tmpl.size() && tmpl[i + 1] == 'H') {
                const char w = i + 2 < tmpl.size() ? tmpl[i + 2] : '\0';
                if (w == '8' || w == '4') { out += digits(w - '0'); i += 2; }
                else { out += digits(0); i += 1; }
                continue;
            }
            out += tmpl[i];
        }
        return out;
    };
    if (cp > 0xFFFF) {
        if (!c.astral.empty() && c.astral != "surrogates") return fill(c.astral, cp);
        const char32_t v = cp - 0x10000;
        return fill(c.codepoint, 0xD800 + (v >> 10)) + fill(c.codepoint, 0xDC00 + (v & 0x3FF));
    }
    return fill(c.codepoint, cp);
}

// P40 A1: the BODY of a target literal (no delimiters) for the decoded `.pg` value `text` in context `ctx`.
// At each position the longest `map` key wins; otherwise a character in the fixed `unicode::mustEscape` set
// goes through the code-point templates; everything else (printable ASCII, printable non-ASCII UTF-8)
// passes through byte for byte. An undeclared context falls back to `string` (the loader refuses a rule
// naming an undeclared one, so this is only reached by hand-built test specs).
inline std::string specLiteral(const BackendSpec& s, const std::string& ctx, const std::string& text) {
    auto it = s.literals.find(ctx);
    if (it == s.literals.end()) it = s.literals.find("string");
    if (it == s.literals.end()) return text;
    const auto& c = it->second;
    std::size_t maxKey = 0;
    for (const auto& kv : c.map) maxKey = kv.first.size() > maxKey ? kv.first.size() : maxKey;
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        bool mapped = false;
        for (std::size_t len = maxKey < text.size() - i ? maxKey : text.size() - i; len > 0 && !mapped; --len) {
            auto m = c.map.find(text.substr(i, len));
            if (m != c.map.end()) { out += m->second; i += len; mapped = true; }
        }
        if (mapped) continue;
        const std::size_t at = i;
        char32_t cp = 0;
        if (!unicode::decodeUtf8(text, i, cp)) { out += text[at]; continue; } // ill-formed: the lexer refused it
        if (unicode::mustEscape(cp)) out += specCodepointEscape(c, cp);
        else out.append(text, at, i - at);
    }
    return out;
}

// The context's literal with its delimiters — the `quoted` builtin.
inline std::string specQuoted(const BackendSpec& s, const std::string& ctx, const std::string& text) {
    auto it = s.literals.find(ctx);
    if (it == s.literals.end()) it = s.literals.find("string");
    const std::string q = it == s.literals.end() ? std::string("\"") : it->second.quote;
    return q + specLiteral(s, ctx, text) + q;
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

// An argument list `(a, b, c)` — the affix is identical across C# and TS (unlike tuple brackets), so it's a
// shared engine constant rather than per-backend spec data. Callers prepend the callee/`new T`/`recv.m`.
inline std::string renderArgs(const std::vector<std::string>& children) {
    return renderDelimited({"(", ", ", ")"}, children);
}

// Binary-operator precedence (higher binds tighter), used by the engine for parenthesization. The
// C-family (C#/TS) order — a shared engine concern, not per-backend data — so it lives here, not in a
// BackendSpec. EVERY operator ir::Binary can carry must appear: an op missing from this table used to
// fall through to the TIGHTEST level, silently dropping required parens — `a + (b ?? c)` emitted as
// `a + b ?? c`, which reparses as `(a + b) ?? c` and produced the FruitCake NaN miscompile (2026-07-04).
// The default stays tightest for genuinely atom-like rendering, but no real operator may rely on it.
inline int operatorPrecedence(const std::string& op) {
    if (op == "??") return 0;
    if (op == "||") return 1;
    if (op == "&&") return 2;
    if (op == "|") return 3;
    if (op == "^") return 4;
    if (op == "&") return 5;
    if (op == "==" || op == "!=") return 6;
    if (op == "<" || op == "<=" || op == ">" || op == ">=") return 7;
    if (op == "<<" || op == ">>" || op == ">>>") return 8;
    if (op == "+" || op == "-") return 9;
    if (op == "*" || op == "/" || op == "%") return 10;
    return 11;
}

} // namespace mintplayer::polyglot
