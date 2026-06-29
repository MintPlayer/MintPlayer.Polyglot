#include "mintplayer/polyglot/polyglot.hpp"

#include "mintplayer/polyglot/ast.hpp"
#include "mintplayer/polyglot/emit.hpp"
#include "mintplayer/polyglot/backend.hpp"
#include "mintplayer/polyglot/capability.hpp"
#include "mintplayer/polyglot/lexer.hpp"
#include "mintplayer/polyglot/lower.hpp"
#include "mintplayer/polyglot/parser.hpp"
#include "mintplayer/polyglot/pg_printer.hpp"
#include "mintplayer/polyglot/sema.hpp"

namespace mintplayer::polyglot {

EmitResult compile(const std::string& source, Target target) {
    EmitResult result;
    DiagnosticBag diags;

    std::vector<Token> tokens = lex(source, diags);
    if (diags.hasErrors()) { result.diagnostics = diags.items(); return result; }

    CompilationUnit unit = parse(tokens, diags);
    if (diags.hasErrors()) { result.diagnostics = diags.items(); return result; }

    check(unit, diags);
    if (diags.hasErrors()) { result.diagnostics = diags.items(); return result; }

    const Backend* backend = findBackend(target == Target::CSharp ? "csharp" : "typescript");
    // §3.E: refuse any used feature this target can't emit, before lowering/emitting (never miscompile).
    checkCapabilities(unit, *backend, diags);
    if (diags.hasErrors()) { result.diagnostics = diags.items(); return result; }

    ir::Module module = lower(unit);
    result.code = backend->emit(module);
    result.ok = true;
    return result;
}

EmitResult format(const std::string& source) {
    EmitResult result;
    DiagnosticBag diags;

    std::vector<Token> tokens = lex(source, diags);
    if (diags.hasErrors()) { result.diagnostics = diags.items(); return result; }

    CompilationUnit unit = parse(tokens, diags);
    if (diags.hasErrors()) { result.diagnostics = diags.items(); return result; }

    result.code = printSource(unit);
    result.ok = true;
    return result;
}

} // namespace mintplayer::polyglot
