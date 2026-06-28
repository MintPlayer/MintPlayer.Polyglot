#pragma once

#include <string>

#include "mintplayer/polyglot/ast.hpp"

// Hand-written pretty-printers, one per target (PRD §4.3 — no Roslyn / no ts-morph). Each walks the
// type-annotated AST/IR and produces idiomatic source for its target. Internal to the Core library;
// callers use compile() in polyglot.hpp.

namespace mintplayer::polyglot {

std::string emitCSharp(const CompilationUnit& unit);
std::string emitTypeScript(const CompilationUnit& unit);

} // namespace mintplayer::polyglot
