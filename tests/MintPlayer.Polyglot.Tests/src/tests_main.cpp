#include <iostream>
#include <string>

#include "mintplayer/polyglot/polyglot.hpp"

// A deliberately tiny, zero-dependency assert harness. Real frameworks (and the differential
// conformance suite) arrive with the pipeline (see docs/prd/PLAN.md P5/P8).

using namespace mintplayer::polyglot;

namespace {

int g_failures = 0;

void check(bool condition, const std::string& name) {
    if (condition) {
        std::cout << "[PASS] " << name << "\n";
    } else {
        std::cout << "[FAIL] " << name << "\n";
        ++g_failures;
    }
}

} // namespace

int main() {
    check(!Compiler::version().empty(), "version is non-empty");
    check(Compiler::version() == std::string(kVersion), "version matches the kVersion constant");

    if (g_failures == 0) {
        std::cout << "\nAll tests passed.\n";
        return 0;
    }
    std::cout << "\n" << g_failures << " test(s) failed.\n";
    return 1;
}
