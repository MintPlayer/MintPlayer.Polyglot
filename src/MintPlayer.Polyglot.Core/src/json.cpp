#include "mintplayer/polyglot/json.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace mintplayer::polyglot::json {

const Value Value::kNull{};

bool Value::has(const std::string& key) const {
    if (kind != Kind::Object) return false;
    for (const auto& m : members) if (m.first == key) return true;
    return false;
}

const Value& Value::operator[](const std::string& key) const {
    if (kind == Kind::Object)
        for (const auto& m : members) if (m.first == key) return m.second;
    return kNull;
}

namespace {

// Append a Unicode code point to `out` as UTF-8.
void appendUtf8(std::string& out, unsigned cp) {
    if (cp <= 0x7F) {
        out += static_cast<char>(cp);
    } else if (cp <= 0x7FF) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
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

struct Parser {
    const std::string& s;
    std::size_t i = 0;

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }

    std::string error; // first malformation found ("" = none); see parse(text, error)

    void fail(std::size_t at, const std::string& what) {
        if (error.empty()) error = what + " at byte offset " + std::to_string(at);
    }

    unsigned hex4() {
        unsigned v = 0;
        const std::size_t at = i;
        for (int k = 0; k < 4; ++k) {
            if (i >= s.size()) { fail(at, "truncated \\u escape"); return v; }
            char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else fail(at, "invalid \\u escape (expected 4 hex digits)");
        }
        return v;
    }

    std::string parseString() {
        std::string out;
        const std::size_t open = i;
        ++i; // opening quote
        bool closed = false;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') { closed = true; break; }
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) break;
            char e = s[i++];
            switch (e) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    const std::size_t at = i - 2;
                    unsigned cp = hex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        i += 2; // consume the "\u" of the low surrogate
                        unsigned lo = hex4();
                        if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else fail(at, "invalid \\u surrogate pair");
                    } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                        // P40 review r4: a lone surrogate has no UTF-8 form; the result would be invalid UTF-8.
                        fail(at, "lone \\u surrogate in a JSON string");
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default:
                    // JSON allows only \" \\ \/ \b \f \n \r \t \uXXXX. An unknown escape is an error (it used to
                    // drop the backslash SILENTLY: pgconfig "Libs\Alpha" read as "LibsAlpha"); the lenient
                    // result keeps both characters, never a quietly different string.
                    fail(i - 2, std::string("invalid escape '\\") + e + "' in a JSON string (a Windows path "
                                "needs '\\\\' or '/')");
                    out += '\\';
                    out += e;
                    break;
            }
        }
        if (!closed) fail(open, "unterminated string");
        return out;
    }

    // A bare literal (true/false/null) must be spelled out in full: `tru` or `nul` is a malformed document.
    bool literal(const char* word) {
        const std::size_t n = std::char_traits<char>::length(word);
        if (s.compare(i, n, word) != 0) { fail(i, "invalid literal (expected '" + std::string(word) + "')"); i = s.size(); return false; }
        i += n;
        return true;
    }

    // Stamps every parsed value with the offset of its first character. Wrapping the dispatch keeps
    // the provenance in ONE place — each parseX() below stays unaware of it, so a new value kind
    // cannot forget to record its position.
    Value parseValue() {
        ws();
        std::size_t start = i;
        Value v = parseValueInner();
        v.offset = start;
        return v;
    }

    Value parseValueInner() {
        if (i >= s.size()) { fail(i, "unexpected end of input (expected a value)"); return {}; }
        char c = s[i];
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') { Value v; v.kind = Value::Kind::String; v.str = parseString(); return v; }
        if (c == 't') { Value v; if (literal("true"))  { v.kind = Value::Kind::Bool; v.boolean = true;  } return v; }
        if (c == 'f') { Value v; if (literal("false")) { v.kind = Value::Kind::Bool; v.boolean = false; } return v; }
        if (c == 'n') { literal("null"); return {}; }
        return parseNumber();
    }

    static bool isDigit(char c) { return c >= '0' && c <= '9'; }

    // The JSON number grammar: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)? — anything else (an empty value,
    // `+1`, `01`, `1.`, a stray character) is a malformed document, never a silent 0.
    Value parseNumber() {
        std::size_t start = i;
        while (i < s.size()) {
            char c = s[i];
            if (isDigit(c) || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') ++i;
            else break;
        }
        Value v;
        v.kind = Value::Kind::Number;
        v.str = s.substr(start, i - start);
        v.num = std::strtod(v.str.c_str(), nullptr);
        const std::string& t = v.str;
        std::size_t k = 0;
        bool ok = !t.empty();
        if (ok && t[k] == '-') ++k;
        if (ok && k < t.size() && t[k] == '0') ++k;
        else if (ok && k < t.size() && isDigit(t[k])) { while (k < t.size() && isDigit(t[k])) ++k; }
        else ok = false;
        if (ok && k < t.size() && t[k] == '.') {
            ++k;
            if (k >= t.size() || !isDigit(t[k])) ok = false;
            while (k < t.size() && isDigit(t[k])) ++k;
        }
        if (ok && k < t.size() && (t[k] == 'e' || t[k] == 'E')) {
            ++k;
            if (k < t.size() && (t[k] == '+' || t[k] == '-')) ++k;
            if (k >= t.size() || !isDigit(t[k])) ok = false;
            while (k < t.size() && isDigit(t[k])) ++k;
        }
        if (!ok || k != t.size()) {
            fail(start, t.empty() ? std::string("unexpected character '") + (start < s.size() ? s[start] : '?') + "' (expected a value)"
                                  : "invalid number '" + t + "'");
            if (t.empty()) i = s.size(); // no progress is possible: stop here rather than loop
        }
        return v;
    }

    Value parseArray() {
        Value v;
        v.kind = Value::Kind::Array;
        ++i; // '['
        ws();
        if (i < s.size() && s[i] == ']') { ++i; return v; }
        bool closed = false;
        while (i < s.size()) {
            v.arr.push_back(parseValue());
            ws();
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == ']') { ++i; closed = true; break; }
            break;
        }
        if (!closed) fail(i, i < s.size() ? "expected ',' or ']' in an array" : "unterminated array");
        return v;
    }

    Value parseObject() {
        Value v;
        v.kind = Value::Kind::Object;
        ++i; // '{'
        ws();
        if (i < s.size() && s[i] == '}') { ++i; return v; }
        bool closed = false;
        while (i < s.size()) {
            ws();
            if (i >= s.size() || s[i] != '"') { fail(i, "expected a string key in an object"); break; }
            const std::size_t keyAt = i;
            std::string key = parseString();
            // P40 review r3: a duplicate key is a malformation. The lookup returns the FIRST member, while
            // JavaScript, jq and VS Code read the LAST — so `"owned": true, "owned": false` meant one thing to
            // the CLI and the opposite to every other tool reading the same config. Refused, never resolved.
            if (std::any_of(v.members.begin(), v.members.end(), [&](const auto& m) { return m.first == key; }))
                fail(keyAt, "duplicate key \"" + key + "\" in an object");
            ws();
            if (i < s.size() && s[i] == ':') ++i;
            else fail(i, "expected ':' after an object key");
            v.members.emplace_back(std::move(key), parseValue());
            ws();
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == '}') { ++i; closed = true; break; }
            break;
        }
        if (!closed) fail(i, i < s.size() ? "expected ',' or '}' in an object" : "unterminated object");
        return v;
    }

    // The whole document: one value (after an optional UTF-8 BOM an editor may add), then only whitespace.
    Value parseDocument() {
        if (s.compare(0, 3, "\xEF\xBB\xBF") == 0) i = 3;
        Value v = parseValue();
        ws();
        if (i < s.size()) fail(i, "unexpected content after the JSON value");
        return v;
    }
};

} // namespace

Value parse(const std::string& text) {
    Parser p{text};
    return p.parseDocument();
}

// P40 review r4: JSON text is UTF-8 (RFC 8259 §8.1). A CP1252 pgconfig (`g\xE9n`) or a record holding a raw 0xE9
// byte used to parse, and the bytes then reached a path conversion that threw "No mapping for the Unicode
// character…" — exit 1 with no file named. Strict readers now refuse the document. Rejects overlong forms,
// surrogate code points and anything above U+10FFFF; returns the offset of the first bad byte, or npos.
static std::size_t firstInvalidUtf8(const std::string& s) {
    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { ++i; continue; }
        int len;
        unsigned cp;
        if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1Fu; }
        else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0Fu; }
        else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07u; }
        else return i;
        if (i + static_cast<std::size_t>(len) > n) return i;
        for (int k = 1; k < len; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]);
            if ((cc & 0xC0) != 0x80) return i;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if ((len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return i;
        i += static_cast<std::size_t>(len);
    }
    return std::string::npos;
}

Value parse(const std::string& text, std::string& error) {
    if (const std::size_t bad = firstInvalidUtf8(text); bad != std::string::npos) {
        char hex[8];
        std::snprintf(hex, sizeof(hex), "0x%02X", static_cast<unsigned>(static_cast<unsigned char>(text[bad])));
        error = std::string("invalid UTF-8 (byte ") + hex + ") at byte offset " + std::to_string(bad) +
                "; save the file as UTF-8";
        return Value{};
    }
    Parser p{text};
    Value v = p.parseDocument();
    error = p.error;
    return v;
}

std::string quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[(c >> 4) & 0xF];
                    out += hex[c & 0xF];
                } else {
                    out += c;
                }
        }
    }
    out += "\"";
    return out;
}

LineIndex::LineIndex(const std::string& text) {
    lineStarts_.push_back(0);
    for (std::size_t k = 0; k < text.size(); ++k) {
        if (text[k] == '\n') lineStarts_.push_back(k + 1);
    }
}

int LineIndex::lineAt(std::size_t offset) const {
    // First line start STRICTLY after `offset`; the line containing it is the one before that.
    auto it = std::upper_bound(lineStarts_.begin(), lineStarts_.end(), offset);
    return static_cast<int>(it - lineStarts_.begin());
}

} // namespace mintplayer::polyglot::json
