#pragma once

#include <string>
#include <utility>
#include <vector>

// A minimal, zero-dependency JSON reader for the language server (PRD §4.8). The CLI already hand-EMITS
// JSON (`check --json`); this is the reader for incoming LSP requests. It targets machine-generated JSON-RPC
// bodies, so it is lenient (a malformed value parses to Null) rather than a validating parser. Objects keep
// insertion order and are looked up linearly — fine for the small messages LSP exchanges.

namespace mintplayer::polyglot::json {

struct Value {
    enum class Kind { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool boolean = false;
    double num = 0;
    std::string str;                                   // String value (also holds the raw number lexeme)
    std::vector<Value> arr;                            // Array elements
    std::vector<std::pair<std::string, Value>> members;// Object members, in order

    bool isNull() const { return kind == Kind::Null; }
    bool asBool(bool dflt = false) const { return kind == Kind::Bool ? boolean : dflt; }
    double asNumber(double dflt = 0) const { return kind == Kind::Number ? num : dflt; }
    long long asInt(long long dflt = 0) const { return kind == Kind::Number ? static_cast<long long>(num) : dflt; }
    const std::string& asString() const { return str; } // "" when not a string

    const std::vector<Value>& items() const { return arr; } // empty when not an array
    bool has(const std::string& key) const;
    // Object member by key (or an Array index rendered as decimal); a Null sentinel when absent/wrong-kind.
    const Value& operator[](const std::string& key) const;

    static const Value kNull;
};

// Parse a UTF-8 JSON document. On malformed input returns whatever was parsed so far (Null for total garbage).
Value parse(const std::string& text);

// A JSON-escaped, double-quoted string literal (for building outgoing messages).
std::string quote(const std::string& s);

} // namespace mintplayer::polyglot::json
