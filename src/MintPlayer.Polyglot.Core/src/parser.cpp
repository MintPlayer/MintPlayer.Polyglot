#include "mintplayer/polyglot/parser.hpp"

namespace mintplayer::polyglot {

namespace {

class Parser {
public:
    Parser(const std::vector<Token>& tokens, DiagnosticBag& diags)
        : toks_(tokens), diags_(diags) {}

    CompilationUnit parseUnit() {
        CompilationUnit unit;
        while (!at(TokKind::End)) {
            if (at(TokKind::KwImport)) unit.imports.push_back(parseImport());
            else if (at(TokKind::KwFn)) unit.functions.push_back(parseFunction());
            else if (at(TokKind::KwEnum)) unit.enums.push_back(parseEnum());
            else if (at(TokKind::KwUnion)) unit.unions.push_back(parseUnion());
            else {
                error("expected a declaration");
                if (!recoverToTopLevel()) break;
            }
        }
        return unit;
    }

    ImportDecl parseImport() {
        ImportDecl d;
        d.pos = peek().pos;
        expect(TokKind::KwImport, "'import'");
        d.path = expect(TokKind::Identifier, "a module path").text;
        while (accept(TokKind::Dot)) {
            if (at(TokKind::LBrace)) { // selective group `.{ a, b }` ends the path
                advance();
                if (!at(TokKind::RBrace)) {
                    do { d.names.push_back(expect(TokKind::Identifier, "an imported name").text); }
                    while (accept(TokKind::Comma) && !at(TokKind::RBrace));
                }
                expect(TokKind::RBrace, "'}'");
                break;
            }
            d.path += "." + expect(TokKind::Identifier, "a path segment").text;
        }
        if (at(TokKind::Identifier) && peek().text == "as") { advance(); d.alias = expect(TokKind::Identifier, "an alias").text; }
        accept(TokKind::Semicolon);
        return d;
    }

private:
    const std::vector<Token>& toks_;
    DiagnosticBag& diags_;
    std::size_t idx_ = 0;
    bool panicked_ = false;

    const Token& peek(std::size_t ahead = 0) const {
        std::size_t i = idx_ + ahead;
        return i < toks_.size() ? toks_[i] : toks_.back();
    }
    bool at(TokKind k) const { return peek().kind == k; }
    const Token& advance() { return toks_[idx_ < toks_.size() - 1 ? idx_++ : idx_]; }

    bool accept(TokKind k) { if (at(k)) { advance(); return true; } return false; }
    Token expect(TokKind k, const char* what) {
        if (at(k)) return advance();
        error(std::string("expected ") + what);
        return peek();
    }
    void error(std::string msg) {
        if (!panicked_) diags_.error(peek().pos, std::move(msg));
        panicked_ = true;
    }

    bool recoverToTopLevel() {
        while (!at(TokKind::End) && !at(TokKind::KwImport) && !at(TokKind::KwFn) &&
               !at(TokKind::KwEnum) && !at(TokKind::KwUnion))
            advance();
        panicked_ = false;
        return !at(TokKind::End);
    }

    EnumDecl parseEnum() {
        EnumDecl d;
        d.pos = peek().pos;
        expect(TokKind::KwEnum, "'enum'");
        d.name = expect(TokKind::Identifier, "an enum name").text;
        expect(TokKind::LBrace, "'{'");
        while (!at(TokKind::RBrace) && !at(TokKind::End)) {
            EnumCase c;
            c.pos = peek().pos;
            c.name = expect(TokKind::Identifier, "an enum case name").text;
            if (accept(TokKind::Assign)) {
                if (at(TokKind::IntLit)) { c.hasValue = true; c.value = std::stoll(advance().text); }
                else error("expected an integer enum value");
            }
            d.cases.push_back(std::move(c));
            accept(TokKind::Comma); // optional separator (cases may be newline-separated)
        }
        expect(TokKind::RBrace, "'}'");
        return d;
    }

    UnionDecl parseUnion() {
        UnionDecl d;
        d.pos = peek().pos;
        expect(TokKind::KwUnion, "'union'");
        d.name = expect(TokKind::Identifier, "a union name").text;
        expect(TokKind::LBrace, "'{'");
        while (!at(TokKind::RBrace) && !at(TokKind::End)) {
            UnionCase c;
            c.pos = peek().pos;
            c.name = expect(TokKind::Identifier, "a union case name").text;
            if (accept(TokKind::LParen)) {
                if (!at(TokKind::RParen)) {
                    do {
                        Param p;
                        p.pos = peek().pos;
                        p.name = expect(TokKind::Identifier, "a field name").text;
                        expect(TokKind::Colon, "':'");
                        p.type = parseType();
                        c.params.push_back(std::move(p));
                    } while (accept(TokKind::Comma));
                }
                expect(TokKind::RParen, "')'");
            }
            d.cases.push_back(std::move(c));
            accept(TokKind::Comma);
        }
        expect(TokKind::RBrace, "'}'");
        return d;
    }

    static ExprPtr mk(ExprKind kind, SourcePos pos) {
        auto e = std::make_unique<Expr>();
        e->kind = kind;
        e->pos = pos;
        return e;
    }

    TypeRef parseType() {
        TypeRef t = parseTypeCore();
        if (accept(TokKind::Question)) t.nullable = true;
        return t;
    }
    TypeRef parseTypeCore() {
        if (at(TokKind::LParen)) {
            advance();
            std::vector<TypeRef> elems;
            if (!at(TokKind::RParen)) {
                do { elems.push_back(parseType()); } while (accept(TokKind::Comma));
            }
            expect(TokKind::RParen, "')'");
            if (accept(TokKind::Arrow)) { // function type (A, B) => C
                TypeRef f;
                f.kind = TypeRef::Kind::Function;
                f.args = std::move(elems);
                f.ret.push_back(parseType());
                return f;
            }
            if (elems.size() == 1) return elems[0]; // parenthesized grouping
            TypeRef tup;
            tup.kind = TypeRef::Kind::Tuple;
            tup.args = std::move(elems);
            return tup;
        }
        TypeRef t = namedType(expect(TokKind::Identifier, "a type name").text);
        if (accept(TokKind::Lt)) { // generic args (single-level; nested >> is handled in the generics increment)
            do { t.args.push_back(parseType()); } while (accept(TokKind::Comma));
            expect(TokKind::Gt, "'>'");
        }
        return t;
    }

    // ---- declarations & statements (statement grammar widens in a later P3 increment) ----

    FunctionDecl parseFunction() {
        FunctionDecl fn;
        fn.pos = peek().pos;
        expect(TokKind::KwFn, "'fn'");
        fn.name = expect(TokKind::Identifier, "a function name").text;
        expect(TokKind::LParen, "'('");
        if (!at(TokKind::RParen)) {
            do {
                Param p;
                p.pos = peek().pos;
                p.name = expect(TokKind::Identifier, "a parameter name").text;
                expect(TokKind::Colon, "':'");
                p.type = parseType();
                fn.params.push_back(std::move(p));
            } while (accept(TokKind::Comma));
        }
        expect(TokKind::RParen, "')'");
        if (accept(TokKind::Colon)) fn.returnType = parseType();

        if (accept(TokKind::Arrow)) {
            auto ret = std::make_unique<Stmt>();
            ret->kind = StmtKind::Return;
            ret->pos = peek().pos;
            ret->value = parseExpr();
            fn.body.push_back(std::move(ret));
        } else {
            fn.body = parseBlock();
        }
        return fn;
    }

    std::vector<StmtPtr> parseBlock() {
        std::vector<StmtPtr> stmts;
        expect(TokKind::LBrace, "'{'");
        while (!at(TokKind::RBrace) && !at(TokKind::End)) {
            stmts.push_back(parseStmt());
            if (panicked_) {
                while (!at(TokKind::Semicolon) && !at(TokKind::RBrace) && !at(TokKind::End)) advance();
                accept(TokKind::Semicolon);
                panicked_ = false;
            }
        }
        expect(TokKind::RBrace, "'}'");
        return stmts;
    }

    StmtPtr parseStmt() {
        if (at(TokKind::KwLet) || at(TokKind::KwVar)) return parseLet();
        if (at(TokKind::KwIf)) return parseIf();
        if (at(TokKind::KwWhile)) return parseWhile();
        if (at(TokKind::KwFor)) return parseFor();
        if (at(TokKind::KwReturn)) return parseReturn();
        if (at(TokKind::KwBreak) || at(TokKind::KwContinue)) {
            auto s = std::make_unique<Stmt>();
            s->kind = at(TokKind::KwBreak) ? StmtKind::Break : StmtKind::Continue;
            s->pos = peek().pos;
            advance();
            accept(TokKind::Semicolon);
            return s;
        }
        if (at(TokKind::KwThrow) || at(TokKind::KwYield)) {
            auto s = std::make_unique<Stmt>();
            s->kind = at(TokKind::KwThrow) ? StmtKind::Throw : StmtKind::Yield;
            s->pos = peek().pos;
            advance();
            s->value = parseExpr();
            accept(TokKind::Semicolon);
            return s;
        }
        if (at(TokKind::KwUse)) return parseUse();
        if (at(TokKind::KwTry)) return parseTry();

        // An expression, optionally the target of an assignment (`=`, `+=`, … on any lvalue).
        SourcePos sp = peek().pos;
        auto e = parseExpr();
        const char* op = assignOp(peek().kind);
        if (op) {
            auto s = std::make_unique<Stmt>();
            s->kind = StmtKind::Assign;
            s->pos = sp;
            s->target = std::move(e);
            s->op = op;
            advance();
            s->value = parseExpr();
            accept(TokKind::Semicolon);
            return s;
        }
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::ExprStmt;
        s->pos = sp;
        s->value = std::move(e);
        accept(TokKind::Semicolon);
        return s;
    }

    // Returns the operator spelling if `k` is an assignment operator, else nullptr.
    static const char* assignOp(TokKind k) {
        switch (k) {
            case TokKind::Assign:             return "=";
            case TokKind::PlusEq:             return "+=";
            case TokKind::MinusEq:            return "-=";
            case TokKind::StarEq:             return "*=";
            case TokKind::SlashEq:            return "/=";
            case TokKind::PercentEq:          return "%=";
            case TokKind::AmpEq:              return "&=";
            case TokKind::PipeEq:             return "|=";
            case TokKind::CaretEq:            return "^=";
            case TokKind::ShlEq:              return "<<=";
            case TokKind::ShrEq:              return ">>=";
            case TokKind::UShrEq:             return ">>>=";
            case TokKind::QuestionQuestionEq: return "??=";
            default:                          return nullptr;
        }
    }

    StmtPtr parseUse() {
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::Use;
        s->pos = peek().pos;
        advance(); // use
        s->name = expect(TokKind::Identifier, "a binding name").text;
        if (accept(TokKind::Colon)) { s->declType = parseType(); s->hasDeclType = true; }
        expect(TokKind::Assign, "'='");
        s->value = parseExpr();
        s->thenBody = parseBlock();
        return s;
    }

    StmtPtr parseTry() {
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::Try;
        s->pos = peek().pos;
        advance(); // try
        s->thenBody = parseBlock();
        while (at(TokKind::KwCatch)) {
            CatchClause c;
            c.pos = peek().pos;
            advance(); // catch
            expect(TokKind::LParen, "'('");
            c.name = expect(TokKind::Identifier, "a catch variable").text;
            expect(TokKind::Colon, "':'");
            c.type = parseType();
            expect(TokKind::RParen, "')'");
            if (accept(TokKind::KwWhen)) {
                expect(TokKind::LParen, "'('");
                c.guard = parseExpr();
                expect(TokKind::RParen, "')'");
            }
            c.body = parseBlock();
            s->catches.push_back(std::move(c));
        }
        if (accept(TokKind::KwFinally)) { s->hasFinally = true; s->finallyBody = parseBlock(); }
        return s;
    }

    StmtPtr parseLet() {
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::Let;
        s->pos = peek().pos;
        s->isMutable = at(TokKind::KwVar);
        advance();
        s->name = expect(TokKind::Identifier, "a binding name").text;
        if (accept(TokKind::Colon)) { s->declType = parseType(); s->hasDeclType = true; }
        expect(TokKind::Assign, "'='");
        s->value = parseExpr();
        accept(TokKind::Semicolon);
        return s;
    }

    StmtPtr parseIf() {
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::If;
        s->pos = peek().pos;
        advance();
        s->value = parseExpr();
        s->thenBody = parseBlock();
        if (accept(TokKind::KwElse)) {
            s->hasElse = true;
            if (at(TokKind::KwIf)) s->elseBody.push_back(parseIf());
            else s->elseBody = parseBlock();
        }
        return s;
    }

    StmtPtr parseWhile() {
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::While;
        s->pos = peek().pos;
        advance();
        s->value = parseExpr();
        s->thenBody = parseBlock();
        return s;
    }

    StmtPtr parseFor() {
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::For;
        s->pos = peek().pos;
        advance(); // for
        s->forBinding = parsePattern();   // `i` or `(a, b)`
        expect(TokKind::KwIn, "'in'");
        s->value = parseExpr();           // iterable
        s->thenBody = parseBlock();
        return s;
    }

    StmtPtr parseReturn() {
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::Return;
        s->pos = peek().pos;
        advance();
        if (!at(TokKind::Semicolon) && !at(TokKind::RBrace)) s->value = parseExpr();
        accept(TokKind::Semicolon);
        return s;
    }

    // ---- expressions (precedence climbing, loosest first) ----

    ExprPtr parseExpr() { return parseCoalesce(); }

    ExprPtr makeBinary(ExprPtr l, const char* op, ExprPtr r, SourcePos p) {
        auto e = mk(ExprKind::Binary, p);
        e->text = op;
        e->lhs = std::move(l);
        e->rhs = std::move(r);
        return e;
    }

    ExprPtr parseCoalesce() {
        auto l = parseOr();
        while (at(TokKind::QuestionQuestion)) { auto p = advance().pos; l = makeBinary(std::move(l), "??", parseOr(), p); }
        return l;
    }
    ExprPtr parseOr() {
        auto l = parseAnd();
        while (at(TokKind::PipePipe)) { auto p = advance().pos; l = makeBinary(std::move(l), "||", parseAnd(), p); }
        return l;
    }
    ExprPtr parseAnd() {
        auto l = parseEquality();
        while (at(TokKind::AmpAmp)) { auto p = advance().pos; l = makeBinary(std::move(l), "&&", parseEquality(), p); }
        return l;
    }
    ExprPtr parseEquality() {
        auto l = parseComparison();
        while (at(TokKind::EqEq) || at(TokKind::NotEq)) {
            const char* op = at(TokKind::EqEq) ? "==" : "!=";
            auto p = advance().pos;
            l = makeBinary(std::move(l), op, parseComparison(), p);
        }
        return l;
    }
    ExprPtr parseComparison() {
        auto l = parseBitOr();
        while (at(TokKind::Lt) || at(TokKind::LtEq) || at(TokKind::Gt) || at(TokKind::GtEq)) {
            const char* op = at(TokKind::Lt) ? "<" : at(TokKind::LtEq) ? "<=" : at(TokKind::Gt) ? ">" : ">=";
            auto p = advance().pos;
            l = makeBinary(std::move(l), op, parseBitOr(), p);
        }
        return l;
    }
    ExprPtr parseBitOr() {
        auto l = parseBitXor();
        while (at(TokKind::Pipe)) { auto p = advance().pos; l = makeBinary(std::move(l), "|", parseBitXor(), p); }
        return l;
    }
    ExprPtr parseBitXor() {
        auto l = parseBitAnd();
        while (at(TokKind::Caret)) { auto p = advance().pos; l = makeBinary(std::move(l), "^", parseBitAnd(), p); }
        return l;
    }
    ExprPtr parseBitAnd() {
        auto l = parseShift();
        while (at(TokKind::Amp)) { auto p = advance().pos; l = makeBinary(std::move(l), "&", parseShift(), p); }
        return l;
    }
    ExprPtr parseShift() {
        auto l = parseAdditive();
        while (at(TokKind::Shl) || at(TokKind::Shr) || at(TokKind::UShr)) {
            const char* op = at(TokKind::Shl) ? "<<" : at(TokKind::Shr) ? ">>" : ">>>";
            auto p = advance().pos;
            l = makeBinary(std::move(l), op, parseAdditive(), p);
        }
        return l;
    }
    ExprPtr parseAdditive() {
        auto l = parseMultiplicative();
        while (at(TokKind::Plus) || at(TokKind::Minus)) {
            const char* op = at(TokKind::Plus) ? "+" : "-";
            auto p = advance().pos;
            l = makeBinary(std::move(l), op, parseMultiplicative(), p);
        }
        return l;
    }
    ExprPtr parseMultiplicative() {
        auto l = parseUnary();
        while (at(TokKind::Star) || at(TokKind::Slash) || at(TokKind::Percent)) {
            const char* op = at(TokKind::Star) ? "*" : at(TokKind::Slash) ? "/" : "%";
            auto p = advance().pos;
            l = makeBinary(std::move(l), op, parseUnary(), p);
        }
        return l;
    }
    ExprPtr parseUnary() {
        if (at(TokKind::Not) || at(TokKind::Minus) || at(TokKind::Tilde)) {
            const char* op = at(TokKind::Not) ? "!" : at(TokKind::Minus) ? "-" : "~";
            auto p = advance().pos;
            auto e = mk(ExprKind::Unary, p);
            e->text = op;
            e->lhs = parseUnary();
            return e;
        }
        return parseRange();
    }
    ExprPtr parseRange() {
        auto l = parsePostfix();
        if (at(TokKind::DotDot) || at(TokKind::DotDotEq)) {
            bool inclusive = at(TokKind::DotDotEq);
            auto p = advance().pos;
            auto e = mk(ExprKind::Range, p);
            e->flag = inclusive;
            e->lhs = std::move(l);
            e->rhs = parsePostfix();
            return e;
        }
        return l;
    }
    ExprPtr parsePostfix() {
        auto e = parsePrimary();
        for (;;) {
            if (at(TokKind::Dot) || at(TokKind::QuestionDot)) {
                bool nullSafe = at(TokKind::QuestionDot);
                auto p = advance().pos;
                auto m = mk(ExprKind::Member, p);
                m->flag = nullSafe;
                m->lhs = std::move(e);
                m->text = expect(TokKind::Identifier, "a member name").text;
                e = std::move(m);
            } else if (at(TokKind::LParen)) {
                auto p = advance().pos;
                auto call = mk(ExprKind::Call, p);
                call->lhs = std::move(e);
                if (!at(TokKind::RParen)) {
                    do { call->args.push_back(parseExpr()); } while (accept(TokKind::Comma));
                }
                expect(TokKind::RParen, "')'");
                e = std::move(call);
            } else if (at(TokKind::LBracket)) {
                auto p = advance().pos;
                auto ix = mk(ExprKind::Index, p);
                ix->lhs = std::move(e);
                do { ix->args.push_back(parseExpr()); } while (accept(TokKind::Comma));
                expect(TokKind::RBracket, "']'");
                e = std::move(ix);
            } else if (at(TokKind::Not)) {
                auto p = advance().pos;
                auto na = mk(ExprKind::NullAssert, p);
                na->lhs = std::move(e);
                e = std::move(na);
            } else {
                break;
            }
        }
        if (at(TokKind::KwWith)) {
            auto p = advance().pos;
            auto w = mk(ExprKind::With, p);
            w->lhs = std::move(e);
            expect(TokKind::LBrace, "'{'");
            if (!at(TokKind::RBrace)) {
                do {
                    FieldInit fi;
                    fi.name = expect(TokKind::Identifier, "a field name").text;
                    expect(TokKind::Assign, "'='");
                    fi.value = parseExpr();
                    w->fields.push_back(std::move(fi));
                } while (accept(TokKind::Comma) && !at(TokKind::RBrace));
            }
            expect(TokKind::RBrace, "'}'");
            e = std::move(w);
        }
        return e;
    }

    // Is the upcoming `( ... )` a lambda parameter list (followed by `=>`)?
    bool isLambdaAhead() const {
        int depth = 0;
        std::size_t j = idx_;
        for (; j < toks_.size(); ++j) {
            TokKind k = toks_[j].kind;
            if (k == TokKind::LParen) ++depth;
            else if (k == TokKind::RParen) { if (--depth == 0) break; }
            else if (k == TokKind::End) return false;
        }
        return j + 1 < toks_.size() && toks_[j + 1].kind == TokKind::Arrow;
    }

    ExprPtr parseLambda() {
        auto p = peek().pos;
        auto e = mk(ExprKind::Lambda, p);
        expect(TokKind::LParen, "'('");
        if (!at(TokKind::RParen)) {
            do {
                Param param;
                param.pos = peek().pos;
                param.name = expect(TokKind::Identifier, "a parameter name").text;
                if (accept(TokKind::Colon)) param.type = parseType();
                e->params.push_back(std::move(param));
            } while (accept(TokKind::Comma));
        }
        expect(TokKind::RParen, "')'");
        expect(TokKind::Arrow, "'=>'");
        if (at(TokKind::LBrace)) { e->flag = true; e->block = parseBlock(); }
        else { e->flag = false; e->lhs = parseExpr(); }
        return e;
    }

    ExprPtr parseIfExpr() {
        auto p = peek().pos;
        expect(TokKind::KwIf, "'if'");
        auto e = mk(ExprKind::IfExpr, p);
        e->lhs = parseExpr();
        expect(TokKind::LBrace, "'{'");
        e->rhs = parseExpr();
        expect(TokKind::RBrace, "'}'");
        expect(TokKind::KwElse, "'else'");
        if (at(TokKind::KwIf)) {
            e->extra = parseIfExpr();
        } else {
            expect(TokKind::LBrace, "'{'");
            e->extra = parseExpr();
            expect(TokKind::RBrace, "'}'");
        }
        return e;
    }

    Pattern parsePattern() {
        Pattern pat;
        pat.pos = peek().pos;
        switch (peek().kind) {
            case TokKind::Identifier: {
                if (peek().text == "_") { advance(); pat.kind = PatKind::Wildcard; return pat; }
                std::string name = advance().text;
                if (at(TokKind::LParen)) {
                    pat.kind = PatKind::Ctor;
                    pat.name = name;
                    advance();
                    if (!at(TokKind::RParen)) {
                        do { pat.sub.push_back(parsePattern()); } while (accept(TokKind::Comma));
                    }
                    expect(TokKind::RParen, "')'");
                } else if (accept(TokKind::Colon)) {
                    pat.kind = PatKind::Binding;
                    pat.name = name;
                    pat.hasType = true;
                    pat.type = parseType();
                } else {
                    pat.kind = PatKind::Binding;
                    pat.name = name;
                }
                return pat;
            }
            case TokKind::LParen: {
                advance();
                pat.kind = PatKind::Tuple;
                if (!at(TokKind::RParen)) {
                    do { pat.sub.push_back(parsePattern()); } while (accept(TokKind::Comma));
                }
                expect(TokKind::RParen, "')'");
                return pat;
            }
            case TokKind::IntLit:  case TokKind::FloatLit: case TokKind::StringLit:
            case TokKind::CharLit: case TokKind::KwTrue:   case TokKind::KwFalse:
            case TokKind::KwNull:
                pat.kind = PatKind::Literal;
                pat.literal = parsePrimary();
                return pat;
            default:
                error("expected a pattern");
                pat.kind = PatKind::Wildcard;
                return pat;
        }
    }

    ExprPtr parseMatch() {
        auto p = peek().pos;
        expect(TokKind::KwMatch, "'match'");
        auto e = mk(ExprKind::Match, p);
        e->lhs = parseExpr(); // scrutinee
        expect(TokKind::LBrace, "'{'");
        while (!at(TokKind::RBrace) && !at(TokKind::End)) {
            MatchArm arm;
            arm.pattern = parsePattern();
            if (accept(TokKind::KwIf)) arm.guard = parseExpr();
            expect(TokKind::Arrow, "'=>'");
            if (at(TokKind::LBrace)) { arm.bodyIsBlock = true; arm.block = parseBlock(); }
            else { arm.body = parseExpr(); }
            e->arms.push_back(std::move(arm));
            accept(TokKind::Comma); // optional separator
        }
        expect(TokKind::RBrace, "'}'");
        return e;
    }

    ExprPtr parsePrimary() {
        SourcePos p = peek().pos;
        switch (peek().kind) {
            case TokKind::IntLit:    { auto e = mk(ExprKind::IntLit, p);    e->text = advance().text; return e; }
            case TokKind::FloatLit:  { auto e = mk(ExprKind::FloatLit, p);  e->text = advance().text; return e; }
            case TokKind::CharLit:   { auto e = mk(ExprKind::CharLit, p);   e->text = advance().text; return e; }
            case TokKind::StringLit: { auto e = mk(ExprKind::StringLit, p); e->text = advance().text; return e; }
            case TokKind::KwTrue:    { advance(); auto e = mk(ExprKind::BoolLit, p); e->boolVal = true;  return e; }
            case TokKind::KwFalse:   { advance(); auto e = mk(ExprKind::BoolLit, p); e->boolVal = false; return e; }
            case TokKind::KwNull:    { advance(); return mk(ExprKind::NullLit, p); }
            case TokKind::KwThis:    { advance(); return mk(ExprKind::This, p); }
            case TokKind::KwSuper:   { advance(); return mk(ExprKind::Super, p); }
            case TokKind::Identifier:{ auto e = mk(ExprKind::Name, p); e->text = advance().text; return e; }
            case TokKind::KwIf:      return parseIfExpr();
            case TokKind::KwMatch:   return parseMatch();
            case TokKind::LBracket: {
                advance();
                auto e = mk(ExprKind::ListLit, p);
                if (!at(TokKind::RBracket)) {
                    do { e->args.push_back(parseExpr()); } while (accept(TokKind::Comma) && !at(TokKind::RBracket));
                }
                expect(TokKind::RBracket, "']'");
                return e;
            }
            case TokKind::LParen: {
                if (isLambdaAhead()) return parseLambda();
                advance();
                auto first = parseExpr();
                if (at(TokKind::Comma)) {
                    auto e = mk(ExprKind::TupleLit, p);
                    e->args.push_back(std::move(first));
                    while (accept(TokKind::Comma) && !at(TokKind::RParen)) e->args.push_back(parseExpr());
                    expect(TokKind::RParen, "')'");
                    return e;
                }
                expect(TokKind::RParen, "')'");
                return first;
            }
            default:
                error("expected an expression");
                { auto e = mk(ExprKind::IntLit, p); e->text = "0"; return e; }
        }
    }
};

} // namespace

CompilationUnit parse(const std::vector<Token>& tokens, DiagnosticBag& diags) {
    Parser parser(tokens, diags);
    return parser.parseUnit();
}

} // namespace mintplayer::polyglot
