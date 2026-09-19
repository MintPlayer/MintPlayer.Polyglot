#include "mintplayer/polyglot/coverage.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <utility>

#include "mintplayer/polyglot/json.hpp"

namespace mintplayer::polyglot::coverage {

// ---------------------------------------------------------------------------------------------------
// Small shared helpers

std::string normalizePath(const std::string& p) {
    std::string s;
    s.reserve(p.size());
    for (char c : p) s += (c == '\\') ? '/' : c;
    while (s.size() >= 2 && s[0] == '.' && s[1] == '/') s.erase(0, 2);
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

std::string formatName(Format f) {
    switch (f) {
        case Format::Lcov:      return "lcov";
        case Format::Cobertura: return "cobertura";
        case Format::Istanbul:  return "istanbul";
        case Format::Clover:    return "clover";
        default:                return "unknown";
    }
}

Format formatFromName(const std::string& name) {
    if (name == "lcov")       return Format::Lcov;
    if (name == "cobertura")  return Format::Cobertura;
    if (name == "istanbul")   return Format::Istanbul;
    if (name == "clover")     return Format::Clover;
    return Format::Unknown;
}

File& Report::fileFor(const std::string& path) {
    for (auto& f : files)
        if (f.path == path) return f;
    files.push_back(File{path, {}});
    return files.back();
}

void mergeLine(Line& dst, const Line& src) {
    // Hits: MAX (PRD §4.3). `hitsKnown == false` means "executed, count unknown" — it must never be
    // demoted to 0, because 0 reads as not-covered and drops the line out of every percentage.
    if (src.hitsKnown) {
        dst.hits = dst.hitsKnown ? std::max(dst.hits, src.hits) : src.hits;
        dst.hitsKnown = true;
    }
    // Arms: union by key, `taken` maxed. `takenKnown == false` (lcov `-`) loses to any known value, so
    // "block never ran" merged with a real count yields the count.
    for (const auto& arm : src.arms) {
        auto it = std::find_if(dst.arms.begin(), dst.arms.end(),
                               [&](const BranchArm& a) { return a.key == arm.key; });
        if (it == dst.arms.end()) {
            dst.arms.push_back(arm);
        } else if (arm.takenKnown) {
            it->taken = it->takenKnown ? std::max(it->taken, arm.taken) : arm.taken;
            it->takenKnown = true;
        }
    }
    // Count-only pairs SUM, which is the same arithmetic union performs on distinct keys: two generated
    // 2-arm branches at 1/2 each onto one `.pg` line give 2/4.
    dst.countCovered += src.countCovered;
    dst.countTotal += src.countTotal;
}

namespace {

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

long long toLong(const std::string& s, long long dflt = 0) {
    if (s.empty()) return dflt;
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (end == s.c_str()) return dflt;
    return v;
}

// ---- a minimal XML tag scanner -------------------------------------------------------------------
// Cobertura and clover are the only XML we read, and between them we need four element names and eight
// attributes. A DOM would buy round-trip fidelity we deliberately do NOT want: the output is a freshly
// constructed report over different entities (`snake_solver.pg`, not `snake_solver.ts`), so "preserve the
// input verbatim" has no defined meaning where N generated classes fold into one `.pg` file.

struct XmlTag {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;
    bool close = false;
    bool selfClose = false;
    std::string attr(const std::string& key) const {
        for (const auto& kv : attrs)
            if (kv.first == key) return kv.second;
        return std::string();
    }
    bool has(const std::string& key) const {
        for (const auto& kv : attrs)
            if (kv.first == key) return true;
        return false;
    }
};

std::string xmlUnescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { out += s[i]; continue; }
        const std::size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) { out += s[i]; continue; }
        const std::string ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp")       out += '&';
        else if (ent == "lt")   out += '<';
        else if (ent == "gt")   out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            const long cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')
                                ? std::strtol(ent.c_str() + 2, nullptr, 16)
                                : std::strtol(ent.c_str() + 1, nullptr, 10);
            if (cp > 0 && cp < 128) out += static_cast<char>(cp);
            else { out += s.substr(i, semi - i + 1); }
        } else { out += s.substr(i, semi - i + 1); i = semi; continue; }
        i = semi;
    }
    return out;
}

// Advance to the next element tag. Skips comments, processing instructions, DOCTYPE and CDATA.
bool nextTag(const std::string& s, std::size_t& pos, XmlTag& out) {
    while (true) {
        const std::size_t lt = s.find('<', pos);
        if (lt == std::string::npos) return false;
        if (s.compare(lt, 4, "<!--") == 0) {
            const std::size_t end = s.find("-->", lt + 4);
            if (end == std::string::npos) return false;
            pos = end + 3;
            continue;
        }
        if (s.compare(lt, 9, "<![CDATA[") == 0) {
            const std::size_t end = s.find("]]>", lt + 9);
            if (end == std::string::npos) return false;
            pos = end + 3;
            continue;
        }
        if (lt + 1 < s.size() && (s[lt + 1] == '?' || s[lt + 1] == '!')) {
            const std::size_t end = s.find('>', lt);
            if (end == std::string::npos) return false;
            pos = end + 1;
            continue;
        }
        const std::size_t gt = s.find('>', lt);
        if (gt == std::string::npos) return false;

        out = XmlTag{};
        std::size_t i = lt + 1;
        if (i < s.size() && s[i] == '/') { out.close = true; ++i; }
        const std::size_t nameStart = i;
        while (i < gt && !std::isspace(static_cast<unsigned char>(s[i])) && s[i] != '/' ) ++i;
        out.name = s.substr(nameStart, i - nameStart);

        while (i < gt) {
            while (i < gt && (std::isspace(static_cast<unsigned char>(s[i])) || s[i] == '/')) ++i;
            if (i >= gt) break;
            const std::size_t keyStart = i;
            while (i < gt && s[i] != '=' && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            const std::string key = s.substr(keyStart, i - keyStart);
            while (i < gt && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            if (i < gt && s[i] == '=') {
                ++i;
                while (i < gt && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
                if (i < gt && (s[i] == '"' || s[i] == '\'')) {
                    const char q = s[i++];
                    const std::size_t valStart = i;
                    while (i < s.size() && s[i] != q) ++i;
                    if (!key.empty()) out.attrs.emplace_back(key, xmlUnescape(s.substr(valStart, i - valStart)));
                    if (i < s.size()) ++i;
                } else {
                    const std::size_t valStart = i;
                    while (i < gt && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
                    if (!key.empty()) out.attrs.emplace_back(key, xmlUnescape(s.substr(valStart, i - valStart)));
                }
            } else if (!key.empty()) {
                out.attrs.emplace_back(key, std::string());
            }
        }
        out.selfClose = gt > lt && s[gt - 1] == '/';
        pos = gt + 1;
        return true;
    }
}

std::string xmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out += c;
        }
    }
    return out;
}

// A rate rendered the way cobertura/clover consumers expect: a bare decimal, not a percentage.
std::string rate(long long covered, long long total) {
    if (total <= 0) return "1";
    std::ostringstream os;
    const double r = static_cast<double>(covered) / static_cast<double>(total);
    os.precision(4);
    os << std::fixed << r;
    return os.str();
}

} // namespace

// ---------------------------------------------------------------------------------------------------
// Sniffing

Format sniffFormat(const std::string& text) {
    const std::string head = text.substr(0, std::min<std::size_t>(text.size(), 65536));
    // lcov is line-oriented and starts with a record keyword.
    std::istringstream is(head);
    std::string firstNonEmpty;
    for (std::string ln; std::getline(is, ln);) {
        const std::string t = trim(ln);
        if (!t.empty()) { firstNonEmpty = t; break; }
    }
    if (firstNonEmpty.rfind("TN:", 0) == 0 || firstNonEmpty.rfind("SF:", 0) == 0) return Format::Lcov;
    if (!firstNonEmpty.empty() && firstNonEmpty[0] == '{') return Format::Istanbul;

    if (head.find("<coverage") != std::string::npos) {
        // Cobertura and clover share this root. Discriminate STRUCTURALLY: a root-name test reads a clover
        // document as an empty cobertura, which is how clover used to be rejected as "no files" rather than
        // as an unknown format.
        if (head.find("clover=") != std::string::npos) return Format::Clover;
        if (head.find("<class ") != std::string::npos || head.find("<class\t") != std::string::npos)
            return Format::Cobertura;
        if (head.find("<project") != std::string::npos) return Format::Clover;
        return Format::Cobertura;
    }
    return Format::Unknown;
}

// ---------------------------------------------------------------------------------------------------
// Readers

namespace {

bool parseLcov(const std::string& text, Report& out, std::string& error) {
    std::istringstream is(text);
    File* current = nullptr;
    bool sawRecord = false;
    for (std::string raw; std::getline(is, raw);) {
        const std::string ln = trim(raw);
        if (ln.empty()) continue;
        const std::size_t colon = ln.find(':');
        const std::string key = colon == std::string::npos ? ln : ln.substr(0, colon);
        const std::string val = colon == std::string::npos ? std::string() : ln.substr(colon + 1);
        if (key == "SF") {
            current = &out.fileFor(normalizePath(trim(val)));
            sawRecord = true;
        } else if (key == "DA" && current) {
            // DA:<line>,<hits>[,<checksum>]
            const std::size_t comma = val.find(',');
            if (comma == std::string::npos) continue;
            const int line = static_cast<int>(toLong(val.substr(0, comma)));
            if (line <= 0) continue;
            std::string hitsField = val.substr(comma + 1);
            const std::size_t second = hitsField.find(',');
            if (second != std::string::npos) hitsField = hitsField.substr(0, second);
            Line l;
            l.hits = toLong(hitsField);
            l.hitsKnown = true;
            mergeLine(current->lines[line], l);
        } else if (key == "BRDA" && current) {
            // BRDA:<line>,[e|f|U]<block>,<branch>,<taken>
            std::vector<std::string> parts;
            std::string cur;
            for (char c : val) {
                if (c == ',') { parts.push_back(cur); cur.clear(); }
                else cur += c;
            }
            parts.push_back(cur);
            if (parts.size() < 4) continue;
            const int line = static_cast<int>(toLong(parts[0]));
            if (line <= 0) continue;
            Line l;
            BranchArm arm;
            // The lcov 2.x `e`/`f`/`U` markers stay in the key: stripping them collides `e0` with block
            // `0`, and under a set model that merges two arms which are not the same arm.
            arm.key = parts[1] + ":" + parts[2];
            if (trim(parts[3]) == "-") { arm.takenKnown = false; arm.taken = 0; }
            else { arm.takenKnown = true; arm.taken = toLong(parts[3]); }
            l.arms.push_back(arm);
            mergeLine(current->lines[line], l);
        } else if (key == "end_of_record") {
            current = nullptr;
        }
    }
    if (!sawRecord) { error = "no SF: records — not an lcov report"; return false; }
    return true;
}

bool parseCobertura(const std::string& text, Report& out, std::string& error) {
    std::size_t pos = 0;
    XmlTag tag;
    std::vector<std::string> sourceRoots;
    File* current = nullptr;
    bool inSources = false, inMethods = false, sawClass = false;
    std::string pendingSourceText;

    while (nextTag(text, pos, tag)) {
        if (tag.name == "source" && !tag.close) {
            // <source> holds TEXT, not an attribute.
            const std::size_t gt = pos;
            const std::size_t lt = text.find('<', gt);
            if (lt != std::string::npos) sourceRoots.push_back(normalizePath(trim(text.substr(gt, lt - gt))));
            continue;
        }
        if (tag.name == "sources") { inSources = !tag.close; continue; }
        (void)inSources;
        // A SELF-CLOSING `<methods/>` opens nothing — treating it as an open section would skip every
        // <line> that follows it, which is exactly the shape our own writer emits.
        if (tag.name == "methods") {
            if (!tag.selfClose) inMethods = !tag.close;
            continue;
        }
        if (tag.name == "class") {
            if (tag.close) { current = nullptr; inMethods = false; continue; }
            const std::string filename = tag.attr("filename");
            if (filename.empty()) { if (tag.selfClose) current = nullptr; continue; }
            sawClass = true;
            // Several <class> elements legitimately share one filename (coverlet emits one per type AND
            // per closed generic instantiation). fileFor() merges them, and the per-line map deduplicates.
            current = &out.fileFor(normalizePath(filename));
            inMethods = false;
            if (tag.selfClose) current = nullptr;
            continue;
        }
        if (tag.name == "line" && !tag.close && current && !inMethods) {
            // Only <lines> that are direct children of <class> count; a <methods> section repeats the same
            // lines per method and would double-count them.
            const int number = static_cast<int>(toLong(tag.attr("number")));
            if (number <= 0) continue;
            Line l;
            l.hits = toLong(tag.attr("hits"));
            l.hitsKnown = tag.has("hits");
            const std::string cc = tag.attr("condition-coverage");
            if (!cc.empty()) {
                // `condition-coverage="50% (1/2)"` — a COUNT, with no arm identity. The optional
                // <conditions><condition coverage="50%"/> child is deliberately not read: recovering arms
                // from a percentage needs an arity assumption we have not measured, and clover taught us
                // what an unverified arity assumption costs. We emit count-only anyway.
                const std::size_t open = cc.find('(');
                const std::size_t slash = cc.find('/', open == std::string::npos ? 0 : open);
                const std::size_t close = cc.find(')', slash == std::string::npos ? 0 : slash);
                if (open != std::string::npos && slash != std::string::npos && close != std::string::npos) {
                    l.countCovered = static_cast<int>(toLong(cc.substr(open + 1, slash - open - 1)));
                    l.countTotal = static_cast<int>(toLong(cc.substr(slash + 1, close - slash - 1)));
                }
            }
            mergeLine(current->lines[number], l);
        }
    }
    if (!sawClass) { error = "no <class filename=…> elements — not a cobertura report"; return false; }
    // `<sources>` is deliberately NOT prepended. Measured against a real `coverage xml` run: its
    // <source> is an absolute directory whose spelling need not match the filesystem's case, so joining
    // it produces a path that then fails to match, while the bare `filename` matches by suffix without
    // it. Path resolution belongs to the matcher, which accepts either side being longer — the same rule
    // the upload ingest applies.
    (void)sourceRoots;
    return true;
}

bool parseClover(const std::string& text, Report& out, std::string& error) {
    std::size_t pos = 0;
    XmlTag tag;
    File* current = nullptr;
    bool sawFile = false;
    while (nextTag(text, pos, tag)) {
        if (tag.name == "file") {
            if (tag.close) { current = nullptr; continue; }
            std::string name = tag.attr("path");
            if (name.empty()) name = tag.attr("name");
            if (name.empty()) continue;
            sawFile = true;
            current = &out.fileFor(normalizePath(name));
            if (tag.selfClose) current = nullptr;
            continue;
        }
        if (tag.name == "line" && !tag.close && current) {
            const int num = static_cast<int>(toLong(tag.attr("num")));
            if (num <= 0) continue;
            const std::string type = tag.attr("type");
            Line l;
            if (type == "cond") {
                // truecount / falsecount are the numbers of TAKEN and UNTAKEN arms on the line, aggregated
                // over every branch on it — NOT a true-arm/false-arm pair. Verified against istanbul for
                // the same run: a line with two branches and four arms, all taken, reports truecount="4".
                // So clover is COUNT-ONLY, and reading it as two arms would flip every partial line.
                const int t = static_cast<int>(toLong(tag.attr("truecount")));
                const int f = static_cast<int>(toLong(tag.attr("falsecount")));
                l.countCovered = t;
                l.countTotal = t + f;
                l.hits = toLong(tag.attr("count"));
                l.hitsKnown = tag.has("count");
            } else if (type == "method" || type == "stmt" || type.empty()) {
                l.hits = toLong(tag.attr("count"));
                l.hitsKnown = tag.has("count");
            } else {
                continue;
            }
            mergeLine(current->lines[num], l);
        }
    }
    if (!sawFile) { error = "no <file> elements — not a clover report"; return false; }
    return true;
}

bool parseIstanbul(const std::string& text, Report& out, std::string& error) {
    const json::Value doc = json::parse(text);
    // The repo's JSON reader is lenient (malformed input parses to Null), so validate the SHAPE.
    if (doc.kind != json::Value::Kind::Object || doc.members.empty()) {
        error = "not a JSON object of file entries — not an istanbul report";
        return false;
    }
    bool sawEntry = false;
    for (const auto& kv : doc.members) {
        const json::Value& entry = kv.second;
        if (entry.kind != json::Value::Kind::Object) continue;
        if (entry["statementMap"].kind != json::Value::Kind::Object &&
            entry["branchMap"].kind != json::Value::Kind::Object)
            continue;
        sawEntry = true;
        std::string path = entry["path"].asString();
        if (path.empty()) path = kv.first;
        File& f = out.fileFor(normalizePath(path));

        // Statements attribute to their START line: 88 of ~306 statements in a real report span lines, and
        // the end line carries no separate execution meaning.
        const json::Value& sm = entry["statementMap"];
        const json::Value& s = entry["s"];
        for (const auto& st : sm.members) {
            const int line = static_cast<int>(st.second["start"]["line"].asInt(-1));
            if (line <= 0) continue;
            Line l;
            l.hits = s[st.first].asInt(0);
            l.hitsKnown = true;
            mergeLine(f.lines[line], l);
        }

        // Branch arms attribute to the BRANCH's line, not to each arm's own start line: 65 of ~153
        // branches in a real report have arms starting on a different line, and per-arm attribution
        // renders every multi-line ternary as two partial lines. istanbul's own lcov and clover
        // reporters do the same.
        const json::Value& bm = entry["branchMap"];
        const json::Value& b = entry["b"];
        for (const auto& br : bm.members) {
            int line = static_cast<int>(br.second["line"].asInt(-1));
            if (line <= 0) line = static_cast<int>(br.second["loc"]["start"]["line"].asInt(-1));
            if (line <= 0) continue;
            const std::vector<json::Value>& counts = b[br.first].items();
            if (counts.empty()) continue;
            Line l;
            for (std::size_t i = 0; i < counts.size(); ++i) {
                BranchArm arm;
                arm.key = br.first + ":" + std::to_string(i);
                arm.taken = counts[i].asInt(0);
                arm.takenKnown = true;
                l.arms.push_back(arm);
            }
            mergeLine(f.lines[line], l);
        }
    }
    if (!sawEntry) { error = "no entries carrying statementMap/branchMap — not an istanbul report"; return false; }
    return true;
}

} // namespace

bool parseReport(const std::string& text, Format format, Report& out, std::string& error) {
    out = Report{};
    out.format = format;
    switch (format) {
        case Format::Lcov:      return parseLcov(text, out, error);
        case Format::Cobertura: return parseCobertura(text, out, error);
        case Format::Clover:    return parseClover(text, out, error);
        case Format::Istanbul:  return parseIstanbul(text, out, error);
        default:
            error = "unknown coverage report format";
            return false;
    }
}

// ---------------------------------------------------------------------------------------------------
// Writers

namespace {

struct LineTotals {
    long long linesValid = 0, linesCovered = 0;
    long long branchesValid = 0, branchesCovered = 0;
};

// Collapse a line's branch state to the (covered, total) pair every count-only format wants. Arms and
// count-only contributions add, because they describe different branches on the same line.
std::pair<int, int> branchPair(const Line& l) {
    int covered = l.countCovered, total = l.countTotal;
    for (const auto& a : l.arms) {
        ++total;
        if (a.takenKnown && a.taken > 0) ++covered;
    }
    return {covered, total};
}

bool lineCovered(const Line& l) { return !l.hitsKnown || l.hits > 0; }

LineTotals totalsFor(const Report& r) {
    LineTotals t;
    for (const auto& f : r.files) {
        for (const auto& kv : f.lines) {
            ++t.linesValid;
            if (lineCovered(kv.second) && kv.second.hitsKnown && kv.second.hits > 0) ++t.linesCovered;
            else if (!kv.second.hitsKnown) ++t.linesCovered; // executed, count unknown
            const auto bp = branchPair(kv.second);
            t.branchesValid += bp.second;
            t.branchesCovered += bp.first;
        }
    }
    return t;
}

std::string writeLcov(const Report& r, const WriteOptions& opts) {
    std::string out;
    for (const auto& f : r.files) {
        out += "TN:\n";
        out += "SF:" + f.path + "\n";
        long long lf = 0, lh = 0, brf = 0, brh = 0;
        for (const auto& kv : f.lines) {
            const Line& l = kv.second;
            ++lf;
            const long long hits = l.hitsKnown ? l.hits : 1; // unknown-but-executed
            if (hits > 0) ++lh;
            out += "DA:" + std::to_string(kv.first) + "," + std::to_string(hits) + "\n";
        }
        if (opts.branchArms) {
            // Opt-in only: BRDA is arm-KEYED, and our keys are not comparable with another target's for
            // the same `.pg` line. Safe for a single-target consumer; inflates the number otherwise.
            for (const auto& kv : f.lines) {
                int idx = 0;
                for (const auto& a : kv.second.arms) {
                    out += "BRDA:" + std::to_string(kv.first) + ",0," + std::to_string(idx++) + "," +
                           (a.takenKnown ? std::to_string(a.taken) : std::string("-")) + "\n";
                    ++brf;
                    if (a.takenKnown && a.taken > 0) ++brh;
                }
                for (int i = 0; i < kv.second.countTotal; ++i) {
                    out += "BRDA:" + std::to_string(kv.first) + ",0," + std::to_string(idx++) + "," +
                           (i < kv.second.countCovered ? "1" : "0") + "\n";
                    ++brf;
                    if (i < kv.second.countCovered) ++brh;
                }
            }
            out += "BRF:" + std::to_string(brf) + "\n";
            out += "BRH:" + std::to_string(brh) + "\n";
        }
        out += "LF:" + std::to_string(lf) + "\n";
        out += "LH:" + std::to_string(lh) + "\n";
        out += "end_of_record\n";
    }
    return out;
}

std::string writeCobertura(const Report& r, const WriteOptions&) {
    const LineTotals t = totalsFor(r);
    std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out += "<coverage line-rate=\"" + rate(t.linesCovered, t.linesValid) + "\" branch-rate=\"" +
           rate(t.branchesCovered, t.branchesValid) + "\" lines-covered=\"" + std::to_string(t.linesCovered) +
           "\" lines-valid=\"" + std::to_string(t.linesValid) + "\" branches-covered=\"" +
           std::to_string(t.branchesCovered) + "\" branches-valid=\"" + std::to_string(t.branchesValid) +
           "\" complexity=\"0\" version=\"1.9\" timestamp=\"0\">\n";
    out += "  <sources>\n    <source>.</source>\n  </sources>\n";
    out += "  <packages>\n    <package name=\"polyglot\" line-rate=\"" + rate(t.linesCovered, t.linesValid) +
           "\" branch-rate=\"" + rate(t.branchesCovered, t.branchesValid) + "\" complexity=\"0\">\n";
    out += "      <classes>\n";
    for (const auto& f : r.files) {
        long long lv = 0, lc = 0, bv = 0, bc = 0;
        for (const auto& kv : f.lines) {
            ++lv;
            if (!kv.second.hitsKnown || kv.second.hits > 0) ++lc;
            const auto bp = branchPair(kv.second);
            bv += bp.second;
            bc += bp.first;
        }
        std::string cls = f.path;
        const std::size_t slash = cls.find_last_of('/');
        if (slash != std::string::npos) cls = cls.substr(slash + 1);
        const std::size_t dot = cls.find_last_of('.');
        if (dot != std::string::npos) cls = cls.substr(0, dot);
        out += "        <class name=\"" + xmlEscape(cls) + "\" filename=\"" + xmlEscape(f.path) +
               "\" line-rate=\"" + rate(lc, lv) + "\" branch-rate=\"" + rate(bc, bv) + "\" complexity=\"0\">\n";
        out += "          <methods/>\n          <lines>\n";
        for (const auto& kv : f.lines) {
            const auto bp = branchPair(kv.second);
            // `hits` is ALWAYS written: a <line> with no hits attribute is read as NOT COVERED rather
            // than as unknown, so omitting it would silently zero the line.
            const long long hits = kv.second.hitsKnown ? kv.second.hits : 1;
            out += "            <line number=\"" + std::to_string(kv.first) + "\" hits=\"" +
                   std::to_string(hits) + "\"";
            if (bp.second > 0) {
                const int pct = static_cast<int>(std::lround(100.0 * bp.first / bp.second));
                out += " branch=\"true\" condition-coverage=\"" + std::to_string(pct) + "% (" +
                       std::to_string(bp.first) + "/" + std::to_string(bp.second) + ")\"";
            } else {
                out += " branch=\"false\"";
            }
            out += "/>\n";
        }
        out += "          </lines>\n        </class>\n";
    }
    out += "      </classes>\n    </package>\n  </packages>\n</coverage>\n";
    return out;
}

std::string writeClover(const Report& r, const WriteOptions&) {
    const LineTotals t = totalsFor(r);
    std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out += "<coverage generated=\"0\" clover=\"3.2.0\">\n";
    out += "  <project timestamp=\"0\" name=\"polyglot\">\n";
    for (const auto& f : r.files) {
        out += "    <file path=\"" + xmlEscape(f.path) + "\" name=\"" + xmlEscape(f.path) + "\">\n";
        for (const auto& kv : f.lines) {
            const auto bp = branchPair(kv.second);
            const long long hits = kv.second.hitsKnown ? kv.second.hits : 1;
            if (bp.second > 0) {
                out += "      <line num=\"" + std::to_string(kv.first) + "\" type=\"cond\" truecount=\"" +
                       std::to_string(bp.first) + "\" falsecount=\"" + std::to_string(bp.second - bp.first) +
                       "\" count=\"" + std::to_string(hits) + "\"/>\n";
            } else {
                out += "      <line num=\"" + std::to_string(kv.first) + "\" type=\"stmt\" count=\"" +
                       std::to_string(hits) + "\"/>\n";
            }
        }
        out += "    </file>\n";
    }
    out += "  </project>\n</coverage>\n";
    (void)t;
    return out;
}

std::string writeIstanbul(const Report& r, const WriteOptions& opts) {
    std::string out = "{";
    bool firstFile = true;
    for (const auto& f : r.files) {
        if (!firstFile) out += ",";
        firstFile = false;
        out += "\n  " + json::quote(f.path) + ": {";
        out += "\n    \"path\": " + json::quote(f.path) + ",";
        out += "\n    \"statementMap\": {";
        bool first = true;
        int id = 0;
        for (const auto& kv : f.lines) {
            if (!first) out += ",";
            first = false;
            const std::string k = json::quote(std::to_string(id++));
            out += "\n      " + k + ": {\"start\": {\"line\": " + std::to_string(kv.first) +
                   ", \"column\": 0}, \"end\": {\"line\": " + std::to_string(kv.first) + ", \"column\": 0}}";
        }
        out += "\n    },\n    \"s\": {";
        first = true;
        id = 0;
        for (const auto& kv : f.lines) {
            if (!first) out += ",";
            first = false;
            const long long hits = kv.second.hitsKnown ? kv.second.hits : 1;
            out += "\n      " + json::quote(std::to_string(id++)) + ": " + std::to_string(hits);
        }
        out += "\n    },\n    \"fnMap\": {},\n    \"f\": {},";
        // branchMap is arm-keyed, so it follows the same opt-in rule as lcov's BRDA.
        out += "\n    \"branchMap\": {";
        if (opts.branchArms) {
            first = true;
            id = 0;
            for (const auto& kv : f.lines) {
                const auto bp = branchPair(kv.second);
                if (bp.second <= 0) continue;
                if (!first) out += ",";
                first = false;
                out += "\n      " + json::quote(std::to_string(id++)) + ": {\"type\": \"branch\", \"line\": " +
                       std::to_string(kv.first) + ", \"locations\": [";
                for (int i = 0; i < bp.second; ++i) {
                    if (i) out += ", ";
                    out += "{\"start\": {\"line\": " + std::to_string(kv.first) +
                           ", \"column\": 0}, \"end\": {\"line\": " + std::to_string(kv.first) +
                           ", \"column\": 0}}";
                }
                out += "]}";
            }
        }
        out += "\n    },\n    \"b\": {";
        if (opts.branchArms) {
            first = true;
            id = 0;
            for (const auto& kv : f.lines) {
                const auto bp = branchPair(kv.second);
                if (bp.second <= 0) continue;
                if (!first) out += ",";
                first = false;
                out += "\n      " + json::quote(std::to_string(id++)) + ": [";
                for (int i = 0; i < bp.second; ++i) {
                    if (i) out += ", ";
                    out += (i < bp.first) ? "1" : "0";
                }
                out += "]";
            }
        }
        out += "\n    }\n  }";
    }
    out += "\n}\n";
    return out;
}

} // namespace

std::string writeReport(const Report& report, Format format, const WriteOptions& opts) {
    switch (format) {
        case Format::Lcov:      return writeLcov(report, opts);
        case Format::Cobertura: return writeCobertura(report, opts);
        case Format::Clover:    return writeClover(report, opts);
        case Format::Istanbul:  return writeIstanbul(report, opts);
        default:                return std::string();
    }
}

// ---------------------------------------------------------------------------------------------------
// Projection

std::string matchGeneratedPath(
    const std::string& reportPath,
    const std::unordered_map<std::string, std::map<int, std::vector<std::pair<std::string, int>>>>& byGenerated) {
    const std::string p = normalizePath(reportPath);
    if (byGenerated.count(p)) return p;

    // Case-INSENSITIVE, because a producer's own spelling of a path need not match the filesystem's:
    // `coverage xml` writes a `<sources>` entry whose drive-relative segments differ in case from the
    // real directory, and coverlet re-normalizes separators to the platform's. Matching case-sensitively
    // silently produces an empty report, which reads as "nothing was covered".
    auto endsWithPath = [](const std::string& full, const std::string& tail) {
        if (full.size() < tail.size()) return false;
        for (std::size_t i = 0; i < tail.size(); ++i) {
            const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(full[full.size() - tail.size() + i])));
            const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(tail[i])));
            if (a != b) return false;
        }
        return full.size() == tail.size() || full[full.size() - tail.size() - 1] == '/';
    };
    std::vector<std::string> candidates;
    for (const auto& kv : byGenerated) {
        // Either direction: a report path can be absolute, or rooted at a prefix the producer chose.
        if (endsWithPath(kv.first, p) || endsWithPath(p, kv.first)) candidates.push_back(kv.first);
    }
    // Exactly one, or nothing: an ambiguous basename must not resolve to the wrong file.
    if (candidates.size() == 1) return candidates.front();
    return std::string();
}

Report projectReport(const Report& generated, const OriginIndex& index, ProjectionStats& stats) {
    stats = ProjectionStats{};
    Report out;

    // Every `.pg` file the origin data knows, with its full mappable line set at zero. Filling this FIRST
    // is what makes an untested module report as uncovered instead of vanishing (D7).
    for (const auto& kv : index.mappedLines) {
        File& f = out.fileFor(kv.first);
        for (int line : kv.second) {
            Line l;
            l.hits = 0;
            l.hitsKnown = true;
            f.lines[line] = l;
        }
    }

    std::set<std::string> pgFilesTouched;
    for (const auto& gf : generated.files) {
        const std::string key = matchGeneratedPath(gf.path, index.byGenerated);
        if (key.empty()) { ++stats.generatedFilesUnmatched; continue; }
        ++stats.generatedFilesMatched;
        const auto& lineMap = index.byGenerated.at(key);
        for (const auto& lkv : gf.lines) {
            const auto it = lineMap.find(lkv.first);
            if (it == lineMap.end()) continue; // generated line with no origin — leaves the report
            for (const auto& origin : it->second) {
                File& pf = out.fileFor(origin.first);
                // Only lines the origin data declares mappable take data; the denominator is the map's,
                // never the report's.
                const auto mi = index.mappedLines.find(origin.first);
                if (mi != index.mappedLines.end() && !mi->second.count(origin.second)) continue;
                mergeLine(pf.lines[origin.second], lkv.second);
                pgFilesTouched.insert(origin.first);
            }
        }
    }

    for (const auto& f : out.files) {
        ++stats.pgFiles;
        if (!pgFilesTouched.count(f.path)) ++stats.pgFilesWithoutReportData;
        for (const auto& kv : f.lines) {
            ++stats.pgLinesMappable;
            if (!kv.second.hitsKnown || kv.second.hits > 0) ++stats.pgLinesCovered;
        }
    }
    std::sort(out.files.begin(), out.files.end(),
              [](const File& a, const File& b) { return a.path < b.path; });
    return out;
}

} // namespace mintplayer::polyglot::coverage
