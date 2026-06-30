#pragma once

#include <string>
#include <unordered_map>

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

    // ---- naming rules (the catalog's "naming/identity" bucket) ----
    std::string printFn;        // the `print` intrinsic target ("global::System.Console.WriteLine" / "console.log")
    // (Math used to live here as a namespace+rename table; it's now the std.math `extern class`, bound per
    // target via templates — see compiler.cpp STD_MATH — so no Math data lives in the backend spec.)
};

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
