#pragma once

#include <string>

#include "mintplayer/polyglot/diagnostics.hpp"

// Token set for the full P1 grammar (P3). The lexer produces all of these; the parser widens to consume
// them feature-group by feature-group. Reserved keywords are listed below; a few words that double as
// identifiers in real code (`get`, `set`, `as`, `with`-less contexts) are handled contextually by the
// parser, not reserved here. String interpolation tokenization lands with the strings feature group.

namespace mintplayer::polyglot {

enum class TokKind {
    End,
    Identifier,
    IntLit,
    FloatLit,
    StringLit,
    CharLit,

    // keywords — declarations & modifiers
    KwFn, KwLet, KwVar, KwConst,
    KwClass, KwRecord, KwInterface, KwEnum, KwUnion, KwExtension, KwImport,
    KwInit, KwOperator,
    KwAbstract, KwOpen, KwOverride, KwSealed, KwStatic, KwPrivate,
    KwAsync, KwAwait,
    // keywords — statements & expressions
    KwIf, KwElse, KwWhile, KwDo, KwFor, KwIn,
    KwReturn, KwBreak, KwContinue, KwYield,
    KwMatch, KwUse, KwTry, KwCatch, KwFinally, KwThrow, KwWhen, KwWith,
    KwThis, KwSuper,
    KwTrue, KwFalse, KwNull,

    // punctuation
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Comma, Colon, Semicolon, Dot, Arrow, Question, QuestionDot,

    // operators
    Plus, Minus, Star, Slash, Percent,
    Amp, Pipe, Caret, Tilde, Shl, Shr, UShr,
    AmpAmp, PipePipe, Not,
    Assign, EqEq, NotEq, Lt, LtEq, Gt, GtEq,
    QuestionQuestion, DotDot, DotDotEq,
    // compound assignments
    PlusEq, MinusEq, StarEq, SlashEq, PercentEq,
    AmpEq, PipeEq, CaretEq, ShlEq, ShrEq, UShrEq, QuestionQuestionEq,

    Unknown,
};

struct Token {
    TokKind kind = TokKind::End;
    std::string text; // identifier name, decoded string/char value, or raw numeric text
    SourcePos pos;
};

} // namespace mintplayer::polyglot
