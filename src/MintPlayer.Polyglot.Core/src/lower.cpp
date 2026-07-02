#include "mintplayer/polyglot/lower.hpp"

#include <unordered_map>
#include <unordered_set>

namespace mintplayer::polyglot {

namespace {

// Strip a numeric literal's width suffix (`100u8`, `0i64`, `1.5f32`) — the type rides on the expr; the
// backends re-add a target-appropriate suffix (C# `L`/`UL`, TS BigInt `n`) from that type.
bool isPrimitiveTypeName(const std::string& n) {
    return n == "i8" || n == "i16" || n == "i32" || n == "i64" || n == "u8" || n == "u16" || n == "u32" ||
           n == "u64" || n == "f32" || n == "f64" || n == "bool" || n == "char" || n == "string";
}

std::string stripNumericSuffix(const std::string& text) {
    static const char* sfx[] = {"i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64", "f", "d"};
    for (const char* s : sfx) {
        std::size_t n = std::char_traits<char>::length(s);
        if (text.size() > n && text.compare(text.size() - n, n, s) == 0) return text.substr(0, text.size() - n);
    }
    return text;
}

class Lowerer {
public:
    explicit Lowerer(const CompilationUnit& unit) {
        // Names that denote a constructible type, so `Name(args)` lowers to a construction, not a call.
        // (Error/Iterable arrive as core-prelude `extern class`es in unit.classes — no special-case here.)
        for (const auto& r : unit.records) typeNames_.insert(r.name);
        for (const auto& c : unit.classes) typeNames_.insert(c.name);
        for (const auto& e : unit.enums) for (const auto& c : e.cases) enumCases_[e.name].insert(c.name);
        for (const auto& u : unit.unions)
            for (const auto& c : u.cases) {
                caseUnion_[c.name] = u.name;
                unionCases_[u.name].insert(c.name);
                for (const auto& f : c.params) caseFields_[c.name].push_back(f.name);
            }
        for (const auto& e : unit.extensions) extensions_[e.receiver.name].insert(e.name);
        for (const auto& fn : unit.functions) freeFns_.insert(fn.name); // top-level fns -> C# qualifies as Program.fn
        // Per-target FFI bindings on std types (e.g. List.add): keyed "Type.member" so a call/access on a
        // receiver of that type lowers to a substituted template instead of a member call.
        for (const auto& c : unit.classes)
            for (const auto& mem : c.members)
                if (!mem.bindings.empty()) bindings_[c.name + "." + mem.name] = &mem.bindings;
        for (const auto& r : unit.records)
            for (const auto& mem : r.members)
                if (!mem.bindings.empty()) bindings_[r.name + "." + mem.name] = &mem.bindings;
        // A bound extension method (`extension fn string.toUpper() { actual… }`): a method on an existing
        // type — keyed like any member binding, so a call `s.toUpper()` lowers to the substituted template.
        for (const auto& e : unit.extensions)
            if (!e.bindings.empty()) bindings_[e.receiver.name + "." + e.name] = &e.bindings;
        // Bound constructors on an `extern class` (its `init` has binding arms): `Type(args)` lowers to a
        // substituted ctor template instead of a plain `new Type(...)`.
        for (const auto& c : unit.classes)
            for (const auto& mem : c.members)
                if (mem.kind == MemberKind::Init && !mem.bindings.empty()) ctorBindings_[c.name] = &mem.bindings;
        // Named base types, so a binding/member inherited from a base resolves on a subclass receiver
        // (e.g. `Error.message`, declared on the core-prelude `extern class Error`, on a `: Error` subclass).
        for (const auto& c : unit.classes) for (const auto& b : c.bases) if (!b.name.empty()) bases_[c.name].push_back(b.name);
        for (const auto& r : unit.records) for (const auto& b : r.bases) if (!b.name.empty()) bases_[r.name].push_back(b.name);
        // Module facts precomputed onto IR nodes (P19), so the emit layer never scans the module: record
        // names (structural `==`), record decls (the `with` ctor-rebuild reads field order), and the types
        // declaring an `operator fn get` (a target without `[]` overloading calls `.get(i)`).
        for (const auto& r : unit.records) { recordNames_.insert(r.name); records_[r.name] = &r; }
        auto noteIndexer = [&](const std::string& name, const std::vector<Member>& ms) {
            for (const auto& mem : ms)
                if (mem.kind == MemberKind::Operator && mem.name == "get") indexerTypes_.insert(name);
        };
        for (const auto& r : unit.records) noteIndexer(r.name, r.members);
        for (const auto& c : unit.classes) noteIndexer(c.name, c.members);
    }

    // Build an ir::Bound from a receiver, args and a "Type.member" binding (picks the per-target arms).
    ir::ExprPtr makeBound(const TypeRef& type, SourcePos pos, const Expr* recv,
                          const std::vector<TargetBinding>& arms, const std::vector<ExprPtr>* args) {
        auto b = std::make_unique<ir::Bound>(pos, type);
        if (recv) b->receiver = expr(*recv); // null for a static member binding (template uses only $0,$1,…)
        if (args) for (const auto& a : *args) b->args.push_back(expr(*a));
        for (const auto& arm : arms) {
            if (arm.target == "csharp") b->csTemplate = arm.code;
            else if (arm.target == "typescript") b->tsTemplate = arm.code;
            else if (arm.target == "python") b->pyTemplate = arm.code;
        }
        return b;
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
            iu.generics = generics(u.generics);
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
            rec.generics = generics(r.generics);
            for (const auto& b : r.bases) rec.bases.push_back(b);
            for (const auto& f : r.fields) rec.fields.push_back({f.name, f.type});
            for (const auto& mem : r.members)
                if (mem.kind == MemberKind::Method || mem.kind == MemberKind::Operator || mem.kind == MemberKind::Property)
                    rec.methods.push_back(method(mem));
            m.records.push_back(std::move(rec));
        }
        for (const auto& c : unit.classes) if (!c.isExtern) m.classes.push_back(lowerClass(c)); // extern = native-backed, not emitted
        for (const auto& d : unit.interfaces) { // contracts: signatures only, emitted as `interface`
            ir::Interface ii;
            ii.name = d.name;
            ii.generics = generics(d.generics);
            for (const auto& b : d.bases) ii.bases.push_back(b);
            for (const auto& mem : d.members)
                if (mem.kind == MemberKind::Method || mem.kind == MemberKind::Operator) ii.methods.push_back(method(mem));
            m.interfaces.push_back(std::move(ii));
        }
        // `extern class` type/ctor spellings -> the IR registry the emitters consult (replaces the hardcoded
        // List/Iterable/Error mappings). Type arms come from the `type { … }` block, ctor arms from `init`.
        for (const auto& c : unit.classes) {
            if (!c.isExtern) continue;
            ir::ExternType et; et.name = c.name;
            for (const auto& b : c.typeBindings) {
                if (b.target == "csharp") et.csType = b.code;
                else if (b.target == "typescript") et.tsType = b.code;
                else if (b.target == "python") et.pyType = b.code;
            }
            for (const auto& mem : c.members)
                if (mem.kind == MemberKind::Init)
                    for (const auto& b : mem.bindings) {
                        if (b.target == "csharp") et.csCtor = b.code;
                        else if (b.target == "typescript") et.tsCtor = b.code;
                        else if (b.target == "python") et.pyCtor = b.code;
                    }
            m.externTypes.push_back(std::move(et));
        }
        for (const auto& v : unit.values) { // top-level const/let
            ir::Global g;
            g.name = v.name;
            g.isConst = v.isConst;
            g.type = v.hasType ? v.type : (v.init ? v.init->type : TypeRef{});
            if (v.init) g.init = expr(*v.init);
            m.globals.push_back(std::move(g));
        }
        for (const auto& ext : unit.extensions) {
            if (!ext.bindings.empty()) continue; // a bound extension isn't emitted — it's a call-site template
            ir::Function f;
            f.name = ext.name;
            f.isExtension = true;
            f.generics = generics(ext.generics);
            f.returnType = ext.returnType;
            f.params.push_back({"self", ext.receiver}); // the receiver becomes the leading `self` parameter
            for (const auto& p : ext.params) f.params.push_back(irParam(p));
            inExtension_ = true; // inside the body, `this` denotes the receiver -> lower to `self`
            if (ext.exprBody) { f.exprBodied = true; f.exprBody = expr(*ext.exprBody); }
            else f.body = block(ext.body);
            inExtension_ = false;
            m.extensions.push_back(std::move(f));
        }
        for (const auto& fn : unit.functions) {
            if (fn.isExpect) continue; // capability signature only — the `actual`s carry the implementation
            ir::Function f;
            f.name = fn.name;
            f.mangledName = fn.mangledName.empty() ? fn.name : fn.mangledName;
            f.actualTarget = fn.actualTarget;
            f.generics = generics(fn.generics);
            f.returnType = fn.returnType;
            f.isEntry = (fn.name == "main" && fn.params.empty());
            f.isAsync = fn.isAsync;
            for (const auto& p : fn.params) f.params.push_back(irParam(p));
            sawYield_ = false;
            f.body = block(fn.body);
            f.isIterator = sawYield_;
            m.functions.push_back(std::move(f));
        }
        return m;
    }

private:
    int tmpCounter_ = 0;        // fresh-name counter for desugared bindings (e.g. `??`-on-Option)
    bool sawYield_ = false;     // set while lowering a function body that contains `yield`
    bool inExtension_ = false;  // set while lowering an extension body, so `this` lowers to `self`
    std::unordered_map<std::string, std::unordered_set<std::string>> extensions_; // receiver type -> method names
    std::unordered_map<std::string, const std::vector<TargetBinding>*> bindings_; // "Type.member" -> FFI arms
    std::unordered_map<std::string, const std::vector<TargetBinding>*> ctorBindings_; // "Type" -> ctor FFI arms
    std::unordered_set<std::string> freeFns_;                                     // top-level function names
    std::unordered_set<std::string> typeNames_;
    std::unordered_set<std::string> recordNames_;                    // user records (structural `==`)
    std::unordered_map<std::string, const RecordDecl*> records_;     // record name -> decl (field order for `with`)
    std::unordered_set<std::string> indexerTypes_;                   // types declaring `operator fn get`
    std::unordered_map<std::string, std::unordered_set<std::string>> enumCases_;
    std::unordered_map<std::string, std::string> caseUnion_;                       // case -> union
    std::unordered_map<std::string, std::vector<std::string>> caseFields_;         // case -> field names
    std::unordered_map<std::string, std::unordered_set<std::string>> unionCases_;  // union -> case names
    std::unordered_map<std::string, std::vector<std::string>> bases_;              // type -> named base(s)

    // Resolve a "Type.member" FFI binding, walking base types so an inherited binding (e.g. `message`
    // declared on the `Error` base) fires on a subclass receiver too.
    const std::vector<TargetBinding>* findBinding(const std::string& typeName, const std::string& member) const {
        if (auto it = bindings_.find(typeName + "." + member); it != bindings_.end()) return it->second;
        if (auto b = bases_.find(typeName); b != bases_.end())
            for (const auto& base : b->second)
                if (auto r = findBinding(base, member)) return r;
        return nullptr;
    }

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

    // `opt ?? rhs` (opt: Option<X>) -> `match opt { Some(v) => v, None => rhs }`. The element type is e.type
    // (sema gave the `??` node the element type X), so the bound `v` and the match are typed X.
    ir::ExprPtr coalesceOption(const Expr& e) {
        std::string v = "__opt" + std::to_string(tmpCounter_++);
        auto m = std::make_unique<ir::Match>(e.pos, e.type, expr(*e.lhs)); // scrutinee keeps its Option<X> type
        ir::MatchArm some;
        some.pattern.kind = ir::PatternKind::Ctor;
        some.pattern.ctorCase = "Some";
        some.pattern.binders.push_back({"value", v}); // bind the Some payload field
        some.body = std::make_unique<ir::Var>(e.pos, e.type, v);
        m->arms.push_back(std::move(some));
        ir::MatchArm none;
        none.pattern.kind = ir::PatternKind::Ctor;
        none.pattern.ctorCase = "None";
        none.body = expr(*e.rhs);
        m->arms.push_back(std::move(none));
        return m;
    }

    static std::vector<ir::GenericParam> generics(const std::vector<GenericParam>& gs) {
        std::vector<ir::GenericParam> out;
        for (const auto& g : gs) out.push_back({g.name, g.bounds});
        return out;
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
        ic.generics = generics(c.generics);
        ic.bases = c.bases;
        for (const auto& mem : c.members) {
            switch (mem.kind) {
                case MemberKind::Field:
                case MemberKind::Const: {
                    ir::ClassField f;
                    f.name = mem.name;
                    f.isMutable = mem.isMutable;
                    f.isStatic = mem.kind == MemberKind::Const;
                    for (const auto& mod : mem.modifiers) if (mod == "static") f.isStatic = true;
                    f.type = mem.type;
                    if (mem.init) f.init = expr(*mem.init);
                    ic.fields.push_back(std::move(f));
                    break;
                }
                case MemberKind::Init:
                    ic.hasInit = true;
                    for (const auto& p : mem.params) ic.initParams.push_back(irParam(p));
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

    // Lower a parameter, carrying its optional `= default` (so emitters reproduce it in the signature
    // and callers may omit trailing defaulted arguments — both C# and TS support default parameters).
    ir::Param irParam(const Param& p) {
        ir::Param ip{p.name, p.type, nullptr};
        if (p.hasDefault && p.defaultValue) ip.defaultValue = expr(*p.defaultValue);
        return ip;
    }

    ir::Method method(const Member& m) {
        ir::Method im;
        im.name = m.name;
        im.isAsync = m.isAsync;
        for (const auto& mod : m.modifiers) if (mod == "static") im.isStatic = true;
        im.generics = generics(m.generics);
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
        for (const auto& p : m.params) im.params.push_back(irParam(p));
        if (m.exprBodied && m.exprBody) { im.exprBodied = true; im.exprBody = expr(*m.exprBody); }
        else { im.exprBodied = false; im.body = block(m.body); }
        return im;
    }

    ir::ExprPtr expr(const Expr& e) {
        switch (e.kind) {
            case ExprKind::IntLit:    return std::make_unique<ir::IntLit>(e.pos, e.type, stripNumericSuffix(e.text));
            case ExprKind::FloatLit:  return std::make_unique<ir::FloatLit>(e.pos, e.type, stripNumericSuffix(e.text));
            case ExprKind::BoolLit:   return std::make_unique<ir::BoolLit>(e.pos, e.type, e.boolVal);
            case ExprKind::NullLit:   return std::make_unique<ir::NullLit>(e.pos, e.type);
            case ExprKind::StringLit: return std::make_unique<ir::StrLit>(e.pos, e.type, e.text);
            case ExprKind::CharLit:   return std::make_unique<ir::CharLit>(e.pos, e.type, e.text);
            case ExprKind::InterpString: {
                auto in = std::make_unique<ir::Interp>(e.pos, e.type);
                in->chunks = e.chunks;
                for (const auto& a : e.args) in->holes.push_back(expr(*a));
                return in;
            }
            case ExprKind::Name:
                if (auto u = caseUnion_.find(e.text); u != caseUnion_.end()) // bare payload-free union case
                    return std::make_unique<ir::MakeCase>(e.pos, e.type, u->second, e.text);
                if (!e.staticOwner.empty()) { // bare ref to an enclosing-class static/const -> `Owner.name`
                    auto m = std::make_unique<ir::Member>(e.pos, e.type, nullptr, e.text, false);
                    m->staticType = e.staticOwner;
                    return m;
                }
                return std::make_unique<ir::Var>(e.pos, e.type, e.text);
            case ExprKind::This:
                if (inExtension_) return std::make_unique<ir::Var>(e.pos, e.type, "self"); // receiver alias
                return std::make_unique<ir::This>(e.pos, e.type);
            case ExprKind::Unary:     return std::make_unique<ir::Unary>(e.pos, e.type, e.text, expr(*e.lhs));
            case ExprKind::Await:     return std::make_unique<ir::Await>(e.pos, e.type, expr(*e.lhs));
            case ExprKind::Cast:      return std::make_unique<ir::Cast>(e.pos, e.castType, expr(*e.lhs));
            // `x!` asserts the non-null type sema put on the node: a cast to it (C# unwraps a Nullable<T>
            // value type via `(int)x`; for reference types it's an identity cast — TS strips both).
            case ExprKind::NullAssert: return std::make_unique<ir::Cast>(e.pos, e.type, expr(*e.lhs));
            case ExprKind::Extern:    return std::make_unique<ir::Extern>(e.pos, e.type, e.text);
            case ExprKind::Binary: {
                // `opt ?? rhs` on an optional generic desugars to `match opt { Some(v) => v, None => rhs }`,
                // reusing the existing match machinery (no special-case in either emitter).
                if (e.text == "??" && e.lhs->type.kind == TypeRef::Kind::Named && e.lhs->type.name == "Option")
                    return coalesceOption(e);
                auto b = std::make_unique<ir::Binary>(e.pos, e.type, e.text, expr(*e.lhs), expr(*e.rhs));
                if (e.lhs->type.kind == TypeRef::Kind::Named && !e.lhs->type.name.empty()) {
                    const std::string& ln = e.lhs->type.name;
                    b->lhsIsRecord = recordNames_.count(ln) != 0;
                    b->lhsIsUserType = !isPrimitiveTypeName(ln) && ln != "unit";
                }
                return b;
            }
            case ExprKind::Member: {
                // Static const binding `Type.FIELD` (e.g. Math.PI as a bound `extern class` const):
                // the LHS is a type name, the binding template uses no receiver.
                if (e.lhs->kind == ExprKind::Name && typeNames_.count(e.lhs->text)) {
                    if (auto it = bindings_.find(e.lhs->text + "." + e.text); it != bindings_.end())
                        return makeBound(e.type, e.pos, nullptr, *it->second, nullptr);
                }
                if (e.lhs->type.kind == TypeRef::Kind::Named) { // bound std/core property (List.count, Error.message)
                    if (auto arms = findBinding(e.lhs->type.name, e.text))
                        return makeBound(e.type, e.pos, e.lhs.get(), *arms, nullptr);
                }
                return std::make_unique<ir::Member>(e.pos, e.type, expr(*e.lhs), e.text, e.flag);
            }
            case ExprKind::Match:     return matchExpr(e);
            case ExprKind::IfExpr:    return std::make_unique<ir::Cond>(e.pos, e.type, expr(*e.lhs), expr(*e.rhs), expr(*e.extra));
            case ExprKind::Lambda: {
                auto lam = std::make_unique<ir::Lambda>(e.pos, e.type);
                for (const auto& p : e.params) lam->params.push_back({p.name, p.type});
                if (e.flag) { lam->exprBodied = false; lam->block = block(e.block); }
                else { lam->exprBodied = true; lam->body = expr(*e.lhs); }
                return lam;
            }
            case ExprKind::Call: {
                if (e.lhs && e.lhs->kind == ExprKind::Member) { // method call `obj.method(args)`
                    // Static call `Type.method(args)`: the receiver is a type name, not a value.
                    if (e.lhs->lhs->kind == ExprKind::Name &&
                        (typeNames_.count(e.lhs->lhs->text) || isPrimitiveTypeName(e.lhs->lhs->text))) {
                        // A bound static method `Type.method(args)` (e.g. a future Math.sqrt as an
                        // `extern class` static): substitute the args into the template, no receiver.
                        if (auto b = bindings_.find(e.lhs->lhs->text + "." + e.lhs->text); b != bindings_.end())
                            return makeBound(e.type, e.pos, nullptr, *b->second, &e.args);
                        auto mc = std::make_unique<ir::MethodCall>(e.pos, e.type, nullptr, e.lhs->text);
                        mc->staticType = e.lhs->lhs->text;
                        for (const auto& a : e.args) mc->args.push_back(expr(*a));
                        return mc;
                    }
                    const TypeRef& rt = e.lhs->lhs->type; // receiver type, resolved by sema
                    if (rt.kind == TypeRef::Kind::Named) { // bound std/core method (List.add / List.removeAll / inherited)
                        if (auto arms = findBinding(rt.name, e.lhs->text))
                            return makeBound(e.type, e.pos, e.lhs->lhs.get(), *arms, &e.args);
                    }
                    auto mc = std::make_unique<ir::MethodCall>(e.pos, e.type, expr(*e.lhs->lhs), e.lhs->text);
                    if (rt.kind == TypeRef::Kind::Named) {
                        auto it = extensions_.find(rt.name);
                        if (it != extensions_.end() && it->second.count(e.lhs->text)) mc->isExtension = true;
                    }
                    for (const auto& a : e.args) mc->args.push_back(expr(*a));
                    return mc;
                }
                std::string callee = (e.lhs && e.lhs->kind == ExprKind::Name) ? e.lhs->text : "";
                if (!callee.empty() && typeNames_.count(callee)) { // record/class construction
                    if (auto it = ctorBindings_.find(callee); it != ctorBindings_.end()) { // bound ctor (extern class)
                        TypeRef ct; ct.name = callee; ct.args = e.typeArgs; // full type so `$T` renders `Name<args>`
                        return makeBound(ct, e.pos, /*recv=*/nullptr, *it->second, &e.args);
                    }
                    auto n = std::make_unique<ir::New>(e.pos, e.type, callee);
                    n->typeArgs = e.typeArgs; // explicit `Box<i32>(7)` type args (empty if inferred)
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
                if (!e.staticOwner.empty()) { // bare call to an enclosing-class static method -> `Owner.method(args)`
                    auto mc = std::make_unique<ir::MethodCall>(e.pos, e.type, nullptr, callee);
                    mc->staticType = e.staticOwner;
                    for (const auto& a : e.args) mc->args.push_back(expr(*a));
                    return mc;
                }
                auto call = std::make_unique<ir::Call>(e.pos, e.type, callee, /*isPrint=*/false); // print is now std.io.print, a normal free fn
                call->isFree = freeFns_.count(callee) > 0; // a top-level fn (vs a function-valued local)
                call->mangledCallee = e.overloadName.empty() ? callee : e.overloadName; // TS overload target
                for (const auto& a : e.args) call->args.push_back(expr(*a));
                return call;
            }
            case ExprKind::Index: {
                // List element access `recv[i]`. The result type is the element type sema resolved onto
                // the node; `receiverHasIndexer` marks a user `operator fn get` receiver (TS -> `.get(i)`).
                auto ix = !e.args.empty()
                    ? std::make_unique<ir::Index>(e.pos, e.type, expr(*e.lhs), expr(*e.args[0]))
                    : std::make_unique<ir::Index>(e.pos, e.type, expr(*e.lhs), std::make_unique<ir::IntLit>(e.pos, namedType("i32"), "0"));
                ix->receiverHasIndexer = e.lhs->type.kind == TypeRef::Kind::Named &&
                                         indexerTypes_.count(e.lhs->type.name) != 0;
                return ix;
            }
            case ExprKind::ListLit: {
                TypeRef elem = e.type.kind == TypeRef::Kind::Named && !e.type.args.empty() ? e.type.args[0] : TypeRef{};
                auto lst = std::make_unique<ir::ListLit>(e.pos, e.type, elem);
                for (const auto& a : e.args) lst->elements.push_back(expr(*a));
                return lst;
            }
            case ExprKind::TupleLit: {
                auto tup = std::make_unique<ir::Tuple>(e.pos, e.type);
                for (const auto& a : e.args) tup->elements.push_back(expr(*a));
                return tup;
            }
            case ExprKind::With: { // record copy `base with { f = v, … }`
                auto w = std::make_unique<ir::With>(e.pos, e.type, expr(*e.lhs));
                for (const auto& f : e.fields) w->fields.push_back({f.name, expr(*f.value)});
                // Precompute the ctor-rebuild (P19): the record's fields in declaration order, each the
                // override or a `<base>.field` read. A non-simple base gets a fresh temp for single eval;
                // a simple Var base is read directly (its name re-lowered per field).
                w->baseIsSimple = w->base->kind == ir::ExprKind::Var;
                std::string baseName = w->baseIsSimple ? static_cast<const ir::Var&>(*w->base).name
                                                       : "__w" + std::to_string(tmpCounter_++);
                if (!w->baseIsSimple) w->tempName = baseName;
                if (auto rit = records_.find(e.type.name); rit != records_.end()) {
                    for (const auto& fld : rit->second->fields) {
                        const Expr* over = nullptr;
                        for (const auto& f : e.fields) if (f.name == fld.name) { over = f.value.get(); break; }
                        if (over) w->ctorArgs.push_back(expr(*over));
                        else w->ctorArgs.push_back(std::make_unique<ir::Member>(
                            e.pos, fld.type,
                            std::make_unique<ir::Var>(e.pos, e.lhs->type, baseName), fld.name, false));
                    }
                }
                return w;
            }
            // A surface form with no lowering rule must FAIL LOUDLY, not silently emit a placeholder: a
            // silent `0` once masked an unlowered `if`-expression as a cross-target-identical miscompile
            // (both backends were equally wrong, so the differential gate couldn't see it). This poison
            // identifier breaks the emitted C#/TS build instead. (Still unlowered: CharLit, Super-expr.)
            default: return std::make_unique<ir::Extern>(e.pos, e.type, "__polyglot_unlowered_expr__");
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
                std::string binding = s.forBinding.kind == PatKind::Binding ? s.forBinding.name : "_";
                auto node = std::make_unique<ir::For>(s.pos, binding);
                if (s.forBinding.kind == PatKind::Tuple) // `for (a, b) in …`: destructure each element
                    for (const auto& sub : s.forBinding.sub)
                        node->tupleBindings.push_back(sub.kind == PatKind::Binding ? sub.name : "_");
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
            case StmtKind::Break:    return std::make_unique<ir::Break>(s.pos);
            case StmtKind::Continue: return std::make_unique<ir::Continue>(s.pos);
            case StmtKind::Yield:
                sawYield_ = true;
                return std::make_unique<ir::Yield>(s.pos, s.value ? expr(*s.value) : nullptr);
            case StmtKind::Throw:
                return std::make_unique<ir::Throw>(s.pos, s.value ? expr(*s.value) : nullptr);
            case StmtKind::Use: {
                auto node = std::make_unique<ir::Use>(s.pos, s.name, expr(*s.value));
                if (s.hasDeclType) node->type = s.declType;
                node->body = block(s.thenBody);
                return node;
            }
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
