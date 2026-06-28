#include "mintplayer/polyglot/parser.hpp"

namespace mintplayer::polyglot {

namespace {

// Recursive-descent parser over the MVP token stream. Expression parsing is precedence-climbing.
class Parser {
public:
    Parser(const std::vector<Token>& tokens, DiagnosticBag& diags)
        : toks_(tokens), diags_(diags) {}

    CompilationUnit parseUnit() {
        CompilationUnit unit;
        while (!at(TokKind::End)) {
            if (at(TokKind::KwFn)) {
                unit.functions.push_back(parseFunction());
            } else {
                error("expected a function declaration");
                if (!recoverToTopLevel()) break;
            }
        }
        return unit;
    }

private:
    const std::vector<Token>& toks_;
    DiagnosticBag& diags_;
    std::size_t idx_ = 0;
    bool panicked_ = false;

    const Token& peek(std::size_t ahead = 0) const {
        std::size_t i = idx_ + ahead;
        return i < toks_.size() ? toks_[i] : toks_.back(); // back() is always End
    }
    bool at(TokKind k) const { return peek().kind == k; }
    const Token& advance() { return toks_[idx_ < toks_.size() - 1 ? idx_++ : idx_]; }

    bool accept(TokKind k) {
        if (at(k)) { advance(); return true; }
        return false;
    }
    Token expect(TokKind k, const char* what) {
        if (at(k)) return advance();
        error(std::string("expected ") + what);
        return peek();
    }
    void error(std::string msg) {
        if (!panicked_) diags_.error(peek().pos, std::move(msg));
        panicked_ = true;
    }

    // Skip to the start of the next top-level function after an error.
    bool recoverToTopLevel() {
        while (!at(TokKind::End) && !at(TokKind::KwFn)) advance();
        panicked_ = false;
        return !at(TokKind::End);
    }

    Ty parseType() {
        if (at(TokKind::Identifier)) {
            std::string name = advance().text;
            if (name == "i32") return Ty::I32;
            if (name == "f64") return Ty::F64;
            if (name == "bool") return Ty::Bool;
            if (name == "string") return Ty::String;
            if (name == "unit") return Ty::Unit;
            error("unknown type '" + name + "'");
            return Ty::Unknown;
        }
        error("expected a type");
        return Ty::Unknown;
    }

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

        fn.returnType = Ty::Unit;
        if (accept(TokKind::Colon)) fn.returnType = parseType();

        if (accept(TokKind::Arrow)) {
            // Expression body: `=> expr` desugars to a single `return expr;`.
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
                // Recover to the next statement/closing brace.
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
        if (at(TokKind::KwReturn)) return parseReturn();

        // Assignment `name = expr;` (MVP target is a bare name), else an expression statement.
        if (at(TokKind::Identifier) && peek(1).kind == TokKind::Assign) {
            auto s = std::make_unique<Stmt>();
            s->kind = StmtKind::Assign;
            s->pos = peek().pos;
            s->name = advance().text; // name
            advance();                // '='
            s->value = parseExpr();
            accept(TokKind::Semicolon); // ';' is an optional separator (statements end at newlines)
            return s;
        }

        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::ExprStmt;
        s->pos = peek().pos;
        s->value = parseExpr();
        accept(TokKind::Semicolon);
        return s;
    }

    StmtPtr parseLet() {
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::Let;
        s->pos = peek().pos;
        s->isMutable = at(TokKind::KwVar);
        advance(); // let/var
        s->name = expect(TokKind::Identifier, "a binding name").text;
        if (accept(TokKind::Colon)) s->declType = parseType();
        expect(TokKind::Assign, "'='");
        s->value = parseExpr();
        accept(TokKind::Semicolon); // optional separator
        return s;
    }

    StmtPtr parseIf() {
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::If;
        s->pos = peek().pos;
        advance(); // if
        s->value = parseExpr(); // condition
        s->thenBody = parseBlock();
        if (accept(TokKind::KwElse)) {
            s->hasElse = true;
            if (at(TokKind::KwIf)) {
                // `else if` — nest the inner if as the sole statement of the else block.
                s->elseBody.push_back(parseIf());
            } else {
                s->elseBody = parseBlock();
            }
        }
        return s;
    }

    StmtPtr parseWhile() {
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::While;
        s->pos = peek().pos;
        advance(); // while
        s->value = parseExpr();
        s->thenBody = parseBlock();
        return s;
    }

    StmtPtr parseReturn() {
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::Return;
        s->pos = peek().pos;
        advance(); // return
        // A bare `return` is followed by `}` or the next statement; otherwise it returns an expression.
        if (!at(TokKind::Semicolon) && !at(TokKind::RBrace)) s->value = parseExpr();
        accept(TokKind::Semicolon); // optional separator
        return s;
    }

    // ---- expressions (precedence climbing) ----

    ExprPtr parseExpr() { return parseOr(); }

    ExprPtr binary(ExprPtr lhs, const char* op, ExprPtr rhs, SourcePos pos) {
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::Binary;
        e->pos = pos;
        e->text = op;
        e->lhs = std::move(lhs);
        e->rhs = std::move(rhs);
        return e;
    }

    ExprPtr parseOr() {
        auto lhs = parseAnd();
        while (at(TokKind::PipePipe)) { auto p = advance().pos; lhs = binary(std::move(lhs), "||", parseAnd(), p); }
        return lhs;
    }
    ExprPtr parseAnd() {
        auto lhs = parseEquality();
        while (at(TokKind::AmpAmp)) { auto p = advance().pos; lhs = binary(std::move(lhs), "&&", parseEquality(), p); }
        return lhs;
    }
    ExprPtr parseEquality() {
        auto lhs = parseComparison();
        while (at(TokKind::EqEq) || at(TokKind::NotEq)) {
            const char* op = at(TokKind::EqEq) ? "==" : "!=";
            auto p = advance().pos;
            lhs = binary(std::move(lhs), op, parseComparison(), p);
        }
        return lhs;
    }
    ExprPtr parseComparison() {
        auto lhs = parseAdditive();
        while (at(TokKind::Lt) || at(TokKind::LtEq) || at(TokKind::Gt) || at(TokKind::GtEq)) {
            const char* op = at(TokKind::Lt) ? "<" : at(TokKind::LtEq) ? "<=" : at(TokKind::Gt) ? ">" : ">=";
            auto p = advance().pos;
            lhs = binary(std::move(lhs), op, parseAdditive(), p);
        }
        return lhs;
    }
    ExprPtr parseAdditive() {
        auto lhs = parseMultiplicative();
        while (at(TokKind::Plus) || at(TokKind::Minus)) {
            const char* op = at(TokKind::Plus) ? "+" : "-";
            auto p = advance().pos;
            lhs = binary(std::move(lhs), op, parseMultiplicative(), p);
        }
        return lhs;
    }
    ExprPtr parseMultiplicative() {
        auto lhs = parseUnary();
        while (at(TokKind::Star) || at(TokKind::Slash) || at(TokKind::Percent)) {
            const char* op = at(TokKind::Star) ? "*" : at(TokKind::Slash) ? "/" : "%";
            auto p = advance().pos;
            lhs = binary(std::move(lhs), op, parseUnary(), p);
        }
        return lhs;
    }
    ExprPtr parseUnary() {
        if (at(TokKind::Not) || at(TokKind::Minus)) {
            const char* op = at(TokKind::Not) ? "!" : "-";
            auto p = advance().pos;
            auto e = std::make_unique<Expr>();
            e->kind = ExprKind::Unary;
            e->pos = p;
            e->text = op;
            e->lhs = parseUnary();
            return e;
        }
        return parsePostfix();
    }
    ExprPtr parsePostfix() {
        auto e = parsePrimary();
        // A name immediately followed by '(' is a call (MVP: callee is always a bare name).
        if (e && e->kind == ExprKind::Name && at(TokKind::LParen)) {
            auto call = std::make_unique<Expr>();
            call->kind = ExprKind::Call;
            call->pos = e->pos;
            call->text = e->text; // callee name
            advance(); // '('
            if (!at(TokKind::RParen)) {
                do { call->args.push_back(parseExpr()); } while (accept(TokKind::Comma));
            }
            expect(TokKind::RParen, "')'");
            return call;
        }
        return e;
    }
    ExprPtr parsePrimary() {
        auto e = std::make_unique<Expr>();
        e->pos = peek().pos;
        switch (peek().kind) {
            case TokKind::IntLit:   e->kind = ExprKind::IntLit;   e->text = advance().text; return e;
            case TokKind::FloatLit: e->kind = ExprKind::FloatLit; e->text = advance().text; return e;
            case TokKind::StringLit:e->kind = ExprKind::StringLit;e->text = advance().text; return e;
            case TokKind::KwTrue:   e->kind = ExprKind::BoolLit;  e->boolVal = true;  advance(); return e;
            case TokKind::KwFalse:  e->kind = ExprKind::BoolLit;  e->boolVal = false; advance(); return e;
            case TokKind::Identifier: e->kind = ExprKind::Name;   e->text = advance().text; return e;
            case TokKind::LParen: {
                advance();
                auto inner = parseExpr();
                expect(TokKind::RParen, "')'");
                return inner;
            }
            default:
                error("expected an expression");
                e->kind = ExprKind::IntLit;
                e->text = "0";
                return e;
        }
    }
};

} // namespace

CompilationUnit parse(const std::vector<Token>& tokens, DiagnosticBag& diags) {
    Parser parser(tokens, diags);
    return parser.parseUnit();
}

} // namespace mintplayer::polyglot
