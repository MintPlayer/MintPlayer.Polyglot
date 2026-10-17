#include "mintplayer/polyglot/reachability.hpp"

#include <algorithm>
#include <cctype>
#include <deque>
#include <unordered_map>
#include <vector>

// P40 §0 — see reachability.hpp for the contract. The walker below follows the reference set the S14 spike
// proved against all conformance programs on all four targets (identical run output, 21–38% smaller files).

namespace mintplayer::polyglot {

namespace {

// Every prelude declaration of a module, by every name a reference can use.
struct PreludeIndex {
    std::unordered_map<std::string, std::vector<const ir::Union*>> unions;
    std::unordered_map<std::string, std::vector<const ir::Record*>> records;
    std::unordered_map<std::string, std::vector<const ir::Class*>> classes;
    std::unordered_map<std::string, std::vector<const ir::Interface*>> interfaces;
    std::unordered_map<std::string, std::vector<const ir::Enum*>> enums;
    std::unordered_map<std::string, std::vector<const ir::Global*>> globals;
    // functions + extensions, keyed by source name AND mangled name (overloads share a source name)
    std::unordered_map<std::string, std::vector<const ir::Function*>> functions;
    std::unordered_map<std::string, std::string> caseToUnion; // a prelude union case name -> its union
    std::unordered_map<std::string, std::string> externTypeText; // extern type name -> its target spelling

    bool names(const std::string& n) const {
        return unions.count(n) || records.count(n) || classes.count(n) || interfaces.count(n) || enums.count(n) ||
               globals.count(n) || functions.count(n);
    }
};

PreludeIndex buildIndex(const ir::Module& m) {
    PreludeIndex ix;
    for (const auto& d : m.unions)
        if (ir::isPreludeOrigin(d.originModule)) {
            ix.unions[d.name].push_back(&d);
            for (const auto& c : d.cases) ix.caseToUnion[c.name] = d.name;
        }
    for (const auto& d : m.records)    if (ir::isPreludeOrigin(d.originModule)) ix.records[d.name].push_back(&d);
    for (const auto& d : m.classes)    if (ir::isPreludeOrigin(d.originModule)) ix.classes[d.name].push_back(&d);
    for (const auto& d : m.interfaces) if (ir::isPreludeOrigin(d.originModule)) ix.interfaces[d.name].push_back(&d);
    for (const auto& d : m.enums)      if (ir::isPreludeOrigin(d.originModule)) ix.enums[d.name].push_back(&d);
    for (const auto& d : m.globals)    if (ir::isPreludeOrigin(d.originModule)) ix.globals[d.name].push_back(&d);
    auto fn = [&](const ir::Function& d) {
        if (!ir::isPreludeOrigin(d.originModule)) return;
        ix.functions[d.name].push_back(&d);
        if (!d.mangledName.empty() && d.mangledName != d.name) ix.functions[d.mangledName].push_back(&d);
    };
    for (const auto& d : m.functions)  fn(d);
    for (const auto& d : m.extensions) fn(d);
    for (const auto& et : m.externTypes)
        if (!et.typeTmpl.empty()) ix.externTypeText[et.name] = et.typeTmpl;
    return ix;
}

class Walker {
public:
    explicit Walker(const PreludeIndex& ix) : ix_(ix) {}

    std::set<std::string> reached;

    // Mark a referenced name. A prelude declaration enters the worklist once; a union case reaches its union.
    void mark(const std::string& n) {
        if (n.empty()) return;
        if (ix_.names(n)) {
            if (reached.insert(n).second) work_.push_back(n);
            return;
        }
        if (auto c = ix_.caseToUnion.find(n); c != ix_.caseToUnion.end())
            if (reached.insert(c->second).second) work_.push_back(c->second);
    }

    // The backstop: every identifier in opaque target text that names a prelude declaration (or case).
    void text(const std::string& s) {
        std::size_t i = 0;
        while (i < s.size()) {
            const unsigned char ch = static_cast<unsigned char>(s[i]);
            if (std::isalpha(ch) || ch == '_') {
                std::size_t j = i + 1;
                while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_')) ++j;
                mark(s.substr(i, j - i));
                i = j;
            } else {
                ++i;
            }
        }
    }

    void drain() {
        while (!work_.empty()) {
            const std::string n = work_.front();
            work_.pop_front();
            if (auto it = ix_.unions.find(n); it != ix_.unions.end())         for (auto* d : it->second) unionDecl(*d);
            if (auto it = ix_.records.find(n); it != ix_.records.end())       for (auto* d : it->second) record(*d);
            if (auto it = ix_.classes.find(n); it != ix_.classes.end())       for (auto* d : it->second) klass(*d);
            if (auto it = ix_.interfaces.find(n); it != ix_.interfaces.end()) for (auto* d : it->second) iface(*d);
            if (auto it = ix_.globals.find(n); it != ix_.globals.end())       for (auto* d : it->second) global(*d);
            if (auto it = ix_.functions.find(n); it != ix_.functions.end())
                for (auto* d : it->second) {
                    // Both spellings stay in step: a phase-2 re-lowering of the shared prelude may reference
                    // it by either, and the other overloads of a source name are reached with it.
                    mark(d->name);
                    mark(d->mangledName);
                    function(*d);
                }
            // enums reference nothing
        }
    }

    void unionDecl(const ir::Union& u) {
        generics(u.generics);
        for (const auto& c : u.cases)
            for (const auto& f : c.fields) type(f.type);
    }
    void record(const ir::Record& r) {
        lines(r.attrLines);
        generics(r.generics);
        for (const auto& b : r.bases) type(b);
        for (const auto& f : r.fields) type(f.type);
        for (const auto& m : r.methods) method(m);
    }
    void klass(const ir::Class& c) {
        lines(c.attrLines);
        generics(c.generics);
        for (const auto& b : c.bases) type(b);
        for (const auto& f : c.fields) {
            lines(f.attrLines);
            type(f.type);
            if (f.init) expr(*f.init);
        }
        params(c.initParams);
        for (const auto& a : c.superArgs) expr(*a);
        body(c.initBody);
        for (const auto& m : c.methods) method(m);
    }
    void iface(const ir::Interface& i) {
        generics(i.generics);
        for (const auto& b : i.bases) type(b);
        for (const auto& m : i.methods) method(m);
    }
    void global(const ir::Global& g) {
        type(g.type);
        if (g.init) expr(*g.init);
    }
    void function(const ir::Function& f) {
        lines(f.attrLines);
        generics(f.generics);
        params(f.params);
        type(f.returnType);
        if (f.exprBody) expr(*f.exprBody);
        body(f.body);
        for (const auto& g : f.globalRefs) mark(g);
    }

private:
    const PreludeIndex& ix_;
    std::deque<std::string> work_;

    void lines(const std::vector<std::string>& ls) { for (const auto& l : ls) text(l); }

    void type(const ir::Type& t) {
        if (t.kind == TypeRef::Kind::Named) {
            mark(t.name);
            if (auto et = ix_.externTypeText.find(t.name); et != ix_.externTypeText.end()) text(et->second);
        }
        for (const auto& a : t.args) type(a);
        for (const auto& r : t.ret) type(r);
    }
    void generics(const std::vector<ir::GenericParam>& gs) {
        for (const auto& g : gs)
            for (const auto& b : g.bounds) type(b);
    }
    void params(const std::vector<ir::Param>& ps) {
        for (const auto& p : ps) {
            type(p.type);
            if (p.defaultValue) expr(*p.defaultValue);
        }
    }
    void body(const std::vector<ir::StmtPtr>& b) {
        for (const auto& s : b)
            if (s) stmt(*s);
    }
    void pattern(const ir::Pattern& p) {
        if (p.literal) expr(*p.literal);
        mark(p.enumType);
        mark(p.ctorCase);
        for (const auto& b : p.binders)
            if (b.literal) expr(*b.literal);
        type(p.testType);
    }
    void method(const ir::Method& m) {
        lines(m.attrLines);
        generics(m.generics);
        params(m.params);
        type(m.returnType);
        if (m.exprBody) expr(*m.exprBody);
        body(m.body);
        body(m.setterBody);
        for (const auto& g : m.globalRefs) mark(g);
    }

    void expr(const ir::Expr& e) {
        using K = ir::ExprKind;
        type(e.type);
        switch (e.kind) {
            case K::Int: case K::Float: case K::Bool: case K::Str: case K::Char: case K::Null: case K::This:
                break;
            case K::Var: mark(static_cast<const ir::Var&>(e).name); break;
            case K::Unary: expr(*static_cast<const ir::Unary&>(e).operand); break;
            case K::Await: expr(*static_cast<const ir::Await&>(e).operand); break;
            case K::Binary: {
                const auto& b = static_cast<const ir::Binary&>(e);
                expr(*b.lhs);
                expr(*b.rhs);
                break;
            }
            case K::Cast: expr(*static_cast<const ir::Cast&>(e).operand); break;
            case K::IsTest: {
                const auto& t = static_cast<const ir::IsTest&>(e);
                expr(*t.operand);
                type(t.testType);
                break;
            }
            case K::AsCast: {
                const auto& a = static_cast<const ir::AsCast&>(e);
                expr(*a.operand);
                type(a.castType);
                break;
            }
            case K::Call: {
                const auto& c = static_cast<const ir::Call&>(e);
                mark(c.callee);
                mark(c.mangledCallee);
                for (const auto& a : c.args) expr(*a);
                break;
            }
            case K::MethodCall: {
                const auto& m = static_cast<const ir::MethodCall&>(e);
                if (m.object) expr(*m.object);
                mark(m.staticType);
                if (m.isExtension) mark(m.method);
                for (const auto& a : m.args) expr(*a);
                break;
            }
            case K::Member: {
                const auto& m = static_cast<const ir::Member&>(e);
                if (m.object) expr(*m.object);
                mark(m.staticType);
                break;
            }
            case K::New: {
                const auto& n = static_cast<const ir::New&>(e);
                mark(n.typeName);
                for (const auto& t : n.typeArgs) type(t);
                for (const auto& a : n.args) expr(*a);
                break;
            }
            case K::MakeCase: {
                const auto& m = static_cast<const ir::MakeCase&>(e);
                mark(m.unionName);
                mark(m.caseName);
                for (const auto& f : m.fields)
                    if (f.value) expr(*f.value);
                break;
            }
            case K::Match: {
                const auto& m = static_cast<const ir::Match&>(e);
                expr(*m.scrutinee);
                for (const auto& a : m.arms) {
                    pattern(a.pattern);
                    if (a.guard) expr(*a.guard);
                    if (a.body) expr(*a.body);
                }
                break;
            }
            case K::Lambda: {
                const auto& l = static_cast<const ir::Lambda&>(e);
                params(l.params);
                if (l.body) expr(*l.body);
                body(l.block);
                for (const auto& c : l.captures) type(c.declType);
                break;
            }
            case K::Extern: text(static_cast<const ir::Extern&>(e).code); break;
            case K::Index: {
                const auto& i = static_cast<const ir::Index&>(e);
                expr(*i.receiver);
                for (const auto& a : i.indices) expr(*a);
                break;
            }
            case K::ListLit: {
                const auto& l = static_cast<const ir::ListLit&>(e);
                type(l.elem);
                for (const auto& a : l.elements) expr(*a);
                break;
            }
            case K::Tuple:
                for (const auto& a : static_cast<const ir::Tuple&>(e).elements) expr(*a);
                break;
            case K::Bound: {
                const auto& b = static_cast<const ir::Bound&>(e);
                if (b.receiver) expr(*b.receiver);
                for (const auto& a : b.args) expr(*a);
                text(b.tmpl);
                break;
            }
            case K::Interp:
                for (const auto& h : static_cast<const ir::Interp&>(e).holes) expr(*h);
                break;
            case K::Cond: {
                const auto& c = static_cast<const ir::Cond&>(e);
                expr(*c.cond);
                expr(*c.then);
                expr(*c.els);
                break;
            }
            case K::With: {
                const auto& w = static_cast<const ir::With&>(e);
                expr(*w.base);
                for (const auto& f : w.fields)
                    if (f.value) expr(*f.value);
                for (const auto& a : w.ctorArgs)
                    if (a) expr(*a);
                break;
            }
        }
    }

    void stmt(const ir::Stmt& s) {
        using K = ir::StmtKind;
        switch (s.kind) {
            case K::Let: {
                const auto& l = static_cast<const ir::Let&>(s);
                type(l.type);
                if (l.init) expr(*l.init);
                break;
            }
            case K::Assign: {
                const auto& a = static_cast<const ir::Assign&>(s);
                expr(*a.target);
                expr(*a.value);
                break;
            }
            case K::IndexAssign: {
                const auto& a = static_cast<const ir::IndexAssign&>(s);
                expr(*a.receiver);
                for (const auto& i : a.indices) expr(*i);
                expr(*a.value);
                break;
            }
            case K::ExprStmt: expr(*static_cast<const ir::ExprStmt&>(s).expr); break;
            case K::If: {
                const auto& i = static_cast<const ir::If&>(s);
                expr(*i.cond);
                body(i.thenBody);
                body(i.elseBody);
                break;
            }
            case K::While: {
                const auto& w = static_cast<const ir::While&>(s);
                expr(*w.cond);
                body(w.body);
                break;
            }
            case K::For: {
                const auto& f = static_cast<const ir::For&>(s);
                if (f.rangeStart) expr(*f.rangeStart);
                if (f.rangeEnd) expr(*f.rangeEnd);
                if (f.iterable) expr(*f.iterable);
                body(f.body);
                break;
            }
            case K::Return: {
                const auto& r = static_cast<const ir::Return&>(s);
                if (r.value) expr(*r.value);
                break;
            }
            case K::Break: case K::Continue: break;
            case K::Yield: expr(*static_cast<const ir::Yield&>(s).value); break;
            case K::Throw: {
                const auto& t = static_cast<const ir::Throw&>(s);
                if (t.value) expr(*t.value);
                break;
            }
            case K::Try: {
                const auto& t = static_cast<const ir::Try&>(s);
                body(t.body);
                for (const auto& c : t.catches) {
                    type(c.type);
                    if (c.guard) expr(*c.guard);
                    body(c.body);
                }
                body(t.finallyBody);
                break;
            }
            case K::Use: {
                const auto& u = static_cast<const ir::Use&>(s);
                type(u.type);
                expr(*u.init);
                body(u.body);
                break;
            }
            case K::LocalFunc: {
                const auto& l = static_cast<const ir::LocalFunc&>(s);
                params(l.params);
                body(l.body);
                break;
            }
        }
    }
};

} // namespace

std::set<std::string> preludeReach(const ir::Module& m, const std::set<std::string>* forcedReach) {
    const PreludeIndex ix = buildIndex(m);
    Walker w(ix);
    if (forcedReach)
        for (const auto& n : *forcedReach) w.mark(n);
    for (const auto& d : m.unions)     if (!ir::isPreludeOrigin(d.originModule)) w.unionDecl(d);
    for (const auto& d : m.records)    if (!ir::isPreludeOrigin(d.originModule)) w.record(d);
    for (const auto& d : m.classes)    if (!ir::isPreludeOrigin(d.originModule)) w.klass(d);
    for (const auto& d : m.interfaces) if (!ir::isPreludeOrigin(d.originModule)) w.iface(d);
    for (const auto& d : m.globals)    if (!ir::isPreludeOrigin(d.originModule)) w.global(d);
    for (const auto& d : m.functions)  if (!ir::isPreludeOrigin(d.originModule)) w.function(d);
    for (const auto& d : m.extensions) if (!ir::isPreludeOrigin(d.originModule)) w.function(d);
    // (a user enum references nothing)
    w.drain(); // (drain marks both spellings of every reached function — see Walker::drain)
    return w.reached;
}

std::set<std::string> pruneUnreachablePrelude(ir::Module& m, const std::set<std::string>* forcedReach) {
    std::set<std::string> reached = preludeReach(m, forcedReach);
    auto erase = [](auto& vec, auto drop) { vec.erase(std::remove_if(vec.begin(), vec.end(), drop), vec.end()); };
    auto dropNamed = [&](const auto& d) { return ir::isPreludeOrigin(d.originModule) && !reached.count(d.name); };
    auto dropFn = [&](const ir::Function& d) {
        return ir::isPreludeOrigin(d.originModule) && !reached.count(d.name) &&
               (d.mangledName.empty() || !reached.count(d.mangledName));
    };
    erase(m.enums, dropNamed);
    erase(m.unions, dropNamed);
    erase(m.records, dropNamed);
    erase(m.classes, dropNamed);
    erase(m.interfaces, dropNamed);
    erase(m.globals, dropNamed);
    erase(m.functions, dropFn);
    erase(m.extensions, dropFn);
    return reached;
}

} // namespace mintplayer::polyglot
