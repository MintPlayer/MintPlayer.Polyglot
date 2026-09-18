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

} // namespace mintplayer::polyglot
