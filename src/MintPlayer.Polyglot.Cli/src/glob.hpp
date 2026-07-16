#pragma once

// Glob matching + output-template expansion for pgconfig `include` rules (PRD issue-30 D7 /
// PLAN §P30 slice 7b). CLI-layer, dependency-free, pure string functions — the filesystem never
// appears here, so the semantics are pinned by table tests.
//
//  * globMatch — `**` (ZERO or more whole segments; the matched span is captured as
//    %(RecursiveDir), MSBuild semantics: trailing separator, empty when `**` matched nothing or
//    the pattern has none), `*` / `?` within a segment. Paths and patterns compare with '/'
//    separators; callers normalize. At most one `**` capture (the first) — a second `**` still
//    matches but captures nothing (documented, tested).
//  * expandOutputTemplate — %(Filename), %(Directory), %(RecursiveDir), %(TargetLanguage).
//    An UNKNOWN %(...) token is an error, not passthrough — a typo'd placeholder must not
//    silently become a literal directory name. Extension tokens deliberately do not exist:
//    the CLI always appends the target's manifest fileExtension() (PRD D7).

#include <string>
#include <string_view>
#include <vector>

namespace mintplayer::polyglot::cli {

namespace detail {

// Classic single-segment wildcard match (`*` any run, `?` one char), iterative backtracking.
inline bool segmentMatch(std::string_view pat, std::string_view s) {
    std::size_t p = 0, i = 0, star = std::string_view::npos, mark = 0;
    while (i < s.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == s[i])) { ++p; ++i; }
        else if (p < pat.size() && pat[p] == '*') { star = p++; mark = i; }
        else if (star != std::string_view::npos) { p = star + 1; i = ++mark; }
        else return false;
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

inline std::vector<std::string_view> splitSegments(std::string_view path) {
    std::vector<std::string_view> segs;
    std::size_t b = 0;
    while (b <= path.size()) {
        std::size_t e = path.find('/', b);
        if (e == std::string_view::npos) e = path.size();
        if (e > b) segs.push_back(path.substr(b, e - b));
        b = e + 1;
    }
    return segs;
}

} // namespace detail

struct GlobResult {
    bool matched = false;
    std::string recursiveDir; // the span the FIRST `**` matched; "a/b/" form, "" when none/empty
};

// Match `path` (config-relative, '/'-separated, no leading "./") against `pattern`.
inline GlobResult globMatch(std::string_view pattern, std::string_view path) {
    using namespace detail;
    if (pattern.rfind("./", 0) == 0) pattern.remove_prefix(2);
    if (path.rfind("./", 0) == 0) path.remove_prefix(2);
    const auto pat = splitSegments(pattern);
    const auto segs = splitSegments(path);

    // Recursive segment matcher; `capB/capE` record the first `**` span in `segs`.
    std::size_t capB = std::string_view::npos, capE = 0;
    auto match = [&](auto&& self, std::size_t pi, std::size_t si, bool captured) -> bool {
        if (pi == pat.size()) return si == segs.size();
        if (pat[pi] == std::string_view("**")) {
            for (std::size_t take = 0; si + take <= segs.size(); ++take) {
                if (self(self, pi + 1, si + take, true)) {
                    if (!captured && capB == std::string_view::npos) { capB = si; capE = si + take; }
                    return true;
                }
            }
            return false;
        }
        if (si == segs.size()) return false;
        return segmentMatch(pat[pi], segs[si]) && self(self, pi + 1, si + 1, captured);
    };

    GlobResult r;
    r.matched = match(match, 0, 0, false);
    if (r.matched && capB != std::string_view::npos)
        for (std::size_t i = capB; i < capE; ++i) r.recursiveDir += std::string(segs[i]) + "/";
    return r;
}

// Values feeding the output template. All path-ish values are config-relative, '/'-separated.
struct TemplateValues {
    std::string filename;       // source stem, no extension
    std::string directory;      // the matched file's directory ("" for the config dir itself)
    std::string recursiveDir;   // GlobResult::recursiveDir
    std::string targetLanguage; // backend name
};

// Expand a D7 output template. Returns false with a diagnostic for an unknown %(...) token.
inline bool expandOutputTemplate(const std::string& tmpl, const TemplateValues& v,
                                 std::string& out, std::string& err) {
    out.clear();
    std::size_t i = 0;
    while (i < tmpl.size()) {
        if (tmpl[i] == '%' && i + 1 < tmpl.size() && tmpl[i + 1] == '(') {
            const std::size_t close = tmpl.find(')', i + 2);
            if (close == std::string::npos) { err = "unterminated %( in output template '" + tmpl + "'"; return false; }
            const std::string name = tmpl.substr(i + 2, close - i - 2);
            if (name == "Filename")           out += v.filename;
            else if (name == "Directory")      out += v.directory;
            else if (name == "RecursiveDir")   out += v.recursiveDir;
            else if (name == "TargetLanguage") out += v.targetLanguage;
            else {
                err = "unknown placeholder %(" + name + ") in output template '" + tmpl +
                      "' (supported: %(Filename) %(Directory) %(RecursiveDir) %(TargetLanguage); "
                      "the target extension is always appended automatically)";
                return false;
            }
            i = close + 1;
        } else {
            out += tmpl[i++];
        }
    }
    return true;
}

} // namespace mintplayer::polyglot::cli
