#pragma once

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "mintplayer/polyglot/sourcemap.hpp"

// P39/issue #71 — coverage attribution: project a coverage report over GENERATED code back onto the `.pg`
// source it came from, so every target's test suite contributes to one `.pg` number.
//
// Two axes, deliberately independent (PRD §3):
//   * ORIGIN varies with the LANGUAGE and is declared by the plugin (`originMapping`). It decides how to
//     project generated lines onto `.pg` lines.
//   * FORMAT varies with the CONSUMER's toolchain and is chosen at the invocation. It decides how to READ
//     the report and how to WRITE the result.
// Neither knows about the other: every reader lowers to the neutral model below, and the projector never
// learns which format its input came from. A new target is a manifest entry; a new format is one reader.
//
// This translation unit does NO file IO and no path resolution — the HOST owns those (polyglot.hpp:86-90).
// It is handed text and an OriginIndex, and returns a model.

namespace mintplayer::polyglot::coverage {

enum class Format { Unknown, Lcov, Cobertura, Istanbul, Clover };

std::string formatName(Format f);
Format formatFromName(const std::string& name); // "" / unknown -> Format::Unknown

// One arm of one branch, from a format that carries real arm IDENTITY (lcov `BRDA`, istanbul `branchMap`,
// cobertura `<conditions>`). `key` is only comparable WITHIN one report — see the emission note below.
struct BranchArm {
    std::string key;
    long long taken = 0;
    bool takenKnown = true; // false = lcov's `-`: the enclosing block never executed, which is NOT `0`
};

struct Line {
    long long hits = 0;
    bool hitsKnown = false; // false = "executed, count unknown" (all JaCoCo can say). Never demote to 0.
    std::vector<BranchArm> arms;
    // Contributions from count-only formats (cobertura `condition-coverage`, clover truecount/falsecount,
    // JaCoCo mb/cb): a covered/total pair with no arm identity to union.
    int countCovered = 0;
    int countTotal = 0;
};

struct File {
    std::string path;
    std::map<int, Line> lines; // keyed by 1-based line, so a file's lines are inherently deduplicated
};

struct Report {
    Format format = Format::Unknown;
    std::vector<File> files;
    File& fileFor(const std::string& path); // find-or-append
};

// Merge `src` into `dst` under the PROJECTION rules (PRD §4.3):
//   * line hits: MAX. A `.pg` line reached from two generated lines was executed; summing would produce a
//     number that READS as an execution count and is not one.
//   * branch arms: UNION by key, `taken` maxed. Within one report the keys come from one instrumenter, so
//     they are comparable and union is correct: two generated 2-arm branches at 1/2 each onto one `.pg`
//     line give 2/4, not 1/2.
//   * count-only pairs: SUM, which is the same arithmetic the union performs on distinct keys.
void mergeLine(Line& dst, const Line& src);

// ---------------------------------------------------------------------------------------------------
// Reading

// Sniff the format from the document's own shape. Cobertura and clover SHARE a `<coverage>` root, so the
// discriminator is structural (`<class filename=…>` vs `<file name=…>`) — a root-name test silently reads
// a clover file as an empty cobertura, which is exactly how clover used to be rejected as "no files".
Format sniffFormat(const std::string& text);

// Parse `text` as `format`. Returns false with `error` set on input this reader cannot honestly consume.
// These are third-party files: a reader REFUSES rather than returning an empty report, because an empty
// report silently becomes "nothing was covered".
bool parseReport(const std::string& text, Format format, Report& out, std::string& error);

// ---------------------------------------------------------------------------------------------------
// Writing

struct WriteOptions {
    // Emit branch data as per-arm records (lcov `BRDA`, istanbul `branchMap`) instead of a count.
    //
    // OFF BY DEFAULT, and that default is load-bearing (PRD §4.4a). The coverage ingest unions arm SETS
    // across reports, which is correct only when the keys are comparable — and ours are not: the C# leg's
    // keys come from coverlet over IL, the TypeScript leg's from istanbul over JS, for the same `.pg`
    // line. When both suites cover the same arm (the common case) a union reports 2/2 where the truth is
    // 1/2, and is *correct* when they cover different arms, which makes the error data-dependent and
    // invisible. Count-only under-credits instead, which is the honest direction.
    //
    // Safe only for a SINGLE-target consumer, where no cross-instrumenter union can occur.
    bool branchArms = false;
};

// Serialize. `format` must be one this writer supports; count-only branch data can only be expressed by
// cobertura, clover and JaCoCo, so lcov and istanbul output is line-only unless `branchArms` is set.
std::string writeReport(const Report& report, Format format, const WriteOptions& opts = {});

// ---------------------------------------------------------------------------------------------------
// Projection

// What the host discovered about origins, built from `.map` sidecars (sourceMapV3 targets) or from a scan
// of emitted `#line` directives (directive targets). The Core does no IO, so the host fills this in.
struct OriginIndex {
    // normalized generated path -> generated line -> the (.pg path, .pg line) pairs it came from.
    // A list, because several `.pg` lines can legitimately share one generated line.
    std::unordered_map<std::string, std::map<int, std::vector<std::pair<std::string, int>>>> byGenerated;
    // Every `.pg` file the origin data mentions, and the line set each one maps.
    //
    // This is the DENOMINATOR, and it comes from the origin data rather than from the coverage report
    // (PRD §4.2 and §4.2a). Two consequences, both deliberate: a `.pg` line with no mapping is ABSENT
    // rather than reported uncovered, and a `.pg` module that no test touched is emitted at ZERO rather
    // than vanishing — which would inflate the percentage precisely when a module is least tested.
    std::map<std::string, std::set<int>> mappedLines;
};

struct ProjectionStats {
    int generatedFilesMatched = 0;
    int generatedFilesUnmatched = 0; // in the report, but no origin data — not ours, skipped silently
    int pgFiles = 0;
    int pgFilesWithoutReportData = 0; // emitted at zero (D7)
    int pgLinesCovered = 0;
    int pgLinesMappable = 0;
};

// Project a generated-keyed report onto `.pg` files. `out.format` is left for the caller to set.
Report projectReport(const Report& generated, const OriginIndex& index, ProjectionStats& stats);

// Resolve a path as written in a coverage report against the generated paths the origin index knows.
// Matching is separator-insensitive and accepts EITHER path being the longer one (a report path can be
// absolute, or rooted at a common prefix the producer chose), but requires exactly ONE candidate —
// an ambiguous basename resolves to nothing rather than to the wrong file.
std::string matchGeneratedPath(const std::string& reportPath,
                               const std::unordered_map<std::string,
                                   std::map<int, std::vector<std::pair<std::string, int>>>>& byGenerated);

// Forward slashes, no `./` prefix, no trailing slash. Applied to every path crossing this boundary.
std::string normalizePath(const std::string& p);

} // namespace mintplayer::polyglot::coverage
