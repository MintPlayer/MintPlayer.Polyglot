#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#undef Yield // winbase.h macro; ir::StmtKind::Yield must survive

#include <fcntl.h>
#include <io.h>
#endif

#include "mintplayer/polyglot/backend.hpp"
#include "mintplayer/polyglot/json.hpp"
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
        << "  polyglot build <input.pg> [--target <name>] [--out <dir>] [--root <dir>] [--lib <a,b>]\n"
        << "  polyglot fmt <input.pg>\n"
        << "  polyglot check <input.pg> [--json] [--root <dir>] [--lib <a,b>]\n"
        << "  polyglot lsp\n"
        << "  polyglot install <plugin-dir | npm-package>\n"
        << "\n"
        << "  build  Transpiles <input.pg>. With no --target, emits BOTH <name>.cs and <name>.ts.\n"
        << "         --out writes outputs to <dir> (default: alongside the input).\n"
        << "  fmt    Re-prints <input.pg> as canonical Polyglot to stdout (the round-trip printer).\n"
        << "  check  Reports parse/type diagnostics without emitting. --json prints a machine-readable\n"
        << "         array (line/col/severity/message) for editor tooling.\n"
        << "  lsp    Runs the Language Server over stdio (JSON-RPC): diagnostics, go-to-definition,\n"
        << "         document symbols, hover. Spawned by the editor extensions; not for interactive use.\n";
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

// Split a comma-separated lib list ("io,math") into a LibConfig, trimming blank entries.
LibConfig parseLibList(const std::string& libArg) {
    LibConfig lib;
    for (std::size_t b = 0, e; b <= libArg.size(); b = e + 1) {
        e = libArg.find(',', b);
        if (e == std::string::npos) e = libArg.size();
        std::string name = libArg.substr(b, e - b);
        if (!name.empty()) lib.libs.push_back(name);
    }
    return lib;
}

// The minimal project manifest (PRD §4.8): `{ "root": <dir>, "lib": ["io","math"] }`. `root` is resolved
// against the pgconfig.json that declares it; `lib` is the ambient prelude. Found by walking up from a start
// directory to the first pgconfig.json. Parsed with the Core JSON reader — Core stays IO-free; this is
// CLI/LSP glue that produces the same `root`/`lib` the compiler already accepts. A precursor of P10's file.
struct PgConfig {
    bool found = false;
    std::string root; // absolute, resolved against the config's directory
    std::string lib;  // comma-joined lib names
    std::vector<std::string> targets; // the project's target set (drives the default build; P19 slice 10)
    std::vector<std::pair<std::string, std::string>> dependencies; // target -> source spec ("file:<dir>")
    fs::path dir; // where the config was found (file: deps resolve against it)
};
PgConfig loadPgConfig(const fs::path& startDir) {
    for (fs::path d = startDir;; d = d.parent_path()) {
        std::string src;
        if (readFile(d / "pgconfig.json", src)) {
            json::Value v = json::parse(src);
            PgConfig pc;
            pc.found = true;
            pc.dir = d;
            std::string r = v["root"].asString();
            pc.root = (r.empty() ? d : (d / r)).lexically_normal().string();
            for (const auto& e : v["lib"].items()) { if (!pc.lib.empty()) pc.lib += ","; pc.lib += e.asString(); }
            for (const auto& e : v["targets"].items())
                if (e.kind == json::Value::Kind::String) pc.targets.push_back(e.asString());
            for (const auto& kv : v["dependencies"].members)
                if (kv.second.kind == json::Value::Kind::String) pc.dependencies.push_back({kv.first, kv.second.asString()});
            return pc;
        }
        if (!d.has_parent_path() || d.parent_path() == d) return {};
    }
}

// The user-level plugin cache `polyglot install` populates (%LOCALAPPDATA%\polyglot\plugins\<name>\).
fs::path pluginCacheDir() {
#ifdef _WIN32
    char buf[4096];
    const unsigned long n = GetEnvironmentVariableA("LOCALAPPDATA", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) return fs::path(std::string(buf, n)) / "polyglot" / "plugins";
#endif
    return fs::temp_directory_path() / "polyglot" / "plugins";
}

// Load one plugin manifest file into the registry; reports (but does not throw on) failures.
bool loadPluginFile(const fs::path& manifest) {
    std::string src;
    if (!readFile(manifest, src)) return false;
    std::string err;
    if (!loadBackend(src, err)) {
        std::cerr << "polyglot: " << manifest.string() << ": " << err << "\n";
        return false;
    }
    return true;
}

// Resolve a target that is not yet registered (P19 slices 10-11): pgconfig `dependencies` `file:` paths
// (relative to the config's directory), then the user cache. The in-box `plugins/` dir next to the exe was
// loaded at startup; an unresolved name ends up at findTarget's error, which names the channels.
void resolveConfiguredTargets(const PgConfig& pc) {
    for (const auto& [name, spec] : pc.dependencies) {
        if (findTarget(name).ok()) continue;
        if (spec.rfind("file:", 0) == 0)
            loadPluginFile((pc.dir / spec.substr(5) / "polyglot-plugin.json").lexically_normal());
        else
            std::cerr << "polyglot: dependency '" << name << "': unsupported spec '" << spec
                      << "' (only file:<dir> resolves in-place; use `polyglot install " << name << "`)\n";
    }
    for (const auto& t : pc.targets)
        if (!findTarget(t).ok()) loadPluginFile(pluginCacheDir() / t / "polyglot-plugin.json");
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
             const BackendHandle& target, const char* ext, ModuleResolver* resolver, const LibConfig& lib) {
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
    // A pgconfig.json near the input fills in root/lib the user didn't pass explicitly (explicit flags win).
    PgConfig pc = loadPgConfig(entryDir);
    if (root.empty() && pc.found && !pc.root.empty()) root = pc.root;
    if (libArg.empty() && pc.found) libArg = pc.lib;
    if (root.empty()) root = entryDir;
    FileModuleResolver resolver(root, entryDir);

    LibConfig lib = parseLibList(libArg);

    resolveConfiguredTargets(pc); // pgconfig `dependencies` (file:) + the install cache (P19 slices 10-11)

    bool ok = true;
    if (target.empty() && !pc.targets.empty()) { // the project declares its target set — build all of it
        for (const auto& t : pc.targets) {
            BackendHandle h = findTarget(t);
            if (!h.ok()) {
                std::cerr << "polyglot: " << h.error() << "\n";
                ok = false;
                continue;
            }
            ok &= emitOne(source, input, outDir, h, h.backend()->fileExtension().c_str(), &resolver, lib);
        }
    } else if (target.empty()) { // no config: the historical default pair
        ok &= emitOne(source, input, outDir, findTarget("csharp"), ".cs", &resolver, lib);
        ok &= emitOne(source, input, outDir, findTarget("typescript"), ".ts", &resolver, lib);
    } else { // ANY loaded plugin is a valid --target; its manifest names the output extension (P19)
        if (!findTarget(target).ok()) loadPluginFile(pluginCacheDir() / target / "polyglot-plugin.json");
        BackendHandle h = findTarget(target);
        if (!h.ok()) {
            std::cerr << "polyglot: " << h.error() << "\n";
            return 64;
        }
        ok &= emitOne(source, input, outDir, h, h.backend()->fileExtension().c_str(), &resolver, lib);
    }
    return ok ? 0 : 1;
}

// Escape a string for embedding in a JSON double-quoted value.
std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[(c >> 4) & 0xF];
                    out += hex[c & 0xF];
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Serialize diagnostics as a JSON array of {line,col,endLine,endCol,severity,message}. Shared by
// `check --json` and the LSP `polyglot/emit` preview response (whole-file list, no identifier-widening —
// that widening is a squiggle-only concern of publishDiagnostics).
std::string diagnosticsToJson(const std::vector<Diagnostic>& diags) {
    auto severityName = [](Severity s) {
        switch (s) {
            case Severity::Warning: return "warning";
            case Severity::Info:    return "info";
            case Severity::Hint:    return "hint";
            default:                return "error";
        }
    };
    std::string out = "[";
    for (std::size_t i = 0; i < diags.size(); ++i) {
        const auto& d = diags[i];
        if (i) out += ",";
        out += "{\"line\":" + std::to_string(d.pos.line) + ",\"col\":" + std::to_string(d.pos.col) +
               ",\"endLine\":" + std::to_string(d.end.line) + ",\"endCol\":" + std::to_string(d.end.col) +
               ",\"severity\":\"" + severityName(d.severity) + "\",\"message\":\"" + jsonEscape(d.message) + "\"}";
    }
    out += "]";
    return out;
}

// `polyglot check <input.pg> [--json] [--root <dir>] [--lib <a,b>]` — runs the frontend (lex/parse/sema +
// capability gating for the reference target) and reports diagnostics, without emitting any output. This is
// the editor-tooling entry point: `--json` prints a machine-readable array editors turn into squiggles.
// (The reference target is C#, whose capability set is the full §3.A surface, so nothing gates spuriously.)
int runCheck(const std::vector<std::string>& args) {
    fs::path input;
    fs::path root;
    std::string libArg;
    bool json = false;

    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--json") json = true;
        else if (a == "--root" && i + 1 < args.size()) root = args[++i];
        else if (a == "--lib" && i + 1 < args.size()) libArg = args[++i];
        else if (!a.empty() && a[0] == '-') { std::cerr << "polyglot: unknown option '" << a << "'\n"; return 64; }
        else if (input.empty()) input = a;
        else { std::cerr << "polyglot: unexpected argument '" << a << "'\n"; return 64; }
    }
    if (input.empty()) { std::cerr << "polyglot: 'check' needs an input file\n"; return 64; }

    std::string source;
    if (!readFile(input, source)) {
        std::cerr << "polyglot: cannot open '" << input.string() << "'\n";
        return 66;
    }

    fs::path entryDir = input.has_parent_path() ? input.parent_path() : fs::path(".");
    PgConfig pc = loadPgConfig(entryDir); // fills root/lib the user didn't pass explicitly
    if (root.empty() && pc.found && !pc.root.empty()) root = pc.root;
    if (libArg.empty() && pc.found) libArg = pc.lib;
    if (root.empty()) root = entryDir;
    FileModuleResolver resolver(root, entryDir);

    LibConfig lib = parseLibList(libArg);

    EmitResult result = compile(source, findTarget("csharp"), &resolver, lib);

    if (json) {
        std::cout << diagnosticsToJson(result.diagnostics) << "\n";
    } else {
        reportDiagnostics(input, result);
        if (result.ok) std::cout << "polyglot: no problems in " << input.string() << "\n";
    }
    return result.ok ? 0 : 1;
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

// ============================ LSP server (`polyglot lsp`) — PRD §4.8 ============================
// A zero-dependency Language Server over the frontend `analyze()` facade. Transport is Content-Length-framed
// JSON-RPC 2.0 on stdio; the JSON reader lives in Core. We negotiate the `utf-8` position encoding so LSP
// columns are byte offsets that map to our 1-based line/col with just a ±1 shift (no UTF-16 walk yet).

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// `file:///c%3A/x/y.pg` -> a native path. Percent-decodes and drops the leading slash before a Windows drive.
std::string uriToPath(const std::string& uri) {
    std::string u = uri;
    if (u.rfind("file://", 0) == 0) u = u.substr(7);
    std::string out;
    for (std::size_t k = 0; k < u.size(); ++k) {
        if (u[k] == '%' && k + 2 < u.size()) {
            int hi = hexVal(u[k + 1]), lo = hexVal(u[k + 2]);
            if (hi >= 0 && lo >= 0) { out += static_cast<char>(hi * 16 + lo); k += 2; continue; }
        }
        out += u[k];
    }
#ifdef _WIN32
    if (out.size() >= 3 && out[0] == '/' && out[2] == ':') out.erase(0, 1);
#endif
    return out;
}

// A native path -> a file:// URI (inverse of uriToPath), for cross-module go-to-definition Locations.
std::string pathToUri(const std::string& p) {
    std::string s = p;
    for (char& c : s) if (c == '\\') c = '/';
    if (s.size() >= 2 && s[1] == ':') return "file:///" + s; // Windows drive: file:///c:/...
    if (!s.empty() && s[0] == '/') return "file://" + s;     // POSIX absolute
    return "file:///" + s;
}

std::size_t byteOffset(const std::string& text, int line, int col) {
    std::size_t off = 0;
    for (int ln = 1; ln < line && off < text.size(); ++off) if (text[off] == '\n') ++ln;
    off += static_cast<std::size_t>(col - 1);
    return off < text.size() ? off : text.size();
}
bool identPart(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// UTF-8 lead-byte -> its sequence length (1..4); a stray continuation/invalid byte counts as 1.
int utf8Seq(unsigned char c) {
    return c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
}
// UTF-16 code units spanned by the first `nbytes` bytes of UTF-8 string `s` (a 4-byte/astral char = 2 units).
int utf16Units(const std::string& s, std::size_t nbytes) {
    int u = 0;
    for (std::size_t i = 0; i < nbytes && i < s.size();) {
        int seq = utf8Seq(static_cast<unsigned char>(s[i]));
        u += seq == 4 ? 2 : 1;
        i += seq;
    }
    return u;
}
// The line's text (1-based line, newline excluded) — for per-line UTF-16<->byte column conversion.
std::string lineOf(const std::string& text, int line) {
    std::size_t start = byteOffset(text, line, 1), end = start;
    while (end < text.size() && text[end] != '\n') ++end;
    return text.substr(start, end - start);
}
// 1-based byte column -> 1-based UTF-16 column on `lineText`.
int protoColFromByte(const std::string& lineText, int byteCol) { return 1 + utf16Units(lineText, static_cast<std::size_t>(byteCol - 1)); }
// 1-based UTF-16 column -> 1-based byte column on `lineText`.
int byteColFromUtf16(const std::string& lineText, int protoCol) {
    int target = protoCol - 1, u = 0;
    std::size_t i = 0;
    while (i < lineText.size() && u < target) { int seq = utf8Seq(static_cast<unsigned char>(lineText[i])); u += seq == 4 ? 2 : 1; i += seq; }
    return static_cast<int>(i) + 1;
}

// Map our SymbolKind -> the LSP SymbolKind enum (numeric).
int lspSymbolKind(SymbolKind k) {
    switch (k) {
        case SymbolKind::Function:  return 12; // Function
        case SymbolKind::Type:      return 5;  // Class
        case SymbolKind::Method:    return 6;  // Method
        case SymbolKind::Field:     return 8;  // Field
        case SymbolKind::Value:     return 14; // Constant
        case SymbolKind::UnionCase:
        case SymbolKind::EnumCase:  return 22; // EnumMember
        default:                    return 13; // Variable
    }
}

// The semantic-token legend index for a symbol (must match the tokenTypes array declared in `initialize`).
int semanticTokenType(SymbolKind k) {
    switch (k) {
        case SymbolKind::Function:  return 0; // function
        case SymbolKind::Type:      return 1; // type
        case SymbolKind::Method:    return 2; // method
        case SymbolKind::Field:     return 3; // property
        case SymbolKind::Value:     return 4; // variable
        case SymbolKind::Parameter: return 5; // parameter
        case SymbolKind::UnionCase:
        case SymbolKind::EnumCase:  return 6; // enumMember
        default:                    return 4; // Local -> variable
    }
}

// The LSP CompletionItemKind for a symbol offered as a bare (non-member) name. Returns 0 for kinds that are
// only reachable via member access (methods/fields) — the caller skips those.
int completionKind(SymbolKind k) {
    switch (k) {
        case SymbolKind::Function:  return 3;  // Function
        case SymbolKind::Type:      return 7;  // Class
        case SymbolKind::Value:     return 21; // Constant
        case SymbolKind::Parameter:
        case SymbolKind::Local:     return 6;  // Variable
        case SymbolKind::UnionCase:
        case SymbolKind::EnumCase:  return 20; // EnumMember
        default:                    return 0;  // Method/Field — member-access only
    }
}

// Read one Content-Length-framed message body from stdin. False on EOF.
bool lspRead(std::string& body) {
    std::size_t len = 0;
    std::string line;
    int c;
    for (;;) {
        line.clear();
        while ((c = std::getchar()) != EOF && c != '\n') if (c != '\r') line += static_cast<char>(c);
        if (c == EOF && line.empty()) return false;
        if (line.empty()) break; // blank line ends the headers
        static const char* kCL = "Content-Length:";
        if (line.rfind(kCL, 0) == 0) len = std::strtoul(line.c_str() + std::strlen(kCL), nullptr, 10);
    }
    body.assign(len, '\0');
    std::size_t got = 0;
    while (got < len) { std::size_t n = std::fread(&body[got], 1, len - got, stdin); if (!n) break; got += n; }
    body.resize(got);
    return true;
}
void lspSend(const std::string& body) {
    std::string h = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    std::fwrite(h.data(), 1, h.size(), stdout);
    std::fwrite(body.data(), 1, body.size(), stdout);
    std::fflush(stdout);
}

// A ModuleResolver that serves an OPEN editor buffer's (possibly unsaved) text for any imported module the
// editor currently has open, delegating to the on-disk resolver otherwise. So editing an imported .pg updates
// its dependents live, before the import is saved. Matching is by real path (`fs::equivalent`, robust to
// uri-encoding/drive-case), and only the source text is swapped — the `canonicalPath` (dedup/cycle identity)
// stays the disk path, so transitive loading is unaffected.
class BufferResolver : public ModuleResolver {
public:
    BufferResolver(ModuleResolver& disk, const std::map<std::string, std::string>& buffers)
        : disk_(disk), buffers_(buffers) {}
    std::optional<ResolvedModule> resolve(const std::string& spec, const std::string& importer) override {
        auto r = disk_.resolve(spec, importer);
        if (r) {
            for (const auto& kv : buffers_) {
                std::error_code ec;
                if (fs::equivalent(uriToPath(kv.first), r->canonicalPath, ec) && !ec) { r->source = kv.second; break; }
            }
        }
        return r;
    }
private:
    ModuleResolver& disk_;
    const std::map<std::string, std::string>& buffers_;
};

struct LspServer {
    std::map<std::string, std::string> text_;    // uri -> current source (Full-sync buffer)
    std::map<std::string, SemanticModel> model_; // uri -> latest analyzed model
    std::map<std::string, SourceMap> sources_;   // uri -> its fileId->origin map (for cross-module locations)
    std::string root_;
    std::string lib_ = "io,math";
    bool utf16_ = false;  // negotiated position encoding: false = utf-8 (byte columns, no conversion) / true = utf-16.

    // A range from 1-based (line,col) positions, emitted as 0-based LSP positions (byte columns).
    static std::string rangeJson(int sl, int sc, int el, int ec) {
        return "{\"start\":{\"line\":" + std::to_string(sl - 1) + ",\"character\":" + std::to_string(sc - 1) +
               "},\"end\":{\"line\":" + std::to_string(el - 1) + ",\"character\":" + std::to_string(ec - 1) + "}}";
    }

    // Position-encoding conversion (only bites when utf-16 was negotiated; utf-8 = identity, so the VS Code
    // path is byte-for-byte unchanged). Internal columns are 1-based byte offsets; the client speaks utf-16
    // code units. Conversion needs the line's text, looked up in `text_` by uri — for a cross-file range in a
    // doc we don't have open, we fall back to byte columns (correct for ASCII; the documented residual).

    // Client position -> internal 1-based byte column. `protoChar0` is the 0-based character from the request.
    int inCol(const std::string& uri, int line, int protoChar0) {
        if (!utf16_) return protoChar0 + 1;
        auto it = text_.find(uri);
        if (it == text_.end()) return protoChar0 + 1;
        return byteColFromUtf16(lineOf(it->second, line), protoChar0 + 1);
    }
    // Emit a range in `docUri`, converting byte columns to utf-16 when negotiated.
    std::string encRange(const std::string& docUri, int sl, int sc, int el, int ec) {
        if (!utf16_) return rangeJson(sl, sc, el, ec);
        auto it = text_.find(docUri);
        if (it == text_.end()) return rangeJson(sl, sc, el, ec);
        const std::string& t = it->second;
        return rangeJson(sl, protoColFromByte(lineOf(t, sl), sc), el, protoColFromByte(lineOf(t, el), ec));
    }

    // Module-resolution context for an open doc, derived from its nearest pgconfig.json (falling back to the
    // client's initializationOptions). Shared by analyzeDoc (diagnostics) and generatedSource (preview) so a
    // live preview resolves imports + lib exactly as the squiggles do. Re-read each call, so editing
    // pgconfig.json takes effect on the next analysis.
    struct DocContext { fs::path root, entryDir; std::string libStr; };
    DocContext contextFor(const std::string& uri) const {
        fs::path p(uriToPath(uri));
        fs::path entryDir = p.has_parent_path() ? p.parent_path() : fs::path(".");
        PgConfig pc = loadPgConfig(entryDir);
        fs::path root = pc.found && !pc.root.empty() ? fs::path(pc.root)
                      : (root_.empty() ? entryDir : fs::path(root_));
        return { root, entryDir, pc.found ? pc.lib : lib_ };
    }

    void analyzeDoc(const std::string& uri) {
        DocContext ctx = contextFor(uri);
        FileModuleResolver disk(ctx.root, ctx.entryDir);
        BufferResolver resolver(disk, text_); // see unsaved edits in open imported modules
        AnalysisResult a = analyze(text_[uri], &resolver, parseLibList(ctx.libStr), uriToPath(uri));
        model_[uri] = std::move(a.model);
        sources_[uri] = std::move(a.sources);
        // Only real files get squiggles. A `polyglot:<std>` virtual doc is read-only embedded std source we
        // analyze solely to power its semantic tokens / hover / go-to-def — analyzing it standalone may raise
        // link-context diagnostics that would be noise on code the user can't edit, so we don't publish them.
        if (uri.rfind("file:", 0) == 0) publishDiagnostics(uri, a.diagnostics);
    }

    // Analyze arbitrary text as if it were `uri` (same resolver/lib context), returning just the model —
    // used by member completion, which analyzes a repaired buffer (the trailing `.member` dropped so it
    // parses) to resolve the receiver's type. Does not store/publish anything.
    SemanticModel analyzeText(const std::string& uri, const std::string& text) {
        DocContext ctx = contextFor(uri);
        FileModuleResolver disk(ctx.root, ctx.entryDir);
        BufferResolver resolver(disk, text_);
        return std::move(analyze(text, &resolver, parseLibList(ctx.libStr), uriToPath(uri)).model);
    }

    // Custom request `polyglot/emit`: params { uri, target } -> { target, code, ok, diagnostics }. Runs the
    // full compile() in memory (no disk write) for a live preview of the emitted target source. One target per
    // request; the client debounces on edit and re-requests on target switch. compile() never returns partial
    // output — on failure it's { ok:false, code:"" } and the client keeps its last-good text with a stale
    // banner (never a miscompile shown as valid).
    std::string generatedSource(const json::Value& params) {
        std::string uri = params["uri"].asString();
        std::string targetName = params["target"].asString();
        auto empty = [&]() {
            return "{\"target\":" + json::quote(targetName) + ",\"code\":\"\",\"ok\":false,\"diagnostics\":[]}";
        };
        if (!text_.count(uri)) return empty();          // doc not open — never insert an empty text_[uri]
        BackendHandle tgt = findTarget(targetName);
        if (!tgt.ok()) return empty();
        DocContext ctx = contextFor(uri);
        FileModuleResolver disk(ctx.root, ctx.entryDir);
        BufferResolver resolver(disk, text_); // preview reflects unsaved edits in open imported modules
        EmitResult r = compile(text_[uri], tgt, &resolver, parseLibList(ctx.libStr));
        return "{\"target\":" + json::quote(targetName) + ",\"code\":" + json::quote(r.code) +
               ",\"ok\":" + (r.ok ? "true" : "false") +
               ",\"diagnostics\":" + diagnosticsToJson(r.diagnostics) + "}";
    }

    void publishDiagnostics(const std::string& uri, const std::vector<Diagnostic>& diags) {
        const std::string& text = text_.count(uri) ? text_[uri] : uri; // uri unused when text absent
        std::string arr;
        bool first = true;
        for (const auto& d : diags) {
            if (d.pos.fileId != 1) continue; // only this file's own diagnostics (fileId 1 = the entry); an
                                             // imported module's errors belong to (and show in) that module.
            int sl = d.pos.line, sc = d.pos.col, el = d.end.line, ec = d.end.col;
            if (el == sl && ec == sc) { // widen a point to the identifier at that spot (else a 1-char range)
                std::size_t off = byteOffset(text, sl, sc), end = off;
                while (end < text.size() && identPart(text[end])) ++end;
                ec = sc + static_cast<int>(end - off);
                if (ec == sc) ec = sc + 1;
            }
            int sev = d.severity == Severity::Warning ? 2 : d.severity == Severity::Info ? 3
                    : d.severity == Severity::Hint ? 4 : 1;
            if (!first) arr += ",";
            first = false;
            arr += "{\"range\":" + encRange(uri, sl, sc, el, ec) + ",\"severity\":" + std::to_string(sev) +
                   ",\"source\":\"polyglot\",\"message\":" + json::quote(d.message) + "}";
        }
        lspSend("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":" +
                json::quote(uri) + ",\"diagnostics\":[" + arr + "]}}");
    }

    const SemanticModel* modelFor(const json::Value& params, int& line, int& col, std::string& uri) {
        uri = params["textDocument"]["uri"].asString();
        line = static_cast<int>(params["position"]["line"].asInt()) + 1;
        col = inCol(uri, line, static_cast<int>(params["position"]["character"].asInt()));
        auto it = model_.find(uri);
        return it == model_.end() ? nullptr : &it->second;
    }

    // The URI a definition's fileId lives in: fileId 1 = the entry (the request doc); a higher id is another
    // resolved module, mapped through the SourceMap to a file:// URI. Returns "" for embedded std / unknown
    // (no on-disk file) — those aren't navigable as files (std click-through is served virtually elsewhere).
    std::string uriForFileId(const std::string& requestUri, int fid) {
        if (fid == 1) return requestUri;
        auto sit = sources_.find(requestUri);
        if (sit == sources_.end()) return "";
        const std::string& canon = sit->second.canon(fid);
        if (canon.empty()) return "";
        if (fs::path(canon).is_absolute()) return pathToUri(canon);            // a resolver-loaded .pg file
        if (canon.rfind("std.", 0) == 0) return "polyglot:" + canon;           // embedded std -> virtual document
        return "";
    }

    std::string definition(const json::Value& params) {
        int line, col; std::string uri;
        const SemanticModel* m = modelFor(params, line, col, uri);
        if (!m) return "null";
        const SymbolDef* d = m->definitionAt(line, col);
        if (!d) return "null";
        std::string targetUri = uriForFileId(uri, d->nameSpan.start.fileId);
        if (targetUri.empty()) return "null";
        int sl = d->nameSpan.start.line, sc = d->nameSpan.start.col;
        return "{\"uri\":" + json::quote(targetUri) + ",\"range\":" + encRange(targetUri, sl, sc, sl, sc + d->nameSpan.length) + "}";
    }

    std::string references(const json::Value& params) {
        int line, col; std::string uri;
        const SemanticModel* m = modelFor(params, line, col, uri);
        if (!m) return "[]";
        int d = m->symbolAt(line, col);
        if (d < 0) return "[]";
        bool incDecl = params["context"]["includeDeclaration"].asBool(true);
        std::string arr;
        bool first = true;
        auto emit = [&](const std::string& u, int sl, int sc, int len) {
            if (u.empty()) return;
            if (!first) arr += ",";
            first = false;
            arr += "{\"uri\":" + json::quote(u) + ",\"range\":" + encRange(u, sl, sc, sl, sc + len) + "}";
        };
        for (const Span& s : m->referencesTo(d)) emit(uri, s.start.line, s.start.col, s.length); // uses (this file)
        if (incDecl) { // the declaration (its own file)
            const SymbolDef& def = m->defs[static_cast<std::size_t>(d)];
            emit(uriForFileId(uri, def.nameSpan.start.fileId), def.nameSpan.start.line, def.nameSpan.start.col,
                 def.nameSpan.length);
        }
        return "[" + arr + "]";
    }

    std::string rename(const json::Value& params) {
        int line, col; std::string uri;
        const SemanticModel* m = modelFor(params, line, col, uri);
        if (!m) return "null";
        int d = m->symbolAt(line, col);
        if (d < 0) return "null";
        const SymbolDef& def = m->defs[static_cast<std::size_t>(d)];
        // Only file-local symbols: the declaration and all recorded uses are in this file, so one atomic
        // single-file edit. A symbol defined in another module would need edits across files we don't index.
        if (def.external || def.nameSpan.start.fileId != 1) return "null";
        std::string newName = params["newName"].asString();
        std::string edits;
        bool first = true;
        auto edit = [&](int sl, int sc, int len) {
            if (!first) edits += ",";
            first = false;
            edits += "{\"range\":" + encRange(uri, sl, sc, sl, sc + len) + ",\"newText\":" + json::quote(newName) + "}";
        };
        edit(def.nameSpan.start.line, def.nameSpan.start.col, def.nameSpan.length);
        for (const Span& s : m->referencesTo(d)) edit(s.start.line, s.start.col, s.length);
        return "{\"changes\":{" + json::quote(uri) + ":[" + edits + "]}}";
    }

    std::string hover(const json::Value& params) {
        int line, col; std::string uri;
        const SemanticModel* m = modelFor(params, line, col, uri);
        if (!m) return "null";
        const SymbolDef* d = m->definitionAt(line, col);
        if (!d) return "null";
        static const char* kindNames[] = {"fn", "type", "value", "param", "let", "field", "method", "case", "case"};
        std::string sig = std::string(kindNames[static_cast<int>(d->kind)]) + " " + d->name;
        if (!d->type.name.empty()) sig += ": " + d->type.name;
        std::string md = "```polyglot\n" + sig + "\n```";
        return "{\"contents\":{\"kind\":\"markdown\",\"value\":" + json::quote(md) + "}}";
    }

    // Accurate identifier coloring: emit an LSP semantic token for every definition and reference the model
    // knows, classified by symbol kind. Delta-encoded (5 ints/token, document order) with `declaration` set on
    // def sites. Layers on top of the TextMate grammar (which still colors keywords/strings/numbers/operators).
    std::string semanticTokens(const json::Value& params) {
        std::string uri = params["textDocument"]["uri"].asString();
        auto it = model_.find(uri);
        if (it == model_.end()) return "{\"data\":[]}";
        const SemanticModel& m = it->second;
        auto tt = text_.find(uri);
        const std::string* textp = tt == text_.end() ? nullptr : &tt->second; // for utf-16 column conversion
        struct Tok { int line, col, len, type, mod; };
        std::vector<Tok> toks;
        for (const auto& d : m.defs)
            if (!d.external && d.nameSpan.length > 0)
                toks.push_back({d.nameSpan.start.line, d.nameSpan.start.col, d.nameSpan.length, semanticTokenType(d.kind), 1});
        for (const auto& r : m.refs)
            if (r.def >= 0 && r.span.length > 0)
                toks.push_back({r.span.start.line, r.span.start.col, r.span.length,
                                semanticTokenType(m.defs[static_cast<std::size_t>(r.def)].kind), 0});
        // stable_sort keeps insertion order (defs before body refs) so a position emitted for two overlapping
        // symbols — e.g. a record field that's also an in-scope local in a method body — keeps the first.
        std::stable_sort(toks.begin(), toks.end(),
                         [](const Tok& a, const Tok& b) { return a.line != b.line ? a.line < b.line : a.col < b.col; });
        std::string data;
        int prevLine = 0, prevChar = 0, lastLine = -1, lastCol = -1;
        bool first = true;
        for (const auto& t : toks) {
            if (t.line == lastLine && t.col == lastCol) continue; // LSP requires non-overlapping tokens
            lastLine = t.line;
            lastCol = t.col;
            int col1 = t.col, len1 = t.len;
            if (utf16_ && textp) { // byte column/length -> utf-16 code units on this line
                std::string ln = lineOf(*textp, t.line);
                col1 = protoColFromByte(ln, t.col);
                len1 = protoColFromByte(ln, t.col + t.len) - col1;
            }
            int line0 = t.line - 1, char0 = col1 - 1;
            int dLine = line0 - prevLine;
            int dChar = dLine == 0 ? char0 - prevChar : char0;
            if (!first) data += ",";
            first = false;
            data += std::to_string(dLine) + "," + std::to_string(dChar) + "," + std::to_string(len1) + "," +
                    std::to_string(t.type) + "," + std::to_string(t.mod);
            prevLine = line0;
            prevChar = char0;
        }
        return "{\"data\":[" + data + "]}";
    }

    // Completion. In a member context (`obj.` / `obj.pre`) it lists the receiver type's members; otherwise it
    // offers keywords + every bare symbol the model knows (file-local + imported). Context-insensitive within
    // each mode — the client filters by the typed prefix.
    std::string completion(const json::Value& params) {
        std::string items;
        bool first = true;
        std::set<std::string> seen;
        auto add = [&](const std::string& label, int kind) {
            if (kind == 0 || !seen.insert(label + "\x1f" + std::to_string(kind)).second) return;
            if (!first) items += ",";
            first = false;
            items += "{\"label\":" + json::quote(label) + ",\"kind\":" + std::to_string(kind) + "}";
        };

        std::string uri = params["textDocument"]["uri"].asString();
        int line = static_cast<int>(params["position"]["line"].asInt()) + 1;
        int col = inCol(uri, line, static_cast<int>(params["position"]["character"].asInt()));

        // Member context: an identifier prefix immediately preceded by '.' — resolve the receiver's type and
        // list its members. Analyze a REPAIRED buffer (the `.member` under the cursor removed) so the receiver
        // parses even mid-edit; look the receiver up in that model; then emit defs owned by its type.
        auto tit = text_.find(uri);
        if (tit != text_.end()) {
            const std::string& text = tit->second;
            std::size_t off = byteOffset(text, line, col);
            std::size_t ps = off;
            while (ps > 0 && identPart(text[ps - 1])) --ps;           // start of the partial member
            if (ps > 0 && text[ps - 1] == '.') {                      // member access
                std::size_t dot = ps - 1, re = dot, rs = dot;
                while (rs > 0 && identPart(text[rs - 1])) --rs;       // the receiver identifier [rs, re)
                if (rs < re) {
                    std::string repaired = text.substr(0, dot) + text.substr(off);
                    SemanticModel m = analyzeText(uri, repaired);
                    std::size_t lineStart = byteOffset(text, line, 1);
                    int rcol = static_cast<int>(rs - lineStart) + 1;
                    const SymbolDef* rd = m.definitionAt(line, rcol);
                    if (rd && !rd->type.name.empty()) {
                        const std::string& ty = rd->type.name;
                        // Explicit member kinds (Field=5 / Method=2) — completionKind() deliberately maps these
                        // to 0 to keep them OUT of the bare-symbol list; here in member context we want them.
                        for (const auto& d : m.defs)
                            if (d.owner == ty) add(d.name, d.kind == SymbolKind::Field ? 5 : 2);
                    }
                }
                return "[" + items + "]"; // after a '.', never fall through to keywords/bare symbols
            }
        }

        auto it = model_.find(uri);
        if (it != model_.end())
            for (const auto& d : it->second.defs) {
                // A local/param is offered only where it's in scope (its enclosing fn/method extent). Defs with
                // no recorded scope (scopeEnd.line==0 — e.g. top-level, lambdas) are always offered, as before.
                if ((d.kind == SymbolKind::Local || d.kind == SymbolKind::Parameter) && d.scopeEnd.line != 0) {
                    bool inScope =
                        (line > d.scopeStart.line || (line == d.scopeStart.line && col >= d.scopeStart.col)) &&
                        (line < d.scopeEnd.line   || (line == d.scopeEnd.line   && col <= d.scopeEnd.col));
                    if (!inScope) continue;
                }
                add(d.name, completionKind(d.kind));
            }
        static const char* kw[] = {
            "fn", "let", "var", "const", "if", "else", "while", "do", "for", "in", "match", "when", "return",
            "break", "continue", "yield", "record", "class", "interface", "enum", "union", "extension",
            "import", "async", "await", "try", "catch", "finally", "throw", "use", "with", "operator",
            "true", "false", "null", "this", "super"};
        for (const char* k : kw) add(k, 14); // Keyword
        return "[" + items + "]";
    }

    std::string formatting(const json::Value& params) {
        std::string uri = params["textDocument"]["uri"].asString();
        auto it = text_.find(uri);
        if (it == text_.end()) return "null";
        EmitResult r = format(it->second);
        if (!r.ok) return "[]"; // a parse error — leave the buffer untouched (diagnostics show why)
        int endLine = 0, endCol = 0;
        for (char c : it->second) { if (c == '\n') { ++endLine; endCol = 0; } else ++endCol; }
        return "[{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":" + std::to_string(endLine) +
               ",\"character\":" + std::to_string(endCol) + "}},\"newText\":" + json::quote(r.code) + "}]";
    }

    std::string documentSymbol(const json::Value& params) {
        std::string uri = params["textDocument"]["uri"].asString();
        auto it = model_.find(uri);
        if (it == model_.end()) return "[]";
        std::string arr;
        bool first = true;
        for (const SymbolDef* s : it->second.documentSymbols()) {
            int sl = s->nameSpan.start.line, sc = s->nameSpan.start.col;
            std::string r = encRange(uri, sl, sc, sl, sc + s->nameSpan.length);
            if (!first) arr += ",";
            first = false;
            arr += "{\"name\":" + json::quote(s->name) + ",\"kind\":" + std::to_string(lspSymbolKind(s->kind)) +
                   ",\"range\":" + r + ",\"selectionRange\":" + r + "}";
        }
        return "[" + arr + "]";
    }
};

void lspReply(const json::Value& id, const std::string& resultJson) {
    std::string idStr = id.kind == json::Value::Kind::String ? json::quote(id.asString())
                      : id.kind == json::Value::Kind::Number ? id.str
                      : "null";
    lspSend("{\"jsonrpc\":\"2.0\",\"id\":" + idStr + ",\"result\":" + resultJson + "}");
}

int runLsp(const std::vector<std::string>&) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    LspServer srv;
    std::string body;
    while (lspRead(body)) {
        json::Value msg = json::parse(body);
        const std::string& method = msg["method"].asString();
        const json::Value& id = msg["id"];
        const json::Value& params = msg["params"];

        if (method == "initialize") {
            std::string root = uriToPath(params["rootUri"].asString());
            if (root.empty()) root = params["rootPath"].asString();
            const json::Value& opts = params["initializationOptions"];
            if (opts["root"].kind == json::Value::Kind::String) root = opts["root"].asString();
            if (opts["lib"].kind == json::Value::Kind::String) srv.lib_ = opts["lib"].asString();
            srv.root_ = root;
            // Only pick "utf-8" if the client offered it (LSP requires choosing from its positionEncodings);
            // otherwise fall back to the utf-16 default. Our columns are byte offsets; under utf-16 the server
            // converts columns per line (inCol/encRange/semanticTokens) so non-ASCII positions are correct too.
            bool utf8 = false;
            for (const auto& e : params["capabilities"]["general"]["positionEncodings"].items())
                if (e.asString() == "utf-8") utf8 = true;
            std::string enc = utf8 ? "utf-8" : "utf-16";
            srv.utf16_ = !utf8;
            lspReply(id, "{\"capabilities\":{\"positionEncoding\":\"" + enc + "\",\"textDocumentSync\":1,"
                         "\"definitionProvider\":true,\"documentSymbolProvider\":true,\"hoverProvider\":true,"
                         "\"documentFormattingProvider\":true,\"referencesProvider\":true,\"renameProvider\":true,"
                         "\"completionProvider\":{\"triggerCharacters\":[\".\"]},"
                         "\"semanticTokensProvider\":{\"legend\":{\"tokenTypes\":[\"function\",\"type\","
                         "\"method\",\"property\",\"variable\",\"parameter\",\"enumMember\"],"
                         "\"tokenModifiers\":[\"declaration\"]},\"full\":true}},"
                         "\"serverInfo\":{\"name\":\"polyglot-lsp\",\"version\":\"0.0.1\"}}");
        } else if (method == "shutdown") {
            lspReply(id, "null");
        } else if (method == "exit") {
            break;
        } else if (method == "textDocument/didOpen") {
            std::string uri = params["textDocument"]["uri"].asString();
            srv.text_[uri] = params["textDocument"]["text"].asString();
            srv.analyzeDoc(uri);
        } else if (method == "textDocument/didChange") {
            std::string uri = params["textDocument"]["uri"].asString();
            const auto& changes = params["contentChanges"].items();
            if (!changes.empty()) srv.text_[uri] = changes.back()["text"].asString(); // Full sync
            // Re-analyze every open doc (not just the edited one) so a dependent importing this now-edited,
            // still-unsaved buffer refreshes live — the BufferResolver serves the open text. Cheap for the
            // handful of files an editor holds open; dependency-tracked re-analysis is a later optimization.
            for (const auto& kv : srv.text_) srv.analyzeDoc(kv.first);
        } else if (method == "workspace/didChangeWatchedFiles") {
            // pgconfig.json changed — re-analyze every open document so their diagnostics reflect the new
            // root/lib immediately (each analyzeDoc re-reads the manifest and re-publishes).
            for (const auto& kv : srv.text_) srv.analyzeDoc(kv.first);
        } else if (method == "textDocument/didClose") {
            std::string uri = params["textDocument"]["uri"].asString();
            srv.text_.erase(uri);
            srv.model_.erase(uri);
            if (uri.rfind("file:", 0) == 0) srv.publishDiagnostics(uri, {}); // clear squiggles for a closed file
        } else if (method == "textDocument/definition") {
            lspReply(id, srv.definition(params));
        } else if (method == "textDocument/documentSymbol") {
            lspReply(id, srv.documentSymbol(params));
        } else if (method == "textDocument/formatting") {
            lspReply(id, srv.formatting(params));
        } else if (method == "textDocument/semanticTokens/full") {
            lspReply(id, srv.semanticTokens(params));
        } else if (method == "textDocument/references") {
            lspReply(id, srv.references(params));
        } else if (method == "textDocument/rename") {
            lspReply(id, srv.rename(params));
        } else if (method == "textDocument/completion") {
            lspReply(id, srv.completion(params));
        } else if (method == "polyglot/moduleSource") {
            // Custom request: serve an embedded std module's source for a `polyglot:<name>` virtual document.
            std::string u = params["uri"].asString();
            std::string name = u.rfind("polyglot:", 0) == 0 ? u.substr(9) : u;
            lspReply(id, "{\"source\":" + json::quote(embeddedModuleSource(name)) + "}");
        } else if (method == "polyglot/emit") {
            // Custom request: emit a live in-memory preview of the target source for an open .pg doc.
            lspReply(id, srv.generatedSource(params));
        } else if (method == "textDocument/hover") {
            lspReply(id, srv.hover(params));
        } else if (!id.isNull()) {
            lspReply(id, "null"); // unknown request — answer so the client doesn't hang
        }
    }
    return 0;
}

// `polyglot install <dir-or-npm-name>` (P19 slice 11): validate a plugin artifact and place it in the
// user cache, where target resolution finds it. A DIRECTORY containing polyglot-plugin.json installs
// locally; a bare name shells out to `npm pack` (targets publish as npm packages wrapping the manifest)
// and extracts `package/polyglot-plugin.json` with the system tar.
int runInstall(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::cerr << "polyglot: install needs a plugin directory or npm package name\n";
        return 64;
    }
    std::string spec = args[1];
    fs::path manifestPath;
    std::error_code ec;
    if (fs::is_directory(spec, ec)) {
        manifestPath = fs::path(spec) / "polyglot-plugin.json";
    } else {
        // A bare target name resolves to the first-party npm naming convention; full package
        // specs (@scope/name, name@version) pass through untouched.
        if (spec.find('@') == std::string::npos && spec.find('/') == std::string::npos)
            spec = "@mintplayer/polyglot-target-" + spec;
        const fs::path tmp = fs::temp_directory_path() / "polyglot-install";
        fs::remove_all(tmp, ec);
        fs::create_directories(tmp, ec);
        const std::string cmd = "npm pack \"" + spec + "\" --pack-destination \"" + tmp.string() + "\" >nul 2>nul";
        if (std::system(cmd.c_str()) != 0) {
            std::cerr << "polyglot: npm pack '" << spec << "' failed (is npm on PATH and the package published?)\n";
            return 1;
        }
        fs::path tgz;
        for (const auto& e : fs::directory_iterator(tmp, ec))
            if (e.path().extension() == ".tgz") tgz = e.path();
        if (tgz.empty() || std::system(("tar -xzf \"" + tgz.string() + "\" -C \"" + tmp.string() + "\"").c_str()) != 0) {
            std::cerr << "polyglot: could not extract the npm package\n";
            return 1;
        }
        manifestPath = tmp / "package" / "polyglot-plugin.json";
    }

    std::string manifest;
    if (!readFile(manifestPath, manifest)) {
        std::cerr << "polyglot: no polyglot-plugin.json at " << manifestPath.string() << "\n";
        return 1;
    }
    std::string err;
    if (!validateBackend(manifest, err)) { // a plugin that fails validation is never cached
        std::cerr << "polyglot: invalid plugin: " << err << "\n";
        return 1;
    }
    const std::string name = json::parse(manifest)["name"].asString();
    const fs::path dest = pluginCacheDir() / name;
    fs::create_directories(dest, ec);
    fs::copy_file(manifestPath, dest / "polyglot-plugin.json", fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::cerr << "polyglot: could not write " << (dest / "polyglot-plugin.json").string() << "\n";
        return 1;
    }
    std::cout << "installed target '" << name << "' -> " << dest.string() << "\n";
    return 0;
}

// Load every target plugin found next to the executable (`plugins/<target>/polyglot-plugin.json`). The
// CLI is a pure engine — no target is compiled in (PRD §4.11); pgconfig-driven resolution (local paths /
// cache / registry) layers on top at P19 slice 10. A missing plugins dir just leaves the registry empty
// (findTarget then explains what was expected); a MALFORMED artifact is reported and skipped.
void loadPluginsNextToExe(const char* argv0) {
    fs::path exe;
#ifdef _WIN32
    char buf[4096];
    const unsigned long n = GetModuleFileNameA(nullptr, buf, sizeof(buf));
    exe = n > 0 ? fs::path(std::string(buf, n)) : fs::path(argv0);
#else
    exe = fs::path(argv0);
#endif
    std::error_code ec;
    const fs::path dir = exe.parent_path() / "plugins";
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        const fs::path manifest = entry.path() / "polyglot-plugin.json";
        if (!fs::exists(manifest, ec)) continue;
        std::ifstream in(manifest, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        std::string err;
        if (!loadBackend(ss.str(), err))
            std::cerr << "polyglot: " << manifest.string() << ": " << err << "\n";
    }
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
    loadPluginsNextToExe(argv[0]);
    if (args[0] == "build") {
        return runBuild(args);
    }
    if (args[0] == "fmt") {
        return runFmt(args);
    }
    if (args[0] == "check") {
        return runCheck(args);
    }
    if (args[0] == "lsp") {
        return runLsp(args);
    }
    if (args[0] == "install") {
        return runInstall(args);
    }

    std::cerr << "polyglot: unknown command '" << args[0] << "'\n\n";
    printUsage();
    return 64; // EX_USAGE
}
