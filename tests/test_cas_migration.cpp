// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// 🎯T69 verification harness — CAS migration (verifies T26).
//
// Fixtures a legacy Cellar (name/version/ layout) in a temp DEN_HOME, runs
// the CAS migration function, and asserts:
//   1. Each keg's content is present in the CAS store under its SHA256 key.
//   2. `list_installed` still returns all original name/version pairs (the
//      metadata layer is preserved).
//   3. Materialising an env that references a migrated package still produces
//      working symlinks (den use semantics are unchanged).
//
// The migration entry-point is expected in `store/cas_migration.h`:
//   - `migrate_to_cas(root, store, cas_store)` → void
//
// If either header is absent the suite is skipped.

#include <doctest.h>

#if __has_include("store/cas_cellar.h") && __has_include("store/cas_migration.h")
#include "store/cas_cellar.h"
#include "store/cas_migration.h"
#define T26_MIGRATION_IMPLEMENTED 1
#else
#define T26_MIGRATION_IMPLEMENTED 0
#endif

#include "env/environment.h"
#include "env/manifest.h"
#include "store/store.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace den {
namespace test {

namespace fs = std::filesystem;

struct MigTmpDir {
    fs::path path;

    MigTmpDir() {
        path =
            fs::temp_directory_path() /
            ("den_test_mig_" +
             std::to_string(
                 std::hash<std::string>{}(std::to_string(reinterpret_cast<uintptr_t>(this))) ^
                 static_cast<size_t>(std::chrono::steady_clock::now().time_since_epoch().count())));
        fs::create_directories(path);
    }

    ~MigTmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    MigTmpDir(const MigTmpDir&) = delete;
    MigTmpDir& operator=(const MigTmpDir&) = delete;

    // Populate a legacy-layout store: store/<name>/<version>/bin/<name>
    void make_legacy_keg(const fs::path& store, const std::string& name, const std::string& version,
                         const std::string& content = "fake binary") {
        auto keg_bin = store / name / version / "bin";
        fs::create_directories(keg_bin);
        std::ofstream(keg_bin / name) << content;
    }
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_SUITE("cas_migration [T26]") {

#if !T26_MIGRATION_IMPLEMENTED
    TEST_CASE("SKIP — T26 CAS migration not yet implemented") {
        WARN("T26 CAS migration not implemented — skipping migration tests");
    }
#else
    TEST_CASE("migrated kegs appear in CAS store under SHA256 key") {
        MigTmpDir tmp;
        auto store = tmp.path / "store";
        auto cas = tmp.path / "cas";

        tmp.make_legacy_keg(store, "tree", "2.1.1", "tree-binary-v2.1.1");
        tmp.make_legacy_keg(store, "curl", "8.5.0", "curl-binary-v8.5.0");

        migrate_to_cas(tmp.path, store, cas);

        // Each keg's SHA256 must be registered and the CAS path must exist.
        auto sha_tree = cas_lookup(tmp.path, "tree", "2.1.1");
        REQUIRE(sha_tree.has_value());
        CHECK(fs::is_directory(cas_store_path(cas, *sha_tree)));

        auto sha_curl = cas_lookup(tmp.path, "curl", "8.5.0");
        REQUIRE(sha_curl.has_value());
        CHECK(fs::is_directory(cas_store_path(cas, *sha_curl)));
    }

    TEST_CASE("list_installed returns all original packages after migration") {
        MigTmpDir tmp;
        auto store = tmp.path / "store";
        auto cas = tmp.path / "cas";

        tmp.make_legacy_keg(store, "tree", "2.1.1");
        tmp.make_legacy_keg(store, "tree", "2.2.0");
        tmp.make_legacy_keg(store, "curl", "8.5.0");

        migrate_to_cas(tmp.path, store, cas);

        auto pkgs = list_installed(store);
        std::set<std::string> found;
        for (const auto& p : pkgs) {
            found.insert(p.name + "@" + p.version);
        }

        CHECK(found.count("tree@2.1.1") == 1);
        CHECK(found.count("tree@2.2.0") == 1);
        CHECK(found.count("curl@8.5.0") == 1);
    }

    TEST_CASE("den use symlinks resolve correctly after migration") {
        MigTmpDir tmp;
        auto den_home = tmp.path / "den_home";
        auto store = tmp.path / "store";
        auto cas = tmp.path / "cas";

        tmp.make_legacy_keg(store, "tree", "2.1.1", "tree-binary");

        Manifest m;
        m.packages["homebrew"]["tree"] = "2.1.1";
        write_manifest(den_home, "/", m);

        migrate_to_cas(tmp.path, store, cas);

        // Materialise must still work — environment layer is unchanged.
        uint32_t count = materialise(den_home, store, "/");
        CHECK(count > 0);

        auto eDir = env_dir(den_home, "/");
        CHECK(fs::is_symlink(eDir / "bin" / "tree"));
    }

    TEST_CASE("migration is idempotent — running twice produces no error") {
        MigTmpDir tmp;
        auto store = tmp.path / "store";
        auto cas = tmp.path / "cas";

        tmp.make_legacy_keg(store, "git", "2.44.0", "git-binary");

        migrate_to_cas(tmp.path, store, cas);
        // Second run must not throw or produce duplicate entries.
        CHECK_NOTHROW(migrate_to_cas(tmp.path, store, cas));

        auto sha = cas_lookup(tmp.path, "git", "2.44.0");
        REQUIRE(sha.has_value());
    }

    TEST_CASE("two kegs with identical content share one CAS object") {
        MigTmpDir tmp;
        auto store = tmp.path / "store";
        auto cas = tmp.path / "cas";

        // Same content for both versions — should deduplicate in CAS.
        tmp.make_legacy_keg(store, "zlib", "1.3.0", "identical content");
        tmp.make_legacy_keg(store, "zlib", "1.3.1", "identical content");

        migrate_to_cas(tmp.path, store, cas);

        auto sha0 = cas_lookup(tmp.path, "zlib", "1.3.0");
        auto sha1 = cas_lookup(tmp.path, "zlib", "1.3.1");
        REQUIRE(sha0.has_value());
        REQUIRE(sha1.has_value());
        CHECK(*sha0 == *sha1); // same object in CAS
    }
#endif // T26_MIGRATION_IMPLEMENTED

} // TEST_SUITE

} // namespace test
} // namespace den
