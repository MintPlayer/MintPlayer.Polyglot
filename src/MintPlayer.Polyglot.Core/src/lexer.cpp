#include "mintplayer/polyglot/lexer.hpp"

#include <cctype>
#include <unordered_map>

namespace mintplayer::polyglot {

namespace {

bool isIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool isIdentPart(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

const std::unordered_map<std::string, TokKind>& keywords() {
    static const std::unordered_map<std::string, TokKind> kw = {
        {"fn", TokKind::KwFn},       {"let", TokKind::KwLet},     {"var", TokKind::KwVar},
        {"if", TokKind::KwIf},       {"else", TokKind::KwElse},   {"while", TokKind::KwWhile},
        {"return", TokKind::KwReturn}, {"true", TokKind::KwTrue}, {"false", TokKind::KwFalse},
    };
    return kw;
}

// A small cursor over the source that tracks 1-based line/column.
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

} // namespace

std::vector<Token> lex(const std::string& source, DiagnosticBag& diags) {
    std::vector<Token> out;
    Cursor cur(source);

    auto push = [&](TokKind kind, std::string text, SourcePos pos) {
        out.push_back({kind, std::move(text), pos});
    };

    while (!cur.eof()) {
        char c = cur.peek();

        // Whitespace.
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

        // Identifiers and keywords.
        if (isIdentStart(c)) {
            std::string text;
            while (!cur.eof() && isIdentPart(cur.peek())) text += cur.advance();
            auto it = keywords().find(text);
            if (it != keywords().end()) {
                TokKind k = it->second;
                if (k == TokKind::KwTrue || k == TokKind::KwFalse) {
                    push(k, std::move(text), start);
                } else {
                    push(k, std::move(text), start);
                }
            } else {
                push(TokKind::Identifier, std::move(text), start);
            }
            continue;
        }

        // Numbers: integer unless a '.' fraction or an exponent makes it a float. '_' separators are
        // dropped so the raw text re-emits cleanly to either target.
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
                text += cur.advance(); // '.'
                readDigits();
            }
            if (cur.peek() == 'e' || cur.peek() == 'E') {
                isFloat = true;
                text += cur.advance();
                if (cur.peek() == '+' || cur.peek() == '-') text += cur.advance();
                readDigits();
            }
            push(isFloat ? TokKind::FloatLit : TokKind::IntLit, std::move(text), start);
            continue;
        }

        // String literals with a minimal escape set; the decoded value is stored and re-escaped on emit.
        if (c == '"') {
            cur.advance(); // opening quote
            std::string value;
            bool terminated = false;
            while (!cur.eof()) {
                char ch = cur.advance();
                if (ch == '"') { terminated = true; break; }
                if (ch == '\\' && !cur.eof()) {
                    char esc = cur.advance();
                    switch (esc) {
                        case 'n': value += '\n'; break;
                        case 't': value += '\t'; break;
                        case 'r': value += '\r'; break;
                        case '"': value += '"'; break;
                        case '\\': value += '\\'; break;
                        default: value += esc; break;
                    }
                } else {
                    value += ch;
                }
            }
            if (!terminated) diags.error(start, "unterminated string literal");
            push(TokKind::StringLit, std::move(value), start);
            continue;
        }

        // Operators and punctuation (multi-char first).
        cur.advance();
        auto two = [&](char next, TokKind ifTwo, TokKind ifOne) {
            if (cur.peek() == next) { cur.advance(); push(ifTwo, "", start); }
            else push(ifOne, "", start);
        };
        switch (c) {
            case '(': push(TokKind::LParen, "", start); break;
            case ')': push(TokKind::RParen, "", start); break;
            case '{': push(TokKind::LBrace, "", start); break;
            case '}': push(TokKind::RBrace, "", start); break;
            case ',': push(TokKind::Comma, "", start); break;
            case ':': push(TokKind::Colon, "", start); break;
            case ';': push(TokKind::Semicolon, "", start); break;
            case '+': push(TokKind::Plus, "", start); break;
            case '-': push(TokKind::Minus, "", start); break;
            case '*': push(TokKind::Star, "", start); break;
            case '/': push(TokKind::Slash, "", start); break;
            case '%': push(TokKind::Percent, "", start); break;
            case '=':
                if (cur.peek() == '=') { cur.advance(); push(TokKind::EqEq, "", start); }
                else if (cur.peek() == '>') { cur.advance(); push(TokKind::Arrow, "", start); }
                else push(TokKind::Assign, "", start);
                break;
            case '!': two('=', TokKind::NotEq, TokKind::Not); break;
            case '<': two('=', TokKind::LtEq, TokKind::Lt); break;
            case '>': two('=', TokKind::GtEq, TokKind::Gt); break;
            case '&':
                if (cur.peek() == '&') { cur.advance(); push(TokKind::AmpAmp, "", start); }
                else { diags.error(start, "unexpected '&' (did you mean '&&'?)"); push(TokKind::Unknown, "&", start); }
                break;
            case '|':
                if (cur.peek() == '|') { cur.advance(); push(TokKind::PipePipe, "", start); }
                else { diags.error(start, "unexpected '|' (did you mean '||'?)"); push(TokKind::Unknown, "|", start); }
                break;
            default:
                diags.error(start, std::string("unexpected character '") + c + "'");
                push(TokKind::Unknown, std::string(1, c), start);
                break;
        }
    }

    out.push_back({TokKind::End, "", cur.pos()});
    return out;
}

} // namespace mintplayer::polyglot
