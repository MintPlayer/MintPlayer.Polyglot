#pragma once

// In-memory tar reader for npm package tarballs (PRD issue-30 §D1 / PLAN §P30 slice 0). CLI-layer,
// dependency-free. npm publishes ustar archives with pax extended headers for long paths; every
// entry lives under a `package/` prefix (callers strip it — this reader reports paths verbatim).
//
// Trust model (design §6 "zip-slip-/path-traversal-safe extraction"): this reader REFUSES rather
// than sanitizes — an absolute path, a `..` segment, a backslash, a link/device/FIFO entry, or a
// GNU 'L' longname (npm never emits one for our payloads) each fail the whole read with a message
// naming the entry. Nothing about a refused archive is usable. Header checksums are verified, and
// per-entry sizes are bounds-checked against the buffer before any allocation.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mintplayer::polyglot::cli {

struct TarEntry {
    std::string path;   // as stored (pax `path` override or prefix+name join), forward slashes
    std::string data;   // file payload (empty for directories)
    bool isDir = false;
};

namespace detail {

// Octal-ASCII field (may be space/NUL padded/terminated). Returns -1 on garbage.
inline long long tarOctal(const char* p, std::size_t n) {
    long long v = 0;
    std::size_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == '\0')) ++i;
    bool any = false;
    for (; i < n && p[i] != ' ' && p[i] != '\0'; ++i) {
        if (p[i] < '0' || p[i] > '7') return -1;
        v = (v << 3) | (p[i] - '0');
        any = true;
    }
    return any ? v : 0;
}

inline std::string tarString(const char* p, std::size_t n) {
    std::size_t len = 0;
    while (len < n && p[len]) ++len;
    return std::string(p, len);
}

// A stored path is safe iff it is relative, forward-slash only, and never escapes: no drive
// letters, no `.`/`..` segments, no empty segments beyond a trailing '/'.
inline bool tarPathSafe(const std::string& path) {
    if (path.empty() || path[0] == '/' || path.find('\\') != std::string::npos) return false;
    if (path.size() > 1 && path[1] == ':') return false; // "C:evil" on Windows
    std::size_t b = 0;
    while (b < path.size()) {
        std::size_t e = path.find('/', b);
        if (e == std::string::npos) e = path.size();
        const std::string_view seg(path.data() + b, e - b);
        if ((seg.empty() && e != path.size()) || seg == "." || seg == "..") return false;
        if (seg.empty() && e == path.size()) break; // trailing '/' (directory entry)
        b = e + 1;
    }
    return true;
}

} // namespace detail

// Parse a whole tar buffer into entries. Returns false with a diagnostic in `err` on ANY
// malformed, unsupported, or unsafe content — partial results are never returned.
inline bool tarRead(const std::string& buf, std::vector<TarEntry>& entries, std::string& err) {
    using namespace detail;
    auto fail = [&](const std::string& what) { err = "tar: " + what; entries.clear(); return false; };

    std::string paxPath;    // pax `path` override for the NEXT entry
    bool paxPathSet = false;

    std::size_t off = 0;
    while (off + 512 <= buf.size()) {
        const char* h = buf.data() + off;

        // End marker: a zero block (spec says two; accept one then stop looking).
        bool zero = true;
        for (int i = 0; i < 512 && zero; ++i) zero = h[i] == '\0';
        if (zero) return true;

        // Verify the header checksum: unsigned byte sum with the chksum field read as spaces.
        const long long wantSum = tarOctal(h + 148, 8);
        long long sum = 0;
        for (int i = 0; i < 512; ++i) sum += (i >= 148 && i < 156) ? ' ' : static_cast<unsigned char>(h[i]);
        if (sum != wantSum) return fail("header checksum mismatch at offset " + std::to_string(off));

        const long long size = tarOctal(h + 124, 12);
        if (size < 0) return fail("non-octal size field (base-256 sizes are unsupported)");
        const std::size_t dataStart = off + 512;
        const std::size_t padded = (static_cast<std::size_t>(size) + 511) & ~std::size_t(511);
        if (dataStart + static_cast<std::size_t>(size) > buf.size()) return fail("entry overruns the archive");

        const char type = h[156];
        std::string name = tarString(h, 100);
        const std::string prefix = tarString(h + 345, 155);
        if (!prefix.empty()) name = prefix + "/" + name;

        if (type == 'x' || type == 'g') {
            // pax extended header: "<len> <key>=<value>\n" records. Only `path` matters to us;
            // a global ('g') default path would be bizarre — apply per-entry ('x') only.
            const std::string_view rec(buf.data() + dataStart, static_cast<std::size_t>(size));
            std::size_t p = 0;
            while (p < rec.size()) {
                const std::size_t sp = rec.find(' ', p);
                if (sp == std::string_view::npos) return fail("malformed pax record");
                std::size_t recLen = 0;
                for (std::size_t i = p; i < sp; ++i) {
                    if (rec[i] < '0' || rec[i] > '9') return fail("malformed pax record length");
                    recLen = recLen * 10 + static_cast<std::size_t>(rec[i] - '0');
                }
                if (recLen == 0 || p + recLen > rec.size() || rec[p + recLen - 1] != '\n')
                    return fail("pax record length out of bounds");
                const std::string_view kv = rec.substr(sp + 1, p + recLen - sp - 2); // between ' ' and '\n'
                const std::size_t eq = kv.find('=');
                if (eq == std::string_view::npos) return fail("pax record missing '='");
                if (type == 'x' && kv.substr(0, eq) == "path") {
                    paxPath = std::string(kv.substr(eq + 1));
                    paxPathSet = true;
                }
                p += recLen;
            }
        } else if (type == '0' || type == '\0' || type == '5') {
            TarEntry e;
            e.path = paxPathSet ? paxPath : name;
            paxPath.clear();
            paxPathSet = false;
            e.isDir = type == '5' || (!e.path.empty() && e.path.back() == '/');
            if (!tarPathSafe(e.path)) return fail("unsafe path '" + e.path + "' (absolute, '..', or backslash)");
            if (!e.isDir) e.data.assign(buf.data() + dataStart, static_cast<std::size_t>(size));
            else if (size != 0) return fail("directory entry with a payload");
            if (!e.path.empty() && e.path.back() == '/') e.path.pop_back();
            entries.push_back(std::move(e));
        } else {
            // Links, devices, FIFOs, GNU 'L'/'K' longnames: nothing a data-only plugin package
            // legitimately contains — refuse the archive rather than skip the entry.
            return fail(std::string("unsupported entry type '") + type + "' for '" + name + "'");
        }

        off = dataStart + padded;
    }
    // Ran off the end without a zero block: accept only a cleanly exhausted buffer (some writers
    // omit the trailer); anything else is truncation.
    return off == buf.size() ? true : fail("truncated archive (no end-of-archive marker)");
}

} // namespace mintplayer::polyglot::cli
