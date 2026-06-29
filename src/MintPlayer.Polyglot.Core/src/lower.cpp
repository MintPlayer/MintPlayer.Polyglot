#include "mintplayer/polyglot/lower.hpp"

#include <unordered_map>
#include <unordered_set>

namespace mintplayer::polyglot {

namespace {

class Lowerer {
public:
    explicit Lowerer(const CompilationUnit& unit) {
        // Names that denote a constructible type, so `Name(args)` lowers to a construction, not a call.
        typeNames_.insert("Error"); // core builtin exception root (System.Exception / JS Error)
        for (const auto& r : unit.records) typeNames_.insert(r.name);
        for (const auto& c : unit.classes) typeNames_.insert(c.name);
        for (const auto& e : unit.enums) for (const auto& c : e.cases) enumCases_[e.name].insert(c.name);
        for (const auto& u : unit.unions)
            for (const auto& c : u.cases) {
                caseUnion_[c.name] = u.name;
                unionCases_[u.name].insert(c.name);
                for (const auto& f : c.params) caseFields_[c.name].push_back(f.name);
            }
    }

    ir::Module run(const CompilationUnit& unit) {
        ir::Module m;
        for (const auto& e : unit.enums) {
            ir::Enum ie;
            ie.name = e.name;
            long long next = 0;
            for (const auto& c : e.cases) { long long v = c.hasValue ? c.value : next; ie.cases.push_back({c.name, v}); next = v + 1; }
            m.enums.push_back(std::move(ie));
        }
        for (const auto& u : unit.unions) {
            ir::Union iu;
            iu.name = u.name;
            for (const auto& c : u.cases) {
                ir::UnionCase ic;
                ic.name = c.name;
                for (const auto& f : c.params) ic.fields.push_back({f.name, f.type});
                iu.cases.push_back(std::move(ic));
            }
            m.unions.push_back(std::move(iu));
        }
        for (const auto& r : unit.records) {
            ir::Record rec;
            rec.name = r.name;
            for (const auto& f : r.fields) rec.fields.push_back({f.name, f.type});
            for (const auto& mem : r.members)
                if (mem.kind == MemberKind::Method || mem.kind == MemberKind::Operator || mem.kind == MemberKind::Property)
                    rec.methods.push_back(method(mem));
            m.records.push_back(std::move(rec));
        }
        for (const auto& c : unit.classes) m.classes.push_back(lowerClass(c));
        for (const auto& fn : unit.functions) {
            ir::Function f;
            f.name = fn.name;
            f.returnType = fn.returnType;
            f.isEntry = (fn.name == "main" && fn.params.empty());
            for (const auto& p : fn.params) f.params.push_back({p.name, p.type});
            sawYield_ = false;
            f.body = block(fn.body);
            f.isIterator = sawYield_;
            m.functions.push_back(std::move(f));
        }
        return m;
    }

private:
    bool sawYield_ = false; // set while lowering a function body that contains `yield`
    std::unordered_set<std::string> typeNames_;
    std::unordered_map<std::string, std::unordered_set<std::string>> enumCases_;
    std::unordered_map<std::string, std::string> caseUnion_;                       // case -> union
    std::unordered_map<std::string, std::vector<std::string>> caseFields_;         // case -> field names
    std::unordered_map<std::string, std::unordered_set<std::string>> unionCases_;  // union -> case names

    ir::Pattern pattern(const Pattern& p, const std::string& scrutEnum, const std::string& scrutUnion) {
        ir::Pattern ip;
        switch (p.kind) {
            case PatKind::Wildcard: ip.kind = ir::PatternKind::Wildcard; break;
            case PatKind::Literal:  ip.kind = ir::PatternKind::Literal; if (p.literal) ip.literal = expr(*p.literal); break;
            case PatKind::Ctor: {
                ip.kind = ir::PatternKind::Ctor;
                ip.ctorCase = p.name;
                const auto fit = caseFields_.find(p.name);
                for (std::size_t i = 0; i < p.sub.size(); ++i) {
                    if (p.sub[i].kind != PatKind::Binding) continue; // nested patterns widen later
                    std::string field = (fit != caseFields_.end() && i < fit->second.size()) ? fit->second[i] : p.sub[i].name;
                    ip.binders.push_back({field, p.sub[i].name});
                }
                break;
            }
            case PatKind::Binding:
                if (!scrutEnum.empty() && enumCases_.at(scrutEnum).count(p.name)) {
                    ip.kind = ir::PatternKind::EnumCase; ip.enumType = scrutEnum; ip.enumCase = p.name;
                } else if (!scrutUnion.empty() && unionCases_.at(scrutUnion).count(p.name)) {
                    ip.kind = ir::PatternKind::Ctor; ip.ctorCase = p.name; // payload-free case
                } else {
                    ip.kind = ir::PatternKind::Binding; ip.binding = p.name;
                }
                break;
            default: ip.kind = ir::PatternKind::Wildcard; break;
        }
        return ip;
    }

    ir::ExprPtr matchExpr(const Expr& e) {
        auto m = std::make_unique<ir::Match>(e.pos, e.type, expr(*e.lhs));
        std::string scrutEnum, scrutUnion;
        if (e.lhs->type.kind == TypeRef::Kind::Named) {
            if (enumCases_.count(e.lhs->type.name)) scrutEnum = e.lhs->type.name;
            else if (unionCases_.count(e.lhs->type.name)) scrutUnion = e.lhs->type.name;
        }
        for (const auto& arm : e.arms) {
            ir::MatchArm ia;
            ia.pattern = pattern(arm.pattern, scrutEnum, scrutUnion);
            if (arm.guard) ia.guard = expr(*arm.guard);
            ia.body = (!arm.bodyIsBlock && arm.body) ? expr(*arm.body)
                                                     : std::make_unique<ir::IntLit>(e.pos, e.type, "0"); // block arms: P5-4b
            m->arms.push_back(std::move(ia));
        }
        return m;
    }

    static std::string operatorSymbol(const std::string& method) {
        if (method == "plus") return "+";
        if (method == "minus") return "-";
        if (method == "times") return "*";
        if (method == "div") return "/";
        if (method == "rem") return "%";
        if (method == "eq") return "==";
        if (method == "lt") return "<";
        if (method == "le") return "<=";
        if (method == "gt") return ">";
        if (method == "ge") return ">=";
        if (method == "neg") return "-"; // unary
        return method;
    }

    ir::Class lowerClass(const ClassDecl& c) {
        ir::Class ic;
        ic.name = c.name;
        ic.bases = c.bases;
        for (const auto& mem : c.members) {
            switch (mem.kind) {
                case MemberKind::Field:
                case MemberKind::Const: {
                    ir::ClassField f;
                    f.name = mem.name;
                    f.isMutable = mem.isMutable;
                    f.type = mem.type;
                    if (mem.init) f.init = expr(*mem.init);
                    ic.fields.push_back(std::move(f));
                    break;
                }
                case MemberKind::Init:
                    ic.hasInit = true;
                    for (const auto& p : mem.params) ic.initParams.push_back({p.name, p.type});
                    // A `super(...)` call carries the base-ctor args; hoist it out of the body so each
                    // backend can place it idiomatically (C# `: base(...)`, TS leading `super(...);`).
                    for (const auto& st : mem.body) {
                        if (st->kind == StmtKind::ExprStmt && st->value &&
                            st->value->kind == ExprKind::Call && st->value->lhs &&
                            st->value->lhs->kind == ExprKind::Super) {
                            ic.hasSuper = true;
                            for (const auto& a : st->value->args) ic.superArgs.push_back(expr(*a));
                            continue;
                        }
                        if (auto s = stmt(*st)) ic.initBody.push_back(std::move(s));
                    }
                    break;
                case MemberKind::Method:
                case MemberKind::Operator:
                case MemberKind::Property:
                    ic.methods.push_back(method(mem));
                    break;
            }
        }
        return ic;
    }

    ir::Method method(const Member& m) {
        ir::Method im;
        im.name = m.name;
        if (m.kind == MemberKind::Property) {
            im.kind = ir::MethodKind::Property;
            im.returnType = m.type;
            im.exprBodied = true;
            if (m.init) im.exprBody = expr(*m.init);
            return im;
        }
        im.kind = (m.kind == MemberKind::Operator) ? ir::MethodKind::Operator : ir::MethodKind::Method;
        if (m.kind == MemberKind::Operator) im.opSymbol = operatorSymbol(m.name);
        im.returnType = m.returnType;
        for (const auto& p : m.params) im.params.push_back({p.name, p.type});
        if (m.exprBodied && m.exprBody) { im.exprBodied = true; im.exprBody = expr(*m.exprBody); }
        else { im.exprBodied = false; im.body = block(m.body); }
        return im;
    }

    ir::ExprPtr expr(const Expr& e) {
        switch (e.kind) {
            case ExprKind::IntLit:    return std::make_unique<ir::IntLit>(e.pos, e.type, e.text);
            case ExprKind::FloatLit:  return std::make_unique<ir::FloatLit>(e.pos, e.type, e.text);
            case ExprKind::BoolLit:   return std::make_unique<ir::BoolLit>(e.pos, e.type, e.boolVal);
            case ExprKind::StringLit: return std::make_unique<ir::StrLit>(e.pos, e.type, e.text);
            case ExprKind::Name:
                if (auto u = caseUnion_.find(e.text); u != caseUnion_.end()) // bare payload-free union case
                    return std::make_unique<ir::MakeCase>(e.pos, e.type, u->second, e.text);
                return std::make_unique<ir::Var>(e.pos, e.type, e.text);
            case ExprKind::This:      return std::make_unique<ir::This>(e.pos, e.type);
            case ExprKind::Unary:     return std::make_unique<ir::Unary>(e.pos, e.type, e.text, expr(*e.lhs));
            case ExprKind::Binary:    return std::make_unique<ir::Binary>(e.pos, e.type, e.text, expr(*e.lhs), expr(*e.rhs));
            case ExprKind::Member:    return std::make_unique<ir::Member>(e.pos, e.type, expr(*e.lhs), e.text, e.flag);
            case ExprKind::Match:     return matchExpr(e);
            case ExprKind::Call: {
                if (e.lhs && e.lhs->kind == ExprKind::Member) { // method call `obj.method(args)`
                    auto mc = std::make_unique<ir::MethodCall>(e.pos, e.type, expr(*e.lhs->lhs), e.lhs->text);
                    for (const auto& a : e.args) mc->args.push_back(expr(*a));
                    return mc;
                }
                std::string callee = (e.lhs && e.lhs->kind == ExprKind::Name) ? e.lhs->text : "";
                if (!callee.empty() && typeNames_.count(callee)) { // record/class construction
                    auto n = std::make_unique<ir::New>(e.pos, e.type, callee);
                    for (const auto& a : e.args) n->args.push_back(expr(*a));
                    return n;
                }
                if (auto u = caseUnion_.find(callee); u != caseUnion_.end()) { // union case construction
                    auto mc = std::make_unique<ir::MakeCase>(e.pos, e.type, u->second, callee);
                    const auto& fields = caseFields_[callee];
                    for (std::size_t i = 0; i < e.args.size(); ++i)
                        mc->fields.push_back({i < fields.size() ? fields[i] : "_" + std::to_string(i), expr(*e.args[i])});
                    return mc;
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
            case StmtKind::For: {
                // Tuple-destructuring bindings widen with collections later; a single binding for now.
                std::string binding = s.forBinding.kind == PatKind::Binding ? s.forBinding.name : "_";
                auto node = std::make_unique<ir::For>(s.pos, binding);
                if (s.value && s.value->kind == ExprKind::Range) {
                    node->isRange = true;
                    node->rangeStart = expr(*s.value->lhs);
                    node->rangeEnd = expr(*s.value->rhs);
                    node->inclusive = s.value->flag;
                } else if (s.value) {
                    node->iterable = expr(*s.value);
                }
                node->body = block(s.thenBody);
                return node;
            }
            case StmtKind::Return:
                return std::make_unique<ir::Return>(s.pos, s.value ? expr(*s.value) : nullptr);
            case StmtKind::Yield:
                sawYield_ = true;
                return std::make_unique<ir::Yield>(s.pos, s.value ? expr(*s.value) : nullptr);
            case StmtKind::Throw:
                return std::make_unique<ir::Throw>(s.pos, s.value ? expr(*s.value) : nullptr);
            case StmtKind::Try: {
                auto node = std::make_unique<ir::Try>(s.pos);
                node->body = block(s.thenBody);
                for (const auto& c : s.catches) {
                    ir::Catch ic;
                    ic.type = c.type;
                    ic.binding = c.name;
                    if (c.guard) ic.guard = expr(*c.guard);
                    ic.body = block(c.body);
                    node->catches.push_back(std::move(ic));
                }
                if (s.hasFinally) { node->hasFinally = true; node->finallyBody = block(s.finallyBody); }
                return node;
            }
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
