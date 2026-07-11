#include "mintplayer/polyglot/polyglot.hpp"

// The toolchain's semantic version is injected at build time via -DPOLYGLOT_VERSION=<x.y.z> — an UNQUOTED
// token, so the exact same spelling works for MSVC (/p:PolyglotVersion → the .vcxproj define), cl.exe, and
// CMake (add/target_compile_definitions), with no quote/backslash escaping fork between the build systems.
// A bare local/IDE build with no define falls back to a dev sentinel, so `--version` self-describes honestly
// as an untagged build instead of asserting a stale number. THIS is the single definition point (PRD §4.16
// D4): only this translation unit needs the define; the header carries no version constant.
#ifndef POLYGLOT_VERSION
#define POLYGLOT_VERSION 0.0.0-dev
#endif
#define POLYGLOT_STRINGIFY_(x) #x
#define POLYGLOT_STRINGIFY(x) POLYGLOT_STRINGIFY_(x)

namespace mintplayer::polyglot {

std::string Compiler::version() {
    return POLYGLOT_STRINGIFY(POLYGLOT_VERSION);
}

} // namespace mintplayer::polyglot
