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

struct Diagnostic {
    SourcePos pos;
    std::string message;
};

class DiagnosticBag {
public:
    void error(SourcePos pos, std::string message) {
        items_.push_back({pos, std::move(message)});
    }

    bool hasErrors() const { return !items_.empty(); }
    const std::vector<Diagnostic>& items() const { return items_; }

private:
    std::vector<Diagnostic> items_;
};

} // namespace mintplayer::polyglot
