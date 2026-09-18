#include "mintplayer/polyglot/sourcemap.hpp"

#include <algorithm>

#include "mintplayer/polyglot/json.hpp"

namespace mintplayer::polyglot {

namespace {
const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
} // namespace

// Base64-VLQ: the sign goes in the low bit, then 5-bit groups little-endian, with bit 6 (32) as the
// continuation flag. ~20 lines, which is why the v3 format needs no library.
std::string vlqEncode(int value) {
    unsigned int v = value < 0 ? ((static_cast<unsigned int>(-value) << 1) | 1u)
                               : (static_cast<unsigned int>(value) << 1);
    std::string out;
    do {
        unsigned int digit = v & 31u;
        v >>= 5;
        if (v != 0) digit |= 32u; // more groups follow
        out += kBase64[digit];
    } while (v != 0);
    return out;
}

std::string buildSourceMapV3(const SourceMapInput& in) {
    // One segment per mapped output line, at column 0 of both sides (line granularity, PRD N1). The v3
    // `mappings` field is a ';'-separated list of output lines, each holding ','-separated segments whose
    // fields are DELTAS against the previous segment — hence the running state below.
    std::vector<OriginRecord> recs = in.origins;
    std::sort(recs.begin(), recs.end(), [](const OriginRecord& a, const OriginRecord& b) {
        return a.outputLine < b.outputLine;
    });

    std::string mappings;
    int emittedLine = 0;   // output line the ';'s have reached so far (0-based)
    int prevSource = 0;    // previous segment's source index
    int prevSourceLine = 0;// previous segment's source line (0-based)
    bool wroteAny = false;

    for (const auto& r : recs) {
        if (r.fileId < 0 || static_cast<std::size_t>(r.fileId) >= in.fileIdToSourceIndex.size()) continue;
        const int srcIndex = in.fileIdToSourceIndex[static_cast<std::size_t>(r.fileId)];
        if (srcIndex < 0) continue; // an origin with no mapped source (std helper, synthesized prelude)
        if (r.outputLine < 1 || r.sourceLine < 1) continue;

        const int targetLine = r.outputLine - 1; // v3 output lines are 0-based
        if (targetLine < emittedLine) continue;  // defensive: never walk backwards
        // Two records on the SAME output line must be separated by ',' — appending a bare second segment
        // would corrupt the mappings string rather than merely misplace a mapping. Cannot arise today
        // (one record per emitted line, outLine_ strictly increasing), so this is purely defensive, but
        // it is the case that produces a malformed map rather than a wrong one.
        const bool sameLine = (targetLine == emittedLine) && wroteAny;
        while (emittedLine < targetLine) { mappings += ';'; ++emittedLine; }
        if (sameLine) mappings += ',';

        // Fields: generatedColumn, sourceIndex, sourceLine, sourceColumn — all deltas except the first,
        // which resets to an absolute 0 on every new output line.
        mappings += vlqEncode(0);
        mappings += vlqEncode(srcIndex - prevSource);
        mappings += vlqEncode((r.sourceLine - 1) - prevSourceLine);
        mappings += vlqEncode(0);
        prevSource = srcIndex;
        prevSourceLine = r.sourceLine - 1;
        wroteAny = true;
    }
    (void)wroteAny;

    std::string out = "{\"version\":3,\"file\":" + json::quote(in.file) + ",\"sourceRoot\":\"\",\"sources\":[";
    for (std::size_t i = 0; i < in.sources.size(); ++i) {
        if (i) out += ",";
        out += json::quote(in.sources[i]);
    }
    out += "],\"sourcesContent\":[";
    for (std::size_t i = 0; i < in.sources.size(); ++i) {
        if (i) out += ",";
        // A source whose text the host could not read is null, which is legal v3 and degrades to
        // "resolve the path yourself" rather than lying about the content.
        if (i < in.sourcesContent.size()) out += json::quote(in.sourcesContent[i]);
        else out += "null";
    }
    out += "],\"names\":[],\"mappings\":" + json::quote(mappings) + "}\n";
    return out;
}

} // namespace mintplayer::polyglot
