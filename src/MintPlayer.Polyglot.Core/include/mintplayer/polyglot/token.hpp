#pragma once

#include <string>

#include "mintplayer/polyglot/diagnostics.hpp"

// The MVP token set (PLAN P2). Only the walking-skeleton subset is lexable: functions, the numeric/
// bool/string scalars, arithmetic/comparison/logical operators, and the keywords below. Features
// outside the subset are widened in P3 — they simply do not tokenize yet.

namespace mintplayer::polyglot {

enum class TokKind {
    End,
    Identifier,
    IntLit,
    FloatLit,
    StringLit,
    // keywords
    KwFn, KwLet, KwVar, KwIf, KwElse, KwWhile, KwReturn, KwTrue, KwFalse,
    // punctuation
    LParen, RParen, LBrace, RBrace, Comma, Colon, Semicolon, Arrow,
    // operators
    Plus, Minus, Star, Slash, Percent,
    Assign, EqEq, NotEq, Lt, LtEq, Gt, GtEq, Not, AmpAmp, PipePipe,
    Unknown,
};

struct Token {
    TokKind kind = TokKind::End;
    std::string text; // identifier name, decoded string value, or raw numeric text
    SourcePos pos;
};

} // namespace mintplayer::polyglot
