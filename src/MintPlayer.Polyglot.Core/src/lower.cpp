#include "mintplayer/polyglot/lower.hpp"

#include <unordered_set>

namespace mintplayer::polyglot {

namespace {

class Lowerer {
public:
    explicit Lowerer(const CompilationUnit& unit) {
        // Names that denote a constructible type, so `Name(args)` lowers to a construction, not a call.
        for (const auto& r : unit.records) typeNames_.insert(r.name);
        for (const auto& c : unit.classes) typeNames_.insert(c.name);
    }

    ir::Module run(const CompilationUnit& unit) {
        ir::Module m;
        for (const auto& r : unit.records) {
            ir::Record rec;
            rec.name = r.name;
            for (const auto& f : r.fields) rec.fields.push_back({f.name, f.type});
            m.records.push_back(std::move(rec));
        }
        for (const auto& fn : unit.functions) {
            ir::Function f;
            f.name = fn.name;
            f.returnType = fn.returnType;
            f.isEntry = (fn.name == "main" && fn.params.empty());
            for (const auto& p : fn.params) f.params.push_back({p.name, p.type});
            f.body = block(fn.body);
            m.functions.push_back(std::move(f));
        }
        return m;
    }

private:
    std::unordered_set<std::string> typeNames_;

    ir::ExprPtr expr(const Expr& e) {
        switch (e.kind) {
            case ExprKind::IntLit:    return std::make_unique<ir::IntLit>(e.pos, e.type, e.text);
            case ExprKind::FloatLit:  return std::make_unique<ir::FloatLit>(e.pos, e.type, e.text);
            case ExprKind::BoolLit:   return std::make_unique<ir::BoolLit>(e.pos, e.type, e.boolVal);
            case ExprKind::StringLit: return std::make_unique<ir::StrLit>(e.pos, e.type, e.text);
            case ExprKind::Name:      return std::make_unique<ir::Var>(e.pos, e.type, e.text);
            case ExprKind::Unary:     return std::make_unique<ir::Unary>(e.pos, e.type, e.text, expr(*e.lhs));
            case ExprKind::Binary:    return std::make_unique<ir::Binary>(e.pos, e.type, e.text, expr(*e.lhs), expr(*e.rhs));
            case ExprKind::Member:    return std::make_unique<ir::Member>(e.pos, e.type, expr(*e.lhs), e.text, e.flag);
            case ExprKind::Call: {
                std::string callee = (e.lhs && e.lhs->kind == ExprKind::Name) ? e.lhs->text : "";
                if (!callee.empty() && typeNames_.count(callee)) { // construction
                    auto n = std::make_unique<ir::New>(e.pos, e.type, callee);
                    for (const auto& a : e.args) n->args.push_back(expr(*a));
                    return n;
                }
                auto call = std::make_unique<ir::Call>(e.pos, e.type, callee, callee == "print");
                for (const auto& a : e.args) call->args.push_back(expr(*a));
                return call;
            }
            default: return std::make_unique<ir::IntLit>(e.pos, e.type, "0"); // surface not yet lowered (P5+)
        }
    }

    ir::StmtPtr stmt(const Stmt& s) {
        switch (s.kind) {
            case StmtKind::Let: {
                ir::Type t = s.hasDeclType ? s.declType : (s.value ? s.value->type : TypeRef{});
                return std::make_unique<ir::Let>(s.pos, s.name, s.isMutable, t, expr(*s.value));
            }
            case StmtKind::Assign:
                return std::make_unique<ir::Assign>(s.pos, expr(*s.target), s.op, expr(*s.value));
            case StmtKind::ExprStmt:
                return std::make_unique<ir::ExprStmt>(s.pos, expr(*s.value));
            case StmtKind::If: {
                auto node = std::make_unique<ir::If>(s.pos, expr(*s.value));
                node->thenBody = block(s.thenBody);
                if (s.hasElse) { node->hasElse = true; node->elseBody = block(s.elseBody); }
                return node;
            }
            case StmtKind::While: {
                auto node = std::make_unique<ir::While>(s.pos, expr(*s.value));
                node->body = block(s.thenBody);
                return node;
            }
            case StmtKind::Return:
                return std::make_unique<ir::Return>(s.pos, s.value ? expr(*s.value) : nullptr);
            default:
                return nullptr; // statements beyond the current surface are lowered in later P5 increments
        }
    }

    std::vector<ir::StmtPtr> block(const std::vector<StmtPtr>& body) {
        std::vector<ir::StmtPtr> out;
        for (const auto& s : body) if (auto st = stmt(*s)) out.push_back(std::move(st));
        return out;
    }
};

} // namespace

ir::Module lower(const CompilationUnit& unit) {
    Lowerer lowerer(unit);
    return lowerer.run(unit);
}

} // namespace mintplayer::polyglot
