#pragma once

// Hashing + SRI primitives for the plugin auto-download pipeline (PRD issue-30 §D1-D3 / PLAN §P30
// slice 0). Core stays IO-free and download-free — everything here is CLI-layer, and deliberately
// dependency-free: one straightforward FIPS 180-4 implementation used identically on all three
// platforms (no BCrypt/CommonCrypto #ifdef surface; inputs are KB-scale plugin tarballs, so
// throughput is irrelevant).
//
//  * sha512 / sha1      — the two digests the npm registry publishes (`dist.integrity` is SRI
//                         sha512 today; `dist.shasum` is the sha1 legacy fallback).
//  * base64Encode / hex — digest renderings: SRI carries standard RFC 4648 base64 (with padding),
//                         the lockfile's manifest pin and shasum compare in lowercase hex.
//  * verifySri          — checks raw bytes against an SRI string ("sha512-<base64>"); unknown
//                         algorithms are refused loudly, never skipped (§6 trust model: an
//                         integrity we cannot check is a failure, not a pass).
//
// The K/H constant tables were generated with exact BigInt integer roots (frac(cbrt/sqrt(prime))
// * 2^64) and cross-checked against the FIPS 180-4 endpoints; the unit tests pin the NIST test
// vectors, so a corrupted table cannot pass the suite.

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace mintplayer::polyglot::cli {

namespace detail {

inline constexpr std::uint64_t kSha512K[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

inline constexpr std::uint64_t kSha512H[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
};

inline std::uint64_t rotr64(std::uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }
inline std::uint32_t rotl32(std::uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

} // namespace detail

// ---- SHA-512 (FIPS 180-4) ------------------------------------------------------------------

inline std::array<std::uint8_t, 64> sha512(const void* data, std::size_t len) {
    using namespace detail;
    std::uint64_t h[8];
    std::memcpy(h, kSha512H, sizeof h);

    // Message + padding: 0x80, zeros, 128-bit big-endian bit length, to a 128-byte boundary.
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint8_t block[128];
    const std::uint64_t bitLenLo = static_cast<std::uint64_t>(len) << 3;
    const std::uint64_t bitLenHi = static_cast<std::uint64_t>(len) >> 61;

    auto compress = [&](const std::uint8_t* p) {
        std::uint64_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = 0;
            for (int b = 0; b < 8; ++b) w[i] = (w[i] << 8) | p[i * 8 + b];
        }
        for (int i = 16; i < 80; ++i) {
            const std::uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
            const std::uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 80; ++i) {
            const std::uint64_t S1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
            const std::uint64_t ch = (e & f) ^ (~e & g);
            const std::uint64_t t1 = hh + S1 + ch + kSha512K[i] + w[i];
            const std::uint64_t S0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
            const std::uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint64_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    };

    std::size_t off = 0;
    for (; off + 128 <= len; off += 128) compress(bytes + off);

    const std::size_t rem = len - off;
    std::memset(block, 0, sizeof block);
    if (rem) std::memcpy(block, bytes + off, rem);
    block[rem] = 0x80;
    if (rem >= 112) { compress(block); std::memset(block, 0, sizeof block); }
    for (int b = 0; b < 8; ++b) block[112 + b] = static_cast<std::uint8_t>(bitLenHi >> (56 - 8 * b));
    for (int b = 0; b < 8; ++b) block[120 + b] = static_cast<std::uint8_t>(bitLenLo >> (56 - 8 * b));
    compress(block);

    std::array<std::uint8_t, 64> out{};
    for (int i = 0; i < 8; ++i)
        for (int b = 0; b < 8; ++b) out[i * 8 + b] = static_cast<std::uint8_t>(h[i] >> (56 - 8 * b));
    return out;
}

inline std::array<std::uint8_t, 64> sha512(const std::string& s) { return sha512(s.data(), s.size()); }

// ---- SHA-1 (legacy `dist.shasum` only — never a trust root when an SRI is present) -----------

inline std::array<std::uint8_t, 20> sha1(const void* data, std::size_t len) {
    using detail::rotl32;
    std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint8_t block[64];
    const std::uint64_t bitLen = static_cast<std::uint64_t>(len) << 3;

    auto compress = [&](const std::uint8_t* p) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint32_t(p[i * 4]) << 24) | (std::uint32_t(p[i * 4 + 1]) << 16) |
                   (std::uint32_t(p[i * 4 + 2]) << 8) | std::uint32_t(p[i * 4 + 3]);
        for (int i = 16; i < 80; ++i) w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                    k = 0xCA62C1D6u; }
            const std::uint32_t t = rotl32(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl32(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    };

    std::size_t off = 0;
    for (; off + 64 <= len; off += 64) compress(bytes + off);
    const std::size_t rem = len - off;
    std::memset(block, 0, sizeof block);
    if (rem) std::memcpy(block, bytes + off, rem);
    block[rem] = 0x80;
    if (rem >= 56) { compress(block); std::memset(block, 0, sizeof block); }
    for (int b = 0; b < 8; ++b) block[56 + b] = static_cast<std::uint8_t>(bitLen >> (56 - 8 * b));
    compress(block);

    std::array<std::uint8_t, 20> out{};
    for (int i = 0; i < 5; ++i)
        for (int b = 0; b < 4; ++b) out[i * 4 + b] = static_cast<std::uint8_t>(h[i] >> (24 - 8 * b));
    return out;
}

inline std::array<std::uint8_t, 20> sha1(const std::string& s) { return sha1(s.data(), s.size()); }

// ---- Renderings ------------------------------------------------------------------------------

template <std::size_t N>
std::string toHex(const std::array<std::uint8_t, N>& digest) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(N * 2);
    for (std::uint8_t b : digest) { out += kHex[b >> 4]; out += kHex[b & 0xF]; }
    return out;
}

// Standard RFC 4648 base64 with '=' padding — the alphabet SRI uses (not base64url).
inline std::string base64Encode(const std::uint8_t* data, std::size_t len) {
    static const char* kAlpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        const std::uint32_t v = (std::uint32_t(data[i]) << 16) | (std::uint32_t(data[i + 1]) << 8) | data[i + 2];
        out += kAlpha[(v >> 18) & 63]; out += kAlpha[(v >> 12) & 63];
        out += kAlpha[(v >> 6) & 63];  out += kAlpha[v & 63];
    }
    if (i + 1 == len) {
        const std::uint32_t v = std::uint32_t(data[i]) << 16;
        out += kAlpha[(v >> 18) & 63]; out += kAlpha[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == len) {
        const std::uint32_t v = (std::uint32_t(data[i]) << 16) | (std::uint32_t(data[i + 1]) << 8);
        out += kAlpha[(v >> 18) & 63]; out += kAlpha[(v >> 12) & 63]; out += kAlpha[(v >> 6) & 63]; out += '=';
    }
    return out;
}

template <std::size_t N>
std::string base64Encode(const std::array<std::uint8_t, N>& digest) { return base64Encode(digest.data(), N); }

// The SRI string npm publishes for a payload: "sha512-<base64>".
inline std::string sriSha512(const std::string& bytes) { return "sha512-" + base64Encode(sha512(bytes)); }

// Verify `bytes` against an SRI integrity string. Only the algorithms npm actually publishes are
// accepted; anything else fails with a message naming the algorithm — an unverifiable integrity
// must never pass silently.
inline bool verifySri(const std::string& integrity, const std::string& bytes, std::string& err) {
    const std::size_t dash = integrity.find('-');
    if (dash == std::string::npos) { err = "malformed integrity '" + integrity + "' (want '<algorithm>-<base64>')"; return false; }
    const std::string algo = integrity.substr(0, dash);
    const std::string want = integrity.substr(dash + 1);
    std::string got;
    if (algo == "sha512")      got = base64Encode(sha512(bytes));
    else if (algo == "sha1")   got = base64Encode(sha1(bytes));
    else { err = "unsupported integrity algorithm '" + algo + "' (supported: sha512, sha1)"; return false; }
    if (got != want) { err = "integrity mismatch: expected " + algo + "-" + want + ", got " + algo + "-" + got; return false; }
    return true;
}

} // namespace mintplayer::polyglot::cli
