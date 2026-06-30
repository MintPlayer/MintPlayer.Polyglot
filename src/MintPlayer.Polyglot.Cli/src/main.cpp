#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "mintplayer/polyglot/polyglot.hpp"

using namespace mintplayer::polyglot;

namespace fs = std::filesystem;

namespace {

void printUsage() {
    std::cout
        << "polyglot " << Compiler::version() << " - cross-SDK transpiler (P2 walking skeleton)\n"
        << "\n"
        << "Usage:\n"
        << "  polyglot --version\n"
        << "  polyglot build <input.pg> [--target <csharp|typescript|python>] [--out <dir>] [--root <dir>] [--lib <a,b>]\n"
        << "  polyglot fmt <input.pg>\n"
        << "\n"
        << "  build  Transpiles <input.pg>. With no --target, emits BOTH <name>.cs and <name>.ts.\n"
        << "         --out writes outputs to <dir> (default: alongside the input).\n"
        << "  fmt    Re-prints <input.pg> as canonical Polyglot to stdout (the round-trip printer).\n";
}

// Read an entire file into a string. Returns false if it could not be opened.
bool readFile(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool writeFile(const fs::path& path, const std::string& content) {
    std::ofstream os(path, std::ios::binary);
    if (!os) return false;
    os << content;
    return true;
}

// Resolves a cross-`.pg` import to a file. A bare specifier ("a.b.c") is a logical module name resolved
// under the workspace root (a.b.c -> <root>/a/b/c.pg); a "./x"/"../x" specifier is resolved relative to
// the importing file. (std.* never reaches here — the Core serves it from its embedded registry first.)
class FileModuleResolver : public ModuleResolver {
public:
    FileModuleResolver(fs::path root, fs::path entryDir)
        : root_(std::move(root)), entryDir_(std::move(entryDir)) {}

    std::optional<ResolvedModule> resolve(const std::string& spec, const std::string& importer) override {
        fs::path file;
        if (spec.rfind("./", 0) == 0 || spec.rfind("../", 0) == 0) {
            fs::path base = importer.empty() ? entryDir_ : fs::path(importer).parent_path();
            file = base / (spec + ".pg");
        } else {
            std::string rel = spec;
            for (char& c : rel) if (c == '.') c = '/';
            file = root_ / (rel + ".pg");
        }
        std::string src;
        if (!readFile(file, src)) return std::nullopt;
        return ResolvedModule{fs::weakly_canonical(file).string(), std::move(src)};
    }

private:
    fs::path root_;
    fs::path entryDir_;
};

void reportDiagnostics(const fs::path& input, const EmitResult& result) {
    for (const auto& d : result.diagnostics) {
        std::cerr << input.string() << ":" << d.pos.line << ":" << d.pos.col
                  << ": error: " << d.message << "\n";
    }
}

// Emit one target next to (or under --out of) the input, returning the written path or "" on failure.
bool emitOne(const std::string& source, const fs::path& input, const fs::path& outDir,
             Target target, const char* ext, ModuleResolver* resolver, const LibConfig& lib) {
    EmitResult result = compile(source, target, resolver, lib);
    if (!result.ok) {
        reportDiagnostics(input, result);
        return false;
    }
    fs::path out = outDir / input.stem();
    out += ext;
    if (!writeFile(out, result.code)) {
        std::cerr << "polyglot: cannot write '" << out.string() << "'\n";
        return false;
    }
    std::cout << "  -> " << out.string() << "\n";
    return true;
}

int runBuild(const std::vector<std::string>& args) {
    fs::path input;
    fs::path outDir;
    fs::path root;      // workspace root for logical-name imports; empty => input's parent dir
    std::string target; // empty => both
    std::string libArg; // comma-separated `lib` prelude entries (e.g. "io,math")

    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--target" && i + 1 < args.size()) {
            target = args[++i];
        } else if (a == "--out" && i + 1 < args.size()) {
            outDir = args[++i];
        } else if (a == "--root" && i + 1 < args.size()) {
            root = args[++i];
        } else if (a == "--lib" && i + 1 < args.size()) {
            libArg = args[++i];
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "polyglot: unknown option '" << a << "'\n";
            return 64;
        } else if (input.empty()) {
            input = a;
        } else {
            std::cerr << "polyglot: unexpected argument '" << a << "'\n";
            return 64;
        }
    }

    if (input.empty()) {
        std::cerr << "polyglot: 'build' needs an input file\n";
        return 64;
    }
    if (outDir.empty()) outDir = input.has_parent_path() ? input.parent_path() : fs::path(".");

    std::string source;
    if (!readFile(input, source)) {
        std::cerr << "polyglot: cannot open '" << input.string() << "'\n";
        return 66; // EX_NOINPUT
    }

    std::cout << "polyglot build " << input.string() << "\n";

    fs::path entryDir = input.has_parent_path() ? input.parent_path() : fs::path(".");
    if (root.empty()) root = entryDir;
    FileModuleResolver resolver(root, entryDir);

    LibConfig lib; // split `--lib io,math` on commas (trimming blanks)
    for (std::size_t b = 0, e; b <= libArg.size(); b = e + 1) {
        e = libArg.find(',', b);
        if (e == std::string::npos) e = libArg.size();
        std::string name = libArg.substr(b, e - b);
        if (!name.empty()) lib.libs.push_back(name);
    }

    bool ok = true;
    if (target.empty() || target == "csharp") ok &= emitOne(source, input, outDir, Target::CSharp, ".cs", &resolver, lib);
    if (target.empty() || target == "typescript") ok &= emitOne(source, input, outDir, Target::TypeScript, ".ts", &resolver, lib);
    // Python is opt-in (explicit --target python): it currently emits only the walking-skeleton subset, so it
    // must not join the default cs+ts emission, which would fail on any program beyond that subset.
    if (target == "python") ok &= emitOne(source, input, outDir, Target::Python, ".py", &resolver, lib);
    if (!target.empty() && target != "csharp" && target != "typescript" && target != "python") {
        std::cerr << "polyglot: unknown target '" << target << "' (expected csharp|typescript|python)\n";
        return 64;
    }
    return ok ? 0 : 1;
}

int runFmt(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::cerr << "polyglot: 'fmt' needs an input file\n";
        return 64;
    }
    fs::path input = args[1];
    std::string source;
    if (!readFile(input, source)) {
        std::cerr << "polyglot: cannot open '" << input.string() << "'\n";
        return 66;
    }
    EmitResult result = format(source);
    if (!result.ok) {
        reportDiagnostics(input, result);
        return 1;
    }
    std::cout << result.code;
    return 0;
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
        return runBuild(args);
    }
    if (args[0] == "fmt") {
        return runFmt(args);
    }

    std::cerr << "polyglot: unknown command '" << args[0] << "'\n\n";
    printUsage();
    return 64; // EX_USAGE
}
