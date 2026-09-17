#pragma once

#include <string>
#include <vector>

// Plugin arm-coverage tracing (docs/prd/code-coverage-upload/PRD.md §4.B).
//
// The four backends ARE their JSON manifests — zero backends are compiled in — so gcov and
// OpenCppCoverage cannot see them at all. The load-time anti-silent-drop contract (backend.cpp
// `kCoverage`) proves every IR construct HAS a rule; nothing proved a rule ever RAN, which is how
// 28 never-executed templates survived until someone read them by hand.
//
// This records which manifest arms actually fire. Both halves of the fraction come from the SAME
// parse — `indexRule` collects every parsed node as the denominator, the engine's TraceSink records
// evaluated nodes as the numerator — so a never-fired arm is reported *uncovered* rather than
// silently missing from the total. A regex over the JSON could not offer that guarantee.
//
// Manifests are identified by an opaque srcId and their own declared name; Core never compares
// against a target name.
namespace mintplayer::polyglot::armtrace {

// Enables tracing. Must be called BEFORE backends load — registration is skipped while disabled, so
// an ordinary run pays nothing beyond a null pointer test per rule evaluation.
void enable();
bool enabled();

// Registers a manifest's source text, returning its srcId (0 when tracing is off). Builds the line
// index used to convert offsets at the reporting boundary.
int registerManifest(const std::string& pluginName, const std::string& text);

// Records the static denominator for `srcId` — the offsets `indexRule`/`indexTest` collected.
void addStaticOffsets(int srcId, const std::vector<std::size_t>& offsets);

// Appends the accumulated trace to `path`, creating it if absent. One tab-separated record per
// line: `<plugin>\t<line>\t<S|H>` — S = a parsed arm (denominator), H = an arm that fired. Append
// rather than overwrite, so a sweep of hundreds of CLI invocations accumulates into one file with
// no merge step; the aggregator dedups and takes the max per line.
bool write(const std::string& path, std::string& error);

} // namespace mintplayer::polyglot::armtrace
