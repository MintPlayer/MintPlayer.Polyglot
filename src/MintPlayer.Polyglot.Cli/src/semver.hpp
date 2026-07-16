#pragma once

// The semver subset the plugin resolver speaks (PRD issue-30 §D4 / PLAN §P30 slice 1) — exactly
// the spec forms `pgconfig.json` dependencies accept:
//
//     1.2.3         exact
//     ^1.2.3        caret: up to (excluding) the next left-most-non-zero bump
//     ~1.2.3        tilde: patch drift only (>=1.2.3 <1.3.0)
//     >=1.2.3       open upper bound
//     latest        the registry's `dist-tags.latest`
//
// Resolution is node-semver `maxSatisfying`: the HIGHEST version satisfying the range, with the
// standard prerelease rule — a prerelease version satisfies a range only when the range's anchor
// names a prerelease of the SAME [major.minor.patch] tuple; otherwise prereleases are invisible.
// Partial versions ("^1.2", "1.x", ranges with spaces) are deliberately refused with a clear
// message — a small, honest grammar beats a half-right big one; widen it when a real plugin
// needs it. Build metadata ("+sha") is parsed and ignored per spec (no precedence weight).

#include <cctype>
#include <optional>
#include <string>
#include <vector>

namespace mintplayer::polyglot::cli {

struct SemVer {
    long long major = 0, minor = 0, patch = 0;
    std::vector<std::string> prerelease; // dot-split identifiers; empty = a release version

    bool isPrerelease() const { return !prerelease.empty(); }
    bool sameCore(const SemVer& o) const { return major == o.major && minor == o.minor && patch == o.patch; }
};

// Parse "1.2.3", "1.2.3-rc.1", "1.2.3+build" (with optional leading 'v'). Full three-part
// versions only — this is a version parser, not a range parser.
inline std::optional<SemVer> parseSemVer(const std::string& s) {
    SemVer v;
    std::size_t i = 0;
    if (i < s.size() && (s[i] == 'v' || s[i] == 'V')) ++i;
    auto num = [&](long long& out) {
        if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) return false;
        // no leading zeros per spec ("01" is invalid) — but be tolerant reading, npm never emits them
        out = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            out = out * 10 + (s[i] - '0');
            if (out > 1'000'000'000'000LL) return false; // absurd — refuse rather than overflow
            ++i;
        }
        return true;
    };
    if (!num(v.major) || i >= s.size() || s[i] != '.') return std::nullopt;
    ++i;
    if (!num(v.minor) || i >= s.size() || s[i] != '.') return std::nullopt;
    ++i;
    if (!num(v.patch)) return std::nullopt;
    if (i < s.size() && s[i] == '-') { // prerelease identifiers
        ++i;
        std::string id;
        for (; i < s.size() && s[i] != '+'; ++i) {
            if (s[i] == '.') {
                if (id.empty()) return std::nullopt;
                v.prerelease.push_back(id);
                id.clear();
            } else if (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '-') {
                id += s[i];
            } else {
                return std::nullopt;
            }
        }
        if (id.empty()) return std::nullopt;
        v.prerelease.push_back(id);
    }
    if (i < s.size() && s[i] == '+') return v; // build metadata: valid, ignored
    return i == s.size() ? std::optional<SemVer>(v) : std::nullopt;
}

// Semver precedence (spec §11): compare core numerically; a prerelease sorts BELOW its release;
// prerelease identifiers compare numerically when both numeric, else lexically, numeric < alpha.
inline int compareSemVer(const SemVer& a, const SemVer& b) {
    auto cmp = [](long long x, long long y) { return x < y ? -1 : x > y ? 1 : 0; };
    if (int c = cmp(a.major, b.major)) return c;
    if (int c = cmp(a.minor, b.minor)) return c;
    if (int c = cmp(a.patch, b.patch)) return c;
    if (a.prerelease.empty() != b.prerelease.empty()) return a.prerelease.empty() ? 1 : -1;
    for (std::size_t i = 0; i < a.prerelease.size() && i < b.prerelease.size(); ++i) {
        const std::string& x = a.prerelease[i];
        const std::string& y = b.prerelease[i];
        const bool xn = x.find_first_not_of("0123456789") == std::string::npos;
        const bool yn = y.find_first_not_of("0123456789") == std::string::npos;
        if (xn && yn) { if (int c = cmp(std::stoll(x), std::stoll(y))) return c; }
        else if (xn != yn) return xn ? -1 : 1; // numeric identifiers sort below alphanumeric
        else if (x != y) return x < y ? -1 : 1;
    }
    return cmp(static_cast<long long>(a.prerelease.size()), static_cast<long long>(b.prerelease.size()));
}

struct VersionRange {
    enum class Kind { Exact, Caret, Tilde, Gte, Latest } kind = Kind::Exact;
    SemVer anchor; // meaningless for Latest
};

// Parse a dependency spec's version form. `file:` specs never reach here. Returns nullopt with a
// human diagnostic in `err` for anything outside the published grammar.
inline std::optional<VersionRange> parseRange(const std::string& spec, std::string& err) {
    VersionRange r;
    std::string body = spec;
    if (spec == "latest") { r.kind = VersionRange::Kind::Latest; return r; }
    if (spec.rfind("^", 0) == 0)       { r.kind = VersionRange::Kind::Caret; body = spec.substr(1); }
    else if (spec.rfind("~", 0) == 0)  { r.kind = VersionRange::Kind::Tilde; body = spec.substr(1); }
    else if (spec.rfind(">=", 0) == 0) { r.kind = VersionRange::Kind::Gte;   body = spec.substr(2); }
    else                               { r.kind = VersionRange::Kind::Exact; }
    if (auto v = parseSemVer(body)) { r.anchor = *v; return r; }
    err = "unsupported version spec '" + spec +
          "' (supported: exact 1.2.3, ^1.2.3, ~1.2.3, >=1.2.3, latest, file:<dir>)";
    return std::nullopt;
}

// The caret/tilde exclusive upper bound (node-semver desugar).
inline SemVer rangeUpperExclusive(const VersionRange& r) {
    SemVer u;
    if (r.kind == VersionRange::Kind::Caret) {
        if (r.anchor.major > 0)      u = {r.anchor.major + 1, 0, 0};
        else if (r.anchor.minor > 0) u = {0, r.anchor.minor + 1, 0};
        else                         u = {0, 0, r.anchor.patch + 1};
    } else { // Tilde
        u = {r.anchor.major, r.anchor.minor + 1, 0};
    }
    return u;
}

inline bool satisfies(const SemVer& v, const VersionRange& r) {
    if (r.kind == VersionRange::Kind::Latest) return true; // resolved via dist-tags, not filtering
    // Prerelease rule: only visible when the anchor is a prerelease of the same core tuple.
    if (v.isPrerelease() && !(r.anchor.isPrerelease() && v.sameCore(r.anchor))) return false;
    switch (r.kind) {
        case VersionRange::Kind::Exact:
            return compareSemVer(v, r.anchor) == 0;
        case VersionRange::Kind::Gte:
            return compareSemVer(v, r.anchor) >= 0;
        case VersionRange::Kind::Caret:
        case VersionRange::Kind::Tilde: {
            if (compareSemVer(v, r.anchor) < 0) return false;
            const SemVer upper = rangeUpperExclusive(r);
            // The bound is "<upper-0": any prerelease of `upper` itself is already excluded by
            // the prerelease rule above, so a plain exclusive compare on the core suffices.
            if (v.major != upper.major) return v.major < upper.major;
            if (v.minor != upper.minor) return v.minor < upper.minor;
            return v.patch < upper.patch;
        }
        case VersionRange::Kind::Latest:
            return true;
    }
    return false;
}

// node-semver `maxSatisfying` over a version list. For `latest`, the dist-tag decides (it must
// name a version that exists in the list). Returns the winning version STRING as published —
// the caller keys caches/lockfiles by it verbatim.
inline std::optional<std::string> maxSatisfying(const std::vector<std::string>& versions,
                                                const VersionRange& range,
                                                const std::string& latestTag) {
    if (range.kind == VersionRange::Kind::Latest) {
        for (const auto& s : versions)
            if (s == latestTag) return s;
        return std::nullopt;
    }
    std::optional<std::string> best;
    std::optional<SemVer> bestV;
    for (const auto& s : versions) {
        const auto v = parseSemVer(s);
        if (!v || !satisfies(*v, range)) continue;
        if (!bestV || compareSemVer(*v, *bestV) > 0) { best = s; bestV = v; }
    }
    return best;
}

} // namespace mintplayer::polyglot::cli
