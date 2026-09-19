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

// ---------------------------------------------------------------------------------------------------
// P39/issue #71 — the read side.

namespace {
// Inverse of kBase64. -1 = not a Base64 digit.
int base64Digit(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
} // namespace

bool vlqDecode(const std::string& s, std::size_t& pos, int& out) {
    unsigned int result = 0;
    int shift = 0;
    bool more = true;
    bool any = false;
    while (more) {
        if (pos >= s.size()) return false;            // truncated run
        const int digit = base64Digit(s[pos]);
        if (digit < 0) return false;                  // not Base64 — refuse, don't guess
        ++pos;
        any = true;
        more = (digit & 32) != 0;
        result |= static_cast<unsigned int>(digit & 31) << shift;
        shift += 5;
        if (shift > 30 && more) return false;         // would overflow a 32-bit value
    }
    if (!any) return false;
    const bool negative = (result & 1u) != 0;
    result >>= 1;
    // -0 is the v3 sentinel for INT_MIN; nothing we consume needs it, and treating it as 0 is what
    // every decoder does.
    out = negative ? -static_cast<int>(result) : static_cast<int>(result);
    return true;
}

bool parseSourceMapV3(const std::string& jsonText, DecodedSourceMap& out, std::string& error) {
    out = DecodedSourceMap{};
    const json::Value doc = json::parse(jsonText);
    // The repo's JSON reader is LENIENT by design (built for JSON-RPC: malformed input parses to Null).
    // A coverage sidecar is a third-party file, so validate the SHAPE here rather than trusting the parse
    // — that is the strict posture the design calls for, applied where it can actually be enforced.
    if (doc.kind != json::Value::Kind::Object) { error = "not a JSON object"; return false; }
    const json::Value& version = doc["version"];
    if (version.kind != json::Value::Kind::Number || version.asInt(0) != 3) {
        error = "missing or unsupported source map 'version' (expected 3)";
        return false;
    }
    const json::Value& mappings = doc["mappings"];
    if (mappings.kind != json::Value::Kind::String) { error = "missing 'mappings'"; return false; }

    for (const auto& s : doc["sources"].items()) out.sources.push_back(s.asString());
    for (const auto& s : doc["sourcesContent"].items())
        out.sourcesContent.push_back(s.kind == json::Value::Kind::String ? s.asString() : std::string());

    const std::string& m = mappings.str;
    int generatedLine = 0; // 0-based while decoding
    int prevSource = 0, prevSourceLine = 0, prevSourceCol = 0, prevGenCol = 0, prevName = 0;
    std::size_t pos = 0;
    while (pos <= m.size()) {
        if (pos == m.size()) break;
        const char c = m[pos];
        if (c == ';') { ++generatedLine; prevGenCol = 0; ++pos; continue; }
        if (c == ',') { ++pos; continue; }

        int genCol = 0;
        if (!vlqDecode(m, pos, genCol)) { error = "malformed VLQ in 'mappings'"; return false; }
        prevGenCol += genCol;
        // A 1-field segment carries no source position — it marks generated code with no origin. Skip it,
        // but only AFTER consuming its field, or the running deltas desynchronize.
        if (pos >= m.size() || m[pos] == ',' || m[pos] == ';') continue;

        int dSource = 0, dLine = 0, dCol = 0;
        if (!vlqDecode(m, pos, dSource) || !vlqDecode(m, pos, dLine) || !vlqDecode(m, pos, dCol)) {
            error = "truncated segment in 'mappings'";
            return false;
        }
        prevSource += dSource;
        prevSourceLine += dLine;
        prevSourceCol += dCol;
        if (pos < m.size() && m[pos] != ',' && m[pos] != ';') { // optional 5th field: the name index
            int dName = 0;
            if (!vlqDecode(m, pos, dName)) { error = "malformed name index in 'mappings'"; return false; }
            prevName += dName;
        }
        if (prevSource < 0 || prevSourceLine < 0) { error = "negative index in 'mappings'"; return false; }
        // EVERY segment is kept (PRD §4.6). Several segments on one generated line are the normal case in
        // a map we did not write, and first-wins is exactly the under-reporting bug this replaces.
        out.segments.push_back(SourceMapSegment{generatedLine + 1, prevSource, prevSourceLine + 1});
    }
    (void)prevName;
    (void)prevSourceCol;
    return true;
}

} // namespace mintplayer::polyglot
