#include "mintplayer/polyglot/polyglot.hpp"

#include "mintplayer/polyglot/ast.hpp"
#include "mintplayer/polyglot/emit.hpp"
#include "mintplayer/polyglot/lexer.hpp"
#include "mintplayer/polyglot/parser.hpp"
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

    result.code = (target == Target::CSharp) ? emitCSharp(unit) : emitTypeScript(unit);
    result.ok = true;
    return result;
}

} // namespace mintplayer::polyglot
