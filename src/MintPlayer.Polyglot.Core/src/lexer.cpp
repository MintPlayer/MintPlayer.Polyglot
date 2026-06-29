#include "mintplayer/polyglot/lexer.hpp"

#include <cctype>
#include <unordered_map>

namespace mintplayer::polyglot {

namespace {

bool isIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool isIdentPart(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

const std::unordered_map<std::string, TokKind>& keywords() {
    static const std::unordered_map<std::string, TokKind> kw = {
        {"fn", TokKind::KwFn}, {"let", TokKind::KwLet}, {"var", TokKind::KwVar}, {"const", TokKind::KwConst},
        {"class", TokKind::KwClass}, {"record", TokKind::KwRecord}, {"interface", TokKind::KwInterface},
        {"enum", TokKind::KwEnum}, {"union", TokKind::KwUnion}, {"extension", TokKind::KwExtension},
        {"import", TokKind::KwImport}, {"init", TokKind::KwInit}, {"operator", TokKind::KwOperator},
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
    Cursor(const std::string& src) : src_(src) {}

    bool eof() const { return idx_ >= src_.size(); }
    char peek(std::size_t ahead = 0) const {
        std::size_t i = idx_ + ahead;
        return i < src_.size() ? src_[i] : '\0';
    }
    SourcePos pos() const { return {line_, col_}; }

    char advance() {
        char c = src_[idx_++];
        if (c == '\n') { ++line_; col_ = 1; } else { ++col_; }
        return c;
    }

private:
    const std::string& src_;
    std::size_t idx_ = 0;
    int line_ = 1;
    int col_ = 1;
};

// One chunk of a (possibly interpolated) string, scanned after the opening quote or a `${...}` hole.
// Stops at the closing `"` (more=false) or at a `${` interpolation hole (more=true), consuming the
// terminator. Decodes escapes.
struct ChunkResult { std::string text; bool more; };
ChunkResult scanChunk(Cursor& cur, DiagnosticBag& diags, SourcePos start) {
    std::string value;
    while (!cur.eof()) {
        if (cur.peek() == '"') { cur.advance(); return {value, false}; }
        if (cur.peek() == '$' && cur.peek(1) == '{') { cur.advance(); cur.advance(); return {value, true}; }
        char ch = cur.advance();
        if (ch == '\\' && !cur.eof()) {
            char esc = cur.advance();
            switch (esc) {
                case 'n': value += '\n'; break;
                case 't': value += '\t'; break;
                case 'r': value += '\r'; break;
                case '0': value += '\0'; break;
                case '"': value += '"'; break;
                case '\\': value += '\\'; break;
                default: value += esc; break;
            }
        } else {
            value += ch;
        }
    }
    diags.error(start, "unterminated string literal");
    return {value, false};
}

// Decode the body of a char literal (after the opening quote). Stops at `quote`.
std::string scanQuoted(Cursor& cur, char quote, bool& terminated) {
    std::string value;
    terminated = false;
    while (!cur.eof()) {
        char ch = cur.advance();
        if (ch == quote) { terminated = true; break; }
        if (ch == '\\' && !cur.eof()) {
            char esc = cur.advance();
            switch (esc) {
                case 'n': value += '\n'; break;
                case 't': value += '\t'; break;
                case 'r': value += '\r'; break;
                case '0': value += '\0'; break;
                case '"': value += '"'; break;
                case '\'': value += '\''; break;
                case '\\': value += '\\'; break;
                default: value += esc; break;
            }
        } else {
            value += ch;
        }
    }
    return value;
}

} // namespace

std::vector<Token> lex(const std::string& source, DiagnosticBag& diags) {
    std::vector<Token> out;
    Cursor cur(source);
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
            cur.advance(); cur.advance();
            while (!cur.eof() && !(cur.peek() == '*' && cur.peek(1) == '/')) cur.advance();
            if (!cur.eof()) { cur.advance(); cur.advance(); }
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
            bool terminated = false;
            std::string value = scanQuoted(cur, '\'', terminated);
            if (!terminated) diags.error(start, "unterminated char literal");
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
