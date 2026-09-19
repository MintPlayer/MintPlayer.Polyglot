#pragma once

#include <string>
#include <vector>

#include "mintplayer/polyglot/polyglot.hpp"

// P38/issue #69 — Source Map v3 generation, so a coverage tool running over emitted TypeScript can
// attribute hits back to the `.pg` it came from.
//
// This is pure computation over data the caller supplies: Base64-VLQ encoding plus the JSON envelope, with
// the repo's own JSON writer, so there is no third-party dependency. It deliberately does NOT resolve
// paths or read files — the HOST decides where an emitted file is routed, and therefore what the sources
// are relative to. Feed it what it should say; it says exactly that.
//
// LINE GRANULARITY ONLY (PRD N1). Every mapping segment points at column 0 of both sides. Column-accurate
// mappings would mean threading positions through the expression rule interpreter, which returns bare
// strings — a far larger job, and coverage needs lines, not columns.

namespace mintplayer::polyglot {

// Base64-VLQ, the encoding the v3 `mappings` field is built from. Exposed for testing.
std::string vlqEncode(int value);

struct SourceMapInput {
    std::string file;                            // the emitted file's name, as it sits beside the map
    std::vector<std::string> sources;            // source paths, RELATIVE TO THE MAP's own location
    std::vector<std::string> sourcesContent;     // each source's full text; same order as `sources`.
                                                 // Embedding it is what makes the map survive bundling,
                                                 // relocation, and output routed into a different tree —
                                                 // the consumer then needs no path resolution at all.
    std::vector<OriginRecord> origins;           // (outputLine, fileId, sourceLine), 1-based lines
    std::vector<int> fileIdToSourceIndex;        // SourceMap fileId -> index into `sources`; -1 = not mapped
};

// Render the v3 JSON document. Origins whose fileId has no `sources` entry are skipped, so a caller can
// hand over everything it recorded and let unmapped origins (std helpers, synthesized preludes) fall away.
std::string buildSourceMapV3(const SourceMapInput& in);

// ---------------------------------------------------------------------------------------------------
// P39/issue #71 — the READ side, for `polyglot coverage`.
//
// Decoding is not the mirror image of encoding: we emit one segment per line at column 0, but a map we are
// handed was written by istanbul, esbuild or tsc and carries real columns and several segments per line.
// So the decoder keeps EVERY segment (PRD §4.6 — "credit all segments, not first-wins"; the consuming
// repo's interim tool discarded all but the first and under-reported because of it).

// Base64-VLQ decode. Reads one value starting at `pos`, advancing it. Returns false on a malformed or
// truncated run — a third-party map is untrusted input, so this reports rather than guesses.
bool vlqDecode(const std::string& s, std::size_t& pos, int& out);

struct SourceMapSegment {
    int generatedLine = 0; // 1-based
    int sourceIndex = 0;
    int sourceLine = 0;    // 1-based
};

struct DecodedSourceMap {
    std::vector<std::string> sources;        // as written in the map (relative to the map's own directory)
    std::vector<std::string> sourcesContent; // may be shorter than `sources`, or hold empty entries
    std::vector<SourceMapSegment> segments;  // every 4- or 5-field segment, in file order
    // `sources` resolved against the map's own directory and normalized to forward slashes. Filled by the
    // caller (the host owns path semantics; the Core does no IO — polyglot.hpp:86-90).
    std::vector<std::string> resolvedSources;
};

// Parse a v3 map. Returns false with `error` set when the document is not a usable map: this is a
// third-party file, so a missing `mappings`, a non-3 `version` or a malformed VLQ run REFUSES rather than
// yielding an empty map that would silently produce a report with nothing in it.
bool parseSourceMapV3(const std::string& json, DecodedSourceMap& out, std::string& error);

} // namespace mintplayer::polyglot
