#include "mintplayer/polyglot/sema.hpp"
#include "mintplayer/polyglot/flow.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <deque>
#include <set>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mintplayer::polyglot {

namespace {

// Semantic types are reused TypeRefs. An empty Named ("") is the "unconstrained / unknown" type — the
// checker stays lenient on it (and on generic params and std types we don't model yet) to avoid false
// errors, and only reports a mismatch/missing-member when both sides are fully known. The scalar rules
// are unchanged: they run on scalarTyOf() of the operand types, so conformance behaviour is preserved.

TypeRef tUnknown() { return TypeRef{}; }
TypeRef tNamed(std::string n) { return namedType(std::move(n)); }
TypeRef tNullableUnknown() { TypeRef t; t.nullable = true; return t; }

// A type every component of which is concrete (no empty-name / unconstrained hole anywhere). Used to
// decide whether a declaration's initializer conveyed enough to reconstruct the declared type on every
// target. A nullable-with-no-underlying-type (bare `null`) is NOT fully known.
bool typeFullyKnown(const TypeRef& t) {
    if (t.kind == TypeRef::Kind::Named && t.name.empty()) return false;
    for (const auto& a : t.args) if (!typeFullyKnown(a)) return false;
    for (const auto& r : t.ret)  if (!typeFullyKnown(r)) return false;
    return true;
}

// Does an *un-annotated* binding's initializer leave the type un-inferable — i.e. would the missing type
// reach a backend as a placeholder (C# `object` / TS evolving-`any`) rather than the author's intent?
// (issue #27 root cause). Precisely the forms that lose type information to emission — an empty/unknown-
// element list literal (`[]`), a bare `null`, and a lambda with un-annotated parameters. Deliberately NOT
// a bare unconstrained `tUnknown()` from an unmodeled generic/std call: the checker stays lenient there
// (see the note above `tUnknown`), so flagging it would be a false positive.
bool uninferableInit(const Expr& value, const TypeRef& inferred) {
    if (value.kind == ExprKind::Lambda) {           // a lambda's type is reconstructible iff every param is typed
        for (const auto& p : value.params) if (p.type.absent()) return true;
        return false;
    }
    if (inferred.kind == TypeRef::Kind::Named && inferred.name == "List" &&
        (inferred.args.empty() || !typeFullyKnown(inferred.args[0]))) return true;   // `[]` -> List<unknown>
    // bare `null` (P40 review r6: a NAMED placeholder — a nullable function type with no parameters, `(() => unit)?`,
    // also has no name and no args, and `let cb = pick(flag)` was refused as un-inferable)
    if (inferred.kind == TypeRef::Kind::Named && inferred.nullable && inferred.name.empty() && inferred.args.empty())
        return true;
    return false;
}

// The fix-it example for the "add an explicit type" diagnostic, tailored to which un-inferable form it is.
// A lambda anywhere in a value's result position (an `if`/`match` arm, a `??` operand).
static bool yieldsLambda(const Expr& v) {
    switch (v.kind) {
        case ExprKind::Lambda: return true;
        case ExprKind::IfExpr: return (v.rhs && yieldsLambda(*v.rhs)) || (v.extra && yieldsLambda(*v.extra));
        case ExprKind::Match:
            for (const auto& a : v.arms) if (a.body && yieldsLambda(*a.body)) return true;
            return false;
        case ExprKind::Binary: return v.text == "??" && ((v.lhs && yieldsLambda(*v.lhs)) || (v.rhs && yieldsLambda(*v.rhs)));
        default: return false;
    }
}

std::string annotationHint(const std::string& name, const Expr& value, const TypeRef& inferred) {
    if (value.kind == ExprKind::Lambda)
        return "annotate the lambda's parameter types, e.g. '(x: i32) => …'";
    if (value.kind == ExprKind::NullLit)
        return "add an explicit type annotation, e.g. 'var " + name + ": T? = null'";
    // P40 review r5 (part 2): a function-valued initializer gets a function-typed example, not a list's.
    if (yieldsLambda(value))
        return "add an explicit function type annotation, e.g. 'let " + name + ": ((i32) => i32)? = …'";
    // P40 review r6 (part 2): the example follows the slot's own kind — a list only for a list.
    const bool list = inferred.kind == TypeRef::Kind::Named && (inferred.name == "List" || inferred.name == "Array");
    if (list) {
        const std::string t = inferred.name == "Array" ? std::string("i32[]") : std::string("List<i32>");
        return "add an explicit type annotation, e.g. 'var " + name + ": " + t + (inferred.nullable ? "?" : "") +
               " = " + (value.kind == ExprKind::ListLit ? "[]" : "…") + "'";
    }
    return "add an explicit type annotation, e.g. 'var " + name + ": T? = …' (the type it holds)";
}

bool isNumeric(Ty t) { return t == Ty::I32 || t == Ty::F64; }

// The MVP scalar lattice (Ty) only tracks i32/f64; the other numeric widths (i64/u64/i8/i16/u8/u16/u32/
// f32) are recognized by name so binary arithmetic keeps them typed — the backends need the exact width
// to lower faithfully (i64 -> BigInt, etc.).
bool isNumericTypeName(const TypeRef& t) {
    if (t.kind != TypeRef::Kind::Named || t.nullable || !t.args.empty()) return false;
    const std::string& n = t.name;
    return n == "i8" || n == "i16" || n == "i32" || n == "i64" || n == "u8" || n == "u16" ||
           n == "u32" || n == "u64" || n == "f32" || n == "f64";
}
bool sameNamedType(const TypeRef& a, const TypeRef& b) {
    return a.kind == TypeRef::Kind::Named && b.kind == TypeRef::Kind::Named &&
           a.name == b.name && !a.nullable && !b.nullable;
}

int numWidth(const std::string& n) {
    if (n == "i8" || n == "u8") return 8;
    if (n == "i16" || n == "u16") return 16;
    if (n == "i32" || n == "u32") return 32;
    if (n == "i64" || n == "u64") return 64;
    return 0;
}
bool isSignedInt(const std::string& n)   { return n == "i8" || n == "i16" || n == "i32" || n == "i64"; }
bool isUnsignedInt(const std::string& n) { return n == "u8" || n == "u16" || n == "u32" || n == "u64"; }
bool isFloatName(const std::string& n)   { return n == "f32" || n == "f64"; }

// Does an (unsuffixed) integer-literal text hold a value representable in i64/u64? Used by literal
// adoption in checkConvert (G3, wave 2). Base 0: decimal and 0x hex alike. The one edge left out is
// the exact INT64_MIN spelling (its magnitude overflows the positive parse) — that falls back to the
// widening-cast path.
bool literalFits64(const std::string& text, bool isUnsigned) {
    errno = 0;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(text.c_str(), &end, 0);
    if (errno == ERANGE || end == text.c_str() || *end != '\0') return false;
    return isUnsigned || v <= 9223372036854775807ull;
}

// The magnitude of an integer literal (underscores and a width suffix dropped); ~0 when it does not parse or
// overflows u64. P40 review r4: used to range-check a literal against the slot it flows into.
unsigned long long literalMagnitude(const std::string& text) {
    std::string d;
    for (char c : text) {
        if (c == '_') continue;
        if ((c == 'i' || c == 'u') && !d.empty()) break; // the width suffix (hex digits never include i/u)
        d += c;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(d.c_str(), &end, 0);
    if (errno == ERANGE || end == d.c_str() || *end != '\0') return ~0ull;
    return v;
}

// Is `from -> to` an implicit, value-preserving widening (no precision loss, no sign surprise)? Everything
// else (narrowing, i64->f64, signed->unsigned) requires an explicit cast. (SPEC §3.)
bool isLosslessWiden(const TypeRef& from, const TypeRef& to) {
    if (!isNumericTypeName(from) || !isNumericTypeName(to) || from.name == to.name) return false;
    const std::string& f = from.name;
    const std::string& t = to.name;
    if (isFloatName(t)) // ints up to 32 bits (and f32) fit exactly in an f64 mantissa; ->f32 is lossy
        return t == "f64" && (f == "f32" || ((isSignedInt(f) || isUnsignedInt(f)) && numWidth(f) <= 32));
    if (isFloatName(f)) return false; // float -> int is never implicit
    if (isSignedInt(f) && isSignedInt(t))     return numWidth(t) > numWidth(f);
    if (isUnsignedInt(f) && isUnsignedInt(t)) return numWidth(t) > numWidth(f);
    if (isUnsignedInt(f) && isSignedInt(t))   return numWidth(t) > numWidth(f); // unsigned fits in wider signed
    return false; // signed -> unsigned never implicit
}

bool isBuiltinType(const std::string& n) {
    static const std::unordered_set<std::string> b = {
        "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64",
        "f32", "f64", "bool", "char", "string", "unit",
        // (`Error` and `Iterable` are NOT builtins — they are `extern class`es in the always-linked core
        //  prelude (compiler.cpp STD_CORE), so they resolve like any declared type.)
    };
    return b.count(n) != 0;
}

// An integer literal's type comes from its width suffix (`100u8`, `0i64`); a bare literal is i32.
std::string intLitType(const std::string& text) {
    static const char* suffixes[] = {"i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64"};
    for (const char* s : suffixes) {
        std::size_t n = std::char_traits<char>::length(s);
        if (text.size() > n && text.compare(text.size() - n, n, s) == 0) return s;
    }
    return "i32";
}

// A float literal's type comes from its suffix: `f`/`f32` => f32, `f64`/`d` => f64; a bare literal
// (`1.5`, `1e3`) is f64. Mirrors C#'s `f`/`d` shorthands so `310f` is single-precision as written.
std::string floatLitType(const std::string& text) {
    if (text.size() >= 3 && text.compare(text.size() - 3, 3, "f32") == 0) return "f32";
    if (text.size() >= 3 && text.compare(text.size() - 3, 3, "f64") == 0) return "f64";
    char last = text.back();
    if (last == 'f') return "f32";
    return "f64";
}

// A §3.B-refused construct named in type position. Returns a targeted "Polyglot refuses X because …"
// message (never a generic "unknown type"), or nullptr. The refused surface has no grammar, but a user
// reaching for a familiar platform type by name should get told *why* and *what to use instead* — the
// PRD §3.B "refuse out loud, never miscompile" rule.
const char* refusedReason(const std::string& n) {
    if (n == "Thread" || n == "Mutex" || n == "Monitor" || n == "Semaphore" || n == "Lock" ||
        n == "Interlocked" || n == "ReaderWriterLock" || n == "ThreadPool")
        return "Polyglot refuses threads and locks — it targets single-threaded async only (PRD §3.B)";
    if (n == "decimal" || n == "Decimal")
        return "Polyglot refuses 'decimal' — no portable cross-target decimal exists; use a fixed-point std type (PRD §3.B/§3.D)";
    if (n == "IntPtr" || n == "UIntPtr" || n == "nint" || n == "nuint" || n == "Span" ||
        n == "ReadOnlySpan" || n == "Pointer")
        return "Polyglot refuses pointers and unsafe memory (no 'unsafe', '*T', 'stackalloc', raw Span) (PRD §3.B)";
    if (n == "dynamic")
        return "Polyglot refuses 'dynamic' and runtime code generation (PRD §3.B)";
    if (n == "Expression")
        return "Polyglot refuses LINQ expression trees — a lambda is always an executable closure, never code-as-data (PRD §3.B)";
    if (n == "Activator")
        return "Polyglot refuses runtime reflection ('Activator', open-world 'Type.GetType') — compile-time metadata only (PRD §3.B)";
    return nullptr;
}

// Map an operator symbol to its overload method name (PRD §6.1 / SPEC §6.1; bitwise added by P37 C, #63).
std::string operatorMethod(const std::string& op) {
    if (op == "+") return "plus";
    if (op == "-") return "minus";
    if (op == "*") return "times";
    if (op == "/") return "div";
    if (op == "%") return "rem";
    if (op == "==") return "eq";
    if (op == "<") return "lt";
    if (op == "<=") return "le";
    if (op == ">") return "gt";
    if (op == ">=") return "ge";
    if (op == "&") return "band";
    if (op == "|") return "bor";
    if (op == "^") return "bxor";
    if (op == "<<") return "shl";
    if (op == ">>") return "shr";
    return "";
}

// The `operator fn` names with a meaning (SPEC §6.1). `explicit`/`implicit`/`hash` carry their own rules.
bool isOperatorMemberName(const std::string& n) {
    static const char* const names[] = {"plus", "minus", "times", "div", "rem", "eq", "lt", "le", "gt", "ge",
                                        "band", "bor", "bxor", "shl", "shr", "neg", "bnot", "get", "set",
                                        "explicit", "implicit", "hash"};
    for (const char* k : names) if (n == k) return true;
    return false;
}

struct MemberInfo {
    std::string name;
    MemberKind kind;
    std::vector<TypeRef> params;  // Method/Operator/Init
    TypeRef type;                 // field/property type, or method/operator return type
    bool isStatic = false;        // a `static fn` member, called as `Type.method(...)`
    bool isOpen = false;          // `open`/`abstract` — overridable from a subclass (C#-convention `override`)
    bool isAsync = false;         // an `async` method — its call result is `Awaitable<ret>` (§4.7)
    bool isRecordField = false;   // a record's positional field — immutable (P40 review r3 2(i))
    bool isReadOnly = false;      // a `let` field or a `const`: never assignable, except a `let` instance
                                  // field through `this` in a constructor of its own class (C# `readonly`)
    std::size_t required = 0;    // leading params with no default — the minimum a call must supply
    std::vector<std::string> generics; // the method's own type-param names (for TypeArg inference)
    std::vector<std::string> numericParams; // subset of `generics` bounded by INumber (must infer to a number)
    std::vector<GenericParam> boundSpecs;   // the method's bounded type params (nominal bounds checked at the call)
};
struct TypeInfo {
    std::vector<GenericParam> generics;
    std::vector<MemberInfo> members;
    std::vector<std::string> bases;  // named base class/interfaces — members are inherited from these
    std::vector<TypeRef> baseRefs;   // the same bases with their type arguments (`: Base<T>`), for member
                                     // substitution through an inherited generic base (P40 slice 7)
    std::vector<TypeRef> ctorParams; // record positional fields, or class init params
    std::size_t ctorRequired = 0;    // leading ctor params with no default
    bool hasCtor = false;
    bool complete = true;            // we fully know this type's members (record/class/interface)
};
struct FnSig {
    std::vector<TypeRef> params;
    std::size_t required = 0;        // leading params with no default
    TypeRef result = namedType("unit");
    std::vector<std::string> generics; // the function's type-param names (for TypeArg inference)
    std::vector<std::string> numericParams; // subset of `generics` bounded by INumber (must infer to a number)
    std::vector<GenericParam> boundSpecs;   // the function's bounded type params (nominal bounds checked at the call)
    TypeRef receiver;                  // extensions only: the declared receiver type (for inference from it)
    bool isAsync = false;              // an `async fn` — its call result is `Awaitable<result>` (§4.7)
    bool isPrelude = false;            // declared by the core/std prelude (not referable as a value, r4 item 20)
    SourcePos pos;                    // the declaration (its file id = its module: D16 overload visibility)
    std::string mangled;               // the declaration's per-module mangled name (FunctionDecl::mangledName)
    std::string linkName;              // the declaration's link name (FunctionDecl::linkName)
    std::string uniqueName;            // the declaration's unique name (FunctionDecl::uniqueName)
    bool prelude = false;              // a std/prelude declaration (not part of the user's one-scope link)
};

// The generic-param names bounded by the INumber marker — those whose inferred type argument must be numeric.
inline std::vector<std::string> numericBounded(const std::vector<GenericParam>& gs) {
    std::vector<std::string> out;
    for (const auto& g : gs)
        for (const auto& b : g.bounds)
            if (b.kind == TypeRef::Kind::Named && b.name == "INumber") { out.push_back(g.name); break; }
    return out;
}

// How many leading parameters a caller MUST supply: the count before the first defaulted one.
// (Defaults are trailing by convention; a non-defaulted param after a default is a latent error
// we don't diagnose here.)
std::size_t requiredCount(const std::vector<Param>& ps) {
    std::size_t n = 0;
    for (const auto& p : ps) { if (p.hasDefault) break; ++n; }
    return n;
}

// Render a TypeRef for a diagnostic (author-facing, loose): `name<args>?`, tuples parenthesized,
// function types elided. An empty name reads as `unit`.
std::string sigTypeName(const TypeRef& t) {
    if (t.kind == TypeRef::Kind::Tuple) {
        std::string s = "(";
        for (std::size_t i = 0; i < t.args.size(); ++i) { if (i) s += ", "; s += sigTypeName(t.args[i]); }
        return s + ")";
    }
    if (t.kind == TypeRef::Kind::Function) return "fn";
    std::string s = t.name.empty() ? "unit" : t.name;
    if (!t.args.empty()) {
        s += "<";
        for (std::size_t i = 0; i < t.args.size(); ++i) { if (i) s += ", "; s += sigTypeName(t.args[i]); }
        s += ">";
    }
    if (t.nullable) s += "?";
    return s;
}

// Two parameter lists denote the same overload signature (a true duplicate): same arity and each position
// the same named type / same scalar category.
bool sameParamList(const std::vector<TypeRef>& a, const std::vector<TypeRef>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (sameNamedType(a[i], b[i])) continue;
        Ty sa = scalarTyOf(a[i]);
        if (sa != Ty::Unknown && sa == scalarTyOf(b[i])) continue;
        return false;
    }
    return true;
}


// A function's per-target name. C# keeps the source name (native overloading); TS needs a distinct name
// per overload, so an overloaded function mangles its parameter types in: `describe$i32`, `add$i32_i32`.
// P40 review r6 #8: a parameter type's spelling in a mangled name, STRUCTURAL so that two declarations whose
// erased signatures differ never share a name (a tuple or function parameter spelled `x`, and `OrNull` only at
// the top level, let `pairOf((i32, i32))` and `pairOf((i32, string))` both mangle to `pairOf$x` — CS0111). The
// common forms stay readable (`i32`, `Shape`, `stringOrNull`); compound forms are bracketed by `$`-markers,
// which no .pg name contains: `List$of$i32$end`, `Map$of$string$and$i32$end`, `$tuple$i32$and$string$end`,
// `$fn$i32$to$bool$end` (`$fn$$to$unit$end` takes none). It reads back unambiguously — a name runs to the next
// `$`, the markers follow `$` — so it is injective: a name's own `_` is `$u` (the `_` between parameters is then
// the only `_`), and a name that itself ends in `OrNull` closes with `$n`.
static std::string mangleTypeSpelling(const TypeRef& t) {
    auto list = [](const std::vector<TypeRef>& ts) {
        std::string s;
        for (std::size_t i = 0; i < ts.size(); ++i) s += (i ? "$and$" : "") + mangleTypeSpelling(ts[i]);
        return s;
    };
    std::string s;
    if (t.kind == TypeRef::Kind::Tuple) {
        s = "$tuple$" + list(t.args) + "$end";
    } else if (t.kind == TypeRef::Kind::Function) {
        s = "$fn$" + list(t.args) + "$to$" + (t.ret.empty() ? std::string("unit") : mangleTypeSpelling(t.ret[0])) + "$end";
    } else {
        const std::string& n = t.name.empty() ? std::string("x") : t.name;
        for (char c : n) s += c == '_' ? std::string("$u") : std::string(1, c);
        if (s.size() >= 6 && s.compare(s.size() - 6, 6, "OrNull") == 0) s += "$n";
        if (!t.args.empty()) s += "$of$" + list(t.args) + "$end";
    }
    if (t.nullable) s += "OrNull"; // review r5 #13: `f(string)` and `f(string?)` get distinct names
    return s;
}

std::string mangleFn(const std::string& name, const std::vector<TypeRef>& params, bool overloaded) {
    if (!overloaded) return name;
    std::string m = name + "$";
    if (params.empty()) return m + "unit";
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (i) m += "_";
        m += mangleTypeSpelling(params[i]);
    }
    return m;
}

// Pick the best-matching overload for the given argument types: exact/scalar match (2 pts) beats an
// implicit widen (1 pt); a non-numeric (generic/user) parameter matches leniently. nullptr = no candidate.
const FnSig* resolveOverload(const std::vector<FnSig>& set, const std::vector<TypeRef>& args) {
    if (set.size() == 1) return &set[0]; // not overloaded — arity/type errors are reported by checkArgs
    // P40 review r6 (part 2): nullability takes part. A maybe-null argument (a `T?` value, or `null`) cannot bind a
    // parameter of a concrete non-null type, and it matches a nullable parameter of its own type exactly — so
    // `show(m)` with `m: string?` picks `show(string?)`, where it used to bind `show(string)` (declared first, an
    // equal lenient score) and then be refused as a null into a non-null slot. A non-null argument still prefers
    // the non-null member (a nullable one is a widening, 1 pt).
    auto concreteNonNull = [](const FnSig& sig, const TypeRef& p) {
        if (p.nullable) return false;
        if (p.kind != TypeRef::Kind::Named) return true; // a non-null tuple or function type
        if (p.name.empty() || p.name == "Option" || p.name == "unit") return false;
        return std::find(sig.generics.begin(), sig.generics.end(), p.name) == sig.generics.end();
    };
    auto pick = [&](bool strictNull) -> const FnSig* {
        const FnSig* best = nullptr;
        int bestScore = -1;
        for (const auto& sig : set) {
            if (sig.params.size() != args.size()) continue;
            int score = 0;
            bool ok = true;
            for (std::size_t i = 0; i < args.size(); ++i) {
                const TypeRef& p = sig.params[i];
                const TypeRef& a = args[i];
                Ty sa = scalarTyOf(a);
                const bool nullLit = a.kind == TypeRef::Kind::Named && a.name.empty() && a.nullable;
                if (strictNull && a.nullable && concreteNonNull(sig, p)) { ok = false; break; }
                TypeRef ab = a, pb = p;
                ab.nullable = pb.nullable = false;
                if (strictNull && p.nullable && (nullLit || (a.nullable && sameNamedType(ab, pb)))) score += 2;
                else if (sameNamedType(a, p) || (sa != Ty::Unknown && sa == scalarTyOf(p))) score += 2;
                else if (isLosslessWiden(a, p)) score += 1;
                else if (strictNull && p.nullable && !a.nullable && sameNamedType(ab, pb)) score += 1;
                else if (!isNumericTypeName(p)) score += 0; // generic/user/unknown parameter: lenient
                else { ok = false; break; }
            }
            if (ok && score > bestScore) { best = &sig; bestScore = score; }
        }
        return best;
    };
    if (const FnSig* s = pick(true)) return s;
    return pick(false); // none can take a maybe-null argument: bind as before, so checkArgs names the conversion
}
struct Local {
    TypeRef type;
    bool isMutable;
    int defId = -1;  // index into the SemanticModel's defs when indexing is on (§4.8); -1 otherwise
    // P40 D14 null flow-narrowing: `nonNull` = proven non-null at the current point (a nullable local or
    // param after `if x != null`, an early exit, `x != null && …`). `assigns` counts assignments, so a
    // region's exit can tell whether the variable was reassigned inside it; `lambdaDepth` is where it was
    // declared — a MUTABLE local's narrowing does not reach into a lambda (it may run after a reassignment).
    bool nonNull = false;
    int assigns = 0;
    int lambdaDepth = 0;
    // P40 review r5 item 19: the lambda depth at which the CURRENT narrowing was established — a narrowing
    // reaches exactly the lambda body that tested it (an outer test never reaches into a lambda, but a test
    // inside the lambda narrows a captured param or `let` for the rest of that body).
    int narrowDepth = 0;
    // P40 review r3 (part 2): a local initialized with a constant and never read is C# CS0219 (a warning a
    // warnings-as-errors consumer cannot build) — refused when its scope closes.
    int reads = 0;
    bool constInit = false;
    // P40 review r5 #17: assigned a non-constant somewhere — C# CS0219 fires only when EVERY assignment is a
    // constant, so such a local is never reported (`var _last = 0; _last = compute()` compiles clean).
    bool nonConstAssign = false;
    SourcePos declPos{0, 0, 0};
    Stmt* declStmt = nullptr; // the `let` that declared it (r4: an unread `_x = <const>` is dropped there)
};

class Checker {
public:
    Checker(DiagnosticBag& diags) : diags_(diags) {}
    Checker(DiagnosticBag& diags, SemanticModel* model, const SemanticRequest* req,
            const ModuleVisibility* modules = nullptr)
        : diags_(diags), model_(model), req_(req), modules_(modules) {}

    void run(CompilationUnit& unit) {
        unit_ = &unit;
        collectTypeNames(unit);
        liftExtensionGenerics(unit);
        normalizeOptionalGenerics(unit); // `T?` (generic T) -> Option<T>, before tables read the types
        buildTables(unit);
        resolveAllTypes(unit);
        checkImplements(unit); // issue #29: implements-conformance, override rule, interface-body shape
        checkAttributesPass(unit); // P37 D: attribute declarations + every [Name(args)] attachment
        checkAbstractClasses(unit); // review r5 #9 (before any body: construction reads abstractClassNames_)

        // Pre-register file-local definitions (functions, types + members, values) so a reference from any
        // user body resolves to one. The linker APPENDS merged std/import decls, so indices [0, userX) are
        // the entry file's own. (First decl of a name wins; overload precision is a follow-up.)
        if (model_ && req_) registerUserSymbols(unit);

        // Type-check top-level const/let initializers first, so their types propagate. Review r5 (part 2): BEFORE
        // the bodies, and an un-annotated value takes its initializer's type — every body read it as unknown, so
        // `d.hello() + " "` on `let d = Derived()` was an untyped `+` (PHP added the strings: a TypeError).
        for (auto& v : unit.values) {
            if (!v.init) continue;
            initDeclPos_ = v.pos; initDeclName_ = v.name;
            TypeRef it = checkExpr(*v.init);
            initDeclPos_ = {0, 0, 0};
            if (v.hasType) checkConvert(v.init, v.type, "initializer of '" + v.name + "'");
            else if (uninferableInit(*v.init, it)) // module-level binding with an un-inferable initializer (issue #27)
                diags_.error(v.pos, "cannot infer the type of '" + v.name + "' from its initializer; " + annotationHint(v.name, *v.init, it));
            else if (concreteType(it)) values_[v.name] = it;
        }

        for (std::size_t i = 0; i < unit.functions.size(); ++i) {
            auto& fn = unit.functions[i];
            recordModel_ = model_ && req_ && i < req_->userFunctions; // index the entry file's own bodies only
            currentReturn_ = fn.returnType;
            currentThis_ = tUnknown();
            inActual_ = !fn.actualTarget.empty(); // only an `actual` body is a target-gated region (§4.4)
            inAsync_ = fn.isAsync;
            scopeStart_ = fn.namePos; scopeEnd_ = fn.bodyEnd; // locals here are in scope only within this fn
            pushGenerics(fn.generics);
            checkCallSiteDefaults(fn.params, "function '" + fn.name + "'"); // P40 review r3 2(j)
            pushScope();
            for (const auto& p : fn.params) declare(p.name, p.type, false, p.pos);
            bodyKey_ = fnBodyKey(fn.pos); // review r4 (part 2): record what this body reads for checkThroughCalls
            checkBlock(fn.body);
            bodyKey_.clear();
            popScope();
            popGenerics(fn.generics);
            scopeStart_ = {}; scopeEnd_ = {};
            inActual_ = false;
            inAsync_ = false;
            recordModel_ = false;
        }
        // P40 review r4 #3: a constructor always runs its base class's constructor — `super()` when none is written.
        // When the base constructor needs arguments, an omitted call (or a missing constructor) is C# CS7036 and a
        // TS error, so the subclass must write `super(…)`.
        for (const auto& c : unit.classes) {
            if (c.isExtern) continue;
            for (const auto& b : c.bases) {
                if (b.kind != TypeRef::Kind::Named || interfaceNames_.count(b.name) || externClassNames_.count(b.name)) continue;
                auto bt = types_.find(b.name);
                if (bt == types_.end() || !bt->second.hasCtor || bt->second.ctorRequired == 0) continue;
                bool anyCtor = false;
                for (const auto& m : c.members) {
                    if (m.kind != MemberKind::Constructor) continue;
                    anyCtor = true;
                    bool calls = false;
                    for (const auto& st : m.body)
                        if (st->kind == StmtKind::ExprStmt && st->value && st->value->kind == ExprKind::Call &&
                            st->value->lhs && st->value->lhs->kind == ExprKind::Super)
                            calls = true;
                    if (!calls)
                        diags_.error(m.pos, "the constructor of class '" + c.name + "' must call `super(…)`: the "
                                            "constructor of base class '" + b.name + "' takes arguments");
                }
                if (!anyCtor)
                    diags_.error(c.namePos.line ? c.namePos : c.pos,
                                 "class '" + c.name + "' needs a constructor that calls `super(…)`: the constructor of "
                                 "base class '" + b.name + "' takes arguments");
                break;
            }
        }
        // P40 review r4 #8: a record's body holds methods, operators and computed properties only. Every emitter
        // dropped a body field (C# CS1061 / TS2339 / Python AttributeError / PHP null), static or not. Its state
        // is its positional fields — they alone take part in equality, hashing and `with` on every target.
        for (const auto& r : unit.records)
            for (const auto& m : r.members) {
                if (m.kind != MemberKind::Field && m.kind != MemberKind::Const) continue;
                bool isStatic = m.kind == MemberKind::Const;
                for (const auto& mod : m.modifiers) if (mod == "static") isStatic = true;
                diags_.error(m.namePos.line ? m.namePos : m.pos,
                             "record '" + r.name + "' declares " + (isStatic ? "a static field" : "a body field") + " '" +
                                 m.name + "' — a record's state is its positional fields (they alone take part in "
                                 "equality, hashing and `with`); " +
                                 (isStatic ? "declare a module-level `let`/`const`, or a `static fn`"
                                           : "make it a positional field (with a default), or a computed property"));
            }
        for (std::size_t i = 0; i < unit.records.size(); ++i) {
            recordModel_ = model_ && req_ && i < req_->userRecords; // record refs inside user type bodies too
            checkTypeBody(unit.records[i].name, unit.records[i].generics, unit.records[i].members, &unit.records[i].fields);
            recordModel_ = false;
        }
        for (std::size_t i = 0; i < unit.classes.size(); ++i) {
            recordModel_ = model_ && req_ && i < req_->userClasses;
            checkTypeBody(unit.classes[i].name, unit.classes[i].generics, unit.classes[i].members, nullptr);
            recordModel_ = false;
        }
        for (auto& d : unit.extensions) { // `this` denotes the receiver inside an extension body
            currentReturn_ = d.returnType;
            currentThis_ = d.receiver;
            pushGenerics(d.generics);
            pushScope();
            for (const auto& p : d.params) declare(p.name, p.type, false, p.pos);
            checkCallSiteDefaults(d.params, "extension '" + d.receiver.name + "." + d.name + "'");
            bodyKey_ = "ext:" + d.receiver.name + "." + d.name; // review r5 #4: followed from an initializer's call
            if (d.exprBody) { checkExpr(*d.exprBody); checkConvert(d.exprBody, d.returnType, "extension body"); }
            else checkBlock(d.body);
            bodyKey_.clear();
            popScope();
            popGenerics(d.generics);
            currentThis_ = tUnknown();
        }
        assignLinkNames(unit);
        checkThroughCalls(unit); // review r4 (part 2): what an initializer reads through the calls it makes
        // P40 review (A9/B11): reachability, missing returns, field definite assignment, self-comparison.
        checkFlow(unit, diags_);
    }

private:
    // P40 review r4 #12/#13: every resolved free-function call, for the link-name decision below.
    struct CallSite {
        Expr* call;
        std::string name;
        std::vector<TypeRef> args;
        SourcePos chosen; // the declaration sema bound (its FnSig::pos)
    };
    std::vector<CallSite> callSites_;
    // P40 review r5 #14: every top-level function used as a value, with the declaration it names — its link name
    // is decided with the calls' (a value kept the provisional one: C# CS0117, a PHP undefined function).
    struct FnRefSite { Expr* ref; SourcePos decl; };
    std::vector<FnRefSite> fnRefSites_;

    // Could a target with native overloading (C#) consider `sig` applicable to these argument types? Answers
    // "no" only when that is certain — an arity outside [required, all], or an argument that can never
    // convert to its parameter (a string for a number, a bool for a string, unrelated user types, …). A type
    // parameter accepts anything, so a generic overload is applicable wherever its arity fits.
    bool mayApply(const FnSig& sig, const std::vector<TypeRef>& args) const {
        if (args.size() < sig.required || args.size() > sig.params.size()) return false;
        auto category = [&](const TypeRef& t) -> int { // 0 unknown/open, 1 numeric, 2 string, 3 bool, 4 char, 5 user
            if (t.kind != TypeRef::Kind::Named || t.name.empty() || !t.args.empty()) return 0;
            if (std::find(sig.generics.begin(), sig.generics.end(), t.name) != sig.generics.end()) return 0;
            TypeRef base = t;
            base.nullable = false; // `i32?` takes an i32 (and null)
            if (isNumericTypeName(base)) return 1;
            if (t.name == "string") return 2;
            if (t.name == "bool") return 3;
            if (t.name == "char") return 4;
            if (unionOrEnum(t.name)) return 6; // review r5 #15: a union/enum takes only itself (it was uncategorized)
            if (types_.count(t.name) && !interfaceNames_.count(t.name)) return 5;
            return 0;
        };
        for (std::size_t i = 0; i < args.size(); ++i) {
            const TypeRef& p = sig.params[i];
            const TypeRef& a = args[i];
            const int cp = category(p), ca = category(a);
            if (cp == 0 || ca == 0) continue;
            if (cp != ca) return false;
            if (cp == 6 && a.name != p.name) return false;
            if (cp == 5 && a.name != p.name && !inheritsFrom(a.name, p.name)) return false;
        }
        return true;
    }

    // C# links every module's free functions into one class and resolves overloads natively. That agrees with
    // sema only when sema's choice is the ONLY applicable same-named declaration at every call: C# prefers an
    // exactly inferred generic over a conversion (`show<T>(Disk)` beats `show(Shape)`), and sees overloads of
    // modules the caller never imported. A name with such a call gets signature link names on all of its
    // declarations (`show__Shape`, `show__T`); every other name keeps its plain name — a library's C# API no
    // longer changes because an unrelated module declares the same name with a different signature.
    void assignLinkNames(CompilationUnit& u) {
        std::set<std::string> clash;
        for (const auto& cs : callSites_) {
            auto f = fns_.find(cs.name);
            if (f == fns_.end() || clash.count(cs.name)) continue;
            int applicable = 0;
            for (const auto& sig : f->second)
                if (!sig.prelude && mayApply(sig, cs.args)) ++applicable;
            if (applicable > 1) clash.insert(cs.name);
        }
        // P40 review r5 #13: declarations alone can clash too — two that C# sees as ONE signature (CS0111) whether or
        // not anything calls them: type parameters differing only in name (`same<T>(T)` / `same<U>(U)`) and reference
        // types differing only in nullability (`describe(string)` / `describe(string?)`). A lone declaration, or two
        // with different C# signatures, keeps its plain name (round 4's e4/e4m).
        {
            std::unordered_map<std::string, std::set<std::string>> sigsByName;
            for (const auto& fn : u.functions) {
                if (fn.originModule == "<prelude>" || !fn.actualTarget.empty() || fn.isExpect) continue;
                std::vector<std::string> gen;
                for (const auto& g : fn.generics) gen.push_back(g.name);
                std::string key = std::to_string(gen.size()) + "(";
                for (const auto& p : fn.params) key += erasedCsType(p.type, gen) + ",";
                if (!sigsByName[fn.name].insert(key).second) clash.insert(fn.name);
            }
        }
        std::unordered_map<std::string, std::string> byPos; // declaration position -> its link name
        auto key = [](const SourcePos& p) { return std::to_string(p.fileId) + ":" + std::to_string(p.line) + ":" + std::to_string(p.col); };
        for (auto& fn : u.functions) {
            std::vector<TypeRef> ps; for (const auto& p : fn.params) ps.push_back(p.type);
            fn.linkName = clash.count(fn.name) ? mangleFn(fn.name, ps, true) : fn.name;
            byPos[key(fn.pos)] = fn.linkName;
        }
        for (auto& [name, set] : fns_)
            for (auto& sig : set)
                if (auto it = byPos.find(key(sig.pos)); it != byPos.end())
                    sig.linkName = it->second;
        for (auto& cs : callSites_)
            if (auto it = byPos.find(key(cs.chosen)); it != byPos.end())
                cs.call->linkName = it->second;
        for (auto& fr : fnRefSites_) // review r5 #14: a function used as a value names the same declaration
            if (auto it = byPos.find(key(fr.decl)); it != byPos.end())
                fr.ref->linkName = it->second;
    }
    // P40 review r5 #9: every target now declares real abstract members (C# / TS / PHP `abstract`, Python
    // `@abc.abstractmethod` on an `abc.ABC` class), so the shapes they reject are refused here: an abstract method
    // with a body or outside an abstract class (CS0513 / TS1244), `abstract` on anything but a method, a concrete
    // class leaving an inherited abstract method unimplemented (CS0534 / TS2515), and constructing an abstract
    // class (CS0144 / TS2511 / a Python TypeError / a PHP Error).
    std::unordered_set<std::string> abstractClassNames_;
    void checkAbstractClasses(const CompilationUnit& u) {
        std::unordered_map<std::string, const ClassDecl*> byName;
        for (const auto& c : u.classes) if (!c.isExtern) byName[c.name] = &c;
        auto has = [](const std::vector<std::string>& mods, const char* m) {
            return std::find(mods.begin(), mods.end(), m) != mods.end();
        };
        for (const auto& c : u.classes) {
            if (c.isExtern) continue;
            const bool abstractCls = has(c.modifiers, "abstract");
            if (abstractCls) abstractClassNames_.insert(c.name);
            for (const auto& m : c.members) {
                if (!has(m.modifiers, "abstract")) continue;
                const SourcePos at = m.namePos.line ? m.namePos : m.pos;
                if (m.kind != MemberKind::Method)
                    diags_.error(at, "only a method can be 'abstract' ('" + m.name + "' in class '" + c.name + "')");
                else if (m.hasBody)
                    diags_.error(at, "abstract method '" + c.name + "." + m.name + "' has a body — an abstract method "
                                     "declares a signature only; drop the body, or make it `open`");
                else if (!abstractCls)
                    diags_.error(at, "abstract method '" + m.name + "' in class '" + c.name + "', which is not abstract "
                                     "— declare `abstract class " + c.name + "` (C# CS0513)");
            }
            if (abstractCls) continue;
            std::set<std::string> implemented, seen;
            for (const ClassDecl* k = &c; k && seen.insert(k->name).second;) {
                for (const auto& m : k->members) {
                    if (m.kind != MemberKind::Method || has(m.modifiers, "static")) continue;
                    if (!has(m.modifiers, "abstract")) implemented.insert(m.name);
                    else if (k != &c && !implemented.count(m.name)) { // its own: reported above
                        diags_.error(c.namePos.line ? c.namePos : c.pos,
                                     "class '" + c.name + "' does not implement the abstract method '" + k->name + "." +
                                         m.name + "' — override it, or declare `abstract class " + c.name + "`");
                        implemented.insert(m.name); // once per method
                    }
                }
                const ClassDecl* next = nullptr;
                for (const auto& b : k->bases)
                    if (auto it = byName.find(b.name); it != byName.end()) { next = it->second; break; }
                k = next;
            }
        }
    }
    // A parameter type as a C# signature sees it: a method type parameter by position, a reference type's
    // nullability erased (a value type's `?` is Nullable<T>, a distinct type).
    std::string erasedCsType(const TypeRef& t, const std::vector<std::string>& gen) const {
        if (t.kind == TypeRef::Kind::Function) {
            std::string s = "fn(";
            for (const auto& a : t.args) s += erasedCsType(a, gen) + ",";
            s += ")";
            for (const auto& r : t.ret) s += erasedCsType(r, gen);
            return s;
        }
        if (t.kind != TypeRef::Kind::Named) {
            std::string s = "k" + std::to_string(static_cast<int>(t.kind)) + "(";
            for (const auto& a : t.args) s += erasedCsType(a, gen) + ",";
            return s + ")";
        }
        for (std::size_t i = 0; i < gen.size(); ++i)
            if (gen[i] == t.name) return "!!" + std::to_string(i);
        TypeRef base = t;
        base.nullable = false;
        const bool valueType = isNumericTypeName(base) || t.name == "bool" || t.name == "char" || enumNames_.count(t.name);
        std::string s = t.name;
        if (!t.args.empty()) {
            s += "<";
            for (const auto& a : t.args) s += erasedCsType(a, gen) + ",";
            s += ">";
        }
        return s + (t.nullable && valueType ? "?" : "");
    }
    DiagnosticBag& diags_;
    std::unordered_set<const Expr*> allowedSupers_; // D6: the `super` callees of hoisted base-ctor calls
    std::unordered_map<std::string, std::vector<FnSig>> fns_; // name -> overload set
    std::unordered_map<std::string, TypeInfo> types_;
    std::unordered_set<std::string> typeNames_;
    std::unordered_set<std::string> externClassNames_;                     // `extern class` names (std/FFI carriers)
    std::unordered_set<std::string> interfaceNames_;                       // declared `interface` names
    std::unordered_map<std::string, const InterfaceDecl*> interfaceDecls_; // for base type-args (conformance)
    std::unordered_set<std::string> enumNames_;
    std::unordered_set<std::string> recordNames_;                          // declared `record` names (P40 B4 key refusal)
    std::unordered_map<std::string, TypeRef> values_;          // top-level const/let
    // P40 review r3 2(h): where each module value / static field is declared, and the declaration whose
    // initializer is being checked (line 0 = none). TS/Python/PHP run initializers in declaration order, so a
    // direct read of one declared LATER in the same file is refused (C# alone initializes statics lazily).
    std::unordered_map<std::string, SourcePos> valueDeclPos_;  // value name -> decl pos
    std::unordered_map<std::string, SourcePos> staticDeclPos_; // "Type.field" -> decl pos
    std::unordered_map<std::string, SourcePos> typeDeclPos_;   // user type / union case name -> decl pos
    SourcePos initDeclPos_{0, 0, 0};
    std::string initDeclName_;
    // P40 review r5 #4: the class whose STATIC initializer is being checked ("" = a module value's initializer).
    // A module value may use a class declared after it: the emission order places that class (with its bases and
    // what its own statics reach) ahead of the value. A static initializer keeps source order.
    std::string initClass_;
    std::unordered_set<std::string> userClassNames_; // non-extern user classes (the ones the emission order may move)
    static bool posBefore(const SourcePos& a, const SourcePos& b) {
        return a.line < b.line || (a.line == b.line && a.col < b.col);
    }
    // `via` non-empty: `shown` is the base class of the used class `via` (a class is defined after its bases).
    // `cls` non-empty: the use needs class `cls` to exist (a construction, a static read or call, a base).
    void checkInitOrder(const SourcePos& declPos, const std::string& shown, SourcePos usePos, const std::string& via = "",
                        const std::string& cls = "") {
        if (declPos.line == 0) return;
        if (initDeclPos_.line == 0) {
            // P40 review r4 (part 2): not in an initializer — inside a body (r5: any body an initializer can run —
            // a function, a static or instance method, a constructor, an extension), the use is recorded, so an
            // initializer that CALLS this body is checked against it (checkThroughCalls).
            if (!bodyKey_.empty() && lambdaDepth_ <= initLambdaFloor_) bodyReads_[bodyKey_].push_back({declPos, shown, via, cls});
            return;
        }
        if (lambdaDepth_ > initLambdaFloor_) return;
        // r5 #4: defining a class runs its static initializers, so they are what its definition reads.
        if (!initClass_.empty()) bodyReads_["cls:" + initClass_].push_back({declPos, shown, via, cls});
        if (declPos.fileId != initDeclPos_.fileId) return;
        // r5 #4: a module value may use a class declared later — the emission order places the class (and its
        // bases) first, and checkThroughCalls checks what defining it reads.
        if (initClass_.empty() && userClassNames_.count(cls)) {
            if (via.empty()) initCalls_.push_back({initDeclPos_, initDeclName_, usePos, "cls:" + cls, cls, "", true});
            return;
        }
        if (posBefore(declPos, initDeclPos_)) return;
        diags_.error(usePos, initOrderMessage(initDeclName_, shown, via, ""));
    }
    static std::string initOrderMessage(const std::string& init, const std::string& shown, const std::string& via,
                                        const std::string& callee, bool uses = false) {
        const std::string what = !via.empty() ? "uses '" + via + "', whose base class '" + shown + "' is declared later"
                                 : callee.empty() ? "reads '" + shown + "', which is declared later"
                                                  : "reads '" + shown + "', declared later";
        return "the initializer of '" + init + "' " +
               (callee.empty() ? what : (uses ? "uses '" : "calls '") + callee + "', which " + what) +
               " — TS, Python and PHP run initializers in declaration order (it would read an uninitialized value "
               "or fail; C# reads a default); declare '" + shown + "' first";
    }
    // A type used (constructed, a static/enum case read, a static call) in an initializer: a class declared later
    // does not exist yet on TS (ReferenceError) or Python (NameError) when the initializer runs — nor does a
    // class whose base class is declared later (review r4 #7: a class is defined only after its bases).
    void noteTypeUse(const std::string& name, SourcePos usePos) {
        if (auto it = typeDeclPos_.find(name); it != typeDeclPos_.end()) checkInitOrder(it->second, name, usePos, "", name);
        std::unordered_set<std::string> seen{name};
        std::vector<std::string> work{name};
        while (!work.empty()) {
            const std::string n = work.back();
            work.pop_back();
            auto ti = types_.find(n);
            if (ti == types_.end()) continue;
            for (const auto& b : ti->second.bases) {
                if (interfaceNames_.count(b) || !seen.insert(b).second) continue;
                if (auto bp = typeDeclPos_.find(b); bp != typeDeclPos_.end()) checkInitOrder(bp->second, b, usePos, name, b);
                work.push_back(b);
            }
        }
    }
    // P40 review r4 (part 2) — the through-call gap: an initializer that calls a same-module function or static
    // method (or invokes a lambda in place) runs that body right then, so the body's reads count as the
    // initializer's. Bodies are keyed `fn:<file>:<line>:<col>` (a free function's declaration) or
    // `st:<Owner>.<method>`; lambdas inside them are deferred, except one invoked in place.
    // P40 review r5 #4: every statically known body an initializer can run is followed — also `ctor:<Class>` (the
    // constructor plus the instance field initializers), `m:<Class>.<name>` (an instance method or computed
    // property, and every override of it), `ext:<Recv>.<name>` (an extension), a function passed as an argument,
    // and `cls:<Class>` (defining the class: its static initializers). A class an initializer reaches is placed
    // ahead of the value by the emission order (ValueDecl/ClassDecl::needsClasses); only a read of a module value
    // declared at or after the initializer — a real cycle — is refused.
    std::string bodyKey_;
    int initLambdaFloor_ = 0; // lambda depth of an in-place-invoked lambda being checked (its body is not deferred)
    struct BodyRead { SourcePos decl; std::string shown, via, cls; };
    std::unordered_map<std::string, std::vector<BodyRead>> bodyReads_;
    std::unordered_map<std::string, std::set<std::string>> bodyCalls_;
    struct InitCall {
        SourcePos initPos; std::string initName; SourcePos callPos; std::string callee, calleeShown;
        std::string initClass; // the class of a static initializer; "" for a module value
        bool use = false;      // the initializer USES class `calleeShown` (no call): worded "uses"
    };
    std::vector<InitCall> initCalls_;
    std::unordered_map<std::string, std::set<std::string>> valueNeeds_; // module value -> classes it reaches
    static std::string fnBodyKey(const SourcePos& p) {
        return "fn:" + std::to_string(p.fileId) + ":" + std::to_string(p.line) + ":" + std::to_string(p.col);
    }
    void noteDirectCall(const std::string& key, const std::string& shown, SourcePos callPos) {
        if (lambdaDepth_ > initLambdaFloor_) return; // inside a lambda: runs whenever it is called
        if (initDeclPos_.line != 0) {
            initCalls_.push_back({initDeclPos_, initDeclName_, callPos, key, shown, initClass_});
            if (!initClass_.empty()) bodyCalls_["cls:" + initClass_].insert(key); // r5 #4: defining the class runs it
        } else if (!bodyKey_.empty()) bodyCalls_[bodyKey_].insert(key);
    }
    // The class whose body a key names (`st:`/`m:`/`ctor:`/`cls:`); "" for a free function or an extension.
    static std::string bodyOwner(const std::string& k) {
        for (const char* p : {"st:", "m:"})
            if (k.rfind(p, 0) == 0) { std::string s = k.substr(std::strlen(p)); return s.substr(0, s.find('.')); }
        for (const char* p : {"ctor:", "cls:"})
            if (k.rfind(p, 0) == 0) return k.substr(std::strlen(p));
        return "";
    }
    // Every body a key may run next: its recorded calls, and for an instance member every override of it.
    void forEachCallee(const std::string& k, const std::function<void(const std::string&)>& f) const {
        if (auto c = bodyCalls_.find(k); c != bodyCalls_.end())
            for (const auto& n : c->second) f(n);
        if (k.rfind("m:", 0) == 0) {
            const std::string owner = bodyOwner(k), name = k.substr(2 + owner.size() + 1);
            for (const auto& [t, info] : types_) {
                if (t == owner || !inheritsFrom(t, owner)) continue;
                for (const auto& m : info.members)
                    if (m.name == name && !m.isStatic) { f("m:" + t + "." + name); break; }
            }
        }
    }
    void checkThroughCalls(CompilationUnit& u) {
        // A constructor runs its base class's constructor; defining a class needs its base class defined.
        for (const auto& c : u.classes) {
            if (c.isExtern) continue;
            for (const auto& b : c.bases)
                if (userClassNames_.count(b.name)) {
                    bodyCalls_["ctor:" + c.name].insert("ctor:" + b.name);
                    bodyCalls_["cls:" + c.name].insert("cls:" + b.name);
                }
        }
        for (const auto& ic : initCalls_) {
            const bool isValue = ic.initClass.empty();
            std::set<std::string> seen{ic.callee};
            std::vector<std::string> work{ic.callee};
            bool reported = false;
            while (!work.empty() && !reported) {
                const std::string k = work.back();
                work.pop_back();
                if (isValue && k.rfind("cls:", 0) == 0) { // a class the value needs: placed ahead of it
                    const std::string cn = k.substr(4);
                    if (auto tp = typeDeclPos_.find(cn); tp != typeDeclPos_.end() && tp->second.fileId == ic.initPos.fileId)
                        valueNeeds_[ic.initName].insert(cn);
                }
                if (auto r = bodyReads_.find(k); r != bodyReads_.end())
                    for (const auto& rd : r->second) {
                        if (isValue && userClassNames_.count(rd.cls)) { // ordered ahead of the value, not refused
                            if (seen.insert("cls:" + rd.cls).second) work.push_back("cls:" + rd.cls);
                            continue;
                        }
                        // r5 #12: code OUTSIDE a class cannot see the class while its static initializers run (TS
                        // binds the class name only after the class is defined: a ReferenceError).
                        if (!isValue && rd.cls == ic.initClass && bodyOwner(k) != ic.initClass) {
                            diags_.error(ic.callPos, "the initializer of '" + ic.initName + "' calls '" + ic.calleeShown +
                                                         "', which reads '" + rd.shown + "' while class '" + ic.initClass +
                                                         "' is still being defined — TS binds a class's name only after "
                                                         "its static initializers ran (a ReferenceError); read it directly "
                                                         "in the initializer, or make the helper a `static fn` of '" +
                                                         ic.initClass + "'");
                            reported = true;
                            break;
                        }
                        if (rd.decl.fileId == ic.initPos.fileId && !posBefore(rd.decl, ic.initPos)) {
                            diags_.error(ic.callPos, initOrderMessage(ic.initName, rd.shown, rd.via, ic.calleeShown, ic.use));
                            reported = true;
                            break;
                        }
                    }
                if (reported) break;
                forEachCallee(k, [&](const std::string& n) { if (seen.insert(n).second) work.push_back(n); });
            }
        }
        // What each module value and each class definition needs placed before it (ir::declarationOrder).
        for (auto& v : u.values)
            if (auto it = valueNeeds_.find(v.name); it != valueNeeds_.end())
                v.needsClasses.assign(it->second.begin(), it->second.end());
        for (auto& c : u.classes) {
            if (c.isExtern) continue;
            std::set<std::string> needs, seen{"cls:" + c.name};
            std::vector<std::string> work{"cls:" + c.name};
            while (!work.empty()) {
                const std::string k = work.back();
                work.pop_back();
                if (auto r = bodyReads_.find(k); r != bodyReads_.end())
                    for (const auto& rd : r->second)
                        if (userClassNames_.count(rd.cls) && seen.insert("cls:" + rd.cls).second) work.push_back("cls:" + rd.cls);
                forEachCallee(k, [&](const std::string& n) { if (seen.insert(n).second) work.push_back(n); });
                if (k.rfind("cls:", 0) == 0 && k != "cls:" + c.name) {
                    const std::string cn = k.substr(4);
                    if (auto tp = typeDeclPos_.find(cn); tp != typeDeclPos_.end() && tp->second.fileId == c.pos.fileId)
                        needs.insert(cn);
                }
            }
            c.needsClasses.assign(needs.begin(), needs.end());
        }
    }
    std::unordered_map<std::string, FnSig> unionCtors_;        // union case name -> ctor sig
    std::unordered_map<std::string, std::string> unionCaseOwner_; // case name -> union type
    std::unordered_map<std::string, std::vector<std::string>> unionAllCases_; // union -> all case names
    std::unordered_map<std::string, std::vector<std::string>> enumAllCases_;  // enum  -> all case names
    std::unordered_map<std::string, FnSig> extensions_;        // "RecvType.method" -> extension sig
    std::unordered_set<std::string> genericsInScope_;
    std::unordered_set<std::string> declaredTypeParams_; // every type-param name any declaration introduces
    std::deque<std::unordered_map<std::string, Local>> scopes_; // deque: a Local* (narrowing saves) survives a push
    TypeRef currentReturn_ = namedType("unit");
    TypeRef currentThis_ = tUnknown();
    std::string currentClass_;  // enclosing type name while checking a member body; survives into static
                                // methods (where currentThis_ is Unknown) so its statics/consts resolve unqualified
    bool inActual_ = false; // checking a target-gated `actual` body — the only place `extern` is allowed
    bool inAsync_ = false;  // checking an `async fn`/method body — the only place `await` is allowed (§4.7)
    bool inCtor_ = false;   // checking a class constructor body — the only place a `let` instance field is written
    bool isBindingAllowed_ = false; // the next `is` checked sits on an if-condition's `&&` spine (P37 B3)
    CompilationUnit* unit_ = nullptr; // for P37 D: Meta materialization is recorded onto the unit
    std::unordered_map<std::string, const ExternAttrDecl*> externAttrs_; // Tier 1 declarations by name
    std::unordered_set<std::string> attributeNames_;                    // Tier 2 `attribute` record names
    // Per-enclosing-block names bound by `if … is T name` (P37 B3): the emitted binding outlives the
    // branch on every target (it's hoisted before the `if`), so redeclaring the name later in the same
    // block would collide in the OUTPUT even though the language scope ended — refused here instead.
    std::vector<std::unordered_set<std::string>> blockIsBindings_;
    // P40 D14: narrowing state. One saved-state frame per statement list (an early-exit narrowing lasts to
    // the end of its list); `closureAssigned_` = names a lambda assigns somewhere in the current top-level
    // body (a mutable local a closure can reassign is never narrowed — the closure may run at any point).
    struct NarrowSave { Local* local; bool prev; int assignsAtEntry; int prevDepth = 0; };
    std::vector<std::vector<NarrowSave>> narrowFrames_;
    std::unordered_set<std::string> closureAssigned_;
    int lambdaDepth_ = 0;
    int bodyDepth_ = 0;

    // ---- semantic model (LSP; §4.8). Populated only when model_ != nullptr, and only for file-local symbols.
    SemanticModel* model_ = nullptr;
    const SemanticRequest* req_ = nullptr;
    const ModuleVisibility* modules_ = nullptr; // P40 review A8: D16 scoping for overload resolution
    bool recordModel_ = false;                       // true only while checking a file-local (user) decl body
    SourcePos scopeStart_, scopeEnd_;                // enclosing fn/method extent; stamped on Local/Parameter defs (§4.8)
    std::unordered_map<std::string, int> fnDefId_;     // user function name -> its SymbolDef index (for call refs)
    std::unordered_map<std::string, int> typeDefId_;   // user type name -> SymbolDef index (construction refs)
    std::unordered_map<std::string, int> valueDefId_;  // user top-level const/let name -> SymbolDef index
    std::unordered_map<std::string, int> memberDefId_; // "Type.member" -> SymbolDef index (own members)

    int recordDef(SymbolKind kind, const std::string& name, SourcePos namePos, const TypeRef& type,
                  bool external = false, const std::string& owner = "") {
        if (!model_) return -1;
        SymbolDef d;
        d.kind = kind;
        d.name = name;
        d.nameSpan = {namePos, static_cast<int>(name.size())};
        d.type = type;
        d.external = external;
        d.owner = owner;
        if (kind == SymbolKind::Local || kind == SymbolKind::Parameter) { d.scopeStart = scopeStart_; d.scopeEnd = scopeEnd_; }
        model_->defs.push_back(std::move(d));
        return static_cast<int>(model_->defs.size()) - 1;
    }
    void recordRef(SourcePos pos, const std::string& name, int defId) {
        if (!model_ || !recordModel_ || defId < 0) return;
        model_->refs.push_back({{pos, static_cast<int>(name.size())}, defId});
    }

    // Register a file-local nominal type + its members as definitions (so a construction / member access
    // can resolve to them). Members are keyed "Type.member". Definition positions are the decl keyword for
    // now (jumps to the right line); precise name columns are a follow-up like FunctionDecl::namePos.
    void registerType(const std::string& name, SourcePos namePos, const std::vector<Member>& members, bool external) {
        typeDefId_.emplace(name, recordDef(SymbolKind::Type, name, namePos, tNamed(name), external));
        for (const auto& m : members) {
            SymbolKind k = (m.kind == MemberKind::Method || m.kind == MemberKind::Operator) ? SymbolKind::Method
                         : (m.kind == MemberKind::Property) ? SymbolKind::Method
                         : SymbolKind::Field;
            const TypeRef& t = (m.kind == MemberKind::Method || m.kind == MemberKind::Operator) ? m.returnType : m.type;
            memberDefId_.emplace(name + "." + m.name, recordDef(k, m.name, m.namePos, t, external, name));
        }
    }
    // Register EVERY merged declaration so references resolve — the entry file's own (indices [0, userX),
    // flagged file-local) and the appended std/import decls (flagged external, carrying their module's
    // fileId for cross-module go-to-definition). Entry decls are registered first, so they win in the
    // name->def maps (a user symbol shadows an external of the same name).
    void registerUserSymbols(const CompilationUnit& unit) {
        for (std::size_t i = 0; i < unit.functions.size(); ++i) {
            const auto& fn = unit.functions[i];
            fnDefId_.emplace(fn.name, recordDef(SymbolKind::Function, fn.name, fn.namePos, fn.returnType, i >= req_->userFunctions));
        }
        for (std::size_t i = 0; i < unit.records.size(); ++i) {
            const auto& r = unit.records[i];
            bool ext = i >= req_->userRecords;
            registerType(r.name, r.namePos, r.members, ext);
            for (const auto& f : r.fields) // positional record fields (Param.pos is already the name)
                memberDefId_.emplace(r.name + "." + f.name, recordDef(SymbolKind::Field, f.name, f.pos, f.type, ext, r.name));
        }
        for (std::size_t i = 0; i < unit.classes.size(); ++i)    registerType(unit.classes[i].name,    unit.classes[i].namePos,    unit.classes[i].members,    i >= req_->userClasses);
        for (std::size_t i = 0; i < unit.interfaces.size(); ++i) registerType(unit.interfaces[i].name, unit.interfaces[i].namePos, unit.interfaces[i].members, i >= req_->userInterfaces);
        for (std::size_t i = 0; i < unit.enums.size(); ++i)      typeDefId_.emplace(unit.enums[i].name,  recordDef(SymbolKind::Type, unit.enums[i].name,  unit.enums[i].namePos,  tNamed(unit.enums[i].name),  i >= req_->userEnums));
        for (std::size_t i = 0; i < unit.unions.size(); ++i)     typeDefId_.emplace(unit.unions[i].name, recordDef(SymbolKind::Type, unit.unions[i].name, unit.unions[i].namePos, tNamed(unit.unions[i].name), i >= req_->userUnions));
        for (std::size_t i = 0; i < unit.values.size(); ++i) {
            const auto& v = unit.values[i];
            valueDefId_.emplace(v.name, recordDef(SymbolKind::Value, v.name, v.namePos, v.hasType ? v.type : TypeRef{}, i >= req_->userValues));
        }
    }

    // ---- scopes ----
    void pushScope() { scopes_.emplace_back(); }
    void popScope() {
        std::vector<std::pair<SourcePos, std::string>> unused;
        for (const auto& [n, l] : scopes_.back()) {
            if (!l.constInit || l.reads != 0 || n.empty() || l.nonConstAssign) continue;
            if (n[0] != '_') { unused.push_back({l.declPos, n}); continue; }
            // P40 review r4 (part 2): the documented `_` opt-out. The declaration has no effect (its initializer is
            // a constant), so it is dropped — C# would report the kept local as CS0219 under TreatWarningsAsErrors.
            // A later assignment would then name an undeclared variable, so that shape is refused instead.
            if (l.assigns == 0 && l.declStmt) l.declStmt->dropUnused = true;
            else if (l.assigns != 0)
                diags_.error(l.declPos, "'" + n + "' is assigned but never read — remove it; C# reports it as an "
                                        "unused variable even with the '_' prefix");
        }
        std::sort(unused.begin(), unused.end(), [](const auto& a, const auto& b) {
            return a.first.line != b.first.line ? a.first.line < b.first.line : a.first.col < b.first.col;
        });
        for (const auto& [pos, n] : unused)
            diags_.error(pos, "'" + n + "' is set to a constant but never read — remove it (or prefix its name with "
                              "'_'); C# reports it as an unused variable");
        scopes_.pop_back();
    }
    // A C# constant expression (what CS0219 calls "assigned a constant"): literals, and unary/binary/cast over
    // them, plus an enum member (`Color.Red`) — P40 review r4 added the cast (`(i64)5`) and the enum member.
    bool isConstExpr(const Expr* e) const {
        if (!e) return false;
        switch (e->kind) {
            case ExprKind::IntLit: case ExprKind::FloatLit: case ExprKind::StringLit: case ExprKind::CharLit:
            case ExprKind::BoolLit: case ExprKind::NullLit: return true;
            case ExprKind::Unary: case ExprKind::Cast: return isConstExpr(e->lhs.get());
            case ExprKind::Binary: return e->text != "??" && isConstExpr(e->lhs.get()) && isConstExpr(e->rhs.get());
            case ExprKind::Member:
                return e->lhs && e->lhs->kind == ExprKind::Name && enumNames_.count(e->lhs->text) && !lookup(e->lhs->text);
            default: return false;
        }
    }
    void declare(const std::string& name, TypeRef type, bool isMutable, SourcePos pos) {
        auto& top = scopes_.back();
        if (top.count(name)) diags_.error(pos, "'" + name + "' is already declared in this scope");
        int defId = recordModel_ ? recordDef(SymbolKind::Local, name, pos, type) : -1;
        Local l{std::move(type), isMutable, defId};
        l.lambdaDepth = lambdaDepth_;
        top[name] = std::move(l);
    }
    const Local* lookup(const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) return &found->second;
        }
        return nullptr;
    }
    Local* lookupMut(const std::string& name) { return const_cast<Local*>(lookup(name)); }

    // A `.pg` spelling of a type for diagnostics (`string?`, `List<i32>`, `(i32) => bool`).
    static std::string showTy(const TypeRef& t) {
        std::string s;
        if (t.kind == TypeRef::Kind::Tuple || t.kind == TypeRef::Kind::Function) {
            s = "(";
            for (std::size_t i = 0; i < t.args.size(); ++i) { if (i) s += ", "; s += showTy(t.args[i]); }
            s += ")";
            if (t.kind == TypeRef::Kind::Function) s += " => " + (t.ret.empty() ? std::string("unit") : showTy(t.ret[0]));
            return t.nullable ? "(" + s + ")?" : s;
        }
        s = t.name.empty() ? std::string("null") : t.name;
        if (!t.args.empty()) {
            s += "<";
            for (std::size_t i = 0; i < t.args.size(); ++i) { if (i) s += ", "; s += showTy(t.args[i]); }
            s += ">";
        }
        return s + (t.nullable && !t.name.empty() ? "?" : "");
    }
    // The nullable spelling of a type for a hint — `i32?`, but `(() => unit)?` for a function type (P40 review r5:
    // the hint read "declare the slot '() => unit?'", which is a function returning `unit?`).
    static std::string showNullable(TypeRef t) {
        t.nullable = true;
        return showTy(t);
    }
    // A local's narrowing applies at this point. It never reaches into a lambda declared inside the
    // narrowed region: a mutable local may be reassigned before the lambda runs, and neither C# nor older
    // TypeScript carries an outer narrowing into a closure (the emitted code would warn / not type-check).
    // P40 review r5 item 19: the test itself may sit inside the lambda (SPEC §4.3) — what matters is where the
    // narrowing was established, not where the variable was declared.
    bool narrowedHere(const Local& l) const {
        return l.nonNull && l.narrowDepth == lambdaDepth_;
    }
    // Member access / method call on a maybe-null receiver without `?.` (C#/TS throw a TypeError-class
    // error, Python an AttributeError, PHP a warning + null — a cross-target divergence, refused).
    void requireNonNullReceiver(const TypeRef& recv, const std::string& member, bool nullSafe, SourcePos pos) {
        if (recv.nullable && !nullSafe && !(recv.kind == TypeRef::Kind::Named && recv.name.empty()))
            diags_.error(pos, "'." + member + "' on a receiver of type '" + showTy(recv) +
                                  "', which may be null; use '?." + member + "', narrow it ('if x != null'), or assert with '!'");
    }
    // N1: a nullable operand to an operator other than `==`, `!=`, `??`.
    void requireNonNullOperand(const TypeRef& t, const std::string& op, SourcePos pos) {
        if (t.nullable)
            diags_.error(pos, "'" + op + "' operand of type '" + showTy(t) +
                                  "' may be null; supply a fallback with '??' or assert with '!'");
    }

    // P40 A3 + D1: the printable guard, shared by `print` and every interpolation hole so the two accept
    // exactly the same set — the types whose text is one rule on every target: the integer and float scalars,
    // bool, char, string, and a type parameter (rendered at run time by the same rule, `null` included).
    // An enum is refused (D1: C# prints the case name, the others its number); records, classes, unions,
    // collections, tuples and functions are refused (C# ToString vs JS `[object Object]` vs Python repr).
    // A nullable value is N2's error, and an unknown type is an earlier error, so neither is re-reported.
    void requirePrintable(const TypeRef& t, SourcePos pos, bool hole) {
        if (t.nullable || (t.kind == TypeRef::Kind::Named && t.name.empty())) return;
        static const std::unordered_set<std::string> kPrintable = {
            "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64", "bool", "char", "string"};
        if (t.kind == TypeRef::Kind::Named && t.args.empty() &&
            (kPrintable.count(t.name) || genericsInScope_.count(t.name)))
            return;
        // Member reads are typed through the receiver (P40 slice 7), so a bare type-parameter name out of its
        // scope is no longer an expected "don't know" — it is judged like any other non-printable type (its
        // run-time text would depend on the target). Only a name that is neither a declaration nor any
        // declared type parameter (an `unknown type` already reported) is skipped, to avoid a second error.
        const bool known = t.kind != TypeRef::Kind::Named || t.name == "unit" || enumNames_.count(t.name) ||
                           types_.count(t.name) || interfaceNames_.count(t.name) ||
                           externClassNames_.count(t.name) || unionAllCases_.count(t.name) ||
                           declaredTypeParams_.count(t.name);
        if (!known) return;
        const std::string what = hole ? "cannot interpolate a value of type '" + showTy(t) + "'"
                                      : "print cannot print a value of type '" + showTy(t) + "'";
        if (t.kind == TypeRef::Kind::Named && declaredTypeParams_.count(t.name) && !types_.count(t.name))
            diags_.error(pos, what + " — its type parameter was never bound (the generic's type arguments are "
                                     "not known here); spell them, e.g. `Box<i32>(…)` or an annotated `let`");
        else if (t.kind == TypeRef::Kind::Named && enumNames_.count(t.name))
            diags_.error(pos, what + " — an enum has no portable text (C# prints the case name, the other "
                                     "targets its number); map it to a string explicitly, e.g. with a match");
        else
            diags_.error(pos, what + " — only numbers, bool, char and string have one text on every target; "
                                     "format the value explicitly");
    }

    // ---- P40 D14: null flow-narrowing ----
    // The names a condition proves non-null when it evaluates to `sense`: `x != null` (true), `x == null`
    // (false), through `&&` (true), `||` (false) and `!`. Only a bare name — a local or param.
    static void nullFacts(const Expr& c, bool sense, std::vector<std::string>& out) {
        if (c.kind == ExprKind::Binary && (c.text == "!=" || c.text == "==") && c.lhs && c.rhs) {
            // An `Option<X>` local compared with null was converted to its native `X?` (an implicit Cast,
            // SPEC §4.4) — the test is still about the bare name.
            auto bare = [](const Expr* x) -> const Expr* {
                if (x->kind == ExprKind::Cast && x->lhs && x->lhs->kind == ExprKind::Name && isOptionType(x->lhs->type))
                    return x->lhs.get();
                return x;
            };
            const Expr* l = bare(c.lhs.get());
            const Expr* r = bare(c.rhs.get());
            const Expr* name = nullptr;
            if (r->kind == ExprKind::NullLit && l->kind == ExprKind::Name) name = l;
            else if (l->kind == ExprKind::NullLit && r->kind == ExprKind::Name) name = r;
            if (name && (c.text == "!=") == sense) out.push_back(name->text);
        } else if (c.kind == ExprKind::Binary && c.text == "&&" && sense) {
            nullFacts(*c.lhs, true, out); nullFacts(*c.rhs, true, out);
        } else if (c.kind == ExprKind::Binary && c.text == "||" && !sense) {
            nullFacts(*c.lhs, false, out); nullFacts(*c.rhs, false, out);
        } else if (c.kind == ExprKind::Unary && c.text == "!" && c.lhs) {
            nullFacts(*c.lhs, !sense, out);
        }
    }
    // Narrow `names` (when they are nullable locals that may be narrowed), recording the prior state into
    // `saves` so the region's exit restores it.
    void narrow(const std::vector<std::string>& names, std::vector<NarrowSave>& saves) {
        for (const auto& n : names) {
            Local* l = lookupMut(n);
            // A native nullable, or an `Option<X>` local (a user generic's `T?`, SPEC §4.4): its narrowed read
            // is the unwrapped X.
            if (!l || (!l->type.nullable && !isOptionType(l->type)) ||
                (l->type.kind == TypeRef::Kind::Named && l->type.name.empty())) continue;
            if (l->isMutable && closureAssigned_.count(n)) continue; // a closure may reassign it at any time
            saves.push_back({l, l->nonNull, l->assigns, l->narrowDepth});
            l->nonNull = true;
            l->narrowDepth = lambdaDepth_;
        }
    }
    // Un-narrow (for a loop body that assigns them: the back edge re-enters with the reassigned value).
    void unnarrow(const std::vector<std::string>& names, std::vector<NarrowSave>& saves) {
        for (const auto& n : names) {
            Local* l = lookupMut(n);
            if (!l || !l->nonNull) continue;
            saves.push_back({l, l->nonNull, l->assigns, l->narrowDepth});
            l->nonNull = false;
        }
    }
    // Leave a region: the prior state comes back unless the variable was assigned inside the region.
    static void restoreNarrow(std::vector<NarrowSave>& saves) {
        for (auto it = saves.rbegin(); it != saves.rend(); ++it) {
            it->local->nonNull = it->prev && it->local->assigns == it->assignsAtEntry;
            it->local->narrowDepth = it->prevDepth;
        }
        saves.clear();
    }
    // A statement list: one narrowing frame, so an early exit's narrowing ends with the list.
    void checkStmts(std::vector<StmtPtr>& body) {
        narrowFrames_.emplace_back();
        for (auto& st : body) checkStmt(*st);
        restoreNarrow(narrowFrames_.back());
        narrowFrames_.pop_back();
    }
    // An unconditional exit as a list's last statement (`return`, `throw`, `break`, `continue`).
    static bool endsInExit(const std::vector<StmtPtr>& body) {
        if (body.empty()) return false;
        const StmtKind k = body.back()->kind;
        return k == StmtKind::Return || k == StmtKind::Throw || k == StmtKind::Break || k == StmtKind::Continue;
    }
    // Names assigned (`name = …` / `name op= …`) anywhere in a statement list; `lambdaOnly` keeps only the
    // assignments inside a lambda body. Name-based and conservative (shadowing is ignored).
    static void collectAssigned(const std::vector<StmtPtr>& body, bool inLambda, bool lambdaOnly,
                                std::unordered_set<std::string>& out) {
        for (const auto& s : body) if (s) collectAssigned(*s, inLambda, lambdaOnly, out);
    }
    static void collectAssigned(const Stmt& s, bool inLambda, bool lambdaOnly, std::unordered_set<std::string>& out) {
        if (s.kind == StmtKind::Assign && s.target && s.target->kind == ExprKind::Name && (inLambda || !lambdaOnly))
            out.insert(s.target->text);
        if (s.target) collectAssigned(s.target.get(), inLambda, lambdaOnly, out);
        if (s.value) collectAssigned(s.value.get(), inLambda, lambdaOnly, out);
        collectAssigned(s.thenBody, inLambda, lambdaOnly, out);
        collectAssigned(s.elseBody, inLambda, lambdaOnly, out);
        for (const auto& c : s.catches) {
            if (c.guard) collectAssigned(c.guard.get(), inLambda, lambdaOnly, out);
            collectAssigned(c.body, inLambda, lambdaOnly, out);
        }
        collectAssigned(s.finallyBody, inLambda, lambdaOnly, out);
    }
    static void collectAssigned(const Expr* e, bool inLambda, bool lambdaOnly, std::unordered_set<std::string>& out) {
        if (!e) return;
        const bool lam = inLambda || e->kind == ExprKind::Lambda;
        collectAssigned(e->lhs.get(), lam, lambdaOnly, out);
        collectAssigned(e->rhs.get(), inLambda, lambdaOnly, out);
        collectAssigned(e->extra.get(), inLambda, lambdaOnly, out);
        for (const auto& a : e->args) collectAssigned(a.get(), inLambda, lambdaOnly, out);
        collectAssigned(e->block, lam, lambdaOnly, out);
        for (const auto& f : e->fields) collectAssigned(f.value.get(), inLambda, lambdaOnly, out);
        for (const auto& arm : e->arms) {
            collectAssigned(arm.guard.get(), inLambda, lambdaOnly, out);
            collectAssigned(arm.body.get(), inLambda, lambdaOnly, out);
            collectAssigned(arm.block, inLambda, lambdaOnly, out);
        }
    }

    // ---- declaration tables ----
    // `externOnBuiltin`: an `extern class` naming a builtin scalar (std.core's `extern class i32 { static
    // fn parse … }`) is a MEMBER CARRIER, not a shadow — the scalar type stays authoritative, so the name
    // is not registered as a declared type; only its binding members matter (lower reads them by name).
    void declareType(const std::string& name, SourcePos pos, bool externOnBuiltin = false) {
        if (isBuiltinType(name)) {
            if (!externOnBuiltin) diags_.error(pos, "'" + name + "' shadows a builtin type");
            return;
        }
        if (!typeNames_.insert(name).second) diags_.error(pos, "duplicate type '" + name + "'");
    }
    void collectTypeNames(const CompilationUnit& u) {
        for (const auto& d : u.records)    { declareType(d.name, d.pos); recordNames_.insert(d.name); }
        for (const auto& d : u.classes)    { declareType(d.name, d.pos, d.isExtern); if (d.isExtern) externClassNames_.insert(d.name); }
        for (const auto& d : u.interfaces) declareType(d.name, d.pos);
        for (const auto& d : u.enums)      { declareType(d.name, d.pos); enumNames_.insert(d.name); }
        for (const auto& d : u.unions)     declareType(d.name, d.pos);
    }

    // An extension's type variables are written on the receiver — SPEC §6.3 `extension fn List<T>.m(): T?`.
    // The receiver is a *pattern*, so a type-arg leaf that names nothing known (not a builtin, not a declared
    // type, not already a generic) is a binding occurrence of a type parameter. Lift those into the
    // extension's generic list so every downstream pass (tables, resolution, body check, lower, both
    // emitters — all of which already read `generics`) treats them uniformly. Runs after collectTypeNames
    // so `typeNames_` is populated.
    void collectReceiverVars(const TypeRef& t, std::unordered_set<std::string>& have, std::vector<std::string>& out) const {
        for (const auto& a : t.args) {
            if (a.kind == TypeRef::Kind::Named && !a.name.empty() && a.args.empty() &&
                !isBuiltinType(a.name) && !typeNames_.count(a.name) && have.insert(a.name).second)
                out.push_back(a.name);
            collectReceiverVars(a, have, out);
        }
        for (const auto& r : t.ret) collectReceiverVars(r, have, out);
    }
    void liftExtensionGenerics(CompilationUnit& u) {
        for (auto& e : u.extensions) {
            std::unordered_set<std::string> have;
            for (const auto& g : e.generics) have.insert(g.name);
            std::vector<std::string> found;
            collectReceiverVars(e.receiver, have, found);
            for (const auto& n : found) { GenericParam g; g.name = n; e.generics.push_back(std::move(g)); }
        }
    }

    // `T?` where `T` is a type parameter in scope has no faithful native nullable (PRD §3.C): rewrite it to
    // the core `Option<T>` union. Concrete `T?` (`string?`, `i32?`, `List<T>?`) keeps the native nullable.
    // Runs before buildTables so every table + downstream pass sees the normalized type.
    static void normalizeOptional(TypeRef& t, const std::unordered_set<std::string>& gens) {
        for (auto& a : t.args) normalizeOptional(a, gens);
        for (auto& r : t.ret) normalizeOptional(r, gens);
        if (t.nullable && t.kind == TypeRef::Kind::Named && gens.count(t.name)) {
            TypeRef inner; inner.kind = TypeRef::Kind::Named; inner.name = t.name; // the bare `T`
            t.name = "Option"; t.nullable = false; t.args = { std::move(inner) };
        }
    }
    static void normalizeMembers(std::vector<Member>& ms, std::unordered_set<std::string> classGens) {
        for (auto& m : ms) {
            auto g = classGens; for (const auto& mg : m.generics) g.insert(mg.name);
            if (m.kind == MemberKind::Field || m.kind == MemberKind::Const || m.kind == MemberKind::Property) normalizeOptional(m.type, g);
            else { for (auto& p : m.params) normalizeOptional(p.type, g); normalizeOptional(m.returnType, g); }
        }
    }
    void normalizeOptionalGenerics(CompilationUnit& u) {
        auto setOf = [](const std::vector<GenericParam>& gs) { std::unordered_set<std::string> s; for (const auto& g : gs) s.insert(g.name); return s; };
        for (auto& fn : u.functions)  { auto g = setOf(fn.generics); for (auto& p : fn.params) normalizeOptional(p.type, g); normalizeOptional(fn.returnType, g); }
        for (auto& d : u.records)     { auto g = setOf(d.generics); for (auto& f : d.fields) normalizeOptional(f.type, g); normalizeMembers(d.members, g); }
        // An `extern class` keeps its class-level `T?` as written: its members are bindings that return a NATIVE
        // nullable, never an Option, so a member read substitutes `V?` with `V = i32` into a native `i32?`
        // (P40 slice 7). Only the member's own type params are normalized.
        for (auto& d : u.classes)     { auto g = d.isExtern ? std::unordered_set<std::string>{} : setOf(d.generics); normalizeMembers(d.members, g); }
        for (auto& d : u.interfaces)  { auto g = setOf(d.generics); normalizeMembers(d.members, g); }
        for (auto& d : u.unions)      { auto g = setOf(d.generics); for (auto& c : d.cases) for (auto& p : c.params) normalizeOptional(p.type, g); }
        for (auto& d : u.extensions)  { auto g = setOf(d.generics); for (auto& p : d.params) normalizeOptional(p.type, g); normalizeOptional(d.returnType, g); }
        for (auto& v : u.values)      if (v.hasType) { std::unordered_set<std::string> g; normalizeOptional(v.type, g); }
        // (local `let x: T?` declared types inside bodies are normalized lazily in checkStmt's resolve.)
    }

    static MemberInfo memberInfo(const Member& m) {
        MemberInfo mi;
        mi.name = m.name;
        mi.kind = m.kind;
        for (const auto& p : m.params) mi.params.push_back(p.type);
        for (const auto& g : m.generics) mi.generics.push_back(g.name);
        mi.numericParams = numericBounded(m.generics);
        mi.boundSpecs = m.generics;
        mi.required = requiredCount(m.params);
        mi.type = (m.kind == MemberKind::Field || m.kind == MemberKind::Const ||
                   m.kind == MemberKind::Property) ? m.type : m.returnType;
        for (const auto& mod : m.modifiers) {
            if (mod == "static") mi.isStatic = true;
            if (mod == "open" || mod == "abstract") mi.isOpen = true;
        }
        mi.isAsync = m.isAsync;
        mi.isReadOnly = m.kind == MemberKind::Const || (m.kind == MemberKind::Field && !m.isMutable);
        return mi;
    }
    void addMembers(TypeInfo& ti, const std::vector<Member>& members) {
        std::unordered_set<std::string> methodNames;
        for (const auto& m : members) {
            // #53: member overloading is not supported (top-level `fn` overloading is — PRD §3.A). A second
            // same-named method used to silently shadow the first and surface a misleading call-site arity
            // error; refuse it at the DECLARATION site instead.
            if (m.kind == MemberKind::Method && !methodNames.insert(m.name).second)
                diags_.error(m.namePos, "method '" + m.name + "' is already defined — Polyglot supports "
                                            "overloading for top-level functions only, not members (PRD §3.A); "
                                            "give the overloads distinct names");
            ti.members.push_back(memberInfo(m));
            if (m.kind == MemberKind::Constructor) { ti.hasCtor = true; ti.ctorRequired = requiredCount(m.params); for (const auto& p : m.params) ti.ctorParams.push_back(p.type); }
        }
    }
    static void collectBaseNames(const std::vector<TypeRef>& bases, TypeInfo& ti) {
        for (const auto& b : bases)
            if (b.kind == TypeRef::Kind::Named && !b.name.empty()) { ti.bases.push_back(b.name); ti.baseRefs.push_back(b); }
    }
    void buildTables(CompilationUnit& u) {
        { // P40 slice 7: the printable guard tells a leaked type parameter from an unknown name
            auto add = [&](const std::vector<GenericParam>& gs) { for (const auto& g : gs) declaredTypeParams_.insert(g.name); };
            auto addMembers = [&](const std::vector<Member>& ms) { for (const auto& m : ms) add(m.generics); };
            for (const auto& d : u.records)    { add(d.generics); addMembers(d.members); }
            for (const auto& d : u.classes)    { add(d.generics); addMembers(d.members); }
            for (const auto& d : u.interfaces) { add(d.generics); addMembers(d.members); }
            for (const auto& d : u.unions)     add(d.generics);
            for (const auto& d : u.functions)  add(d.generics);
            for (const auto& d : u.extensions) add(d.generics);
        }
        for (const auto& d : u.records) {
            TypeInfo ti; ti.generics = d.generics; ti.hasCtor = true; ti.ctorRequired = requiredCount(d.fields);
            for (const auto& f : d.fields) {
                ti.ctorParams.push_back(f.type);
                MemberInfo fi{f.name, MemberKind::Field, {}, f.type};
                fi.isReadOnly = true; // P40 review r3 2(i): a record is immutable (SPEC); C# CS8852 on a write
                fi.isRecordField = true;
                ti.members.push_back(std::move(fi));
            }
            addMembers(ti, d.members); collectBaseNames(d.bases, ti);
            types_[d.name] = std::move(ti);
        }
        for (const auto& d : u.classes)    { TypeInfo ti; ti.generics = d.generics; addMembers(ti, d.members); collectBaseNames(d.bases, ti); types_[d.name] = std::move(ti); }
        for (const auto& d : u.interfaces) {
            TypeInfo ti; ti.generics = d.generics; addMembers(ti, d.members); collectBaseNames(d.bases, ti);
            types_[d.name] = std::move(ti);
            interfaceNames_.insert(d.name);
            interfaceDecls_[d.name] = &d; // base TypeRefs with their generic args (TypeInfo.bases keeps names only)
        }
        for (const auto& d : u.unions) {
            for (const auto& c : d.cases) {
                // A union case name must be unique across the program (it's a global constructor). Two
                // cases sharing a name — within a union, across unions, or merged from two modules — is a
                // hard error, not a silent last-wins overwrite.
                if (unionCtors_.count(c.name))
                    diags_.error(c.pos, "duplicate union case '" + c.name + "'");
                // A generic union's case is a generic constructor: `Some(value: T): Option<T>`. Carrying the
                // union's type params (sig.generics) + a parameterized result lets `Some(5)` infer Option<i32>.
                FnSig sig; for (const auto& p : c.params) sig.params.push_back(p.type);
                for (const auto& g : d.generics) sig.generics.push_back(g.name);
                TypeRef res = tNamed(d.name);
                for (const auto& g : d.generics) res.args.push_back(namedType(g.name));
                sig.result = res;
                unionCtors_[c.name] = sig;
                unionCaseOwner_[c.name] = d.name;
                unionAllCases_[d.name].push_back(c.name);
            }
        }
        for (const auto& d : u.enums)
            for (const auto& c : d.cases) enumAllCases_[d.name].push_back(c.name);
        for (const auto& fn : u.functions) { // collect overload sets; an `actual` is an impl, not a signature
            if (!fn.actualTarget.empty()) continue;
            FnSig sig; for (const auto& p : fn.params) sig.params.push_back(p.type); sig.result = fn.returnType;
            sig.required = requiredCount(fn.params);
            sig.isAsync = fn.isAsync;
            sig.isPrelude = fn.originModule == "<prelude>";
            for (const auto& g : fn.generics) sig.generics.push_back(g.name);
            sig.numericParams = numericBounded(fn.generics);
            sig.boundSpecs = fn.generics;
            sig.pos = fn.pos;
            sig.prelude = fn.originModule == "<prelude>";
            auto& set = fns_[fn.name];
            for (const auto& existing : set)
                if (sameParamList(existing.params, sig.params)) {
                    // P40 review A8: name BOTH declarations — across modules the error's own position names
                    // only one file, and the other declaration was left to be guessed.
                    std::string other = "line " + std::to_string(existing.pos.line);
                    if (existing.pos.fileId != fn.pos.fileId && modules_ && modules_->describe)
                        other = "module '" + modules_->describe(existing.pos.fileId) + "', " + other;
                    std::string here;
                    if (existing.pos.fileId != fn.pos.fileId && modules_ && modules_->describe)
                        here = " in module '" + modules_->describe(fn.pos.fileId) + "'";
                    diags_.error(fn.pos, "duplicate function '" + fn.name + "' with the same parameter types" + here +
                                             " (also declared at " + other + ")");
                }
            set.push_back(std::move(sig));
        }
        // Assign per-target names: an overloaded function mangles its params. P40 review r3 (part 2): the overload
        // set is the one the DECLARING module sees (its own + imported declarations, D16) — a library's exported
        // name must not change because an unrelated module declares the same name. The unique name (PHP, one
        // function namespace, no overloading) is the mangled one whenever the program declares the name twice.
        // The link name (C#, one shared class WITH native overloading) starts as the plain name and is mangled
        // only on a real clash, decided once every call is resolved (assignLinkNames, review r4 #12/#13).
        std::unordered_map<std::string, std::set<std::string>> userOrigins; // name -> user modules declaring it
        for (const auto& fn : u.functions)
            if (fn.actualTarget.empty() && fn.originModule != "<prelude>") userOrigins[fn.name].insert(fn.originModule);
        auto samePos = [](const SourcePos& a, const SourcePos& b) {
            return a.line == b.line && a.col == b.col && a.fileId == b.fileId;
        };
        for (auto& fn : u.functions) {
            std::vector<TypeRef> ps; for (const auto& p : fn.params) ps.push_back(p.type);
            const bool crossModule = userOrigins[fn.name].size() > 1;
            fn.linkName = fn.name;
            if (!fn.actualTarget.empty()) { // an actual reuses its expect's names
                fn.mangledName = fn.name;
                fn.uniqueName = crossModule ? mangleFn(fn.name, ps, true) : fn.name;
                continue;
            }
            auto& set = fns_[fn.name];
            std::size_t visible = 0;
            for (const auto& sig : set)
                if (!modules_ || !modules_->visible || sig.pos.fileId == fn.pos.fileId ||
                    modules_->visible(fn.pos.fileId, sig.pos.fileId, fn.name))
                    ++visible;
            fn.mangledName = mangleFn(fn.name, ps, visible > 1);
            fn.uniqueName = crossModule ? mangleFn(fn.name, ps, true) : fn.mangledName;
            for (auto& sig : set)
                if (samePos(sig.pos, fn.pos)) { sig.mangled = fn.mangledName; sig.linkName = fn.linkName; sig.uniqueName = fn.uniqueName; }
        }
        for (const auto& v : u.values) {
            if (values_.count(v.name)) diags_.error(v.pos, "duplicate top-level '" + v.name + "'");
            values_[v.name] = v.hasType ? v.type : tUnknown();
            valueDeclPos_[v.name] = v.pos;
        }
        auto noteStatics = [&](const std::string& owner, const std::vector<Member>& ms) {
            for (const auto& m : ms) {
                if (m.kind != MemberKind::Field && m.kind != MemberKind::Const) continue;
                bool st = m.kind == MemberKind::Const;
                for (const auto& mod : m.modifiers) if (mod == "static") st = true;
                if (st) staticDeclPos_[owner + "." + m.name] = m.pos;
            }
        };
        for (const auto& d : u.classes) noteStatics(d.name, d.members);
        for (const auto& d : u.records) noteStatics(d.name, d.members);
        for (const auto& d : u.classes) if (!d.isExtern) { typeDeclPos_[d.name] = d.pos; userClassNames_.insert(d.name); }
        for (const auto& d : u.records) typeDeclPos_[d.name] = d.pos;
        for (const auto& d : u.enums) typeDeclPos_[d.name] = d.pos;
        for (const auto& d : u.unions) { typeDeclPos_[d.name] = d.pos; for (const auto& c : d.cases) typeDeclPos_[c.name] = d.pos; }
        for (const auto& e : u.extensions) {
            std::string key = e.receiver.name + "." + e.name;
            if (extensions_.count(key)) diags_.error(e.pos, "duplicate extension '" + key + "'");
            FnSig sig; for (const auto& p : e.params) sig.params.push_back(p.type); sig.result = e.returnType;
            sig.required = requiredCount(e.params); // P40 slice 7: trailing defaults are filled at the call
            sig.receiver = e.receiver;
            for (const auto& g : e.generics) sig.generics.push_back(g.name); // for inference from receiver/args
            extensions_[key] = std::move(sig); // keyed by receiver type + method
        }
    }

    const MemberInfo* findMember(const std::string& typeName, const std::string& name) const {
        // `Error.message` resolves here like any other member: `Error` is a core-prelude `extern class` whose
        // `message` property is in `types_`, reached directly or via a `: Error` base (the loop below).
        auto it = types_.find(typeName);
        if (it == types_.end()) return nullptr;
        for (const auto& m : it->second.members) if (m.name == name) return &m;
        for (const auto& base : it->second.bases) if (const MemberInfo* m = findMember(base, name)) return m; // inherited
        return nullptr;
    }
    const TypeInfo* typeInfoOf(const std::string& name) const {
        auto it = types_.find(name);
        return it == types_.end() ? nullptr : &it->second;
    }

    // ---- P40 slice 7: generic member typing ----
    // A member found on a receiver, with the bindings of its OWNER's type params taken from the receiver's
    // type arguments — `Cell<string>.first(): V` binds V = string; through `class D<T> : B<List<T>>` a member
    // of B binds B's param to `List<T'>` with D's own binding applied. This is the member-side twin of
    // inferResult: every member read/call types with the instance's arguments, never a bare `V`.
    struct MemberHit {
        const MemberInfo* m = nullptr;
        std::string owner;                              // the declaring type (may be a base of the receiver)
        bool ownerExtern = false;                       // an `extern class` (its members are native bindings)
        std::unordered_set<std::string> gen;            // the owner's type-param names
        std::unordered_map<std::string, TypeRef> binds; // owner param -> the receiver's argument
    };
    MemberHit findMemberOn(const std::string& typeName, const std::vector<TypeRef>& args, const std::string& name,
                           int depth = 0) {
        MemberHit hit;
        auto it = types_.find(typeName);
        if (it == types_.end() || depth > 32) return hit;
        const TypeInfo& ti = it->second;
        std::unordered_set<std::string> gen;
        std::unordered_map<std::string, TypeRef> binds;
        for (std::size_t i = 0; i < ti.generics.size(); ++i) {
            gen.insert(ti.generics[i].name);
            if (i < args.size() && !(args[i].kind == TypeRef::Kind::Named && args[i].name.empty()))
                binds[ti.generics[i].name] = args[i];
        }
        for (const auto& m : ti.members)
            if (m.name == name) {
                hit.m = &m; hit.owner = typeName; hit.ownerExtern = externClassNames_.count(typeName) != 0;
                hit.gen = std::move(gen); hit.binds = std::move(binds);
                return hit;
            }
        for (const auto& b : ti.baseRefs) {
            TypeRef sb = substGeneric(b, gen, binds); // the base as THIS instance sees it
            MemberHit h = findMemberOn(sb.name, sb.args, name, depth + 1);
            if (h.m) return h;
        }
        return hit;
    }
    MemberHit findMemberOn(const TypeRef& recv, const std::string& name) {
        if (recv.kind != TypeRef::Kind::Named || recv.name.empty()) return {};
        return findMemberOn(recv.name, recv.args, name);
    }
    // A member's declared type as seen through `hit` (the method's own type params shadow the class's).
    // An extern member's `V?` becomes a native nullable once V is concrete; while V is still a type
    // parameter it has no faithful form (a binding returns native null, the language's `T?` is an Option),
    // so it is refused rather than typed either way.
    static std::string nullableTypeParamIn(const TypeRef& t, const std::unordered_set<std::string>& a,
                                           const std::unordered_set<std::string>& b) {
        if (t.kind == TypeRef::Kind::Named && t.nullable && t.args.empty() && (a.count(t.name) || b.count(t.name)))
            return t.name;
        for (const auto& x : t.args) if (auto n = nullableTypeParamIn(x, a, b); !n.empty()) return n;
        for (const auto& x : t.ret)  if (auto n = nullableTypeParamIn(x, a, b); !n.empty()) return n;
        return "";
    }
    TypeRef memberTypeOf(const MemberHit& hit, const TypeRef& declared, SourcePos pos, bool isResult = true) {
        if (hit.binds.empty() && hit.gen.empty()) return declared;
        std::unordered_set<std::string> gen = hit.gen;
        if (hit.m) for (const auto& g : hit.m->generics) gen.erase(g);
        TypeRef r = substGeneric(declared, gen, hit.binds);
        if (hit.ownerExtern && isResult) {
            std::string leaked = nullableTypeParamIn(r, gen, genericsInScope_);
            if (!leaked.empty())
                diags_.error(pos, "'" + hit.owner + "." + (hit.m ? hit.m->name : std::string()) + "' yields '" +
                                      leaked + "?' while '" + leaked + "' is still a type parameter — a binding "
                                      "returns a native null, which has no faithful form for an unresolved type "
                                      "parameter (PRD §3.C); use it where the type argument is concrete");
        }
        return r;
    }
    std::vector<TypeRef> memberParamsOf(const MemberHit& hit, SourcePos pos) {
        std::vector<TypeRef> ps;
        for (const auto& p : hit.m->params) ps.push_back(memberTypeOf(hit, p, pos, /*isResult=*/false));
        return ps;
    }
    bool knownType(const TypeRef& t) const {
        return t.kind == TypeRef::Kind::Named && !t.name.empty() && types_.count(t.name) != 0;
    }

    // ---- interface conformance + `override` validation (issue #29) ----
    // The contract behind `class X : IFoo` is checked at the SOURCE: every method the interface (and its
    // base interfaces) declares must be implemented with a matching signature, so a breach is a Polyglot
    // diagnostic — never an error surfacing later in one target's generated code. `override` follows C#
    // conventions: it marks only an override of an `open`/`abstract` base-CLASS member; implementing an
    // interface method takes no `override`.
    bool inheritsFrom(const std::string& sub, const std::string& super) const {
        auto it = types_.find(sub);
        if (it == types_.end()) return false;
        for (const auto& b : it->second.bases)
            if (b == super || inheritsFrom(b, super)) return true;
        return false;
    }
    bool interfaceHasMethod(const std::string& iface, const std::string& name) const {
        auto it = types_.find(iface);
        if (it == types_.end()) return false;
        for (const auto& m : it->second.members) if (m.name == name) return true;
        for (const auto& b : it->second.bases) if (interfaceHasMethod(b, name)) return true;
        return false;
    }
    // Methods named `name` on the type or its base-CLASS chain. Interface bases are excluded — an
    // interface's own signature must never satisfy (or be overridden as) a class member.
    void gatherMethods(const std::string& typeName, const std::string& name, std::vector<const MemberInfo*>& out) const {
        auto it = types_.find(typeName);
        if (it == types_.end()) return;
        for (const auto& m : it->second.members)
            if (m.name == name && (m.kind == MemberKind::Method || m.kind == MemberKind::Operator)) out.push_back(&m);
        for (const auto& b : it->second.bases)
            if (!interfaceNames_.count(b)) gatherMethods(b, name, out);
    }
    // Like sameNamedType but signature-position: nullability must AGREE (not be absent), so an interface's
    // `Node?` is satisfied by an implementing `Node?`.
    static bool sameSigType(const TypeRef& a, const TypeRef& b) {
        if (a.kind == TypeRef::Kind::Named && b.kind == TypeRef::Kind::Named &&
            a.name == b.name && a.nullable == b.nullable) return true;
        Ty sa = scalarTyOf(a);
        return sa != Ty::Unknown && sa == scalarTyOf(b);
    }
    static bool sameSigParams(const std::vector<TypeRef>& a, const std::vector<TypeRef>& b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) if (!sameSigType(a[i], b[i])) return false;
        return true;
    }
    static bool sameRetType(TypeRef a, TypeRef b) {
        if (a.kind == TypeRef::Kind::Named && a.name.empty()) a.name = "unit";
        if (b.kind == TypeRef::Kind::Named && b.name.empty()) b.name = "unit";
        return sameSigType(a, b);
    }
    struct RequiredMethod { std::string iface; std::string name; std::vector<TypeRef> params; TypeRef ret; };
    static std::string describeRequired(const RequiredMethod& r) {
        std::string s = "fn " + r.name + "(";
        for (std::size_t i = 0; i < r.params.size(); ++i) { if (i) s += ", "; s += sigTypeName(r.params[i]); }
        s += ")";
        std::string ret = sigTypeName(r.ret);
        if (ret != "unit") s += ": " + ret;
        return s;
    }
    // Every method the interface demands of an implementer, with the interface's type params substituted
    // from the implements-clause type args (`Comparable<Version>` demands `compareTo(Version)`), incl.
    // transitively inherited interface methods (args composed through each hop).
    void collectRequired(const std::string& iface, const std::vector<TypeRef>& args,
                         std::vector<RequiredMethod>& out, std::unordered_set<std::string>& seen) {
        if (!seen.insert(iface).second) return; // repeated/diamond base — first visit wins
        auto ti = types_.find(iface);
        if (ti == types_.end()) return;
        std::unordered_set<std::string> gen;
        std::unordered_map<std::string, TypeRef> binds;
        for (std::size_t i = 0; i < ti->second.generics.size(); ++i) {
            gen.insert(ti->second.generics[i].name);
            if (i < args.size()) binds[ti->second.generics[i].name] = args[i];
        }
        for (const auto& m : ti->second.members) {
            if (m.kind != MemberKind::Method) continue;
            RequiredMethod r{iface, m.name, {}, substGeneric(m.type, gen, binds)};
            for (const auto& p : m.params) r.params.push_back(substGeneric(p, gen, binds));
            out.push_back(std::move(r));
        }
        if (auto di = interfaceDecls_.find(iface); di != interfaceDecls_.end())
            for (const auto& b : di->second->bases) {
                if (b.kind != TypeRef::Kind::Named || !interfaceNames_.count(b.name)) continue;
                std::vector<TypeRef> subArgs;
                for (const auto& a : b.args) subArgs.push_back(substGeneric(a, gen, binds));
                collectRequired(b.name, subArgs, out, seen);
            }
    }
    void checkImplements(const CompilationUnit& u) {
        // Interface bodies: bodiless, non-static method signatures only (issue #29 v1 scope). Anything
        // else used to be accepted and silently DROPPED at lowering — refuse it instead.
        for (const auto& d : u.interfaces) {
            for (const auto& m : d.members) {
                const char* what = m.kind == MemberKind::Field ? "a field"
                                 : m.kind == MemberKind::Const ? "a const"
                                 : m.kind == MemberKind::Property ? "a property"
                                 : m.kind == MemberKind::Constructor ? "an initializer" : nullptr;
                if (what) {
                    diags_.error(m.pos, "an interface declares method signatures only — " + std::string(what) +
                                            " is not allowed in 'interface " + d.name + "' (express it as a method)");
                    continue;
                }
                if (m.kind == MemberKind::Operator) {
                    diags_.error(m.pos, "an interface cannot declare an operator — operator contracts are not "
                                        "portable across targets; declare a named method instead");
                    continue;
                }
                if (m.hasBody)
                    diags_.error(m.pos, "interface method '" + m.name + "' must be a bodiless signature "
                                        "(default method bodies are not supported)");
                for (const auto& mod : m.modifiers)
                    if (mod == "static")
                        diags_.error(m.pos, "interface method '" + m.name + "' cannot be static");
            }
        }
        auto checkType = [&](const std::string& name, SourcePos namePos, const std::vector<TypeRef>& bases,
                             const std::vector<Member>& members, const char* what) {
            for (const auto& b : bases) {
                if (b.kind != TypeRef::Kind::Named || !interfaceNames_.count(b.name)) continue;
                std::vector<RequiredMethod> reqs;
                std::unordered_set<std::string> seen;
                collectRequired(b.name, b.args, reqs, seen);
                for (const auto& r : reqs) {
                    std::vector<const MemberInfo*> cands;
                    gatherMethods(name, r.name, cands);
                    bool ok = false, nameExists = false;
                    for (const MemberInfo* c : cands) {
                        if (c->isStatic) continue;
                        nameExists = true;
                        if (sameSigParams(c->params, r.params) && sameRetType(c->type, r.ret)) { ok = true; break; }
                    }
                    if (ok) continue;
                    if (nameExists)
                        diags_.error(namePos, std::string(what) + " '" + name + "' implements '" + r.iface +
                                                  "' but its '" + r.name + "' does not match the interface signature '" +
                                                  describeRequired(r) + "'");
                    else
                        diags_.error(namePos, std::string(what) + " '" + name + "' does not implement '" +
                                                  describeRequired(r) + "' declared by interface '" + r.iface + "'");
                }
            }
            for (const auto& m : members) {
                bool hasOverride = false;
                for (const auto& mod : m.modifiers) if (mod == "override") hasOverride = true;
                { // P40 review r5 #10: a static is never dispatched virtually — C# CS0112 (`static virtual`/`override`)
                    bool isStatic = m.kind == MemberKind::Const;
                    std::string virt;
                    for (const auto& mod : m.modifiers) {
                        if (mod == "static") isStatic = true;
                        if (mod == "open" || mod == "override" || mod == "abstract") virt = mod;
                    }
                    if (isStatic && !virt.empty()) {
                        diags_.error(m.namePos.line ? m.namePos : m.pos,
                                     "static member '" + m.name + "' cannot be '" + virt + "' — a static is not "
                                     "inherited or dispatched virtually (C# CS0112); remove '" + virt +
                                     "', or make it an instance member");
                        continue;
                    }
                }
                if (!hasOverride) {
                    // P40 review r3 2(f): redeclaring an inherited INSTANCE member without `override` is member
                    // hiding — C# binds it non-virtually (CS0108 is only a warning: `Base`-typed access sees the
                    // base member), while TS/Python/PHP override it. Refused, whatever the member kind.
                    bool isStatic = m.kind == MemberKind::Const;
                    for (const auto& mod : m.modifiers) if (mod == "static") isStatic = true;
                    if (m.kind == MemberKind::Constructor || m.kind == MemberKind::Operator) continue;
                    for (const auto& b : bases) {
                        if (b.kind != TypeRef::Kind::Named || b.name.empty() || interfaceNames_.count(b.name)) continue;
                        MemberHit hit = findMemberOn(b, m.name);
                        if (!hit.m || hit.m->kind == MemberKind::Constructor || interfaceNames_.count(hit.owner))
                            continue;
                        // P40 review r4 #6: statics hide too. A static over an instance member (or the reverse) is
                        // a PHP fatal ("Cannot make static method … non static"); a static over a static is C#
                        // CS0108 (an error under warnings-as-errors) and TS4114 under noImplicitOverride.
                        const bool hitStatic = hit.m->isStatic || hit.m->kind == MemberKind::Const;
                        if (isStatic || hitStatic) {
                            diags_.error(m.namePos, "'" + m.name + "' redeclares " +
                                                        std::string(hitStatic ? "the static" : "the instance") +
                                                        " member '" + hit.owner + "." + m.name + "' as " +
                                                        (isStatic ? "a static" : "an instance") + " member — rename "
                                                        "it; Polyglot has no member hiding (" +
                                                        (isStatic && hitStatic
                                                             ? "C# hides a static non-virtually, CS0108; TS requires "
                                                               "'override', TS4114"
                                                             : "PHP refuses a static/instance clash") + ")");
                            break;
                        }
                        const bool bothMethods = m.kind == MemberKind::Method && hit.m->kind == MemberKind::Method;
                        diags_.error(m.namePos, "'" + m.name + "' redeclares a member inherited from '" + hit.owner + "' — " +
                                                    (bothMethods && hit.m->isOpen
                                                         ? std::string("mark it 'override'")
                                                         : std::string("rename it; Polyglot has no member hiding (C# would "
                                                                       "hide it non-virtually, the other targets override it)")));
                        break;
                    }
                    continue;
                }
                bool inBaseClass = false, baseOpen = false, inInterface = false;
                for (const auto& b : bases) {
                    if (b.kind != TypeRef::Kind::Named || b.name.empty()) continue;
                    if (interfaceNames_.count(b.name)) {
                        if (interfaceHasMethod(b.name, m.name)) inInterface = true;
                    } else {
                        std::vector<const MemberInfo*> cands;
                        gatherMethods(b.name, m.name, cands);
                        for (const MemberInfo* c : cands) { inBaseClass = true; if (c->isOpen) baseOpen = true; }
                    }
                }
                if (inBaseClass) {
                    if (!baseOpen)
                        diags_.error(m.namePos, "'" + m.name + "' overrides a base-class member that is not "
                                                "'open'/'abstract'");
                } else if (inInterface) {
                    diags_.error(m.namePos, "implementing an interface method takes no 'override' — remove it "
                                            "('override' marks only an override of an open/abstract base-class member)");
                } else {
                    diags_.error(m.namePos, "'override' on '" + m.name + "' has no base-class member to override");
                }
            }
        };
        for (const auto& d : u.records) checkType(d.name, d.namePos, d.bases, d.members, "record");
        for (const auto& d : u.classes) if (!d.isExtern) checkType(d.name, d.namePos, d.bases, d.members, "class");
    }

    // `Awaitable<T>` — the type of an in-flight async result. Compile-time-only (§4.7): the author never
    // writes it (they write the unwrapped `T` and `await` to get it back); each backend synthesizes the real
    // wrapper (`Task<T>`/`Promise<T>`) from the callee's `isAsync` flag, so this name never reaches emission.
    static TypeRef awaitable(const TypeRef& inner) {
        TypeRef a; a.kind = TypeRef::Kind::Named; a.name = "Awaitable"; a.args.push_back(inner); return a;
    }
    static bool isAwaitable(const TypeRef& t) {
        return t.kind == TypeRef::Kind::Named && t.name == "Awaitable";
    }

    // ---- type resolution (P4-1) ----
    void pushGenerics(const std::vector<GenericParam>& gs) { for (const auto& g : gs) genericsInScope_.insert(g.name); }
    void popGenerics(const std::vector<GenericParam>& gs) { for (const auto& g : gs) genericsInScope_.erase(g.name); }
    void resolveTypeRef(const TypeRef& t, SourcePos pos) {
        switch (t.kind) {
            case TypeRef::Kind::Named:
                if (!t.name.empty()) {
                    if (const char* why = refusedReason(t.name)) diags_.error(pos, why);
                    else if (!isBuiltinType(t.name) && !genericsInScope_.count(t.name) && !typeNames_.count(t.name))
                        diags_.error(pos, "unknown type '" + t.name + "'");
                }
                for (const auto& a : t.args) resolveTypeRef(a, pos);
                checkTypeArgBounds(t.name, t.args, pos); // P40 slice 7: class type-param bounds
                break;
            case TypeRef::Kind::Tuple:
                for (const auto& a : t.args) resolveTypeRef(a, pos);
                break;
            case TypeRef::Kind::Function:
                for (const auto& a : t.args) resolveTypeRef(a, pos);
                for (const auto& r : t.ret) resolveTypeRef(r, pos);
                break;
        }
    }
    void resolveParams(const std::vector<Param>& ps, SourcePos fb) { for (const auto& p : ps) if (!p.type.absent()) resolveTypeRef(p.type, p.pos.line ? p.pos : fb); }
    void resolveBounds(const std::vector<GenericParam>& gs, SourcePos pos) { for (const auto& g : gs) for (const auto& b : g.bounds) resolveTypeRef(b, pos); }
    void resolveMembers(const std::vector<Member>& ms) {
        for (const auto& m : ms) {
            pushGenerics(m.generics); resolveBounds(m.generics, m.pos);
            if (m.kind == MemberKind::Field || m.kind == MemberKind::Const || m.kind == MemberKind::Property) {
                if (!m.type.absent()) resolveTypeRef(m.type, m.pos);
            } else { resolveParams(m.params, m.pos); resolveTypeRef(m.returnType, m.pos); }
            popGenerics(m.generics);
        }
    }
    void resolveAllTypes(const CompilationUnit& u) {
        for (const auto& fn : u.functions) { pushGenerics(fn.generics); resolveBounds(fn.generics, fn.pos); resolveParams(fn.params, fn.pos); resolveTypeRef(fn.returnType, fn.pos); popGenerics(fn.generics); }
        for (const auto& d : u.records)    { pushGenerics(d.generics); resolveBounds(d.generics, d.pos); resolveParams(d.fields, d.pos); for (const auto& b : d.bases) resolveTypeRef(b, d.pos); resolveMembers(d.members); popGenerics(d.generics); }
        for (const auto& d : u.classes)    { pushGenerics(d.generics); resolveBounds(d.generics, d.pos); for (const auto& b : d.bases) resolveTypeRef(b, d.pos); resolveMembers(d.members); popGenerics(d.generics); }
        for (const auto& d : u.interfaces) { pushGenerics(d.generics); resolveBounds(d.generics, d.pos); for (const auto& b : d.bases) resolveTypeRef(b, d.pos); resolveMembers(d.members); popGenerics(d.generics); }
        for (const auto& d : u.unions) { pushGenerics(d.generics); resolveBounds(d.generics, d.pos); for (const auto& c : d.cases) resolveParams(c.params, c.pos); popGenerics(d.generics); }
        for (const auto& d : u.extensions) { pushGenerics(d.generics); resolveBounds(d.generics, d.pos); resolveTypeRef(d.receiver, d.pos); resolveParams(d.params, d.pos); resolveTypeRef(d.returnType, d.pos); popGenerics(d.generics); }
        for (const auto& v : u.values)     if (v.hasType) resolveTypeRef(v.type, v.pos);
    }

    // P40 slice 7: a default that is filled in AT THE CALL (an extension's — bound or not — and a binding
    // member's, whose template has no native default to fall back on) is re-evaluated in the caller's scope,
    // so it must mean the same thing there: a literal (optionally negated) or `null`. It is type-checked
    // against its parameter once, here; lowering re-lowers the checked expression at every call.
    // A float constant expression (`1.0 / 60.0`, `-2.5 * 4.0`): folded at compile time so a default stays a plain
    // literal on every target (a PHP default may not call a helper; Python would evaluate it once). Only `+ - * /`
    // over float literals (and int literals inside them) fold; an infinity or NaN result does not.
    // P40 review r4 #11: the fold follows the expression's real typed semantics — an int-only subexpression is
    // i32 arithmetic (`1 / 2` is 0, as `print(1 / 2 + 0.5)` prints 0.5), widened to f64 only where it meets a
    // float. An i32 result out of range, or an integer division by zero, is not folded (the default is then
    // refused as non-constant). A non-finite result — at a literal (`1e400`), a negation or an operator — is
    // reported as NonFinite: no target can spell it as a constant default.
    enum class Fold { No, Ok, NonFinite };
    struct FoldVal { bool isInt = false; long long i = 0; double d = 0; double asDouble() const { return isInt ? static_cast<double>(i) : d; } };
    static bool finite(double d) { return d == d && d - d == 0; }
    static bool inI32(long long x) { return x >= -2147483648LL && x <= 2147483647LL; }
    static Fold foldConst(const Expr* e, FoldVal& v, bool& sawFloat) {
        if (!e) return Fold::No;
        if (e->kind == ExprKind::FloatLit) {
            sawFloat = true; v.isInt = false; v.d = std::strtod(e->text.c_str(), nullptr);
            return finite(v.d) ? Fold::Ok : Fold::NonFinite;
        }
        if (e->kind == ExprKind::IntLit) {
            if (e->text.empty() || e->text.size() > 10) return Fold::No;
            for (char c : e->text) if (!std::isdigit(static_cast<unsigned char>(c))) return Fold::No;
            v.isInt = true; v.i = std::strtoll(e->text.c_str(), nullptr, 10);
            return inI32(v.i) ? Fold::Ok : Fold::No;
        }
        if (e->kind == ExprKind::Unary && e->text == "-") {
            Fold f = foldConst(e->lhs.get(), v, sawFloat);
            if (f != Fold::Ok) return f;
            if (v.isInt) { v.i = -v.i; return inI32(v.i) ? Fold::Ok : Fold::No; }
            v.d = -v.d;
            return Fold::Ok;
        }
        if (e->kind == ExprKind::Binary && (e->text == "+" || e->text == "-" || e->text == "*" || e->text == "/")) {
            FoldVal a, b;
            Fold fa = foldConst(e->lhs.get(), a, sawFloat);
            if (fa != Fold::Ok) return fa;
            Fold fb = foldConst(e->rhs.get(), b, sawFloat);
            if (fb != Fold::Ok) return fb;
            const char op = e->text[0];
            if (a.isInt && b.isInt) {
                v.isInt = true;
                if (op == '/') { if (b.i == 0) return Fold::No; v.i = a.i / b.i; } // C-style truncation, as i32 `/`
                else v.i = op == '+' ? a.i + b.i : op == '-' ? a.i - b.i : a.i * b.i;
                return inI32(v.i) ? Fold::Ok : Fold::No;
            }
            const double x = a.asDouble(), y = b.asDouble();
            v.isInt = false;
            v.d = op == '+' ? x + y : op == '-' ? x - y : op == '*' ? x * y : x / y;
            return finite(v.d) ? Fold::Ok : Fold::NonFinite;
        }
        return Fold::No;
    }

    // P40 review r4 (part 2): an f32 constant expression (`0.1f + 0.2f`) — every float literal f32-suffixed — folds
    // in single precision, each operation rounded to f32 as C# folds it, to an f32 literal. (The f64 fold below read
    // `0.1f` as the double 0.1 and produced an f64 literal, refused as "f64 to f32".)
    static bool foldF32(const Expr* e, float& v, bool* nonFinite = nullptr) { // r5: nonFinite = an f32 overflow
        if (!e) return false;
        if (e->kind == ExprKind::FloatLit) {
            if (floatLitType(e->text) != "f32") return false;
            std::string d;
            for (char c : e->text) if (c != '_') d += c;
            if (d.size() >= 3 && d.compare(d.size() - 3, 3, "f32") == 0) d.resize(d.size() - 3);
            else if (!d.empty() && d.back() == 'f') d.pop_back();
            v = std::strtof(d.c_str(), nullptr);
            if (!(v == v && v - v == 0) && nonFinite) *nonFinite = true;
            return v == v && v - v == 0;
        }
        if (e->kind == ExprKind::Unary && e->text == "-") { if (!foldF32(e->lhs.get(), v, nonFinite)) return false; v = -v; return true; }
        if (e->kind == ExprKind::Binary && (e->text == "+" || e->text == "-" || e->text == "*" || e->text == "/")) {
            float a, b;
            if (!foldF32(e->lhs.get(), a, nonFinite) || !foldF32(e->rhs.get(), b, nonFinite)) return false;
            volatile float r = e->text == "+" ? a + b : e->text == "-" ? a - b : e->text == "*" ? a * b : a / b;
            v = r;
            if (!(v == v && v - v == 0) && nonFinite) *nonFinite = true;
            return v == v && v - v == 0; // finite
        }
        return false;
    }

    void checkCallSiteDefaults(std::vector<Param>& ps, const std::string& what) {
        for (auto& p : ps) {
            if (!p.hasDefault || !p.defaultValue) continue;
            if (p.defaultValue->kind == ExprKind::FloatLit) { // a bare literal is kept as written, if finite
                FoldVal lv; bool sf = false;
                if (foldConst(p.defaultValue.get(), lv, sf) == Fold::NonFinite) {
                    diags_.error(p.pos.line ? p.pos : p.defaultValue->pos,
                                 "the default of '" + p.name + "' in " + what + " is not a finite number — no "
                                 "target can spell an infinity or NaN as a constant default");
                    continue;
                }
            }
            bool foldedF32 = false; // r4: an all-f32 default folded in single precision (skip the f64 fold)
            if (p.defaultValue->kind == ExprKind::Binary || p.defaultValue->kind == ExprKind::Unary) {
                float f32v = 0;
                bool f32Overflow = false;
                if (p.defaultValue->kind == ExprKind::Binary && !foldF32(p.defaultValue.get(), f32v, &f32Overflow) &&
                    f32Overflow) { // review r5 (part 2): it was refused as "f64 to f32" (the f64 fold is finite)
                    diags_.error(p.pos.line ? p.pos : p.defaultValue->pos,
                                 "the default of '" + p.name + "' in " + what + " is not a finite f32 — it overflows "
                                 "single precision; no target can spell an infinity as a constant default");
                    continue;
                }
                if (p.defaultValue->kind == ExprKind::Binary && foldF32(p.defaultValue.get(), f32v)) {
                    foldedF32 = true;
                    std::string txt;
                    for (int prec = 6; prec <= 9; ++prec) { // the shortest text that round-trips the float
                        char buf[64];
                        std::snprintf(buf, sizeof buf, "%.*g", prec, static_cast<double>(std::fabs(f32v)));
                        txt = buf;
                        if (std::strtof(buf, nullptr) == std::fabs(f32v)) break;
                    }
                    if (txt.find_first_of(".eE") == std::string::npos) txt += ".0";
                    auto lit = std::make_unique<Expr>();
                    lit->kind = ExprKind::FloatLit; lit->pos = p.defaultValue->pos; lit->text = txt + "f";
                    if (std::signbit(f32v)) {
                        auto neg = std::make_unique<Expr>();
                        neg->kind = ExprKind::Unary; neg->text = "-"; neg->pos = lit->pos; neg->lhs = std::move(lit);
                        p.defaultValue = std::move(neg);
                    } else {
                        p.defaultValue = std::move(lit);
                    }
                }
            }
            if (!foldedF32 && (p.defaultValue->kind == ExprKind::Binary || p.defaultValue->kind == ExprKind::Unary)) {
                FoldVal fold; bool sawFloat = false;
                const Fold folded = foldConst(p.defaultValue.get(), fold, sawFloat);
                if (folded == Fold::NonFinite) {
                    diags_.error(p.pos.line ? p.pos : p.defaultValue->pos,
                                 "the default of '" + p.name + "' in " + what + " is not a finite number — no "
                                 "target can spell an infinity or NaN as a constant default");
                    continue;
                }
                const double fv = fold.asDouble();
                if (folded == Fold::Ok && sawFloat) {
                    std::string txt;
                    for (int prec = 15; prec <= 17; ++prec) { // the shortest text that round-trips the double
                        char buf[64];
                        std::snprintf(buf, sizeof buf, "%.*g", prec, std::fabs(fv));
                        txt = buf;
                        if (std::strtod(buf, nullptr) == std::fabs(fv)) break;
                    }
                    if (txt.find_first_of(".eE") == std::string::npos) txt += ".0";
                    auto lit = std::make_unique<Expr>();
                    lit->kind = ExprKind::FloatLit; lit->pos = p.defaultValue->pos; lit->text = txt;
                    if (std::signbit(fv)) { // r4: `-0.0` keeps its sign (`fv < 0` is false for it)
                        auto neg = std::make_unique<Expr>();
                        neg->kind = ExprKind::Unary; neg->text = "-"; neg->pos = lit->pos; neg->lhs = std::move(lit);
                        p.defaultValue = std::move(neg);
                    } else {
                        p.defaultValue = std::move(lit);
                    }
                }
            }
            const Expr* v = p.defaultValue.get();
            if (v->kind == ExprKind::Unary && v->text == "-" && v->lhs) v = v->lhs.get();
            const bool literal = v->kind == ExprKind::IntLit || v->kind == ExprKind::FloatLit ||
                                 v->kind == ExprKind::StringLit || v->kind == ExprKind::CharLit ||
                                 v->kind == ExprKind::BoolLit || v->kind == ExprKind::NullLit ||
                                 (v->kind == ExprKind::Member && v->lhs && v->lhs->kind == ExprKind::Name &&
                                  enumNames_.count(v->lhs->text)); // an enum case is a constant on every target
            if (!literal) {
                diags_.error(p.pos.line ? p.pos : p.defaultValue->pos,
                             "the default of '" + p.name + "' in " + what + " must be a literal (or null) — "
                             "C# and PHP accept only constants there, Python evaluates it once, and an extension fills it in at each call");
                continue;
            }
            checkExpr(*p.defaultValue);
            if (!p.type.absent()) checkConvert(p.defaultValue, p.type, "default of '" + p.name + "'");
        }
    }

    // P40 review A9: a custom `eq` must come with an author `hash` (and a `hash` only with an `eq`). Equal
    // values must hash equally (C# GetHashCode, Python __hash__ — a C# Dictionary/HashSet or a Python
    // dict/set key misbehaves otherwise), and no compiler can derive a hash from an arbitrary `eq` body: the
    // default structural hash disagrees with a custom eq, and a constant hash is consistent but degenerate.
    void checkEqHashPair(const std::string& typeName, const std::vector<Member>& members) {
        const Member* eq = nullptr;
        const Member* hash = nullptr;
        for (const auto& m : members) {
            if (m.kind != MemberKind::Operator) continue;
            if (m.name == "eq") eq = &m;
            else if (m.name == "hash") hash = &m;
        }
        if (eq && !hash)
            diags_.error(eq->pos, "'" + typeName + "' declares a custom `operator fn eq` but no `operator fn hash(): i32` "
                                  "— equal values must hash equally (C# GetHashCode, Python __hash__); hash exactly "
                                  "the state eq compares");
        if (hash && !eq)
            diags_.error(hash->pos, "`operator fn hash` on '" + typeName + "' needs a custom `operator fn eq` — "
                                    "without one, equality and its hash are the generated ones");
    }

    // ---- body checking ----
    void checkTypeBody(const std::string& typeName, const std::vector<GenericParam>& generics,
                       std::vector<Member>& members, std::vector<Param>* recordFields) {
        pushGenerics(generics);
        currentThis_ = tNamed(typeName);
        currentClass_ = typeName;
        const bool externType = externClassNames_.count(typeName) != 0;
        checkEqHashPair(typeName, members);
        if (recordFields) checkCallSiteDefaults(*recordFields, "record '" + typeName + "'");
        for (auto& m : members) {
            pushGenerics(m.generics);
            // P40 review r3 2(j): EVERY member's defaults (not only extern/bound ones) — they were never checked.
            (void)externType;
            if (m.kind == MemberKind::Method || m.kind == MemberKind::Constructor || m.kind == MemberKind::Operator)
                checkCallSiteDefaults(m.params, "'" + typeName + "." + m.name + "'");
            if (m.kind == MemberKind::Operator) { // P37 C: conversion-operator declaration rules
                if (m.name == "implicit")
                    diags_.error(m.pos, "Polyglot refuses implicit user conversions — an invisible "
                                        "call-site injection has no TS hook and hides behavior; declare "
                                        "`operator fn explicit(): T` and cast explicitly");
                else if (m.name == "explicit") {
                    if (!m.params.empty())
                        diags_.error(m.pos, "`operator fn explicit` takes no parameters — the conversion "
                                            "source is `this`, the target is the return type");
                    const std::string& tgt = m.returnType.name;
                    if (m.returnType.kind != TypeRef::Kind::Named || tgt.empty() || tgt == "unit" ||
                        tgt == "string" || tgt == "bool")
                        diags_.error(m.pos, "an explicit conversion targets a numeric or user type — "
                                            "string/bool conversions stay named methods (toString/truthiness "
                                            "semantics differ per target)");
                } else if (m.name == "hash") { // P40 review A9: the hash that agrees with a custom `eq`
                    if (!m.params.empty() || m.returnType.kind != TypeRef::Kind::Named ||
                        m.returnType.name != "i32" || m.returnType.nullable)
                        diags_.error(m.pos, "`operator fn hash` takes no parameters and returns i32: "
                                            "`operator fn hash(): i32`");
                } else if (!isOperatorMemberName(m.name)) {
                    diags_.error(m.pos, "'" + m.name + "' is not an operator — an `operator fn` is one of the "
                                        "fixed names of SPEC §6.1 (plus, minus, times, div, rem, eq, hash, lt, le, "
                                        "gt, ge, band, bor, bxor, shl, shr, neg, bnot, get, set, explicit; `implicit` is recognized and refused)");
                }
            }
            if (m.kind == MemberKind::Method || m.kind == MemberKind::Operator || m.kind == MemberKind::Constructor) {
                bool isStatic = false;
                for (const auto& mod : m.modifiers) if (mod == "static") isStatic = true;
                currentThis_ = isStatic ? tUnknown() : tNamed(typeName); // no `this` inside a static method
                currentReturn_ = (m.kind == MemberKind::Constructor) ? namedType("unit") : m.returnType;
                inAsync_ = m.isAsync;
                inCtor_ = m.kind == MemberKind::Constructor;
                if (isStatic && m.kind == MemberKind::Method) bodyKey_ = "st:" + typeName + "." + m.name; // review r4
                else if (m.kind == MemberKind::Method) bodyKey_ = "m:" + typeName + "." + m.name;         // review r5 #4
                else if (m.kind == MemberKind::Constructor) bodyKey_ = "ctor:" + typeName;              // review r5 #4
                scopeStart_ = m.namePos; scopeEnd_ = m.bodyEnd; // method locals scoped to this member body
                pushScope();
                if (recordFields) for (const auto& f : *recordFields) declare(f.name, f.type, false, f.pos);
                for (const auto& p : m.params) declare(p.name, p.type, false, p.pos);
                // D6: the one legal `super` — a top-level `super(…)` statement in a constructor body, the exact
                // shape lowering hoists into the base-constructor call. Every other `super` is refused.
                if (m.kind == MemberKind::Constructor)
                    for (std::size_t si = 0; si < m.body.size(); ++si) {
                        const auto& st = m.body[si];
                        if (st->kind == StmtKind::ExprStmt && st->value && st->value->kind == ExprKind::Call &&
                            st->value->lhs && st->value->lhs->kind == ExprKind::Super) {
                            allowedSupers_.insert(st->value->lhs.get());
                            // P40 review r5 #8: the base constructor runs before the constructor's first statement
                            // (SPEC §4.4) — C# `: base(…)` cannot see a local, and the other targets would run
                            // a statement written first after it. Every target needs `super(…)` first.
                            if (si != 0)
                                diags_.error(m.body[0]->pos, "a statement before `super(…)` in the constructor of '" +
                                                                 typeName + "' — the base constructor runs first on "
                                                                 "every target (C# `: base(…)` cannot see a local); "
                                                                 "make `super(…)` the first statement, computing its "
                                                                 "arguments inline or in a `static fn`");
                        }
                    }
                if (m.hasBody && m.exprBodied && m.exprBody) { checkExpr(*m.exprBody); if (m.kind != MemberKind::Constructor) checkConvert(m.exprBody, m.returnType, "method body"); }
                else if (m.hasBody) checkBlock(m.body);
                popScope();
                scopeStart_ = {}; scopeEnd_ = {};
                inAsync_ = false;
                inCtor_ = false;
                bodyKey_.clear();
                currentThis_ = tNamed(typeName);
            } else if ((m.kind == MemberKind::Field || m.kind == MemberKind::Const) && m.init) {
                // P40 follow-up: a field initializer is an expression like any other — it was never checked, so it
                // carried no types: no i32 overflow masking (`static let D: i32 = K.X * 3` wrapped on C# only),
                // no conversion into the declared type, no name/type errors. A static one has no `this`.
                bool isStatic = m.kind == MemberKind::Const;
                for (const auto& mod : m.modifiers) if (mod == "static") isStatic = true;
                currentThis_ = isStatic ? tUnknown() : tNamed(typeName);
                if (isStatic) { initDeclPos_ = m.pos; initDeclName_ = typeName + "." + m.name; initClass_ = typeName; }
                else bodyKey_ = "ctor:" + typeName; // review r5 #4: an instance field initializer runs in the constructor
                pushScope();
                checkExpr(*m.init);
                initDeclPos_ = {0, 0, 0};
                initClass_.clear();
                bodyKey_.clear();
                checkConvert(m.init, m.type, "initializer of field '" + m.name + "'");
                popScope();
                currentThis_ = tNamed(typeName);
            } else if (m.kind == MemberKind::Property && (m.init || !m.body.empty() || m.hasSetter)) {
                currentReturn_ = m.type;
                pushScope();
                bodyKey_ = "m:" + typeName + "." + m.name; // review r5 #4: a getter runs where the property is read
                if (recordFields) for (const auto& f : *recordFields) declare(f.name, f.type, false, f.pos);
                if (m.init) { checkExpr(*m.init); checkConvert(m.init, m.type, "property getter"); }
                else if (!m.body.empty()) checkBlock(m.body); // block-bodied getter (accessor block, #39c)
                bodyKey_.clear();
                popScope();
                if (m.hasSetter) { // the setter body sees `this`, the record fields, and the value param
                    currentReturn_ = namedType("unit");
                    pushScope();
                    if (recordFields) for (const auto& f : *recordFields) declare(f.name, f.type, false, f.pos);
                    declare(m.setterParam, m.type, false, m.pos);
                    checkBlock(m.setterBody);
                    popScope();
                }
            }
            popGenerics(m.generics);
        }
        currentThis_ = tUnknown();
        currentClass_.clear();
        popGenerics(generics);
    }

    void checkBlock(std::vector<StmtPtr>& body) {
        // A top-level body (fn/method/ctor/accessor/extension): collect the names its lambdas assign, so a
        // closure-reassignable mutable local is never narrowed (P40 D14).
        std::unordered_set<std::string> savedClosureAssigned;
        if (bodyDepth_ == 0) {
            savedClosureAssigned = std::move(closureAssigned_);
            closureAssigned_.clear();
            collectAssigned(body, false, /*lambdaOnly=*/true, closureAssigned_);
        }
        ++bodyDepth_;
        pushScope();
        blockIsBindings_.emplace_back();
        checkStmts(body);
        blockIsBindings_.pop_back();
        popScope();
        --bodyDepth_;
        if (bodyDepth_ == 0) closureAssigned_ = std::move(savedClosureAssigned);
    }

    // Wrap an expression in an implicit widening cast (mutates the AST so lowering/emit convert it).
    void coerce(ExprPtr& slot, const TypeRef& target) {
        auto cast = std::make_unique<Expr>();
        cast->kind = ExprKind::Cast;
        cast->pos = slot->pos;
        cast->castType = target;
        cast->type = target;
        cast->lhs = std::move(slot);
        slot = std::move(cast);
    }

    // A payload-free union case (e.g. `None`) is typed `U<unbound>` bottom-up, since a bare case carries no
    // value to infer the union's type args from. When it flows into a known `U<concrete>` slot, stamp the
    // concrete type so construction emits the right args (C# `new None<int>()`). Recurse through if/match so a
    // bare case in a branch instantiates too (`if b { Some(x) } else { None }` returned as Option<i32>).
    void instantiateBareCases(ExprPtr& slot, const TypeRef& expected) {
        if (!slot || expected.kind != TypeRef::Kind::Named || expected.args.empty()) return;
        switch (slot->kind) {
            case ExprKind::Name: {
                auto owner = unionCaseOwner_.find(slot->text);
                if (owner != unionCaseOwner_.end() && owner->second == expected.name) slot->type = expected;
                break;
            }
            case ExprKind::IfExpr:
                instantiateBareCases(slot->rhs, expected);   // then-branch
                instantiateBareCases(slot->extra, expected); // else-branch
                break;
            case ExprKind::Match:
                for (auto& arm : slot->arms) if (!arm.bodyIsBlock && arm.body) instantiateBareCases(arm.body, expected);
                break;
            default: break;
        }
    }

    // Synthesize a `Some(inner)` construction around `slot`, typed `opt` (lower turns the Call into a
    // MakeCase). Used to wrap a bare `T` value flowing into an `Option<T>` slot (the `T?` sugar).
    void wrapSome(ExprPtr& slot, const TypeRef& opt) {
        auto call = std::make_unique<Expr>();
        call->kind = ExprKind::Call; call->pos = slot->pos; call->type = opt;
        auto callee = std::make_unique<Expr>(); callee->kind = ExprKind::Name; callee->text = "Some"; callee->pos = slot->pos;
        call->lhs = std::move(callee);
        call->args.push_back(std::move(slot));
        slot = std::move(call);
    }
    // Coerce a value into an `Option<X>` slot (the `T?`-sugar leaf rules), recursing through if/match so each
    // branch is coerced: `null` -> `None`, a bare `None` -> typed None, an already-Option value -> instantiate,
    // a bare `X` value -> `Some(value)`.
    void coerceToOptional(ExprPtr& slot, const TypeRef& opt) {
        if (!slot) return;
        if (slot->kind == ExprKind::IfExpr) { coerceToOptional(slot->rhs, opt); coerceToOptional(slot->extra, opt); slot->type = opt; return; }
        if (slot->kind == ExprKind::Match)  { for (auto& a : slot->arms) if (!a.bodyIsBlock && a.body) coerceToOptional(a.body, opt); slot->type = opt; return; }
        if (slot->kind == ExprKind::NullLit) { auto none = std::make_unique<Expr>(); none->kind = ExprKind::Name; none->text = "None"; none->pos = slot->pos; none->type = opt; slot = std::move(none); return; }
        if (slot->type.kind == TypeRef::Kind::Named && slot->type.name == "Option") { instantiateBareCases(slot, opt); return; } // already optional (or a bare None)
        // A NATIVE nullable (`string?` — a concrete type's null-carrying form) flowing into `Option<string>`:
        // its null is the None, not a present value, so it is not `Some(value)` (that made `Some(null)`).
        // The representation conversion is an implicit Cast that lowering expands into
        // `match v { null => None, x => Some(x) }` (SPEC §4.4, the T?-representation boundary). Only when the
        // element itself is non-nullable: into `Option<string?>`, a null IS a present value (`Some(null)`).
        if (isNativeNullable(slot->type) && !opt.args[0].nullable) { coerce(slot, opt); return; }
        wrapSome(slot, opt); // a present value of the element type
    }
    // `Option<X>` — the representation a user generic's `T?` has at a concrete use site (SPEC §4.4).
    static bool isOptionType(const TypeRef& t) {
        return t.kind == TypeRef::Kind::Named && t.name == "Option" && t.args.size() == 1 && !t.nullable;
    }
    // A concrete type's native nullable form (`string?`, `i32?`, `Box?`) — NOT the untyped null literal, and
    // never an Option (whose absence is the None case, not a null).
    static bool isNativeNullable(const TypeRef& t) {
        return t.nullable && !(t.kind == TypeRef::Kind::Named && (t.name.empty() || t.name == "Option"));
    }
    // The T?-representation boundary, Option -> native (SPEC §4.4): an `Option<X>` value that meets a
    // native-nullable context converts to `X?` (lowering expands the Cast into
    // `match o { Some(v) => v, None => null }`). Returns false (and diagnoses) when the conversion would lose
    // information: `Option<X?>` merges `Some(null)` and `None` into one null.
    bool optionToNative(ExprPtr& slot, const std::string& ctx) {
        const TypeRef& elem = slot->type.args[0];
        if (elem.nullable) {
            diags_.error(slot->pos, "cannot convert '" + showTy(slot->type) + "' to a native nullable (" + ctx +
                                        "): its element is itself nullable, so 'Some(null)' and 'None' would "
                                        "both become null; match on it instead");
            return false;
        }
        TypeRef nat = elem;
        nat.nullable = true;
        coerce(slot, nat);
        return true;
    }

    // A value flowing into a typed slot (let/assign/return/argument): widen losslessly if possible, else
    // a numeric mismatch demands an explicit cast and any other known-scalar mismatch is a type error.
    // A type with no unbound type parameter in it — every name a scalar, a declared type/union/enum, or a type
    // parameter of the scope being checked. A callee's own still-open `U` is not concrete: converting into it
    // would stamp the parameter itself into the emitted code (`new Some<U>(…)`).
    bool concreteType(const TypeRef& t) const {
        if (t.kind == TypeRef::Kind::Named) {
            if (t.name.empty()) return false;
            if (!(isBuiltinType(t.name) || t.name == "unit" || types_.count(t.name) ||
                  unionAllCases_.count(t.name) || enumAllCases_.count(t.name) || genericsInScope_.count(t.name)))
                return false;
        }
        for (const auto& a : t.args) if (!concreteType(a)) return false;
        for (const auto& r : t.ret) if (!concreteType(r)) return false;
        return true;
    }
    // P40 review A7: a lambda flowing into a function-typed slot — its RESULT flows into the slot's return
    // type, exactly as a `return` value flows into a function's declared return type. So an `Option<X>` result
    // meeting a `X?` return converts (the T?-representation boundary, SPEC §4.4), an `i32` result widens into
    // `f64`, and a mismatch is refused — before, the lambda was opaque (typed Unknown) and nothing converted:
    // C# CS0029, TS `[object Object]`. Every `return` of a block lambda (not of a lambda nested in it) counts.
    void convertLambdaResult(Expr& lam, const TypeRef& ret) {
        if (isUnitT(ret)) {
            // P40 review r3 item 6: a lambda flowing into a `(…) => unit` slot produces no value. A block's
            // `return 5` emitted C# CS8030 (an Action cannot return a value); an expression body that is not a
            // call (`() => k + 1`) is not a statement, CS0201. A call body is fine — its result is discarded.
            if (!lam.flag) {
                const Expr* b = lam.lhs.get();
                while (b && b->kind == ExprKind::Await) b = b->lhs.get();
                if (b && b->kind != ExprKind::Call)
                    diags_.error(lam.pos, "this lambda flows into a function type returning unit, so its body must "
                                          "be a call or a block — a value expression would be computed and discarded");
                // P40 review r4 item 18: a call body's value is discarded on every target — lowering spells a body
                // that is not a real invocation there (a bound property read, a lookup) as a discarded statement.
                lam.lambdaUnitResult = true;
                return;
            }
            std::function<void(const std::vector<StmtPtr>&)> walkUnit = [&](const std::vector<StmtPtr>& body) {
                for (const auto& s : body) {
                    if (!s) continue;
                    if (s->kind == StmtKind::Return && s->value)
                        diags_.error(s->pos, "this lambda flows into a function type returning unit; 'return' takes no value");
                    walkUnit(s->thenBody);
                    walkUnit(s->elseBody);
                    for (const auto& c : s->catches) walkUnit(c.body);
                    walkUnit(s->finallyBody);
                    for (const Expr* x : {s->value.get(), s->target.get()})
                        if (x && x->kind == ExprKind::Match)
                            for (const auto& arm : x->arms) walkUnit(arm.block);
                }
            };
            walkUnit(lam.block);
            return;
        }
        if (!concreteType(ret)) return;
        if (!lam.flag) {
            if (lam.lhs) checkConvert(lam.lhs, ret, "lambda result");
            return;
        }
        // The block must produce that result on every path (checked by the flow pass, like a function body). An
        // async result of unit (`Awaitable<unit>`) is a plain completion, not a value.
        if (!(isAwaitable(ret) && (ret.args.empty() || isUnitT(ret.args[0])))) lam.lambdaNeedsValue = true;
        std::function<void(std::vector<StmtPtr>&)> walk = [&](std::vector<StmtPtr>& body) {
            for (auto& s : body) {
                if (!s) continue;
                if (s->kind == StmtKind::Return && s->value) checkConvert(s->value, ret, "lambda result");
                walk(s->thenBody);
                walk(s->elseBody);
                for (auto& c : s->catches) walk(c.body);
                walk(s->finallyBody);
                for (Expr* x : {s->value.get(), s->target.get()}) // a statement-position match's arm blocks
                    if (x && x->kind == ExprKind::Match)
                        for (auto& arm : x->arms) walk(arm.block);
            }
        };
        walk(lam.block);
    }

    // P40 review r5 (part 2): a function VALUE converts to a function type whose parameters are SUBtypes of its own
    // (contravariance) and whose result is a SUPERtype of its own (covariance), for user classes and interfaces —
    // `(Animal) => string` where `(Dog) => string` is expected, `() => Dog` where `() => Animal` is. Every target
    // accepts it: C# delegate variance (`Func<in T, out R>`, `Action<in T>`; class/interface types are reference
    // types), TS function-type variance under strictFunctionTypes, and Python/PHP are untyped. Anything else —
    // scalars (no variance for value types), nullability, generics — must still match exactly.
    bool functionVariant(const TypeRef& from, const TypeRef& to) const {
        if (from.args.size() != to.args.size() || from.ret.size() != to.ret.size()) return false;
        auto sub = [&](const TypeRef& a, const TypeRef& b) { // a is b, or a nominal class/interface subtype of b
            if (showTy(a) == showTy(b)) return true;
            return a.args.empty() && b.args.empty() && a.nullable == b.nullable && nominalKnown(a) &&
                   nominalKnown(b) && !recordNames_.count(a.name) && !recordNames_.count(b.name) &&
                   inheritsFrom(a.name, b.name);
        };
        for (std::size_t i = 0; i < from.args.size(); ++i)
            if (!sub(to.args[i], from.args[i])) return false;
        for (std::size_t i = 0; i < from.ret.size(); ++i)
            if (!sub(from.ret[i], to.ret[i])) return false;
        return true;
    }

    // A `.pg` spelling of a name/`this`/member path for a hint (anything else is elided).
    static std::string exprText(const Expr& e) {
        switch (e.kind) {
            case ExprKind::Name: return e.text;
            case ExprKind::This: return "this";
            case ExprKind::Member: return (e.lhs ? exprText(*e.lhs) : std::string("…")) + (e.flag ? "?." : ".") + e.text;
            default: return "…";
        }
    }

    // P40 review r5 item 18: a lambda flowing into a function slot must take exactly the slot's parameters — its
    // arity, and each ANNOTATED parameter's type (an unannotated one takes the slot's). Only the result used to be
    // checked, so `(x: i32) => …` where `(i32?) => i32` is expected passed (C# CS1661, TS2345, a Python TypeError
    // on `null + 1`), as did a one-parameter lambda for a two-parameter slot (CS1593).
    bool lambdaParamsMatch(const Expr& lam, const TypeRef& target) {
        TypeRef shown = target;
        shown.nullable = false;
        if (lam.params.size() != target.args.size()) {
            diags_.error(lam.pos, "this lambda takes " + std::to_string(lam.params.size()) + " parameter(s), but the "
                                  "function type '" + showTy(shown) + "' it flows into passes " +
                                  std::to_string(target.args.size()));
            return false;
        }
        bool ok = true;
        for (std::size_t i = 0; i < lam.params.size(); ++i) {
            const auto& p = lam.params[i];
            if (p.type.absent() || !concreteType(p.type) || !concreteType(target.args[i])) continue;
            if (showTy(p.type) == showTy(target.args[i])) continue;
            diags_.error(p.pos, "lambda parameter '" + p.name + "' is declared '" + showTy(p.type) + "', but the "
                                "function type '" + showTy(shown) + "' passes '" + showTy(target.args[i]) +
                                "' — a lambda's parameter types must match exactly (or leave it unannotated)");
            ok = false;
        }
        return ok;
    }

    // P40 review r5 item 18: the type ARGUMENTS of a non-literal value must agree with the slot's. A generic
    // collection or user generic is invariant on C# (`List<int?>` is not a `List<int>`, CS0029; a `string?`/
    // `string` argument mismatch is CS8619 under the floor), and a tuple element that may be null cannot go where
    // one may not (CS8629/TS2322). `Iterable` reads only, so only its element's nullability is judged. Returns the
    // reason when they disagree, "" when they agree (or a side is still open).
    std::string typeArgsDisagree(const TypeRef& from, const TypeRef& to) const {
        auto nullIntoNonNull = [&](const TypeRef& f, const TypeRef& t) {
            return f.nullable && !t.nullable && nullRefusingSlot(t);
        };
        if (from.kind == TypeRef::Kind::Tuple && to.kind == TypeRef::Kind::Tuple && from.args.size() == to.args.size()) {
            for (std::size_t i = 0; i < from.args.size(); ++i) {
                if (nullIntoNonNull(from.args[i], to.args[i]))
                    return "element " + std::to_string(i + 1) + " may be null";
                if (std::string r = typeArgsDisagree(from.args[i], to.args[i]); !r.empty()) return r;
            }
            return "";
        }
        if (from.kind != TypeRef::Kind::Named || to.kind != TypeRef::Kind::Named || to.args.empty()) return "";
        if (to.name == "Iterable" && to.args.size() == 1 && from.args.size() == 1 &&
            (from.name == "List" || from.name == "Array" || from.name == "Iterable")) {
            if (nullIntoNonNull(from.args[0], to.args[0])) return "its elements may be null";
            return "";
        }
        if (from.name != to.name || from.args.size() != to.args.size() || to.name == "Option" || to.name == "Iterable")
            return "";
        for (std::size_t i = 0; i < from.args.size(); ++i)
            if (concreteType(from.args[i]) && concreteType(to.args[i]) && showTy(from.args[i]) != showTy(to.args[i]))
                return "type arguments must match exactly ('" + showTy(from.args[i]) + "' is not '" +
                       showTy(to.args[i]) + "'; a generic type is invariant)";
        return "";
    }

    // The value of a built-in compound assignment `t op= v` types exactly as the binary `t op v` would
    // (P40 review r3): for most operators it converts into the slot, but a shift's COUNT is always an i32
    // (the binary rule, G4) — converting it into an i64 slot emitted C# `h <<= 3L`, CS0019.
    void checkCompoundValue(Stmt& s, const TypeRef& slot, const std::string& ctx) {
        if (s.op == ">>>=") { // refused like the binary `>>>` (see checkBinary)
            diags_.error(s.pos, "'>>>=' (unsigned shift) is not supported — the targets disagree on its result "
                                "type; shift an unsigned value with '>>=' instead");
            return;
        }
        if (s.op == "<<=" || s.op == ">>=") {
            if (!slot.name.empty() && !(isSignedInt(slot.name) || isUnsignedInt(slot.name))) {
                diags_.error(s.pos, "'" + s.op + "' expects an integer target, found " + showTy(slot));
                return;
            }
            const TypeRef& vt = s.value->type;
            if (vt.name != "i32" || vt.nullable) {
                if ((isSignedInt(vt.name) || isUnsignedInt(vt.name)) && !vt.nullable)
                    checkConvert(s.value, tNamed("i32"), "shift count");
                else
                    diags_.error(s.pos, "'" + s.op + "' expects an i32 shift count, found " + showTy(vt));
            }
            return;
        }
        checkConvert(s.value, slot, ctx);
    }

    // P40 review r4 item 17: `t ??= v` assigns only when `t` is null. A target that cannot be null here (a non-null
    // type, or a narrowed local) makes it dead — and C# rejects it (CS0019) — so it is refused like `??`. An
    // optional generic's `T?` (an Option) has no native `??=`; the value converts into the nullable slot.
    void checkCoalesceAssign(Stmt& s, const TypeRef& readType, const TypeRef& slot, const std::string& ctx) {
        if (isOptionType(slot)) {
            diags_.error(s.pos, "'??=' on a value of type '" + showTy(slot) + "' (a generic 'T?') is not supported — "
                                "write 't = t ?? value'");
            return;
        }
        if (!readType.nullable && nullRefusingSlot(readType)) {
            diags_.error(s.pos, "the target of '??=' has type '" + showTy(readType) + "', which cannot be null here, "
                                "so the value is never assigned — remove the '??='");
            return;
        }
        checkConvert(s.value, slot, ctx);
    }

    void checkConvert(ExprPtr& slot, const TypeRef& target, const std::string& ctx) {
        if (!slot) return;
        if (slot->kind == ExprKind::Lambda && target.kind == TypeRef::Kind::Function && !target.ret.empty()) {
            if (!lambdaParamsMatch(*slot, target)) return;
            // P40 review r6 (part 2): an unannotated parameter takes the slot's type, and the body is typed again
            // with it — `mapS(s => s + "!")` typed `s + "!"` with an unknown `s`, so the `+` was not known to be a
            // string concatenation (PHP emitted `+`: a TypeError). The parameter stays unannotated in the output
            // (each target infers it from the same slot); only the body's types change.
            std::vector<std::size_t> filled;
            for (std::size_t j = 0; j < slot->params.size() && j < target.args.size(); ++j)
                if (slot->params[j].type.absent() && concreteType(target.args[j])) {
                    slot->params[j].type = target.args[j];
                    filled.push_back(j);
                }
            if (!filled.empty()) {
                checkLambda(*slot);
                for (std::size_t j : filled) slot->params[j].type = TypeRef{};
            }
            convertLambdaResult(*slot, target.ret[0]);
            return;
        }
        // An un-awaited async result (`Awaitable<T>`) used where a plain value is expected — e.g. `return f()`
        // inside an async fn — is the "forgot to await" mistake. Refuse it (as C#/TS do), naming the fix. §4.7
        if (isAwaitable(slot->type) && !isAwaitable(target)) {
            diags_.error(slot->pos, "this is an async result (Awaitable) — add 'await' before using it (" + ctx + ") (PRD §4.7)");
            return;
        }
        if (target.kind == TypeRef::Kind::Named && target.name == "Option" && !target.args.empty()) { coerceToOptional(slot, target); return; }
        // A list literal takes its element type from the target — so `[]` is `List<i32>`, not `List<unknown>`
        // (which emits invalid C# `List<object>`), and elements widen to the target element type. The same
        // literal coerces to a fixed-size `Array<T>` target (`var a: i32[] = [1, 2]`), which the emitter then
        // spells as an array (C# `new int[]{…}`) instead of a List.
        if (target.kind == TypeRef::Kind::Named && (target.name == "List" || target.name == "Array") &&
            !target.args.empty() && slot->kind == ExprKind::ListLit) {
            for (auto& el : slot->args) checkConvert(el, target.args[0], ctx);
            slot->type = target;
            return;
        }
        // P40 review r5 item 18: a tuple literal into a tuple slot converts element by element (`(maybe(), 1)` into
        // `(i32, i32)` was accepted: C# CS0029, TS2322).
        if (target.kind == TypeRef::Kind::Tuple && slot->kind == ExprKind::TupleLit && !target.nullable &&
            slot->args.size() == target.args.size()) {
            for (std::size_t i = 0; i < slot->args.size(); ++i) checkConvert(slot->args[i], target.args[i], ctx);
            for (std::size_t i = 0; i < slot->args.size(); ++i) slot->type.args[i] = slot->args[i]->type;
            return;
        }
        // P40 review r5 item 18: a `match` / `if` value flows into the slot ARM BY ARM — each arm is converted
        // (widened, null-checked, type-checked) on its own, as a `return` in each branch would be. Judging only
        // the joined type let a mismatched or nullable arm through (`match k { 1 => "one", _ => 2 }` into i32).
        if (slot->kind == ExprKind::Match) {
            bool any = false;
            for (auto& arm : slot->arms)
                if (!arm.bodyIsBlock && arm.body) { checkConvert(arm.body, target, ctx); any = true; }
            if (any) return;
        }
        if (slot->kind == ExprKind::IfExpr && slot->rhs && slot->extra) {
            checkConvert(slot->rhs, target, ctx);
            checkConvert(slot->extra, target, ctx);
            return;
        }
        instantiateBareCases(slot, target); // a contextually-typed bare union case (`None`) takes `target`
        // The T?-representation boundary (SPEC §4.4): a user generic's `T?` is an `Option<X>` at a concrete
        // use site, a distinct type from the native `X?`. Into a native-nullable slot it converts (the null is
        // the None); into a non-nullable slot it is refused, it may be absent (it used to pass silently, and
        // the targets then held a `{tag:"Some"}` object where a string was expected).
        if (isOptionType(slot->type) && !isOptionType(target)) {
            if (target.nullable) {
                if (!optionToNative(slot, ctx)) return;
            } else if (target.kind == TypeRef::Kind::Named && !target.name.empty() &&
                       !genericsInScope_.count(target.name) && target.name != "Iterable" &&
                       (scalarTyOf(target) != Ty::Unknown || knownType(target))) {
                diags_.error(slot->pos, "cannot convert '" + showTy(slot->type) + "' to '" + showTy(target) +
                                            "' (" + ctx + "): the value may be absent; unwrap it with "
                                            "'?? fallback', '!' or a match, or declare the slot '" +
                                            showNullable(target) + "'");
                return;
            }
        }
        // G3 (wave 2): an UNSUFFIXED integer literal flowing into an i64/u64 slot is retyped to the slot's
        // width — the literal IS a 64-bit value, so targets with a distinct 64-bit literal spelling emit it
        // exactly (TS `9007199254740993n`, C# `…L`). The widening-CAST path would instead emit a runtime
        // conversion of an already-parsed narrower value (TS `BigInt(9007199254740993)` rounds through an
        // f64 before BigInt ever sees it — a silent §3.C violation). Negative literals arrive as unary
        // minus over an IntLit; retype through the minus. A suffixed literal (`5i32`) keeps its width.
        if (target.kind == TypeRef::Kind::Named && (target.name == "i64" || target.name == "u64")) {
            Expr* lit = nullptr;
            if (slot->kind == ExprKind::IntLit) lit = slot.get();
            else if (slot->kind == ExprKind::Unary && slot->text == "-" && slot->lhs &&
                     slot->lhs->kind == ExprKind::IntLit && target.name == "i64")
                lit = slot->lhs.get();
            if (lit && lit->text.find_first_of("iu") == std::string::npos &&
                (literalFits64(lit->text, target.name == "u64") ||
                 (lit != slot.get() && literalMagnitude(lit->text) == 9223372036854775808ull))) { // r4: -2^63
                lit->type = target;
                slot->type = target;
                return;
            }
        }
        if (!intLiteralFits(*slot, target, ctx)) return;
        TypeRef from = slot->type;
        // P40 review r4 item 14: a value that may be null flowing into a slot that may not — `fn f(x: i32?): i32
        // => x`, `let w: i32 = x ?? y` with a nullable `y`, a bare `null` into an `i32`. Every target accepted
        // it (C# CS0266/CS8629, TS2322) or returned `null` from an `i32` function. Flow narrowing is respected
        // because a narrowed read is already typed non-null (checkName). A slot whose type is still open (a type
        // parameter, an unresolved hole, `Iterable`) is left alone: its instantiation decides.
        if (from.nullable && !target.nullable && nullRefusingSlot(target)) {
            const std::string shown = (from.kind == TypeRef::Kind::Named && from.name.empty()) ? std::string("null")
                                                                                                 : "'" + showTy(from) + "'";
            // P40 review r5 (part 2): a FIELD read is never narrowed (SPEC §4.3 — any call may reassign it between
            // the test and the read), so `if this.name != null { shout(this.name) }` needs the rule named.
            if (slot->kind == ExprKind::Member && !slot->flag && slot->lhs) {
                diags_.error(slot->pos, "cannot convert " + shown + " to '" + showTy(target) + "' (" + ctx +
                                            "): fields are not narrowed by a null test (SPEC §4.3); read it into a "
                                            "local first ('let v = " + exprText(*slot) + "', then test 'v') or assert "
                                            "it with '!'");
                return;
            }
            diags_.error(slot->pos, "cannot convert " + shown + " to '" + showTy(target) + "' (" + ctx +
                                        "): the value may be null; supply a fallback with '??', assert with '!', "
                                        "narrow it first ('if x != null'), or declare the slot '" + showNullable(target) + "'");
            return;
        }
        // P40 review r5 item 18: a bare type-parameter slot `T` of the scope being checked may be instantiated
        // with a non-null type, so `null`, a maybe-null value or a `T?` (an Option here, SPEC §4.4) cannot go
        // there — `x: T = null`, `fn f<T>(): T => null` (C# CS0403 / a `null` that a caller's `i32` then reads).
        if (target.kind == TypeRef::Kind::Named && !target.nullable && target.args.empty() &&
            genericsInScope_.count(target.name) &&
            (from.nullable || (isOptionType(from) && from.args[0].kind == TypeRef::Kind::Named &&
                               from.args[0].name == target.name && !from.args[0].nullable))) {
            const std::string shown = (from.kind == TypeRef::Kind::Named && from.name.empty()) ? std::string("null")
                                                                                                 : "'" + showTy(from) + "'";
            diags_.error(slot->pos, "cannot convert " + shown + " to '" + target.name + "' (" + ctx + "): the type "
                                    "parameter '" + target.name + "' may be a non-null type, so the value may not be "
                                    "absent — declare the slot '" + target.name + "?', or unwrap it with '??' or '!'");
            return;
        }
        if (std::string why = typeArgsDisagree(from, target); !why.empty()) {
            diags_.error(slot->pos, "cannot convert '" + showTy(from) + "' to '" + showTy(target) + "' (" + ctx +
                                        "): " + why);
            return;
        }
        // P40 review r4 item 18: a function VALUE (a named function, a function-typed local) flowing into a function
        // slot must have exactly that type — no target converts between delegate/function shapes the same way (C#
        // has no `Func<int>` -> `Action` conversion, CS0407/CS1503; TS accepts it). Lambdas are handled above.
        if (from.kind == TypeRef::Kind::Function && target.kind == TypeRef::Kind::Function &&
            concreteType(from) && concreteType(target)) {
            TypeRef f = from, t = target;
            f.nullable = t.nullable = false;
            if (showTy(f) != showTy(t) && !functionVariant(f, t)) {
                const bool toUnit = !target.ret.empty() && isUnitT(target.ret[0]) &&
                                    !(from.ret.empty() || isUnitT(from.ret[0]));
                diags_.error(slot->pos, "cannot convert a function of type '" + showTy(f) + "' to '" + showTy(t) +
                                            "' (" + ctx + ")" +
                                            (toUnit ? ": it returns a value the slot would discard — pass a lambda "
                                                      "that calls it ('(…) => f(…)')"
                                                    : ": function types must match exactly"));
                return;
            }
        }
        if (isLosslessWiden(from, target)) { coerce(slot, target); return; }
        // P40 review r6 (part 2): a non-null number into a NULLABLE wider slot widens to the slot's own type the
        // same way (the value is present, so it is the plain conversion) — `let x: i64? = a` with an `i32 a` left
        // TS holding a number where a bigint was declared (TS2322; `BigInt.asIntN` then threw a TypeError).
        // A narrowing VALUE needs the cast it needs into the non-null slot (`i64` into `i32?`: C# CS0266, TS2322). A
        // literal keeps the leniency it always had there (`fn f(): u8? => 5` compiles on every target).
        if (target.nullable && !from.nullable && target.kind == TypeRef::Kind::Named) {
            TypeRef base = target;
            base.nullable = false;
            if (isLosslessWiden(from, base)) { coerce(slot, base); return; }
            const Expr* lit = slot.get();
            if (lit->kind == ExprKind::Unary && lit->text == "-" && lit->lhs) lit = lit->lhs.get();
            const bool literal = lit->kind == ExprKind::IntLit || lit->kind == ExprKind::FloatLit;
            if (!literal && isNumericTypeName(from) && isNumericTypeName(base) && !sameNamedType(from, base)) {
                diags_.error(slot->pos, "cannot implicitly convert " + from.name + " to " + showTy(target) + " (" +
                                            ctx + "); add an explicit cast '(" + base.name + ")'");
                return;
            }
        }
        if (isNumericTypeName(from) && isNumericTypeName(target) && !sameNamedType(from, target)) {
            diags_.error(slot->pos, "cannot implicitly convert " + from.name + " to " + target.name +
                                        " (" + ctx + "); add an explicit cast '(" + target.name + ")'");
            return;
        }
        Ty a = scalarTyOf(target), i = scalarTyOf(from);
        if (a != Ty::Unknown && i != Ty::Unknown && a != i)
            diags_.error(slot->pos, std::string("type mismatch (") + ctx + "): expected " + tyName(a) + ", found " + tyName(i));
        // P40 review r5 #15: a scalar never converts to a declared type (a class, record, interface, union or enum),
        // nor the reverse, and a union or enum converts only to itself — `kind(5)` bound `kind(s: Shape)` (C#
        // CS1503, TS2345; Python and PHP printed "shape").
        if ((a == Ty::Unknown) != (i == Ty::Unknown) ? declaredUserType(a == Ty::Unknown ? target : from)
                                                     : (a == Ty::Unknown && declaredUserType(from) && declaredUserType(target) &&
                                                        from.name != target.name &&
                                                        (unionOrEnum(from.name) || unionOrEnum(target.name)))) {
            diags_.error(slot->pos, "cannot convert '" + showTy(from) + "' to '" + showTy(target) + "' (" + ctx + ")");
            return;
        }
        // Nominal subtyping (issue #29): a known user type flowing into a differently-named known user-type
        // slot is valid only when the source extends/implements the target — previously ANY named-type
        // mismatch passed silently and failed (or dispatched wrongly) in the generated target code.
        // Conservative: both names must be declared types (scalars, unions, enums, generic params excluded);
        // an `Iterable` target stays open (every collection/iterator flows into it).
        if (a == Ty::Unknown && i == Ty::Unknown && nominalKnown(from) && nominalKnown(target) &&
            from.name != target.name && target.name != "Iterable" && !inheritsFrom(from.name, target.name)) {
            std::string why = interfaceNames_.count(target.name)
                                  ? " — '" + from.name + "' does not declare that it implements '" + target.name + "'"
                                  : "";
            diags_.error(slot->pos, "cannot convert '" + from.name + "' to '" + target.name + "' (" + ctx + ")" + why);
        }
    }

    // P40 review r4 (part 2): an unsuffixed integer literal (optionally negated) is typed i32, so one outside the
    // range of the ≤32-bit integer slot it flows into (`let v: i32 = 2147483648`, `x: i32 = 2147483648`) was
    // accepted — C# then typed it `uint` (CS0266) while TS/Python/PHP held the out-of-range value. Refused here.
    bool intLiteralFits(const Expr& slot, const TypeRef& target, const std::string& ctx) {
        if (target.kind != TypeRef::Kind::Named || target.nullable) return true;
        const bool sgn = isSignedInt(target.name), uns = isUnsignedInt(target.name);
        const int w = numWidth(target.name);
        if ((!sgn && !uns) || w == 0 || w > 32) return true;
        const Expr* lit = &slot;
        bool neg = false;
        if (lit->kind == ExprKind::Unary && lit->text == "-" && lit->lhs) { neg = true; lit = lit->lhs.get(); }
        if (lit->kind != ExprKind::IntLit || lit->text.find_first_of("iu") != std::string::npos) return true;
        const unsigned long long mag = literalMagnitude(lit->text);
        const unsigned long long maxPos = uns ? (1ull << w) - 1 : (1ull << (w - 1)) - 1;
        const unsigned long long maxNeg = uns ? 0 : 1ull << (w - 1);
        if (neg ? mag <= maxNeg : mag <= maxPos) return true;
        diags_.error(slot.pos, "the integer literal " + std::string(neg ? "-" : "") + lit->text + " does not fit '" +
                                   target.name + "' (" + ctx + "); give it a wider type (e.g. '" + lit->text + "i64')");
        return false;
    }

    // review r5 #15: a type the program declares (class, record, interface, union, enum) — not a type parameter,
    // not a std carrier (`List`, …), not a scalar.
    bool declaredUserType(const TypeRef& t) const {
        return t.kind == TypeRef::Kind::Named && !t.name.empty() && !genericsInScope_.count(t.name) &&
               (typeDeclPos_.count(t.name) || interfaceNames_.count(t.name)) && !unionCaseOwner_.count(t.name);
    }
    bool unionOrEnum(const std::string& n) const { return unionAllCases_.count(n) || enumNames_.count(n); }

    // A member that holds a value (a field, const or computed property) rather than a callable method.
    static bool isValueMember(const MemberInfo& m) {
        return m.kind == MemberKind::Field || m.kind == MemberKind::Const || m.kind == MemberKind::Property;
    }

    // A slot that refuses a may-be-null value (item 14): a function type, or a concrete named type (a scalar,
    // a declared/std type) — never a type parameter, `unit`, `Option` (it converts), `Iterable` or a hole.
    bool nullRefusingSlot(const TypeRef& t) const {
        if (t.kind == TypeRef::Kind::Function) return true;
        if (t.kind != TypeRef::Kind::Named || t.name.empty()) return false;
        if (genericsInScope_.count(t.name) || t.name == "unit" || t.name == "Option" || t.name == "Iterable" ||
            t.name == "null")
            return false;
        return scalarTyOf(t) != Ty::Unknown || knownType(t) || isBuiltinType(t.name);
    }

    // A named type declared in the program (class/record/interface/extern class) — the nominal-subtyping
    // domain. Scalars and generic type params are excluded even when a member-carrier extern class (e.g.
    // `extern class i32`) puts their name in types_.
    bool nominalKnown(const TypeRef& t) const {
        return t.kind == TypeRef::Kind::Named && !t.name.empty() && types_.count(t.name) != 0 &&
               scalarTyOf(t) == Ty::Unknown && !genericsInScope_.count(t.name);
    }

    // Reconcile two numeric binary operands, widening the narrower to the wider. Returns the common type
    // (true), or false if they aren't both numeric or neither widens to the other (caller decides).
    bool reconcileNumeric(Expr& e, const TypeRef& lt, const TypeRef& rt, TypeRef& common) {
        if (!isNumericTypeName(lt) || !isNumericTypeName(rt)) return false;
        if (sameNamedType(lt, rt)) { common = lt; return true; }
        if (isLosslessWiden(lt, rt)) { coerce(e.lhs, rt); common = rt; return true; }
        if (isLosslessWiden(rt, lt)) { coerce(e.rhs, lt); common = lt; return true; }
        return false;
    }

    // P40 A2: a `for` range's bounds. Both must be integers; they widen to one type (the narrower bound gets
    // the usual implicit widening cast), which becomes the loop variable's type. An incompatible pair (a
    // signed/unsigned mix neither side of which widens) is an error. Also decides D7's `rangeEndSimple`.
    TypeRef checkRangeBounds(Expr& r) {
        TypeRef t = checkRangeBoundsImpl(r);
        r.rangeEndBelowMax = rangeEndBelowMax(*r.rhs, t);
        return t;
    }
    // P40 review B6: is the end bound PROVABLY below the loop type's maximum? Only a literal is: an inclusive
    // range whose end may be the maximum would step the counter past it, and a fixed-width counter wraps
    // (C# `i <= int.MaxValue; i++` never ends; PHP's int overflows to a float that still compares <=).
    static bool rangeEndBelowMax(const Expr& e, const TypeRef& t) {
        if (t.kind != TypeRef::Kind::Named) return false;
        const int w = numWidth(t.name);
        if (w == 0) return false;
        if (e.kind == ExprKind::Unary && e.text == "-" && e.lhs && e.lhs->kind == ExprKind::IntLit) return true;
        if (e.kind != ExprKind::IntLit) return false;
        std::string digits;
        for (char c : e.text) {
            if (c == 'i' || c == 'u') break; // the width suffix
            if (c != '_') digits += c;
        }
        errno = 0;
        char* end = nullptr;
        const unsigned long long v = std::strtoull(digits.c_str(), &end, 0);
        if (errno == ERANGE || end == digits.c_str() || *end != '\0') return false;
        const unsigned long long max = isUnsignedInt(t.name) ? (w == 64 ? ~0ull : (1ull << w) - 1)
                                                             : (1ull << (w - 1)) - 1;
        return v < max;
    }
    TypeRef checkRangeBoundsImpl(Expr& r) {
        TypeRef lo = checkExpr(*r.lhs);
        TypeRef hi = checkExpr(*r.rhs);
        r.rangeEndSimple = rangeEndIsSimple(*r.rhs);
        auto isInt = [](const TypeRef& t) {
            return t.kind == TypeRef::Kind::Named && !t.nullable && (isSignedInt(t.name) || isUnsignedInt(t.name));
        };
        auto known = [&](const TypeRef& t) {
            return scalarTyOf(t) != Ty::Unknown || (t.nullable && !t.name.empty()) || nominalKnown(t);
        };
        bool ok = true;
        for (const auto* side : {&lo, &hi}) {
            if (!known(*side) || isInt(*side)) continue;
            const Expr& b = side == &lo ? *r.lhs : *r.rhs;
            diags_.error(b.pos, "range bounds must be integers (got " + side->name + (side->nullable ? "?" : "") + ")");
            ok = false;
        }
        if (!ok) return tUnknown();
        if (!known(lo) || !known(hi)) return known(lo) ? lo : known(hi) ? hi : tNamed("i32"); // lenient
        TypeRef common;
        if (reconcileNumeric(r, lo, hi, common)) return common;
        diags_.error(r.pos, "range bounds have incompatible types " + lo.name + " and " + hi.name +
                            " (neither widens to the other); add an explicit cast");
        return tUnknown();
    }
    // D7: may the end bound be re-read by the loop condition on every iteration? Only when it cannot change:
    // an integer literal (or its negation), a module value (never assignable), or an immutable local/param.
    // Anything else — a mutable local, a member read, a call — is hoisted into a temp by lowering.
    bool rangeEndIsSimple(const Expr& e) const {
        if (e.kind == ExprKind::IntLit) return true;
        if (e.kind == ExprKind::Unary && e.text == "-" && e.lhs && e.lhs->kind == ExprKind::IntLit) return true;
        if (e.kind != ExprKind::Name || !e.staticOwner.empty()) return false;
        if (const Local* l = lookup(e.text)) return !l->isMutable;
        return values_.count(e.text) != 0;
    }

    // Declare a pattern's bindings, typed from the matched value `scrut`: a binding takes `scrut`; a ctor
    // pattern's sub-bindings take the case's field types with the union's generics substituted from `scrut`'s
    // args (so `match o: Option<i32> { Some(v) => … }` types `v` as i32, not Unknown); tuple sub-patterns
    // take the matched element types.
    void declarePattern(const Pattern& p, const TypeRef& scrut) {
        switch (p.kind) {
            case PatKind::Binding:
                // A typed 'match' binding (`d: Disk`) is a runtime TYPE TEST (#38): C# declaration pattern /
                // TS·PHP instanceof / Python isinstance. An INTERFACE test is no longer refused here (P37
                // B4): it gates in the target-aware capability pass instead — refused iff a configured
                // target lacks runtime interface identity (`interfaces:runtimeIdentity`, false on TS where
                // interfaces erase). The binding narrows to the tested type.
                declare(p.name, p.hasType ? p.type : scrut, false, p.pos);
                break;
            case PatKind::Ctor: {
                std::vector<TypeRef> fieldTypes;
                if (auto uc = unionCtors_.find(p.name); uc != unionCtors_.end()) {
                    std::unordered_set<std::string> gen(uc->second.generics.begin(), uc->second.generics.end());
                    std::unordered_map<std::string, TypeRef> binds;
                    for (std::size_t i = 0; i < uc->second.generics.size() && i < scrut.args.size(); ++i) binds[uc->second.generics[i]] = scrut.args[i];
                    for (const auto& pt : uc->second.params) fieldTypes.push_back(substGeneric(pt, gen, binds));
                }
                for (std::size_t i = 0; i < p.sub.size(); ++i) declarePattern(p.sub[i], i < fieldTypes.size() ? fieldTypes[i] : tUnknown());
                break;
            }
            case PatKind::Tuple:
                // §3.B never-miscompile: tuple patterns bind + type-check here but have no lowering yet, so a
                // `match` on a tuple would emit against undefined names. Refuse cleanly until it's a real
                // feature. (`for (a, b) in …` destructuring is a separate, supported path — see declareForBinding.)
                diags_.error(p.pos, "tuple patterns in 'match' are not yet supported; destructure the tuple after "
                                    "the match, or use a `for (a, b) in …` binding");
                for (std::size_t i = 0; i < p.sub.size(); ++i) declarePattern(p.sub[i], i < scrut.args.size() ? scrut.args[i] : tUnknown());
                break;
            default: break;
        }
    }

    // Declare a for-loop binding with the iterated element type: a single binding takes `elem`; a tuple
    // pattern `(a, b)` distributes a tuple element type across its sub-bindings (else they stay unknown).
    void declareForBinding(const Pattern& p, const TypeRef& elem) {
        if (p.kind == PatKind::Binding) {
            declare(p.name, p.hasType ? p.type : elem, false, p.pos);
        } else if (p.kind == PatKind::Tuple) {
            for (std::size_t i = 0; i < p.sub.size(); ++i) {
                TypeRef sub = (elem.kind == TypeRef::Kind::Tuple && i < elem.args.size()) ? elem.args[i] : tUnknown();
                declareForBinding(p.sub[i], sub);
            }
        } else {
            declarePattern(p, elem);
        }
    }

    void checkStmt(Stmt& s) {
        switch (s.kind) {
            case StmtKind::Let: {
                TypeRef init = checkExpr(*s.value);
                if (!s.tupleNames.empty()) { // `let (a, b) = t` — declare each name from the tuple's elements (#39b)
                    for (std::size_t i = 0; i < s.tupleNames.size(); ++i) {
                        TypeRef sub = (init.kind == TypeRef::Kind::Tuple && i < init.args.size()) ? init.args[i] : tUnknown();
                        declare(s.tupleNames[i], sub, s.isMutable, s.namePos);
                    }
                    break;
                }
                if (s.hasDeclType) { normalizeOptional(s.declType, genericsInScope_); resolveTypeRef(s.declType, s.pos); checkConvert(s.value, s.declType, "initializer of '" + s.name + "'"); }
                else if (s.value && !intLiteralFits(*s.value, init, "initializer of '" + s.name + "'")) {}
                else if (s.value && uninferableInit(*s.value, init)) // no annotation + un-inferable init: no target can reconstruct the type (issue #27)
                    diags_.error(s.pos, "cannot infer the type of '" + s.name + "' from its initializer; " + annotationHint(s.name, *s.value, init));
                if (!blockIsBindings_.empty() && blockIsBindings_.back().count(s.name))
                    diags_.error(s.namePos, "'" + s.name + "' was bound by an `is` test earlier in this block; "
                                            "pick another name (the emitted binding outlives the branch)");
                declare(s.name, s.hasDeclType ? s.declType : init, s.isMutable, s.namePos);
                if (Local* nl = lookupMut(s.name); nl && isConstExpr(s.value.get())) { nl->constInit = true; nl->declPos = s.namePos; nl->declStmt = &s; }
                break;
            }
            case StmtKind::Assign: {
                checkExpr(*s.value);
                if (s.target && s.target->kind == ExprKind::Name) {
                    const std::string& nm = s.target->text;
                    const Local* local = lookup(nm);
                    // A bare name resolves for a write exactly as for a read (checkName): a local first, then
                    // — unless a module value claims the name — a static field of the enclosing class, which
                    // lowers to the qualified `Owner.name` access every target already writes.
                    const MemberInfo* st = (!local && !values_.count(nm)) ? enclosingStatic(nm) : nullptr;
                    if (st) {
                        s.target->staticOwner = currentClass_;
                        s.target->type = st->type;
                        checkMemberWritable(*st, currentClass_, currentClass_ + "." + nm, /*viaThis=*/false, /*isStatic=*/true, s.pos);
                        if (s.op == "??=") checkCoalesceAssign(s, st->type, st->type, "'??=' assignment to '" + nm + "'");
                        else {
                            if (s.op != "=" && st->type.nullable)
                                diags_.error(s.pos, "'" + s.op + "' target '" + nm + "' of type '" + showTy(st->type) +
                                                        "' may be null; read it with '??' or '!' and assign the result");
                            const bool userCompound = s.op != "=" && st->type.kind == TypeRef::Kind::Named &&
                                                      !st->type.name.empty() && knownType(st->type) &&
                                                      !isBuiltinType(st->type.name);
                            if (!userCompound) checkCompoundValue(s, st->type, "assignment to '" + nm + "'");
                        }
                    } else if (!local) diags_.error(s.pos, "assignment to undeclared '" + nm + "'");
                    else {
                        s.target->type = local->type; // stamp: the C7 compound rewrite keys on this
                        if (!local->isMutable) diags_.error(s.pos, "cannot assign to immutable '" + nm + "' (declared with 'let')");
                        // P40 A4 (N1, S7 hole): a compound op reads the target — a nullable one must be
                        // narrowed first. Then any assignment ends the narrowing (D14).
                        if (s.op != "=" && s.op != "??=" && local->type.nullable && !narrowedHere(*local))
                            diags_.error(s.pos, "'" + s.op + "' target '" + nm + "' of type '" + showTy(local->type) +
                                                    "' may be null; narrow it first ('if " + nm + " != null') or assign a non-null value");
                        if (s.op == "??=") {
                            TypeRef readT = local->type;
                            if (readT.nullable && narrowedHere(*local)) readT.nullable = false;
                            checkCoalesceAssign(s, readT, local->type, "'??=' assignment to '" + nm + "'");
                        }
                        Local* ml = lookupMut(nm);
                        ml->assigns++;
                        if (s.op != "=") ml->reads++; // a compound reads the target
                        else if (!isConstExpr(s.value.get())) ml->nonConstAssign = true; // review r5 #17: no CS0219
                        ml->nonNull = false;
                        // P37 C7: a compound op on a USER type converts against the operator member's own
                        // signature (lenient, like binary resolution) — the rhs is the operator's operand,
                        // not a value of the target's type (`v *= 2.0` with `times(s: f64)`).
                        const bool userCompound = s.op != "=" && local->type.kind == TypeRef::Kind::Named &&
                                                  !local->type.name.empty() && knownType(local->type) &&
                                                  !isBuiltinType(local->type.name);
                        if (!userCompound && s.op != "??=") checkCompoundValue(s, local->type, "assignment to '" + nm + "'");
                    }
                } else if (s.target) {
                    const TypeRef tt = checkExpr(*s.target);
                    if (s.target->kind == ExprKind::Member && s.target->lhs) { // `Type.f = …` / `recv.f = …`
                        const Expr& r = *s.target->lhs;
                        if (r.kind == ExprKind::Name && !enumNames_.count(r.text) && types_.count(r.text)) {
                            if (const MemberInfo* m = findMember(r.text, s.target->text))
                                checkMemberWritable(*m, r.text, r.text + "." + s.target->text, false, true, s.pos);
                        } else if (knownType(r.type)) {
                            if (MemberHit hit = findMemberOn(r.type, s.target->text); hit.m)
                                checkMemberWritable(*hit.m, hit.owner, hit.owner + "." + s.target->text,
                                                    r.kind == ExprKind::This, hit.m->isStatic, s.pos);
                        }
                    }
                    if (s.op == "??=") checkCoalesceAssign(s, tt, tt, "'??=' assignment");
                    else if (s.op != "=" && tt.nullable) // P40 A4 (N1, S7 hole): the compound op reads a maybe-null value
                        diags_.error(s.pos, "'" + s.op + "' target of type '" + showTy(tt) +
                                                "' may be null; read it with '??' or '!' and assign the result");
                    // A field/element is a typed slot like a local: the value converts into it (widening,
                    // list-literal typing, the T?-representation boundary of SPEC §4.4). It used to skip
                    // every conversion, so `this.item = slot.peek()` stored an Option in a `string?` field.
                    // P37 C7: a compound op on a user-typed Member/Index target — or through a USER
                    // indexer even with scalar elements — re-lowers the target for the read side, so the
                    // base must be pure (a name or `this`) for single evaluation; a computed receiver is
                    // refused toward an explicit local.
                    const bool userTyped = s.target->type.kind == TypeRef::Kind::Named &&
                                           !s.target->type.name.empty() && knownType(s.target->type) &&
                                           !isBuiltinType(s.target->type.name);
                    const bool userIndexer = s.target->kind == ExprKind::Index && s.target->lhs &&
                                             s.target->lhs->type.kind == TypeRef::Kind::Named &&
                                             findMember(s.target->lhs->type.name, "get") != nullptr;
                    // P40: a compound op on a built-in slot converts its value into the slot like a local's
                    // does (it used to skip conversion here, so `this.big += 1` on an i64 field added a JS
                    // number to a BigInt, and `this.n += 1.5` on an i32 field reached the targets). On a user
                    // type the operator member's own signature governs instead (C7, as for locals).
                    if (s.op == "=" || (s.op != "??=" && !userTyped))
                        checkCompoundValue(s, tt, s.op == "=" ? "assignment" : "'" + s.op + "' assignment");
                    if (s.op == "??=" && userIndexer) // the get/set pair has no conditional-store form
                        diags_.error(s.pos, "'??=' through a user indexer is not supported — read it, test it for "
                                            "null and assign explicitly");
                    else if (s.op != "=" && !(s.op == "??=" && !userIndexer) &&
                        (s.target->kind == ExprKind::Member || s.target->kind == ExprKind::Index) &&
                        (userTyped || userIndexer)) {
                        const Expr* base = s.target->lhs.get();
                        if (base && base->kind != ExprKind::Name && base->kind != ExprKind::This)
                            diags_.error(s.pos, "a compound assignment on a user type or user indexer "
                                                "needs a simple receiver — hoist the receiver to a local "
                                                "first (single evaluation)");
                    }
                }
                break;
            }
            case StmtKind::ExprStmt:
                checkExpr(*s.value);
                // review r5 (part 2): a bare function name is a value, not a call — C# CS0201 (it emitted
                // `PolyglotProgram.five;`), and a no-op elsewhere.
                if (s.value->kind == ExprKind::Name && s.value->fnRef)
                    diags_.error(s.value->pos, "'" + s.value->text + "' names a function but does not call it — "
                                               "write '" + s.value->text + "()'");
                break;
            case StmtKind::Throw:    if (s.value) checkExpr(*s.value); break;
            case StmtKind::Yield:
                if (inAsync_)
                    diags_.error(s.pos, "Polyglot refuses async iterators — a function cannot be both 'async' "
                                        "and use 'yield' (PRD §4.7)");
                if (s.value) {
                    checkExpr(*s.value);
                    // P40 review r5 item 18: a yielded value flows into the iterator's element type, like a
                    // `return` value into the return type (`yield null` from an `Iterable<i32>` was accepted).
                    if (currentReturn_.kind == TypeRef::Kind::Named && currentReturn_.name == "Iterable" &&
                        currentReturn_.args.size() == 1 && !currentReturn_.nullable)
                        checkConvert(s.value, currentReturn_.args[0], "yielded value");
                }
                break;
            case StmtKind::If: {
                // P37 B3: `is` bindings on the condition's `&&` spine narrow into the rest of the
                // condition AND the then-body (one scope) — never the else-branch or past the `if`.
                // P40 D14: the condition's null facts narrow the then-body (true) and the else-body (false);
                // a branch that always exits narrows the REST of the enclosing list by the other sense.
                std::vector<std::string> whenTrue, whenFalse;
                nullFacts(*s.value, true, whenTrue);
                nullFacts(*s.value, false, whenFalse);
                pushScope();
                requireBool(checkIfCond(*s.value), s.pos, "'if' condition");
                blockIsBindings_.emplace_back(); // then-body is its own block for later-redecl purposes
                {
                    std::vector<NarrowSave> saves;
                    narrow(whenTrue, saves);
                    checkStmts(s.thenBody);
                    restoreNarrow(saves);
                }
                blockIsBindings_.pop_back();
                popScope();
                if (s.hasElse) {
                    std::vector<NarrowSave> saves;
                    narrow(whenFalse, saves);
                    checkBlock(s.elseBody);
                    restoreNarrow(saves);
                }
                if (!blockIsBindings_.empty()) collectIsBindingNames(*s.value, blockIsBindings_.back());
                if (!narrowFrames_.empty()) { // early exit: lasts to the end of the enclosing statement list
                    if (endsInExit(s.thenBody)) narrow(whenFalse, narrowFrames_.back());
                    if (s.hasElse && endsInExit(s.elseBody)) narrow(whenTrue, narrowFrames_.back());
                }
                break;
            }
            case StmtKind::While: {
                // P40 D14: a name the loop assigns is not narrowed anywhere in it (the back edge re-enters
                // with the reassigned value); the condition's facts then narrow the body up to an assignment.
                std::unordered_set<std::string> assigned;
                collectAssigned(s.thenBody, false, false, assigned);
                std::vector<NarrowSave> saves;
                unnarrow(std::vector<std::string>(assigned.begin(), assigned.end()), saves);
                requireBool(checkExpr(*s.value), s.pos, "'while' condition");
                if (!s.isDoWhile) {
                    std::vector<std::string> whenTrue;
                    nullFacts(*s.value, true, whenTrue);
                    narrow(whenTrue, saves);
                }
                checkBlock(s.thenBody);
                restoreNarrow(saves);
                break;
            }
            case StmtKind::For: {
                // A range yields its widened integer bound type (P40 A2); a collection its element type. The
                // range is checked HERE, never through checkExpr — that path is the "range outside a `for`
                // iterable" error.
                TypeRef elem;
                if (s.value && s.value->kind == ExprKind::Range) {
                    elem = checkRangeBounds(*s.value);
                    s.value->type = elem;
                } else {
                    TypeRef iter = s.value ? checkExpr(*s.value) : tUnknown();
                    elem = elementType(iter);
                    refuseNonIterableExtern(iter, s.value ? s.value->pos : s.pos);
                }
                std::unordered_set<std::string> assigned; // P40 D14: see While
                collectAssigned(s.thenBody, false, false, assigned);
                std::vector<NarrowSave> saves;
                unnarrow(std::vector<std::string>(assigned.begin(), assigned.end()), saves);
                pushScope();
                declareForBinding(s.forBinding, elem);
                checkStmts(s.thenBody);
                popScope();
                restoreNarrow(saves);
                break;
            }
            case StmtKind::Use:
                if (s.value) checkExpr(*s.value);
                if (s.hasDeclType) { normalizeOptional(s.declType, genericsInScope_); resolveTypeRef(s.declType, s.pos); }
                pushScope();
                declare(s.name, s.hasDeclType ? s.declType : tUnknown(), false, s.namePos);
                checkStmts(s.thenBody);
                popScope();
                break;
            case StmtKind::Try:
                checkBlock(s.thenBody);
                for (auto& c : s.catches) {
                    pushScope();
                    if (!c.type.absent()) resolveTypeRef(c.type, c.pos);
                    declare(c.name, c.type, false, c.pos);
                    if (c.guard) requireBool(checkExpr(*c.guard), c.pos, "'when' guard");
                    checkStmts(c.body);
                    popScope();
                }
                if (s.hasFinally) checkBlock(s.finallyBody);
                break;
            case StmtKind::Return: {
                // Inside a lambda the result type is open here (see checkLambda): any form is accepted.
                if (currentReturn_.kind == TypeRef::Kind::Named && currentReturn_.name.empty()) {
                    if (s.value) checkExpr(*s.value);
                    break;
                }
                if (s.value) {
                    checkExpr(*s.value);
                    if (isUnitT(currentReturn_)) diags_.error(s.pos, "this function returns unit; 'return' takes no value");
                    else checkConvert(s.value, currentReturn_, "return value");
                } else if (!isUnitT(currentReturn_)) {
                    diags_.error(s.pos, "this function must return a value");
                }
                break;
            }
            case StmtKind::Break: case StmtKind::Continue: break;
        }
    }
    // P40 B4: an extern (native-bound) class is `for`-iterable only as List/Array/Iterable or through an
    // indexer; anything else (a `Map`) would iterate whatever the native type happens to yield — keys on
    // TS/Python, pairs on C#, values on PHP — so it is refused, pointing at a snapshot member when one exists.
    void refuseNonIterableExtern(const TypeRef& t, SourcePos pos) {
        if (t.kind != TypeRef::Kind::Named || t.nullable || isBuiltinType(t.name) || !externClassNames_.count(t.name))
            return;
        if (t.name == "List" || t.name == "Array" || t.name == "Iterable") return;
        auto it = types_.find(t.name);
        if (it == types_.end()) return;
        std::vector<std::string> snaps;
        for (const auto& m : it->second.members) {
            if (m.kind == MemberKind::Operator && m.name == "get") return;
            if (m.kind == MemberKind::Method && !m.isStatic && m.params.empty() && m.type.kind == TypeRef::Kind::Named &&
                m.type.name == "List")
                snaps.push_back(m.name);
        }
        std::string hint;
        for (std::size_t i = 0; i < snaps.size(); ++i)
            hint += (i == 0 ? "" : i + 1 == snaps.size() ? " or " : ", ") + std::string("'.") + snaps[i] + "()'";
        diags_.error(pos, "'" + showTy(t) + "' is not iterable" +
                              (hint.empty() ? std::string()
                                            : "; iterate a snapshot instead: " + hint));
    }
    static bool isUnitT(const TypeRef& t) { return t.kind == TypeRef::Kind::Named && t.name == "unit" && !t.nullable; }
    // ---- P37 D: attributes (both tiers) + the Meta intrinsics -----------------------------------------

    // The const envelope attribute params may use: what every target holds as a plain literal — scalars,
    // strings, enum types, and single-level arrays of those.
    bool isConstEnvelopeType(const TypeRef& t) const {
        if (t.kind != TypeRef::Kind::Named || t.nullable) return false;
        const std::string& n = t.name;
        if (n == "Array" && t.args.size() == 1) { // `T[]` — one level, element must itself be envelope
            const TypeRef& el = t.args[0];
            return el.kind == TypeRef::Kind::Named && el.args.empty() && isConstEnvelopeType(el);
        }
        if (!t.args.empty()) return false;
        if (enumNames_.count(n)) return true;
        return n == "bool" || n == "string" || n == "i8" || n == "i16" || n == "i32" || n == "i64" ||
               n == "u8" || n == "u16" || n == "u32" || n == "u64" || n == "f32" || n == "f64";
    }
    // A compile-time-constant attribute argument: a literal, a signed numeric literal, an enum member,
    // or a non-empty array literal of those. There is no constant evaluator and none is wanted —
    // resolvability is decided by FORM (M6 discipline).
    bool isConstAttrValue(const Expr& e) const {
        switch (e.kind) {
            case ExprKind::IntLit: case ExprKind::FloatLit: case ExprKind::StringLit: case ExprKind::BoolLit:
                return true;
            case ExprKind::Unary:
                return e.text == "-" && e.lhs &&
                       (e.lhs->kind == ExprKind::IntLit || e.lhs->kind == ExprKind::FloatLit);
            case ExprKind::Member: // an enum member `Color.Red`
                return e.lhs && e.lhs->kind == ExprKind::Name && enumNames_.count(e.lhs->text) != 0;
            case ExprKind::ListLit: { // a non-empty array of consts (empty = uninferable, like #27)
                if (e.args.empty()) return false;
                for (const auto& el : e.args)
                    if (!el || el->kind == ExprKind::ListLit || !isConstAttrValue(*el)) return false;
                return true;
            }
            default:
                return false;
        }
    }
    enum class AttrPoint { Type, Method, Field, Param, Function, Other };

    void checkAttributesPass(CompilationUnit& u) {
        for (auto& ea : u.externAttrs) {
            if (!externAttrs_.emplace(ea.name, &ea).second)
                diags_.error(ea.pos, "duplicate extern attribute '" + ea.name + "'");
            std::unordered_set<std::string> targets;
            for (auto& arm : ea.arms)
                if (!targets.insert(arm.target).second)
                    diags_.error(arm.pos, "extern attribute '" + ea.name + "': duplicate arm for target '" +
                                              arm.target + "'");
        }
        for (auto& r : u.records) {
            if (r.isAttribute) {
                attributeNames_.insert(r.name);
                if (!r.bases.empty())
                    diags_.error(r.pos, "an `attribute` is a pure data shape — no base types");
                if (!r.generics.empty())
                    diags_.error(r.pos, "an `attribute` cannot be generic (its values must be plain constants)");
                for (const auto& f : r.fields)
                    if (!isConstEnvelopeType(f.type))
                        diags_.error(f.pos, "attribute parameter '" + f.name + "' must be a constant-"
                                            "envelope type (bool, an integer/float scalar, or string)");
            }
        }
        for (auto& r : u.records) {
            checkAttrList(r.attributes, AttrPoint::Type);
            if (r.isAttribute) { // an attribute's own params carry data, not further attributes
                for (auto& p : r.fields)
                    if (!p.attributes.empty())
                        diags_.error(p.attributes.front().pos,
                                     "attributes on an `attribute` declaration's parameters are refused");
            }
            for (auto& m : r.members) {
                checkAttrList(m.attributes, memberPoint(m));
                for (auto& p : m.params) checkAttrList(p.attributes, AttrPoint::Param);
            }
        }
        for (auto& c : u.classes) {
            checkAttrList(c.attributes, AttrPoint::Type);
            for (auto& m : c.members) {
                checkAttrList(m.attributes, memberPoint(m));
                for (auto& p : m.params) checkAttrList(p.attributes, AttrPoint::Param);
            }
        }
        for (auto& f : u.functions) {
            checkAttrList(f.attributes, AttrPoint::Function);
            for (auto& p : f.params) checkAttrList(p.attributes, AttrPoint::Param);
        }
        for (auto& ea : u.externAttrs)
            for (auto& p : ea.params)
                if (!p.attributes.empty())
                    diags_.error(p.attributes.front().pos,
                                 "attributes on an `extern attribute` declaration's parameters are refused");
        for (auto& i : u.interfaces)
            for (auto& m : i.members)
                if (!m.attributes.empty())
                    diags_.error(m.attributes.front().pos,
                                 "attributes on interface members are not supported (v1)");
    }
    static AttrPoint memberPoint(const Member& m) {
        if (m.kind == MemberKind::Method) return AttrPoint::Method;
        if (m.kind == MemberKind::Field || m.kind == MemberKind::Property) return AttrPoint::Field;
        return AttrPoint::Other;
    }
    void checkAttrList(std::vector<AttrUse>& attrs, AttrPoint point) {
        std::unordered_set<std::string> seen;
        for (auto& a : attrs) {
            const bool tier1 = externAttrs_.count(a.name) != 0;
            const bool tier2 = attributeNames_.count(a.name) != 0;
            if (!tier1 && !tier2) {
                diags_.error(a.pos, "unknown attribute '" + a.name + "' — declare `attribute " + a.name +
                                        "(…)` (portable metadata) or `extern attribute " + a.name +
                                        "(…) { … }` (native pass-through)");
                continue;
            }
            if (!seen.insert(a.name).second)
                diags_.error(a.pos, "attribute '" + a.name + "' is applied more than once on this "
                                        "declaration (AllowMultiple is a deferred follow-up)");
            if (point == AttrPoint::Other)
                diags_.error(a.pos, "attributes are not supported on this member kind (v1: methods, "
                                        "fields/properties, and parameters)");
            else if (tier2 && point == AttrPoint::Function)
                diags_.error(a.pos, "portable metadata on a free function has no query surface (`Meta` "
                                        "takes a type) — deferred; a pass-through `extern attribute` works");
            const std::vector<Param>& params =
                tier1 ? externAttrs_[a.name]->params : typeInfoParams(a.name);
            checkAttrArgs(a, params);
        }
    }
    // A Tier 2 attribute's params are its record fields (registered as ctorParams); re-read the DECL's
    // fields so names + defaults are visible for named-arg matching.
    const std::vector<Param>& typeInfoParams(const std::string& attrName) const {
        static const std::vector<Param> kEmpty;
        for (const auto& r : unit_->records)
            if (r.name == attrName) return r.fields;
        return kEmpty;
    }
    void checkAttrArgs(AttrUse& a, const std::vector<Param>& params) {
        std::unordered_set<std::string> given;
        std::size_t positional = 0;
        bool sawNamed = false;
        for (auto& arg : a.args) {
            if (!arg.value) continue;
            const Param* p = nullptr;
            if (arg.name.empty()) {
                if (sawNamed)
                    diags_.error(arg.pos, "positional attribute arguments must precede named ones");
                if (positional < params.size()) p = &params[positional];
                else diags_.error(arg.pos, "too many arguments for attribute '" + a.name + "'");
                ++positional;
            } else {
                sawNamed = true;
                for (const auto& cand : params)
                    if (cand.name == arg.name) { p = &cand; break; }
                if (!p)
                    diags_.error(arg.pos, "attribute '" + a.name + "' has no parameter named '" +
                                              arg.name + "'");
            }
            if (p && !given.insert(p->name).second)
                diags_.error(arg.pos, "attribute argument '" + p->name + "' is given twice");
            if (!isConstAttrValue(*arg.value))
                diags_.error(arg.pos, "attribute arguments must be compile-time constants (a literal); "
                                          "variable values are a deferred follow-up");
            checkExpr(*arg.value);
            if (p) checkConvert(arg.value, p->type, "attribute argument '" + p->name + "'");
        }
        for (std::size_t i = 0; i < params.size(); ++i)
            if (!params[i].hasDefault && (i >= positional && !given.count(params[i].name)))
                diags_.error(a.pos, "attribute '" + a.name + "' is missing required argument '" +
                                        params[i].name + "'");
    }

    // P37 D.3 (M6, permanent): the `Meta` intrinsics — `Meta.has<T, A>()`, `Meta.get<T, A>() -> A?`,
    // `Meta.member<T, A>("m") -> A?` — resolved entirely at transpile time. Type arguments must be
    // compile-time-concrete names (never a type parameter: the compiler is generic-preserving, so there
    // is no point at which `T` is one type) and the member name a string literal. All diagnosed here in
    // target-independent sema so build/check/LSP agree.
    TypeRef checkMetaCall(Expr& e) {
        const std::string& method = e.lhs->text;
        if (method != "has" && method != "get" && method != "member" && method != "param") {
            diags_.error(e.pos, "unknown Meta intrinsic '" + method + "' (has / get / member / param)");
            return tUnknown();
        }
        if (e.typeArgs.size() != 2) {
            diags_.error(e.pos, "Meta." + method + " takes exactly two type arguments: <T, A>");
            return tUnknown();
        }
        auto concrete = [&](const TypeRef& t, const char* what) -> bool {
            if (t.kind != TypeRef::Kind::Named || t.name.empty()) {
                diags_.error(e.pos, std::string("Meta's ") + what + " must be a named type");
                return false;
            }
            if (genericsInScope_.count(t.name)) {
                diags_.error(e.pos, std::string("Meta's ") + what + " cannot be a type parameter ('" +
                                        t.name + "') — Meta resolves at transpile time and generics are "
                                        "emitted generically, never specialized (M6, permanent)");
                return false;
            }
            if (!t.args.empty()) {
                diags_.error(e.pos, std::string("Meta's ") + what + " cannot be a generic instantiation");
                return false;
            }
            if (!typeNames_.count(t.name)) {
                diags_.error(e.pos, "unknown type '" + t.name + "' in Meta." + method);
                return false;
            }
            return true;
        };
        if (!concrete(e.typeArgs[0], "subject type <T>") || !concrete(e.typeArgs[1], "attribute type <A>"))
            return tUnknown();
        const std::string& tn = e.typeArgs[0].name;
        const std::string& an = e.typeArgs[1].name;
        if (interfaceNames_.count(tn) || enumNames_.count(tn) || unionAllCases_.count(tn) ||
            externClassNames_.count(tn) || attributeNames_.count(tn)) {
            diags_.error(e.pos, "Meta's subject type <T> must be a user class or record ('" + tn + "')");
            return tUnknown();
        }
        if (!attributeNames_.count(an)) {
            diags_.error(e.pos, "'" + an + "' is not an `attribute` declaration — Meta queries portable "
                                    "(Tier 2) metadata only");
            return tUnknown();
        }
        if (method == "param") {
            // Meta.param<T, A>("method", "param") — both names are string literals resolved at
            // transpile time against the declaration (M6: computed names cannot resolve).
            if (e.args.size() != 2 || !e.args[0] || e.args[0]->kind != ExprKind::StringLit ||
                !e.args[1] || e.args[1]->kind != ExprKind::StringLit) {
                diags_.error(e.pos, "Meta.param takes two STRING LITERALS: the method name and the "
                                        "parameter name (M6)");
                return tUnknown();
            }
            checkExpr(*e.args[0]);
            checkExpr(*e.args[1]);
            const std::string& mem = e.args[0]->text;
            const std::string& par = e.args[1]->text;
            const Member* found = findDeclMember(tn, mem);
            if (!found) {
                diags_.error(e.args[0]->pos, "'" + tn + "' has no member named '" + mem + "'");
                return tUnknown();
            }
            bool hasParam = false;
            for (const auto& p : found->params) if (p.name == par) hasParam = true;
            if (!hasParam) {
                SourcePos endp = e.args[1]->pos;
                endp.col += static_cast<int>(par.size()) + 2;
                diags_.error(e.args[1]->pos, endp,
                             "'" + tn + "." + mem + "' has no parameter named '" + par + "'");
                return tUnknown();
            }
        } else if (method == "member") {
            if (e.args.size() != 1 || !e.args[0] || e.args[0]->kind != ExprKind::StringLit) {
                diags_.error(e.pos, "Meta.member takes one STRING LITERAL member name — a variable or "
                                        "computed name cannot resolve at transpile time (M6)");
                return tUnknown();
            }
            checkExpr(*e.args[0]);
            const std::string& mem = e.args[0]->text;
            if (!findMember(tn, mem)) {
                SourcePos endp = e.args[0]->pos;
                endp.col += static_cast<int>(mem.size()) + 2; // underline the literal (incl. quotes)
                diags_.error(e.args[0]->pos, endp,
                             "'" + tn + "' has no member named '" + mem + "'");
                return tUnknown();
            }
        } else if (!e.args.empty()) {
            diags_.error(e.pos, "Meta." + method + " takes no value arguments");
            return tUnknown();
        }
        if (method != "has" && unit_) {
            auto& mm = unit_->metaMaterialized;
            bool present = false;
            for (const auto& n : mm) if (n == an) present = true;
            if (!present) mm.push_back(an);
        }
        if (method == "has") return tNamed("bool");
        TypeRef result = e.typeArgs[1];
        result.nullable = true;
        return result;
    }

    // P37 B3: check an `if` condition, accepting `is` bindings along its top-level `&&` spine only —
    // there the test dominates the then-branch. Under `||`, `!`, or a lambda the binding wouldn't be
    // guaranteed, so the ordinary checkExpr path refuses it.
    TypeRef checkIfCond(Expr& e) {
        if (e.kind == ExprKind::Binary && e.text == "&&") {
            requireBool(checkIfCond(*e.lhs), e.lhs->pos, "'&&' operand");
            std::vector<std::string> facts; // P40 D14: `x != null && …` narrows the right operand
            nullFacts(*e.lhs, true, facts);
            std::vector<NarrowSave> saves;
            narrow(facts, saves);
            requireBool(checkIfCond(*e.rhs), e.rhs->pos, "'&&' operand");
            restoreNarrow(saves);
            e.type = tNamed("bool");
            return e.type;
        }
        if (e.kind == ExprKind::Is) {
            isBindingAllowed_ = true;
            TypeRef t = checkExpr(e); // checkIsAs consumes + resets the flag
            isBindingAllowed_ = false;
            return t;
        }
        return checkExpr(e);
    }
    // The `is`-binding names on an if-condition's `&&` spine (for the enclosing block's redecl guard).
    void collectIsBindingNames(const Expr& e, std::unordered_set<std::string>& out) const {
        if (e.kind == ExprKind::Binary && e.text == "&&") {
            if (e.lhs) collectIsBindingNames(*e.lhs, out);
            if (e.rhs) collectIsBindingNames(*e.rhs, out);
        } else if (e.kind == ExprKind::Is && !e.text.empty()) {
            out.insert(e.text);
        }
    }

    // P37 B: `x is T [name]` / `x as T` — runtime tests over class/record hierarchies (M1: `as` yields
    // null on failure, never throws). Interfaces pass here and gate per-target in the capability pass
    // (B4: refused only when a configured target lacks runtime interface identity). Unions/cases point
    // at `match` (no case-as-type exists to narrow to); enums/scalars/extern classes are refused.
    TypeRef checkIsAs(Expr& e, bool isTest) {
        const bool bindingAllowed = isBindingAllowed_;
        isBindingAllowed_ = false;
        TypeRef operand = checkExpr(*e.lhs);
        const char* op = isTest ? "is" : "as";
        TypeRef& target = e.castType;
        normalizeOptional(target, genericsInScope_);
        const std::string& n = target.name;
        auto fail = [&](const std::string& msg) {
            diags_.error(e.pos, msg);
            return isTest ? tNamed("bool") : tUnknown();
        };
        if (target.kind != TypeRef::Kind::Named || n.empty())
            return fail(std::string("'") + op + "' expects a named class or record type");
        if (genericsInScope_.count(n))
            return fail(std::string("'") + op + "' cannot test a type parameter ('" + n +
                        "') — generics are emitted generically, never specialized, so no runtime test exists");
        if (isBuiltinType(n))
            return fail(std::string("'") + op + "' does not apply to scalar/builtin types; use a numeric cast");
        if (enumNames_.count(n))
            return fail(std::string("'") + op + "' does not apply to enums (enum values are not class instances)");
        if (unionAllCases_.count(n) || unionCtors_.count(n))
            return fail(std::string("Polyglot refuses '") + op + "' on union types/cases — use 'match' "
                        "(union values are tagged data, and cases are not types to narrow to)");
        if (externClassNames_.count(n))
            return fail(std::string("'") + op + "' cannot test an extern/std type ('" + n +
                        "') — its runtime shape is target-defined");
        if (!typeNames_.count(n))
            return fail("unknown type '" + n + "' in '" + op + "'");
        if (!target.args.empty())
            return fail(std::string("'") + op + "' cannot test a generic instantiation ('" + n +
                        "<…>') — type arguments are erased at runtime on TS/Python");
        // Nominal relatedness: refuse a provably-impossible test (neither type derives from the other,
        // and the operand isn't interface-typed). An unknown/erased operand type stays lenient.
        if (operand.kind == TypeRef::Kind::Named && !operand.name.empty() && typeNames_.count(operand.name) &&
            !interfaceNames_.count(operand.name) && !interfaceNames_.count(n)) {
            const std::string& on = operand.name;
            if (on != n && !derivesFrom(n, on) && !derivesFrom(on, n))
                return fail(std::string("'") + operand.name + "' can never be a '" + n + "' — the '" + op +
                            "' test is provably impossible (unrelated types)");
        }
        if (isTest && !e.text.empty()) {
            if (!bindingAllowed)
                diags_.error(e.pos, "an `is` binding ('" + e.text + "') is only allowed on an `if` "
                                    "condition's `&&` chain, where the test guards the branch (P37 B3)");
            else {
                TypeRef bound = target;
                declare(e.text, bound, false, e.pos);
            }
        }
        if (isTest) return tNamed("bool");
        TypeRef result = target;
        result.nullable = true; // `as` = checked conversion -> T? (null on failure, never throws)
        return result;
    }
    // Decl-level member lookup (P37 D: Meta.param needs parameter NAMES, which MemberInfo drops).
    const Member* findDeclMember(const std::string& typeName, const std::string& member) const {
        if (!unit_) return nullptr;
        for (const auto& c : unit_->classes)
            if (c.name == typeName)
                for (const auto& m : c.members) if (m.name == member) return &m;
        for (const auto& r : unit_->records)
            if (r.name == typeName)
                for (const auto& m : r.members) if (m.name == member) return &m;
        return nullptr;
    }

    // Transitive nominal derivation: `derived` reaches `base` through class/interface base lists.
    bool derivesFrom(const std::string& derived, const std::string& base) const {
        auto it = types_.find(derived);
        if (it == types_.end()) return false;
        for (const auto& b : it->second.bases) {
            if (b == base || derivesFrom(b, base)) return true;
        }
        return false;
    }

    void requireBool(const TypeRef& t, SourcePos pos, const char* what) {
        if (t.nullable) { // P40 A4 (S7 hole): a `bool?` condition — null is neither branch on every target
            diags_.error(pos, std::string(what) + " of type '" + showTy(t) +
                                  "' may be null; supply a fallback with '??' or assert with '!'");
            return;
        }
        Ty s = scalarTyOf(t);
        if (s != Ty::Unknown && s != Ty::Bool) diags_.error(pos, std::string(what) + " must be bool, found " + tyName(s));
    }

    // The element type of an iterable/collection: List<T> and Iterable<T> -> T; anything else -> unknown.
    // Used to type `lst[i]` and the `for x in lst` binding.
    TypeRef elementType(const TypeRef& t) {
        if (t.kind == TypeRef::Kind::Named && (t.name == "List" || t.name == "Array" || t.name == "Iterable") && !t.args.empty())
            return t.args[0];
        // A user type with an `operator fn get(index): R` indexer: `recv[i]` is R, with the type's generics
        // substituted from `t`'s args (e.g. RingBuffer<string>[i] -> string).
        if (t.kind == TypeRef::Kind::Named) {
            if (auto it = types_.find(t.name); it != types_.end())
                for (const auto& m : it->second.members)
                    if (m.kind == MemberKind::Operator && m.name == "get") {
                        std::vector<std::string> gnames; for (const auto& g : it->second.generics) gnames.push_back(g.name);
                        std::unordered_set<std::string> gen(gnames.begin(), gnames.end());
                        std::unordered_map<std::string, TypeRef> binds;
                        for (std::size_t i = 0; i < gnames.size() && i < t.args.size(); ++i) binds[gnames[i]] = t.args[i];
                        return substGeneric(m.type, gen, binds);
                    }
        }
        return tUnknown();
    }

    // Arity-check a subscript against its target's indexer (issue #42). Native collections take exactly one
    // index; a user `operator fn get(...)` takes as many as it declares. Lenient when the receiver type is
    // unknown or has no resolvable indexer (a bare `recv[]` degrading to `0` also passes).
    void checkIndexArity(const Expr& e, const TypeRef& recv) {
        if (e.args.empty() || recv.kind != TypeRef::Kind::Named) return;
        if (recv.name == "List" || recv.name == "Array" || recv.name == "Iterable") {
            if (e.args.size() != 1)
                diags_.error(e.pos, "indexing '" + recv.name + "' takes exactly one index, got " +
                                        std::to_string(e.args.size()));
            return;
        }
        if (auto it = types_.find(recv.name); it != types_.end())
            for (const auto& m : it->second.members)
                if (m.kind == MemberKind::Operator && m.name == "get") {
                    if (e.args.size() != m.params.size())
                        diags_.error(e.pos, "indexer 'get' on '" + recv.name + "' expects " +
                                                std::to_string(m.params.size()) + " index argument(s), got " +
                                                std::to_string(e.args.size()));
                    return;
                }
    }

    // P40 review r5 item 18: an index flows into its slot like an argument — a native collection's index is an
    // `i32` (`xs[m]` with an `i32?` or a string index was accepted: TS2538, a PHP null offset), a user indexer's
    // are its `get` parameters (instantiated from the receiver). Runs after the arity check; a mismatched count
    // was already reported there.
    void convertIndexArgs(Expr& e, const TypeRef& recv) {
        if (recv.kind != TypeRef::Kind::Named || recv.nullable) return;
        if (recv.name == "List" || recv.name == "Array") {
            if (e.args.size() == 1) checkConvert(e.args[0], tNamed("i32"), "index");
            return;
        }
        MemberHit hit = findMemberOn(recv, "get");
        if (!hit.m || hit.m->kind != MemberKind::Operator || hit.m->params.size() != e.args.size()) return;
        std::vector<TypeRef> ps = memberParamsOf(hit, e.pos);
        for (std::size_t i = 0; i < ps.size(); ++i)
            checkConvert(e.args[i], ps[i], "index " + std::to_string(i + 1) + " of '" + recv.name + "'");
    }

    // ---- expression typing ----
    // checkExpr annotates each node with its resolved type (read later by IR lowering), then returns it.
    TypeRef checkExpr(Expr& e) {
        TypeRef t = computeType(e);
        e.type = t;
        return t;
    }
    TypeRef computeType(Expr& e) {
        switch (e.kind) {
            case ExprKind::IntLit:    return tNamed(intLitType(e.text));
            case ExprKind::FloatLit:  return tNamed(floatLitType(e.text));
            case ExprKind::CharLit:   return tNamed("char");
            case ExprKind::StringLit: return tNamed("string");
            case ExprKind::InterpString:
                for (auto& a : e.args) {
                    const TypeRef ht = checkExpr(*a);
                    if (ht.nullable) // P40 A4 (N2): the targets render a null hole differently ("" / "null" / "None")
                        diags_.error(a->pos, "interpolation hole of type '" + showTy(ht) +
                                                 "' may be null; supply a fallback with '??' or assert with '!'");
                    requirePrintable(ht, a->pos, true); // P40 A3: a hole accepts exactly what print accepts
                }
                return tNamed("string");
            case ExprKind::BoolLit:   return tNamed("bool");
            case ExprKind::NullLit:   return tNullableUnknown();
            case ExprKind::This:      return currentThis_;
            case ExprKind::Name:      return checkName(e);
            case ExprKind::Unary:     return checkUnary(e);
            case ExprKind::Await:     { // `await e` unwraps `Awaitable<T>` (an async call's result) to `T` (§4.7)
                TypeRef t = checkExpr(*e.lhs);
                if (!inAsync_) { // placement error takes precedence; still unwrap so downstream typing is sane
                    diags_.error(e.pos, "'await' is only allowed inside an 'async fn' (PRD §4.7)");
                    return isAwaitable(t) && !t.args.empty() ? t.args[0] : t;
                }
                if (isAwaitable(t)) return t.args.empty() ? tUnknown() : t.args[0];
                // The only awaitables are async-call results; awaiting a plain value is a mistake. Diagnose it
                // when the operand's type is definitely known (a scalar or a nominal type), lenient otherwise.
                if (scalarTyOf(t) != Ty::Unknown || knownType(t))
                    diags_.error(e.pos, "'await' expects the result of an async call — this value is not "
                                        "awaitable (did you await a non-async call?) (PRD §4.7)");
                return t;
            }
            case ExprKind::Binary:    return checkBinary(e);
            case ExprKind::Cast:      return checkCast(e);
            case ExprKind::Is:        return checkIsAs(e, true);
            case ExprKind::As:        return checkIsAs(e, false);
            case ExprKind::Extern: // raw target code — type asserted by context (§4.4 FFI)
                if (!inActual_)
                    diags_.error(e.pos, "'extern' target code is only allowed in a target-gated 'actual' — "
                                        "portable code must stay target-neutral (PRD §4.4)");
                return tUnknown();
            case ExprKind::Range: // a `for` iterable never reaches here (checkRangeBounds) — anywhere else is an error
                diags_.error(e.pos, "a range ('a..b') is only valid as a for-loop iterable");
                checkExpr(*e.lhs); checkExpr(*e.rhs); // still type the bounds (their own errors, tooling)
                return tUnknown(); // unknown, so the enclosing expression does not cascade
            case ExprKind::Super:
                if (allowedSupers_.count(&e)) return tUnknown(); // the hoisted base-constructor `super(…)`
                diags_.error(e.pos, "'super' is only supported as a base-constructor call 'super(…)' at the top "
                                    "level of a constructor — 'super.m()' and bare 'super' are refused (Polyglot "
                                    "has no portable base-member access; PHP's parent:: differs)");
                return tUnknown();
            case ExprKind::Call:      return nullSafeCallResult(e, checkCall(e));
            case ExprKind::Member:    return checkMember(e);
            case ExprKind::Index:     { TypeRef recv = checkExpr(*e.lhs); for (auto& a : e.args) checkExpr(*a); checkIndexArity(e, recv); convertIndexArgs(e, recv); return elementType(recv); }
            case ExprKind::NullAssert:{
                TypeRef t = checkExpr(*e.lhs);
                if (t.kind == TypeRef::Kind::Named && t.name == "Option") // `opt!` (P40 D12): the element type
                    return t.args.empty() ? tUnknown() : t.args[0];
                t.nullable = false; return t;
            }
            case ExprKind::Lambda:    return checkLambda(e);
            case ExprKind::ListLit:   { // `[a, b, …]` -> List<T>, T inferred from the first element
                TypeRef elem = tUnknown();
                for (auto& a : e.args) { TypeRef t = checkExpr(*a); if (elem.name.empty() && t.kind == TypeRef::Kind::Named) elem = t; }
                TypeRef list; list.kind = TypeRef::Kind::Named; list.name = "List"; list.args.push_back(elem);
                return list;
            }
            case ExprKind::TupleLit:  { TypeRef t; t.kind = TypeRef::Kind::Tuple; for (auto& a : e.args) t.args.push_back(checkExpr(*a)); return t; }
            case ExprKind::With: {
                TypeRef base = checkExpr(*e.lhs);
                for (auto& f : e.fields) {
                    checkExpr(*f.value);
                    // P40 review r5 item 18: each override flows into its record field's (instantiated) type —
                    // `p with { x = null }` on an `i32` field was accepted unchecked.
                    if (!knownType(base)) continue;
                    MemberHit hit = findMemberOn(base, f.name);
                    if (hit.m && hit.m->isRecordField)
                        checkConvert(f.value, memberTypeOf(hit, hit.m->type, f.value->pos), "field '" + f.name + "' of 'with'");
                }
                return base;
            }
            case ExprKind::IfExpr:    {
                requireBool(checkExpr(*e.lhs), e.pos, "'if' condition");
                std::vector<std::string> whenTrue, whenFalse; // P40 D14: each branch sees its sense's facts
                nullFacts(*e.lhs, true, whenTrue);
                nullFacts(*e.lhs, false, whenFalse);
                std::vector<NarrowSave> saves;
                narrow(whenTrue, saves);
                TypeRef t = checkExpr(*e.rhs);
                restoreNarrow(saves);
                narrow(whenFalse, saves);
                TypeRef f = checkExpr(*e.extra);
                restoreNarrow(saves);
                // P40 A4 (S7 hole): a `null` branch makes the expression nullable — `if c { 1 } else { null }`
                // is `i32?`, not `i32` (the type comes from the non-null branch).
                if (t.kind == TypeRef::Kind::Named && t.name.empty() && t.nullable) { t = f; t.nullable = true; }
                else if (f.nullable) t.nullable = true;
                return t;
            }
            case ExprKind::Match:     return checkMatch(e);
        }
        return tUnknown();
    }

    // A member of the enclosing class reachable by its bare name (no `this`/`Type.` qualifier): a `const`
    // or any `static` field/property. Mirrors C#/the .pg surface where a class names its own statics plainly.
    const MemberInfo* enclosingStatic(const std::string& name) const {
        if (currentClass_.empty()) return nullptr;
        const MemberInfo* m = findMember(currentClass_, name);
        if (m && (m->kind == MemberKind::Const || m->isStatic) &&
            (m->kind == MemberKind::Const || m->kind == MemberKind::Field || m->kind == MemberKind::Property))
            return m;
        return nullptr;
    }

    // A write to a member slot (`Owner.f = …`, a bare static `f = …`, `recv.f = …`, compound or plain). A
    // `const` and a `static let` are never assignable (C# `const`/`static readonly`, a PHP class `const`); a
    // `let` instance field only through `this`, directly in a constructor of the class that declares it — the
    // C# `readonly` rule. Every other write used to reach the targets unchecked: C# refused it at compile
    // time (CS0131/CS0198/CS0191) while TS/Python/PHP silently mutated the "immutable" field.
    void checkMemberWritable(const MemberInfo& m, const std::string& owner, const std::string& shown,
                             bool viaThis, bool isStatic, SourcePos pos) {
        if (!m.isReadOnly) return;
        if (m.isRecordField) {
            diags_.error(pos, "cannot assign to record field '" + shown + "' — a record is immutable; build a "
                              "changed copy with 'with { … }'");
            return;
        }
        if (m.kind == MemberKind::Const) {
            diags_.error(pos, "cannot assign to constant '" + shown + "'");
            return;
        }
        if (isStatic) {
            diags_.error(pos, "cannot assign to immutable static field '" + shown + "' (declared with 'static let'); "
                              "declare it 'static var' to make it assignable");
            return;
        }
        const bool ownInit = viaThis && inCtor_ && lambdaDepth_ == 0 && owner == currentClass_;
        if (!ownInit)
            diags_.error(pos, "cannot assign to immutable field '" + shown + "' (declared with 'let') outside a "
                              "constructor of '" + owner + "'; declare it 'var' to make it assignable");
    }

    TypeRef checkName(Expr& e) {
        if (const Local* l = lookup(e.text)) {
            recordRef(e.pos, e.text, l->defId);
            lookupMut(e.text)->reads++; // P40 review r3: CS0219 tracking
            e.callStable = !l->isMutable || !closureAssigned_.count(e.text); // r5 item 23
            if (isOptionType(l->type) && narrowedHere(*l)) { // an Option local proven present: the read
                e.narrowedNonNull = true;                    // unwraps it (lowered like `o!`, SPEC §4.4)
                e.castType = l->type;                        // the local's own (Option) type, for lowering
                return l->type.args[0];
            }
            if (l->type.nullable && narrowedHere(*l)) { // P40 D14: proven non-null at this read
                e.narrowedNonNull = true;
                TypeRef t = l->type;
                t.nullable = false;
                return t;
            }
            e.narrowedNonNull = false;
            return l->type;
        }
        auto v = values_.find(e.text);
        if (v != values_.end()) {
            if (auto it = valueDefId_.find(e.text); it != valueDefId_.end()) recordRef(e.pos, e.text, it->second);
            if (auto dp = valueDeclPos_.find(e.text); dp != valueDeclPos_.end()) checkInitOrder(dp->second, e.text, e.pos);
            e.liveModuleRead = lambdaDepth_ > 0; // r5 (part 2): read when the lambda runs, not when it is made
            return v->second;
        }
        auto uc = unionCtors_.find(e.text);
        if (uc != unionCtors_.end() && uc->second.params.empty()) { noteTypeUse(e.text, e.pos); return uc->second.result; }
        if (const MemberInfo* m = enclosingStatic(e.text)) {
            e.staticOwner = currentClass_;
            if (auto dp = staticDeclPos_.find(currentClass_ + "." + e.text); dp != staticDeclPos_.end())
                checkInitOrder(dp->second, currentClass_ + "." + e.text, e.pos, "", currentClass_);
            return m->type;
        }
        if (auto f = fns_.find(e.text); f != fns_.end()) return checkFnRef(e, f->second);
        if (typeNames_.count(e.text) || unionCtors_.count(e.text)) return tUnknown();
        diags_.error(e.pos, "undeclared name '" + e.text + "'");
        return tUnknown();
    }

    // P40 review r4 items 18/20: a top-level function used as a VALUE (`apply(five)`). It is typed as its function
    // type — it was Unknown, so `run(five)` passed a value-returning fn into `() => unit` unchecked (C# CS0407) —
    // and lowered to a function reference every target can spell (PHP needs `five(...)`; it printed `$five`). Only
    // a single, non-generic user function without default parameters has one such type; anything else is refused
    // with the lambda to write instead.
    TypeRef checkFnRef(Expr& e, const std::vector<FnSig>& set) {
        std::vector<const FnSig*> cands;
        for (const auto& sig : set)
            if (set.size() == 1 || !modules_ || !modules_->visible || modules_->visible(e.pos.fileId, sig.pos.fileId, e.text))
                cands.push_back(&sig);
        if (cands.empty()) for (const auto& sig : set) cands.push_back(&sig);
        auto refuse = [&](const std::string& why) {
            diags_.error(e.pos, "function '" + e.text + "' cannot be used as a value: " + why +
                                " — pass a lambda that calls it instead ('(…) => " + e.text + "(…)')");
            return tUnknown();
        };
        if (cands.size() != 1) return refuse("it is overloaded");
        const FnSig& sig = *cands[0];
        if (sig.isPrelude) return refuse("it is a std library function");
        if (!sig.generics.empty()) return refuse("it is generic");
        if (sig.required != sig.params.size()) return refuse("it has default parameters");
        if (auto it = fnDefId_.find(e.text); it != fnDefId_.end()) recordRef(e.pos, e.text, it->second);
        e.fnRef = true;
        e.overloadName = sig.mangled.empty() ? e.text : sig.mangled;
        e.linkName = sig.linkName;     // provisional, like a call's: assignLinkNames rewrites it (review r5 #14)
        e.uniqueName = sig.uniqueName; // review r5 #14: PHP names the function by its unique name
        fnRefSites_.push_back({&e, sig.pos});
        TypeRef ft; ft.kind = TypeRef::Kind::Function;
        ft.args = sig.params;
        ft.ret.push_back(sig.isAsync ? awaitable(sig.result) : sig.result);
        return ft;
    }

    TypeRef checkLambda(Expr& e) {
        ++lambdaDepth_; // P40 D14: outer narrowings stop here; the lambda's own params/locals narrow inside it
        std::unordered_set<std::string> savedClosureAssigned;
        const bool topLevel = bodyDepth_ == 0 && e.flag; // a lambda outside any body (a module initializer)
        if (topLevel) {
            savedClosureAssigned = std::move(closureAssigned_);
            closureAssigned_.clear();
            collectAssigned(e.block, false, /*lambdaOnly=*/true, closureAssigned_);
            ++bodyDepth_;
        }
        pushScope();
        for (const auto& p : e.params) {
            if (!p.type.absent()) resolveTypeRef(p.type, p.pos);
            declare(p.name, p.type, false, p.pos);
        }
        // A block lambda's `return` is the LAMBDA's result, not the enclosing function's: it is checked
        // against the function type the lambda flows into (convertLambdaResult), so here it is open.
        TypeRef savedReturn = currentReturn_;
        currentReturn_ = tUnknown();
        if (e.flag) checkStmts(e.block);
        else if (e.lhs) checkExpr(*e.lhs);
        currentReturn_ = std::move(savedReturn);
        popScope();
        if (topLevel) { --bodyDepth_; closureAssigned_ = std::move(savedClosureAssigned); }
        --lambdaDepth_;
        // P40 review r6 (part 2): a lambda whose parameters are all typed and whose body is an expression has a
        // type every target can spell (as an immediately-invoked one already did) — so `let f = () => "x"` makes
        // `f()` a string, and PHP's `f() + "|"` is a concatenation (it emitted `+`: a TypeError).
        if (!e.flag && e.lhs && !(e.lhs->type.kind == TypeRef::Kind::Named && e.lhs->type.name.empty())) {
            TypeRef ft;
            ft.kind = TypeRef::Kind::Function;
            for (const auto& p : e.params) {
                if (p.type.absent()) return tUnknown();
                ft.args.push_back(p.type);
            }
            ft.ret.push_back(e.lhs->type);
            return ft;
        }
        return tUnknown();
    }

    // An explicit numeric conversion `(T)expr`. v0.1 casts are numeric<->numeric; the result is the
    // target type. Narrowing/lossy conversions are allowed *because* they're explicit. String/bool/char
    // are not numerically castable: string->number is std parsing, and char->ordinal is
    // `s.codePointAt(i)` (std.strings) — a char is a native char only on C# but a string on TS/Python/PHP,
    // so `(i32)c` cannot be honored uniformly and must refuse instead of miscompile (issue #34).
    TypeRef checkCast(Expr& e) {
        TypeRef from = checkExpr(*e.lhs);
        resolveTypeRef(e.castType, e.pos);
        // P37 C: a USER explicit conversion — `(T)v` where v's type declares `operator fn explicit(): T`.
        // C# emits its native `explicit operator`; method-form targets rewrite this cast site to the
        // generated `to<T>()` call (stamped here as the overloadName, carried to ir::Cast.convMethod).
        if (from.kind == TypeRef::Kind::Named && !from.name.empty() && knownType(from) &&
            !isBuiltinType(from.name) && from.name != "unit") {
            if (const TypeInfo* ti = typeInfoOf(from.name)) {
                for (const auto& m : ti->members)
                    if (m.kind == MemberKind::Operator && m.name == "explicit" &&
                        m.type.kind == TypeRef::Kind::Named && m.type.name == e.castType.name) {
                        e.overloadName = conversionMethodName(e.castType.name);
                        return e.castType;
                    }
            }
            diags_.error(e.pos, "no explicit conversion from '" + from.name + "' to '" + e.castType.name +
                                    "' — declare `operator fn explicit(): " + e.castType.name + "` on '" +
                                    from.name + "'");
            return e.castType;
        }
        Ty fs = scalarTyOf(from);
        if (isNumericTypeName(e.castType) && (fs == Ty::Bool || fs == Ty::String))
            diags_.error(e.pos, std::string("cannot cast ") + tyName(fs) + " to numeric type '" + e.castType.name + "'");
        if (isNumericTypeName(e.castType) && from.kind == TypeRef::Kind::Named && from.name == "char")
            diags_.error(e.pos, "cannot cast char to numeric type '" + e.castType.name +
                                    "' — use s.codePointAt(i) from std.strings for the ordinal");
        return e.castType;
    }
    // The generated conversion-method name shared by lowering and the cast-site rewrite: `toF64`, `toVec2`.
    static std::string conversionMethodName(const std::string& target) {
        std::string n = target;
        if (!n.empty()) n[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(n[0])));
        return "to" + n;
    }

    TypeRef checkUnary(Expr& e) {
        TypeRef ot = checkExpr(*e.lhs);
        requireNonNullOperand(ot, "unary " + e.text, e.pos); // P40 A4 (N1)
        Ty o = scalarTyOf(ot);
        if (e.text == "!") {
            if (o != Ty::Unknown && o != Ty::Bool) diags_.error(e.pos, std::string("'!' expects bool, found ") + tyName(o));
            return tNamed("bool");
        }
        // P37 C (#62): a user type must DECLARE `neg`/`bnot` for unary `-`/`~` — the old path silently
        // accepted `-v` on any user type and TS then emitted a native minus (NaN garbage).
        if (ot.kind == TypeRef::Kind::Named && !ot.name.empty() && knownType(ot) &&
            !isBuiltinType(ot.name) && ot.name != "unit") {
            const char* meth = e.text == "-" ? "neg" : e.text == "~" ? "bnot" : nullptr;
            if (meth) {
                if (MemberHit h = findMemberOn(ot, meth); h.m) return memberTypeOf(h, h.m->type, e.pos); // P40 s7: substituted
                diags_.error(e.pos, "type '" + ot.name + "' declares no 'operator fn " + meth +
                                        "' for unary '" + e.text + "'");
                return tUnknown();
            }
        }
        if (e.text == "~") return ot;
        if (o != Ty::Unknown && !isNumeric(o)) diags_.error(e.pos, std::string("unary '-' expects a numeric operand, found ") + tyName(o));
        return ot;
    }

    TypeRef checkBinary(Expr& e) {
        const std::string& op = e.text;
        TypeRef lt = checkExpr(*e.lhs);
        TypeRef rt;
        if (op == "&&" || op == "||") { // P40 D14: `x != null && …` / `x == null || …` narrow the right side
            std::vector<std::string> facts;
            nullFacts(*e.lhs, op == "&&", facts);
            std::vector<NarrowSave> saves;
            narrow(facts, saves);
            rt = checkExpr(*e.rhs);
            restoreNarrow(saves);
        } else {
            rt = checkExpr(*e.rhs);
        }
        // The T?-representation boundary (SPEC §4.4): an `Option<X>` compared with a native operand (`null`,
        // a `string?`, a `string`) compares as its native `X?` — `slot.peek() == null` is an absence test, not
        // a union-vs-null reference test (which was constant false). Two Options compare structurally (#57).
        if ((op == "==" || op == "!=") && isOptionType(lt) != isOptionType(rt)) {
            ExprPtr& side = isOptionType(lt) ? e.lhs : e.rhs;
            if (optionToNative(side, "'" + op + "' operand")) (isOptionType(lt) ? lt : rt) = side->type;
        }
        // P40 review r3 (part 2): comparing a value that cannot be null with `null` is constant — C# warns
        // CS0472 (`n == null` on an int), which a warnings-as-errors consumer cannot build.
        if (op == "==" || op == "!=") {
            // C#'s value types only (numbers, bool, char, enums): a reference type compared with null is legal
            // everywhere (and `m == null` on a user type is the documented null test, M3).
            auto neverNull = [&](const TypeRef& t) {
                return !t.nullable && t.kind == TypeRef::Kind::Named &&
                       ((isBuiltinType(t.name) && t.name != "unit" && t.name != "string") || enumNames_.count(t.name));
            };
            const bool lNull = e.lhs->kind == ExprKind::NullLit, rNull = e.rhs->kind == ExprKind::NullLit;
            if ((rNull && neverNull(lt)) || (lNull && neverNull(rt)))
                diags_.error(e.pos, "'" + op + " null' on a value of type '" + showTy(rNull ? lt : rt) +
                                        "', which is never null here — the result is constant");
        }
        Ty l = scalarTyOf(lt), r = scalarTyOf(rt);
        // P40 A4 (N1): only `==`, `!=` and `??` accept a maybe-null operand — every other operator would
        // diverge per target on null (C# lifts to null, JS coerces to 0/"null", Python/PHP throw or coerce).
        if (op != "==" && op != "!=" && op != "??") {
            requireNonNullOperand(lt, op, e.pos);
            requireNonNullOperand(rt, op, e.pos);
        }
        // P40 A4 (N3): `string + T` — a type parameter may be anything, incl. null and a non-scalar (the
        // targets stringify those differently, like `string + i32`, which is already refused).
        if (op == "+") {
            auto isTP = [&](const TypeRef& t) {
                return t.kind == TypeRef::Kind::Named && !t.nullable && genericsInScope_.count(t.name) > 0;
            };
            if ((l == Ty::String && isTP(rt)) || (r == Ty::String && isTP(lt)))
                diags_.error(e.pos, "'string + " + (isTP(rt) ? rt.name : lt.name) +
                                        "' concatenates a type parameter, which may be null or a non-scalar; "
                                        "interpolate instead (\"${…}\")");
        }

        if (op == "&&" || op == "||") {
            if ((l != Ty::Unknown && l != Ty::Bool) || (r != Ty::Unknown && r != Ty::Bool)) diags_.error(e.pos, "'" + op + "' expects bool operands");
            return tNamed("bool");
        }
        if (op == "??") { // null-coalescing
            if (lt.kind == TypeRef::Kind::Named && lt.name == "Option" && !lt.args.empty()) {
                // optional-generic: `opt ?? d` yields the element type; the fallback must produce it too.
                TypeRef elem = lt.args[0];
                checkConvert(e.rhs, elem, "'??' fallback");
                return elem;
            }
            // P40 review r3 item 6: a left operand that cannot be null (a non-nullable type, or a nullable one
            // narrowed by `!= null`) makes the fallback dead code — and C# rejects `x ?? 0` on an `int` (CS0019),
            // TS on a literal (TS2869). Refused rather than emitted.
            if (!lt.nullable &&
                (lt.kind == TypeRef::Kind::Function ||
                 (lt.kind == TypeRef::Kind::Named && !lt.name.empty() && lt.name != "null" && lt.name != "unit" &&
                  !genericsInScope_.count(lt.name) && lt.name != "Option" &&
                  (knownType(lt) || isBuiltinType(lt.name)))))
                diags_.error(e.pos, "the left operand of '??' has type '" + showTy(lt) + "', which cannot be null "
                                    "here, so the fallback is never used — remove the '??'");
            TypeRef res = lt; res.nullable = false; // native nullable: the left's type, made non-nullable
            if (res.name.empty() && res.kind == TypeRef::Kind::Named) res = rt;
            // …unless the fallback may itself be null: `a ?? b ?? 0` chains through a nullable `b` (P40 review r3)
            if (rt.nullable) res.nullable = true;
            return res;
        }
        // P37 C: a user-declared operator member resolves the op before any scalar rule (this is what
        // admits bitwise/shift overloads, #63 — the scalar shift/bitwise checks below would otherwise
        // reject a user operand). `x == null` still emits as a null TEST, never a user-eq call: the
        // rhsIsNullLit guard in lowering owns that routing (M3); here the types agree either way (bool).
        if (lt.kind == TypeRef::Kind::Named && !lt.name.empty() && knownType(lt) &&
            !isBuiltinType(lt.name) && lt.name != "unit") {
            const std::string meth = operatorMethod(op);
            if (!meth.empty())
                if (MemberHit h = findMemberOn(lt, meth); h.m) return memberTypeOf(h, h.m->type, e.pos); // P40 s7: substituted
        }
        TypeRef common;
        if (op == "==" || op == "!=") {
            // P40 review r6 (part 2): function values have no portable equality — C# rejects a lambda operand
            // (CS0019) and compares delegates by invocation list, TS/Python by identity, PHP closures by object
            // equality. Only the null test (`cb == null`, `cb != null`) is defined.
            const bool lNullLit = e.lhs->kind == ExprKind::NullLit, rNullLit = e.rhs->kind == ExprKind::NullLit;
            auto fnValue = [](const Expr& x, const TypeRef& t) {
                return x.kind == ExprKind::Lambda || t.kind == TypeRef::Kind::Function;
            };
            if (!lNullLit && !rNullLit && (fnValue(*e.lhs, lt) || fnValue(*e.rhs, rt))) {
                diags_.error(e.pos, "'" + op + "' on a function value is refused — the targets disagree on function "
                                    "equality (C# has none for a lambda: CS0019); only a null test ('f " + op +
                                    " null') is defined");
                return tNamed("bool");
            }
            if (reconcileNumeric(e, lt, rt, common)) return tNamed("bool"); // widen the narrower, then compare
            if (l != Ty::Unknown && r != Ty::Unknown && l != r) diags_.error(e.pos, "'" + op + "' compares mismatched types " + tyName(l) + " and " + tyName(r));
            return tNamed("bool");
        }
        if (op == "<" || op == "<=" || op == ">" || op == ">=") {
            if (reconcileNumeric(e, lt, rt, common)) return tNamed("bool");
            if (l != Ty::Unknown && r != Ty::Unknown && (l != r || !isNumeric(l))) diags_.error(e.pos, "'" + op + "' expects matching numeric operands, found " + tyName(l) + " and " + tyName(r));
            return tNamed("bool");
        }
        // Shifts are asymmetric (the C# rule, adopted for every target): the COUNT is a plain i32 — it
        // never reconciles with the shiftee (C# has no `long << long` / `uint << uint` operator), and
        // the result takes the SHIFTEE's type. (G4, wave 2.)
        // P40 review r3 item 6: `>>>` reached the targets verbatim — a Python SyntaxError, a PHP parse error,
        // and on TS `-8 >>> 0` is 4294967288 where C# gives -8. Refused (an unsigned type's `>>` is logical).
        if (op == ">>>") {
            diags_.error(e.pos, "'>>>' (unsigned shift) is not supported — the targets disagree on its result "
                                "type; shift an unsigned value with '>>' instead");
            return lt;
        }
        if (op == "<<" || op == ">>") {
            if (!(isSignedInt(lt.name) || isUnsignedInt(lt.name))) {
                diags_.error(e.pos, "'" + op + "' expects an integer left operand, found " + lt.name);
                return lt;
            }
            if (rt.name != "i32") {
                if (isSignedInt(rt.name) || isUnsignedInt(rt.name))
                    checkConvert(e.rhs, tNamed("i32"), "shift count");
                else
                    diags_.error(e.pos, "'" + op + "' expects an i32 shift count, found " + rt.name);
            }
            return lt;
        }
        // arithmetic / bitwise
        if (op == "+" && l == Ty::String && r == Ty::String) return tNamed("string");
        if (reconcileNumeric(e, lt, rt, common)) return common; // widen the narrower operand to the wider
        if (isNumericTypeName(lt) && isNumericTypeName(rt)) { // both numeric but neither widens -> needs a cast
            diags_.error(e.pos, "'" + op + "' has mismatched operand types " + lt.name + " and " + rt.name + "; add an explicit cast");
            return tUnknown();
        }
        if (l != Ty::Unknown && r != Ty::Unknown && (l != r || !isNumeric(l))) { diags_.error(e.pos, "'" + op + "' expects matching numeric operands, found " + tyName(l) + " and " + tyName(r)); return tUnknown(); }
        // a user type with an operator method (e.g. Vec2 + Vec2)
        if (knownType(lt)) { if (MemberHit h = findMemberOn(lt, operatorMethod(op)); h.m) return memberTypeOf(h, h.m->type, e.pos); }
        return tUnknown();
    }

    // ---- TypeArg inference: bind a callable's type params from the argument types, then substitute them
    // into its return type (so `fn pick<T>(a:T,b:T):T` called `pick(10i64,20i64)` returns i64, not bare T).
    void unifyGeneric(const std::unordered_set<std::string>& gen, const TypeRef& p, const TypeRef& a,
                      std::unordered_map<std::string, TypeRef>& binds) {
        if (p.kind == TypeRef::Kind::Named && p.args.empty() && gen.count(p.name)) {
            // Bind T to the argument type (first binding wins); ignore an unknown arg so we don't bind T=unknown.
            if (!binds.count(p.name) && !(a.kind == TypeRef::Kind::Named && a.name.empty())) binds[p.name] = a;
            return;
        }
        // A `T?` parameter (normalized to `Option<T>`) given a non-Option argument: a native `string?` or a
        // bare `string` binds T to `string` — the argument converts at the call (SPEC §4.4), it is not the
        // element type itself (`T = string?` would make the parameter an `Option<string?>` holding `Some(null)`).
        if (isOptionType(p) && !isOptionType(a) && !(a.kind == TypeRef::Kind::Named && a.name.empty())) {
            TypeRef bare = a;
            bare.nullable = false;
            unifyGeneric(gen, p.args[0], bare, binds);
            return;
        }
        for (std::size_t i = 0; i < p.args.size() && i < a.args.size(); ++i) unifyGeneric(gen, p.args[i], a.args[i], binds);
        for (std::size_t i = 0; i < p.ret.size() && i < a.ret.size(); ++i) unifyGeneric(gen, p.ret[i], a.ret[i], binds);
    }
    TypeRef substGeneric(const TypeRef& t, const std::unordered_set<std::string>& gen,
                         const std::unordered_map<std::string, TypeRef>& binds) {
        if (t.kind == TypeRef::Kind::Named && t.args.empty() && gen.count(t.name)) {
            auto it = binds.find(t.name);
            if (it == binds.end()) return t; // unbound -> leave the type-param name (lenient, as before)
            TypeRef r = it->second;
            if (t.nullable) r.nullable = true;
            return r;
        }
        TypeRef r = t;
        for (auto& a : r.args) a = substGeneric(a, gen, binds);
        for (auto& rt : r.ret) rt = substGeneric(rt, gen, binds);
        return r;
    }
    static bool mentionsName(const TypeRef& t, const std::unordered_set<std::string>& names) {
        if (t.kind == TypeRef::Kind::Named && names.count(t.name)) return true;
        for (const auto& a : t.args) if (mentionsName(a, names)) return true;
        for (const auto& r : t.ret) if (mentionsName(r, names)) return true;
        return false;
    }
    // #56b, generalized (P40 slice 7): a lambda argument with untyped params takes them from the (already
    // instantiated) function-typed parameter it flows into, then its body is re-checked so members on those
    // params bind. A param type still naming an open type parameter (`open`) is left alone.
    void instantiateLambdaArgs(const std::vector<TypeRef>& params, std::vector<ExprPtr>& args,
                               const std::unordered_set<std::string>& open) {
        for (std::size_t i = 0; i < params.size() && i < args.size(); ++i) {
            if (args[i]->kind != ExprKind::Lambda || params[i].kind != TypeRef::Kind::Function) continue;
            Expr& lam = *args[i];
            bool retyped = false;
            for (std::size_t j = 0; j < lam.params.size() && j < params[i].args.size(); ++j)
                if (lam.params[j].type.absent() && !params[i].args[j].absent() && !mentionsName(params[i].args[j], open)) {
                    lam.params[j].type = params[i].args[j]; retyped = true;
                }
            if (retyped) checkLambda(lam); // re-walk the body with the now-instantiated params
        }
    }
    TypeRef inferResult(const std::vector<std::string>& generics, const std::vector<TypeRef>& params,
                        const std::vector<TypeRef>& args, const TypeRef& result) {
        if (generics.empty()) return result;
        std::unordered_set<std::string> gen(generics.begin(), generics.end());
        std::unordered_map<std::string, TypeRef> binds;
        for (std::size_t i = 0; i < params.size() && i < args.size(); ++i) unifyGeneric(gen, params[i], args[i], binds);
        return substGeneric(result, gen, binds);
    }
    // Like inferResult, but also binds type params from the extension receiver (`List<T>` vs the actual
    // `List<i32>`), so an extension method's return type substitutes correctly.
    TypeRef inferExtResult(const std::vector<std::string>& generics, const TypeRef& recvDecl, const TypeRef& recvActual,
                           const std::vector<TypeRef>& params, const std::vector<TypeRef>& args, const TypeRef& result) {
        if (generics.empty()) return result;
        std::unordered_set<std::string> gen(generics.begin(), generics.end());
        std::unordered_map<std::string, TypeRef> binds;
        unifyGeneric(gen, recvDecl, recvActual, binds);
        for (std::size_t i = 0; i < params.size() && i < args.size(); ++i) unifyGeneric(gen, params[i], args[i], binds);
        return substGeneric(result, gen, binds);
    }

    // INumber enforcement: a generic param bounded by INumber must infer to a numeric type. Reject a
    // non-numeric type argument (e.g. `Math.max("a", "b")`) at Polyglot compile time rather than deferring to
    // the target (a C# overload error, or — worse — a silent TS NaN). An uninferred T is left alone (a
    // separate arity/type diagnostic covers it); so is a still-generic T (calling one generic from another).
    void checkNumericBounds(const std::vector<std::string>& numericParams, const std::vector<std::string>& generics,
                            const std::vector<TypeRef>& params, const std::vector<TypeRef>& args,
                            const std::string& what, SourcePos pos) {
        if (numericParams.empty()) return;
        std::unordered_set<std::string> gen(generics.begin(), generics.end());
        std::unordered_map<std::string, TypeRef> binds;
        for (std::size_t i = 0; i < params.size() && i < args.size(); ++i) unifyGeneric(gen, params[i], args[i], binds);
        for (const auto& n : numericParams) {
            auto it = binds.find(n);
            if (it == binds.end()) continue;
            const TypeRef& t = it->second;
            if (gen.count(t.name)) continue; // still generic (T bound to another type param) — not yet concrete
            if (t.kind == TypeRef::Kind::Named && !t.name.empty() && !isNumericTypeName(t))
                diags_.error(pos, what + " requires a numeric argument (type parameter '" + n +
                                      "' is constrained to INumber), found " + t.name);
        }
    }

    // P40 slice 7: does a (concrete) type argument satisfy one bound of its type parameter? INumber wants a
    // numeric scalar; an interface/class bound wants that type or a subtype (by name — the bound's own args are
    // not compared, matching the implements-conformance check). An unknown argument, or one that is itself a
    // type parameter in scope (a generic forwarding its own T), is not judged — the same leniency as
    // checkNumericBounds. A bound naming anything else (no declaration) was already an `unknown type` error.
    bool satisfiesBound(const TypeRef& arg, const TypeRef& bound) const {
        if (arg.kind != TypeRef::Kind::Named || arg.name.empty() || genericsInScope_.count(arg.name)) return true;
        if (bound.kind != TypeRef::Kind::Named) return true;
        if (bound.name == "INumber") return !arg.nullable && isNumericTypeName(arg);
        if (bound.name == "IMapKey") return mapKeyRefusal(arg).empty();
        if (!types_.count(bound.name)) return true;
        return arg.name == bound.name || inheritsFrom(arg.name, bound.name);
    }
    // P40 B4: why a type cannot key a Map ("" = it can). Only `string` and the integer scalars hash and compare
    // by value, identically, in every target's native dictionary (OrderedDictionary / Map / dict / a PHP array
    // behind \ArrayObject). Each refusal names the divergence it avoids.
    std::string mapKeyRefusal(const TypeRef& arg) const {
        if (arg.kind == TypeRef::Kind::Named && arg.name.empty()) return "";
        if (arg.kind == TypeRef::Kind::Named && genericsInScope_.count(arg.name)) return "";
        if (arg.kind == TypeRef::Kind::Named && arg.nullable)
            return "a nullable key is refused: null is not a dictionary key on every target";
        if (arg.kind != TypeRef::Kind::Named || !arg.args.empty())
            return "only 'string' and the integer scalars can key a Map";
        const std::string& n = arg.name;
        if (n == "string" || (isNumericTypeName(arg) && n != "f32" && n != "f64")) return "";
        if (n == "f32" || n == "f64")
            return "floating-point keys are refused: NaN never equals itself and -0.0/0.0 collide on some targets "
                   "but not others (key by an integer instead)";
        if (n == "bool")
            return "bool keys are refused: Python's dict treats True as the key 1 and PHP array keys coerce a bool "
                   "to 0/1, so they would collide with integer keys";
        if (n == "char")
            return "char keys are refused: a char is a one-unit string on TS/Python/PHP but a distinct integral "
                   "type on C#, so a char key and a string key would compare differently across targets";
        if (recordNames_.count(n))
            return "record keys are refused: a record compares structurally, but TS's Map and PHP arrays cannot "
                   "hash it structurally (key by a string or integer derived from its fields)";
        if (types_.count(n) && !enumNames_.count(n))
            return "class and interface keys are refused: object identity is not a PHP array key, and TS/Python "
                   "would key by identity where C# may use an overridden Equals";
        return "only 'string' and the integer scalars can key a Map";
    }
    static std::string describeBound(const TypeRef& b) {
        return b.name == "INumber" || b.name == "IMapKey" ? b.name : sigTypeName(b);
    }
    // The reason suffix a failed marker bound adds to its diagnostic.
    std::string boundReason(const TypeRef& arg, const TypeRef& b) const {
        if (b.name == "INumber") return " (a numeric type is required)";
        if (b.name == "IMapKey") return "; " + mapKeyRefusal(arg);
        return "";
    }
    // P40 B4: a nullable type argument for a type parameter `T` of an EXTERN class that has a member typed
    // `T?` is refused: the native binding's null would then mean both "absent" and "a stored null" (`Map.get`
    // with `V = i32?`). A user class is exempt — there `T?` with a nullable T is an Option of the argument.
    void checkNullableExternArgs(const std::string& typeName, const std::vector<TypeRef>& args, SourcePos pos) {
        if (!externClassNames_.count(typeName)) return;
        auto it = types_.find(typeName);
        if (it == types_.end()) return;
        const auto& gs = it->second.generics;
        for (std::size_t i = 0; i < gs.size() && i < args.size(); ++i) {
            if (!args[i].nullable) continue;
            for (const auto& m : it->second.members) {
                if (m.isStatic || m.type.kind != TypeRef::Kind::Named || !m.type.nullable || m.type.name != gs[i].name)
                    continue;
                bool hasHas = false;
                for (const auto& h : it->second.members) if (h.name == "has" && !h.isStatic) hasHas = true;
                diags_.error(pos, "type argument '" + showTy(args[i]) + "' for '" + gs[i].name + "' of '" + typeName +
                                      "' must not be nullable: '" + typeName + "." + m.name + "' returns '" +
                                      gs[i].name + "?', so an absent entry and a stored null would be "
                                      "indistinguishable" +
                                      (hasHas ? "; store a non-nullable value and test presence with '" + typeName +
                                                    ".has'"
                                              : std::string()));
                break;
            }
        }
    }
    // Nominal (non-INumber) bounds of a generic function or method, checked on the inferred arguments.
    // (INumber keeps its own historical message in checkNumericBounds.)
    void checkGenericBounds(const std::vector<GenericParam>& specs, const std::vector<std::string>& generics,
                            const std::vector<TypeRef>& params, const std::vector<TypeRef>& args,
                            const std::string& what, SourcePos pos) {
        bool any = false;
        for (const auto& g : specs) for (const auto& b : g.bounds) if (b.name != "INumber") any = true;
        if (!any) return;
        std::unordered_set<std::string> gen(generics.begin(), generics.end());
        std::unordered_map<std::string, TypeRef> binds;
        for (std::size_t i = 0; i < params.size() && i < args.size(); ++i) unifyGeneric(gen, params[i], args[i], binds);
        for (const auto& g : specs) {
            auto it = binds.find(g.name);
            if (it == binds.end() || gen.count(it->second.name)) continue;
            for (const auto& b : g.bounds)
                if (b.name != "INumber" && !satisfiesBound(it->second, b))
                    diags_.error(pos, what + ": type argument '" + showTy(it->second) + "' does not satisfy the bound '" +
                                          g.name + ": " + describeBound(b) + "'" + boundReason(it->second, b));
        }
    }
    // A generic TYPE's arguments against its params' bounds (`class Box<T: INumber>` refuses `Box<string>`),
    // wherever a type is spelled (annotations, construction type args) or inferred at a construction.
    void checkTypeArgBounds(const std::string& typeName, const std::vector<TypeRef>& args, SourcePos pos) {
        auto it = types_.find(typeName);
        if (it == types_.end() || args.empty()) return;
        const auto& gs = it->second.generics;
        for (std::size_t i = 0; i < gs.size() && i < args.size(); ++i)
            for (const auto& b : gs[i].bounds)
                if (!satisfiesBound(args[i], b))
                    diags_.error(pos, "type argument '" + showTy(args[i]) + "' of '" + typeName +
                                          "' does not satisfy the bound '" + gs[i].name + ": " + describeBound(b) +
                                          "'" + boundReason(args[i], b));
        checkNullableExternArgs(typeName, args, pos);
    }

    TypeRef checkMember(Expr& e) {
        // Enum case access: `EnumName.Case`.
        if (e.lhs->kind == ExprKind::Name && enumNames_.count(e.lhs->text)) { noteTypeUse(e.lhs->text, e.pos); return tNamed(e.lhs->text); }
        // Static member access `Type.MEMBER` (e.g. `Math.PI` on the std.math extern class): the LHS is a
        // type name, so resolve the member on the type rather than evaluating the LHS as a value.
        if (e.lhs->kind == ExprKind::Name && types_.count(e.lhs->text)) {
            noteTypeUse(e.lhs->text, e.pos);
            if (const MemberInfo* m = findMember(e.lhs->text, e.text)) {
                if (auto it = memberDefId_.find(e.lhs->text + "." + e.text); it != memberDefId_.end()) recordRef(e.pos, e.text, it->second);
                if (auto dp = staticDeclPos_.find(e.lhs->text + "." + e.text); dp != staticDeclPos_.end())
                    checkInitOrder(dp->second, e.lhs->text + "." + e.text, e.pos, "", e.lhs->text);
                TypeRef t = m->type; if (e.flag) t.nullable = true; return t;
            }
            diags_.error(e.pos, "type '" + e.lhs->text + "' has no member '" + e.text + "'");
            return tUnknown();
        }

        TypeRef recv = checkExpr(*e.lhs);
        requireNonNullReceiver(recv, e.text, e.flag, e.pos); // P40 A4 (S7 hole)
        TypeRef result = tUnknown();
        if (knownType(recv)) {
            if (MemberHit hit = findMemberOn(recv, e.text); hit.m) {
                result = memberTypeOf(hit, hit.m->type, e.pos); // P40 slice 7: `Box<i32>.value` is i32, not T
                if (hit.m->kind == MemberKind::Property) // review r5 #4: reading a computed property runs its getter
                    noteDirectCall("m:" + hit.owner + "." + e.text, recv.name + "." + e.text, e.pos);
                if (auto it = memberDefId_.find(recv.name + "." + e.text); it != memberDefId_.end()) recordRef(e.pos, e.text, it->second);
            } else diags_.error(e.pos, "type '" + recv.name + "' has no member '" + e.text + "'");
        } else if (recv.kind == TypeRef::Kind::Function) { // P40 review r3 item 6: was accepted, emitted verbatim
            diags_.error(e.pos, "a function value of type '" + showTy(recv) + "' has no member '" + e.text + "'");
        }
        if (e.flag) result.nullable = true; // null-safe `?.`
        return result;
    }

    // `required` is the minimum arg count (params with no default); SIZE_MAX means "all params required".
    // A call through a function value: the args convert into the function type's parameters and the call
    // yields its return type. A maybe-null function value must be narrowed first; a value of a non-function
    // type cannot be called. An Unknown callee (error recovery, an uninstantiated lambda param) stays lenient.
    TypeRef callFunctionValue(const TypeRef& ft, const std::vector<TypeRef>& argTypes, Expr& e, const std::string& what) {
        if (ft.kind == TypeRef::Kind::Function) {
            if (ft.nullable)
                diags_.error(e.pos, what + " has type '" + showTy(ft) + "', which may be null; narrow it first");
            checkArgs(ft.args, argTypes, e.args, what, e.pos);
            return ft.ret.empty() ? tUnknown() : ft.ret[0];
        }
        if (ft.kind == TypeRef::Kind::Named && !ft.name.empty() && (knownType(ft) || isBuiltinType(ft.name)))
            diags_.error(e.pos, what + " has type '" + showTy(ft) + "', which is not a function and cannot be called");
        return tUnknown();
    }

    void checkArgs(const std::vector<TypeRef>& params, const std::vector<TypeRef>& argTypes,
                   std::vector<ExprPtr>& args, const std::string& what, SourcePos pos,
                   std::size_t required = static_cast<std::size_t>(-1)) {
        if (required == static_cast<std::size_t>(-1)) required = params.size();
        if (argTypes.size() < required || argTypes.size() > params.size()) {
            std::string want = required == params.size() ? std::to_string(params.size())
                                                         : std::to_string(required) + "-" + std::to_string(params.size());
            diags_.error(pos, what + " expects " + want + " argument(s), got " + std::to_string(argTypes.size()));
            return;
        }
        // Each supplied argument flows into its parameter slot: widen losslessly, else require an explicit cast.
        for (std::size_t i = 0; i < argTypes.size(); ++i)
            checkConvert(args[i], params[i], "argument " + std::to_string(i + 1) + " of " + what);
    }

    // P40 review r5 item 20: `recv?.m(args)` on a maybe-null receiver yields `T?` — the call is skipped when the
    // receiver is null. It used to keep the call's plain type and lose the `?.` in every emitter (`let w: i32 =
    // r?.val()` was accepted; C# CS8602, a TS TypeError at run time). On a receiver that cannot be null the `?.`
    // changes nothing and the call keeps its type. A unit call stays unit (it is only ever a statement).
    TypeRef nullSafeCallResult(Expr& e, TypeRef t) {
        if (!e.lhs || e.lhs->kind != ExprKind::Member || !e.lhs->flag || !e.lhs->lhs) return t;
        const TypeRef& recv = e.lhs->lhs->type;
        if (!recv.nullable || (recv.kind == TypeRef::Kind::Named && recv.name.empty())) return t;
        if (e.lhs->lhs->kind == ExprKind::Name && types_.count(e.lhs->lhs->text) && !lookup(e.lhs->lhs->text)) return t;
        if (isOptionType(t)) {
            diags_.error(e.pos, "'?." + e.lhs->text + "(…)' returns '" + showTy(t) + "' (a generic 'T?'), which has no "
                                "null-safe form — test the receiver first ('if x != null { … }')");
            return t;
        }
        e.nullSafeCall = true;
        if (isUnitT(t) || (t.kind == TypeRef::Kind::Named && t.name.empty())) return t;
        t.nullable = true;
        return t;
    }

    TypeRef checkCall(Expr& e) {
        // P37 D: the Meta intrinsics — resolved before ordinary checking (`Meta` is a compiler name
        // unless the user shadows it with a type or local of their own).
        if (e.lhs && e.lhs->kind == ExprKind::Member && e.lhs->lhs && e.lhs->lhs->kind == ExprKind::Name &&
            e.lhs->lhs->text == "Meta" && !typeNames_.count("Meta") && !lookup("Meta"))
            return checkMetaCall(e);
        // P37 D.3 anti-silent-drop: explicit type args on a member call used to parse and be silently
        // IGNORED by every resolution path (inference-only). Now that Meta consumes them, any other
        // member call carrying them refuses instead of dropping them.
        if (e.lhs && e.lhs->kind == ExprKind::Member && !e.typeArgs.empty())
            diags_.error(e.pos, "explicit type arguments are not supported on this call (they are "
                                "inferred from the arguments; only construction and Meta take them)");
        // P37 D Tier 2: attribute values are constructed by the compiler (Meta.get/member) from the
        // attachment's recorded constants — never directly (an unqueried attribute emits no type to
        // construct against).
        if (e.lhs && e.lhs->kind == ExprKind::Name && attributeNames_.count(e.lhs->text)) {
            diags_.error(e.pos, "an `attribute` type ('" + e.lhs->text + "') cannot be constructed "
                                "directly — attach it with [" + e.lhs->text + "(…)] and read it back "
                                "via Meta.get/Meta.member");
            return tUnknown();
        }

        std::vector<TypeRef> argTypes;
        for (auto& a : e.args) {
            argTypes.push_back(checkExpr(*a));
            // review r5 (part 2): a function passed as an argument may run inside the call — follow its body
            if (a->kind == ExprKind::Name && a->fnRef && !fnRefSites_.empty() && fnRefSites_.back().ref == a.get())
                noteDirectCall(fnBodyKey(fnRefSites_.back().decl), a->text, a->pos);
        }

        if (e.lhs->kind == ExprKind::Member) {
            // Method call `recv.method(args)`.
            const std::string& method = e.lhs->text;
            // Static call `Type.method(args)`: the receiver is a type name, not an evaluated value.
            if (e.lhs->lhs->kind == ExprKind::Name) {
                const std::string& typeName = e.lhs->lhs->text;
                // Builtin `Math` namespace. `min`/`max`/`abs` are type-preserving (like C#'s overloads:
                // (`Math` is no longer a builtin namespace — it's the std.math `extern class`, resolved by
                // the general static-method path below; min/max/abs are generic and TypeArg-inferred.)
                // Primitive static intrinsic: `i32.parse(s)` / `f64.parse(s)` — string -> that numeric type.
                if (isNumericTypeName(tNamed(typeName)) && method == "parse") {
                    Ty a = argTypes.size() == 1 ? scalarTyOf(argTypes[0]) : Ty::Unknown;
                    if (argTypes.size() != 1 || (a != Ty::Unknown && a != Ty::String))
                        diags_.error(e.pos, "'" + typeName + ".parse' expects a single string argument");
                    return tNamed(typeName);
                }
                if (types_.count(typeName)) {
                    noteTypeUse(typeName, e.pos);
                    // P40 review r4: `K.g(4)` where `g` is a static FIELD of function type calls the field's value.
                    if (const MemberInfo* m = findMember(typeName, method);
                        m && m->isStatic && isValueMember(*m) && m->type.kind == TypeRef::Kind::Function) {
                        TypeRef ft = checkExpr(*e.lhs); // the static field read (`K.g`), typed and stamped
                        e.lhs->type = ft;
                        e.calleeIsField = true;
                        return callFunctionValue(ft, argTypes, e, "field '" + typeName + "." + method + "'");
                    }
                    if (const MemberInfo* m = findMember(typeName, method); m && m->isStatic) {
                        { // review r4 (part 2): a static call runs the declaring class's body right away
                            MemberHit owner = findMemberOn(tNamed(typeName), method);
                            noteDirectCall("st:" + (owner.m ? owner.owner : typeName) + "." + method, typeName + "." + method, e.pos);
                        }
                        if (auto it = memberDefId_.find(typeName + "." + method); it != memberDefId_.end()) recordRef(e.lhs->pos, method, it->second);
                        std::string what = "static method '" + typeName + "." + method + "'";
                        checkArgs(m->params, argTypes, e.args, what, e.pos, m->required);
                        checkNumericBounds(m->numericParams, m->generics, m->params, argTypes, what, e.pos);
                        checkGenericBounds(m->boundSpecs, m->generics, m->params, argTypes, what, e.pos);
                        TypeRef r = inferResult(m->generics, m->params, argTypes, m->type);
                        return m->isAsync ? awaitable(r) : r;
                    }
                    diags_.error(e.pos, "type '" + typeName + "' has no static method '" + method + "'");
                    return tUnknown();
                }
            }
            TypeRef recv = checkExpr(*e.lhs->lhs);
            { // P40 A4 (S7 hole): a maybe-null receiver — unless an extension declares a nullable receiver
                bool extTakesNull = false;
                if (recv.kind == TypeRef::Kind::Named)
                    if (auto it = extensions_.find(recv.name + "." + method); it != extensions_.end())
                        extTakesNull = it->second.receiver.nullable;
                if (!extTakesNull) requireNonNullReceiver(recv, method, e.lhs->flag, e.lhs->pos);
            }
            // An extension method augments any receiver type (scalar or nominal); check it first so a
            // `type 'T' has no method` error doesn't fire for a method an extension actually supplies.
            if (recv.kind == TypeRef::Kind::Named) {
                if (auto it = extensions_.find(recv.name + "." + method); it != extensions_.end()) {
                    noteDirectCall("ext:" + recv.name + "." + method, recv.name + "." + method, e.pos); // review r5 #4
                    // #56b: bidirectionally instantiate a lambda arg's param types from the extension's
                    // function-typed param (generics bound from the receiver — List<T> vs List<string> -> T=
                    // string), then RE-CHECK the lambda body so std methods on those params bind (else
                    // `s.len()` stays Unknown -> emitted verbatim -> runtime crash).
                    if (!it->second.generics.empty()) {
                        std::unordered_set<std::string> gen(it->second.generics.begin(), it->second.generics.end());
                        std::unordered_map<std::string, TypeRef> binds;
                        unifyGeneric(gen, it->second.receiver, recv, binds);
                        std::vector<TypeRef> ps;
                        for (const auto& p : it->second.params) ps.push_back(substGeneric(p, gen, binds));
                        instantiateLambdaArgs(ps, e.args, {}); // (as before: an unbound T is kept as written)
                    }
                    // P40 slice 7: trailing default params may be omitted; lowering fills them in at the call.
                    // P40 review A7: arguments convert into the INSTANTIATED parameters (type params bound from
                    // the receiver and the arguments), never into a still-open `T?` (`new None<T>()`).
                    std::vector<TypeRef> psInst = it->second.params;
                    if (!it->second.generics.empty()) {
                        std::unordered_set<std::string> gen(it->second.generics.begin(), it->second.generics.end());
                        std::unordered_map<std::string, TypeRef> binds;
                        unifyGeneric(gen, it->second.receiver, recv, binds);
                        for (std::size_t i = 0; i < psInst.size() && i < argTypes.size(); ++i)
                            unifyGeneric(gen, psInst[i], argTypes[i], binds);
                        for (auto& p : psInst) p = substGeneric(p, gen, binds);
                    }
                    checkArgs(psInst, argTypes, e.args, "extension '" + method + "'", e.pos, it->second.required);
                    // Infer the extension's type params from the receiver (List<i32> vs List<T> -> T=i32) and
                    // args, then substitute the result — so `xs.secondOrNull()` is Option<i32>, not Option<T>.
                    return inferExtResult(it->second.generics, it->second.receiver, recv, it->second.params, argTypes, it->second.result);
                }
            }
            if (knownType(recv)) {
                if (MemberHit hit = findMemberOn(recv, method); hit.m) {
                    const MemberInfo* m = hit.m;
                    // P40 review r4: `h.f(4)` where `f` is a FIELD (or property) of function type calls its value
                    // — it was checked as a method and refused as "method 'f' expects 0 argument(s)".
                    if (isValueMember(*m) && m->type.kind == TypeRef::Kind::Function) {
                        if (auto it = memberDefId_.find(recv.name + "." + method); it != memberDefId_.end()) recordRef(e.lhs->pos, method, it->second);
                        // `h?.f(4)` (r5): the call runs only when `h` is present — nullSafeCallResult makes the
                        // result nullable; the field's own value is the declared (non-null) function.
                        TypeRef ft = memberTypeOf(hit, m->type, e.pos);
                        e.lhs->type = ft;
                        e.calleeIsField = true;
                        return callFunctionValue(ft, argTypes, e, "field '" + hit.owner + "." + method + "'");
                    }
                    // P40 review r3 item 6: an operator member is reached through its operator, never by name —
                    // C# emits `operator +`/`GetHashCode`/an indexer, so `m.plus(o)`/`m.hash()` is CS1061 there
                    // (and an AttributeError on Python, where `hash` becomes `__hash__`).
                    if (m->kind == MemberKind::Operator)
                        diags_.error(e.pos, "'" + method + "' is an operator member of '" + hit.owner +
                                                "'; use its operator instead of calling it by name");
                    // review r5 #4: an instance method call runs that body (or an override's) right away
                    noteDirectCall("m:" + hit.owner + "." + method, recv.name + "." + method, e.pos);
                    if (auto it = memberDefId_.find(recv.name + "." + method); it != memberDefId_.end()) recordRef(e.lhs->pos, method, it->second);
                    // P40 slice 7: the class's type params come from the receiver (`Box<i32>.set(v: T)` takes
                    // an i32), then the method's own params are inferred from the args as for a free fn.
                    std::vector<TypeRef> ps = memberParamsOf(hit, e.pos);
                    {
                        std::unordered_set<std::string> open; // still-unbound class params: never retype with them
                        for (const auto& g : hit.gen) if (!hit.binds.count(g) && !genericsInScope_.count(g)) open.insert(g);
                        for (const auto& g : m->generics) open.insert(g);
                        instantiateLambdaArgs(ps, e.args, open);
                    }
                    const std::string what = "method '" + method + "'";
                    // P40 review A7: bind the method's OWN type params from the arguments first, so each
                    // argument converts into its instantiated parameter (as a free fn's do) — a native `string?`
                    // into `U?` is `Option<string>`, not `Option<U>` (C# `new None<U>()` named the open param).
                    std::vector<TypeRef> psInst = ps;
                    if (!m->generics.empty()) {
                        std::unordered_set<std::string> mg(m->generics.begin(), m->generics.end());
                        std::unordered_map<std::string, TypeRef> mb;
                        for (std::size_t i = 0; i < psInst.size() && i < argTypes.size(); ++i)
                            unifyGeneric(mg, psInst[i], argTypes[i], mb);
                        for (auto& p : psInst) p = substGeneric(p, mg, mb);
                    }
                    checkArgs(psInst, argTypes, e.args, what, e.pos, m->required);
                    checkNumericBounds(m->numericParams, m->generics, ps, argTypes, what, e.pos);
                    checkGenericBounds(m->boundSpecs, m->generics, ps, argTypes, what, e.pos);
                    TypeRef r = inferResult(m->generics, ps, argTypes, memberTypeOf(hit, m->type, e.pos));
                    return m->isAsync ? awaitable(r) : r;
                }
                diags_.error(e.pos, "type '" + recv.name + "' has no method '" + method + "'");
            }
            // #56a: a method call on a CONCRETE primitive receiver (string/i32/bool/char/f64/…) that bound
            // no extension and no member is a guaranteed runtime crash — refuse it (usually a missing
            // `import`) instead of emitting it verbatim. Unknown and still-generic receivers stay lenient (an
            // uninstantiated lambda param resolves later — #56b), because scalarTyOf is Unknown for those.
            else if (recv.kind == TypeRef::Kind::Named && scalarTyOf(recv) != Ty::Unknown)
                diags_.error(e.pos, "no method '" + method + "' on '" + recv.name +
                                        "' — is an import missing (e.g. \"std.strings\")? (PRD §3.B)");
            // P40 review r4 item 19: a method call on a function value (`g.toString()`) — like member access
            // on one (r3 item 6). C# CS1061, a Python AttributeError, a PHP fatal; TS printed the source.
            else if (recv.kind == TypeRef::Kind::Function)
                diags_.error(e.pos, "a function value of type '" + showTy(recv) + "' has no method '" + method +
                                        "' — call it ('f(…)') instead");
            return tUnknown();
        }
        // P40 review r3 2(a): any other callee is a function VALUE (`adder(1)(4)`, `fs[0](4)`, an IIFE) — its
        // arguments check against the function type and the call yields its return type, like a named fn.
        if (e.lhs->kind == ExprKind::Lambda) { // an immediately-invoked lambda: its type is spelled from its parts
            { // review r4 (part 2): it runs right here — inside an initializer its body is not deferred
                const int floor = initLambdaFloor_;
                if (lambdaDepth_ <= initLambdaFloor_) initLambdaFloor_ = lambdaDepth_ + 1;
                checkExpr(*e.lhs);
                initLambdaFloor_ = floor;
            }
            const Expr& lam = *e.lhs;
            bool typed = !lam.flag && lam.lhs && !lam.lhs->type.absent() && !lam.lhs->type.name.empty();
            for (const auto& p : lam.params) typed = typed && !p.type.absent();
            if (!typed) {
                diags_.error(e.pos, "an immediately-invoked lambda needs typed parameters and an expression body "
                                    "(C# spells its delegate type); bind it to a typed local first");
                return tUnknown();
            }
            TypeRef ft; ft.kind = TypeRef::Kind::Function;
            for (const auto& p : lam.params) ft.args.push_back(p.type);
            ft.ret.push_back(lam.lhs->type);
            e.lhs->type = ft;
            return callFunctionValue(ft, argTypes, e, "this lambda");
        }
        if (e.lhs->kind != ExprKind::Name) return callFunctionValue(checkExpr(*e.lhs), argTypes, e, "this call");

        const std::string& name = e.lhs->text;
        // Call-syntax primitive cast `i32(x)` / `f64(n)`: the numeric scalars are extern-class prelude types
        // (so `i32.parse` resolves), which would otherwise resolve `i32(x)` as construction -> `new i32(x)`,
        // invalid in every target (issue #9 Bug 1). Rewrite it into the identical `(i32)x` Cast node so it
        // reuses the existing, correct Cast lowering/emission. Also turns the previously-silent bad forms
        // (`i32()`, `i32(a,b)`, `i32(true)`) into diagnostics.
        if (isNumericTypeName(tNamed(name))) {
            if (e.args.size() != 1) {
                diags_.error(e.pos, "cast to '" + name + "' expects a single argument");
                return tNamed(name);
            }
            Ty fs = scalarTyOf(argTypes[0]);
            if (fs == Ty::Bool || fs == Ty::String)
                diags_.error(e.pos, std::string("cannot cast ") + tyName(fs) + " to numeric type '" + name + "'");
            if (argTypes[0].kind == TypeRef::Kind::Named && argTypes[0].name == "char")
                diags_.error(e.pos, "cannot cast char to numeric type '" + name +
                                        "' — use s.codePointAt(i) from std.strings for the ordinal");
            TypeRef target = tNamed(name);
            resolveTypeRef(target, e.pos);
            e.castType = target;
            e.type = target;
            e.lhs = std::move(e.args[0]); // replace the `i32` callee with the operand; `name` now dangles
            e.args.clear();
            e.kind = ExprKind::Cast;
            return target;
        }
        // `print` is the std.io `print<T>` function (import-only), but it keeps a printable-arg guard: its
        // generic signature accepts any T, yet a non-scalar would emit divergent output (C# ToString vs JS
        // String) — a §3 miscompile. Diagnose here, then fall through to normal resolution (so it's still
        // import-gated and emits as an ordinary call to std.io.print).
        if (name == "print" && argTypes.size() == 1) {
            if (isAwaitable(argTypes[0])) // an un-awaited async result would print a Task/Promise — a §3 divergence
                diags_.error(e.pos, "cannot print the result of an async call directly — did you forget 'await'? (PRD §4.7)");
            if (argTypes[0].nullable) // P40 A4 (N2)
                diags_.error(e.pos, "print argument of type '" + showTy(argTypes[0]) +
                                        "' may be null; supply a fallback with '??' or assert with '!'");
            if (!isAwaitable(argTypes[0])) requirePrintable(argTypes[0], e.pos, false); // P40 A3 + D1
        }
        if (const Local* l = lookup(name)) { // calling a function-valued local (lambda/delegate)
            recordRef(e.lhs->pos, name, l->defId);
            lookupMut(name)->reads++;
            // P40 review r4 item 15: a nullable function local proven non-null (`if cb != null { cb() }`) is
            // called through its narrowed type, exactly like a narrowed read (checkName).
            TypeRef ft = l->type;
            if (ft.nullable && narrowedHere(*l)) ft.nullable = false;
            e.lhs->type = ft;
            // P40 review r6: a local that shadows a top-level function of the same name is called through its
            // VALUE — lowering otherwise took the name for the free function (C# `PolyglotProgram.f()`).
            if (fns_.count(name)) e.calleeIsField = true;
            return callFunctionValue(ft, argTypes, e, "'" + name + "'");
        }
        if (auto t = types_.find(name); t != types_.end()) { // construction
            noteTypeUse(name, e.pos);
            noteDirectCall("ctor:" + name, name, e.pos); // review r5 #4: the constructor (and field initializers) run now
            if (abstractClassNames_.count(name)) // review r5 #9
                diags_.error(e.pos, "cannot construct abstract class '" + name + "' — construct a concrete subclass");
            if (auto it = typeDefId_.find(name); it != typeDefId_.end()) recordRef(e.lhs->pos, name, it->second);
            // P40 slice 7: a generic type's construction binds its type params — from explicit args
            // (`Box<i32>(7)`), else inferred from the ctor args (`Box(7)` is Box<i32>, and the args are
            // recorded so every target spells them: C# has no constructor type inference). The ctor params are
            // then checked against the instantiated types, and the arguments against the params' bounds.
            const auto& gs = t->second.generics;
            std::vector<TypeRef> ctorPs = t->second.ctorParams;
            if (!gs.empty()) {
                std::unordered_set<std::string> gen; for (const auto& g : gs) gen.insert(g.name);
                std::unordered_map<std::string, TypeRef> binds;
                if (!e.typeArgs.empty()) {
                    if (e.typeArgs.size() != gs.size())
                        diags_.error(e.pos, "'" + name + "' takes " + std::to_string(gs.size()) +
                                                " type argument(s), got " + std::to_string(e.typeArgs.size()));
                    for (auto& ta : e.typeArgs) { normalizeOptional(ta, genericsInScope_); resolveTypeRef(ta, e.pos); }
                    for (std::size_t i = 0; i < gs.size() && i < e.typeArgs.size(); ++i) binds[gs[i].name] = e.typeArgs[i];
                    checkTypeArgBounds(name, e.typeArgs, e.pos);
                } else if (t->second.hasCtor) {
                    for (std::size_t i = 0; i < ctorPs.size() && i < argTypes.size(); ++i) unifyGeneric(gen, ctorPs[i], argTypes[i], binds);
                    bool all = true;
                    for (const auto& g : gs) {
                        auto b = binds.find(g.name);
                        if (b == binds.end() || b->second.kind != TypeRef::Kind::Named) { all = false; break; }
                    }
                    if (all) {
                        for (const auto& g : gs) e.typeArgs.push_back(binds[g.name]);
                        checkTypeArgBounds(name, e.typeArgs, e.pos);
                    } else binds.clear(); // not fully inferable: lenient, as before (an annotation may supply it)
                }
                for (auto& p : ctorPs) p = substGeneric(p, gen, binds);
            }
            if (t->second.hasCtor) checkArgs(ctorPs, argTypes, e.args, "'" + name + "'", e.pos, t->second.ctorRequired);
            TypeRef r = tNamed(name); r.args = e.typeArgs; // carry `Box<i32>()`'s args so the result is Box<i32>
            return r;
        }
        if (auto uc = unionCtors_.find(name); uc != unionCtors_.end()) {
            noteTypeUse(name, e.pos);
            checkArgs(uc->second.params, argTypes, e.args, "'" + name + "'", e.pos);
            return inferResult(uc->second.generics, uc->second.params, argTypes, uc->second.result); // Some(5) -> Option<i32>
        }
        if (auto f = fns_.find(name); f != fns_.end()) {
            if (auto it = fnDefId_.find(name); it != fnDefId_.end()) recordRef(e.lhs->pos, name, it->second);
            // P40 review A8 (D16): only the overloads the CALLING module can see compete — a never-imported
            // overload in another module must not win (C# bound it, TS called an unimported mangled name). When
            // none is visible the set is kept whole, and the scope pass refuses the name with its import hint.
            std::vector<FnSig> visibleSet;
            const std::vector<FnSig>* candidates = &f->second;
            if (modules_ && modules_->visible && f->second.size() > 1) {
                for (const auto& sig : f->second)
                    if (modules_->visible(e.pos.fileId, sig.pos.fileId, name)) visibleSet.push_back(sig);
                if (!visibleSet.empty()) candidates = &visibleSet;
            }
            // P40 review r4 #12: an overload that can never take these arguments (a string for a `Shape`) does not
            // compete — resolveOverload scores a user-typed parameter leniently, and `show("text")` bound
            // `show(Shape)` beside an applicable `show<T>`. When none applies, the whole set stays (checkArgs
            // then names the mismatch).
            std::vector<FnSig> applicableSet;
            if (candidates->size() > 1) {
                for (const auto& sig : *candidates) if (mayApply(sig, argTypes)) applicableSet.push_back(sig);
                if (!applicableSet.empty() && applicableSet.size() < candidates->size()) candidates = &applicableSet;
            }
            const FnSig* chosen = resolveOverload(*candidates, argTypes);
            if (!chosen) { diags_.error(e.pos, "no overload of '" + name + "' matches the argument types"); return tUnknown(); }
            // Bind the type params first — from explicit `f<T>(…)` args (P40 A4, emitted) or inferred from the
            // argument types — so each argument converts into its INSTANTIATED parameter: `present(s)` into
            // `Option<string>` (SPEC §4.4), `first([4, 5])` into `i32[]`. Checking against the raw `T`-typed
            // parameter typed those conversions with the type parameter (C# `new None<T>()`, `new T[]`).
            std::unordered_set<std::string> gen(chosen->generics.begin(), chosen->generics.end());
            std::unordered_map<std::string, TypeRef> binds;
            bool explicitArgs = false;
            if (!e.typeArgs.empty()) {
                if (e.typeArgs.size() != chosen->generics.size()) {
                    diags_.error(e.pos, "'" + name + "' takes " + std::to_string(chosen->generics.size()) +
                                            " type argument(s), got " + std::to_string(e.typeArgs.size()));
                } else {
                    for (auto& ta : e.typeArgs) { normalizeOptional(ta, genericsInScope_); resolveTypeRef(ta, e.pos); }
                    for (std::size_t i = 0; i < chosen->generics.size(); ++i) binds[chosen->generics[i]] = e.typeArgs[i];
                    explicitArgs = true;
                }
            }
            if (!explicitArgs) {
                bool viaBoundary = false;
                for (std::size_t i = 0; i < chosen->params.size() && i < argTypes.size(); ++i) {
                    unifyGeneric(gen, chosen->params[i], argTypes[i], binds);
                    if (isOptionType(chosen->params[i]) && !isOptionType(argTypes[i])) viaBoundary = true;
                }
                // A type param bound THROUGH the T?-boundary (a `string?` argument for an `Option<T>` param) is
                // invisible to the target's own inference, which sees only the converted match expression (TS
                // infers `T` from its object literals as `null | undefined`). Record the binding so every
                // target spells it, as an inferred construction does (P40 slice 7).
                bool allBound = viaBoundary;
                for (const auto& g : chosen->generics) if (!binds.count(g)) allBound = false;
                if (allBound) for (const auto& g : chosen->generics) e.typeArgs.push_back(binds[g]);
                refuseUnitInference(*chosen, binds, e, name, argTypes);
            }
            std::vector<TypeRef> ps;
            for (const auto& p : chosen->params) ps.push_back(gen.empty() ? p : substGeneric(p, gen, binds));
            checkArgs(ps, argTypes, e.args, "'" + name + "'", e.pos, chosen->required);
            checkNumericBounds(chosen->numericParams, chosen->generics, chosen->params, argTypes, "'" + name + "'", e.pos);
            checkGenericBounds(chosen->boundSpecs, chosen->generics, chosen->params, argTypes, "'" + name + "'", e.pos);
            e.overloadName = chosen->mangled.empty() ? name : chosen->mangled; // the declaring module's name for it
            e.uniqueName = chosen->uniqueName;
            e.linkName = chosen->linkName; // provisional: assignLinkNames rewrites it once every call is resolved
            callSites_.push_back({&e, name, argTypes, chosen->pos});
            noteDirectCall(fnBodyKey(chosen->pos), name, e.pos); // review r4 (part 2): the through-call gap
            TypeRef r = gen.empty() ? chosen->result : substGeneric(chosen->result, gen, binds);
            return chosen->isAsync ? awaitable(r) : r;
        }
        // (`Error(msg)` construction is handled by the types_ branch above — Error is a core-prelude type.)
        // A bare call inside a class body may target one of its own static methods.
        if (!currentClass_.empty()) {
            if (const MemberInfo* m = findMember(currentClass_, name);
                m && m->isStatic && (m->kind == MemberKind::Method || m->kind == MemberKind::Operator)) {
                checkArgs(m->params, argTypes, e.args, "static method '" + currentClass_ + "." + name + "'", e.pos, m->required);
                e.staticOwner = currentClass_;
                TypeRef r = inferResult(m->generics, m->params, argTypes, m->type);
                return m->isAsync ? awaitable(r) : r;
            }
        }
        // review r5 (part 2): a module value of function type is called through its value (`twice(4)`), like a
        // function-typed local — it was "call to undeclared function".
        if (auto v = values_.find(name); v != values_.end() && v->second.kind == TypeRef::Kind::Function) {
            TypeRef ft = checkName(*e.lhs); // the read: init-order checks, references, the declared type
            e.lhs->type = ft;
            e.calleeIsField = true; // lowered through the callee value, like a function-typed field
            return callFunctionValue(ft, argTypes, e, "'" + name + "'");
        }
        diags_.error(e.pos, "call to undeclared function '" + name + "'");
        return tUnknown();
    }

    // P40 review r5 item 22: `apply<T>(f: () => T)` called with a lambda that returns nothing would bind `T` to
    // unit — which no target can spell as a type argument (C# has no `Func<void>`: CS0411). TS/Python/PHP ran it,
    // C# did not compile. Refused when a type parameter is left unbound by every other argument and only such a
    // lambda's result could bind it. P40 review r6 #9: not only a lambda literal — ANY argument whose function type
    // returns unit (a named unit function, a `() => unit` local or field) binds it the same way.
    void refuseUnitInference(const FnSig& sig, const std::unordered_map<std::string, TypeRef>& binds, const Expr& e,
                             const std::string& name, const std::vector<TypeRef>& argTypes) {
        auto unitFnValue = [&](std::size_t i) {
            if (i >= argTypes.size()) return false;
            const TypeRef& a = argTypes[i];
            return a.kind == TypeRef::Kind::Function && (a.ret.empty() || isUnitT(a.ret[0]));
        };
        auto returnsNothing = [](const Expr& lam) {
            if (!lam.flag) return lam.lhs && lam.lhs->type.kind == TypeRef::Kind::Named && lam.lhs->type.name == "unit";
            std::function<bool(const std::vector<StmtPtr>&)> anyValue = [&](const std::vector<StmtPtr>& body) {
                for (const auto& s : body) {
                    if (!s) continue;
                    if (s->kind == StmtKind::Return && s->value) return true;
                    if (anyValue(s->thenBody) || anyValue(s->elseBody) || anyValue(s->finallyBody)) return true;
                    for (const auto& c : s->catches) if (anyValue(c.body)) return true;
                }
                return false;
            };
            return !anyValue(lam.block);
        };
        for (std::size_t i = 0; i < sig.params.size() && i < e.args.size(); ++i) {
            const TypeRef& p = sig.params[i];
            if (p.kind != TypeRef::Kind::Function || p.ret.size() != 1 || p.ret[0].kind != TypeRef::Kind::Named ||
                !p.ret[0].args.empty())
                continue;
            const std::string& g = p.ret[0].name;
            if (std::find(sig.generics.begin(), sig.generics.end(), g) == sig.generics.end()) continue;
            if (auto b = binds.find(g); b != binds.end() && !isUnitT(b->second)) continue;
            if (!e.args[i]) continue;
            const bool lambda = e.args[i]->kind == ExprKind::Lambda;
            if (lambda ? !returnsNothing(*e.args[i]) : !unitFnValue(i)) continue;
            diags_.error(e.args[i]->pos, "cannot infer '" + g + "' of '" + name + "' from " +
                                         (lambda ? "a lambda" : "a function value") + " that returns no "
                                         "value — a type parameter cannot stand for 'unit' (C# has no Func<void>); "
                                         "return a value from " + (lambda ? "the lambda" : "it") + ", or give '" +
                                         name + "' a parameter of a '(…) => unit' function type");
            return;
        }
    }

    bool isCaseOf(const std::vector<std::string>& cases, const std::string& name) const {
        for (const auto& c : cases) if (c == name) return true;
        return false;
    }

    // P40 review r5 item 18: a match's value type joins its arms — it used to be the LAST arm's type, so
    // `match k { 1 => maybe(), _ => 0 }` was a non-null `i32` and flowed into an `i32` slot unchecked. Any
    // nullable arm (or a `null` arm) makes the match nullable; numeric arms widen to the wider one; otherwise
    // the later arm's type is kept as before (a slot converts each arm on its own — checkConvert).
    TypeRef joinArmType(const TypeRef& acc, const TypeRef& arm) const {
        auto isNullLit = [](const TypeRef& t) { return t.kind == TypeRef::Kind::Named && t.name.empty() && t.nullable; };
        if (acc.kind == TypeRef::Kind::Named && acc.name.empty() && !acc.nullable) return arm; // the first arm
        if (isNullLit(arm)) { TypeRef r = acc; r.nullable = true; return r; }
        if (isNullLit(acc)) { TypeRef r = arm; r.nullable = true; return r; }
        const bool nullable = acc.nullable || arm.nullable;
        TypeRef a = acc, b = arm;
        a.nullable = b.nullable = false;
        TypeRef r = (isNumericTypeName(a) && isNumericTypeName(b) && isLosslessWiden(b, a)) ? acc : arm;
        if (nullable && !isOptionType(r)) r.nullable = true;
        return r;
    }

    TypeRef checkMatch(Expr& e) {
        TypeRef st = checkExpr(*e.lhs);
        const std::vector<std::string>* unionCases = nullptr;
        const std::vector<std::string>* enumCases = nullptr;
        if (st.kind == TypeRef::Kind::Named && !st.nullable) {
            if (auto u = unionAllCases_.find(st.name); u != unionAllCases_.end()) unionCases = &u->second;
            else if (auto en = enumAllCases_.find(st.name); en != enumAllCases_.end()) enumCases = &en->second;
        }

        bool catchAll = false, coveredTrue = false, coveredFalse = false;
        std::unordered_set<std::string> covered;
        TypeRef result = tUnknown();

        for (auto& arm : e.arms) {
            pushScope();
            declarePattern(arm.pattern, st);
            if (arm.guard) requireBool(checkExpr(*arm.guard), arm.pattern.pos, "'match' guard");
            if (arm.bodyIsBlock) {
                // §3.B: a match arm must be an EXPRESSION. Python/PHP lower `match` to an expression form
                // (a ternary/arrow fold) with no room for a statement block, and hoisting each block arm to
                // a named function is out of scope — so a block-bodied arm is refused loudly instead of the
                // old silent drop-to-`0` (issue #51). Extract the block into a function and call it.
                diags_.error(arm.pattern.pos,
                             "Polyglot refuses a block-bodied match arm — a match arm must be an expression "
                             "(extract the block into a function and call it from the arm) (PRD §3.B)");
                checkStmts(arm.block); // still check the body so nested errors surface too
            }
            else if (arm.body) result = joinArmType(result, checkExpr(*arm.body));
            popScope();

            // Only unguarded arms contribute to exhaustiveness (a guard may fail at run time).
            if (arm.guard) continue;
            const Pattern& p = arm.pattern;
            switch (p.kind) {
                case PatKind::Wildcard: catchAll = true; break;
                case PatKind::Ctor: covered.insert(p.name); break;
                case PatKind::Binding:
                    if ((unionCases && isCaseOf(*unionCases, p.name)) || (enumCases && isCaseOf(*enumCases, p.name)))
                        covered.insert(p.name);
                    else catchAll = true; // a plain variable binding matches anything
                    break;
                case PatKind::Literal:
                    if (p.literal && p.literal->kind == ExprKind::BoolLit) (p.literal->boolVal ? coveredTrue : coveredFalse) = true;
                    break;
                case PatKind::Tuple: break;
            }
        }

        if (!catchAll) reportNonExhaustive(e.pos, st, unionCases, enumCases, covered, coveredTrue, coveredFalse);
        return result;
    }

    void reportNonExhaustive(SourcePos pos, const TypeRef& st,
                             const std::vector<std::string>* unionCases, const std::vector<std::string>* enumCases,
                             const std::unordered_set<std::string>& covered, bool coveredTrue, bool coveredFalse) {
        auto missingOf = [&](const std::vector<std::string>& all) {
            std::string s;
            for (const auto& c : all) if (!covered.count(c)) { if (!s.empty()) s += ", "; s += c; }
            return s;
        };
        if (unionCases) { std::string m = missingOf(*unionCases); if (!m.empty()) diags_.error(pos, "non-exhaustive match: missing case(s) " + m); return; }
        if (enumCases)  { std::string m = missingOf(*enumCases);  if (!m.empty()) diags_.error(pos, "non-exhaustive match: missing case(s) " + m); return; }
        Ty s = scalarTyOf(st);
        if (s == Ty::Bool) { if (!(coveredTrue && coveredFalse)) diags_.error(pos, "non-exhaustive match: cover both true and false (or add '_')"); return; }
        if (s != Ty::Unknown) diags_.error(pos, "non-exhaustive match: add a '_' arm"); // a known, non-enumerable type
    }
};

} // namespace

void check(CompilationUnit& unit, DiagnosticBag& diags, SemanticModel* model, const SemanticRequest* req,
           const ModuleVisibility* modules) {
    Checker checker(diags, model, req, modules);
    checker.run(unit);
}

} // namespace mintplayer::polyglot
