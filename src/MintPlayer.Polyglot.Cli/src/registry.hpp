#pragma once

// The npm-registry client for plugin auto-download (PRD issue-30 §D1/§D6 / PLAN §P30 slice 1).
// Data-only by construction: one HTTPS GET for the abbreviated packument, one for the tarball —
// no npm process, no lifecycle scripts, nothing executed (§6 trust model). CLI-layer; Core stays
// IO-free.
//
//  * HttpGet            — the ONE platform-specific seam, injectable so every caller above it is
//                         unit-testable offline. defaultHttpGet() is the real ladder:
//                         Windows WinHTTP (OS TLS/certs/proxy) → POSIX dlopen(libcurl) (present
//                         on macOS always, virtually every Linux distro; zero link-time dep) →
//                         `curl` subprocess → a diagnostic naming the alternatives. No vendored
//                         TLS stack (the PRD-recorded fork if this ladder proves flaky).
//  * fetchPackument     — GET <registry>/<name-with-%2F> with the abbreviated-packument Accept
//                         header; parsePackument is split out for fixture-driven tests.
//  * registryBaseFor    — POLYGLOT_REGISTRY env → .npmrc `@scope:registry=` / `registry=`
//                         (project dir upward, then the user profile) → registry.npmjs.org.
//                         Proxy: WinHTTP uses the system proxy; libcurl honors HTTPS_PROXY /
//                         NO_PROXY natively. Auth tokens are deliberately unsupported (public
//                         packages only — documented refusal, not an oversight).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef Yield
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#else
#include <dlfcn.h>
#endif

#include "mintplayer/polyglot/json.hpp"

#include "pgconfig.hpp" // readFile

namespace mintplayer::polyglot::cli {

// GET `url` with extra request `headers` ("Name: value"), filling `body` (binary-safe).
// Implementations return false with a human diagnostic in `err`; a non-200 final status is an
// error (redirects are followed internally).
using HttpGet = std::function<bool(const std::string& url, const std::vector<std::string>& headers,
                                   std::string& body, std::string& err)>;

inline std::string envVar(const char* name) {
#ifdef _WIN32
    char buf[8192];
    const unsigned long n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    return (n > 0 && n < sizeof(buf)) ? std::string(buf, n) : std::string();
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

// ---- default transport ladder ----------------------------------------------------------------

#ifdef _WIN32

namespace detail {
inline std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
} // namespace detail

inline bool httpGetWinHttp(const std::string& url, const std::vector<std::string>& headers,
                           std::string& body, std::string& err) {
    using detail::widen;
    auto fail = [&](const std::string& what) {
        err = "http: " + what + " (" + std::to_string(GetLastError()) + ") for " + url;
        return false;
    };

    const std::wstring wurl = widen(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    wchar_t host[256], path[4096];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;  uc.dwUrlPathLength = 4096;
    uc.dwExtraInfoLength = static_cast<DWORD>(-1); // fold ?query into lpszUrlPath
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return fail("bad URL");
    const bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;

    HINTERNET ses = WinHttpOpen(L"polyglot-cli", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return fail("WinHttpOpen failed");
    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    if (!con) { WinHttpCloseHandle(ses); return fail("connect failed"); }
    HINTERNET req = WinHttpOpenRequest(con, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    auto done = [&](bool ok) {
        if (req) WinHttpCloseHandle(req);
        WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        return ok;
    };
    if (!req) return done(fail("open request failed"));

    std::wstring hdrBlock;
    for (const auto& h : headers) hdrBlock += widen(h) + L"\r\n";
    if (!hdrBlock.empty() &&
        !WinHttpAddRequestHeaders(req, hdrBlock.c_str(), static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD))
        return done(fail("add headers failed"));

    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, nullptr))
        return done(fail("request failed (offline? proxy?)"));

    DWORD status = 0, statusLen = sizeof status;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusLen, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        err = "http: status " + std::to_string(status) + " for " + url;
        return done(false);
    }

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) return done(fail("read failed"));
        if (avail == 0) break;
        const std::size_t at = body.size();
        body.resize(at + avail);
        DWORD got = 0;
        if (!WinHttpReadData(req, body.data() + at, avail, &got)) return done(fail("read failed"));
        body.resize(at + got);
    }
    return done(true);
}

inline HttpGet defaultHttpGet() { return httpGetWinHttp; }

#else // POSIX: dlopen(libcurl) first, `curl` subprocess second

namespace detail {

// The handful of ABI-stable libcurl constants the client needs (curl.h is not a build dep).
inline constexpr int kCurloptWriteData = 10001;
inline constexpr int kCurloptUrl = 10002;
inline constexpr int kCurloptTimeout = 13;
inline constexpr int kCurloptUserAgent = 10018;
inline constexpr int kCurloptHttpHeader = 10023;
inline constexpr int kCurloptWriteFunction = 20011;
inline constexpr int kCurloptFollowLocation = 52;
inline constexpr int kCurloptNoSignal = 99;
inline constexpr int kCurlinfoResponseCode = 0x200000 + 2;

struct Curl {
    void* lib = nullptr;
    void* (*init)() = nullptr;
    int (*setopt)(void*, int, ...) = nullptr;
    int (*perform)(void*) = nullptr;
    int (*getinfo)(void*, int, ...) = nullptr;
    void (*cleanup)(void*) = nullptr;
    const char* (*strerr)(int) = nullptr;
    void* (*slistAppend)(void*, const char*) = nullptr;
    void (*slistFreeAll)(void*) = nullptr;

    static const Curl& get() {
        static const Curl c = [] {
            Curl r;
            for (const char* name : {
#ifdef __APPLE__
                     "libcurl.4.dylib", "libcurl.dylib"
#else
                     "libcurl.so.4", "libcurl.so", "libcurl.so.3"
#endif
                 }) {
                if ((r.lib = dlopen(name, RTLD_NOW | RTLD_LOCAL))) break;
            }
            if (!r.lib) return r;
            r.init = reinterpret_cast<void* (*)()>(dlsym(r.lib, "curl_easy_init"));
            r.setopt = reinterpret_cast<int (*)(void*, int, ...)>(dlsym(r.lib, "curl_easy_setopt"));
            r.perform = reinterpret_cast<int (*)(void*)>(dlsym(r.lib, "curl_easy_perform"));
            r.getinfo = reinterpret_cast<int (*)(void*, int, ...)>(dlsym(r.lib, "curl_easy_getinfo"));
            r.cleanup = reinterpret_cast<void (*)(void*)>(dlsym(r.lib, "curl_easy_cleanup"));
            r.strerr = reinterpret_cast<const char* (*)(int)>(dlsym(r.lib, "curl_easy_strerror"));
            r.slistAppend = reinterpret_cast<void* (*)(void*, const char*)>(dlsym(r.lib, "curl_slist_append"));
            r.slistFreeAll = reinterpret_cast<void (*)(void*)>(dlsym(r.lib, "curl_slist_free_all"));
            if (!r.init || !r.setopt || !r.perform || !r.getinfo || !r.cleanup) r.lib = nullptr;
            return r;
        }();
        return c;
    }
};

inline std::size_t curlWrite(const char* ptr, std::size_t size, std::size_t nmemb, void* ud) {
    static_cast<std::string*>(ud)->append(ptr, size * nmemb);
    return size * nmemb;
}

} // namespace detail

inline bool httpGetLibcurl(const std::string& url, const std::vector<std::string>& headers,
                           std::string& body, std::string& err) {
    const auto& c = detail::Curl::get();
    if (!c.lib) { err = "http: libcurl not found"; return false; }
    void* h = c.init();
    if (!h) { err = "http: curl_easy_init failed"; return false; }
    void* hdrs = nullptr;
    if (c.slistAppend)
        for (const auto& hd : headers) hdrs = c.slistAppend(hdrs, hd.c_str());
    c.setopt(h, detail::kCurloptUrl, url.c_str());
    c.setopt(h, detail::kCurloptWriteFunction, detail::curlWrite);
    c.setopt(h, detail::kCurloptWriteData, &body);
    c.setopt(h, detail::kCurloptFollowLocation, 1L);
    c.setopt(h, detail::kCurloptNoSignal, 1L);
    c.setopt(h, detail::kCurloptTimeout, 300L);
    c.setopt(h, detail::kCurloptUserAgent, "polyglot-cli");
    if (hdrs) c.setopt(h, detail::kCurloptHttpHeader, hdrs);
    const int rc = c.perform(h);
    long status = 0;
    if (rc == 0) c.getinfo(h, detail::kCurlinfoResponseCode, &status);
    if (hdrs && c.slistFreeAll) c.slistFreeAll(hdrs);
    c.cleanup(h);
    if (rc != 0) {
        err = std::string("http: ") + (c.strerr ? c.strerr(rc) : "curl error") + " for " + url;
        body.clear();
        return false;
    }
    if (status != 200) {
        err = "http: status " + std::to_string(status) + " for " + url;
        body.clear();
        return false;
    }
    return true;
}

inline bool httpGetCurlSubprocess(const std::string& url, const std::vector<std::string>& headers,
                                  std::string& body, std::string& err) {
    // Refuse anything that could escape single quotes — registry URLs/headers never contain them.
    for (const std::string& s : headers)
        if (s.find('\'') != std::string::npos) { err = "http: unsafe header"; return false; }
    if (url.find('\'') != std::string::npos) { err = "http: unsafe URL"; return false; }
    std::string cmd = "curl --fail --silent --show-error --location --max-time 300";
    for (const auto& h : headers) cmd += " -H '" + h + "'";
    cmd += " '" + url + "'";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) { err = "http: cannot spawn curl"; return false; }
    char buf[65536];
    std::size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) body.append(buf, n);
    const int rc = pclose(p);
    if (rc != 0) {
        err = "http: curl exited with status " + std::to_string(rc) + " for " + url;
        body.clear();
        return false;
    }
    return true;
}

inline HttpGet defaultHttpGet() {
    return [](const std::string& url, const std::vector<std::string>& headers,
              std::string& body, std::string& err) {
        if (detail::Curl::get().lib) return httpGetLibcurl(url, headers, body, err);
        std::string subErr;
        if (httpGetCurlSubprocess(url, headers, body, subErr)) return true;
        err = subErr + "\n  (no libcurl and no curl binary — install one, use a `file:` dependency, "
                       "or pre-warm the plugin cache on a connected machine)";
        return false;
    };
}

#endif // _WIN32

// ---- registry selection (PRD §D6) -------------------------------------------------------------

// Minimal .npmrc reading: `registry=` and `@scope:registry=` keys, '#'/';' comments. Project dir
// upward first (nearest wins), then the user-profile .npmrc.
inline std::string npmrcRegistryLookup(const std::string& scope, const std::filesystem::path& startDir) {
    auto fromFile = [&](const std::filesystem::path& p) -> std::string {
        std::string src, fallback;
        if (!readFile(p, src)) return {};
        std::istringstream in(src);
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
            std::size_t b = line.find_first_not_of(" \t");
            if (b == std::string::npos || line[b] == '#' || line[b] == ';') continue;
            const std::string trimmed = line.substr(b);
            if (!scope.empty() && trimmed.rfind(scope + ":registry=", 0) == 0)
                return trimmed.substr(scope.size() + 10);
            if (trimmed.rfind("registry=", 0) == 0) fallback = trimmed.substr(9);
        }
        return fallback;
    };
    for (std::filesystem::path d = startDir;; d = d.parent_path()) {
        if (const std::string r = fromFile(d / ".npmrc"); !r.empty()) return r;
        if (!d.has_parent_path() || d.parent_path() == d) break;
    }
    const std::string home = envVar(
#ifdef _WIN32
        "USERPROFILE"
#else
        "HOME"
#endif
    );
    if (!home.empty())
        if (const std::string r = fromFile(std::filesystem::path(home) / ".npmrc"); !r.empty()) return r;
    return {};
}

// The registry base URL for a package: POLYGLOT_REGISTRY env (the test harness / CI seam) →
// .npmrc → the public npm registry. Trailing '/' normalized away.
inline std::string registryBaseFor(const std::string& packageName, const std::filesystem::path& projectDir) {
    std::string base = envVar("POLYGLOT_REGISTRY");
    if (base.empty()) {
        const std::string scope = packageName.rfind('@', 0) == 0
                                      ? packageName.substr(0, packageName.find('/'))
                                      : std::string();
        base = npmrcRegistryLookup(scope, projectDir);
    }
    if (base.empty()) base = "https://registry.npmjs.org";
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base;
}

// The packument path encodes the scope slash: @scope/name -> @scope%2Fname.
inline std::string packumentUrl(const std::string& registryBase, const std::string& packageName) {
    std::string enc = packageName;
    if (const std::size_t slash = enc.find('/'); slash != std::string::npos)
        enc = enc.substr(0, slash) + "%2F" + enc.substr(slash + 1);
    return registryBase + "/" + enc;
}

// ---- abbreviated packument --------------------------------------------------------------------

struct PackumentVersion {
    std::string version;
    std::string tarball;   // dist.tarball — ALWAYS taken from the packument, never constructed
    std::string integrity; // dist.integrity (SRI), may be empty on ancient packages
    std::string shasum;    // dist.shasum (sha1 hex legacy)
};

struct Packument {
    std::string name;
    std::vector<PackumentVersion> versions;
    std::map<std::string, std::string> distTags; // "latest" -> version
};

// Parse an (abbreviated or full) packument. Registry "not found" bodies become a clean error.
inline bool parsePackument(const std::string& src, Packument& out, std::string& err) {
    const json::Value v = json::parse(src);
    if (v.kind != json::Value::Kind::Object) { err = "registry response is not a JSON object"; return false; }
    const std::string regErr = v["error"].asString();
    if (!regErr.empty()) { err = "registry error: " + regErr; return false; }
    out.name = v["name"].asString();
    for (const auto& kv : v["dist-tags"].members)
        if (kv.second.kind == json::Value::Kind::String) out.distTags[kv.first] = kv.second.asString();
    for (const auto& kv : v["versions"].members) {
        PackumentVersion pv;
        pv.version = kv.first;
        const json::Value& dist = kv.second["dist"];
        pv.tarball = dist["tarball"].asString();
        pv.integrity = dist["integrity"].asString();
        pv.shasum = dist["shasum"].asString();
        if (!pv.tarball.empty()) out.versions.push_back(std::move(pv));
    }
    if (out.versions.empty()) { err = "packument for '" + out.name + "' lists no installable versions"; return false; }
    return true;
}

inline bool fetchPackument(const HttpGet& http, const std::string& registryBase,
                           const std::string& packageName, Packument& out, std::string& err) {
    std::string body;
    if (!http(packumentUrl(registryBase, packageName),
              {"Accept: application/vnd.npm.install-v1+json"}, body, err))
        return false;
    return parsePackument(body, out, err);
}

} // namespace mintplayer::polyglot::cli
