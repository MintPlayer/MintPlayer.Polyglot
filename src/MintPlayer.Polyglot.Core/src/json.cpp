#include "mintplayer/polyglot/json.hpp"

#include <cstdlib>

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

    unsigned hex4() {
        unsigned v = 0;
        for (int k = 0; k < 4 && i < s.size(); ++k) {
            char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
        }
        return v;
    }

    std::string parseString() {
        std::string out;
        ++i; // opening quote
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') break;
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
                    unsigned cp = hex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        i += 2; // consume the "\u" of the low surrogate
                        unsigned lo = hex4();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: out += e; break;
            }
        }
        return out;
    }

    Value parseValue() {
        ws();
        if (i >= s.size()) return {};
        char c = s[i];
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') { Value v; v.kind = Value::Kind::String; v.str = parseString(); return v; }
        if (c == 't') { i += 4; Value v; v.kind = Value::Kind::Bool; v.boolean = true;  return v; }
        if (c == 'f') { i += 5; Value v; v.kind = Value::Kind::Bool; v.boolean = false; return v; }
        if (c == 'n') { i += 4; return {}; } // null
        return parseNumber();
    }

    Value parseNumber() {
        std::size_t start = i;
        while (i < s.size()) {
            char c = s[i];
            if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') ++i;
            else break;
        }
        Value v;
        v.kind = Value::Kind::Number;
        v.str = s.substr(start, i - start);
        v.num = std::strtod(v.str.c_str(), nullptr);
        return v;
    }

    Value parseArray() {
        Value v;
        v.kind = Value::Kind::Array;
        ++i; // '['
        ws();
        if (i < s.size() && s[i] == ']') { ++i; return v; }
        while (i < s.size()) {
            v.arr.push_back(parseValue());
            ws();
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == ']') { ++i; break; }
            break;
        }
        return v;
    }

    Value parseObject() {
        Value v;
        v.kind = Value::Kind::Object;
        ++i; // '{'
        ws();
        if (i < s.size() && s[i] == '}') { ++i; return v; }
        while (i < s.size()) {
            ws();
            if (i >= s.size() || s[i] != '"') break;
            std::string key = parseString();
            ws();
            if (i < s.size() && s[i] == ':') ++i;
            v.members.emplace_back(std::move(key), parseValue());
            ws();
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == '}') { ++i; break; }
            break;
        }
        return v;
    }
};

} // namespace

Value parse(const std::string& text) {
    Parser p{text};
    return p.parseValue();
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

} // namespace mintplayer::polyglot::json
