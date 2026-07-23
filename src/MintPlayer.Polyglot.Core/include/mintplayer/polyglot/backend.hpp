#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "mintplayer/polyglot/ir.hpp"

// A code-generation backend turns the typed IR (ir::Module) into target source. This is the seam the P9
// declarative-plugin API grows from: today the C# and TS backends are first-party implementations selected
// through a registry by name, instead of an `if/else` on the target. Adding a target becomes "register a
// Backend"; at P9 a downloaded declarative spec becomes another Backend over this same interface.
//
// (Build-dependency declaration — NuGet PackageReferences, npm deps — attaches here at P10; for now a
// backend just turns IR into a source string.)

namespace mintplayer::polyglot {

// A §3.A language feature that a backend may or may not be able to emit on its target (PRD §3.E). The set
// is deliberately finite and closed — one flag per supported-surface feature whose availability genuinely
// varies across SDKs. The usable surface for a build is the *intersection* of the configured backends'
// supported features; using a feature a target lacks is refused at compile time (capability.hpp), distinct
// from a §3.B global refusal. C# and TS both support all of these today, so nothing is gated yet.
enum class Feature {
    ExtensionMethods,    // `extension fn T.m()` keeping `x.m()` call syntax (impossible on Java/Go/C++/PHP)
    OperatorOverloading, // `operator fn plus(...)`
    Properties,          // computed properties / indexers
    Iterators,           // `yield` sequences
    PatternMatching,     // `match` + discriminated unions
    Closures,            // lambdas
    Exceptions,          // throw / try / catch / finally
    Disposal,            // `use` deterministic disposal
    Inheritance,         // class `: Base` + `super`
    Async,               // `async fn` + `await` (single-threaded coroutines); a future PHP-like target may lack it
    BlockLambdas,        // statement-bodied lambdas `x => { … }` (Python lambdas are expression-only)
    WithExpressions,     // record update `expr with { f = v }` (until a target has a ctor-rebuild emission)
    // P26 slice 0 — the second-wave capability vocabulary (PRD §3.E / §4.11). All three are `native` (or
    // absent) on C#/TS/Python, so they gate/warn nothing today; they exist so a second-wave target that
    // lacks one refuses or warns instead of silently miscompiling — the no-retrofit discipline.
    MutableRefClasses,   // a mutable reference class (`var` field / object identity) — absent on Haskell/Elixir
    FixedWidthIntegers,  // sub-64-bit or unsigned int widths (i8/i16/i32/u8/u16/u32/u64) — emulated on Dart's single int
    Utf16Strings,        // `char` / UTF-16 code-unit strings — emulated on grapheme-string targets (Swift)
    PropertySetters,     // a property accessor-block SETTER (`var x: T { get …; set(v) {…} }`) — C#/TS/Python
                         // have native accessors; PHP (< 8.4) has no property setters, so it refuses (#39c)
};
const char* featureName(Feature f); // stable lowerCamel id for diagnostics, e.g. "extensionMethods"

inline constexpr Feature kAllFeatures[] = {
    Feature::ExtensionMethods, Feature::OperatorOverloading, Feature::Properties, Feature::Iterators,
    Feature::PatternMatching, Feature::Closures, Feature::Exceptions, Feature::Disposal, Feature::Inheritance,
    Feature::Async, Feature::BlockLambdas, Feature::WithExpressions,
    Feature::MutableRefClasses, Feature::FixedWidthIntegers, Feature::Utf16Strings,
    Feature::PropertySetters,
};

// P37 slice 0 — the keyed capability vocabulary. A capability is addressed by a string key: either a
// bare parent (`"operatorOverloading"` — the 16 Feature names plus the coverage-only entries like
// `"interfaces"`), or a `parent:child` refinement (`"operatorOverloading:eq"`,
// `"attributes:target.param"`). The vocabulary stays CLOSED and load-validated (PRD §3.E / P37 H5):
// sub-keying a fixed entry into a small set of grades is refinement, not per-feature growth, and a
// manifest declaring a key outside the vocabulary is a LOAD ERROR — never silently ignored. Stance
// lookup applies the umbrella rule: a `parent:child` key with no explicit manifest entry inherits the
// bare parent's stance (so PHP's `"operatorOverloading": false` covers every sub-key it doesn't
// override), and an absent parent is "native" (supported) as before.
bool isKnownCapabilityKey(const std::string& key);

class Backend {
public:
    virtual ~Backend() = default;
    virtual std::string name() const = 0;                 // stable id, e.g. "csharp" / "typescript"
    virtual std::string emit(const ir::Module& module) const = 0;
    // The tri-state stance for a capability KEY (PRD §4.11 / P37 slice 0): "native" | "emulated" | "false".
    // supports() is the coarse gate (anything but "false"); the stance additionally lets the compiler WARN
    // on "emulated" — the "we rewrote your call site, here's why" surface. Implementations answering from a
    // declared map must apply the umbrella rule (see isKnownCapabilityKey): exact key, else the bare parent
    // of a `parent:child` key, else "native". The default supports everything.
    virtual std::string capabilityStance(const std::string& key) const {
        (void)key;
        return "native";
    }
    // Convenience overloads over the keyed source of truth (kept so the 16 legacy features stay typed at
    // call sites). Derived classes overriding the string virtual should `using Backend::capabilityStance;`
    // if they also call the Feature form on a derived-typed object.
    std::string capabilityStance(Feature f) const { return capabilityStance(std::string(featureName(f))); }
    bool supports(const std::string& key) const { return capabilityStance(key) != "false"; }
    bool supports(Feature f) const { return capabilityStance(f) != "false"; }
    // The emitted file's extension (plugin manifest `fileExtension`, e.g. ".cs") — the build driver is
    // target-agnostic; what a target's output is CALLED is its plugin's business.
    virtual std::string fileExtension() const { return ".txt"; }
    // Whether this target's emitted cross-module import specifiers may span directories (plugin
    // manifest `crossDirImports`, P30 slice 8). True: the compiler hands the import rule a FULL
    // relative specifier ("./name" or "../shared/name") computed from routed output dirs, and the
    // host may spread one closure across directories. False (default): specifiers are bare
    // basenames and the host must keep a closure in one directory (the CLI refuses a split).
    virtual bool crossDirImports() const { return false; }
    // The names this target's GENERATED code claims (`identifiers.reserved` — scaffolding, synthesized
    // temps; trailing `*` = prefix family) and its runtime globals (`identifiers.globals`). A user
    // identifier colliding with either refuses at compile time (checkReservedNames, P19 §7).
    virtual const std::vector<std::string>& reservedIdentifiers() const {
        static const std::vector<std::string> kEmpty;
        return kEmpty;
    }
    virtual const std::vector<std::string>& globalIdentifiers() const {
        static const std::vector<std::string> kEmpty;
        return kEmpty;
    }
    // The target's std overlay arms (P19 slice 9b): the FFI templates its plugin ships for the std
    // SKELETONS embedded in Core — keyed "<Class>.<member>" / "<Class>.type" / "<Class>.constructor" /
    // "<receiver>.<extension>" / "<expectFnName>", flattened across std modules. The compiler injects
    // them onto the skeleton decls before capability-checking/lowering; a used member with no overlay
    // entry refuses (§3.E). Empty for a backend without std support.
    virtual const std::unordered_map<std::string, std::string>& stdOverlays() const {
        static const std::unordered_map<std::string, std::string> kEmpty;
        return kEmpty;
    }
};

// The registered backend with this name, or nullptr if none.
const Backend* findBackend(const std::string& name);

// Names of all registered backends (for diagnostics / a future `--list-targets`).
std::vector<std::string> backendNames();

// Load a backend from its plugin artifact (`polyglot-plugin.json` bytes — Core stays IO-free, the host
// reads the file) and register it so findTarget()/findBackend() resolve it. The ENTIRE backend is data:
// the spec + rule tables interpreted by the shared emitter, plus the capability map `supports` answers
// from (a feature absent from the map is supported; `"blockLambdas": false` gates). Returns false and
// fills `error` on a malformed artifact — a plugin that fails validation is never partially registered.
// `replaceExisting` lets a config-pinned plugin SHADOW a same-named registration (P30: a pgconfig
// dependency wins over the in-box copy). The displaced backend stays alive for the process lifetime —
// BackendHandle holds raw pointers — but findBackend()/backendNames() see only the replacement.
bool loadBackend(const std::string& artifactJson, std::string& error, bool replaceExisting = false);

// Validate an artifact WITHOUT registering it (`polyglot install` checks a manifest before caching it —
// possibly for a target already loaded in this process). Same checks as loadBackend minus the name clash.
bool validateBackend(const std::string& artifactJson, std::string& error);

} // namespace mintplayer::polyglot
