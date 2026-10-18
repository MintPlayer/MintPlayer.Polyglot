#include "mintplayer/polyglot/lower.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <unordered_map>
#include <unordered_set>

#include "mintplayer/polyglot/backend.hpp"
#include "mintplayer/polyglot/capture_analysis.hpp"
#include "mintplayer/polyglot/hoist_block_lambdas.hpp"

namespace mintplayer::polyglot {

namespace {

// Strip a numeric literal's width suffix (`100u8`, `0i64`, `1.5f32`) — the type rides on the expr; the
// backends re-add a target-appropriate suffix (C# `L`/`UL`, TS BigInt `n`) from that type.
bool isPrimitiveTypeName(const std::string& n) {
    return n == "i8" || n == "i16" || n == "i32" || n == "i64" || n == "u8" || n == "u16" || n == "u32" ||
           n == "u64" || n == "f32" || n == "f64" || n == "bool" || n == "char" || n == "string";
}

std::string stripNumericSuffix(const std::string& text) {
    static const char* intSfx[] = {"i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64"};
    static const char* fltSfx[] = {"f32", "f64", "f", "d"};
    auto strip = [&](const char* const* sfx, std::size_t count) -> std::string {
        for (std::size_t i = 0; i < count; ++i) {
            std::size_t n = std::char_traits<char>::length(sfx[i]);
            if (text.size() > n && text.compare(text.size() - n, n, sfx[i]) == 0)
                return text.substr(0, text.size() - n);
        }
        return {};
    };
    if (std::string s = strip(intSfx, std::size(intSfx)); !s.empty()) return s;
    // 'f'/'d' (and the f32/f64 tails) are hex digits: a radix literal never carries a float suffix,
    // so stripping one would truncate its digits (`0xff` -> `0xf`).
    const bool radix = text.size() > 1 && text[0] == '0' &&
                       (text[1] == 'x' || text[1] == 'X' || text[1] == 'b' || text[1] == 'B' ||
                        text[1] == 'o' || text[1] == 'O');
    if (!radix) {
        if (std::string s = strip(fltSfx, std::size(fltSfx)); !s.empty()) return s;
    }
    return text;
}

// The top-level module globals a function body references but does not itself bind. This is the fact PHP
// needs to emit `global $x;` (module globals aren't visible in a PHP function's scope). Target-neutral,
// collected on the AST: referenced Names ∩ module globals, minus every name the function binds itself
// (params + any let/use/for/lambda/catch/match binder). Conservative on shadowing — a locally-bound name is
// never reported as a global ref, so we never emit a `global` that would wrongly alias a local; a genuinely
// missed global surfaces as a loud runtime error the differential gate catches, never a silent miscompile.
struct GlobalRefScan {
    std::unordered_set<std::string> bound;
    std::vector<std::string> used; // first-use order, for deterministic output
    std::unordered_set<std::string> usedSeen;
    void use_(const std::string& n) { if (!n.empty() && usedSeen.insert(n).second) used.push_back(n); }
    void pat(const Pattern& p) {
        if ((p.kind == PatKind::Binding || p.kind == PatKind::Ctor) && !p.name.empty()) bound.insert(p.name);
        for (const auto& s : p.sub) pat(s);
    }
    void ex(const Expr* e) {
        if (!e) return;
        if (e->kind == ExprKind::Name) use_(e->text);
        for (const auto& p : e->params) bound.insert(p.name); // lambda params
        ex(e->lhs.get()); ex(e->rhs.get()); ex(e->extra.get());
        for (const auto& a : e->args) ex(a.get());
        for (const auto& s : e->block) st(s.get());
        for (const auto& f : e->fields) ex(f.value.get());
        for (const auto& a : e->arms) { pat(a.pattern); ex(a.guard.get()); ex(a.body.get()); for (const auto& s : a.block) st(s.get()); }
    }
    void st(const Stmt* s) {
        if (!s) return;
        if ((s->kind == StmtKind::Let || s->kind == StmtKind::Use) && !s->name.empty()) bound.insert(s->name);
        if (s->kind == StmtKind::For) pat(s->forBinding);
        ex(s->value.get()); ex(s->target.get());
        for (const auto& t : s->thenBody) st(t.get());
        for (const auto& t : s->elseBody) st(t.get());
        for (const auto& c : s->catches) { if (!c.name.empty()) bound.insert(c.name); ex(c.guard.get()); for (const auto& t : c.body) st(t.get()); }
        for (const auto& t : s->finallyBody) st(t.get());
    }
};
std::vector<std::string> scanGlobalRefs(const std::unordered_set<std::string>& globals,
                                        const std::vector<Param>& params,
                                        const std::vector<StmtPtr>& body, const Expr* exprBody) {
    GlobalRefScan s;
    for (const auto& p : params) s.bound.insert(p.name);
    for (const auto& st : body) s.st(st.get());
    if (exprBody) s.ex(exprBody);
    std::vector<std::string> refs;
    for (const auto& n : s.used)
        if (globals.count(n) && !s.bound.count(n)) refs.push_back(n);
    return refs;
}

class Lowerer { // per-target since P19 slice 9: binding/extern-type arms are picked here, not at emit
public:
    Lowerer(const CompilationUnit& unit, std::string target) : target_(std::move(target)) {
        if (const Backend* b = findBackend(target_)) {
            nativeCompound_ = b->nativeCompoundAssign();
            lateOperands_ = b->lateOperandReads();
            nativeCoalesceAssign_ = b->nativeCoalesceAssign();
        }
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
        for (const auto& e : unit.extensions) { extensions_[e.receiver.name].insert(e.name); extParams_[e.receiver.name + "." + e.name] = &e.params; }
        // P40 B3: a std (prelude) extension written in .pg — a guard over unchecked primitives — is emitted
        // under the reserved name `_pg_<name>`. On the targets where an extension is a free function
        // (TS/Python/PHP) its plain name (`contains`, `split`, …) would share the user's function namespace
        // and silently shadow a user `fn contains`; the `_pg_` prefix is refused in user code, so it cannot.
        // P40 review B10: and it is lowered as a plain prelude FREE function whose first parameter is the
        // receiver (`s.charAt(1)` -> `_pg_charAt(s, 1)`), not as an extension: on C# an extension is a `this`
        // method that every assembly granted InternalsVisibleTo saw on `string`. An internal guard is not API.
        for (const auto& e : unit.extensions)
            if (e.bindings.empty() && ir::isPreludeOrigin(e.originModule) && (!e.body.empty() || e.exprBody)) {
                extEmitName_[e.receiver.name + "." + e.name] = "_pg_" + e.name;
                freeFns_.insert("_pg_" + e.name);
            }
        // P40 B3: an `extern class` static with a .pg body (std `string.fromCodePoint`, a guard) is emitted as a
        // prelude free function `_pg_<Type>_<name>`; `Type.name(args)` calls it.
        for (const auto& c : unit.classes) {
            if (!c.isExtern) continue;
            for (const auto& mem : c.members) {
                if (mem.kind != MemberKind::Method || !mem.bindings.empty() || mem.body.empty()) continue; // block bodies (free fns emit blocks)
                bool isStatic = false;
                for (const auto& mod : mem.modifiers) if (mod == "static") isStatic = true;
                if (!isStatic) continue;
                const std::string fn = "_pg_" + c.name + "_" + mem.name;
                staticRedirect_[c.name + "." + mem.name] = fn;
                freeFns_.insert(fn);
            }
        }
        for (const auto& fn : unit.functions) freeFns_.insert(fn.name); // top-level fns -> C# qualifies as Program.fn
        // Per-target FFI bindings on std types (e.g. List.add): keyed "Type.member" so a call/access on a
        // receiver of that type lowers to a substituted template instead of a member call.
        for (const auto& c : unit.classes)
            for (const auto& mem : c.members)
                if (!mem.bindings.empty()) bindings_[c.name + "." + mem.name] = {&mem.bindings, &mem.params};
        for (const auto& r : unit.records)
            for (const auto& mem : r.members)
                if (!mem.bindings.empty()) bindings_[r.name + "." + mem.name] = {&mem.bindings, &mem.params};
        // A bound extension method (`extension fn string.toUpper() { actual… }`): a method on an existing
        // type — keyed like any member binding, so a call `s.toUpper()` lowers to the substituted template.
        for (const auto& e : unit.extensions)
            if (!e.bindings.empty()) bindings_[e.receiver.name + "." + e.name] = {&e.bindings, &e.params};
        // Bound constructors on an `extern class` (its `init` has binding arms): `Type(args)` lowers to a
        // substituted ctor template instead of a plain `new Type(...)`.
        for (const auto& c : unit.classes)
            for (const auto& mem : c.members)
                if (mem.kind == MemberKind::Constructor && !mem.bindings.empty()) ctorBindings_[c.name] = {&mem.bindings, &mem.params};
        // Named base types, so a binding/member inherited from a base resolves on a subclass receiver
        // (e.g. `Error.message`, declared on the core-prelude `extern class Error`, on a `: Error` subclass).
        for (const auto& c : unit.classes) for (const auto& b : c.bases) if (!b.name.empty()) bases_[c.name].push_back(b.name);
        for (const auto& r : unit.records) for (const auto& b : r.bases) if (!b.name.empty()) bases_[r.name].push_back(b.name);
        // Module facts precomputed onto IR nodes (P19), so the emit layer never scans the module: record
        // names (structural `==`), record decls (the `with` ctor-rebuild reads field order), and the types
        // declaring an `operator fn get` (a target without `[]` overloading calls `.get(i)`).
        for (const auto& r : unit.records) { recordNames_.insert(r.name); records_[r.name] = &r; }
        auto noteIndexer = [&](const std::string& name, const std::vector<Member>& ms) {
            for (const auto& mem : ms) {
                if (mem.kind == MemberKind::Operator && mem.name == "get") indexerTypes_.insert(name);
                if (mem.kind == MemberKind::Operator && mem.name == "eq") userEqTypes_.insert(name); // #49

                // Computed properties (getters): a target that emulates them as methods (PHP) needs the
                // access site to call `recv->name()`; native-property targets ignore the stamp.
                if (mem.kind == MemberKind::Property) propertyMembers_[name].insert(mem.name);
            }
        };
        for (const auto& r : unit.records) noteIndexer(r.name, r.members);
        for (const auto& c : unit.classes) noteIndexer(c.name, c.members);
        // Class static fields: a static access `Owner.name` to one carries its mutability and literal-ness
        // (ir::Member::staticField*), so a target that spells constants and static variables differently can.
        for (const auto& c : unit.classes)
            for (const auto& mem : c.members)
                if (mem.kind == MemberKind::Field || mem.kind == MemberKind::Const) {
                    bool isStatic = mem.kind == MemberKind::Const;
                    for (const auto& mod : mem.modifiers) if (mod == "static") isStatic = true;
                    if (isStatic) staticFields_[c.name + "." + mem.name] = {mem.isMutable, mem.init && isLiteralInit(*mem.init)};
                }
        // P37 D: attribute machinery — Tier 1 bindings by name, and the Tier 2 materialized set (attribute
        // records some Meta query constructs; the rest emit nothing).
        unit_ = &unit;
        for (const auto& ea : unit.externAttrs) externAttrMap_[ea.name] = &ea;
        for (const auto& n : unit.metaMaterialized) materialized_.insert(n);
        if (const Backend* b = findBackend(target_)) spell_ = b->constSpelling();
    }

    // P37 D Tier 1: render each pass-through attribute into its native annotation line for the ACTIVE
    // target — the arm's template IS the whole line, with $0…/$name substituted from the (constant) args;
    // the arm's import line is accumulated once onto the module. Tier 2 attributes render nothing.
    std::vector<std::string> renderAttrLines(const std::vector<AttrUse>& attrs) {
        std::vector<std::string> out;
        for (const auto& a : attrs) {
            auto it = externAttrMap_.find(a.name);
            if (it == externAttrMap_.end()) continue; // Tier 2: compile-time data, no emission
            const ExternAttrDecl& d = *it->second;
            const ExternAttrArm* arm = nullptr;
            for (const auto& cand : d.arms)
                if (cand.target == target_ && !cand.refuse) arm = &cand;
            if (!arm) continue; // no arm for this target: D12 refused it before lowering
            // Resolve each declared param to its constant value (positional, named, or default).
            std::vector<std::string> values;
            for (std::size_t i = 0; i < d.params.size(); ++i) {
                const Expr* v = nullptr;
                std::size_t positional = 0;
                for (const auto& arg : a.args) {
                    if (arg.name.empty()) {
                        if (positional == i) { v = arg.value.get(); }
                        ++positional;
                    } else if (arg.name == d.params[i].name) {
                        v = arg.value.get();
                    }
                }
                if (!v && d.params[i].hasDefault) v = d.params[i].defaultValue.get();
                values.push_back(v ? constAttrText(*v) : std::string());
            }
            std::string line = arm->code;
            // $<name> first (longest names first so a prefix never clobbers), then $0…$n.
            std::vector<std::size_t> order(d.params.size());
            for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
            std::sort(order.begin(), order.end(), [&](std::size_t x, std::size_t y) {
                return d.params[x].name.size() > d.params[y].name.size();
            });
            for (std::size_t i : order) substAll(line, "$" + d.params[i].name, values[i]);
            for (std::size_t i = 0; i < values.size(); ++i) substAll(line, "$" + std::to_string(i), values[i]);
            out.push_back(std::move(line));
            if (!arm->importLine.empty()) {
                bool dup = false;
                for (const auto& l : attrImports_) if (l == arm->importLine) dup = true;
                if (!dup) attrImports_.push_back(arm->importLine);
            }
        }
        return out;
    }
    static void substAll(std::string& s, const std::string& key, const std::string& val) {
        for (std::size_t i = 0; (i = s.find(key, i)) != std::string::npos; i += val.size())
            s.replace(i, key.size(), val);
    }
    // Render a constant attribute argument as target source text (bools differ per target; strings are
    // double-quoted with minimal escaping, which every current target accepts).
    std::string constAttrText(const Expr& v) const {
        switch (v.kind) {
            case ExprKind::IntLit:
            case ExprKind::FloatLit: return v.text;
            case ExprKind::BoolLit:
                return v.boolVal ? spell_.trueLit : spell_.falseLit;
            case ExprKind::StringLit: {
                std::string out = "\"";
                for (char c : v.text) {
                    if (c == '"' || c == '\\') out += '\\';
                    out += c;
                }
                return out + "\"";
            }
            case ExprKind::Unary:
                return "-" + (v.lhs ? constAttrText(*v.lhs) : std::string());
            case ExprKind::Member: // enum member — the plugin's enumMemberOp spells the accessor
                if (v.lhs) return v.lhs->text + spell_.enumMemberOp + v.text;
                return "";
            case ExprKind::ListLit: { // array of consts — the plugin's constArray delimiters
                std::string items;
                for (const auto& el : v.args) {
                    if (!items.empty()) items += ", ";
                    if (el) items += constAttrText(*el);
                }
                return spell_.arrayOpen + items + spell_.arrayClose;
            }
            default:
                return "";
        }
    }

    // A "Type.member" FFI binding: its per-target arms, plus the declared params (for filling defaults).
    struct BindingEntry { const std::vector<TargetBinding>* arms = nullptr; const std::vector<Param>* params = nullptr; };

    // P40 slice 7: lower the defaults of the parameters a call omitted (from index `given` on). Each call
    // lowers the declaration's checked default expression afresh, so every call site owns its own IR.
    void appendDefaults(const std::vector<Param>& params, std::size_t given, std::vector<ir::ExprPtr>& out) {
        for (std::size_t i = given; i < params.size(); ++i)
            if (params[i].hasDefault && params[i].defaultValue) out.push_back(expr(*params[i].defaultValue));
    }

    // Build an ir::Bound from a receiver, args and a "Type.member" binding — the ACTIVE target's arm
    // (lowering is per-target since P19 slice 9; a used member with no arm was refused before lowering).
    ir::ExprPtr makeBound(const TypeRef& type, SourcePos pos, const Expr* recv,
                          const BindingEntry& entry, const std::vector<ExprPtr>* args) {
        const std::vector<TargetBinding>& arms = *entry.arms;
        auto b = std::make_unique<ir::Bound>(pos, type);
        if (recv) b->receiver = expr(*recv); // null for a static member binding (template uses only $0,$1,…)
        if (args) {
            for (const auto& a : *args) b->args.push_back(expr(*a));
            // P40 slice 7: a template has no native default to fall back on, so an omitted trailing
            // parameter's (sema-checked, literal) default is filled in here — `$1` of `indexOf(n, from = 0)`.
            if (entry.params) appendDefaults(*entry.params, args->size(), b->args);
        }
        for (const auto& arm : arms)
            if (arm.target == target_) { b->tmpl = arm.code; b->preludeKeys = arm.preludeKeys; }
        return b;
    }

    ir::Module run(const CompilationUnit& unit) {
        ir::Module m;
        // Top-level global names — the set a function or member body may reference from an outer scope
        // (PHP `global $x;`). A member, because the record/class loops below lower methods before the
        // globals themselves are lowered.
        for (const auto& v : unit.values) moduleGlobals_.insert(v.name);
        for (const auto& e : unit.enums) {
            ir::Enum ie;
            ie.name = e.name;
            ie.pos = e.pos;
            long long next = 0;
            for (const auto& c : e.cases) { long long v = c.hasValue ? c.value : next; ie.cases.push_back({c.name, v}); next = v + 1; }
            ie.originModule = e.originModule;
            m.enums.push_back(std::move(ie));
        }
        for (const auto& u : unit.unions) {
            ir::Union iu;
            iu.name = u.name;
            iu.pos = u.pos;
            iu.generics = generics(u.generics);
            iu.originModule = u.originModule;
            for (const auto& c : u.cases) {
                ir::UnionCase ic;
                ic.name = c.name;
                for (const auto& f : c.params) ic.fields.push_back({f.name, f.type});
                iu.cases.push_back(std::move(ic));
            }
            m.unions.push_back(std::move(iu));
        }
        for (const auto& r : unit.records) {
            // P37 D Tier 2: an `attribute` record is emitted ONLY when some Meta query materialized it —
            // attached-but-unqueried metadata produces zero runtime output.
            if (r.isAttribute && !materialized_.count(r.name)) continue;
            ir::Record rec;
            rec.name = r.name;
            rec.pos = r.pos;
            rec.attrLines = renderAttrLines(r.attributes);
            rec.generics = generics(r.generics);
            for (const auto& b : r.bases) rec.bases.push_back(b);
            for (const auto& f : r.fields) rec.fields.push_back({f.name, f.type});
            for (const auto& mem : r.members)
                if (mem.kind == MemberKind::Method || mem.kind == MemberKind::Operator || mem.kind == MemberKind::Property) {
                    rec.methods.push_back(method(mem));
                    rec.methods.back().ownerIsRecord = true;
                }
            rec.originModule = r.originModule;
            m.records.push_back(std::move(rec));
        }
        for (const auto& c : unit.classes) if (!c.isExtern) { ir::Class ic = lowerClass(c); ic.originModule = c.originModule; m.classes.push_back(std::move(ic)); } // extern = native-backed, not emitted
        for (const auto& d : unit.interfaces) { // contracts: signatures only, emitted as `interface`
            ir::Interface ii;
            ii.name = d.name;
            ii.pos = d.pos;
            ii.generics = generics(d.generics);
            for (const auto& b : d.bases) ii.bases.push_back(b);
            for (const auto& mem : d.members)
                if (mem.kind == MemberKind::Method || mem.kind == MemberKind::Operator) ii.methods.push_back(method(mem));
            ii.originModule = d.originModule;
            m.interfaces.push_back(std::move(ii));
        }
        // `extern class` type/ctor spellings -> the IR registry the emitters consult (replaces the hardcoded
        // List/Iterable/Error mappings). Type arms come from the `type { … }` block, ctor arms from `init`.
        for (const auto& c : unit.classes) {
            if (!c.isExtern) continue;
            ir::ExternType et; et.name = c.name;
            for (const auto& b : c.typeBindings)
                if (b.target == target_) et.typeTmpl = b.code;
            m.externTypes.push_back(std::move(et));
        }
        for (const auto& v : unit.values) { // top-level const/let
            ir::Global g;
            g.name = v.name;
            g.pos = v.pos;
            g.isConst = v.isConst;
            g.type = v.hasType ? v.type : (v.init ? v.init->type : TypeRef{});
            if (v.init) g.init = expr(*v.init);
            g.originModule = v.originModule;
            m.globals.push_back(std::move(g));
        }
        for (const auto& ext : unit.extensions) {
            if (!ext.bindings.empty()) continue; // a bound extension isn't emitted — it's a call-site template
            ir::Function f;
            f.pos = ext.pos; // P38: ExtensionDecl has no namePos (ast.hpp) — the decl start is the anchor
            f.name = ext.name;
            const auto en = extEmitName_.find(ext.receiver.name + "." + ext.name);
            const bool stdGuard = en != extEmitName_.end(); // P40 review B10: a plain free function, not an extension
            if (stdGuard) { f.name = en->second; f.mangledName = en->second; }
            f.isExtension = !stdGuard;
            f.generics = generics(ext.generics);
            f.returnType = ext.returnType;
            f.params.push_back({"self", ext.receiver}); // the receiver becomes the leading `self` parameter
            for (const auto& p : ext.params) f.params.push_back(irParam(p));
            inExtension_ = true; // inside the body, `this` denotes the receiver -> lower to `self`
            if (ext.exprBody) { f.exprBodied = true; f.exprBody = expr(*ext.exprBody); }
            else f.body = block(ext.body);
            inExtension_ = false;
            f.originModule = ext.originModule;
            f.globalRefs = scanGlobalRefs(moduleGlobals_, ext.params, ext.body, ext.exprBody.get());
            if (stdGuard) m.functions.push_back(std::move(f));
            else m.extensions.push_back(std::move(f));
        }
        for (const auto& c : unit.classes) { // P40 B3: .pg-bodied extern statics -> prelude free functions
            if (!c.isExtern) continue;
            for (const auto& mem : c.members) {
                auto rd = staticRedirect_.find(c.name + "." + mem.name);
                if (rd == staticRedirect_.end()) continue;
                ir::Function f;
                f.pos = mem.namePos.fileId != 0 ? mem.namePos : mem.pos;
                f.name = rd->second;
                f.mangledName = rd->second;
                f.generics = generics(mem.generics);
                f.returnType = mem.returnType;
                for (const auto& p : mem.params) f.params.push_back(irParam(p));
                f.body = block(mem.body);
                f.originModule = c.originModule;
                f.globalRefs = scanGlobalRefs(moduleGlobals_, mem.params, mem.body, mem.exprBody.get());
                m.functions.push_back(std::move(f));
            }
        }
        for (const auto& fn : unit.functions) {
            if (fn.isExpect) continue; // capability signature only — the `actual`s carry the implementation
            ir::Function f;
            f.pos = fn.namePos.fileId != 0 ? fn.namePos : fn.pos; // P38: the function-entry anchor
            f.name = fn.name;
            f.mangledName = fn.mangledName.empty() ? fn.name : fn.mangledName;
            f.linkName = fn.linkName.empty() ? fn.name : fn.linkName;
            f.uniqueName = fn.uniqueName.empty() ? f.mangledName : fn.uniqueName;
            f.actualTarget = fn.actualTarget;
            f.generics = generics(fn.generics);
            f.returnType = fn.returnType;
            f.isEntry = (fn.name == "main" && fn.params.empty());
            f.isAsync = fn.isAsync;
            for (const auto& p : fn.params) f.params.push_back(irParam(p));
            f.attrLines = renderAttrLines(fn.attributes);
            sawYield_ = false;
            f.body = block(fn.body);
            f.isIterator = sawYield_;
            f.originModule = fn.originModule;
            f.globalRefs = scanGlobalRefs(moduleGlobals_, fn.params, fn.body, nullptr);
            m.functions.push_back(std::move(f));
        }
        m.attrImports = attrImports_; // P37 D Tier 1: verbatim import/using lines for used bindings

        // Stamp ir::Class.baseHasInit transitively now that every class/interface is lowered (issue #48).
        // A base contributes a chainable ctor when it is a user class with an `init` or an instance field
        // initializer (or one whose own ancestors do), OR a base that is neither a user class nor a user
        // interface — i.e. an extern/native type like Error/Exception, which always has a constructor.
        // Interface bases contribute nothing. Every target must run that constructor: an emitted
        // constructor chains to it even without a written `super()` (review r4 #3).
        {
            std::unordered_map<std::string, const ir::Class*> byName;
            for (const auto& c : m.classes) byName[c.name] = &c;
            std::unordered_set<std::string> ifaceNames;
            for (const auto& it : m.interfaces) ifaceNames.insert(it.name);
            std::unordered_map<std::string, int> memo; // per-class: -1 computing, 0 no, 1 yes
            std::function<bool(const ir::Class&)> classChains = [&](const ir::Class& c) -> bool {
                auto mit = memo.find(c.name);
                if (mit != memo.end() && mit->second >= 0) return mit->second == 1;
                memo[c.name] = -1; // guard against inheritance cycles
                bool chains = false;
                for (const auto& b : c.bases) {
                    auto cit = byName.find(b.name);
                    if (cit != byName.end()) {                 // a user class base
                        // P40 review r4 #3: a base whose only constructor work is an instance field
                        // initializer still gets a runtime constructor (needsCtor) that must run.
                        bool baseInits = cit->second->hasInit;
                        for (const auto& f : cit->second->fields) if (!f.isStatic && f.init) baseInits = true;
                        if (baseInits || classChains(*cit->second)) { chains = true; break; }
                    } else if (ifaceNames.count(b.name) == 0) { // not a class, not an interface → native base
                        chains = true; break;
                    }
                }
                memo[c.name] = chains ? 1 : 0;
                return chains;
            };
            for (auto& c : m.classes) {
                c.baseHasInit = classChains(c);
                // Review r4 #3: a constructor with no written `super()` in a class with a base class still
                // calls the base constructor (C# does so implicitly; TS requires the call — it threw "Must
                // call super constructor" — and PHP/Python only run it when called). The argument-less
                // implicit call is well-typed: sema refuses an omitted `super(…)` when the base needs args.
                if (c.hasInit && !c.hasSuper)
                    for (const auto& b : c.bases)
                        if (ifaceNames.count(b.name) == 0) { c.hasSuper = true; break; }
            }
            // P40 B2: the native root of each base chain (same walk, same cycle guard).
            std::unordered_set<std::string> visiting;
            std::function<std::string(const ir::Class&)> rootOf = [&](const ir::Class& c) -> std::string {
                if (!visiting.insert(c.name).second) return "";
                std::string root;
                for (const auto& b : c.bases) {
                    auto cit = byName.find(b.name);
                    if (cit != byName.end()) { root = rootOf(*cit->second); break; }
                    if (ifaceNames.count(b.name) == 0) { root = b.name; break; }
                }
                visiting.erase(c.name);
                return root;
            };
            for (auto& c : m.classes) c.baseRoot = rootOf(c);
        }
        m.loweringFailures = std::move(failures_); // P40 A2 backstop: compile() turns these into an ICE
        m.tempsUsed = tmpCounter_; // P40 review r3 2(c): the emitter's `fresh` counter continues from here
        return m;
    }

private:
    std::string target_;        // the active target name — binding/extern arms are picked for it
    const CompilationUnit* unit_ = nullptr; // P37 D: Meta resolution re-reads decl attributes
    std::unordered_map<std::string, const ExternAttrDecl*> externAttrMap_; // Tier 1 bindings by name
    std::unordered_set<std::string> materialized_;  // Tier 2 attribute records some Meta query constructs
    std::vector<std::string> attrImports_;          // Tier 1 import/using lines (deduped, verbatim)
    Backend::ConstSpelling spell_;                  // the active plugin's constant spellings (data, not names)
    int tmpCounter_ = 0;        // fresh-name counter for desugared bindings (e.g. `??`-on-Option)
    // P40: the target's native `a op= b` is faithful to its Binary rule (plugin trait); else compounds expand.
    bool nativeCompound_ = false;
    bool lateOperands_ = false; // review r4 #9: the target reads a variable operand when its operator runs
    bool nativeCoalesceAssign_ = false;
    // P40: surface sub-expressions of a compound target that were hoisted into a temp — `expr()` lowers each
    // to that temp's Var, so the write side and the re-read side both see the single evaluation.
    std::unordered_map<const Expr*, std::string> evalOnce_;
    std::vector<ir::LoweringFailure> failures_; // P40 A2: expressions with no lowering (a Polyglot bug)
    bool sawYield_ = false;     // set while lowering a function body that contains `yield`
    bool inExtension_ = false;  // set while lowering an extension body, so `this` lowers to `self`
    bool inOperator_ = false;   // set while lowering a (non-indexer) operator body; stamps This.insideOperator
    bool inEqOperator_ = false; // set while lowering a user `eq` body; stamps This.insideEqOperator (P37 C5)
    std::unordered_set<std::string> moduleGlobals_; // top-level let/const names (PHP `global` fact)
    std::unordered_map<std::string, std::unordered_set<std::string>> extensions_; // receiver type -> method names
    std::unordered_map<std::string, BindingEntry> bindings_; // "Type.member" -> FFI arms
    std::unordered_map<std::string, BindingEntry> ctorBindings_; // "Type" -> ctor FFI arms
    std::unordered_map<std::string, const std::vector<Param>*> extParams_; // "Recv.method" -> extension params (defaults)
    std::unordered_map<std::string, std::string> extEmitName_;    // "Recv.method" -> emitted name (std .pg guards)
    std::unordered_map<std::string, std::string> staticRedirect_; // "Type.member" -> prelude free fn (extern static with a body)
    std::unordered_set<std::string> freeFns_;                                     // top-level function names
    std::unordered_set<std::string> typeNames_;
    std::unordered_set<std::string> recordNames_;                    // user records (structural `==`)
    std::unordered_map<std::string, const RecordDecl*> records_;     // record name -> decl (field order for `with`)
    std::unordered_set<std::string> indexerTypes_;                   // types declaring `operator fn get`
    std::unordered_set<std::string> userEqTypes_;                    // types declaring `operator fn eq` (#49)
    std::unordered_map<std::string, std::unordered_set<std::string>> propertyMembers_; // type -> computed-property names
    struct StaticFieldFacts { bool isMutable; bool initIsLiteral; };
    std::unordered_map<std::string, StaticFieldFacts> staticFields_; // "Class.field" -> its facts (class statics)
    // A single literal — optionally a negated numeric one — is what every target accepts as a compile-time
    // constant (ir::ClassField::initIsLiteral).
    static bool isLiteralInit(const Expr& e) {
        switch (e.kind) {
            case ExprKind::IntLit: case ExprKind::FloatLit: case ExprKind::CharLit:
            case ExprKind::StringLit: case ExprKind::BoolLit: case ExprKind::NullLit:
                return true;
            case ExprKind::Unary:
                return e.text == "-" && e.lhs && (e.lhs->kind == ExprKind::IntLit || e.lhs->kind == ExprKind::FloatLit);
            default:
                return false;
        }
    }
    // Stamps a static Member with the facts of the class static field it names (no-op for anything else).
    void stampStaticField(ir::Member& m) const {
        auto it = staticFields_.find(m.staticType + "." + m.field);
        if (it == staticFields_.end()) return;
        m.staticField = true;
        m.staticFieldMutable = it->second.isMutable;
        m.staticFieldInitIsLiteral = it->second.initIsLiteral;
    }
    std::unordered_map<std::string, std::unordered_set<std::string>> enumCases_;
    std::unordered_map<std::string, std::string> caseUnion_;                       // case -> union
    std::unordered_map<std::string, std::vector<std::string>> caseFields_;         // case -> field names
    std::unordered_map<std::string, std::unordered_set<std::string>> unionCases_;  // union -> case names
    std::unordered_map<std::string, std::vector<std::string>> bases_;              // type -> named base(s)

    // Resolve a "Type.member" FFI binding, walking base types so an inherited binding (e.g. `message`
    // declared on the `Error` base) fires on a subclass receiver too.
    const BindingEntry* findBinding(const std::string& typeName, const std::string& member) const {
        if (auto it = bindings_.find(typeName + "." + member); it != bindings_.end()) return &it->second;
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
                // Every sub-slot in declaration order (issue #43): a binder binds a field, a LITERAL slot
                // carries its constant (its equality test must not be dropped), a wildcard binds nothing.
                for (std::size_t i = 0; i < p.sub.size(); ++i) {
                    ir::Binder b;
                    b.field = (fit != caseFields_.end() && i < fit->second.size())
                                  ? fit->second[i]
                                  : (p.sub[i].kind == PatKind::Binding ? p.sub[i].name : std::string());
                    if (p.sub[i].kind == PatKind::Literal && p.sub[i].literal) b.literal = expr(*p.sub[i].literal);
                    else if (p.sub[i].kind == PatKind::Binding) b.binding = p.sub[i].name;
                    // else: wildcard slot (no binding, no literal)
                    ip.binders.push_back(std::move(b));
                }
                break;
            }
            case PatKind::Binding:
                if (p.hasType && p.type.kind == TypeRef::Kind::Named && !p.type.name.empty()) {
                    // `d: Disk` — a runtime type test (#38): C# declaration pattern / instanceof / isinstance.
                    ip.kind = ir::PatternKind::TypeTest; ip.binding = p.name; ip.testType = p.type;
                } else if (!scrutEnum.empty() && enumCases_.at(scrutEnum).count(p.name)) {
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

    // An unguarded wildcard/binding arm catches everything — precomputed so the C# rule knows whether to
    // append its unreachable `_ => throw` default (P19).
    static void setHasCatchAll(ir::Match& m) {
        for (const auto& a : m.arms)
            if (!a.guard && (a.pattern.kind == ir::PatternKind::Wildcard || a.pattern.kind == ir::PatternKind::Binding))
                m.hasCatchAll = true;
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
        setHasCatchAll(*m);
        return m;
    }

    // `opt ?? rhs` (opt: Option<X>) -> `match opt { Some(v) => v, None => rhs }`. The element type is e.type
    // (sema gave the `??` node the element type X), so the bound `v` and the match are typed X.
    ir::ExprPtr coalesceOption(const Expr& e) {
        return optionMatch(e, [&] { return expr(*e.rhs); });
    }
    // `match <e.lhs> { Some(v) => v, None => noneBody() }` typed e.type — shared by `opt ?? d` and `opt!`
    // (whose None arm is the NullAssert fail form, P40 D12). The None body is lowered after the scrutinee.
    template <class NoneFn>
    ir::ExprPtr optionMatch(const Expr& e, NoneFn noneBody) {
        ir::ExprPtr scrut = expr(*e.lhs); // scrutinee keeps its Option<X> type
        return optionMatchOn(std::move(scrut), e.pos, e.type,
                             [&](const std::string& v) { return std::make_unique<ir::Var>(e.pos, e.type, v); },
                             noneBody);
    }
    // `match <scrut> { Some(v) => someBody(v), None => noneBody() }` typed `result`.
    template <class SomeFn, class NoneFn>
    ir::ExprPtr optionMatchOn(ir::ExprPtr scrut, SourcePos pos, const TypeRef& result, SomeFn someBody, NoneFn noneBody) {
        std::string v = "__opt" + std::to_string(tmpCounter_++);
        auto m = std::make_unique<ir::Match>(pos, result, std::move(scrut));
        ir::MatchArm some;
        some.pattern.kind = ir::PatternKind::Ctor;
        some.pattern.ctorCase = "Some";
        some.pattern.binders.push_back({"value", v}); // bind the Some payload field
        some.body = someBody(v);
        m->arms.push_back(std::move(some));
        ir::MatchArm none;
        none.pattern.kind = ir::PatternKind::Ctor;
        none.pattern.ctorCase = "None";
        none.body = noneBody();
        m->arms.push_back(std::move(none));
        setHasCatchAll(*m);
        return m;
    }

    // ---- The T?-representation boundary (SPEC §4.4) ----
    // A user generic's `T?` is an `Option<X>` at a concrete use site; a concrete `X?` is a native nullable.
    // Sema marks each crossing with an implicit Cast; these expand it with the existing match machinery
    // (no new IR node, no plugin rule).
    //
    // Option -> native: `match o { Some(v) => (X?)v, None => null }`, typed `X?` (the Some arm widens to the
    // nullable so C#'s switch expression has a natural type for a value-type X).
    ir::ExprPtr optionToNative(const Expr& e) {
        const TypeRef nat = e.castType;
        TypeRef elem = nat;
        elem.nullable = false;
        return optionMatchOn(expr(*e.lhs), e.pos, nat,
                             [&](const std::string& v) {
                                 return std::make_unique<ir::Cast>(e.pos, nat, std::make_unique<ir::Var>(e.pos, elem, v));
                             },
                             [&] { return std::make_unique<ir::NullLit>(e.pos, nat); });
    }
    // native -> Option: `match n { null => None, v => Some((X)v) }`, typed `Option<X>`. The scrutinee is
    // evaluated once (every target's match binds it); the bound `v` is the non-null read (C# unwraps a
    // Nullable<T>, the other targets read it bare — the P40 D14 narrowed-read convention).
    ir::ExprPtr nativeToOption(const Expr& e) {
        const TypeRef& opt = e.castType;
        const TypeRef nat = e.lhs->type;
        std::string v = "__opt" + std::to_string(tmpCounter_++);
        auto m = std::make_unique<ir::Match>(e.pos, opt, expr(*e.lhs));
        ir::MatchArm none;
        none.pattern.kind = ir::PatternKind::Literal;
        none.pattern.literal = std::make_unique<ir::NullLit>(e.pos, nat);
        // The None arm is up-cast to the union so the match has a natural type (`Option<X>`) where a target
        // infers through it (a C# generic call `present(n switch {…})`); a same-type Cast is the bare value on
        // the other targets (node.castSame).
        none.body = std::make_unique<ir::Cast>(e.pos, opt, std::make_unique<ir::MakeCase>(e.pos, opt, "Option", "None"));
        m->arms.push_back(std::move(none));
        ir::MatchArm some;
        some.pattern.kind = ir::PatternKind::Binding;
        some.pattern.binding = v;
        auto mc = std::make_unique<ir::MakeCase>(e.pos, opt, "Option", "Some");
        mc->fields.push_back({"value", std::make_unique<ir::Cast>(e.pos, opt.args[0], std::make_unique<ir::Var>(e.pos, nat, v))});
        some.body = std::move(mc);
        m->arms.push_back(std::move(some));
        setHasCatchAll(*m);
        return m;
    }
    static bool isOptionType(const TypeRef& t) {
        return t.kind == TypeRef::Kind::Named && t.name == "Option" && t.args.size() == 1 && !t.nullable;
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
        if (method == "band") return "&";  // P37 C (#63): bitwise/shift operator members
        if (method == "bor") return "|";
        if (method == "bxor") return "^";
        if (method == "shl") return "<<";
        if (method == "shr") return ">>";
        if (method == "neg") return "-";  // unary
        if (method == "bnot") return "~"; // unary
        return method;
    }

    ir::Class lowerClass(const ClassDecl& c) {
        ir::Class ic;
        ic.name = c.name;
        ic.pos = c.pos;
        ic.attrLines = renderAttrLines(c.attributes);
        ic.generics = generics(c.generics);
        ic.bases = c.bases;
        for (const auto& mem : c.members) {
            switch (mem.kind) {
                case MemberKind::Field:
                case MemberKind::Const: {
                    ir::ClassField f;
                    f.attrLines = renderAttrLines(mem.attributes);
                    f.name = mem.name;
                    f.isMutable = mem.isMutable;
                    f.isStatic = mem.kind == MemberKind::Const;
                    for (const auto& mod : mem.modifiers) if (mod == "static") f.isStatic = true;
                    f.type = mem.type;
                    if (mem.init) { f.init = expr(*mem.init); f.initIsLiteral = isLiteralInit(*mem.init); }
                    ic.fields.push_back(std::move(f));
                    break;
                }
                case MemberKind::Constructor:
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
        // Pair a get/set indexer so C# can merge them into one `this[...] { get; set; }` (issue #40). Safe:
        // ic.methods is complete here and its buffer is never reallocated afterwards (the Class is only moved).
        ir::Method* getM = nullptr;
        ir::Method* setM = nullptr;
        for (auto& mm : ic.methods) {
            if (mm.kind == ir::MethodKind::Operator && mm.opSymbol == "get") getM = &mm;
            else if (mm.kind == ir::MethodKind::Operator && mm.opSymbol == "set") setM = &mm;
        }
        if (getM && setM) getM->pairedSetter = setM;
        return ic;
    }

    // Lower a parameter, carrying its optional `= default` (so emitters reproduce it in the signature
    // and callers may omit trailing defaulted arguments — both C# and TS support default parameters).
    ir::Param irParam(const Param& p) {
        ir::Param ip{p.name, p.type, nullptr};
        for (const std::string& l : renderAttrLines(p.attributes)) {
            if (!ip.attrInline.empty()) ip.attrInline += " ";
            ip.attrInline += l;
        }
        if (p.hasDefault && p.defaultValue) ip.defaultValue = expr(*p.defaultValue);
        return ip;
    }

    ir::Method method(const Member& m) {
        ir::Method im;
        // P38/issue #69: the declaration's own origin, for the method-entry sequence point. Prefer the name
        // token; fall back to the member's start. A synthesized member (std overlay/skeleton) is unstamped
        // (fileId 0), which the emitter reads as "no known origin" and renders as a hidden directive.
        im.pos = m.namePos.fileId != 0 ? m.namePos : m.pos;
        im.name = m.name;
        im.attrLines = renderAttrLines(m.attributes);
        im.isAsync = m.isAsync;
        for (const auto& mod : m.modifiers) {
            if (mod == "static") im.isStatic = true;
            else if (mod == "open") im.isVirtual = true;
            else if (mod == "override") im.isOverride = true;
        }
        im.generics = generics(m.generics);
        // Same module-global fact as on ir::Function — a property getter's expr counts as its body.
        im.globalRefs = scanGlobalRefs(moduleGlobals_, m.params, m.body,
                                       m.kind == MemberKind::Property ? m.init.get() : m.exprBody.get());
        if (m.kind == MemberKind::Property) {
            im.kind = ir::MethodKind::Property;
            im.returnType = m.type;
            if (m.init) { im.exprBodied = true; im.exprBody = expr(*m.init); }
            else if (!m.body.empty()) { im.exprBodied = false; im.body = block(m.body); } // block-bodied getter (#39c)
            else im.exprBodied = true; // bound/skeleton property (overlay-armed) — unchanged
            if (m.hasSetter) { // accessor-block setter (#39c)
                im.propHasSetter = true;
                im.setterParamName = m.setterParam;
                im.setterBody = block(m.setterBody);
            }
            return im;
        }
        im.kind = (m.kind == MemberKind::Operator) ? ir::MethodKind::Operator : ir::MethodKind::Method;
        if (m.kind == MemberKind::Operator) {
            im.opSymbol = operatorSymbol(m.name);
            if (m.name == "explicit") { // P37 C: explicit conversion — method-form targets emit `to<T>()`
                im.opSymbol = "explicit";
                std::string tgt = m.returnType.name;
                if (!tgt.empty()) tgt[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(tgt[0])));
                im.name = "to" + tgt;
            }
        }
        im.returnType = m.returnType;
        for (const auto& p : m.params) im.params.push_back(irParam(p));
        // A real operator (not a `get`/`set` indexer) rebinds `this` on targets whose operator declaration
        // is static (C#): stamp the fact on every This lowered from its body (nested lambdas included). The
        // indexer accessors are INSTANCE members (C# `this[...]{get;set;}`), so `this` stays `this` (#40).
        // `get`/`set` indexers and `eq` (C# `override Equals`, #49) are INSTANCE members, so `this` stays
        // `this`; only a genuinely static operator (C# `operator +`, taking lhs/rhs) rebinds `this` -> lhs.
        // `hash` (P40 review A9) is an instance member everywhere (C# `override GetHashCode`, `__hash__`, `hash()`).
        const bool opBody = im.kind == ir::MethodKind::Operator && im.opSymbol != "get" &&
                            im.opSymbol != "set" && im.opSymbol != "==" && im.opSymbol != "hash";
        // P37 C5: an `eq` body is target-split — instance `override Equals` on C# (`this` stays) but the
        // static null-tolerant `T.eq(lhs, rhs)` on TS (`this` -> `lhs`). A second node-local fact carries
        // the distinction; each target's This rule picks its side.
        const bool eqBody = im.kind == ir::MethodKind::Operator && im.opSymbol == "==";
        if (opBody) inOperator_ = true;
        if (eqBody) inEqOperator_ = true;
        sawYield_ = false; // a `yield` anywhere in the body marks the method an iterator (mirrors ir::Function)
        if (m.exprBodied && m.exprBody) { im.exprBodied = true; im.exprBody = expr(*m.exprBody); }
        else { im.exprBodied = false; im.body = block(m.body); }
        im.isIterator = sawYield_;
        if (opBody) inOperator_ = false;
        if (eqBody) inEqOperator_ = false;
        return im;
    }

    ir::ExprPtr expr(const Expr& e) {
        // P40: a compound target's impure receiver/index, already evaluated once into a temp.
        if (!evalOnce_.empty())
            if (auto it = evalOnce_.find(&e); it != evalOnce_.end())
                return std::make_unique<ir::Var>(e.pos, e.type, it->second);
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
                if (e.narrowedNonNull && isOptionType(e.castType)) { // an Option local proven present (SPEC
                    // §4.4): the read unwraps it, `match o { Some(v) => v, None => <unreachable fail> }` — the
                    // `o!` form (P40 D12), whose None arm the narrowing proves dead.
                    return optionMatchOn(std::make_unique<ir::Var>(e.pos, e.castType, e.text), e.pos, e.type,
                                         [&](const std::string& v) { return std::make_unique<ir::Var>(e.pos, e.type, v); },
                                         [&]() -> ir::ExprPtr { return std::make_unique<ir::NullAssert>(e.pos, e.type, nullptr); });
                }
                if (e.narrowedNonNull) { // P40 D14: a flow-narrowed read keeps the variable's nullable type and
                    TypeRef vt = e.type; // converts to the non-null one (C# unwraps a Nullable<T>; the other
                    vt.nullable = true;  // targets' same-type conversion is the bare read)
                    return std::make_unique<ir::Cast>(e.pos, e.type, std::make_unique<ir::Var>(e.pos, vt, e.text));
                }
                if (e.fnRef) { // P40 review r4 item 20: a top-level function used as a value
                    auto ref = std::make_unique<ir::Call>(e.pos, e.type, e.text, /*isPrint=*/false);
                    ref->isRef = true;
                    ref->isFree = true;
                    ref->mangledCallee = e.overloadName.empty() ? e.text : e.overloadName;
                    ref->linkCallee = e.linkName;
                    return ref;
                }
                if (auto u = caseUnion_.find(e.text); u != caseUnion_.end()) // bare payload-free union case
                    return std::make_unique<ir::MakeCase>(e.pos, e.type, u->second, e.text);
                if (!e.staticOwner.empty()) { // bare ref to an enclosing-class static/const -> `Owner.name`
                    auto m = std::make_unique<ir::Member>(e.pos, e.type, nullptr, e.text, false);
                    m->staticType = e.staticOwner;
                    stampStaticField(*m);
                    return m;
                }
                return std::make_unique<ir::Var>(e.pos, e.type, e.text);
            case ExprKind::This: {
                if (inExtension_) return std::make_unique<ir::Var>(e.pos, e.type, "self"); // receiver alias
                auto t = std::make_unique<ir::This>(e.pos, e.type);
                t->insideOperator = inOperator_;
                t->insideEqOperator = inEqOperator_;
                return t;
            }
            case ExprKind::Unary: {
                auto u = std::make_unique<ir::Unary>(e.pos, e.type, e.text, expr(*e.lhs));
                // P37 C (#62): a user-typed operand routes to its neg/bnot operator on method-form targets.
                const TypeRef& ot = e.lhs->type;
                if (ot.kind == TypeRef::Kind::Named && !ot.name.empty())
                    u->operandIsUserType = !isPrimitiveTypeName(ot.name) && ot.name != "unit";
                return u;
            }
            case ExprKind::Await:     return std::make_unique<ir::Await>(e.pos, e.type, expr(*e.lhs));
            case ExprKind::Cast: {
                // The T?-representation boundary (SPEC §4.4): Option<X> <-> native X?.
                if (isOptionType(e.lhs->type) && !isOptionType(e.castType) && e.castType.nullable) return optionToNative(e);
                if (isOptionType(e.castType) && !isOptionType(e.lhs->type) && e.lhs->type.nullable) return nativeToOption(e);
                auto c = std::make_unique<ir::Cast>(e.pos, e.castType, expr(*e.lhs));
                c->convMethod = e.overloadName; // P37 C: user explicit conversion — method targets call to<T>()
                return c;
            }
            case ExprKind::Is: { // binding-less test (a bound form was rewritten by lowerIfCond)
                auto t = std::make_unique<ir::IsTest>(e.pos, namedType("bool"), expr(*e.lhs), e.castType);
                return t;
            }
            case ExprKind::As: return makeAsCast(e);
            // P40 A4: `x!` — a checked assertion on a nullable operand (ir::NullAssert, throwing at the
            // source site); a no-op on a non-nullable one (incl. a flow-narrowed variable); and on an
            // optional generic `Option<T>`, a match whose None arm is the fail form (D12).
            case ExprKind::NullAssert: {
                const TypeRef& ot = e.lhs->type;
                if (ot.kind == TypeRef::Kind::Named && ot.name == "Option" && !ot.args.empty())
                    return optionMatch(e, [&]() -> ir::ExprPtr {
                        return std::make_unique<ir::NullAssert>(e.pos, e.type, nullptr);
                    });
                if (!ot.nullable) return expr(*e.lhs);
                auto na = std::make_unique<ir::NullAssert>(e.pos, e.type, expr(*e.lhs));
                if (na->operand) markAbsentConsumer(*na->operand, "nullAssert"); // r4 item 16 (TS2871)
                na->operandIsSimple = e.lhs->kind == ExprKind::Name || e.lhs->kind == ExprKind::This;
                if (!na->operandIsSimple) na->tempName = "__nn" + std::to_string(tmpCounter_++);
                return na;
            }
            case ExprKind::Extern: {
                auto x = std::make_unique<ir::Extern>(e.pos, e.type, e.text);
                x->preludeKeys = e.preludeKeys; // P40 §0: a synthesized std actual's helper requirements
                return x;
            }
            case ExprKind::Binary: {
                // `opt ?? rhs` on an optional generic desugars to `match opt { Some(v) => v, None => rhs }`,
                // reusing the existing match machinery (no special-case in either emitter).
                if (e.text == "??" && e.lhs->type.kind == TypeRef::Kind::Named && e.lhs->type.name == "Option")
                    return coalesceOption(e);
                auto b = std::make_unique<ir::Binary>(e.pos, e.type, e.text, expr(*e.lhs), expr(*e.rhs));
                // P40 review A1: a bound left operand of `??` — `??` folds every absent form, so the emitter
                // may drop the template's absent-value normalization there (BackendSpec::absentNormalize).
                // P40 review r3 item 3: not only a DIRECT left operand — tsc's "always nullish" check (TS2871)
                // also looks through a nested `??`'s right operand and both arms of a conditional, so every
                // bound whose value flows into this `??` left operand along those paths is marked.
                if (e.text == "??" && b->lhs) markAbsentConsumer(*b->lhs, "coalesce");
                // Comparing against a null literal is a null TEST, never structural equality — don't
                // route it to the record equals() (TS/PHP `.equals(null)` would read fields off null).
                const bool rhsIsNullLit = e.rhs->kind == ExprKind::NullLit;
                if (!rhsIsNullLit && e.lhs->type.kind == TypeRef::Kind::Named && !e.lhs->type.name.empty()) {
                    const std::string& ln = e.lhs->type.name;
                    b->lhsIsRecord = recordNames_.count(ln) != 0;
                    b->lhsIsUserType = !isPrimitiveTypeName(ln) && ln != "unit";
                    b->lhsIsUnion = unionCases_.count(ln) != 0;   // #57: structural `==` over union cases
                    b->lhsHasUserEq = userEqTypes_.count(ln) != 0; // #49: user `operator fn eq` wins
                }
                if (e.text != "&&" && e.text != "||" && e.text != "??" && !rereadable(*e.rhs))
                    b->snapshotLhs = lateVariable(*b->lhs);
                return b;
            }
            case ExprKind::Member: {
                // Static const binding `Type.FIELD` (e.g. Math.PI as a bound `extern class` const):
                // the LHS is a type name, the binding template uses no receiver.
                if (e.lhs->kind == ExprKind::Name && typeNames_.count(e.lhs->text)) {
                    if (auto it = bindings_.find(e.lhs->text + "." + e.text); it != bindings_.end())
                        return makeBound(e.type, e.pos, nullptr, it->second, nullptr);
                }
                // A static member of a type — an enum case `Color.Green`, or a static const/field
                // `Owner.NAME`: the LHS is a *type name* (record/class in typeNames_, or an enum), not a
                // value. Stamp `staticType` so the Member rule takes its static branch (`Type::field` on PHP,
                // `Type.field` on C#/TS/Python — byte-identical to the value-receiver path there) instead of
                // lowering the type name to a value receiver (which PHP would emit as `$Color->Green`).
                if (e.lhs->kind == ExprKind::Name &&
                    (typeNames_.count(e.lhs->text) || enumCases_.count(e.lhs->text))) {
                    auto m = std::make_unique<ir::Member>(e.pos, e.type, nullptr, e.text, e.flag);
                    m->staticType = e.lhs->text;
                    stampStaticField(*m);
                    return m;
                }
                if (e.lhs->type.kind == TypeRef::Kind::Named) { // bound std/core property (List.count, Error.message)
                    if (auto arms = findBinding(e.lhs->type.name, e.text))
                        return makeBound(e.type, e.pos, e.lhs.get(), *arms, nullptr);
                }
                auto mem = std::make_unique<ir::Member>(e.pos, e.type, expr(*e.lhs), e.text, e.flag);
                if (e.flag && mem->object) markAbsentConsumer(*mem->object, "nullSafe"); // a `?.` receiver
                if (e.lhs->type.kind == TypeRef::Kind::Named) { // is the accessed member a computed property?
                    auto pit = propertyMembers_.find(e.lhs->type.name);
                    if (pit != propertyMembers_.end() && pit->second.count(e.text)) mem->isProperty = true;
                }
                return mem;
            }
            case ExprKind::Match:     return matchExpr(e);
            case ExprKind::IfExpr:    return std::make_unique<ir::Cond>(e.pos, e.type, expr(*e.lhs), expr(*e.rhs), expr(*e.extra));
            case ExprKind::Lambda: {
                auto lam = std::make_unique<ir::Lambda>(e.pos, e.type);
                for (const auto& p : e.params) lam->params.push_back({p.name, p.type});
                if (e.flag) { lam->exprBodied = false; lam->block = block(e.block); }
                else { lam->exprBodied = true; lam->body = expr(*e.lhs); }
                // P40 review r4 item 18: an expression lambda flowing into `(…) => unit` whose body is not a
                // statement expression (a bound property read, a lookup, a case literal) becomes a block holding
                // that expression as a discarded statement — C# admits only statement expressions there (CS0201).
                if (e.lambdaUnitResult && lam->exprBodied && lam->body && !isStatementExpr(*lam->body)) {
                    auto st = std::make_unique<ir::ExprStmt>(e.pos, std::move(lam->body));
                    st->discard = true;
                    lam->exprBodied = false;
                    lam->block.push_back(std::move(st));
                }
                return lam;
            }
            case ExprKind::Call: {
                // P37 D Tier 2: a Meta intrinsic resolves NOW, at transpile time — the output carries
                // only the answer: a bool literal, a construction of the attribute record from its
                // recorded constant args, or a typed null. Nothing is looked up at run time.
                if (e.lhs && e.lhs->kind == ExprKind::Member && e.lhs->lhs &&
                    e.lhs->lhs->kind == ExprKind::Name && e.lhs->lhs->text == "Meta" &&
                    e.typeArgs.size() == 2 && !typeNames_.count("Meta"))
                    return lowerMetaCall(e);
                if (e.lhs && e.lhs->kind == ExprKind::Member && !e.calleeIsField) { // method call `obj.method(args)`
                    // Static call `Type.method(args)`: the receiver is a type name, not a value.
                    if (e.lhs->lhs->kind == ExprKind::Name &&
                        (typeNames_.count(e.lhs->lhs->text) || isPrimitiveTypeName(e.lhs->lhs->text))) {
                        // A bound static method `Type.method(args)` (e.g. a future Math.sqrt as an
                        // `extern class` static): substitute the args into the template, no receiver.
                        if (auto b = bindings_.find(e.lhs->lhs->text + "." + e.lhs->text); b != bindings_.end())
                            return makeBound(e.type, e.pos, nullptr, b->second, &e.args);
                        if (auto rd = staticRedirect_.find(e.lhs->lhs->text + "." + e.lhs->text); rd != staticRedirect_.end()) {
                            auto call = std::make_unique<ir::Call>(e.pos, e.type, rd->second, /*isPrint=*/false);
                            call->isFree = true;
                            call->mangledCallee = rd->second;
                            for (const auto& a : e.args) call->args.push_back(expr(*a));
                            return call;
                        }
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
                    if (rt.kind == TypeRef::Kind::Named && extensions_.count(rt.name) &&
                        extensions_.at(rt.name).count(e.lhs->text))
                        if (auto en = extEmitName_.find(rt.name + "." + e.lhs->text); en != extEmitName_.end()) {
                            // P40 review B10: a std .pg guard is a plain free function — the receiver is argument 0.
                            auto call = std::make_unique<ir::Call>(e.pos, e.type, en->second, /*isPrint=*/false);
                            call->isFree = true;
                            call->mangledCallee = en->second;
                            call->args.push_back(expr(*e.lhs->lhs));
                            for (const auto& a : e.args) call->args.push_back(expr(*a));
                            if (auto p = extParams_.find(rt.name + "." + e.lhs->text); p != extParams_.end())
                                appendDefaults(*p->second, e.args.size(), call->args);
                            return call;
                        }
                    auto mc = std::make_unique<ir::MethodCall>(e.pos, e.type, expr(*e.lhs->lhs), e.lhs->text);
                    if (rt.kind == TypeRef::Kind::Named) {
                        auto it = extensions_.find(rt.name);
                        if (it != extensions_.end() && it->second.count(e.lhs->text)) mc->isExtension = true;
                        if (mc->isExtension)
                            if (auto en = extEmitName_.find(rt.name + "." + e.lhs->text); en != extEmitName_.end())
                                mc->method = en->second;
                    }
                    for (const auto& a : e.args) mc->args.push_back(expr(*a));
                    if (mc->isExtension) // P40 slice 7: an extension's omitted defaults are filled at the call
                        if (auto p = extParams_.find(rt.name + "." + e.lhs->text); p != extParams_.end())
                            appendDefaults(*p->second, e.args.size(), mc->args);
                    return mc;
                }
                if (e.lhs && e.lhs->kind != ExprKind::Name) {
                    // P40 review r3 2(a): any other callee is a function VALUE — `adder(1)(4)`, `fs[0](4)`,
                    // `(if c {f} else {g})(4)`, an immediately-invoked lambda. It used to lower to an empty
                    // callee name, so `adder(10)(4)` emitted `(4)` and returned the argument.
                    auto call = std::make_unique<ir::Call>(e.pos, e.type, "", /*isPrint=*/false);
                    call->calleeExpr = expr(*e.lhs);
                    for (const auto& a : e.args) call->args.push_back(expr(*a));
                    return call;
                }
                std::string callee = (e.lhs && e.lhs->kind == ExprKind::Name) ? e.lhs->text : "";
                if (!callee.empty() && typeNames_.count(callee)) { // record/class construction
                    if (auto it = ctorBindings_.find(callee); it != ctorBindings_.end()) { // bound ctor (extern class)
                        TypeRef ct; ct.name = callee; ct.args = e.typeArgs; // full type so `$T` renders `Name<args>`
                        return makeBound(ct, e.pos, /*recv=*/nullptr, it->second, &e.args);
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
                call->linkCallee = e.linkName; // C#: the one-scope link name (P40 review r3 2(e))
                call->uniqueCallee = e.uniqueName; // PHP: the one-scope unique name (P40 review r4 #13)
                call->typeArgs = e.typeArgs; // P40 A4: explicit `f<T>(…)` survives (C# CS0411 otherwise)
                for (const auto& a : e.args) call->args.push_back(expr(*a));
                return call;
            }
            case ExprKind::Index: {
                // Element access `recv[a]` or a multi-arg indexer `recv[a, b]`. The result type is the
                // element type sema resolved onto the node; `receiverHasIndexer` marks a user `operator fn
                // get` receiver (a target without `[]` overloading emits `recv.get(a, b)`). Every subscript
                // arg is carried in declaration order (issue #42); a bare `recv[]` degrades to a single `0`.
                auto ix = std::make_unique<ir::Index>(e.pos, e.type, expr(*e.lhs));
                if (!e.args.empty())
                    for (const auto& a : e.args) ix->indices.push_back(expr(*a));
                else
                    ix->indices.push_back(std::make_unique<ir::IntLit>(e.pos, namedType("i32"), "0"));
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
            // A surface form with no lowering rule must FAIL LOUDLY, never silently emit a placeholder: a
            // silent `0` once masked an unlowered `if`-expression as a cross-target-identical miscompile, and
            // the later poison identifier still let TS/Python/PHP builds exit 0 (failing only at runtime).
            // P40 A2: sema refuses both forms below (a range outside a `for` iterable — the `for` site lowers
            // its range directly — and any `super` but the hoisted base-ctor call), so reaching them is a
            // compiler bug: record it and compile() reports an internal compiler error and writes nothing.
            // The switch is exhaustive on purpose (no `default:`): a new ExprKind is a build error (/we4062,
            // -Werror=switch) until it gets an arm here.
            case ExprKind::Range: return unlowered(e, "range");
            case ExprKind::Super: return unlowered(e, "super");
        }
        return unlowered(e, "unknown"); // an out-of-range enum value
    }
    ir::ExprPtr unlowered(const Expr& e, const char* kind) {
        failures_.push_back({e.pos, kind});
        return std::make_unique<ir::Extern>(e.pos, e.type, "__polyglot_unlowered_expr__");
    }

    ir::StmtPtr stmt(const Stmt& s) {
        switch (s.kind) {
            case StmtKind::Let: {
                if (s.dropUnused) return nullptr; // r4: an unread `_x = <constant>` has no effect (C# CS0219)
                ir::Type t = s.hasDeclType ? s.declType : (s.value ? s.value->type : TypeRef{});
                auto let = std::make_unique<ir::Let>(s.pos, s.name, s.isMutable, t, expr(*s.value));
                let->declExplicit = s.hasDeclType;
                let->tupleNames = s.tupleNames; // `let (a, b) = t` destructuring (#39b)
                return let;
            }
            case StmtKind::Assign: {
                // A plain write `recv[a, b] = v` through a USER indexer lowers to an IndexAssign so each
                // target renders it its own way (C# native subscript-set vs a `set(...)` method call). A
                // native collection write (`xs[i] = v`, no user indexer) or a compound op stays an Assign.
                if (s.op == "=" && s.target && s.target->kind == ExprKind::Index && s.target->lhs &&
                    s.target->lhs->type.kind == TypeRef::Kind::Named &&
                    indexerTypes_.count(s.target->lhs->type.name) != 0) {
                    auto ia = std::make_unique<ir::IndexAssign>(s.pos);
                    ia->receiver = expr(*s.target->lhs);
                    for (const auto& a : s.target->args) ia->indices.push_back(expr(*a));
                    ia->value = expr(*s.value);
                    return ia;
                }
                // P37 C7: a compound op lowers to `target = <op>(target, value)` when the target is a
                // USER type (operator-method routing: C# native operator, TS static-on-type, Python
                // dunder) or sits behind a USER indexer (which has no native `[...] op=` on any
                // method-form target — it needs the get+set pair even for scalar elements). Sema
                // restricts the target to pure bases, so re-lowering the read side is single-eval-safe.
                const bool userTypedTarget = s.op != "=" && s.op != "??=" && s.target &&
                    s.target->type.kind == TypeRef::Kind::Named && !s.target->type.name.empty() &&
                    !isPrimitiveTypeName(s.target->type.name) && s.target->type.name != "unit";
                const bool userIndexerTarget = s.op != "=" && s.op != "??=" && s.target &&
                    s.target->kind == ExprKind::Index && s.target->lhs &&
                    s.target->lhs->type.kind == TypeRef::Kind::Named &&
                    indexerTypes_.count(s.target->lhs->type.name) != 0;
                if (userTypedTarget || userIndexerTarget) {
                    const std::string binOp = s.op.substr(0, s.op.size() - 1);
                    auto read = expr(*s.target);
                    auto rhs = expr(*s.value);
                    auto b = std::make_unique<ir::Binary>(s.pos, s.target->type, binOp,
                                                          std::move(read), std::move(rhs));
                    b->lhsIsUserType = userTypedTarget; // operator-method routing (scalar elements stay native)
                    if (userIndexerTarget) {
                        auto ia = std::make_unique<ir::IndexAssign>(s.pos); // user indexer: get+set pair
                        ia->receiver = expr(*s.target->lhs);
                        for (const auto& a : s.target->args) ia->indices.push_back(expr(*a));
                        ia->value = std::move(b);
                        return ia;
                    }
                    return std::make_unique<ir::Assign>(s.pos, expr(*s.target), "=", std::move(b));
                }
                // P40: a compound op on a built-in scalar slot (local, field, static, element) is the binary
                // op plus a store — `t op= v` means `t = t op v` with t's receiver/indices evaluated once. On
                // a target whose native compound is not faithful to its Binary rule (no §3.C wrap on TS/
                // Python/PHP, float `/=` on Python, `+=` on a PHP string) lower it to exactly that, so the
                // Binary rule's masking, integer division and concatenation apply. An impure receiver/index
                // is evaluated once into a temp first (in source order, ahead of the value, as `op=` does).
                if (!nativeCompound_ && s.op != "=" && s.op != "??=" && s.target)
                    return expandCompound(s);
                if (!nativeCoalesceAssign_ && s.op == "??=" && s.target) return expandCoalesceAssign(s);
                return std::make_unique<ir::Assign>(s.pos, expr(*s.target), s.op, expr(*s.value));
            }
            case StmtKind::ExprStmt: {
                auto ev = expr(*s.value);
                // A match whose value is discarded is a STATEMENT match (void/side-effecting arms) — C#
                // must render a switch statement, not a switch expression (issue #52).
                if (ev->kind == ir::ExprKind::Match) static_cast<ir::Match&>(*ev).isStatement = true;
                const bool discard = !isStatementExpr(*ev); // P40 review r4 item 18 (C# CS0201)
                auto es = std::make_unique<ir::ExprStmt>(s.pos, std::move(ev));
                es->discard = discard;
                return es;
            }
            case StmtKind::If: {
                // P37 B3: `x is T name` on the condition's `&&` spine — hoist `let name = x as T` before
                // the `if` (via pendingStmts_) and test `name != null` instead. Single-eval by
                // construction (the operand is evaluated once, inside the AsCast), and every target then
                // reads the narrowed binding directly: C# `var c = x as T; if (c != null …)`, TS
                // `const c = x instanceof T ? x : null; if (c !== null …)`, Python/PHP alike.
                auto cond = lowerIfCond(*s.value);
                // The hoisted binding Lets must survive the NESTED block() calls below (each flushes
                // pendingStmts_ into ITS body) — steal them now, restore after, so the OUTER block()
                // emits them directly above this `if`.
                std::vector<ir::StmtPtr> hoisted;
                hoisted.swap(pendingStmts_);
                auto node = std::make_unique<ir::If>(s.pos, std::move(cond));
                node->thenBody = block(s.thenBody);
                if (s.hasElse) { node->hasElse = true; node->elseBody = block(s.elseBody); }
                pendingStmts_ = std::move(hoisted);
                return node;
            }
            case StmtKind::While: {
                auto node = std::make_unique<ir::While>(s.pos, expr(*s.value));
                node->body = block(s.thenBody);
                node->isDoWhile = s.isDoWhile;
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
                    node->rangeType = s.value->type; // sema's widened bound type = the loop variable's
                    node->rangeEndSimple = s.value->rangeEndSimple; // D7: else hoisted, evaluated once
                    node->inclusiveMayWrap = node->inclusive && !s.value->rangeEndBelowMax; // P40 review B6
                    if (!node->rangeEndSimple) node->rangeEndTemp = "__end" + std::to_string(tmpCounter_++);
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
                    // P40 review A9: a binding the clause never reads (`catch (e: T)` with `e` unused, or the
                    // `_` discard) is dropped — every target has a binding-less typed catch, and C# warns
                    // CS0168 on an unused catch variable. Name-based, so a shadowed use keeps it (safe side).
                    if (!c.name.empty()) {
                        GlobalRefScan uses;
                        uses.ex(c.guard.get());
                        for (const auto& b : c.body) uses.st(b.get());
                        if (!uses.usedSeen.count(c.name)) ic.binding.clear();
                    }
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

    // P40 review r4 item 18: is this expression, used as a statement, a "statement expression" on every target — an
    // invocation, construction or await, a statement-position match, or something that yields no value (unit, or a
    // type sema left open)? Anything else computes a value only to drop it, which C# refuses (CS0201) unless it is
    // spelled as a discard; ExprStmt::discard carries that fact to the emitter.
    static bool isStatementExpr(const ir::Expr& x) {
        switch (x.kind) {
            case ir::ExprKind::Call: case ir::ExprKind::MethodCall: case ir::ExprKind::New: case ir::ExprKind::Await:
                return true;
            case ir::ExprKind::Match: return static_cast<const ir::Match&>(x).isStatement;
            default: break;
        }
        const TypeRef& t = x.type;
        return t.kind == TypeRef::Kind::Named && (t.name == "unit" || t.name.empty());
    }

    // P40 review r3 item 3: mark every bound whose value flows straight into a `??` left operand — the operand
    // itself, a nested `??`'s right side (its left side was marked when that `??` was lowered) and both arms
    // of a conditional. `??` folds every absent form, so their absent-normalization (TS `?? null`) may go.
    // P40 review r4 item 16: generalized from the `??` left operand to every absence-testing consumer (`x!`,
    // a `?.` receiver); the target's manifest says which of them fold every absent form (absentFoldedBy).
    static void markAbsentConsumer(ir::Expr& x, const char* consumer) {
        switch (x.kind) {
            case ir::ExprKind::Bound: {
                auto& b = static_cast<ir::Bound&>(x);
                if (b.absentConsumer.empty()) b.absentConsumer = consumer;
                break;
            }
            case ir::ExprKind::Binary: {
                auto& b = static_cast<ir::Binary&>(x);
                if (b.op == "??" && b.rhs) markAbsentConsumer(*b.rhs, consumer);
                break;
            }
            case ir::ExprKind::Cond: {
                auto& c = static_cast<ir::Cond&>(x);
                if (c.then) markAbsentConsumer(*c.then, consumer);
                if (c.els) markAbsentConsumer(*c.els, consumer);
                break;
            }
            default: break;
        }
    }

    // P40: may this surface expression be lowered twice (the write and the re-read of a compound target)
    // without changing behaviour? Names, `this`, literals, field reads off such a base, and built-in
    // arithmetic over them. A call, a computed property (getter), a bound std member, a user operator or
    // anything else is not — it is evaluated once into a temp instead.
    bool rereadable(const Expr& e) const {
        auto primitive = [](const TypeRef& t) {
            return t.kind == TypeRef::Kind::Named && !t.nullable && isPrimitiveTypeName(t.name);
        };
        switch (e.kind) {
            case ExprKind::IntLit: case ExprKind::FloatLit: case ExprKind::CharLit: case ExprKind::StringLit:
            case ExprKind::BoolLit: case ExprKind::NullLit: case ExprKind::Name: case ExprKind::This:
                return true;
            case ExprKind::Member: {
                if (!e.lhs || e.flag) return false; // a null-safe `?.` read is not a plain field read
                if (e.lhs->kind == ExprKind::Name &&
                    (typeNames_.count(e.lhs->text) || enumCases_.count(e.lhs->text)))
                    return true; // a static field `Owner.f`
                if (e.lhs->type.kind == TypeRef::Kind::Named) {
                    if (findBinding(e.lhs->type.name, e.text)) return false;
                    auto pit = propertyMembers_.find(e.lhs->type.name);
                    if (pit != propertyMembers_.end() && pit->second.count(e.text)) return false;
                }
                return rereadable(*e.lhs);
            }
            case ExprKind::Unary:
                return e.lhs && primitive(e.lhs->type) && rereadable(*e.lhs);
            case ExprKind::Binary:
                return e.lhs && e.rhs && e.text != "??" && primitive(e.lhs->type) && primitive(e.rhs->type) &&
                       rereadable(*e.lhs) && rereadable(*e.rhs);
            default:
                return false;
        }
    }

    // P40: `t op= v` -> `t = t op v` (ir::Assign::compoundOp keeps the source op for the emitter's fold).
    // P40 review r4 item 17: `t ??= v` on a target without a native `??=` (Python) is `if t == null { t = v }` —
    // the value is evaluated only when the target is null, and the target's receiver/indices once (the same
    // hoisting as a compound: an impure value may rebind a receiver before the store re-reads it).
    ir::StmtPtr expandCoalesceAssign(const Stmt& s) {
        std::vector<const Expr*> hoisted = hoistTargetParts(s);
        auto read = expr(*s.target);
        auto write = expr(*s.target);
        for (const Expr* p : hoisted) evalOnce_.erase(p);
        auto test = std::make_unique<ir::Binary>(s.pos, namedType("bool"), "==", std::move(read),
                                                 std::make_unique<ir::NullLit>(s.pos, s.target->type));
        auto node = std::make_unique<ir::If>(s.pos, std::move(test));
        node->thenBody.push_back(std::make_unique<ir::Assign>(s.pos, std::move(write), "=", expr(*s.value)));
        return node;
    }

    ir::StmtPtr expandCompound(const Stmt& s) {
        std::vector<const Expr*> hoisted = hoistTargetParts(s);
        const Expr& t = *s.target;
        auto write = expr(t);
        auto read = expr(t);
        for (const Expr* p : hoisted) evalOnce_.erase(p);
        const std::string binOp = s.op.substr(0, s.op.size() - 1);
        auto b = std::make_unique<ir::Binary>(s.pos, t.type, binOp, std::move(read), expr(*s.value));
        // Review r4 #9: on a late-read target, a native `t op= v` reads t after v ran — it is kept expanded, and a
        // variable t is snapshotted (a field/static/element read is fetched in order).
        const bool valueImpure = s.value && !rereadable(*s.value);
        if (valueImpure) b->snapshotLhs = lateVariable(*b->lhs);
        auto a = std::make_unique<ir::Assign>(s.pos, std::move(write), "=", std::move(b));
        a->compoundOp = s.op;
        a->keepExpanded = lateOperands_ && valueImpure;
        return a;
    }

    // The receiver/index parts of a compound target that must be evaluated once into a temp (pushed to
    // pendingStmts_, recorded in evalOnce_ — the caller erases them after lowering its reads/writes).
    std::vector<const Expr*> hoistTargetParts(const Stmt& s) {
        const Expr& t = *s.target;
        std::vector<const Expr*> parts; // the target's receiver, then its indices — source evaluation order
        if ((t.kind == ExprKind::Member || t.kind == ExprKind::Index) && t.lhs) parts.push_back(t.lhs.get());
        if (t.kind == ExprKind::Index)
            for (const auto& a : t.args) parts.push_back(a.get());
        // P40 review r3 2(d): C# and JS evaluate the target's receiver and indices BEFORE the value; Python and
        // PHP evaluate an assignment's right side first. So when the value may have side effects (it could
        // rebind `G.cur` or bump an index variable), every part that could change is hoisted too — only
        // literals, `this` and type names (a static's owner) are immune.
        const bool valueImpure = s.value && !rereadable(*s.value);
        auto immune = [&](const Expr& p) {
            switch (p.kind) {
                case ExprKind::IntLit: case ExprKind::FloatLit: case ExprKind::CharLit: case ExprKind::StringLit:
                case ExprKind::BoolLit: case ExprKind::NullLit: case ExprKind::This: return true;
                case ExprKind::Name: return typeNames_.count(p.text) > 0 || enumCases_.count(p.text) > 0;
                default: return false;
            }
        };
        std::vector<const Expr*> hoisted;
        for (const Expr* p : parts) {
            if (rereadable(*p) && !(valueImpure && !immune(*p))) continue;
            std::string name = "__c" + std::to_string(tmpCounter_++);
            pendingStmts_.push_back(std::make_unique<ir::Let>(p->pos, name, false, p->type, expr(*p)));
            evalOnce_[p] = name;
            hoisted.push_back(p);
        }
        return hoisted;
    }

    // P40 review r4 #9: on a target that reads a variable operand only when its operator runs (PHP's CV
    // operands), is this lowered left operand such a variable — a local, parameter or module value, under any
    // conversions (an identity cast prints bare)? Anything else is evaluated into a temporary in order.
    bool lateVariable(const ir::Expr& x) const {
        if (!lateOperands_) return false;
        const ir::Expr* p = &x;
        while (p->kind == ir::ExprKind::Cast && static_cast<const ir::Cast&>(*p).operand)
            p = static_cast<const ir::Cast&>(*p).operand.get();
        return p->kind == ir::ExprKind::Var;
    }

    std::vector<ir::StmtPtr> block(const std::vector<StmtPtr>& body) {
        std::vector<ir::StmtPtr> out;
        for (const auto& s : body) {
            auto st = stmt(*s);
            // Statements a lowering hoisted out of `s` (P37 B3 `is`-binding Lets) go first.
            for (auto& p : pendingStmts_) out.push_back(std::move(p));
            pendingStmts_.clear();
            if (st) out.push_back(std::move(st));
        }
        return out;
    }

    // P37 D Tier 2 — resolve a Meta query against the unit's recorded attribute attachments.
    ir::ExprPtr lowerMetaCall(const Expr& e) {
        const std::string& method = e.lhs->text;
        const std::string& tn = e.typeArgs[0].name;
        const std::string& an = e.typeArgs[1].name;
        const std::vector<AttrUse>* attrs = nullptr;
        if (method == "param") { // Meta.param<T, A>("method", "param")
            const std::string& mem = e.args.size() > 0 ? e.args[0]->text : std::string();
            const std::string& par = e.args.size() > 1 ? e.args[1]->text : std::string();
            auto paramAttrs = [&](const std::vector<Member>& ms) -> const std::vector<AttrUse>* {
                for (const auto& m : ms)
                    if (m.name == mem)
                        for (const auto& p : m.params)
                            if (p.name == par) return &p.attributes;
                return nullptr;
            };
            for (const auto& c : unit_->classes) if (c.name == tn) attrs = paramAttrs(c.members);
            for (const auto& r : unit_->records) if (r.name == tn && !attrs) attrs = paramAttrs(r.members);
        } else if (method == "member") {
            const std::string& mem = e.args.empty() ? std::string() : e.args[0]->text;
            auto memberAttrs = [&](const std::vector<Member>& ms) -> const std::vector<AttrUse>* {
                for (const auto& m : ms) if (m.name == mem) return &m.attributes;
                return nullptr;
            };
            for (const auto& c : unit_->classes) if (c.name == tn) attrs = memberAttrs(c.members);
            for (const auto& r : unit_->records) if (r.name == tn && !attrs) attrs = memberAttrs(r.members);
        } else {
            for (const auto& c : unit_->classes) if (c.name == tn) attrs = &c.attributes;
            for (const auto& r : unit_->records) if (r.name == tn) attrs = &r.attributes;
        }
        const AttrUse* use = nullptr;
        if (attrs)
            for (const auto& a : *attrs) if (a.name == an) use = &a;
        if (method == "has")
            return std::make_unique<ir::BoolLit>(e.pos, namedType("bool"), use != nullptr);
        if (!use) {
            TypeRef nt = namedType(an);
            nt.nullable = true;
            return std::make_unique<ir::NullLit>(e.pos, nt);
        }
        const RecordDecl* adecl = nullptr;
        for (const auto& r : unit_->records) if (r.name == an) adecl = &r;
        auto n = std::make_unique<ir::New>(e.pos, namedType(an), an);
        if (adecl) {
            for (std::size_t i = 0; i < adecl->fields.size(); ++i) {
                const Expr* v = nullptr;
                std::size_t positional = 0;
                for (const auto& arg : use->args) {
                    if (arg.name.empty()) {
                        if (positional == i) v = arg.value.get();
                        ++positional;
                    } else if (arg.name == adecl->fields[i].name) {
                        v = arg.value.get();
                    }
                }
                if (!v && adecl->fields[i].hasDefault) v = adecl->fields[i].defaultValue.get();
                if (v) n->args.push_back(expr(*v));
            }
        }
        return n;
    }

    // P37 B: `x as T` → ir::AsCast (checked conversion, null on failure). Targets whose guard
    // re-evaluates the operand (TS/Python/PHP) bind an inline temp when it isn't a simple variable.
    ir::ExprPtr makeAsCast(const Expr& e) {
        TypeRef nt = e.castType;
        nt.nullable = true;
        auto cast = std::make_unique<ir::AsCast>(e.pos, nt, expr(*e.lhs), e.castType);
        cast->operandIsSimple = e.lhs->kind == ExprKind::Name || e.lhs->kind == ExprKind::This;
        if (!cast->operandIsSimple) cast->tempName = "__as" + std::to_string(tmpCounter_++);
        return cast;
    }
    // P37 B3: lower an `if` condition, rewriting each bound `is` on the `&&` spine (see StmtKind::If).
    ir::ExprPtr lowerIfCond(const Expr& e) {
        if (e.kind == ExprKind::Binary && e.text == "&&") {
            auto b = std::make_unique<ir::Binary>(e.pos, namedType("bool"), "&&",
                                                  lowerIfCond(*e.lhs), lowerIfCond(*e.rhs));
            return b;
        }
        if (e.kind == ExprKind::Is && !e.text.empty()) {
            TypeRef nt = e.castType;
            nt.nullable = true;
            pendingStmts_.push_back(std::make_unique<ir::Let>(e.pos, e.text, false, nt, makeAsCast(e)));
            return std::make_unique<ir::Binary>(e.pos, namedType("bool"), "!=",
                                                std::make_unique<ir::Var>(e.pos, nt, e.text),
                                                std::make_unique<ir::NullLit>(e.pos, TypeRef{}));
        }
        return expr(e);
    }
    std::vector<ir::StmtPtr> pendingStmts_; // hoisted by the CURRENT statement's lowering; flushed by block()
};

} // namespace

// #41: C# forbids a nested-scope local reusing a method-scope local's name (CS0136), unlike TS/Python/PHP.
// This target-gated pass renames a for-binding that collides with a Let/param name in the same body (and
// rewrites its in-body references); non-colliding loops are untouched, so existing C# output stays
// byte-identical. Runs after lowering, only for the C# target.
namespace {
struct CsScopeLegalizer {
    int seq = 0;
    static std::unordered_set<std::string> paramNames(const std::vector<ir::Param>& ps) {
        std::unordered_set<std::string> s; for (const auto& p : ps) s.insert(p.name); return s;
    }
    void run(ir::Module& m) {
        for (auto& f : m.functions)  fixBody(f.body, paramNames(f.params));
        for (auto& f : m.extensions) fixBody(f.body, paramNames(f.params));
        for (auto& r : m.records) for (auto& mm : r.methods) if (!mm.exprBodied) fixBody(mm.body, paramNames(mm.params));
        for (auto& c : m.classes) {
            for (auto& mm : c.methods) if (!mm.exprBodied) fixBody(mm.body, paramNames(mm.params));
            fixBody(c.initBody, paramNames(c.initParams));
        }
    }
    void fixBody(std::vector<ir::StmtPtr>& body, std::unordered_set<std::string> declared) {
        for (auto& s : body) collectLets(*s, declared);
        for (auto& s : body) renameFors(*s, declared);
    }
    void collectLets(ir::Stmt& s, std::unordered_set<std::string>& out) {
        using K = ir::StmtKind;
        switch (s.kind) {
            case K::Let: { auto& l = static_cast<ir::Let&>(s); if (!l.name.empty()) out.insert(l.name); for (auto& n : l.tupleNames) out.insert(n); break; }
            case K::If: { auto& i = static_cast<ir::If&>(s); for (auto& b : i.thenBody) collectLets(*b, out); for (auto& b : i.elseBody) collectLets(*b, out); break; }
            case K::While: for (auto& b : static_cast<ir::While&>(s).body) collectLets(*b, out); break;
            case K::For: for (auto& b : static_cast<ir::For&>(s).body) collectLets(*b, out); break;
            case K::Use: for (auto& b : static_cast<ir::Use&>(s).body) collectLets(*b, out); break;
            case K::Try: { auto& t = static_cast<ir::Try&>(s); for (auto& b : t.body) collectLets(*b, out); for (auto& c : t.catches) for (auto& b : c.body) collectLets(*b, out); for (auto& b : t.finallyBody) collectLets(*b, out); break; }
            default: break;
        }
    }
    void renameFors(ir::Stmt& s, const std::unordered_set<std::string>& declared) {
        using K = ir::StmtKind;
        switch (s.kind) {
            case K::For: {
                auto& f = static_cast<ir::For&>(s);
                if (!f.binding.empty() && f.binding != "_" && declared.count(f.binding)) {
                    std::string nn = f.binding + "__cs" + std::to_string(seq++);
                    for (auto& b : f.body) renameStmt(*b, f.binding, nn);
                    f.binding = nn;
                }
                for (auto& b : f.body) renameFors(*b, declared);
                break;
            }
            case K::If: { auto& i = static_cast<ir::If&>(s); for (auto& b : i.thenBody) renameFors(*b, declared); for (auto& b : i.elseBody) renameFors(*b, declared); break; }
            case K::While: for (auto& b : static_cast<ir::While&>(s).body) renameFors(*b, declared); break;
            case K::Use: for (auto& b : static_cast<ir::Use&>(s).body) renameFors(*b, declared); break;
            case K::Try: { auto& t = static_cast<ir::Try&>(s); for (auto& b : t.body) renameFors(*b, declared); for (auto& c : t.catches) for (auto& b : c.body) renameFors(*b, declared); for (auto& b : t.finallyBody) renameFors(*b, declared); break; }
            default: break;
        }
    }
    void renameStmt(ir::Stmt& s, const std::string& o, const std::string& n) {
        using K = ir::StmtKind;
        switch (s.kind) {
            case K::Let: { auto& l = static_cast<ir::Let&>(s); if (l.init) renameExpr(*l.init, o, n); break; }
            case K::Assign: { auto& a = static_cast<ir::Assign&>(s); if (a.target) renameExpr(*a.target, o, n); if (a.value) renameExpr(*a.value, o, n); break; }
            case K::IndexAssign: { auto& ia = static_cast<ir::IndexAssign&>(s); if (ia.receiver) renameExpr(*ia.receiver, o, n); for (auto& x : ia.indices) renameExpr(*x, o, n); if (ia.value) renameExpr(*ia.value, o, n); break; }
            case K::ExprStmt: renameExpr(*static_cast<ir::ExprStmt&>(s).expr, o, n); break;
            case K::Return: { auto& r = static_cast<ir::Return&>(s); if (r.value) renameExpr(*r.value, o, n); break; }
            case K::Yield: { auto& y = static_cast<ir::Yield&>(s); if (y.value) renameExpr(*y.value, o, n); break; }
            case K::Throw: { auto& t = static_cast<ir::Throw&>(s); if (t.value) renameExpr(*t.value, o, n); break; }
            case K::If: { auto& i = static_cast<ir::If&>(s); if (i.cond) renameExpr(*i.cond, o, n); for (auto& b : i.thenBody) renameStmt(*b, o, n); for (auto& b : i.elseBody) renameStmt(*b, o, n); break; }
            case K::While: { auto& w = static_cast<ir::While&>(s); if (w.cond) renameExpr(*w.cond, o, n); for (auto& b : w.body) renameStmt(*b, o, n); break; }
            case K::For: { auto& f = static_cast<ir::For&>(s); if (f.rangeStart) renameExpr(*f.rangeStart, o, n); if (f.rangeEnd) renameExpr(*f.rangeEnd, o, n); if (f.iterable) renameExpr(*f.iterable, o, n); if (f.binding != o) for (auto& b : f.body) renameStmt(*b, o, n); break; }
            case K::Use: { auto& u = static_cast<ir::Use&>(s); if (u.init) renameExpr(*u.init, o, n); for (auto& b : u.body) renameStmt(*b, o, n); break; }
            case K::Try: { auto& t = static_cast<ir::Try&>(s); for (auto& b : t.body) renameStmt(*b, o, n); for (auto& c : t.catches) for (auto& b : c.body) renameStmt(*b, o, n); for (auto& b : t.finallyBody) renameStmt(*b, o, n); break; }
            default: break;
        }
    }
    void renameExpr(ir::Expr& e, const std::string& o, const std::string& n) {
        using K = ir::ExprKind;
        switch (e.kind) {
            case K::Var: { auto& v = static_cast<ir::Var&>(e); if (v.name == o) v.name = n; break; }
            case K::Unary: renameExpr(*static_cast<ir::Unary&>(e).operand, o, n); break;
            case K::Await: renameExpr(*static_cast<ir::Await&>(e).operand, o, n); break;
            case K::Cast:  renameExpr(*static_cast<ir::Cast&>(e).operand, o, n); break;
            case K::NullAssert: { auto& na = static_cast<ir::NullAssert&>(e); if (na.operand) renameExpr(*na.operand, o, n); break; }
            case K::IsTest: renameExpr(*static_cast<ir::IsTest&>(e).operand, o, n); break;
            case K::AsCast: renameExpr(*static_cast<ir::AsCast&>(e).operand, o, n); break;
            case K::Binary: { auto& b = static_cast<ir::Binary&>(e); renameExpr(*b.lhs, o, n); renameExpr(*b.rhs, o, n); break; }
            case K::Cond: { auto& c = static_cast<ir::Cond&>(e); renameExpr(*c.cond, o, n); renameExpr(*c.then, o, n); renameExpr(*c.els, o, n); break; }
            case K::Call: { auto& c = static_cast<ir::Call&>(e); if (c.calleeExpr) renameExpr(*c.calleeExpr, o, n); for (auto& a : c.args) renameExpr(*a, o, n); break; }
            case K::MethodCall: { auto& mc = static_cast<ir::MethodCall&>(e); if (mc.object) renameExpr(*mc.object, o, n); for (auto& a : mc.args) renameExpr(*a, o, n); break; }
            case K::Member: { auto& m = static_cast<ir::Member&>(e); if (m.object) renameExpr(*m.object, o, n); break; }
            case K::Index: { auto& i = static_cast<ir::Index&>(e); if (i.receiver) renameExpr(*i.receiver, o, n); for (auto& x : i.indices) renameExpr(*x, o, n); break; }
            case K::ListLit: for (auto& x : static_cast<ir::ListLit&>(e).elements) renameExpr(*x, o, n); break;
            case K::Tuple: for (auto& x : static_cast<ir::Tuple&>(e).elements) renameExpr(*x, o, n); break;
            case K::New: for (auto& a : static_cast<ir::New&>(e).args) renameExpr(*a, o, n); break;
            case K::Interp: for (auto& h : static_cast<ir::Interp&>(e).holes) renameExpr(*h, o, n); break;
            case K::MakeCase: for (auto& f : static_cast<ir::MakeCase&>(e).fields) renameExpr(*f.value, o, n); break;
            case K::With: { auto& w = static_cast<ir::With&>(e); if (w.base) renameExpr(*w.base, o, n); for (auto& f : w.fields) renameExpr(*f.value, o, n); break; }
            case K::Bound: { auto& b = static_cast<ir::Bound&>(e); if (b.receiver) renameExpr(*b.receiver, o, n); for (auto& a : b.args) renameExpr(*a, o, n); break; }
            case K::Lambda: { auto& l = static_cast<ir::Lambda&>(e); bool shadow = false; for (auto& p : l.params) if (p.name == o) shadow = true; if (shadow) break; if (l.exprBodied) { if (l.body) renameExpr(*l.body, o, n); } else for (auto& b : l.block) renameStmt(*b, o, n); break; }
            case K::Match: { auto& m = static_cast<ir::Match&>(e); if (m.scrutinee) renameExpr(*m.scrutinee, o, n); for (auto& a : m.arms) { if (a.guard) renameExpr(*a.guard, o, n); if (a.body) renameExpr(*a.body, o, n); } break; }
            default: break;
        }
    }
};
} // namespace

ir::Module lower(const CompilationUnit& unit, const std::string& target) {
    Lowerer lowerer(unit, target);
    ir::Module m = lowerer.run(unit);
    // Target-trait passes gate on PLUGIN FLAGS, never target names (the Core cannot know what
    // languages exist): scope legalization for targets that forbid shadowed nested locals (#41,
    // CS0136-class rules), and block-lambda hoisting for expression-only-lambda targets.
    const Backend* b = findBackend(target);
    if (b && b->forbidsShadowedLocals()) { CsScopeLegalizer cs; cs.run(m); }
    analyzeCaptures(m); // P25 §4.18: classify closure captures + stamp cell decisions onto the IR
    if (b && b->expressionOnlyLambdas()) hoistBlockLambdas(m);
    return m;
}

} // namespace mintplayer::polyglot
