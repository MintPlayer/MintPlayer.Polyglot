#pragma once

#include "mintplayer/polyglot/ast.hpp"
#include "mintplayer/polyglot/diagnostics.hpp"

// P40 review (A9/B11) — statement-level flow checks over the type-checked AST.
//
// One "can this complete normally?" walk (the C#/Java reachability rules: `return`/`throw`/`break`/`continue`
// never complete, an `if` completes when either branch does, a `while true` only through a `break`, a `try`
// when its body or any catch does and its `finally` does) drives three refusals, each a program every target
// would otherwise treat differently:
//   - unreachable code: a statement after one that never completes (C# CS0162; dead on every target);
//   - a missing return: a value-returning body whose end is reachable (C# CS0161, TS TS2366, while
//     Python/PHP silently return None/null);
//   - definite assignment of class fields: a field without an initializer must be assigned on EVERY path
//     through each `init` that completes (or returns) — not merely somewhere in it (C# CS8618; the field
//     reads 0/undefined/AttributeError/null per target).
// Plus one expression-level refusal: comparing a side-effect-free path with itself (`x == x`, `a >= a`,
// `E.A == E.A`) is constant except for NaN — `Math.isNaN` is the NaN test (C# CS1718).
//
// Language-agnostic by construction: it reads only the AST and never a target.

namespace mintplayer::polyglot {

void checkFlow(const CompilationUnit& unit, DiagnosticBag& diags);

} // namespace mintplayer::polyglot
