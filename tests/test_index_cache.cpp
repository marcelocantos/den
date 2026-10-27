// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Binary index lookup cache (index_cache.h): lazy lookups must be
// indistinguishable from parsing index.json, and a stale, missing or corrupt
// cache must always fall back to index.json.

#include <doctest.h>

#include "index/index.h"
#include "index/index_cache.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace den {
namespace test {

namespace {

namespace fs = std::filesystem;

struct IndexCacheTmpDir {
    fs::path path;

    IndexCacheTmpDir() {
        path = fs::temp_directory_path() /
               ("den_test_index_cache_" +
                std::to_string(static_cast<size_t>(
                    std::chrono::steady_clock::now().time_since_epoch().count())));
        fs::create_directories(path);
    }
    ~IndexCacheTmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    IndexCacheTmpDir(const IndexCacheTmpDir&) = delete;
    IndexCacheTmpDir& operator=(const IndexCacheTmpDir&) = delete;
};

struct EnvGuard {
    std::string name;
    explicit EnvGuard(const char* n, const char* v) : name(n) { ::setenv(n, v, 1); }
    ~EnvGuard() { ::unsetenv(name.c_str()); }
};

Package formula(const std::string& name, const std::string& version,
                std::vector<std::string> deps = {}) {
    Package p;
    p.name = name;
    p.version = version;
    p.description = "Description of " + name;
    p.homepage = "https://example.com/" + name;
    p.license = "MIT";
    p.dependencies = std::move(deps);
    p.build_dependencies = {"pkgconf"};
    p.stability = Stability::Stable;
    p.archives["arm64_sequoia"] = {"https://ghcr.io/" + name + "/arm", std::string(64, 'a'),
                                   RelocationType::NeedsSubstitution};
    p.archives["x86_64_sonoma"] = {"https://ghcr.io/" + name + "/x86", std::string(64, 'b'),
                                   RelocationType::HardcodedPrefix, "/usr/local/Cellar"};
    p.source_url = "https://example.com/" + name + ".tar.gz";
    p.source_sha256 = std::string(64, 'c');
    p.ruby_source_path = "Formula/" + name.substr(0, 1) + "/" + name + ".rb";
    return p;
}

Package cask(const std::string& name) {
    Package p;
    p.name = name;
    p.version = "1.2.3";
    p.description = "A cask";
    p.artifact_type = ArtifactType::App;
    p.archives["default"] = {"https://dl.example.com/x86.dmg", "", RelocationType::Portable};
    p.archives["arm64_sequoia"] = {"https://dl.example.com/arm.dmg", std::string(64, 'd'),
                                   RelocationType::Portable};
    p.cask_artifacts.push_back({CaskArtifactKind::App, "Foo.app", ""});
    p.cask_artifacts.push_back({CaskArtifactKind::Binary, "Foo.app/bin/foo", "foo"});
    p.cask_artifacts.push_back({CaskArtifactKind::Pkg, "Foo.pkg", ""});
    p.cask_uninstall.quit = {"com.example.foo"};
    p.cask_uninstall.signal = {{"TERM", "com.example.foo"}, {"KILL", "com.example.helper"}};
    p.cask_uninstall.launchctl = {"com.example.foo.agent"};
    p.cask_uninstall.pkgutil = {"com.example.foo.*"};
    p.cask_uninstall.delete_paths = {"/Library/Foo"};
    p.cask_uninstall.trash = {"~/Library/Preferences/com.example.foo.plist"};
    p.cask_uninstall.rmdir = {"~/Library/Foo"};
    p.cask_uninstall.unsupported = {"script"};
    return p;
}

PackageIndex fixture() {
    PackageIndex idx;
    for (auto p : {formula("hello", "2.12.3"), formula("ffmpeg", "8.0", {"x264", "lame"}),
                   formula("x264", "r3222"), formula("lame", "3.100"), formula("docker", "28.1"),
                   formula("zstd", "1.5.7")}) {
        idx.packages[p.name] = p;
    }
    auto c = cask("cask/docker");
    idx.packages[c.name] = c;
    auto f = cask("firefox");
    f.deprecated = true;
    f.stability = Stability::Buggy;
    idx.packages[f.name] = f;
    return idx;
}

void check_same(const Package& a, const Package& b) {
    CHECK(a.name == b.name);
    CHECK(a.version == b.version);
    CHECK(a.description == b.description);
    CHECK(a.homepage == b.homepage);
    CHECK(a.license == b.license);
    CHECK(a.artifact_type == b.artifact_type);
    CHECK(a.stability == b.stability);
    CHECK(a.deprecated == b.deprecated);
    CHECK(a.disabled == b.disabled);
    CHECK(a.dependencies == b.dependencies);
    CHECK(a.build_dependencies == b.build_dependencies);
    CHECK(a.source_url == b.source_url);
    CHECK(a.source_sha256 == b.source_sha256);
    CHECK(a.ruby_source_path == b.ruby_source_path);
    CHECK(a.keg_only == b.keg_only);
    CHECK(a.keg_only_reason == b.keg_only_reason);
    CHECK(a.conflicts_with == b.conflicts_with);
    CHECK(a.cask_uninstall.quit == b.cask_uninstall.quit);
    CHECK(a.cask_uninstall.signal == b.cask_uninstall.signal);
    CHECK(a.cask_uninstall.launchctl == b.cask_uninstall.launchctl);
    CHECK(a.cask_uninstall.pkgutil == b.cask_uninstall.pkgutil);
    CHECK(a.cask_uninstall.delete_paths == b.cask_uninstall.delete_paths);
    CHECK(a.cask_uninstall.trash == b.cask_uninstall.trash);
    CHECK(a.cask_uninstall.rmdir == b.cask_uninstall.rmdir);
    CHECK(a.cask_uninstall.unsupported == b.cask_uninstall.unsupported);
    REQUIRE(a.archives.size() == b.archives.size());
    for (const auto& [tag, ar] : a.archives) {
        REQUIRE(b.archives.count(tag) == 1);
        CHECK(b.archives.at(tag).url == ar.url);
        CHECK(b.archives.at(tag).sha256 == ar.sha256);
        CHECK(b.archives.at(tag).relocation == ar.relocation);
        CHECK(b.archives.at(tag).cellar == ar.cellar);
    }
    REQUIRE(a.cask_artifacts.size() == b.cask_artifacts.size());
    for (size_t i = 0; i < a.cask_artifacts.size(); ++i) {
        CHECK(a.cask_artifacts[i].kind == b.cask_artifacts[i].kind);
        CHECK(a.cask_artifacts[i].source == b.cask_artifacts[i].source);
        CHECK(a.cask_artifacts[i].target == b.cask_artifacts[i].target);
    }
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

void spit(const fs::path& p, const std::string& s) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << s;
}

/// Every package must look the same via the lazy index as via a full parse.
void check_matches_json(const PackageIndex& lazy, const fs::path& json) {
    auto full = load_index_json(json);
    REQUIRE_FALSE(full.packages.empty());
    for (const auto& [name, pkg] : full.packages) {
        const auto* got = lazy.find(name);
        REQUIRE_MESSAGE(got != nullptr, name);
        check_same(pkg, *got);
    }
}

} // namespace

TEST_SUITE("index_cache") {

    TEST_CASE("save_index writes a cache that load_index serves lazily") {
        IndexCacheTmpDir tmp;
        auto json = tmp.path / "index.json";
        save_index(fixture(), json);
        REQUIRE(fs::exists(index_cache::cache_path_for(json)));

        auto idx = load_index(json);
        REQUIRE(idx.backing != nullptr);
        CHECK(idx.packages.empty()); // nothing decoded yet
        CHECK_FALSE(idx.empty());

        const auto* hello = idx.find("hello");
        REQUIRE(hello != nullptr);
        CHECK(hello->version == "2.12.3");
        CHECK(idx.packages.size() == 1);   // only what was asked for
        CHECK(idx.find("hello") == hello); // memoised, stable pointer
        CHECK(idx.find("no-such-package") == nullptr);
        CHECK(idx.find("") == nullptr);

        check_matches_json(idx, json);
    }

    TEST_CASE("cask variants, artifacts and cask/ prefix survive the cache") {
        IndexCacheTmpDir tmp;
        auto json = tmp.path / "index.json";
        save_index(fixture(), json);
        auto idx = load_index(json);
        REQUIRE(idx.backing != nullptr);

        const auto* d = idx.find("cask/docker");
        REQUIRE(d != nullptr);
        CHECK(d->artifact_type == ArtifactType::App);
        CHECK(d->archives.at("arm64_sequoia").url == "https://dl.example.com/arm.dmg");
        CHECK(d->archives.at("default").sha256.empty());
        REQUIRE(d->cask_artifacts.size() == 3);
        CHECK(d->cask_artifacts[1].kind == CaskArtifactKind::Binary);
        CHECK(d->cask_artifacts[1].target == "foo");
        // 🎯T12 uninstall stanzas survive the binary cache.
        REQUIRE(d->cask_uninstall.signal.size() == 2);
        CHECK(d->cask_uninstall.signal[1].first == "KILL");
        CHECK(d->cask_uninstall.pkgutil == std::vector<std::string>{"com.example.foo.*"});
        CHECK(d->cask_uninstall.unsupported == std::vector<std::string>{"script"});
        CHECK(idx.find("docker")->artifact_type == ArtifactType::Binary);

        const auto* ff = idx.find("firefox");
        REQUIRE(ff != nullptr);
        CHECK(ff->deprecated);
        CHECK(ff->stability == Stability::Buggy);
    }

    TEST_CASE("materialise_all yields the complete index for iteration") {
        IndexCacheTmpDir tmp;
        auto json = tmp.path / "index.json";
        save_index(fixture(), json);
        auto idx = load_index(json);
        REQUIRE(idx.backing != nullptr);
        idx.find("zstd");
        idx.materialise_all();
        CHECK(idx.backing == nullptr);
        auto full = load_index_json(json);
        REQUIRE(idx.packages.size() == full.packages.size());
        for (const auto& [name, pkg] : full.packages) {
            REQUIRE(idx.packages.count(name) == 1);
            check_same(pkg, idx.packages.at(name));
        }
    }

    TEST_CASE("materialised entries (e.g. merged taps) shadow the cache") {
        IndexCacheTmpDir tmp;
        auto json = tmp.path / "index.json";
        save_index(fixture(), json);
        auto idx = load_index(json);
        REQUIRE(idx.backing != nullptr);
        auto tap = formula("hello", "9.9.9");
        idx.packages["hello"] = tap;
        CHECK(idx.find("hello")->version == "9.9.9");
        idx.materialise_all();
        CHECK(idx.packages.at("hello").version == "9.9.9");
    }

    TEST_CASE("missing cache: load_index parses JSON and rebuilds the cache") {
        IndexCacheTmpDir tmp;
        auto json = tmp.path / "index.json";
        save_index(fixture(), json);
        fs::remove(index_cache::cache_path_for(json));

        std::string why;
        CHECK(index_cache::open(json, &why) == nullptr);
        CHECK(why == "missing");

        auto idx = load_index(json);
        CHECK(idx.backing == nullptr); // full parse this time
        CHECK(idx.find("hello")->version == "2.12.3");
        REQUIRE(fs::exists(index_cache::cache_path_for(json)));

        auto again = load_index(json);
        CHECK(again.backing != nullptr); // next run is lazy
        check_matches_json(again, json);
    }

    TEST_CASE("stale cache: index.json replaced with different content") {
        IndexCacheTmpDir tmp;
        auto json = tmp.path / "index.json";
        save_index(fixture(), json);
        auto old_cache = slurp(index_cache::cache_path_for(json));

        auto changed = fixture();
        changed.packages["hello"].version = "3.0.0-much-longer";
        changed.packages.erase("zstd");
        save_index(changed, json);
        // Simulate a cache left over from the previous index.json (e.g. the
        // file was replaced by something other than den update).
        spit(index_cache::cache_path_for(json), old_cache);

        std::string why;
        CHECK(index_cache::open(json, &why) == nullptr);
        CHECK(why == "stale (index.json changed)");

        auto idx = load_index(json);
        CHECK(idx.find("hello")->version == "3.0.0-much-longer");
        CHECK(idx.find("zstd") == nullptr);
        auto again = load_index(json);
        REQUIRE(again.backing != nullptr);
        CHECK(again.find("hello")->version == "3.0.0-much-longer");
        CHECK(again.find("zstd") == nullptr);
    }

    TEST_CASE("stale cache: same-size rewrite with the mtime restored is still caught") {
        IndexCacheTmpDir tmp;
        auto json = tmp.path / "index.json";
        save_index(fixture(), json);
        auto before = index_cache::stamp_source(json);
        auto mtime = fs::last_write_time(json);

        auto text = slurp(json);
        auto pos = text.find("2.12.3");
        REQUIRE(pos != std::string::npos);
        text.replace(pos, 6, "2.12.4");
        spit(json, text);
        fs::last_write_time(json, mtime);

        auto after = index_cache::stamp_source(json);
        REQUIRE(before);
        REQUIRE(after);
        CHECK(before->size == after->size);
        CHECK(before->mtime == after->mtime);
        CHECK(before->fingerprint != after->fingerprint);

        std::string why;
        CHECK(index_cache::open(json, &why) == nullptr);
        CHECK(why == "stale (index.json changed)");
        CHECK(load_index(json).find("hello")->version == "2.12.4");
    }

    TEST_CASE("corrupt cache header or truncation falls back to JSON") {
        IndexCacheTmpDir tmp;
        auto json = tmp.path / "index.json";
        save_index(fixture(), json);
        auto cache = index_cache::cache_path_for(json);
        auto good = slurp(cache);

        struct Case {
            const char* label;
            std::string bytes;
            const char* why;
        };
        std::string bad_magic = good;
        bad_magic[0] = 'X';
        std::string bad_version = good;
        bad_version[8] ^= 0x7f;
        std::string bad_header = good;
        bad_header[40] ^= 0x01; // entry count
        std::string bad_table = good;
        bad_table[90] ^= 0x01; // inside the entry table
        std::string truncated = good.substr(0, good.size() - 10);
        std::string tiny = good.substr(0, 12);

        for (const auto& c :
             {Case{"magic", bad_magic, "bad magic"},
              Case{"version", bad_version, "format version mismatch"},
              Case{"header", bad_header, "header hash mismatch"},
              Case{"table", bad_table, "table hash mismatch"},
              Case{"truncated", truncated, "file size mismatch (truncated?)"},
              Case{"tiny", tiny, "too small"}, Case{"empty", std::string(), "too small"}}) {
            CAPTURE(c.label);
            spit(cache, c.bytes);
            std::string why;
            CHECK(index_cache::open(json, &why) == nullptr);
            CHECK(why == c.why);
            auto idx = load_index(json);
            check_matches_json(idx, json);
            // ...and the cache was rebuilt in place.
            CHECK(index_cache::open(json) != nullptr);
        }
    }

    TEST_CASE("cache written by the previous format version is rebuilt") {
        // Pins the on-disk format version: bump this alongside kFormatVersion
        // whenever the record layout changes, so an older den's cache is
        // never decoded with the new layout.
        constexpr uint32_t kExpectedFormatVersion = 6;
        IndexCacheTmpDir tmp;
        auto json = tmp.path / "index.json";
        save_index(fixture(), json);
        auto cache = index_cache::cache_path_for(json);
        auto bytes = slurp(cache);
        REQUIRE(bytes.size() >= 12);

        uint32_t version = 0;
        std::memcpy(&version, bytes.data() + 8, sizeof version);
        REQUIRE(version == kExpectedFormatVersion);

        for (uint32_t older = 1; older < kExpectedFormatVersion; ++older) {
            CAPTURE(older);
            std::string stale = bytes;
            std::memcpy(stale.data() + 8, &older, sizeof older);
            spit(cache, stale);

            std::string why;
            CHECK(index_cache::open(json, &why) == nullptr);
            CHECK(why == "format version mismatch");

            auto idx = load_index(json);
            CHECK(idx.backing == nullptr); // parsed from JSON, not the old cache
            check_matches_json(idx, json);

            // ...and the cache was rewritten at the current version.
            auto rebuilt = slurp(cache);
            REQUIRE(rebuilt.size() >= 12);
            uint32_t now = 0;
            std::memcpy(&now, rebuilt.data() + 8, sizeof now);
            CHECK(now == kExpectedFormatVersion);
            auto again = load_index(json);
            CHECK(again.backing != nullptr);
            check_matches_json(again, json);
        }
    }

    TEST_CASE("corrupt record discovered at lookup time falls back to JSON") {
        IndexCacheTmpDir tmp;
        auto json = tmp.path / "index.json";
        save_index(fixture(), json);
        auto cache = index_cache::cache_path_for(json);
        auto bytes = slurp(cache);
        uint64_t blob_off;
        std::memcpy(&blob_off, bytes.data() + 64, sizeof blob_off);
        // Records are not covered by the table hash, so open() succeeds and
        // the damage is only seen when the record is decoded.
        bytes[blob_off + 6] ^= 0x55;
        spit(cache, bytes);

        auto idx = load_index(json);
        REQUIRE(idx.backing != nullptr);
        // First record by name is "cask/docker".
        const auto* d = idx.find("cask/docker");
        REQUIRE(d != nullptr);
        CHECK(d->version == "1.2.3");
        CHECK_FALSE(fs::exists(cache)); // bad cache removed
        check_matches_json(idx, json);
        CHECK(idx.find("no-such-package") == nullptr);
        CHECK_FALSE(idx.empty());
    }

    TEST_CASE("corrupt record during materialise_all falls back to JSON") {
        IndexCacheTmpDir tmp;
        auto json = tmp.path / "index.json";
        save_index(fixture(), json);
        auto cache = index_cache::cache_path_for(json);
        auto bytes = slurp(cache);
        bytes[bytes.size() - 3] ^= 0x55; // last record
        spit(cache, bytes);

        auto idx = load_index(json);
        REQUIRE(idx.backing != nullptr);
        idx.materialise_all();
        auto full = load_index_json(json);
        REQUIRE(idx.packages.size() == full.packages.size());
        for (const auto& [name, pkg] : full.packages) {
            check_same(pkg, idx.packages.at(name));
        }
    }

    TEST_CASE("DEN_NO_INDEX_CACHE disables the cache") {
        IndexCacheTmpDir tmp;
        auto json = tmp.path / "index.json";
        EnvGuard env("DEN_NO_INDEX_CACHE", "1");
        save_index(fixture(), json);
        CHECK_FALSE(fs::exists(index_cache::cache_path_for(json)));
        auto idx = load_index(json);
        CHECK(idx.backing == nullptr);
        CHECK(idx.find("hello")->version == "2.12.3");
        CHECK_FALSE(fs::exists(index_cache::cache_path_for(json)));
    }

    TEST_CASE("missing index.json yields an empty index and no cache") {
        IndexCacheTmpDir tmp;
        auto json = tmp.path / "index.json";
        auto idx = load_index(json);
        CHECK(idx.empty());
        CHECK_FALSE(fs::exists(index_cache::cache_path_for(json)));
    }

    TEST_CASE("record codec round-trips and rejects malformed input") {
        auto p = cask("cask/docker");
        p.keg_only = true;
        p.keg_only_reason = ":provided_by_macos";
        p.conflicts_with = {"docker-desktop"};
        auto enc = index_cache::encode_package(p);
        auto q = index_cache::decode_package(enc.data(), enc.size());
        check_same(p, q);
        CHECK(q.keg_only);
        CHECK(q.keg_only_reason == ":provided_by_macos");
        CHECK(q.conflicts_with == std::vector<std::string>{"docker-desktop"});

        for (size_t n = 0; n < enc.size(); ++n) {
            CHECK_THROWS(index_cache::decode_package(enc.data(), n));
        }
        auto extra = enc + "x";
        CHECK_THROWS(index_cache::decode_package(extra.data(), extra.size()));
    }
}

} // namespace test
} // namespace den
