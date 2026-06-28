#pragma once

#include <string>
#include <vector>

#include "mintplayer/polyglot/diagnostics.hpp"
#include "mintplayer/polyglot/token.hpp"

namespace mintplayer::polyglot {

// Tokenize source into the MVP token set. Whitespace and comments are skipped (trivia); P3 makes the
// lexer trivia-bearing so the backends can reproduce author spacing/comments. Always ends with an
// End token. Lexical errors (e.g. an unterminated string) are reported into `diags`.
std::vector<Token> lex(const std::string& source, DiagnosticBag& diags);

} // namespace mintplayer::polyglot
