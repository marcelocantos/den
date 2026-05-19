// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// 🎯T69 verification harness — popularity-tier-1 regression after CAS migration.
//
// This suite wraps the existing tier-1 smoke corpus (tests/smoke/tier1.json)
// and runs a structural regression pass after simulating a CAS migration over
// a synthetic Cellar built from the tier-1 package list.
//
// It does NOT actually install packages from the internet — that belongs to
// integration / end-to-end tests.  Instead it:
//
//   1. Builds a synthetic legacy Cellar: for each package in tier1.json, creates
//      a stub keg (name/version/bin/<name>) so the package "exists".
//   2. Runs migrate_to_cas over the synthetic Cellar.
//   3. Verifies that list_installed() still returns every package.
//   4. Verifies that materialise() succeeds (produces symlinks) for every
//      package in the root manifest.
//
// Skip conditions:
//   - T26 CAS migration not implemented (header absent).
//   - tier1.json not found at the expected path.
//
// The test never touches the network or /opt/homebrew.

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
#include <sstream>
#include <string>

// Tier-1 smoke definition is located relative to the source tree.
// CMakeLists.txt already defines DEN_CORPUS_DIR; we piggyback on it to
// locate the smoke directory one level up.
#ifndef DEN_TIER1_SMOKE_JSON
#define DEN_TIER1_SMOKE_JSON ""
#endif

namespace den {
namespace test {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Minimal JSON parser for the tier-1 list
// ---------------------------------------------------------------------------

namespace {

// Extract all "name" values from the tests array in the tier1.json file.
// We use a simple line-oriented scan rather than a full JSON parser to avoid
// a heavyweight dependency in a test file.
std::vector<std::string> read_tier1_names(const fs::path& json_path) {
    std::vector<std::string> names;
    if (!fs::is_regular_file(json_path)) {
        return names;
    }
    std::ifstream f(json_path);
    std::string line;
    bool in_tests = false;
    while (std::getline(f, line)) {
        if (line.find("\"tests\"") != std::string::npos) {
            in_tests = true;
        }
        if (!in_tests)
            continue;
        // Match lines of the form:   "name": "openssl@3",
        auto npos = line.find("\"name\"");
        if (npos == std::string::npos)
            continue;
        auto q1 = line.find('"', npos + 6);
        if (q1 == std::string::npos)
            continue;
        q1 = line.find('"', q1 + 1); // skip the colon and whitespace
        if (q1 == std::string::npos)
            continue;
        auto q2 = line.find('"', q1 + 1);
        if (q2 == std::string::npos)
            continue;
        names.push_back(line.substr(q1 + 1, q2 - q1 - 1));
    }
    return names;
}

} // namespace

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

struct Tier1TmpDir {
    fs::path path;

    Tier1TmpDir() {
        path =
            fs::temp_directory_path() /
            ("den_test_tier1_" +
             std::to_string(
                 std::hash<std::string>{}(std::to_string(reinterpret_cast<uintptr_t>(this))) ^
                 static_cast<size_t>(std::chrono::steady_clock::now().time_since_epoch().count())));
        fs::create_directories(path);
    }

    ~Tier1TmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    Tier1TmpDir(const Tier1TmpDir&) = delete;
    Tier1TmpDir& operator=(const Tier1TmpDir&) = delete;

    // Build a synthetic legacy Cellar entry for each name@version.
    void seed_legacy_cellar(const fs::path& store, const std::vector<std::string>& names,
                            const std::string& version = "1.0.0") {
        for (const auto& name : names) {
            auto bin = store / name / version / "bin";
            fs::create_directories(bin);
            // Write a tiny stub so the keg has distinct content per package.
            std::ofstream(bin / name) << "#!/bin/sh\necho " << name << "\n";
        }
    }
};

// ---------------------------------------------------------------------------
// Tier-1 regression tests
// ---------------------------------------------------------------------------

TEST_SUITE("tier1_smoke_regression [T69]") {

    // Locate tier1.json.  Two candidate paths (runtime checks, not preprocessor
    // string comparisons which are not valid in #if expressions):
    //   1. DEN_TIER1_SMOKE_JSON compile-time override (if non-empty string macro).
    //   2. Sibling of DEN_CORPUS_DIR: <corpus_root>/../smoke/tier1.json
    //      where DEN_CORPUS_DIR == <src>/tests/corpus/homebrew-core.
    static fs::path tier1_json() {
        fs::path p;
#if defined(DEN_TIER1_SMOKE_JSON)
        if (std::string_view(DEN_TIER1_SMOKE_JSON).size() > 0) {
            p = DEN_TIER1_SMOKE_JSON;
            if (fs::is_regular_file(p))
                return p;
        }
#endif
#if defined(DEN_CORPUS_DIR)
        if (std::string_view(DEN_CORPUS_DIR).size() > 0) {
            // DEN_CORPUS_DIR = .../tests/corpus/homebrew-core
            // tier1.json     = .../tests/smoke/tier1.json
            p = fs::path(DEN_CORPUS_DIR).parent_path().parent_path() / "smoke" / "tier1.json";
            if (fs::is_regular_file(p))
                return p;
        }
#endif
        return {};
    }

#if !T26_MIGRATION_IMPLEMENTED
    TEST_CASE("SKIP — T26 CAS migration not yet implemented") {
        WARN("T26 CAS migration not implemented — skipping tier-1 regression");
    }
#else
    TEST_CASE("list_installed returns all tier-1 packages after CAS migration") {
        Tier1TmpDir tmp;

        const auto json = tier1_json();
        if (json.empty()) {
            WARN("tier1.json not found — skipping regression");
            return;
        }
        const auto names = read_tier1_names(json);
        if (names.empty()) {
            WARN("tier1.json parsed zero packages — skipping regression");
            return;
        }

        auto store = tmp.path / "store";
        auto cas = tmp.path / "cas";
        const std::string ver = "1.0.0";

        tmp.seed_legacy_cellar(store, names, ver);
        migrate_to_cas(tmp.path, store, cas);

        auto installed = list_installed(store);
        std::set<std::string> found;
        for (const auto& pkg : installed) {
            found.insert(pkg.name);
        }

        for (const auto& name : names) {
            CHECK_MESSAGE(found.count(name) == 1, "package missing after migration: " + name);
        }
    }

    TEST_CASE("materialise succeeds for all tier-1 packages after CAS migration") {
        Tier1TmpDir tmp;

        const auto json = tier1_json();
        if (json.empty()) {
            WARN("tier1.json not found — skipping regression");
            return;
        }
        const auto names = read_tier1_names(json);
        if (names.empty()) {
            WARN("tier1.json parsed zero packages — skipping regression");
            return;
        }

        auto den_home = tmp.path / "den_home";
        auto store = tmp.path / "store";
        auto cas = tmp.path / "cas";
        const std::string ver = "1.0.0";

        tmp.seed_legacy_cellar(store, names, ver);

        // Build root manifest with all tier-1 packages.
        Manifest m;
        for (const auto& name : names) {
            m.packages["homebrew"][name] = ver;
        }
        write_manifest(den_home, "/", m);

        migrate_to_cas(tmp.path, store, cas);

        uint32_t total = materialise(den_home, store, "/");
        CHECK(total > 0);

        // Verify each package produced at least one symlink in env/bin/.
        auto eDir = env_dir(den_home, "/");
        for (const auto& name : names) {
            auto bin_link = eDir / "bin" / name;
            // Some packages may have a different binary name; check at minimum
            // that the opt/<name> link was created.
            auto opt_link = eDir / "opt" / name;
            bool linked = fs::is_symlink(bin_link) || fs::is_symlink(opt_link);
            CHECK_MESSAGE(linked, "no env link created for package after migration: " + name);
        }
    }

    TEST_CASE("CAS SHA256 keys are all unique across tier-1 corpus") {
        Tier1TmpDir tmp;

        const auto json = tier1_json();
        if (json.empty()) {
            WARN("tier1.json not found — skipping");
            return;
        }
        const auto names = read_tier1_names(json);
        if (names.empty())
            return;

        auto store = tmp.path / "store";
        auto cas = tmp.path / "cas";
        const std::string ver = "1.0.0";

        tmp.seed_legacy_cellar(store, names, ver);
        migrate_to_cas(tmp.path, store, cas);

        std::set<std::string> shas;
        for (const auto& name : names) {
            auto sha = cas_lookup(tmp.path, name, ver);
            if (!sha.has_value())
                continue;
            // All stubs have unique content (contain the package name), so
            // all SHAs should be distinct.
            CHECK_MESSAGE(shas.find(*sha) == shas.end(),
                          "SHA256 collision between packages (unexpected): " + name);
            shas.insert(*sha);
        }
        CHECK(!shas.empty());
    }
#endif // T26_MIGRATION_IMPLEMENTED

} // TEST_SUITE

} // namespace test
} // namespace den
