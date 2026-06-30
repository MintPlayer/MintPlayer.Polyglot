#pragma once

#include <string>

#include "mintplayer/polyglot/ir.hpp"

// Hand-written pretty-printers, one per target (PRD §4.3 — no Roslyn / no ts-morph). Each walks the typed
// IR (ir::Module) — NOT the AST — and produces idiomatic source for its target. The IR is the backend
// contract; lowering (lower.hpp) bridges the AST to it. Internal to the Core library; callers use
// compile() in polyglot.hpp.

namespace mintplayer::polyglot {

std::string emitCSharp(const ir::Module& module);
std::string emitTypeScript(const ir::Module& module);
std::string emitPython(const ir::Module& module);

} // namespace mintplayer::polyglot
