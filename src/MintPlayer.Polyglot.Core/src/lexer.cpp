#include "mintplayer/polyglot/lexer.hpp"

#include <cctype>
#include <cstdio>
#include <unordered_map>

#include "mintplayer/polyglot/unicode.hpp"

namespace mintplayer::polyglot {

namespace {

bool isIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool isIdentPart(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

const std::unordered_map<std::string, TokKind>& keywords() {
    static const std::unordered_map<std::string, TokKind> kw = {
        {"fn", TokKind::KwFn}, {"let", TokKind::KwLet}, {"var", TokKind::KwVar}, {"const", TokKind::KwConst},
        {"class", TokKind::KwClass}, {"record", TokKind::KwRecord}, {"interface", TokKind::KwInterface},
        {"enum", TokKind::KwEnum}, {"union", TokKind::KwUnion}, {"extension", TokKind::KwExtension},
        {"import", TokKind::KwImport}, {"constructor", TokKind::KwConstructor}, {"operator", TokKind::KwOperator},
        {"expect", TokKind::KwExpect}, {"actual", TokKind::KwActual}, {"extern", TokKind::KwExtern},
        {"abstract", TokKind::KwAbstract}, {"open", TokKind::KwOpen}, {"override", TokKind::KwOverride},
        {"sealed", TokKind::KwSealed}, {"static", TokKind::KwStatic}, {"private", TokKind::KwPrivate},
        {"async", TokKind::KwAsync}, {"await", TokKind::KwAwait},
        {"if", TokKind::KwIf}, {"else", TokKind::KwElse}, {"while", TokKind::KwWhile}, {"do", TokKind::KwDo},
        {"for", TokKind::KwFor}, {"in", TokKind::KwIn},
        {"return", TokKind::KwReturn}, {"break", TokKind::KwBreak}, {"continue", TokKind::KwContinue},
        {"yield", TokKind::KwYield},
        {"match", TokKind::KwMatch}, {"use", TokKind::KwUse}, {"try", TokKind::KwTry},
        {"catch", TokKind::KwCatch}, {"finally", TokKind::KwFinally}, {"throw", TokKind::KwThrow},
        {"when", TokKind::KwWhen}, {"with", TokKind::KwWith},
        {"this", TokKind::KwThis}, {"super", TokKind::KwSuper},
        {"true", TokKind::KwTrue}, {"false", TokKind::KwFalse}, {"null", TokKind::KwNull},
    };
    return kw;
}

class Cursor {
public:
    Cursor(const std::string& src, int fileId) : src_(src), fileId_(fileId) {}

    bool eof() const { return idx_ >= src_.size(); }
    char peek(std::size_t ahead = 0) const {
        std::size_t i = idx_ + ahead;
        return i < src_.size() ? src_[i] : '\0';
    }
    SourcePos pos() const { return {line_, col_, fileId_}; }

    char advance() {
        char c = src_[idx_++];
        if (c == '\n') { ++line_; col_ = 1; } else { ++col_; }
        return c;
    }

    // Consume one UTF-8 character into `out` (its raw bytes). False on an ill-formed sequence, in which case
    // exactly one byte was consumed — the caller reports it.
    bool takeChar(std::string& out, char32_t& cp) {
        std::size_t j = idx_;
        const bool ok = unicode::decodeUtf8(src_, j, cp);
        while (idx_ < j) out += advance();
        return ok;
    }

private:
    const std::string& src_;
    std::size_t idx_ = 0;
    int line_ = 1;
    int col_ = 1;
    int fileId_ = 0;
};

// (G47's "non-ASCII string content" warning is gone since P40 B3: every target's string std now indexes
// UTF-16 code units — Python and PHP through helpers — so non-ASCII text is faithful, not a caveat.)

std::string hexUpper(unsigned long v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%lX", v);
    return buf;
}

// P40 A1: the ONE escape decoder, shared by string chunks and char literals. Called with the cursor ON the
// backslash; appends the decoded character (UTF-8) to `out`. Every escape outside the published set is an
// error at the backslash — never a silently dropped backslash (the pre-P40 `\u{1D}` -> "u{1D}" miscompile).
// Returns false when an error was reported (nothing appended). A backslash at end of input appends nothing
// and reports nothing: the caller's "unterminated" diagnostic covers it.
bool decodeEscape(Cursor& cur, DiagnosticBag& diags, std::string& out) {
    const SourcePos at = cur.pos();
    cur.advance(); // the backslash
    if (cur.eof()) return true;
    const char esc = cur.peek();
    switch (esc) {
        case 'n': cur.advance(); out += '\n'; return true;
        case 't': cur.advance(); out += '\t'; return true;
        case 'r': cur.advance(); out += '\r'; return true;
        case '0': cur.advance(); out += '\0'; return true;
        case '\\': case '"': case '\'': case '$': cur.advance(); out += esc; return true;
        case 'u': {
            cur.advance();
            if (cur.peek() != '{') {
                diags.error(at, "`\\uXXXX` is not a Polyglot escape — write the code point in braces: `\\u{XXXX}` "
                                "(1 to 6 hex digits)");
                return false;
            }
            cur.advance();
            std::string hex;
            while (!cur.eof() && std::isxdigit(static_cast<unsigned char>(cur.peek()))) hex += cur.advance();
            const bool closed = !cur.eof() && cur.peek() == '}';
            if (closed) cur.advance();
            if (!closed || hex.empty() || hex.size() > 6) {
                diags.error(at, "malformed `\\u{…}` escape — expected 1 to 6 hex digits and a closing `}`");
                return false;
            }
            const unsigned long v = std::stoul(hex, nullptr, 16);
            if (v > 0x10FFFF) {
                diags.error(at, "`\\u{" + hex + "}` is beyond U+10FFFF, the largest Unicode code point");
                return false;
            }
            if (v >= 0xD800 && v <= 0xDFFF) {
                diags.error(at, "`\\u{" + hex + "}` is a surrogate, not a character — escape the whole code "
                                "point instead (e.g. `\\u{1F600}`)");
                return false;
            }
            unicode::appendUtf8(out, static_cast<char32_t>(v));
            return true;
        }
        case 'x':
            cur.advance();
            diags.error(at, "`\\xHH` is not a Polyglot escape — write the code point in braces: `\\u{HH}`");
            return false;
        default: break;
    }
    // Unknown: name the whole character after the backslash (a multi-byte one included).
    std::string shown;
    char32_t cp = 0;
    cur.takeChar(shown, cp);
    if (cp < 0x20 || cp == 0x7F) shown = "U+" + hexUpper(cp);
    diags.error(at, "unknown escape sequence `\\" + shown +
                    "` — valid escapes are \\n \\t \\r \\0 \\\\ \\\" \\' \\$ and \\u{…}");
    return false;
}

// A non-escape literal character: raw ASCII, or one validated UTF-8 character (ill-formed -> one error per
// literal, `badUtf8` latches).
void takeLiteralChar(Cursor& cur, DiagnosticBag& diags, std::string& out, bool& badUtf8, const char* what) {
    if (static_cast<unsigned char>(cur.peek()) < 0x80) { out += cur.advance(); return; }
    const SourcePos p = cur.pos();
    char32_t cp = 0;
    if (!cur.takeChar(out, cp) && !badUtf8) {
        diags.error(p, std::string("invalid UTF-8 in ") + what + " — save the source file as UTF-8");
        badUtf8 = true;
    }
}

// One chunk of a (possibly interpolated) string, scanned after the opening quote or a `${...}` hole.
// Stops at the closing `"` (more=false) or at a `${` interpolation hole (more=true), consuming the
// terminator. Decodes escapes (decodeEscape).
struct ChunkResult { std::string text; bool more; };
ChunkResult scanChunk(Cursor& cur, DiagnosticBag& diags, SourcePos start) {
    std::string value;
    bool badUtf8 = false;
    while (!cur.eof()) {
        const char c = cur.peek();
        if (c == '"') { cur.advance(); return {value, false}; }
        if (c == '$' && cur.peek(1) == '{') { cur.advance(); cur.advance(); return {value, true}; }
        if (c == '\\') {
            decodeEscape(cur, diags, value);
            continue;
        }
        takeLiteralChar(cur, diags, value, badUtf8, "a string literal");
    }
    diags.error(start, "unterminated string literal");
    return {value, false};
}

// The body of a char literal (after the opening quote), through the closing quote. Decodes escapes and
// validates the result is exactly ONE character that fits one UTF-16 code unit (SPEC §8).
std::string scanChar(Cursor& cur, DiagnosticBag& diags, SourcePos start) {
    std::string value;
    bool terminated = false, bad = false, badUtf8 = false;
    while (!cur.eof()) {
        if (cur.peek() == '\'') { cur.advance(); terminated = true; break; }
        if (cur.peek() == '\\') {
            if (!decodeEscape(cur, diags, value)) bad = true;
            continue;
        }
        takeLiteralChar(cur, diags, value, badUtf8, "a char literal");
    }
    if (!terminated) { diags.error(start, "unterminated char literal"); return value; }
    if (bad || badUtf8) return value;
    std::size_t i = 0, count = 0;
    char32_t cp = 0, first = 0;
    while (i < value.size()) {
        unicode::decodeUtf8(value, i, cp);
        if (count++ == 0) first = cp;
    }
    if (count == 0) {
        diags.error(start, "empty char literal — a char is exactly one character");
    } else if (count > 1) {
        diags.error(start, "char literal holds more than one character — a char is exactly one character; "
                           "use a string (\"…\") for text");
    } else if (first > 0xFFFF) {
        diags.error(start, "char literal holds U+" + hexUpper(first) + ", an astral character — a char is one "
                           "UTF-16 code unit (SPEC §8); use a string");
    }
    return value;
}

} // namespace

std::vector<Token> lex(const std::string& source, DiagnosticBag& diags, int fileId) {
    std::vector<Token> out;
    Cursor cur(source, fileId);
    // G43: tolerate a UTF-8 BOM — the default save format of several Windows editors. Consumed as
    // trivia so positions/diagnostics start at the real first character.
    if (source.size() >= 3 && source.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        cur.advance(); cur.advance(); cur.advance();
    }
    std::vector<int> interpDepth; // brace depth per open interpolation hole; non-empty = inside `${ ... }`

    auto push = [&](TokKind kind, std::string text, SourcePos pos) {
        out.push_back({kind, std::move(text), pos});
    };

    while (!cur.eof()) {
        char c = cur.peek();

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { cur.advance(); continue; }

        // Comments (trivia).
        if (c == '/' && cur.peek(1) == '/') {
            while (!cur.eof() && cur.peek() != '\n') cur.advance();
            continue;
        }
        if (c == '/' && cur.peek(1) == '*') {
            SourcePos cstart = cur.pos();
            cur.advance(); cur.advance();
            while (!cur.eof() && !(cur.peek() == '*' && cur.peek(1) == '/')) cur.advance();
            if (!cur.eof()) { cur.advance(); cur.advance(); }
            else // G44: an unterminated /* would otherwise SILENTLY swallow every token after it —
                 // miscompile-adjacent trivia. One diagnostic at the comment's start.
                diags.error(cstart, "unterminated block comment ('/*' without a closing '*/')");
            continue;
        }

        SourcePos start = cur.pos();

        // End of an interpolation hole: `}` at hole-brace-depth 0 resumes scanning the string chunk.
        if (!interpDepth.empty() && c == '}' && interpDepth.back() == 0) {
            cur.advance();
            interpDepth.pop_back();
            ChunkResult r = scanChunk(cur, diags, start);
            if (r.more) { push(TokKind::InterpMid, std::move(r.text), start); interpDepth.push_back(0); }
            else push(TokKind::InterpEnd, std::move(r.text), start);
            continue;
        }

        // Identifiers and keywords.
        if (isIdentStart(c)) {
            std::string text;
            while (!cur.eof() && isIdentPart(cur.peek())) text += cur.advance();
            auto it = keywords().find(text);
            push(it != keywords().end() ? it->second : TokKind::Identifier, std::move(text), start);
            continue;
        }

        // Numbers: integer unless a '.' fraction or exponent makes it a float. '_' separators dropped.
        if (std::isdigit(static_cast<unsigned char>(c))) {
            std::string text;
            // Hex literal: its digit alphabet overlaps the float suffix letters ('f'/'d'), so the
            // generic width-suffix scan below must never run on it — after the digits only an integer
            // width suffix is legal, and a trailing 'f'/'d' is a digit, never a suffix.
            if (c == '0' && (cur.peek(1) == 'x' || cur.peek(1) == 'X')) {
                text += cur.advance();
                text += cur.advance();
                bool anyDigit = false;
                while (!cur.eof() && (std::isxdigit(static_cast<unsigned char>(cur.peek())) || cur.peek() == '_')) {
                    char d = cur.advance();
                    if (d != '_') { text += d; anyDigit = true; }
                }
                if (!anyDigit) diags.error(start, "hex literal has no digits");
                if (isIdentStart(cur.peek())) {
                    std::string suffix;
                    while (!cur.eof() && isIdentPart(cur.peek())) suffix += cur.advance();
                    static const char* kIntSuffixes[] = {"i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64"};
                    bool known = false;
                    for (const char* s : kIntSuffixes) if (suffix == s) { known = true; break; }
                    if (known) text += suffix;
                    else diags.error(start, "invalid suffix '" + suffix + "' on hex literal");
                }
                push(TokKind::IntLit, std::move(text), start);
                continue;
            }
            bool isFloat = false;
            auto readDigits = [&]() {
                while (!cur.eof() && (std::isdigit(static_cast<unsigned char>(cur.peek())) || cur.peek() == '_')) {
                    char d = cur.advance();
                    if (d != '_') text += d;
                }
            };
            readDigits();
            if (cur.peek() == '.' && std::isdigit(static_cast<unsigned char>(cur.peek(1)))) {
                isFloat = true;
                text += cur.advance();
                readDigits();
            }
            if (cur.peek() == 'e' || cur.peek() == 'E') {
                isFloat = true;
                text += cur.advance();
                if (cur.peek() == '+' || cur.peek() == '-') text += cur.advance();
                readDigits();
            }
            // Optional width suffix (i32/u8/f32/...), kept on the token text for later passes.
            if (isIdentStart(cur.peek())) {
                std::string suffix;
                while (!cur.eof() && isIdentPart(cur.peek())) suffix += cur.advance();
                if (!suffix.empty() && (suffix[0] == 'f' || suffix == "d")) isFloat = true;
                text += suffix;
            }
            push(isFloat ? TokKind::FloatLit : TokKind::IntLit, std::move(text), start);
            continue;
        }

        // String / char literals; the decoded value is stored and re-escaped on emit.
        if (c == '"') {
            cur.advance();
            ChunkResult r = scanChunk(cur, diags, start);
            if (r.more) { push(TokKind::InterpStart, std::move(r.text), start); interpDepth.push_back(0); }
            else push(TokKind::StringLit, std::move(r.text), start);
            continue;
        }
        if (c == '\'') {
            cur.advance();
            std::string value = scanChar(cur, diags, start);
            push(TokKind::CharLit, std::move(value), start);
            continue;
        }

        // Operators & punctuation (peek-based, longest-match-first).
        char c1 = cur.peek(1), c2 = cur.peek(2), c3 = cur.peek(3);
        auto emit = [&](TokKind k, int len) { for (int i = 0; i < len; ++i) cur.advance(); push(k, "", start); };

        switch (c) {
            case '(': emit(TokKind::LParen, 1); break;
            case ')': emit(TokKind::RParen, 1); break;
            case '{': if (!interpDepth.empty()) ++interpDepth.back(); emit(TokKind::LBrace, 1); break;
            case '}': if (!interpDepth.empty()) --interpDepth.back(); emit(TokKind::RBrace, 1); break;
            case '[': emit(TokKind::LBracket, 1); break;
            case ']': emit(TokKind::RBracket, 1); break;
            case ',': emit(TokKind::Comma, 1); break;
            case ':': emit(TokKind::Colon, 1); break;
            case ';': emit(TokKind::Semicolon, 1); break;
            case '~': emit(TokKind::Tilde, 1); break;

            case '+': emit(c1 == '=' ? TokKind::PlusEq : TokKind::Plus, c1 == '=' ? 2 : 1); break;
            case '-': emit(c1 == '=' ? TokKind::MinusEq : TokKind::Minus, c1 == '=' ? 2 : 1); break;
            case '*': emit(c1 == '=' ? TokKind::StarEq : TokKind::Star, c1 == '=' ? 2 : 1); break;
            case '/': emit(c1 == '=' ? TokKind::SlashEq : TokKind::Slash, c1 == '=' ? 2 : 1); break;
            case '%': emit(c1 == '=' ? TokKind::PercentEq : TokKind::Percent, c1 == '=' ? 2 : 1); break;
            case '^': emit(c1 == '=' ? TokKind::CaretEq : TokKind::Caret, c1 == '=' ? 2 : 1); break;

            case '=':
                if (c1 == '=') emit(TokKind::EqEq, 2);
                else if (c1 == '>') emit(TokKind::Arrow, 2);
                else emit(TokKind::Assign, 1);
                break;
            case '!': emit(c1 == '=' ? TokKind::NotEq : TokKind::Not, c1 == '=' ? 2 : 1); break;

            case '&':
                if (c1 == '&') emit(TokKind::AmpAmp, 2);
                else if (c1 == '=') emit(TokKind::AmpEq, 2);
                else emit(TokKind::Amp, 1);
                break;
            case '|':
                if (c1 == '|') emit(TokKind::PipePipe, 2);
                else if (c1 == '=') emit(TokKind::PipeEq, 2);
                else emit(TokKind::Pipe, 1);
                break;

            case '<':
                if (c1 == '<' && c2 == '=') emit(TokKind::ShlEq, 3);
                else if (c1 == '<') emit(TokKind::Shl, 2);
                else if (c1 == '=') emit(TokKind::LtEq, 2);
                else emit(TokKind::Lt, 1);
                break;
            case '>':
                if (c1 == '>' && c2 == '>' && c3 == '=') emit(TokKind::UShrEq, 4);
                else if (c1 == '>' && c2 == '>') emit(TokKind::UShr, 3);
                else if (c1 == '>' && c2 == '=') emit(TokKind::ShrEq, 3);
                else if (c1 == '>') emit(TokKind::Shr, 2);
                else if (c1 == '=') emit(TokKind::GtEq, 2);
                else emit(TokKind::Gt, 1);
                break;

            case '?':
                if (c1 == '?' && c2 == '=') emit(TokKind::QuestionQuestionEq, 3);
                else if (c1 == '?') emit(TokKind::QuestionQuestion, 2);
                else if (c1 == '.') emit(TokKind::QuestionDot, 2);
                else emit(TokKind::Question, 1);
                break;
            case '.':
                if (c1 == '.' && c2 == '=') emit(TokKind::DotDotEq, 3);
                else if (c1 == '.') emit(TokKind::DotDot, 2);
                else emit(TokKind::Dot, 1);
                break;

            default:
                // G48: a non-ASCII byte is one multi-byte character — consume the WHOLE UTF-8
                // sequence and diagnose once, position-correct, instead of one error per byte.
                if (static_cast<unsigned char>(c) >= 0x80) {
                    std::string seq;
                    while (!cur.eof() && static_cast<unsigned char>(cur.peek()) >= 0x80) seq += cur.advance();
                    diags.error(start, "non-ASCII character in source — Polyglot identifiers and keywords are ASCII (string literals: see the SPEC §9 PHP byte-string caveat)");
                    push(TokKind::Unknown, std::move(seq), start);
                    break;
                }
                diags.error(start, std::string("unexpected character '") + c + "'");
                cur.advance();
                push(TokKind::Unknown, std::string(1, c), start);
                break;
        }
    }

    out.push_back({TokKind::End, "", cur.pos()});
    return out;
}

} // namespace mintplayer::polyglot
