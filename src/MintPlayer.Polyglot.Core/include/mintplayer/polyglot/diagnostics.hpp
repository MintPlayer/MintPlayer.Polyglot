#pragma once

#include <string>
#include <vector>

// Source positions and the diagnostic bag shared by every pass. A diagnostic carries a 1-based
// line/column and a human-readable message; the bag accumulates them so a pass can report many
// errors before giving up (real recovery arrives in P3).

namespace mintplayer::polyglot {

struct SourcePos {
    int line = 1;
    int col = 1;
    int fileId = 0;   // index into the compilation's source-file table (0 = the primary/only file). Rides by
                      // value through every token/AST/IR position, so multi-file positions cost no plumbing (§4.8).
};

enum class Severity { Error, Warning, Info, Hint };

struct Diagnostic {
    SourcePos pos;                        // start (1-based)
    SourcePos end;                        // end (1-based, exclusive); == pos when the reporter gave only a point
    std::string message;
    Severity severity = Severity::Error;
};

class DiagnosticBag {
public:
    // Point diagnostics (the common case): the range is a single position; a consumer that has the source
    // (e.g. the language server) widens it to the identifier at that spot. The overload takes an explicit end.
    void error(SourcePos pos, std::string message) {
        items_.push_back({pos, pos, std::move(message), Severity::Error});
    }
    void error(SourcePos pos, SourcePos end, std::string message) {
        items_.push_back({pos, end, std::move(message), Severity::Error});
    }
    void warn(SourcePos pos, std::string message) {
        items_.push_back({pos, pos, std::move(message), Severity::Warning});
    }

    // `hasErrors` counts only error-severity items, so a future warning never blocks compilation.
    bool hasErrors() const {
        for (const auto& d : items_) if (d.severity == Severity::Error) return true;
        return false;
    }
    const std::vector<Diagnostic>& items() const { return items_; }

private:
    std::vector<Diagnostic> items_;
};

} // namespace mintplayer::polyglot
