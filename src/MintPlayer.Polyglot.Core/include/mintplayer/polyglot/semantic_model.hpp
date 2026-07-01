#pragma once

#include <string>
#include <vector>

#include "mintplayer/polyglot/ast.hpp"
#include "mintplayer/polyglot/diagnostics.hpp"

// The position-indexed semantic model — the foundation for `polyglot lsp` (go-to-definition, hover,
// document symbols). It is a by-product of sema (`check(..., SemanticModel*)`), which is the pass that
// already resolves names to declarations; recording the resolution as it happens is both accurate (the
// scope stack is exactly right for shadowing) and the only way to reach members/overloads. See PRD §4.8.
//
// v1 is SAME-FILE: definitions and references for symbols declared in the file being edited (functions,
// parameters, locals). Symbols merged in from other modules (std, imports) are flagged `external` and are
// not indexed as defs — cross-module go-to-def waits on file-tracked positions (PRD §4.8 / PLAN §P16c).

namespace mintplayer::polyglot {

enum class SymbolKind { Function, Type, Value, Parameter, Local, Field, Method, UnionCase, EnumCase };

// A source span: a start position plus the identifier's length, so a client can build the covering range
// (the AST stores only a start `SourcePos`; `length` recovers the width from the name token's text).
struct Span {
    SourcePos start;
    int length = 0;
    bool covers(int line, int col) const {
        return start.line == line && col >= start.col && col < start.col + length;
    }
};

// A declaration site: where a symbol is introduced. `nameSpan` is where go-to-definition jumps.
struct SymbolDef {
    SymbolKind kind = SymbolKind::Local;
    std::string name;
    Span nameSpan;
    TypeRef type;         // best-effort; may be absent/unknown
    bool external = false;// merged from std/an import, not the file being edited (not offered as a target yet)
    std::string owner;    // Field/Method: the owning type's name (for member completion `obj.`); else empty
};

// A use site: an identifier occurrence resolved to a definition. `def` indexes `SemanticModel::defs`, or is
// -1 when resolution was incomplete (still recorded so highlighting/selection can use the span).
struct SymbolRef {
    Span span;
    int def = -1;
};

struct SemanticModel {
    std::vector<SymbolDef> defs;
    std::vector<SymbolRef> refs;

    // The definition of the symbol whose reference span covers (line, col) — 1-based, matching SourcePos.
    // Also matches when (line,col) lands on a definition's own name (go-to-def on a def returns itself).
    // Returns nullptr when nothing resolvable is at that position.
    const SymbolDef* definitionAt(int line, int col) const {
        for (const auto& r : refs)
            if (r.def >= 0 && r.span.covers(line, col)) return &defs[static_cast<std::size_t>(r.def)];
        for (const auto& d : defs)
            if (!d.external && d.nameSpan.covers(line, col)) return &d;
        return nullptr;
    }

    // The definition index of the symbol at (line, col) — via a reference covering it, or its own name;
    // -1 if none. The shared entry point for references/rename (definitionAt returns the def; this the index).
    int symbolAt(int line, int col) const {
        for (const auto& r : refs)
            if (r.def >= 0 && r.span.covers(line, col)) return r.def;
        for (std::size_t i = 0; i < defs.size(); ++i)
            if (!defs[i].external && defs[i].nameSpan.covers(line, col)) return static_cast<int>(i);
        return -1;
    }

    // Every reference-use span resolving to definition index `d` (in this file; the model records only the
    // edited file's references). Callers add the declaration's own span separately when wanted.
    std::vector<Span> referencesTo(int d) const {
        std::vector<Span> out;
        for (const auto& r : refs) if (r.def == d) out.push_back(r.span);
        return out;
    }

    // File-local top-level/member definitions, for `textDocument/documentSymbol` (locals + parameters are
    // excluded — they aren't document-scope symbols).
    std::vector<const SymbolDef*> documentSymbols() const {
        std::vector<const SymbolDef*> out;
        for (const auto& d : defs)
            if (!d.external && d.kind != SymbolKind::Local && d.kind != SymbolKind::Parameter)
                out.push_back(&d);
        return out;
    }
};

// The user/external boundary: only top-level declarations at index < these counts belong to the file being
// edited (the linker APPENDS merged std/import/lib decls, so the entry file's own decls stay at the front).
// Captured right after parsing the entry unit and passed to `check` so the model flags external symbols.
struct SemanticRequest {
    std::size_t userFunctions = 0;
    std::size_t userRecords = 0;
    std::size_t userClasses = 0;
    std::size_t userInterfaces = 0;
    std::size_t userEnums = 0;
    std::size_t userUnions = 0;
    std::size_t userExtensions = 0;
    std::size_t userValues = 0;
};

} // namespace mintplayer::polyglot
