#pragma once

#include <cstddef>
#include <string>

// UTF-8 primitives shared by the lexer (decoding `.pg` literals), the literal emitter (re-escaping them per
// target, P40 A1) and the `.pg` formatter (printing them back). Header-only: three small pure functions.

namespace mintplayer::polyglot::unicode {

// Decode one UTF-8 sequence at `s[i]`, advancing `i` past it. Returns false (advancing one byte) on an
// ill-formed sequence: a stray continuation byte, a truncated sequence, an overlong form, an encoded
// surrogate (U+D800..U+DFFF) or a value above U+10FFFF.
inline bool decodeUtf8(const std::string& s, std::size_t& i, char32_t& cp) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80) { cp = b0; ++i; return true; }
    int len = 0;
    char32_t min = 0;
    if ((b0 & 0xE0) == 0xC0)      { len = 2; cp = b0 & 0x1F; min = 0x80; }
    else if ((b0 & 0xF0) == 0xE0) { len = 3; cp = b0 & 0x0F; min = 0x800; }
    else if ((b0 & 0xF8) == 0xF0) { len = 4; cp = b0 & 0x07; min = 0x10000; }
    else { ++i; return false; }
    if (i + len > s.size()) { ++i; return false; }
    for (int k = 1; k < len; ++k) {
        const auto b = static_cast<unsigned char>(s[i + k]);
        if ((b & 0xC0) != 0x80) { ++i; return false; }
        cp = (cp << 6) | (b & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { ++i; return false; }
    i += len;
    return true;
}

inline void appendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// P40 A1: the ONE language-agnostic decision of which characters a generated literal must spell as an escape
// rather than write raw — C0 controls, DEL, C1 controls (incl. U+0085 NEL, CS1010 raw in C#), the line and
// paragraph separators U+2028/U+2029 (CS1010 in C#, line terminators in older JS) and U+FEFF (an invisible
// BOM). HOW each is spelled is the plugin's `literals` data; printable non-ASCII passes through as UTF-8.
inline bool mustEscape(char32_t cp) {
    return cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F) || cp == 0x2028 || cp == 0x2029 ||
           cp == 0xFEFF;
}

} // namespace mintplayer::polyglot::unicode
