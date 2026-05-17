// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// 🎯T69 verification harness — CAS Cellar layout (verifies T26).
//
// This suite verifies that the Cellar is keyed by SHA256 of keg contents and
// that name/version are metadata pointers into that store.  It requires T26 to
// be implemented: a `cas_cellar.h` header that exposes
//   - `cas_store_path(root, sha256)` → fs::path
//   - `cas_keg_sha256(keg_path)` → std::string
//   - `cas_store_keg(root, keg_path)` → std::string   (returns sha256 key)
//   - `cas_lookup(root, name, version)` → std::optional<std::string> (sha256)
//   - `cas_register(root, name, version, sha256)` → void
//
// If the header is absent the entire suite is skipped with a clear message.

#include <doctest.h>

// Probe for T26 implementation.
#if __has_include("store/cas_cellar.h")
#include "store/cas_cellar.h"
#define T26_IMPLEMENTED 1
#else
#define T26_IMPLEMENTED 0
#endif

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace den {
namespace test {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Shared fixture
// ---------------------------------------------------------------------------

struct CasTmpDir {
    fs::path path;

    CasTmpDir() {
        path = fs::temp_directory_path() /
               ("den_test_cas_" +
                std::to_string(
                    std::hash<std::string>{}(
                        std::to_string(reinterpret_cast<uintptr_t>(this))) ^
                    static_cast<size_t>(
                        std::chrono::steady_clock::now().time_since_epoch().count())));
        fs::create_directories(path);
    }

    ~CasTmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    CasTmpDir(const CasTmpDir&) = delete;
    CasTmpDir& operator=(const CasTmpDir&) = delete;

    // Create a fake keg with deterministic content.  Returns the keg path.
    fs::path make_keg(const std::string& name, const std::string& version,
                      const std::string& content = "binary content") {
        auto keg = path / "cellar" / name / version / "bin";
        fs::create_directories(keg);
        std::ofstream(keg / name) << content;
        return path / "cellar" / name / version;
    }
};

// ---------------------------------------------------------------------------
// Tests — skipped entirely when T26 is not yet implemented
// ---------------------------------------------------------------------------

TEST_SUITE("cas_cellar_layout [T26]") {

#if !T26_IMPLEMENTED
    TEST_CASE("SKIP — T26 (content-addressed Cellar) not yet implemented") {
        // This test exists as an acceptance gate for 🎯T26.  When T26 ships,
        // add store/cas_cellar.h and this block will be replaced by the real
        // tests below.
        WARN("T26 not implemented — skipping CAS Cellar layout tests");
    }
#else
    TEST_CASE("cas_store_path returns SHA256-keyed path under cas/ root") {
        CasTmpDir tmp;
        const std::string sha = "ba7816bf8f01cfea414140de5dae2223"
                                "b00361a396177a9cb410ff61f20015ad";
        auto expected = tmp.path / "cas" / sha.substr(0, 2) / sha;
        CHECK(cas_store_path(tmp.path / "cas", sha) == expected);
    }

    TEST_CASE("cas_keg_sha256 is deterministic for identical content") {
        CasTmpDir tmp;
        auto keg1 = tmp.make_keg("tree", "2.1.1", "same content");
        auto keg2 = tmp.make_keg("tree", "2.1.2", "same content");
        CHECK(cas_keg_sha256(keg1) == cas_keg_sha256(keg2));
    }

    TEST_CASE("cas_keg_sha256 differs for different content") {
        CasTmpDir tmp;
        auto keg1 = tmp.make_keg("tree", "2.1.1", "content A");
        auto keg2 = tmp.make_keg("tree", "2.2.0", "content B");
        CHECK(cas_keg_sha256(keg1) != cas_keg_sha256(keg2));
    }

    TEST_CASE("cas_store_keg stores keg under SHA256 key and returns key") {
        CasTmpDir tmp;
        auto keg = tmp.make_keg("curl", "8.5.0", "curl binary");
        auto store = tmp.path / "cas";

        std::string sha = cas_store_keg(store, keg);
        CHECK(!sha.empty());
        CHECK(sha.size() == 64); // hex SHA256

        // The store path must exist and be a directory.
        CHECK(fs::is_directory(cas_store_path(store, sha)));
    }

    TEST_CASE("storing the same keg twice is idempotent") {
        CasTmpDir tmp;
        auto keg = tmp.make_keg("jq", "1.7.1", "jq binary");
        auto store = tmp.path / "cas";

        auto sha1 = cas_store_keg(store, keg);
        auto sha2 = cas_store_keg(store, keg);
        CHECK(sha1 == sha2);
    }

    TEST_CASE("cas_register and cas_lookup round-trip") {
        CasTmpDir tmp;
        const std::string sha = "aabbccdd" + std::string(56, '0');
        cas_register(tmp.path, "git", "2.44.0", sha);

        auto result = cas_lookup(tmp.path, "git", "2.44.0");
        REQUIRE(result.has_value());
        CHECK(*result == sha);
    }

    TEST_CASE("cas_lookup returns nullopt for unknown name/version") {
        CasTmpDir tmp;
        CHECK(!cas_lookup(tmp.path, "nonexistent", "9.9.9").has_value());
    }

    TEST_CASE("multiple packages and versions are tracked independently") {
        CasTmpDir tmp;
        const std::string sha_a = "aaa" + std::string(61, '0');
        const std::string sha_b = "bbb" + std::string(61, '0');
        const std::string sha_c = "ccc" + std::string(61, '0');

        cas_register(tmp.path, "tree", "2.1.1", sha_a);
        cas_register(tmp.path, "tree", "2.2.0", sha_b);
        cas_register(tmp.path, "curl", "8.5.0", sha_c);

        CHECK(cas_lookup(tmp.path, "tree", "2.1.1") == std::optional<std::string>{sha_a});
        CHECK(cas_lookup(tmp.path, "tree", "2.2.0") == std::optional<std::string>{sha_b});
        CHECK(cas_lookup(tmp.path, "curl", "8.5.0") == std::optional<std::string>{sha_c});
    }
#endif // T26_IMPLEMENTED

} // TEST_SUITE

} // namespace test
} // namespace den
