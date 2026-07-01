#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

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
        << "  polyglot build <input.pg> [--target <csharp|typescript|python>] [--out <dir>] [--root <dir>] [--lib <a,b>]\n"
        << "  polyglot fmt <input.pg>\n"
        << "  polyglot check <input.pg> [--json] [--root <dir>] [--lib <a,b>]\n"
        << "  polyglot lsp\n"
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
};
PgConfig loadPgConfig(const fs::path& startDir) {
    for (fs::path d = startDir;; d = d.parent_path()) {
        std::string src;
        if (readFile(d / "pgconfig.json", src)) {
            json::Value v = json::parse(src);
            PgConfig pc;
            pc.found = true;
            std::string r = v["root"].asString();
            pc.root = (r.empty() ? d : (d / r)).lexically_normal().string();
            for (const auto& e : v["lib"].items()) { if (!pc.lib.empty()) pc.lib += ","; pc.lib += e.asString(); }
            return pc;
        }
        if (!d.has_parent_path() || d.parent_path() == d) return {};
    }
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
    // A pgconfig.json near the input fills in root/lib the user didn't pass explicitly (explicit flags win).
    PgConfig pc = loadPgConfig(entryDir);
    if (root.empty() && pc.found && !pc.root.empty()) root = pc.root;
    if (libArg.empty() && pc.found) libArg = pc.lib;
    if (root.empty()) root = entryDir;
    FileModuleResolver resolver(root, entryDir);

    LibConfig lib = parseLibList(libArg);

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

    EmitResult result = compile(source, Target::CSharp, &resolver, lib);

    if (json) {
        auto severityName = [](Severity s) {
            switch (s) {
                case Severity::Warning: return "warning";
                case Severity::Info:    return "info";
                case Severity::Hint:    return "hint";
                default:                return "error";
            }
        };
        std::cout << "[";
        for (std::size_t i = 0; i < result.diagnostics.size(); ++i) {
            const auto& d = result.diagnostics[i];
            if (i) std::cout << ",";
            std::cout << "{\"line\":" << d.pos.line << ",\"col\":" << d.pos.col
                      << ",\"endLine\":" << d.end.line << ",\"endCol\":" << d.end.col
                      << ",\"severity\":\"" << severityName(d.severity) << "\""
                      << ",\"message\":\"" << jsonEscape(d.message) << "\"}";
        }
        std::cout << "]\n";
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

std::size_t byteOffset(const std::string& text, int line, int col) {
    std::size_t off = 0;
    for (int ln = 1; ln < line && off < text.size(); ++off) if (text[off] == '\n') ++ln;
    off += static_cast<std::size_t>(col - 1);
    return off < text.size() ? off : text.size();
}
bool identPart(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

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

struct LspServer {
    std::map<std::string, std::string> text_;    // uri -> current source (Full-sync buffer)
    std::map<std::string, SemanticModel> model_; // uri -> latest analyzed model
    std::string root_;
    std::string lib_ = "io,math";

    // A range from 1-based (line,col) positions, emitted as 0-based LSP positions (utf-8 = byte columns).
    static std::string rangeJson(int sl, int sc, int el, int ec) {
        return "{\"start\":{\"line\":" + std::to_string(sl - 1) + ",\"character\":" + std::to_string(sc - 1) +
               "},\"end\":{\"line\":" + std::to_string(el - 1) + ",\"character\":" + std::to_string(ec - 1) + "}}";
    }

    void analyzeDoc(const std::string& uri) {
        fs::path p(uriToPath(uri));
        fs::path entryDir = p.has_parent_path() ? p.parent_path() : fs::path(".");
        // A pgconfig.json near the file drives root/lib and wins over the client's initializationOptions
        // (re-read each analysis, so editing pgconfig.json takes effect on the next change).
        PgConfig pc = loadPgConfig(entryDir);
        fs::path root = pc.found && !pc.root.empty() ? fs::path(pc.root)
                      : (root_.empty() ? entryDir : fs::path(root_));
        std::string libStr = pc.found ? pc.lib : lib_;
        FileModuleResolver resolver(root, entryDir);
        AnalysisResult a = analyze(text_[uri], &resolver, parseLibList(libStr));
        model_[uri] = std::move(a.model);
        publishDiagnostics(uri, a.diagnostics);
    }

    void publishDiagnostics(const std::string& uri, const std::vector<Diagnostic>& diags) {
        const std::string& text = text_.count(uri) ? text_[uri] : uri; // uri unused when text absent
        std::string arr;
        for (std::size_t i = 0; i < diags.size(); ++i) {
            const auto& d = diags[i];
            int sl = d.pos.line, sc = d.pos.col, el = d.end.line, ec = d.end.col;
            if (el == sl && ec == sc) { // widen a point to the identifier at that spot (else a 1-char range)
                std::size_t off = byteOffset(text, sl, sc), end = off;
                while (end < text.size() && identPart(text[end])) ++end;
                ec = sc + static_cast<int>(end - off);
                if (ec == sc) ec = sc + 1;
            }
            int sev = d.severity == Severity::Warning ? 2 : d.severity == Severity::Info ? 3
                    : d.severity == Severity::Hint ? 4 : 1;
            if (i) arr += ",";
            arr += "{\"range\":" + rangeJson(sl, sc, el, ec) + ",\"severity\":" + std::to_string(sev) +
                   ",\"source\":\"polyglot\",\"message\":" + json::quote(d.message) + "}";
        }
        lspSend("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":" +
                json::quote(uri) + ",\"diagnostics\":[" + arr + "]}}");
    }

    const SemanticModel* modelFor(const json::Value& params, int& line, int& col, std::string& uri) {
        uri = params["textDocument"]["uri"].asString();
        line = static_cast<int>(params["position"]["line"].asInt()) + 1;
        col = static_cast<int>(params["position"]["character"].asInt()) + 1;
        auto it = model_.find(uri);
        return it == model_.end() ? nullptr : &it->second;
    }

    std::string definition(const json::Value& params) {
        int line, col; std::string uri;
        const SemanticModel* m = modelFor(params, line, col, uri);
        if (!m) return "null";
        const SymbolDef* d = m->definitionAt(line, col);
        if (!d) return "null";
        int sl = d->nameSpan.start.line, sc = d->nameSpan.start.col;
        return "{\"uri\":" + json::quote(uri) + ",\"range\":" + rangeJson(sl, sc, sl, sc + d->nameSpan.length) + "}";
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
        auto it = model_.find(params["textDocument"]["uri"].asString());
        if (it == model_.end()) return "{\"data\":[]}";
        const SemanticModel& m = it->second;
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
            int line0 = t.line - 1, char0 = t.col - 1;
            int dLine = line0 - prevLine;
            int dChar = dLine == 0 ? char0 - prevChar : char0;
            if (!first) data += ",";
            first = false;
            data += std::to_string(dLine) + "," + std::to_string(dChar) + "," + std::to_string(t.len) + "," +
                    std::to_string(t.type) + "," + std::to_string(t.mod);
            prevLine = line0;
            prevChar = char0;
        }
        return "{\"data\":[" + data + "]}";
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
            std::string r = rangeJson(sl, sc, sl, sc + s->nameSpan.length);
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
            // otherwise fall back to the utf-16 default. Our columns are byte offsets = utf-16 units for
            // ASCII, so either encoding maps correctly for ASCII (a non-ASCII walk is the documented follow-up).
            bool utf8 = false;
            for (const auto& e : params["capabilities"]["general"]["positionEncodings"].items())
                if (e.asString() == "utf-8") utf8 = true;
            std::string enc = utf8 ? "utf-8" : "utf-16";
            lspReply(id, "{\"capabilities\":{\"positionEncoding\":\"" + enc + "\",\"textDocumentSync\":1,"
                         "\"definitionProvider\":true,\"documentSymbolProvider\":true,\"hoverProvider\":true,"
                         "\"documentFormattingProvider\":true,"
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
            srv.analyzeDoc(uri);
        } else if (method == "textDocument/didClose") {
            std::string uri = params["textDocument"]["uri"].asString();
            srv.text_.erase(uri);
            srv.model_.erase(uri);
            srv.publishDiagnostics(uri, {}); // clear squiggles for a closed file
        } else if (method == "textDocument/definition") {
            lspReply(id, srv.definition(params));
        } else if (method == "textDocument/documentSymbol") {
            lspReply(id, srv.documentSymbol(params));
        } else if (method == "textDocument/formatting") {
            lspReply(id, srv.formatting(params));
        } else if (method == "textDocument/semanticTokens/full") {
            lspReply(id, srv.semanticTokens(params));
        } else if (method == "textDocument/hover") {
            lspReply(id, srv.hover(params));
        } else if (!id.isNull()) {
            lspReply(id, "null"); // unknown request — answer so the client doesn't hang
        }
    }
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
    if (args[0] == "check") {
        return runCheck(args);
    }
    if (args[0] == "lsp") {
        return runLsp(args);
    }

    std::cerr << "polyglot: unknown command '" << args[0] << "'\n\n";
    printUsage();
    return 64; // EX_USAGE
}
