#pragma once

// Gzip/DEFLATE decoder for the plugin auto-download pipeline (PRD issue-30 §D1 / PLAN §P30 slice 0).
// Decode-only, CLI-layer, dependency-free: npm plugin tarballs are KB-scale gzipped tars, so a
// straightforward bit-by-bit canonical-Huffman decoder (RFC 1951) behind a strict gzip framing
// parser (RFC 1952) is all the job needs — no zlib, no streaming, no compressor.
//
// Trust-model guarantees (design §6 "bounded/validated parsing"):
//  * `maxOut` hard-caps the inflated size BEFORE any allocation trusts attacker-controlled counts
//    (the gzip ISIZE trailer is a hint for reservation, never a limit).
//  * The CRC32 + ISIZE trailer are verified — a truncated or corrupted stream is an error, not a
//    best-effort result.
//  * Every Huffman table is validated (oversubscribed/incomplete codes refused), every
//    length/distance is bounds-checked against the window produced so far.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mintplayer::polyglot::cli {

// ---- CRC-32 (IEEE, reflected 0xEDB88320) — the gzip trailer checksum ------------------------

inline std::uint32_t crc32(const void* data, std::size_t len, std::uint32_t seed = 0) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = ~seed;
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < len; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

namespace detail {

// LSB-first bit reader over a byte buffer (DEFLATE's bit order).
struct BitReader {
    const std::uint8_t* data;
    std::size_t size;
    std::size_t pos = 0;   // next byte
    std::uint32_t bitBuf = 0;
    int bitCount = 0;
    bool truncated = false;

    std::uint32_t bits(int n) { // n <= 16
        while (bitCount < n) {
            if (pos >= size) { truncated = true; return 0; }
            bitBuf |= std::uint32_t(data[pos++]) << bitCount;
            bitCount += 8;
        }
        const std::uint32_t v = bitBuf & ((1u << n) - 1);
        bitBuf >>= n;
        bitCount -= n;
        return v;
    }
    void alignToByte() { bitBuf = 0; bitCount = 0; }
};

// A canonical Huffman decoder built from code lengths (RFC 1951 §3.2.2): per-length counts +
// first-code offsets, decoded bit by bit. Rejects oversubscribed tables; incomplete tables are
// permitted only in the degenerate one-code form the spec allows for distance trees.
struct Huffman {
    std::array<std::uint16_t, 16> count{}; // codes per bit length 0..15
    std::vector<std::uint16_t> symbols;    // symbols ordered by (length, symbol)

    // Returns false on an invalid (oversubscribed) code set.
    bool build(const std::uint8_t* lengths, std::size_t n) {
        count.fill(0);
        for (std::size_t i = 0; i < n; ++i) ++count[lengths[i]];
        if (count[0] == n) return false; // no codes at all
        int left = 1; // codes available; a valid tree never goes negative
        for (int len = 1; len <= 15; ++len) {
            left <<= 1;
            left -= count[len];
            if (left < 0) return false; // oversubscribed
        }
        std::array<std::uint16_t, 16> offs{};
        for (int len = 1; len < 15; ++len) offs[len + 1] = static_cast<std::uint16_t>(offs[len] + count[len]);
        symbols.assign(n, 0);
        std::vector<std::uint16_t> next(offs.begin(), offs.end());
        for (std::size_t sym = 0; sym < n; ++sym)
            if (lengths[sym]) symbols[next[lengths[sym]]++] = static_cast<std::uint16_t>(sym);
        return true;
    }

    // Decode one symbol; -1 on invalid/truncated input.
    int decode(BitReader& br) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; ++len) {
            code |= static_cast<int>(br.bits(1));
            if (br.truncated) return -1;
            const int cnt = count[len];
            if (code - first < cnt) return symbols[index + (code - first)];
            index += cnt;
            first = (first + cnt) << 1;
            code <<= 1;
        }
        return -1;
    }
};

inline const std::uint16_t kLenBase[29]  = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                            35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
inline const std::uint8_t  kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                            3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
inline const std::uint16_t kDistBase[30]  = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                             257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                             8193, 12289, 16385, 24577};
inline const std::uint8_t  kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                             7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

} // namespace detail

// Inflate a raw DEFLATE stream (RFC 1951) into `out`, appending. `maxOut` bounds the total output
// size (zip-bomb guard). Returns false with a diagnostic in `err`.
inline bool inflate(const std::uint8_t* data, std::size_t size, std::string& out,
                    std::size_t maxOut, std::string& err) {
    using namespace detail;
    BitReader br{data, size};

    auto fail = [&](const char* what) { err = std::string("inflate: ") + what; return false; };
    auto emit = [&](std::uint8_t b) {
        if (out.size() >= maxOut) return false;
        out.push_back(static_cast<char>(b));
        return true;
    };

    for (;;) {
        const std::uint32_t bfinal = br.bits(1);
        const std::uint32_t btype = br.bits(2);
        if (br.truncated) return fail("truncated block header");

        if (btype == 0) { // stored
            br.alignToByte();
            if (br.pos + 4 > br.size) return fail("truncated stored-block header");
            const std::uint32_t len = br.data[br.pos] | (std::uint32_t(br.data[br.pos + 1]) << 8);
            const std::uint32_t nlen = br.data[br.pos + 2] | (std::uint32_t(br.data[br.pos + 3]) << 8);
            br.pos += 4;
            if ((len ^ 0xFFFFu) != nlen) return fail("stored-block LEN/NLEN mismatch");
            if (br.pos + len > br.size) return fail("truncated stored block");
            if (out.size() + len > maxOut) return fail("output exceeds the size cap");
            out.append(reinterpret_cast<const char*>(br.data + br.pos), len);
            br.pos += len;
        } else if (btype == 1 || btype == 2) {
            Huffman lit, dist;
            if (btype == 1) { // fixed tables (RFC 1951 §3.2.6)
                std::uint8_t litLen[288];
                for (int i = 0; i < 144; ++i) litLen[i] = 8;
                for (int i = 144; i < 256; ++i) litLen[i] = 9;
                for (int i = 256; i < 280; ++i) litLen[i] = 7;
                for (int i = 280; i < 288; ++i) litLen[i] = 8;
                std::uint8_t distLen[30];
                for (int i = 0; i < 30; ++i) distLen[i] = 5;
                if (!lit.build(litLen, 288) || !dist.build(distLen, 30)) return fail("bad fixed tables");
            } else { // dynamic tables (§3.2.7)
                const std::uint32_t hlit = br.bits(5) + 257;
                const std::uint32_t hdist = br.bits(5) + 1;
                const std::uint32_t hclen = br.bits(4) + 4;
                if (br.truncated) return fail("truncated dynamic-table header");
                if (hlit > 286 || hdist > 30) return fail("dynamic table counts out of range");
                static const std::uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                                        11, 4, 12, 3, 13, 2, 14, 1, 15};
                std::uint8_t clLen[19] = {};
                for (std::uint32_t i = 0; i < hclen; ++i) clLen[kOrder[i]] = static_cast<std::uint8_t>(br.bits(3));
                if (br.truncated) return fail("truncated code-length lengths");
                Huffman cl;
                if (!cl.build(clLen, 19)) return fail("bad code-length table");
                std::uint8_t lens[286 + 30] = {};
                std::uint32_t n = 0;
                while (n < hlit + hdist) {
                    const int sym = cl.decode(br);
                    if (sym < 0) return fail("bad code-length symbol");
                    if (sym < 16) { lens[n++] = static_cast<std::uint8_t>(sym); continue; }
                    std::uint32_t repeat; std::uint8_t value = 0;
                    if (sym == 16) {
                        if (n == 0) return fail("repeat with no previous length");
                        value = lens[n - 1];
                        repeat = 3 + br.bits(2);
                    } else if (sym == 17) repeat = 3 + br.bits(3);
                    else                  repeat = 11 + br.bits(7);
                    if (br.truncated) return fail("truncated repeat count");
                    if (n + repeat > hlit + hdist) return fail("repeat overflows the length table");
                    while (repeat--) lens[n++] = value;
                }
                if (lens[256] == 0) return fail("dynamic table lacks an end-of-block code");
                if (!lit.build(lens, hlit)) return fail("bad literal/length table");
                // A literal-only dynamic block may declare zero distance codes (RFC 1951 permits
                // it); the table is then absent and any match symbol below is the error instead.
                bool anyDist = false;
                for (std::uint32_t i = 0; i < hdist; ++i) anyDist |= lens[hlit + i] != 0;
                if (anyDist && !dist.build(lens + hlit, hdist)) return fail("bad distance table");
                if (!anyDist) dist.count.fill(0); // decode() then always returns -1
            }

            for (;;) { // decode symbols until end-of-block
                const int sym = lit.decode(br);
                if (sym < 0) return fail("bad literal/length symbol");
                if (sym < 256) {
                    if (!emit(static_cast<std::uint8_t>(sym))) return fail("output exceeds the size cap");
                } else if (sym == 256) {
                    break;
                } else {
                    if (sym > 285) return fail("length symbol out of range");
                    const int li = sym - 257;
                    const std::uint32_t len = kLenBase[li] + br.bits(kLenExtra[li]);
                    const int dsym = dist.decode(br);
                    if (dsym < 0 || dsym > 29) return fail("bad distance symbol");
                    const std::uint32_t d = kDistBase[dsym] + br.bits(kDistExtra[dsym]);
                    if (br.truncated) return fail("truncated match");
                    if (d > out.size()) return fail("distance reaches before the output start");
                    if (out.size() + len > maxOut) return fail("output exceeds the size cap");
                    for (std::uint32_t i = 0; i < len; ++i) out.push_back(out[out.size() - d]);
                }
            }
        } else {
            return fail("reserved block type");
        }

        if (bfinal) return true;
    }
}

// Decode a SINGLE-member gzip stream (RFC 1952): framing + inflate + CRC32/ISIZE trailer
// verification. npm tarballs are single-member; the rarely-used FHCRC/FEXTRA/FNAME/FCOMMENT
// header fields are handled, FTEXT is ignored (advisory), trailing extra members are refused
// implicitly by the ISIZE/CRC check anchored at the stream end.
inline bool gunzip(const std::uint8_t* data, std::size_t size, std::string& out,
                   std::size_t maxOut, std::string& err) {
    auto fail = [&](const std::string& what) { err = "gunzip: " + what; return false; };
    if (size < 18) return fail("too short to be a gzip stream");
    if (data[0] != 0x1F || data[1] != 0x8B) return fail("bad magic (not gzip)");
    if (data[2] != 8) return fail("unsupported compression method " + std::to_string(data[2]));
    const std::uint8_t flg = data[3];
    if (flg & 0xE0) return fail("reserved FLG bits set");
    std::size_t p = 10; // fixed header
    if (flg & 4) {      // FEXTRA
        if (p + 2 > size) return fail("truncated FEXTRA");
        const std::size_t xlen = data[p] | (std::size_t(data[p + 1]) << 8);
        p += 2 + xlen;
        if (p > size) return fail("truncated FEXTRA payload");
    }
    for (const int bit : {8, 16}) { // FNAME, FCOMMENT: nul-terminated
        if (flg & bit) {
            while (p < size && data[p]) ++p;
            if (p++ >= size) return fail("truncated FNAME/FCOMMENT");
        }
    }
    if (flg & 2) { // FHCRC: crc16 of the header so far
        if (p + 2 > size) return fail("truncated FHCRC");
        const std::uint16_t want = static_cast<std::uint16_t>(data[p] | (data[p + 1] << 8));
        if (static_cast<std::uint16_t>(crc32(data, p) & 0xFFFF) != want) return fail("header CRC mismatch");
        p += 2;
    }
    if (size - p < 8) return fail("truncated deflate payload");

    const std::size_t before = out.size();
    if (!inflate(data + p, size - p - 8, out, maxOut, err)) return false;

    const std::uint8_t* tr = data + size - 8;
    const std::uint32_t wantCrc = tr[0] | (std::uint32_t(tr[1]) << 8) | (std::uint32_t(tr[2]) << 16) | (std::uint32_t(tr[3]) << 24);
    const std::uint32_t wantSize = tr[4] | (std::uint32_t(tr[5]) << 8) | (std::uint32_t(tr[6]) << 16) | (std::uint32_t(tr[7]) << 24);
    const std::string_view produced(out.data() + before, out.size() - before);
    if (static_cast<std::uint32_t>(produced.size()) != wantSize) return fail("ISIZE mismatch (truncated or corrupt stream)");
    if (crc32(produced.data(), produced.size()) != wantCrc) return fail("payload CRC mismatch");
    return true;
}

inline bool gunzip(const std::string& bytes, std::string& out, std::size_t maxOut, std::string& err) {
    return gunzip(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), out, maxOut, err);
}

} // namespace mintplayer::polyglot::cli
