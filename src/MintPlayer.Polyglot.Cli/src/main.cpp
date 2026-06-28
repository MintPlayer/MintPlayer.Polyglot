#include <iostream>
#include <string>
#include <vector>

#include "mintplayer/polyglot/polyglot.hpp"

using namespace mintplayer::polyglot;

namespace {

void printUsage() {
    std::cout
        << "polyglot " << Compiler::version() << " - cross-SDK transpiler (skeleton)\n"
        << "\n"
        << "Usage:\n"
        << "  polyglot --version\n"
        << "  polyglot build <input.pg> --target <csharp|typescript> [--out <dir>]\n"
        << "\n"
        << "Status: v0 skeleton. The compiler pipeline is not implemented yet;\n"
        << "see docs/prd/POLYGLOT_PRD.md for the roadmap.\n";
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);

    if (args.empty() || args[0] == "-h" || args[0] == "--help") {
        printUsage();
        return 0;
    }
    if (args[0] == "--version" || args[0] == "-v") {
        std::cout << Compiler::version() << "\n";
        return 0;
    }
    if (args[0] == "build") {
        std::cerr << "polyglot: 'build' is not implemented yet (v0 skeleton). See docs/prd/PLAN.md (P2+).\n";
        return 2;
    }

    std::cerr << "polyglot: unknown command '" << args[0] << "'\n\n";
    printUsage();
    return 64; // EX_USAGE
}
