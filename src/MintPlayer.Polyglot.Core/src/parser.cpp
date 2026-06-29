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
            else if (at(TokKind::KwRecord)) unit.records.push_back(parseRecord());
            else if (at(TokKind::KwClass) || at(TokKind::KwAbstract) || at(TokKind::KwOpen) ||
                     at(TokKind::KwSealed)) unit.classes.push_back(parseClass());
            else if (at(TokKind::KwInterface)) unit.interfaces.push_back(parseInterface());
            else if (at(TokKind::KwExtension)) unit.extensions.push_back(parseExtension());
            else if (at(TokKind::KwConst) || at(TokKind::KwLet)) unit.values.push_back(parseValueDecl());
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
    int pendingAngles_ = 0; // leftover '>' borrowed from a split '>>' / '>>>' when closing nested generics

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
               !at(TokKind::KwEnum) && !at(TokKind::KwUnion) && !at(TokKind::KwRecord) &&
               !at(TokKind::KwClass) && !at(TokKind::KwInterface) && !at(TokKind::KwExtension) &&
               !at(TokKind::KwConst) && !at(TokKind::KwLet))
            advance();
        panicked_ = false;
        return !at(TokKind::End);
    }

    std::vector<GenericParam> parseGenericParams() {
        std::vector<GenericParam> gs;
        if (!accept(TokKind::Lt)) return gs;
        do {
            GenericParam g;
            g.name = expect(TokKind::Identifier, "a type parameter").text;
            if (accept(TokKind::Colon)) {
                do { g.bounds.push_back(parseType()); } while (accept(TokKind::Amp));
            }
            gs.push_back(std::move(g));
        } while (accept(TokKind::Comma));
        expectAngleClose();
        return gs;
    }

    // Parameter list between '(' and ')'. Caller consumes the parentheses.
    std::vector<Param> parseParamList() {
        std::vector<Param> ps;
        if (at(TokKind::RParen)) return ps;
        do {
            Param p;
            p.pos = peek().pos;
            p.name = expect(TokKind::Identifier, "a parameter name").text;
            expect(TokKind::Colon, "':'");
            p.type = parseType();
            if (accept(TokKind::Assign)) { p.defaultValue = parseExpr(); p.hasDefault = true; }
            ps.push_back(std::move(p));
        } while (accept(TokKind::Comma));
        return ps;
    }

    static const char* modifierText(TokKind k) {
        switch (k) {
            case TokKind::KwAbstract: return "abstract";
            case TokKind::KwOpen:     return "open";
            case TokKind::KwSealed:   return "sealed";
            case TokKind::KwOverride: return "override";
            case TokKind::KwStatic:   return "static";
            case TokKind::KwPrivate:  return "private";
            case TokKind::KwAsync:    return "async";
            default:                  return "";
        }
    }
    bool atModifier() const {
        return at(TokKind::KwAbstract) || at(TokKind::KwOpen) || at(TokKind::KwOverride) ||
               at(TokKind::KwStatic) || at(TokKind::KwPrivate) || at(TokKind::KwAsync);
    }

    void parseMemberBody(Member& m) {
        if (accept(TokKind::Arrow)) { m.exprBody = parseExpr(); m.hasBody = true; m.exprBodied = true; }
        else if (at(TokKind::LBrace)) { m.body = parseBlock(); m.hasBody = true; m.exprBodied = false; }
        else { accept(TokKind::Semicolon); m.hasBody = false; } // interface stub
    }

    Member parseMember() {
        Member m;
        m.pos = peek().pos;
        while (atModifier()) m.modifiers.push_back(modifierText(advance().kind));

        if (at(TokKind::KwInit)) {
            m.kind = MemberKind::Init;
            m.name = "init";
            advance();
            expect(TokKind::LParen, "'('");
            m.params = parseParamList();
            expect(TokKind::RParen, "')'");
            m.body = parseBlock();
            m.hasBody = true;
            return m;
        }
        if (at(TokKind::KwConst)) {
            m.kind = MemberKind::Const;
            advance();
            m.name = expect(TokKind::Identifier, "a constant name").text;
            expect(TokKind::Colon, "':'");
            m.type = parseType();
            expect(TokKind::Assign, "'='");
            m.init = parseExpr();
            accept(TokKind::Semicolon);
            return m;
        }
        if (at(TokKind::KwOperator) || at(TokKind::KwFn)) {
            m.kind = at(TokKind::KwOperator) ? MemberKind::Operator : MemberKind::Method;
            advance();
            if (m.kind == MemberKind::Operator) expect(TokKind::KwFn, "'fn'");
            m.name = expect(TokKind::Identifier, "a method name").text;
            m.generics = parseGenericParams();
            expect(TokKind::LParen, "'('");
            m.params = parseParamList();
            expect(TokKind::RParen, "')'");
            if (accept(TokKind::Colon)) m.returnType = parseType();
            else m.returnType = namedType("unit");
            parseMemberBody(m);
            return m;
        }
        if (at(TokKind::KwLet) || at(TokKind::KwVar)) {
            m.isMutable = at(TokKind::KwVar);
            advance();
            m.name = expect(TokKind::Identifier, "a member name").text;
            expect(TokKind::Colon, "':'");
            m.type = parseType();
            if (accept(TokKind::Arrow)) { m.kind = MemberKind::Property; m.init = parseExpr(); }
            else if (accept(TokKind::Assign)) { m.kind = MemberKind::Field; m.init = parseExpr(); }
            else { m.kind = MemberKind::Field; }
            accept(TokKind::Semicolon);
            return m;
        }
        error("expected a member");
        m.kind = MemberKind::Field;
        return m;
    }

    std::vector<Member> parseMemberBlock() {
        std::vector<Member> members;
        expect(TokKind::LBrace, "'{'");
        while (!at(TokKind::RBrace) && !at(TokKind::End)) {
            members.push_back(parseMember());
            if (panicked_) { // skip to the next member boundary
                while (!at(TokKind::RBrace) && !at(TokKind::End) && !at(TokKind::Semicolon)) advance();
                accept(TokKind::Semicolon);
                panicked_ = false;
            }
        }
        expect(TokKind::RBrace, "'}'");
        return members;
    }

    RecordDecl parseRecord() {
        RecordDecl d;
        d.pos = peek().pos;
        expect(TokKind::KwRecord, "'record'");
        d.name = expect(TokKind::Identifier, "a record name").text;
        d.generics = parseGenericParams();
        expect(TokKind::LParen, "'('");
        d.fields = parseParamList();
        expect(TokKind::RParen, "')'");
        if (accept(TokKind::Colon)) {
            do { d.bases.push_back(parseType()); } while (accept(TokKind::Comma));
        }
        if (at(TokKind::LBrace)) d.members = parseMemberBlock();
        else accept(TokKind::Semicolon);
        return d;
    }

    ClassDecl parseClass() {
        ClassDecl d;
        d.pos = peek().pos;
        while (at(TokKind::KwAbstract) || at(TokKind::KwOpen) || at(TokKind::KwSealed))
            d.modifiers.push_back(modifierText(advance().kind));
        expect(TokKind::KwClass, "'class'");
        d.name = expect(TokKind::Identifier, "a class name").text;
        d.generics = parseGenericParams();
        if (accept(TokKind::Colon)) {
            do { d.bases.push_back(parseType()); } while (accept(TokKind::Comma));
        }
        d.members = parseMemberBlock();
        return d;
    }

    InterfaceDecl parseInterface() {
        InterfaceDecl d;
        d.pos = peek().pos;
        expect(TokKind::KwInterface, "'interface'");
        d.name = expect(TokKind::Identifier, "an interface name").text;
        d.generics = parseGenericParams();
        if (accept(TokKind::Colon)) {
            do { d.bases.push_back(parseType()); } while (accept(TokKind::Comma));
        }
        d.members = parseMemberBlock();
        return d;
    }

    ExtensionDecl parseExtension() {
        ExtensionDecl d;
        d.pos = peek().pos;
        expect(TokKind::KwExtension, "'extension'");
        expect(TokKind::KwFn, "'fn'");
        d.receiver = parseType();         // e.g. string, i32, List<T>
        expect(TokKind::Dot, "'.'");
        d.name = expect(TokKind::Identifier, "an extension method name").text;
        d.generics = parseGenericParams();
        expect(TokKind::LParen, "'('");
        d.params = parseParamList();
        expect(TokKind::RParen, "')'");
        if (accept(TokKind::Colon)) d.returnType = parseType();
        if (accept(TokKind::Arrow)) { d.exprBody = parseExpr(); d.exprBodied = true; }
        else { d.body = parseBlock(); d.exprBodied = false; }
        return d;
    }

    ValueDecl parseValueDecl() {
        ValueDecl d;
        d.pos = peek().pos;
        d.isConst = at(TokKind::KwConst);
        advance(); // const / let
        d.name = expect(TokKind::Identifier, "a name").text;
        if (accept(TokKind::Colon)) { d.type = parseType(); d.hasType = true; }
        expect(TokKind::Assign, "'='");
        d.init = parseExpr();
        accept(TokKind::Semicolon);
        return d;
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
        if (accept(TokKind::Lt)) { // generic args (nested `>>` split via acceptAngleClose)
            do { t.args.push_back(parseType()); } while (accept(TokKind::Comma));
            expectAngleClose();
        }
        return t;
    }

    // ---- declarations & statements (statement grammar widens in a later P3 increment) ----

    FunctionDecl parseFunction() {
        FunctionDecl fn;
        fn.pos = peek().pos;
        expect(TokKind::KwFn, "'fn'");
        fn.name = expect(TokKind::Identifier, "a function name").text;
        fn.generics = parseGenericParams();
        expect(TokKind::LParen, "'('");
        fn.params = parseParamList();
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
            } else if (at(TokKind::Lt) && (e->kind == ExprKind::Name || e->kind == ExprKind::Member) &&
                       genericCallAhead()) {
                // Generic construction / call: Name<TypeArgs>(args), e.g. List<i32>().
                auto call = mk(ExprKind::Call, e->pos);
                advance(); // '<'
                do { call->typeArgs.push_back(parseType()); } while (accept(TokKind::Comma));
                expectAngleClose();
                expect(TokKind::LParen, "'('");
                call->lhs = std::move(e);
                if (!at(TokKind::RParen)) {
                    do { call->args.push_back(parseExpr()); } while (accept(TokKind::Comma));
                }
                expect(TokKind::RParen, "')'");
                e = std::move(call);
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

    // Close a generic argument/parameter list, splitting `>>`/`>>>` so nested generics close correctly.
    bool acceptAngleClose() {
        if (pendingAngles_ > 0) { --pendingAngles_; return true; }
        if (at(TokKind::Gt)) { advance(); return true; }
        if (at(TokKind::Shr)) { advance(); pendingAngles_ = 1; return true; }
        if (at(TokKind::UShr)) { advance(); pendingAngles_ = 2; return true; }
        return false;
    }
    void expectAngleClose() { if (!acceptAngleClose()) error("expected '>'"); }

    // Lookahead: starting at a `<` after a name, is this `< types... > (` — i.e. a generic construction/
    // call — rather than a less-than comparison? Scans balanced angle/paren depth without consuming.
    bool genericCallAhead() const {
        int angle = 0, paren = 0;
        for (std::size_t i = idx_; i < toks_.size(); ++i) {
            switch (toks_[i].kind) {
                case TokKind::Lt: ++angle; break;
                case TokKind::Gt:
                    if (paren == 0 && --angle == 0) return next(i) == TokKind::LParen;
                    break;
                case TokKind::Shr:
                    if (paren == 0) { angle -= 2; if (angle <= 0) return next(i) == TokKind::LParen; }
                    break;
                case TokKind::UShr:
                    if (paren == 0) { angle -= 3; if (angle <= 0) return next(i) == TokKind::LParen; }
                    break;
                case TokKind::LParen: ++paren; break;
                case TokKind::RParen: if (paren == 0) return false; --paren; break;
                case TokKind::Identifier: case TokKind::Comma: case TokKind::Dot:
                case TokKind::Question:    case TokKind::Arrow:
                    break; // tokens that can appear inside a type argument list
                default:
                    return false; // anything else => this `<` is a comparison
            }
        }
        return false;
    }
    TokKind next(std::size_t i) const { return i + 1 < toks_.size() ? toks_[i + 1].kind : TokKind::End; }

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

    // `x => expr` / `x => { … }` — a single untyped parameter without parentheses. A typed or
    // multi/zero-parameter lambda still requires parens (`(x: T) => …`, `(a, b) => …`, `() => …`).
    ExprPtr parseBareLambda() {
        auto p = peek().pos;
        auto e = mk(ExprKind::Lambda, p);
        Param param;
        param.pos = peek().pos;
        param.name = expect(TokKind::Identifier, "a parameter name").text;
        e->params.push_back(std::move(param));
        expect(TokKind::Arrow, "'=>'");
        if (at(TokKind::LBrace)) { e->flag = true; e->block = parseBlock(); }
        else { e->flag = false; e->lhs = parseExpr(); }
        return e;
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
            case TokKind::InterpStart: {
                auto e = mk(ExprKind::InterpString, p);
                e->chunks.push_back(advance().text); // leading chunk
                for (;;) {
                    e->args.push_back(parseExpr());   // a `${ ... }` hole
                    if (at(TokKind::InterpMid)) { e->chunks.push_back(advance().text); continue; }
                    e->chunks.push_back(expect(TokKind::InterpEnd, "end of interpolated string").text);
                    break;
                }
                return e;
            }
            case TokKind::KwTrue:    { advance(); auto e = mk(ExprKind::BoolLit, p); e->boolVal = true;  return e; }
            case TokKind::KwFalse:   { advance(); auto e = mk(ExprKind::BoolLit, p); e->boolVal = false; return e; }
            case TokKind::KwNull:    { advance(); return mk(ExprKind::NullLit, p); }
            case TokKind::KwThis:    { advance(); return mk(ExprKind::This, p); }
            case TokKind::KwSuper:   { advance(); return mk(ExprKind::Super, p); }
            case TokKind::Identifier:
                if (peek(1).kind == TokKind::Arrow) return parseBareLambda(); // `x => …`
                { auto e = mk(ExprKind::Name, p); e->text = advance().text; return e; }
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
