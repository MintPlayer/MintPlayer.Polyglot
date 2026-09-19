#pragma once

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "mintplayer/polyglot/backend.hpp"
#include "mintplayer/polyglot/coverage.hpp"
#include "mintplayer/polyglot/polyglot.hpp"
#include "mintplayer/polyglot/sourcemap.hpp"

// P39/issue #71 — `polyglot coverage remap`.
//
// Projects a coverage report over GENERATED code back onto the `.pg` source, so every target's test suite
// contributes to one `.pg` number. The pure model (readers, writers, projector) lives in the Core and is
// unit-tested there; this header owns only what the Core deliberately refuses to do — file IO and path
// resolution (polyglot.hpp:86-90).
//
// Two sinks, from `originMapping.style`, and the Core never learns which:
//   sourceMapV3 — read the `.map` sidecars beside the emitted files; the report names GENERATED files and
//                 is genuinely projected.
//   directive   — the downstream compiler already rewrote its own debug metadata, so the report ALREADY
//                 names `.pg` files. Nothing to project, but three jobs remain: prove the report really is
//                 `.pg`-keyed (if it isn't, `--origin-info` was off, whose only other symptom is a
//                 mysteriously lower percentage), enumerate the complete `.pg` file set so untested
//                 modules report as uncovered rather than vanishing, and rebase paths.

namespace mintplayer::polyglot::cli {

namespace fs = std::filesystem;

inline bool readFileText(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream os;
    os << in.rdbuf();
    out = os.str();
    // A BOM would otherwise reach a sniffer as the first character and defeat every leading-token test.
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB && static_cast<unsigned char>(out[2]) == 0xBF)
        out.erase(0, 3);
    return true;
}

// Repo-relative, forward-slashed. The emitted report must name paths a coverage service can match against
// its file list; anything longer or absolute is the producer's problem to avoid, not the consumer's.
inline std::string relativeToRoot(const fs::path& abs, const fs::path& root) {
    std::error_code ec;
    fs::path rel = fs::relative(abs, root, ec);
    if (ec || rel.empty() || rel.native().rfind(fs::path("..").native(), 0) == 0)
        return coverage::normalizePath(abs.string());
    return coverage::normalizePath(rel.string());
}

// Walk `dir` for files whose name ends with `suffix`. Recursive, because pgconfig `include` rules can
// route one closure's output into several trees and the tool must not need to be told where each went.
inline std::vector<fs::path> findBySuffix(const fs::path& dir, const std::string& suffix) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::exists(dir, ec)) return out;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string name = it->path().filename().string();
        if (name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            out.push_back(it->path());
    }
    return out;
}

// sourceMapV3: every sidecar under `genDir` contributes its generated file's line->origin table, and the
// union of its segments' source lines is that `.pg` file's mappable set (the DENOMINATOR, PRD §4.2).
inline bool buildIndexFromSidecars(const fs::path& genDir, const std::string& sidecarExt, const fs::path& root,
                                   coverage::OriginIndex& index, std::string& error) {
    const std::vector<fs::path> maps = findBySuffix(genDir, sidecarExt);
    if (maps.empty()) {
        error = "no '*" + sidecarExt + "' sidecars under '" + genDir.string() +
                "' - was the build run with --origin-info?";
        return false;
    }
    for (const fs::path& mapPath : maps) {
        std::string text;
        if (!readFileText(mapPath, text)) continue;
        DecodedSourceMap decoded;
        std::string err;
        if (!parseSourceMapV3(text, decoded, err)) {
            error = "sidecar '" + mapPath.string() + "': " + err;
            return false; // a malformed sidecar is a hard error: silently skipping it under-reports
        }
        // The emitted file is the sidecar's name minus the sidecar extension (`solver.ts.map` ->
        // `solver.ts`), which is the convention the manifest's `sidecarExtension` describes.
        std::string genName = mapPath.filename().string();
        genName = genName.substr(0, genName.size() - sidecarExt.size());
        const fs::path genPath = mapPath.parent_path() / genName;
        const std::string genKey = relativeToRoot(genPath, root);

        // `sources` are relative to the MAP's own directory (that is what makes routing into another tree
        // work), so resolve there, then re-root for output.
        std::vector<std::string> resolved;
        for (const std::string& s : decoded.sources) {
            std::error_code ec;
            const fs::path abs = fs::weakly_canonical(mapPath.parent_path() / s, ec);
            resolved.push_back(relativeToRoot(ec ? (mapPath.parent_path() / s) : abs, root));
        }
        for (const SourceMapSegment& seg : decoded.segments) {
            if (seg.sourceIndex < 0 || static_cast<std::size_t>(seg.sourceIndex) >= resolved.size()) continue;
            const std::string& pg = resolved[seg.sourceIndex];
            index.byGenerated[genKey][seg.generatedLine].emplace_back(pg, seg.sourceLine);
            index.mappedLines[pg].insert(seg.sourceLine);
        }
    }
    return true;
}

// Build a matcher from the manifest's own directive template (`#line $n "$f"`), so the scan is plugin data
// rather than knowledge of C#. Splits the template into the literal text around `$n` and `$f`.
struct DirectivePattern {
    std::string pre, mid, post;
    bool numberFirst = true;
    bool ok = false;
};

inline DirectivePattern compileDirectivePattern(const std::string& tmpl) {
    DirectivePattern p;
    const std::size_t n = tmpl.find("$n");
    const std::size_t f = tmpl.find("$f");
    if (n == std::string::npos || f == std::string::npos) return p;
    p.numberFirst = n < f;
    const std::size_t a = std::min(n, f), b = std::max(n, f);
    p.pre = tmpl.substr(0, a);
    p.mid = tmpl.substr(a + 2, b - (a + 2));
    p.post = tmpl.substr(b + 2);
    p.ok = true;
    return p;
}

// Parse one emitted line against the pattern. Returns false when it is not a directive.
inline bool matchDirective(const std::string& line, const DirectivePattern& p, int& outLine, std::string& outFile) {
    if (!p.ok) return false;
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (line.compare(i, p.pre.size(), p.pre) != 0) return false;
    i += p.pre.size();
    const std::size_t midAt = p.mid.empty() ? std::string::npos : line.find(p.mid, i);
    if (!p.mid.empty() && midAt == std::string::npos) return false;
    const std::string first = line.substr(i, (p.mid.empty() ? line.size() : midAt) - i);
    std::size_t rest = p.mid.empty() ? line.size() : midAt + p.mid.size();
    std::string second = line.substr(rest);
    if (!p.post.empty()) {
        const std::size_t postAt = second.rfind(p.post);
        if (postAt == std::string::npos) return false;
        second = second.substr(0, postAt);
    }
    const std::string& numStr = p.numberFirst ? first : second;
    const std::string& fileStr = p.numberFirst ? second : first;
    if (numStr.empty()) return false;
    for (char c : numStr)
        if (c < '0' || c > '9') return false;
    outLine = std::atoi(numStr.c_str());
    outFile = fileStr;
    return outLine > 0;
}

// directive: the report is already `.pg`-keyed, so the index is the IDENTITY map over the `.pg` lines the
// emitted code claims. Feeding that to the same projector gives path matching, denominator filtering and
// zero-fill for free, with no second code path.
inline bool buildIndexFromDirectives(const fs::path& genDir, const std::string& fileExt,
                                     const std::string& lineTemplate, const fs::path& root,
                                     coverage::OriginIndex& index, std::string& error) {
    const DirectivePattern pattern = compileDirectivePattern(lineTemplate);
    if (!pattern.ok) {
        error = "plugin's originMapping.line template has no $n/$f placeholders";
        return false;
    }
    const std::vector<fs::path> files = findBySuffix(genDir, fileExt);
    if (files.empty()) {
        error = "no '*" + fileExt + "' files under '" + genDir.string() + "'";
        return false;
    }
    bool sawAny = false;
    for (const fs::path& f : files) {
        std::string text;
        if (!readFileText(f, text)) continue;
        std::istringstream is(text);
        for (std::string ln; std::getline(is, ln);) {
            int srcLine = 0;
            std::string srcFile;
            if (!matchDirective(ln, pattern, srcLine, srcFile)) continue;
            std::error_code ec;
            const fs::path abs = fs::weakly_canonical(fs::path(srcFile), ec);
            const std::string pg = relativeToRoot(ec ? fs::path(srcFile) : abs, root);
            index.mappedLines[pg].insert(srcLine);
            index.byGenerated[pg][srcLine].emplace_back(pg, srcLine); // identity
            sawAny = true;
        }
    }
    if (!sawAny) {
        error = "no origin directives found in the emitted output - was the build run with --origin-info?";
        return false;
    }
    return true;
}

inline void printCoverageUsage() {
    std::cout
        << "Usage:\n"
        << "  polyglot coverage remap <report> --target <name> --out <path>\n"
        << "                          [--format <fmt>] [--out-format <fmt>] [--root <dir>]\n"
        << "                          [--generated-dir <dir>] [--branch-arms]\n"
        << "\n"
        << "  Projects a coverage report over generated code back onto the .pg source it came from,\n"
        << "  so each target's suite contributes to one .pg number. Formats (read and write):\n"
        << "  cobertura, lcov, istanbul, clover. --format is sniffed by default; --out-format\n"
        << "  defaults to the input format.\n"
        << "\n"
        << "  Branch data is emitted COUNT-ONLY, so only cobertura, clover and JaCoCo can carry it;\n"
        << "  lcov and istanbul output is line-only. --branch-arms emits per-arm records instead and\n"
        << "  is safe ONLY for a single-target consumer: arm identities from two different targets\n"
        << "  are not comparable, so merging them inflates the number.\n";
}

inline int runCoverage(const std::vector<std::string>& args) {
    if (args.size() < 2 || args[1] == "-h" || args[1] == "--help") {
        printCoverageUsage();
        return args.size() < 2 ? 64 : 0;
    }
    if (args[1] != "remap") {
        std::cerr << "polyglot: unknown coverage subcommand '" << args[1] << "'\n\n";
        printCoverageUsage();
        return 64;
    }

    std::string reportPath, targetName, outPath, formatName, outFormatName, rootArg, genDirArg;
    bool branchArms = false;
    for (std::size_t i = 2; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= args.size()) {
                std::cerr << "polyglot: " << what << " needs a value\n";
                return std::string();
            }
            return args[++i];
        };
        if (a == "--target")            targetName = next("--target");
        else if (a == "--out")          outPath = next("--out");
        else if (a == "--format")       formatName = next("--format");
        else if (a == "--out-format")   outFormatName = next("--out-format");
        else if (a == "--root")         rootArg = next("--root");
        else if (a == "--generated-dir") genDirArg = next("--generated-dir");
        else if (a == "--branch-arms")  branchArms = true;
        else if (!a.empty() && a[0] == '-') {
            std::cerr << "polyglot: unknown option '" << a << "'\n\n";
            printCoverageUsage();
            return 64;
        } else if (reportPath.empty()) reportPath = a;
        else {
            std::cerr << "polyglot: unexpected argument '" << a << "'\n";
            return 64;
        }
    }
    if (reportPath.empty() || targetName.empty() || outPath.empty()) {
        std::cerr << "polyglot: coverage remap needs <report>, --target and --out\n\n";
        printCoverageUsage();
        return 64;
    }

    BackendHandle target = findTarget(targetName);
    if (!target.ok()) {
        std::cerr << "polyglot: unknown target '" << targetName << "'\n";
        return 64;
    }
    const OriginMapping& om = target.backend()->originMapping();
    if (!om.recordsOrigins()) {
        std::cerr << "polyglot: target '" << targetName << "' declares no originMapping, so its output "
                  << "carries no link back to the .pg source; nothing to attribute\n";
        return 65;
    }

    const fs::path root = rootArg.empty() ? fs::current_path() : fs::absolute(fs::path(rootArg));
    const fs::path genDir = genDirArg.empty() ? root : fs::path(genDirArg);

    coverage::OriginIndex index;
    std::string err;
    const bool built = om.emitsSourceMap()
                           ? buildIndexFromSidecars(genDir, om.sidecarExtension, root, index, err)
                           : buildIndexFromDirectives(genDir, target.backend()->fileExtension(), om.line,
                                                      root, index, err);
    if (!built) {
        std::cerr << "polyglot: " << err << "\n";
        return 65;
    }

    std::string reportText;
    if (!readFileText(fs::path(reportPath), reportText)) {
        std::cerr << "polyglot: cannot open '" << reportPath << "'\n";
        return 66;
    }

    coverage::Format inFormat = coverage::formatFromName(formatName);
    if (!formatName.empty() && inFormat == coverage::Format::Unknown) {
        std::cerr << "polyglot: unknown --format '" << formatName
                  << "' (known: cobertura, lcov, istanbul, clover)\n";
        return 64;
    }
    if (inFormat == coverage::Format::Unknown) inFormat = coverage::sniffFormat(reportText);
    if (inFormat == coverage::Format::Unknown) {
        std::cerr << "polyglot: cannot determine the format of '" << reportPath
                  << "'; pass --format (cobertura, lcov, istanbul, clover)\n";
        return 65;
    }

    coverage::Report input;
    if (!coverage::parseReport(reportText, inFormat, input, err)) {
        std::cerr << "polyglot: '" << reportPath << "' does not parse as "
                  << coverage::formatName(inFormat) << ": " << err << "\n";
        return 65;
    }

    coverage::ProjectionStats stats;
    coverage::Report projected = coverage::projectReport(input, index, stats);
    if (stats.generatedFilesMatched == 0) {
        std::cerr << "polyglot: no file in '" << reportPath << "' matched the origin data under '"
                  << genDir.string() << "'";
        if (om.emitsDirectives())
            std::cerr << " - a directive target's report should already name .pg files, so this usually "
                      << "means --origin-info was off for the build under test";
        std::cerr << "\n";
        return 65;
    }

    coverage::Format outFormat = coverage::formatFromName(outFormatName);
    if (!outFormatName.empty() && outFormat == coverage::Format::Unknown) {
        std::cerr << "polyglot: unknown --out-format '" << outFormatName
                  << "' (known: cobertura, lcov, istanbul, clover)\n";
        return 64;
    }
    if (outFormat == coverage::Format::Unknown) outFormat = inFormat; // echo the consumer's own pipeline
    projected.format = outFormat;

    coverage::WriteOptions opts;
    opts.branchArms = branchArms;
    const std::string rendered = coverage::writeReport(projected, outFormat, opts);

    const fs::path out(outPath);
    std::error_code ec;
    if (out.has_parent_path()) fs::create_directories(out.parent_path(), ec);
    std::ofstream os(out, std::ios::binary);
    if (!os) {
        std::cerr << "polyglot: cannot write '" << outPath << "'\n";
        return 73;
    }
    os << rendered;
    os.close();

    std::cout << "polyglot: " << stats.pgFiles << " .pg file(s), " << stats.pgLinesCovered << "/"
              << stats.pgLinesMappable << " mappable line(s) covered";
    if (stats.pgFilesWithoutReportData > 0)
        std::cout << "; " << stats.pgFilesWithoutReportData << " file(s) had no coverage data and are "
                  << "reported at zero";
    if (stats.generatedFilesUnmatched > 0)
        std::cout << "; " << stats.generatedFilesUnmatched << " file(s) in the report had no origin data "
                  << "and were skipped";
    std::cout << " -> " << out.string() << " (" << coverage::formatName(outFormat) << ")\n";
    return 0;
}

} // namespace mintplayer::polyglot::cli
