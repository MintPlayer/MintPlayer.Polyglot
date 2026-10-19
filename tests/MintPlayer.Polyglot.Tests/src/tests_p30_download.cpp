// P30 slices 0-4: sha/SRI/gzip/tar, semver + registry, lockfile + cache, the auto-download pipeline, derived reference backend.
// P41 D14: one translation unit of the unit suite (split from tests_main.cpp so the areas compile in
// parallel). The bodies are unchanged; the shared harness lives in test_support.hpp.

#include "test_support.hpp"
#include "gzip_fixtures.hpp" // generated golden gzip/SRI vectors (Node zlib/crypto — independent impl)

void run_p30_download() {
    // ---- P30 slice 0: auto-download primitives (sha / SRI / gzip / tar / pgconfig glue) ------

    // SHA-512 / SHA-1 against the NIST FIPS 180-4 vectors — these pin the generated K/H tables.
    {
        check(cli::toHex(cli::sha512(std::string("abc"))) ==
                  "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                  "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
              "P30 s0: sha512('abc') matches NIST");
        check(cli::toHex(cli::sha512(std::string())) ==
                  "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
                  "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e",
              "P30 s0: sha512('') matches NIST");
        check(cli::toHex(cli::sha512(std::string(
                  "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
                  "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"))) ==
                  "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
                  "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909",
              "P30 s0: sha512(two-block vector) matches NIST");
        check(cli::toHex(cli::sha1(std::string("abc"))) == "a9993e364706816aba3e25717850c26c9cd0d89d",
              "P30 s0: sha1('abc') matches NIST");
        check(cli::toHex(cli::sha1(std::string(1000000, 'a'))) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f",
              "P30 s0: sha1(million 'a') matches NIST (multi-block path)");

        // SRI: npm's dist.integrity form. The reference string was produced by Node crypto.
        const std::string sri = cli::sriSha512("polyglot");
        check(sri == "sha512-CQa4A4lIW4ghrXVECgs3HAWEbMqpb5YHPlBw+nU+4pYViS88L5Hyh8OYBdfevZNSqElzL9ZJRpONx94rrPQDPQ==",
              "P30 s0: sriSha512 renders npm's base64 form");
        std::string err;
        check(cli::verifySri(sri, "polyglot", err), "P30 s0: verifySri accepts the right payload");
        check(!cli::verifySri(sri, "Polyglot", err) && has(err, "integrity mismatch"),
              "P30 s0: verifySri refuses a tampered payload naming the mismatch");
        check(!cli::verifySri("sha384-AAAA", "x", err) && has(err, "unsupported integrity algorithm 'sha384'"),
              "P30 s0: verifySri refuses an algorithm it cannot check (never a silent pass)");
        check(!cli::verifySri("garbage", "x", err) && has(err, "malformed integrity"),
              "P30 s0: verifySri refuses a malformed SRI string");
    }

    // CRC-32 (gzip trailer): the classic check value.
    {
        check(cli::crc32("123456789", 9) == 0xCBF43926u, "P30 s0: crc32 check value");
    }

    // gunzip against Node-zlib golden vectors: fixed-Huffman, FNAME header, dynamic-Huffman,
    // incompressible/stored — plus the corruption and zip-bomb-cap refusals.
    {
        std::string out, err;
        check(cli::gunzip(gzfix::kP1Gz, gzfix::kP1GzLen, out, 1 << 20, err) && out == gzfix::kP1,
              "P30 s0: gunzip decodes a fixed-Huffman member");
        out.clear();
        check(cli::gunzip(gzfix::kP1GzName, gzfix::kP1GzNameLen, out, 1 << 20, err) && out == gzfix::kP1,
              "P30 s0: gunzip skips an FNAME header field");
        out.clear();
        check(cli::gunzip(gzfix::kP2Gz, gzfix::kP2GzLen, out, 1 << 20, err) && out == gzfix::kP2,
              "P30 s0: gunzip decodes a dynamic-Huffman member");
        out.clear();
        check(cli::gunzip(gzfix::kP3Gz, gzfix::kP3GzLen, out, 1 << 20, err) &&
                  out.size() == gzfix::kP3Len && std::memcmp(out.data(), gzfix::kP3, gzfix::kP3Len) == 0,
              "P30 s0: gunzip decodes an incompressible (stored) member");
        check(cli::toHex(cli::sha512(out)) == gzfix::kP3Sha512Hex,
              "P30 s0: sha512 of the inflated payload matches Node crypto (binary input)");
        check(cli::verifySri(gzfix::kP3Sri, out, err),
              "P30 s0: verifySri agrees with an npm-shaped integrity for binary bytes");

        // Corruption: flip one payload byte — the CRC32 trailer must catch it.
        std::string bad(reinterpret_cast<const char*>(gzfix::kP2Gz), gzfix::kP2GzLen);
        bad[bad.size() / 2] ^= 0x40;
        out.clear();
        check(!cli::gunzip(reinterpret_cast<const std::uint8_t*>(bad.data()), bad.size(), out, 1 << 20, err),
              "P30 s0: gunzip refuses a corrupted stream");
        // Zip-bomb guard: a cap below the payload size fails BEFORE unbounded growth.
        out.clear();
        check(!cli::gunzip(gzfix::kP2Gz, gzfix::kP2GzLen, out, 16, err) && has(err, "size cap"),
              "P30 s0: gunzip enforces the output size cap");
        out.clear();
        check(!cli::gunzip(reinterpret_cast<const std::uint8_t*>("nope"), 4, out, 16, err),
              "P30 s0: gunzip refuses a non-gzip buffer");
    }

    // tar: ustar + pax reading with the zip-slip refusals. Archives are built in-test.
    {
        auto oct = [](std::string& h, std::size_t at, std::size_t width, unsigned long long v) {
            for (std::size_t i = width - 1; i-- > 0;) { h[at + i] = static_cast<char>('0' + (v & 7)); v >>= 3; }
            h[at + width - 1] = '\0';
        };
        auto hdr = [&](const std::string& name, std::size_t size, char type, const std::string& prefix = "") {
            std::string h(512, '\0');
            std::memcpy(&h[0], name.data(), name.size());
            oct(h, 100, 8, 0644); oct(h, 108, 8, 0); oct(h, 116, 8, 0);
            oct(h, 124, 12, size); oct(h, 136, 12, 0);
            std::memset(&h[148], ' ', 8);
            h[156] = type;
            std::memcpy(&h[257], "ustar", 6); h[263] = '0'; h[264] = '0';
            if (!prefix.empty()) std::memcpy(&h[345], prefix.data(), prefix.size());
            unsigned sum = 0;
            for (unsigned char c : h) sum += c;
            for (int i = 5; i >= 0; --i) { h[148 + i] = static_cast<char>('0' + (sum & 7)); sum >>= 3; }
            h[154] = '\0'; h[155] = ' ';
            return h;
        };
        auto pad512 = [](std::string s) { s.resize((s.size() + 511) & ~std::size_t(511), '\0'); return s; };
        const std::string endMark(1024, '\0');

        // A plain npm-shaped archive: dir + manifest under package/.
        const std::string manifest = "{ \"schema\": 2, \"name\": \"demo\" }";
        std::string ar = hdr("package/", 0, '5') +
                         hdr("package/polyglot-plugin.json", manifest.size(), '0') + pad512(manifest) + endMark;
        std::vector<cli::TarEntry> es;
        std::string err;
        check(cli::tarRead(ar, es, err) && es.size() == 2 && es[0].isDir && es[0].path == "package" &&
                  es[1].path == "package/polyglot-plugin.json" && es[1].data == manifest,
              "P30 s0: tar reads an npm-shaped ustar archive");

        // pax extended header overrides the (truncated) name field of the entry that follows.
        const std::string longPath = "package/deeply/nested/dirs/going/on/for/quite/a/while/to/exceed/"
                                     "the/ustar/name/field/limit/of/one/hundred/characters/manifest.json";
        std::string rec = " path=" + longPath + "\n";
        std::size_t recLen = rec.size() + 2; // 2-digit guess, then fix up
        recLen = std::to_string(recLen).size() + rec.size();
        rec = std::to_string(recLen) + rec;
        std::string ar2 = hdr("PaxHeader/manifest.json", rec.size(), 'x') + pad512(rec) +
                          hdr("package/short-name", 3, '0') + pad512("hey") + endMark;
        es.clear();
        check(cli::tarRead(ar2, es, err) && es.size() == 1 && es[0].path == longPath && es[0].data == "hey",
              "P30 s0: tar honors a pax path override");

        // The ustar prefix field joins ahead of name.
        std::string ar3 = hdr("file.json", 2, '0', "package/pfx") + pad512("{}") + endMark;
        es.clear();
        check(cli::tarRead(ar3, es, err) && es.size() == 1 && es[0].path == "package/pfx/file.json",
              "P30 s0: tar joins the ustar prefix field");

        // Zip-slip refusals: traversal, absolute, backslash, and link entries all fail the READ.
        for (const auto& evil : {std::string("package/../evil"), std::string("/abs"), std::string("a\\b")}) {
            std::string bad = hdr(evil, 1, '0') + pad512("x") + endMark;
            es.clear();
            check(!cli::tarRead(bad, es, err) && has(err, "unsafe path") && es.empty(),
                  "P30 s0: tar refuses unsafe path '" + evil + "'");
        }
        std::string link = hdr("package/link", 0, '2') + endMark;
        es.clear();
        check(!cli::tarRead(link, es, err) && has(err, "unsupported entry type"),
              "P30 s0: tar refuses a symlink entry");

        // A corrupted header checksum fails loudly.
        std::string badSum = ar;
        badSum[0] ^= 1;
        es.clear();
        check(!cli::tarRead(badSum, es, err) && has(err, "checksum"),
              "P30 s0: tar refuses a header checksum mismatch");

        // Truncation (entry claims more data than the buffer holds) fails loudly.
        std::string trunc = hdr("package/big", 100000, '0') + pad512("tiny");
        es.clear();
        check(!cli::tarRead(trunc, es, err) && has(err, "overruns"),
              "P30 s0: tar refuses an entry that overruns the archive");
    }

    // pgconfig glue: the walk-up loader and the POLYGLOT_CACHE override (P30's test seam).
    {
        namespace fs = std::filesystem;
        const fs::path base = fs::temp_directory_path() / "polyglot-p30-s0-test";
        std::error_code ec;
        fs::remove_all(base, ec);
        fs::create_directories(base / "sub" / "inner");
        cli::writeFile(base / "pgconfig.json",
                       "{ \"root\": \"src\", \"targets\": [\"python\"],"
                       " \"dependencies\": { \"python\": \"^0.3.0\" } }");
        const cli::PgConfig pc = cli::loadPgConfig(base / "sub" / "inner");
        check(pc.found && pc.dir == base, "P30 s0: loadPgConfig walks up to the config");
        check(pc.targets.size() == 1 && pc.targets[0] == "python", "P30 s0: pgconfig targets parsed");
        check(pc.dependencies.size() == 1 && pc.dependencies[0].first == "python" &&
                  pc.dependencies[0].second == "^0.3.0",
              "P30 s0: pgconfig versioned dependency spec parsed verbatim");

        const fs::path cacheOverride = base / "cache";
#ifdef _WIN32
        _putenv_s("POLYGLOT_CACHE", cacheOverride.string().c_str());
#else
        setenv("POLYGLOT_CACHE", cacheOverride.string().c_str(), 1);
#endif
        check(cli::pluginCacheDir() == cacheOverride, "P30 s0: POLYGLOT_CACHE overrides the plugin cache dir");
#ifdef _WIN32
        _putenv_s("POLYGLOT_CACHE", "");
#else
        unsetenv("POLYGLOT_CACHE");
#endif
        check(cli::pluginCacheDir() != cacheOverride, "P30 s0: clearing POLYGLOT_CACHE restores the default");
        fs::remove_all(base, ec);
    }

    // ---- P30 slice 1: semver subset + registry client ----------------------------------------

    // Version parsing + precedence (semver spec §11, incl. the classic prerelease ordering chain).
    {
        auto v = [](const char* s) { return *cli::parseSemVer(s); };
        check(cli::parseSemVer("1.2.3") && v("1.2.3").major == 1 && v("1.2.3").patch == 3,
              "P30 s1: parseSemVer parses a release version");
        check(cli::parseSemVer("v0.3.1") && !v("v0.3.1").isPrerelease(), "P30 s1: leading 'v' tolerated");
        check(v("1.2.3-rc.1").prerelease.size() == 2, "P30 s1: prerelease identifiers split");
        check(cli::parseSemVer("1.2.3+build.5") && !v("1.2.3+build.5").isPrerelease(),
              "P30 s1: build metadata parsed and ignored");
        check(!cli::parseSemVer("1.2") && !cli::parseSemVer("1.x") && !cli::parseSemVer(""),
              "P30 s1: partial/x-range versions are refused");

        const char* chain[] = {"1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
                               "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0"};
        bool ordered = true;
        for (int i = 0; i + 1 < 8; ++i)
            ordered &= cli::compareSemVer(v(chain[i]), v(chain[i + 1])) < 0;
        check(ordered, "P30 s1: the spec's prerelease precedence chain orders correctly");
    }

    // Range semantics: caret/tilde desugar, prerelease exclusion, maxSatisfying.
    {
        std::string err;
        auto sat = [&](const char* range, const char* ver) {
            return cli::satisfies(*cli::parseSemVer(ver), *cli::parseRange(range, err));
        };
        check(sat("^1.2.3", "1.2.3") && sat("^1.2.3", "1.9.9") && !sat("^1.2.3", "2.0.0") && !sat("^1.2.3", "1.2.2"),
              "P30 s1: ^1.2.3 = >=1.2.3 <2.0.0");
        check(sat("^0.2.3", "0.2.9") && !sat("^0.2.3", "0.3.0"), "P30 s1: ^0.2.3 = >=0.2.3 <0.3.0");
        check(sat("^0.0.3", "0.0.3") && !sat("^0.0.3", "0.0.4"), "P30 s1: ^0.0.3 pins the patch");
        check(sat("~1.2.3", "1.2.9") && !sat("~1.2.3", "1.3.0"), "P30 s1: ~1.2.3 allows patch drift only");
        check(sat(">=1.2.3", "2.5.0") && !sat(">=1.2.3", "1.2.2"), "P30 s1: >= has an open upper bound");
        check(sat("1.2.3", "1.2.3") && !sat("1.2.3", "1.2.4"), "P30 s1: a bare version is exact");
        check(!sat("^1.2.3", "1.3.0-rc.1"), "P30 s1: prereleases are invisible to a release range");
        check(sat("^1.2.3-rc.1", "1.2.3-rc.2") && !sat("^1.2.3-rc.1", "1.2.4-rc.1") && sat("^1.2.3-rc.1", "1.2.4"),
              "P30 s1: a prerelease anchor admits prereleases of its own core tuple only");
        check(!cli::parseRange("1.x", err) && has(err, "unsupported version spec"),
              "P30 s1: outside-the-grammar ranges are refused with the supported list");

        const std::vector<std::string> vers = {"0.2.9", "0.3.0", "0.3.1", "0.4.0-rc.1", "0.4.0"};
        check(cli::maxSatisfying(vers, *cli::parseRange("^0.3.0", err), "") == std::optional<std::string>("0.3.1"),
              "P30 s1: maxSatisfying picks the highest in-range version");
        check(cli::maxSatisfying(vers, *cli::parseRange(">=0.2.0", err), "") == std::optional<std::string>("0.4.0"),
              "P30 s1: maxSatisfying skips prereleases even at the top");
        check(cli::maxSatisfying(vers, *cli::parseRange("latest", err), "0.3.1") == std::optional<std::string>("0.3.1"),
              "P30 s1: 'latest' resolves through the dist-tag");
        check(!cli::maxSatisfying(vers, *cli::parseRange("^1.0.0", err), ""),
              "P30 s1: an unsatisfiable range resolves to nothing");
    }

    // Registry client: URL encoding, packument parsing (abbreviated shape), error responses.
    {
        check(cli::packumentUrl("https://registry.npmjs.org", "@mintplayer/polyglot-target-python") ==
                  "https://registry.npmjs.org/@mintplayer%2Fpolyglot-target-python",
              "P30 s1: the packument URL encodes the scope slash");

        const std::string fixture = R"({
            "name": "@mintplayer/polyglot-target-python",
            "dist-tags": { "latest": "0.3.1" },
            "versions": {
                "0.3.0": { "version": "0.3.0", "dist": {
                    "tarball": "https://registry.npmjs.org/@mintplayer/polyglot-target-python/-/polyglot-target-python-0.3.0.tgz",
                    "integrity": "sha512-AAAA" } },
                "0.3.1": { "version": "0.3.1", "dist": {
                    "tarball": "https://registry.npmjs.org/@mintplayer/polyglot-target-python/-/polyglot-target-python-0.3.1.tgz",
                    "integrity": "sha512-BBBB", "shasum": "abc123" } }
            }
        })";
        cli::Packument pk;
        std::string err;
        check(cli::parsePackument(fixture, pk, err) && pk.versions.size() == 2 &&
                  pk.distTags["latest"] == "0.3.1" && pk.versions[1].integrity == "sha512-BBBB" &&
                  has(pk.versions[0].tarball, "-0.3.0.tgz"),
              "P30 s1: parsePackument reads the abbreviated shape");
        cli::Packument none;
        check(!cli::parsePackument(R"({"error":"Not found"})", none, err) && has(err, "Not found"),
              "P30 s1: a registry error body becomes a clean diagnostic");
    }

    // Registry selection: POLYGLOT_REGISTRY env, then .npmrc (scoped beats unscoped), then npmjs.
    {
        namespace fs = std::filesystem;
        const fs::path base = fs::temp_directory_path() / "polyglot-p30-s1-test";
        std::error_code ec;
        fs::remove_all(base, ec);
        fs::create_directories(base / "proj");
        check(cli::registryBaseFor("@x/y", base / "proj") == "https://registry.npmjs.org",
              "P30 s1: the public registry is the default");
        cli::writeFile(base / ".npmrc",
                       "# comment\nregistry=https://mirror.example/npm/\n"
                       "@mintplayer:registry=https://plugins.example/registry\n");
        check(cli::registryBaseFor("@mintplayer/polyglot-target-python", base / "proj") ==
                  "https://plugins.example/registry",
              "P30 s1: a scoped .npmrc registry wins for its scope");
        check(cli::registryBaseFor("left-pad", base / "proj") == "https://mirror.example/npm",
              "P30 s1: the unscoped .npmrc registry applies otherwise (trailing '/' trimmed)");
#ifdef _WIN32
        _putenv_s("POLYGLOT_REGISTRY", "http://127.0.0.1:9999/");
#else
        setenv("POLYGLOT_REGISTRY", "http://127.0.0.1:9999/", 1);
#endif
        check(cli::registryBaseFor("@mintplayer/polyglot-target-python", base / "proj") == "http://127.0.0.1:9999",
              "P30 s1: POLYGLOT_REGISTRY overrides everything (the test-harness seam)");
#ifdef _WIN32
        _putenv_s("POLYGLOT_REGISTRY", "");
#else
        unsetenv("POLYGLOT_REGISTRY");
#endif
        fs::remove_all(base, ec);
    }

    // ---- P30 slice 2: pgconfig.lock.json + the versioned plugin cache ------------------------

    // Lockfile round-trip, stable emission, and the ignore-unknown-versions rule.
    {
        namespace fs = std::filesystem;
        const fs::path base = fs::temp_directory_path() / "polyglot-p30-s2-lock";
        std::error_code ec;
        fs::remove_all(base, ec);
        fs::create_directories(base);

        check(!cli::loadLockfile(base).found, "P30 s2: a missing lockfile reads as not-found");

        cli::Lockfile lf;
        lf.packages["@mintplayer/polyglot-target-python"] = {"0.3.1",
            "https://registry.npmjs.org/@mintplayer/polyglot-target-python/-/polyglot-target-python-0.3.1.tgz",
            "sha512-abc"};
        lf.packages["@mintplayer/polyglot-target-php"] = {"0.3.0", "https://x/php-0.3.0.tgz", "sha512-def"};
        check(cli::saveLockfile(base, lf), "P30 s2: saveLockfile writes");
        const cli::Lockfile rt = cli::loadLockfile(base);
        check(rt.found && rt.packages.size() == 2 &&
                  rt.packages.at("@mintplayer/polyglot-target-python").version == "0.3.1" &&
                  rt.packages.at("@mintplayer/polyglot-target-python").integrity == "sha512-abc" &&
                  has(rt.packages.at("@mintplayer/polyglot-target-php").resolved, "php-0.3.0.tgz"),
              "P30 s2: lockfile round-trips");
        std::string emitted1, emitted2;
        cli::readFile(cli::lockfilePath(base), emitted1);
        cli::saveLockfile(base, rt);
        cli::readFile(cli::lockfilePath(base), emitted2);
        check(emitted1 == emitted2 && has(emitted1, "\"lockfileVersion\": 1"),
              "P30 s2: lockfile emission is byte-stable (committed diffs stay quiet)");

        cli::writeFile(cli::lockfilePath(base), "{ \"lockfileVersion\": 99, \"packages\": {} }");
        check(!cli::loadLockfile(base).found, "P30 s2: an unknown future lockfileVersion is ignored");
        fs::remove_all(base, ec);
    }

    // Versioned cache: store/load, tamper refusal, lock-integrity cross-check, atomic layout.
    {
        namespace fs = std::filesystem;
        const fs::path cache = fs::temp_directory_path() / "polyglot-p30-s2-cache";
        std::error_code ec;
        fs::remove_all(cache, ec);
#ifdef _WIN32
        _putenv_s("POLYGLOT_CACHE", cache.string().c_str());
#else
        setenv("POLYGLOT_CACHE", cache.string().c_str(), 1);
#endif
        const std::string name = "@mintplayer/polyglot-target-python";
        const std::string manifest = "{ \"schema\": 2, \"name\": \"python\" }";
        std::string err, loaded;

        check(!cli::cacheLoad(name, "0.3.1", "", loaded, err) && has(err, "no entry"),
              "P30 s2: a cold cache misses cleanly");
        check(cli::cacheStore(name, "0.3.1", "https://x/py-0.3.1.tgz", "sha512-abc", manifest, err),
              "P30 s2: cacheStore writes a versioned entry");
        check(fs::exists(cache / "@mintplayer" / "polyglot-target-python" / "0.3.1" / "polyglot-plugin.json"),
              "P30 s2: the cache layout is <name>/<version>/ (scoped names nest)");
        loaded.clear();
        check(cli::cacheLoad(name, "0.3.1", "sha512-abc", loaded, err) && loaded == manifest,
              "P30 s2: cacheLoad verifies and returns the manifest");
        check(!cli::cacheLoad(name, "0.3.1", "sha512-OTHER", loaded, err) && has(err, "lockfile pins"),
              "P30 s2: a lock-integrity mismatch refuses the entry");

        // Tamper with the cached manifest on disk: the re-hash refuses it.
        cli::writeFile(cache / "@mintplayer" / "polyglot-target-python" / "0.3.1" / "polyglot-plugin.json",
                       "{ \"schema\": 2, \"name\": \"python\", \"evil\": true }");
        check(!cli::cacheLoad(name, "0.3.1", "sha512-abc", loaded, err) && has(err, "hash mismatch"),
              "P30 s2: a tampered cache entry reads as absent-with-reason");
        // And a fresh store over the refused entry heals it.
        check(cli::cacheStore(name, "0.3.1", "https://x/py-0.3.1.tgz", "sha512-abc", manifest, err) &&
                  cli::cacheLoad(name, "0.3.1", "sha512-abc", loaded, err),
              "P30 s2: re-storing over a refused entry heals the cache");

        check(!cli::cacheStore("../evil", "1.0.0", "u", "i", manifest, err) && has(err, "unsafe"),
              "P30 s2: a path-hostile package name is refused outright");
        check(!cli::cacheStore("a/b/c", "1.0.0", "u", "i", manifest, err),
              "P30 s2: more than one scope slash is refused");
#ifdef _WIN32
        _putenv_s("POLYGLOT_CACHE", "");
#else
        unsetenv("POLYGLOT_CACHE");
#endif
        fs::remove_all(cache, ec);
    }

    // ---- P30 slice 3: the auto-download pipeline (fake transport, real pipeline) --------------
    {
        namespace fs = std::filesystem;

        // npm-shaped .tgz built in-test: ustar entry under package/ + stored-block gzip (the
        // decoder's Huffman paths are covered by the slice-0 golden vectors).
        auto tarFile = [](const std::string& path, const std::string& data) {
            std::string h(512, '\0');
            std::memcpy(&h[0], path.data(), path.size());
            auto oct = [&](std::size_t at, std::size_t width, unsigned long long v) {
                for (std::size_t i = width - 1; i-- > 0;) { h[at + i] = static_cast<char>('0' + (v & 7)); v >>= 3; }
                h[at + width - 1] = '\0';
            };
            oct(100, 8, 0644); oct(108, 8, 0); oct(116, 8, 0);
            oct(124, 12, data.size()); oct(136, 12, 0);
            std::memset(&h[148], ' ', 8);
            h[156] = '0';
            std::memcpy(&h[257], "ustar", 6); h[263] = '0'; h[264] = '0';
            unsigned sum = 0;
            for (unsigned char c : h) sum += c;
            for (int i = 5; i >= 0; --i) { h[148 + i] = static_cast<char>('0' + (sum & 7)); sum >>= 3; }
            h[154] = '\0'; h[155] = ' ';
            std::string padded = data;
            padded.resize((padded.size() + 511) & ~std::size_t(511), '\0');
            return h + padded + std::string(1024, '\0');
        };
        auto gzipStored = [](const std::string& p) {
            std::string gz("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03", 10);
            std::size_t off = 0;
            do {
                const std::size_t chunk = std::min<std::size_t>(p.size() - off, 65535);
                const bool last = off + chunk == p.size();
                gz += last ? '\x01' : '\x00';
                gz += static_cast<char>(chunk & 0xFF); gz += static_cast<char>(chunk >> 8);
                gz += static_cast<char>(~chunk & 0xFF); gz += static_cast<char>((~chunk >> 8) & 0xFF);
                gz.append(p, off, chunk);
                off += chunk;
            } while (off < p.size());
            const std::uint32_t crc = cli::crc32(p.data(), p.size());
            for (int i = 0; i < 4; ++i) gz += static_cast<char>(crc >> (8 * i));
            const std::uint32_t sz = static_cast<std::uint32_t>(p.size());
            for (int i = 0; i < 4; ++i) gz += static_cast<char>(sz >> (8 * i));
            return gz;
        };
        // A REAL, fully valid manifest: the in-box python plugin renamed (schema + spec name must agree).
        auto renamedManifest = [&](const std::string& newName) {
            std::string m;
            cli::readFile(cli::executablePath().parent_path() / "plugins" / "python" / "polyglot-plugin.json", m);
            const std::string from = "\"name\": \"python\"", to = "\"name\": \"" + newName + "\"";
            for (std::size_t at; (at = m.find(from)) != std::string::npos;) m.replace(at, from.size(), to);
            return m;
        };
        auto packument = [](const std::string& name, const std::string& version, const std::string& tarballUrl,
                            const std::string& integrity) {
            return "{ \"name\": \"" + name + "\", \"dist-tags\": { \"latest\": \"" + version + "\" }, "
                   "\"versions\": { \"0.9.0\": { \"dist\": { \"tarball\": \"" + tarballUrl + "-0.9.0\", "
                   "\"integrity\": \"sha512-stale\" } }, \"" + version + "\": { \"dist\": { \"tarball\": \"" +
                   tarballUrl + "\", \"integrity\": \"" + integrity + "\" } } } }";
        };

        const fs::path base = fs::temp_directory_path() / "polyglot-p30-s3";
        std::error_code ec;
        fs::remove_all(base, ec);
        fs::create_directories(base / "proj");
#ifdef _WIN32
        _putenv_s("POLYGLOT_CACHE", (base / "cache").string().c_str());
#else
        setenv("POLYGLOT_CACHE", (base / "cache").string().c_str(), 1);
#endif

        std::map<std::string, std::string> served; // url -> body; absent = network failure
        int hits = 0;
        cli::HttpGet fake = [&](const std::string& url, const std::vector<std::string>&, std::string& body,
                                std::string& err) {
            ++hits;
            if (const auto it = served.find(url); it != served.end()) { body = it->second; return true; }
            err = "http: offline (test)";
            return false;
        };

        // Happy path: packument -> maxSatisfying -> download -> verify -> extract -> cache -> lock.
        const std::string pkg = "@mintplayer/polyglot-target-pydemo";
        const std::string tgz = gzipStored(tarFile("package/polyglot-plugin.json", renamedManifest("pydemo")));
        const std::string tgzUrl = "https://registry.npmjs.org/" + pkg + "/-/polyglot-target-pydemo-1.1.0.tgz";
        served["https://registry.npmjs.org/@mintplayer%2Fpolyglot-target-pydemo"] =
            packument(pkg, "1.1.0", tgzUrl, cli::sriSha512(tgz));
        served[tgzUrl] = tgz;

        cli::PgConfig pc;
        pc.found = true;
        pc.dir = base / "proj";
        pc.targets = {"pydemo"};
        pc.dependencies = {{"pydemo", "^1.0.0"}};
        cli::writeFile(pc.dir / "pgconfig.json", "{}"); // stamp source for the memo key

        cli::ResolveState st1;
        cli::ResolveResult r1 = cli::resolvePluginDependencies(pc, fake, false, &st1);
        check(r1.ok && r1.lockChanged && r1.messages.empty(), "P30 s3: a versioned dependency resolves clean");
        check(findBackend("pydemo") != nullptr, "P30 s3: the downloaded plugin registers its target");
        check(hits == 2, "P30 s3: exactly two requests (packument + tarball)");
        const cli::Lockfile lk = cli::loadLockfile(pc.dir);
        check(lk.found && lk.packages.count(pkg) && lk.packages.at(pkg).version == "1.1.0" &&
                  lk.packages.at(pkg).integrity == cli::sriSha512(tgz),
              "P30 s3: the lockfile pins version + SRI integrity");

        // Already-registered (and dev-CLI in-box rule): a re-resolve with fresh state is network-free.
        hits = 0;
        cli::ResolveState st2;
        check(cli::resolvePluginDependencies(pc, fake, false, &st2).ok && hits == 0,
              "P30 s3: an already-registered dependency costs zero network");

        // Lock-first offline: cache + lock present, network dead, target not yet loaded.
        {
            const std::string pkg2 = "@mintplayer/polyglot-target-pydemo2";
            const std::string man2 = renamedManifest("pydemo2");
            std::string err;
            check(cli::cacheStore(pkg2, "2.0.0", "https://x/pydemo2-2.0.0.tgz", "sha512-pin", man2, err),
                  "P30 s3: (setup) pre-warmed cache");
            cli::Lockfile lock2 = cli::loadLockfile(pc.dir);
            lock2.packages[pkg2] = {"2.0.0", "https://x/pydemo2-2.0.0.tgz", "sha512-pin"};
            cli::saveLockfile(pc.dir, lock2);
            cli::PgConfig pc2 = pc;
            pc2.targets = {"pydemo2"};
            pc2.dependencies = {{"pydemo2", "^2.0.0"}};
            served.clear();
            hits = 0;
            cli::ResolveState st;
            const cli::ResolveResult r = cli::resolvePluginDependencies(pc2, fake, false, &st);
            check(r.ok && hits == 0 && findBackend("pydemo2") != nullptr,
                  "P30 s3: lock+cache resolve fully OFFLINE (zero network I/O)");
        }

        // Integrity mismatch: the served tarball does not match the packument's SRI -> refused, uncached.
        {
            const std::string pkg3 = "@mintplayer/polyglot-target-pydemo3";
            const std::string tgz3 = gzipStored(tarFile("package/polyglot-plugin.json", renamedManifest("pydemo3")));
            const std::string url3 = "https://registry.npmjs.org/" + pkg3 + "/-/x-3.0.0.tgz";
            served["https://registry.npmjs.org/@mintplayer%2Fpolyglot-target-pydemo3"] =
                packument(pkg3, "3.0.0", url3, cli::sriSha512("something else entirely"));
            served[url3] = tgz3;
            cli::PgConfig pc3 = pc;
            pc3.dependencies = {{"pydemo3", "^3.0.0"}};
            cli::ResolveState st;
            const cli::ResolveResult r = cli::resolvePluginDependencies(pc3, fake, false, &st);
            check(!r.ok && r.messages.size() == 1 && has(r.messages[0], "integrity mismatch"),
                  "P30 s3: a tarball failing its SRI check is refused with the mismatch named");
            check(r.networkMisses.empty(),
                  "P40 review r6: a tarball failing its SRI check is an error, never a network miss (it was 'not verified')");
            std::string m, err;
            check(!cli::cacheLoad(pkg3, "3.0.0", "", m, err) && findBackend("pydemo3") == nullptr,
                  "P30 s3: nothing is cached or registered from a refused tarball");
        }

        // Unsatisfiable range + failure memoization (no re-network within a config generation).
        // (A fresh package name: an already-REGISTERED target short-circuits on the in-box rule.)
        {
            cli::PgConfig pc4 = pc;
            pc4.dependencies = {{"pydemo4", "^9.0.0"}};
            served["https://registry.npmjs.org/@mintplayer%2Fpolyglot-target-pydemo4"] =
                packument("@mintplayer/polyglot-target-pydemo4", "1.1.0", tgzUrl + "-p4", cli::sriSha512(tgz));
            cli::ResolveState st;
            cli::ResolveResult r = cli::resolvePluginDependencies(pc4, fake, false, &st);
            check(!r.ok && has(r.messages[0], "no published version satisfies '^9.0.0'"),
                  "P30 s3: an unsatisfiable range names the range and what exists");
            hits = 0;
            r = cli::resolvePluginDependencies(pc4, fake, false, &st);
            check(!r.ok && hits == 0 && has(r.messages[0], "no published version satisfies"),
                  "P30 s3: a failed dependency is memoized per config generation (no network hammering)");
        }

        // Offline with no lock: the transport's diagnostic reaches the user.
        {
            cli::PgConfig pc5 = pc;
            pc5.dependencies = {{"pydemo9", "^1.0.0"}};
            served.clear();
            cli::ResolveState st;
            const cli::ResolveResult r = cli::resolvePluginDependencies(pc5, fake, false, &st);
            check(!r.ok && has(r.messages[0], "offline (test)"),
                  "P30 s3: offline-with-no-lock surfaces the transport diagnostic");
            check(r.networkMisses.size() == 1,
                  "P40 review r6: a GET that got no answer (transport failure) is a network miss");
        }

        // P40 review r6: the registry ANSWERING with a non-200 status (a 404 for an unpublished package) is an
        // answer, not a missing network — an error, never degraded to "not verified".
        {
            cli::PgConfig pc404 = pc;
            pc404.dependencies = {{"pydemo404", "^1.0.0"}};
            const cli::HttpGet answers404 = [&](const std::string& url, const std::vector<std::string>&, std::string&,
                                                std::string& err) -> cli::HttpOutcome {
                err = "http: status 404 for " + url;
                return cli::HttpOutcome::Kind::Response;
            };
            cli::ResolveState st;
            const cli::ResolveResult r = cli::resolvePluginDependencies(pc404, answers404, false, &st);
            check(!r.ok && has(r.messages[0], "status 404") && r.networkMisses.empty(),
                  "P40 review r6: a registry 404 is a dependency error, not a network miss");
            const cli::ResolveResult again = cli::resolvePluginDependencies(pc404, answers404, false, &st);
            check(!again.ok && again.networkMisses.empty(), "P40 review r6: ... also when replayed from the memo");
        }

        // P40 review r7: the status classification every real client shares — a 5xx (a proxy with no upstream, an
        // unavailable registry) is a network miss; a 4xx stays the registry's answer.
        {
            std::string e;
            check(cli::httpStatusOutcome(502, "u", e).kind == cli::HttpOutcome::Kind::Transport && has(e, "status 502") &&
                      cli::httpStatusOutcome(503, "u", e).kind == cli::HttpOutcome::Kind::Transport &&
                      cli::httpStatusOutcome(504, "u", e).kind == cli::HttpOutcome::Kind::Transport,
                  "P40 review r7: a registry/proxy 502/503/504 is a network miss (Transport)");
            check(cli::httpStatusOutcome(404, "u", e).kind == cli::HttpOutcome::Kind::Response &&
                      cli::httpStatusOutcome(403, "u", e).kind == cli::HttpOutcome::Kind::Response &&
                      cli::httpStatusOutcome(301, "u", e).kind == cli::HttpOutcome::Kind::Response,
                  "P40 review r7: a 4xx (or any other non-200 answer) stays the registry's answer (Response)");
            cli::PgConfig pc503 = pc;
            pc503.dependencies = {{"pydemo503", "^1.0.0"}};
            const cli::HttpGet answers503 = [&](const std::string& url, const std::vector<std::string>&, std::string&,
                                                std::string& err) -> cli::HttpOutcome {
                return cli::httpStatusOutcome(503, url, err);
            };
            const cli::ResolveResult r = cli::resolvePluginDependencies(pc503, answers503, false, nullptr);
            check(!r.ok && r.networkMisses.size() == 1, "P40 review r7: a registry 503 is a network miss, as offline is");
        }

        // G37 (wave 2): a lock pin OUTSIDE an edited range re-resolves (online), and REFUSES offline —
        // never silently keeps the stale plugin.
        {
            const std::string pkg7 = "@mintplayer/polyglot-target-pydemo7";
            cli::Lockfile stale = cli::loadLockfile(pc.dir);
            stale.found = true;
            stale.packages[pkg7] = {"0.9.0", "https://registry.npmjs.org/x-0.9.0.tgz", "sha512-stale"};
            check(cli::saveLockfile(pc.dir, stale), "G37: (setup) stale lock pin written");

            const std::string tgz7 = gzipStored(tarFile("package/polyglot-plugin.json", renamedManifest("pydemo7")));
            const std::string url7 = "https://registry.npmjs.org/" + pkg7 + "/-/p7-1.1.0.tgz";
            served.clear();
            served["https://registry.npmjs.org/@mintplayer%2Fpolyglot-target-pydemo7"] =
                packument(pkg7, "1.1.0", url7, cli::sriSha512(tgz7));
            served[url7] = tgz7;
            hits = 0;
            cli::PgConfig pc7 = pc;
            pc7.dependencies = {{"pydemo7", "^1.0.0"}}; // the 0.9.0 pin does NOT satisfy this
            cli::ResolveState st;
            const cli::ResolveResult r = cli::resolvePluginDependencies(pc7, fake, false, &st);
            const cli::Lockfile after = cli::loadLockfile(pc.dir);
            check(r.ok && hits > 0 && after.packages.count(pkg7) && after.packages.at(pkg7).version == "1.1.0",
                  "G37: a pin outside the edited range re-resolves and advances the lock");
        }
        {
            const std::string pkg8 = "@mintplayer/polyglot-target-pydemo8";
            cli::Lockfile stale = cli::loadLockfile(pc.dir);
            stale.found = true;
            stale.packages[pkg8] = {"0.9.0", "https://registry.npmjs.org/x8-0.9.0.tgz", "sha512-stale"};
            cli::saveLockfile(pc.dir, stale);
            served.clear(); // network dead
            cli::PgConfig pc8 = pc;
            pc8.dependencies = {{"pydemo8", "^1.0.0"}};
            cli::ResolveState st;
            const cli::ResolveResult r = cli::resolvePluginDependencies(pc8, fake, false, &st);
            check(!r.ok, "G37: an out-of-range pin refuses OFFLINE (never silently keeps the old plugin)");
        }

        // G37 (wave 2): `update=true` advances a SATISFIED pin to the newest satisfying version.
        {
            const std::string pkg9 = "@mintplayer/polyglot-target-pydemo9u";
            const std::string tgz12 = gzipStored(tarFile("package/polyglot-plugin.json", renamedManifest("pydemo9u")));
            const std::string url12 = "https://registry.npmjs.org/" + pkg9 + "/-/p9-1.2.0.tgz";
            cli::Lockfile lk9 = cli::loadLockfile(pc.dir);
            lk9.found = true;
            lk9.packages[pkg9] = {"1.1.0", "https://registry.npmjs.org/x9-1.1.0.tgz", "sha512-oldpin"};
            cli::saveLockfile(pc.dir, lk9);
            served.clear();
            served["https://registry.npmjs.org/@mintplayer%2Fpolyglot-target-pydemo9u"] =
                packument(pkg9, "1.2.0", url12, cli::sriSha512(tgz12));
            served[url12] = tgz12;
            cli::PgConfig pc9 = pc;
            pc9.dependencies = {{"pydemo9u", "^1.0.0"}};
            hits = 0;
            cli::ResolveState st;
            const cli::ResolveResult r = cli::resolvePluginDependencies(pc9, fake, /*update=*/true, &st);
            const cli::Lockfile after = cli::loadLockfile(pc.dir);
            check(r.ok && hits > 0 && after.packages.count(pkg9) && after.packages.at(pkg9).version == "1.2.0",
                  "G37: update=true advances a satisfied pin to the newest satisfying version");
        }

        // A package that is not a Polyglot plugin (no manifest in the tarball) is named as such.
        {
            const std::string pkg6 = "@mintplayer/polyglot-target-notaplugin";
            const std::string tgz6 = gzipStored(tarFile("package/index.js", "module.exports = 1;"));
            const std::string url6 = "https://registry.npmjs.org/" + pkg6 + "/-/n-1.0.0.tgz";
            served["https://registry.npmjs.org/@mintplayer%2Fpolyglot-target-notaplugin"] =
                packument(pkg6, "1.0.0", url6, cli::sriSha512(tgz6));
            served[url6] = tgz6;
            cli::PgConfig pc6 = pc;
            pc6.dependencies = {{"notaplugin", "1.0.0"}};
            cli::ResolveState st;
            const cli::ResolveResult r = cli::resolvePluginDependencies(pc6, fake, false, &st);
            check(!r.ok && has(r.messages[0], "not a Polyglot target plugin"),
                  "P30 s3: a non-plugin npm package is refused by name");
        }

#ifdef _WIN32
        _putenv_s("POLYGLOT_CACHE", "");
#else
        unsetenv("POLYGLOT_CACHE");
#endif
        fs::remove_all(base, ec);
    }

    // ---- P30 slice 4: the derived reference backend (no hard-bound "csharp") -----------------
    {
        // Among the loaded fixture set, python/php carry `emulated` stances; csharp and typescript
        // are all-native, and name order breaks the tie — so the DERIVATION lands on csharp. The
        // assertion pins the derivation over this known set, not a hard-bound name in the product.
        const Backend* ref = cli::referenceBackend();
        check(ref != nullptr && ref->name() == "csharp",
              "P30 s4: referenceBackend derives the all-native, name-first backend");
        bool allNative = true;
        for (const Feature f : kAllFeatures) allNative &= ref->capabilityStance(f) == "native";
        check(allNative, "P30 s4: the derived reference backend is all-native (never warns spuriously)");
    }

}
