// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Non-destructive migration tests for 🎯T71.
//
// Asserts that migrate_from_homebrew() leaves the Homebrew Cellar layout
// byte-identical (or content-identical) before and after the migration.
// Den migration is metadata-only — it writes a ROOT.json manifest under
// den_home but never modifies, moves, or deletes files in the Cellar.

#include <doctest.h>

#include "migrate/migrate.h"
#include "core/config.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace den {
namespace test {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// RAII temporary directory.
// ---------------------------------------------------------------------------
struct NdTmpDir {
    fs::path path;

    NdTmpDir() {
        std::string tmpl = (fs::temp_directory_path() / "den_nd_XXXXXX").string();
        char* result = ::mkdtemp(tmpl.data());
        if (!result) {
            throw std::runtime_error("mkdtemp failed");
        }
        path = result;
    }

    ~NdTmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    NdTmpDir(const NdTmpDir&) = delete;
    NdTmpDir& operator=(const NdTmpDir&) = delete;
};

// ---------------------------------------------------------------------------
// Snapshot helpers: record every file under a directory tree as a map of
// relative path -> file content, so we can compare before/after.
// ---------------------------------------------------------------------------

using FileSnapshot = std::map<std::string, std::string>;

static std::string read_file_content(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// Recursively snapshot all regular files under root.
// Keys are paths relative to root (with "/" as separator).
static FileSnapshot snapshot(const fs::path& root) {
    FileSnapshot snap;
    if (!fs::exists(root)) {
        return snap;
    }
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file() && !entry.is_symlink()) {
            continue;
        }
        const auto rel = fs::relative(entry.path(), root).string();
        snap[rel] = read_file_content(entry.path());
    }
    return snap;
}

// ---------------------------------------------------------------------------
// Fixture.
// ---------------------------------------------------------------------------

static void write_receipt(const fs::path& keg_path, bool on_request) {
    nlohmann::json j;
    j["installed_on_request"] = on_request;
    j["runtime_dependencies"] = nlohmann::json::array();
    std::ofstream out(keg_path / "INSTALL_RECEIPT.json");
    out << j.dump(2) << "\n";
}

static fs::path build_nd_cellar(const fs::path& root) {
    const fs::path cellar = root / "Cellar";

    const fs::path git_keg = cellar / "git" / "2.44.0";
    fs::create_directories(git_keg / "bin");
    std::ofstream(git_keg / "bin" / "git") << "#!/bin/sh\necho git\n";
    write_receipt(git_keg, true);

    const fs::path curl_keg = cellar / "curl" / "8.7.1";
    fs::create_directories(curl_keg / "lib");
    std::ofstream(curl_keg / "lib" / "libcurl.dylib") << "DYLIB";
    write_receipt(curl_keg, false);

    return cellar;
}

// ---------------------------------------------------------------------------
// Tests.
// ---------------------------------------------------------------------------

TEST_SUITE("migration::non_destructive") {

    // -------------------------------------------------------------------------
    // 1. Cellar is byte-identical before and after migration
    // -------------------------------------------------------------------------
    TEST_CASE("migration does not modify any file in the Cellar") {
        NdTmpDir root;
        const auto cellar = build_nd_cellar(root.path);

        // Snapshot the Cellar before migration.
        const auto before = snapshot(cellar);
        CHECK_FALSE(before.empty()); // sanity: fixture was built

        Config cfg;
        cfg.homebrew_cellar = cellar;
        cfg.den_home = root.path / "den_home";

        migrate_from_homebrew(cfg, {});

        // Snapshot the Cellar after migration.
        const auto after = snapshot(cellar);

        CHECK(before == after);
    }

    // -------------------------------------------------------------------------
    // 2. Migration writes only inside den_home, not inside the Cellar
    // -------------------------------------------------------------------------
    TEST_CASE("migration creates files only under den_home, not under cellar root") {
        NdTmpDir root;
        const auto cellar = build_nd_cellar(root.path);
        const fs::path den_home = root.path / "den_home";

        Config cfg;
        cfg.homebrew_cellar = cellar;
        cfg.den_home = den_home;

        migrate_from_homebrew(cfg, {});

        // The Cellar directory should contain no extra files that weren't
        // there before (we check by comparing the snapshot with the known
        // set of files we created in build_nd_cellar).
        std::vector<std::string> cellar_paths;
        for (const auto& entry : fs::recursive_directory_iterator(cellar)) {
            if (entry.is_regular_file()) {
                cellar_paths.push_back(fs::relative(entry.path(), cellar).string());
            }
        }

        // Known files from the fixture.
        const std::vector<std::string> expected_cellar_files = {
            "git/2.44.0/bin/git",
            "git/2.44.0/INSTALL_RECEIPT.json",
            "curl/8.7.1/lib/libcurl.dylib",
            "curl/8.7.1/INSTALL_RECEIPT.json",
        };

        // Sort both for comparison.
        auto sorted_actual = cellar_paths;
        auto sorted_expected = expected_cellar_files;
        std::sort(sorted_actual.begin(), sorted_actual.end());
        std::sort(sorted_expected.begin(), sorted_expected.end());

        CHECK(sorted_actual == sorted_expected);

        // The manifest should exist under den_home only.
        CHECK(fs::is_regular_file(den_home / "manifests" / "ROOT.json"));
    }

    // -------------------------------------------------------------------------
    // 3. Migration does not delete any Cellar entry
    // -------------------------------------------------------------------------
    TEST_CASE("migration does not remove any keg directory from the Cellar") {
        NdTmpDir root;
        const auto cellar = build_nd_cellar(root.path);

        // Count directories before.
        int dir_count_before = 0;
        for (const auto& entry : fs::recursive_directory_iterator(cellar)) {
            if (entry.is_directory()) ++dir_count_before;
        }

        Config cfg;
        cfg.homebrew_cellar = cellar;
        cfg.den_home = root.path / "den_home";

        migrate_from_homebrew(cfg, {});

        // Count directories after.
        int dir_count_after = 0;
        for (const auto& entry : fs::recursive_directory_iterator(cellar)) {
            if (entry.is_directory()) ++dir_count_after;
        }

        CHECK(dir_count_before == dir_count_after);
    }

    // -------------------------------------------------------------------------
    // 4. Repeated migration is non-destructive to the Cellar
    // -------------------------------------------------------------------------
    TEST_CASE("repeated migration does not alter the Cellar") {
        NdTmpDir root;
        const auto cellar = build_nd_cellar(root.path);

        Config cfg;
        cfg.homebrew_cellar = cellar;
        cfg.den_home = root.path / "den_home";

        const auto before = snapshot(cellar);

        migrate_from_homebrew(cfg, {});
        migrate_from_homebrew(cfg, {});
        migrate_from_homebrew(cfg, {});

        const auto after = snapshot(cellar);

        CHECK(before == after);
    }

    // -------------------------------------------------------------------------
    // 5. Non-destructive when a pre-existing den manifest is present
    // -------------------------------------------------------------------------
    TEST_CASE("migration with pre-existing manifest does not clobber Cellar") {
        NdTmpDir root;
        const auto cellar = build_nd_cellar(root.path);
        const fs::path den_home = root.path / "den_home";

        // Pre-seed a manifest as if a previous migration had run.
        const fs::path manifest_dir = den_home / "manifests";
        fs::create_directories(manifest_dir);
        nlohmann::json existing;
        existing["packages"]["git"] = "2.44.0"; // git already tracked
        existing["auto"] = nlohmann::json::array();
        std::ofstream(manifest_dir / "ROOT.json") << existing.dump(2) << "\n";

        const auto before = snapshot(cellar);

        Config cfg;
        cfg.homebrew_cellar = cellar;
        cfg.den_home = den_home;

        migrate_from_homebrew(cfg, {});

        const auto after = snapshot(cellar);

        // Cellar unchanged.
        CHECK(before == after);

        // curl was not in the pre-seeded manifest and should now be added.
        std::ifstream f(manifest_dir / "ROOT.json");
        const auto manifest = nlohmann::json::parse(f);
        CHECK(manifest["packages"].contains("git"));
        CHECK(manifest["packages"].contains("curl"));
    }

} // TEST_SUITE migration::non_destructive

} // namespace test
} // namespace den
