// The unit/golden suite's driver. P41 D14: the ~6.7k-line body that used to live here is split into per-area
// translation units (tests_<area>.cpp, one run_<area>() each) so they compile in parallel and unoptimized;
// this file only loads the plugins, runs the areas in the original order, and prints the summary.

#include "test_support.hpp"

int main() {
    // Zero backends are compiled in (P19 slice 7e): load the target plugins copied next to this exe by the
    // post-build step. The suite depends on all three — fail hard, not test-by-test, if any is missing.
    {
        namespace fs = std::filesystem;
        const fs::path exe = cli::executablePath();
        for (const char* t : {"csharp", "typescript", "python", "php"}) {
            const fs::path manifest = exe.parent_path() / "plugins" / t / "polyglot-plugin.json";
            std::ifstream in(manifest, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            std::string err;
            if (!in || !loadBackend(ss.str(), err)) {
                std::cerr << "FATAL: cannot load target plugin " << manifest.string() << ": "
                          << (in ? err : "file not readable") << "\n";
                return 1;
            }
        }
    }

    // The original top-to-bottom order of main(): some areas share process state (registered backends, temp
    // directories, env overrides), so the order is part of the contract.
    run_frontend();
    run_p40_manifest();
    run_p40_nullability();
    run_p40_strings();
    run_p40_review();
    run_sema_targets();
    run_lsp_modules();
    run_engine_p37();
    run_p30_download();
    run_routing_origin();
    run_wave2_coverage();
    run_p40_outputs();

    if (g_failures == 0) {
        std::cout << "\nAll tests passed.\n";
        return 0;
    }
    std::cout << "\n" << g_failures << " test(s) failed.\n";
    return 1;
}
