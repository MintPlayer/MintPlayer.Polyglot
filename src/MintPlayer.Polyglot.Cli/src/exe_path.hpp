#pragma once

// Locate the running executable's own path, portably. Plugin discovery (loadPluginsNextToExe) needs the
// DIRECTORY the binary lives in, not the current working directory: the CLI is installed on PATH (npm /
// NuGet / tar / Homebrew) and invoked by bare name, so argv[0] carries no directory and resolving it
// against the cwd finds the wrong `plugins/` (or none). Each OS has a reliable primitive for "where am I";
// argv[0] is only the last resort when that primitive is somehow unavailable.

#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef Yield
#undef Yield // winbase.h macro; ir::StmtKind::Yield must survive
#endif
#elif defined(__APPLE__)
#include <cstdint>
#include <mach-o/dyld.h>
#endif

namespace mintplayer::polyglot::cli {

// The absolute path of the running executable. Pass argv[0] when available (it is the fallback if the OS
// primitive fails); a caller with no argv (the test harness' main() takes none) may omit it — the OS
// primitive answers on every platform we ship.
inline std::filesystem::path executablePath(const char* argv0 = nullptr) {
    namespace fs = std::filesystem;
    const fs::path fallback = argv0 ? fs::path(argv0) : fs::path();
#ifdef _WIN32
    char buf[4096];
    const unsigned long n = GetModuleFileNameA(nullptr, buf, sizeof(buf));
    return n > 0 && n < sizeof(buf) ? fs::path(std::string(buf, n)) : fallback;
#elif defined(__APPLE__)
    char buf[4096];
    std::uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) return fallback; // buffer too small — unreachable at 4 KiB
    std::error_code ec;
    const fs::path resolved = fs::weakly_canonical(fs::path(buf), ec); // _NSGetExecutablePath may hand back symlinks/..
    return ec ? fs::path(buf) : resolved;
#else
    std::error_code ec;
    const fs::path self = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fallback : self;
#endif
}

} // namespace mintplayer::polyglot::cli
