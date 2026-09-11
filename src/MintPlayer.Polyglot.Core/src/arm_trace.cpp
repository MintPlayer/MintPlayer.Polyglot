#include "mintplayer/polyglot/arm_trace.hpp"

#include <fstream>
#include <memory>
#include <set>

#include "mintplayer/polyglot/backend_engine.hpp"
#include "mintplayer/polyglot/json.hpp"

namespace mintplayer::polyglot::armtrace {
namespace {

struct Manifest {
    std::string name;
    json::LineIndex lines;
    std::set<std::size_t> staticOffsets; // denominator
    std::set<std::size_t> hitOffsets;    // numerator

    Manifest(std::string n, const std::string& text) : name(std::move(n)), lines(text) {}
};

// Manifests are append-only for the process lifetime; srcId is index+1 so 0 stays "unregistered".
std::vector<std::unique_ptr<Manifest>>& manifests() {
    static std::vector<std::unique_ptr<Manifest>> m;
    return m;
}

bool g_enabled = false;

// Records hits into the owning manifest. Deliberately does no I/O and no allocation beyond the set
// insert — it runs once per rule/test evaluation, i.e. millions of times across a sweep.
class Sink : public engine::TraceSink {
public:
    void hit(int srcId, std::size_t offset) override {
        if (srcId <= 0 || static_cast<std::size_t>(srcId) > manifests().size()) return;
        manifests()[static_cast<std::size_t>(srcId) - 1]->hitOffsets.insert(offset);
    }
};

Sink& sink() {
    static Sink s;
    return s;
}

} // namespace

void enable() {
    if (g_enabled) return;
    g_enabled = true;
    engine::setTraceSink(&sink());
}

bool enabled() { return g_enabled; }

int registerManifest(const std::string& pluginName, const std::string& text) {
    if (!g_enabled) return 0;
    manifests().push_back(std::make_unique<Manifest>(pluginName, text));
    return static_cast<int>(manifests().size());
}

void addStaticOffsets(int srcId, const std::vector<std::size_t>& offsets) {
    if (srcId <= 0 || static_cast<std::size_t>(srcId) > manifests().size()) return;
    Manifest& m = *manifests()[static_cast<std::size_t>(srcId) - 1];
    for (std::size_t o : offsets) m.staticOffsets.insert(o);
}

bool write(const std::string& path, std::string& error) {
    std::ofstream out(path, std::ios::app);
    if (!out) {
        error = "cannot open arm-trace file '" + path + "' for append";
        return false;
    }
    for (const auto& m : manifests()) {
        // Denominator first, then hits: the aggregator takes the max per line, so record order
        // across the hundreds of appended invocations never matters.
        for (std::size_t o : m->staticOffsets) out << m->name << '\t' << m->lines.lineAt(o) << "\tS\n";
        for (std::size_t o : m->hitOffsets)    out << m->name << '\t' << m->lines.lineAt(o) << "\tH\n";
    }
    if (!out) {
        error = "failed writing arm-trace file '" + path + "'";
        return false;
    }
    return true;
}

} // namespace mintplayer::polyglot::armtrace
