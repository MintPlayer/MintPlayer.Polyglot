#pragma once

// Watch-mode support for the `polyglot` CLI (PRD §4.13 / PLAN §P21). Core stays IO-free — everything
// here is CLI-layer. Two pieces:
//
//  * FileWatcher — watches a FIXED SET of files for content changes and coalesces bursts into single
//    wakeups. v1 is portable timestamp polling of the exact input set: the watched closure is a handful
//    of files, where polling wins outright (self-triggering impossible by construction — emitted outputs
//    are never in the polled set; atomic-save/rename-over transparent; no re-arm race; no thread). A
//    native ReadDirectoryChangesW / inotify impl can replace PollingFileWatcher behind this interface
//    with no caller change.
//
//  * RecordingResolver — a ModuleResolver decorator that records every file the compiler actually loads
//    (the transitive import closure `compile()` otherwise discards) plus the specifiers it failed to
//    resolve, so the watch loop can re-arm with the exact dependency set after every rebuild.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "mintplayer/polyglot/polyglot.hpp"

namespace mintplayer::polyglot::cli {

class FileWatcher {
public:
    enum class Event { Changed, TimedOut, Stopped };
    virtual ~FileWatcher() = default;

    // (Re)set the exact absolute paths to watch and take a fresh baseline snapshot. Called after every
    // rebuild, when the closure may have changed. A path that does not exist (yet) is valid: its
    // appearance counts as a change (this is what makes "create the file that fixes the broken import"
    // trigger a rebuild).
    virtual void watch(const std::vector<std::filesystem::path>& files) = 0;

    // Block until a watched file changed, `timeout` elapsed, or stop() ran. Detected changes are folded
    // into the baseline before returning, so a subsequent call reports only FURTHER changes — that is
    // what makes the caller's quiet-window debounce drain terminate.
    virtual Event waitNext(std::chrono::milliseconds timeout) = 0;

    // Wake waitNext from a console-control handler (runs on an OS-injected thread); makes it return
    // Stopped promptly.
    virtual void stop() = 0;
};

// Timestamp polling over the watched set: a (exists, mtime, size) baseline re-statted every `tick`.
// Transient stat failures (a file mid-atomic-save) read as "missing"; the rename completing flips it
// back — both edges are changes, and the caller's debounce coalesces them into one rebuild.
class PollingFileWatcher : public FileWatcher {
public:
    explicit PollingFileWatcher(std::chrono::milliseconds tick = std::chrono::milliseconds(250))
        : tick_(tick) {}

    void watch(const std::vector<std::filesystem::path>& files) override {
        baseline_.clear();
        for (const auto& f : files) baseline_[f.lexically_normal()] = stamp(f);
    }

    Event waitNext(std::chrono::milliseconds timeout) override {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            if (stop_.load()) return Event::Stopped;
            if (scan()) return Event::Changed;
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return Event::TimedOut;
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            std::this_thread::sleep_for(remaining < tick_ ? remaining : tick_);
        }
    }

    void stop() override { stop_.store(true); }

private:
    struct Stamp {
        bool exists = false;
        std::filesystem::file_time_type mtime{};
        std::uintmax_t size = 0;
        bool operator==(const Stamp&) const = default;
    };

    static Stamp stamp(const std::filesystem::path& p) {
        std::error_code ec;
        Stamp s;
        s.mtime = std::filesystem::last_write_time(p, ec);
        if (ec) return {}; // missing (or mid-rename): a definite state, not an error
        s.size = std::filesystem::file_size(p, ec);
        if (ec) return {};
        s.exists = true;
        return s;
    }

    // Re-stat the set; fold any differences into the baseline and report whether there were any.
    bool scan() {
        bool changed = false;
        for (auto& [path, old] : baseline_) {
            Stamp now = stamp(path);
            if (now != old) {
                old = now;
                changed = true;
            }
        }
        return changed;
    }

    std::map<std::filesystem::path, Stamp> baseline_;
    std::atomic<bool> stop_{false};
    std::chrono::milliseconds tick_;
};

// Records what the compiler loads through it. `loaded()` is the transitive `.pg` closure (canonical
// absolute paths); `unresolved()` is the (specifier, importer) pairs that failed, so the watch loop can
// also poll the files that WOULD satisfy them once created.
class RecordingResolver : public ModuleResolver {
public:
    explicit RecordingResolver(ModuleResolver& inner) : inner_(inner) {}

    std::optional<ResolvedModule> resolve(const std::string& specifier,
                                          const std::string& importerCanonicalPath) override {
        auto r = inner_.resolve(specifier, importerCanonicalPath);
        if (r)
            loaded_.insert(r->canonicalPath);
        else
            unresolved_.insert({specifier, importerCanonicalPath});
        return r;
    }

    const std::set<std::string>& loaded() const { return loaded_; }
    const std::set<std::pair<std::string, std::string>>& unresolved() const { return unresolved_; }

private:
    ModuleResolver& inner_;
    std::set<std::string> loaded_;
    std::set<std::pair<std::string, std::string>> unresolved_;
};

} // namespace mintplayer::polyglot::cli
