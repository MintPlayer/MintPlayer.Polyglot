#include "mintplayer/polyglot/lower.hpp"

namespace mintplayer::polyglot {

namespace {

ir::ExprPtr lowerExpr(const Expr& e);

std::vector<ir::StmtPtr> lowerBlock(const std::vector<StmtPtr>& body);

// Placeholder for surface not yet lowered (records/match/lambdas/...). Never reached by the MVP subset
// the backends currently consume; P5 replaces these with real lowering.
ir::ExprPtr unsupportedExpr(const Expr& e) {
    return std::make_unique<ir::IntLit>(e.pos, e.type, "0");
}

ir::ExprPtr lowerExpr(const Expr& e) {
    switch (e.kind) {
        case ExprKind::IntLit:    return std::make_unique<ir::IntLit>(e.pos, e.type, e.text);
        case ExprKind::FloatLit:  return std::make_unique<ir::FloatLit>(e.pos, e.type, e.text);
        case ExprKind::BoolLit:   return std::make_unique<ir::BoolLit>(e.pos, e.type, e.boolVal);
        case ExprKind::StringLit: return std::make_unique<ir::StrLit>(e.pos, e.type, e.text);
        case ExprKind::Name:      return std::make_unique<ir::Var>(e.pos, e.type, e.text);
        case ExprKind::Unary:     return std::make_unique<ir::Unary>(e.pos, e.type, e.text, lowerExpr(*e.lhs));
        case ExprKind::Binary:    return std::make_unique<ir::Binary>(e.pos, e.type, e.text, lowerExpr(*e.lhs), lowerExpr(*e.rhs));
        case ExprKind::Call: {
            // MVP: callee is a direct name; `print` is resolved to an intrinsic flag.
            std::string callee = (e.lhs && e.lhs->kind == ExprKind::Name) ? e.lhs->text : "";
            auto call = std::make_unique<ir::Call>(e.pos, e.type, callee, callee == "print");
            for (const auto& a : e.args) call->args.push_back(lowerExpr(*a));
            return call;
        }
        default: return unsupportedExpr(e);
    }
}

ir::StmtPtr lowerStmt(const Stmt& s) {
    switch (s.kind) {
        case StmtKind::Let: {
            ir::Type t = s.hasDeclType ? s.declType : (s.value ? s.value->type : TypeRef{});
            return std::make_unique<ir::Let>(s.pos, s.name, s.isMutable, t, lowerExpr(*s.value));
        }
        case StmtKind::Assign:
            return std::make_unique<ir::Assign>(s.pos, lowerExpr(*s.target), s.op, lowerExpr(*s.value));
        case StmtKind::ExprStmt:
            return std::make_unique<ir::ExprStmt>(s.pos, lowerExpr(*s.value));
        case StmtKind::If: {
            auto node = std::make_unique<ir::If>(s.pos, lowerExpr(*s.value));
            node->thenBody = lowerBlock(s.thenBody);
            if (s.hasElse) { node->hasElse = true; node->elseBody = lowerBlock(s.elseBody); }
            return node;
        }
        case StmtKind::While: {
            auto node = std::make_unique<ir::While>(s.pos, lowerExpr(*s.value));
            node->body = lowerBlock(s.thenBody);
            return node;
        }
        case StmtKind::Return:
            return std::make_unique<ir::Return>(s.pos, s.value ? lowerExpr(*s.value) : nullptr);
        default:
            return nullptr; // statements beyond the MVP subset are lowered in P5
    }
}

std::vector<ir::StmtPtr> lowerBlock(const std::vector<StmtPtr>& body) {
    std::vector<ir::StmtPtr> out;
    for (const auto& s : body) if (auto st = lowerStmt(*s)) out.push_back(std::move(st));
    return out;
}

} // namespace

ir::Module lower(const CompilationUnit& unit) {
    ir::Module m;
    for (const auto& fn : unit.functions) {
        ir::Function f;
        f.name = fn.name;
        f.returnType = fn.returnType;
        f.isEntry = (fn.name == "main" && fn.params.empty());
        for (const auto& p : fn.params) f.params.push_back({p.name, p.type});
        f.body = lowerBlock(fn.body);
        m.functions.push_back(std::move(f));
    }
    return m;
}

} // namespace mintplayer::polyglot
