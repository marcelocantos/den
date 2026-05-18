// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Idempotency tests for migrate_from_homebrew() — 🎯T71.
//
// Verifies that running migration twice on the same Cellar produces the same
// manifest state: packages present after the first run are not duplicated,
// overwritten, or otherwise disturbed on the second run.
//
// Also verifies that new kegs added to the Cellar between runs are picked up
// on the second run without disturbing already-migrated packages.

#include <doctest.h>

#include "core/config.h"
#include "migrate/migrate.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace den {
namespace test {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// RAII temporary directory.
// ---------------------------------------------------------------------------
struct IdmTmpDir {
    fs::path path;

    IdmTmpDir() {
        std::string tmpl = (fs::temp_directory_path() / "den_idm_XXXXXX").string();
        char* result = ::mkdtemp(tmpl.data());
        if (!result) {
            throw std::runtime_error("mkdtemp failed");
        }
        path = result;
    }

    ~IdmTmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    IdmTmpDir(const IdmTmpDir&) = delete;
    IdmTmpDir& operator=(const IdmTmpDir&) = delete;
};

// ---------------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------------

static void write_receipt(const fs::path& keg_path, bool on_request) {
    nlohmann::json j;
    j["installed_on_request"] = on_request;
    j["runtime_dependencies"] = nlohmann::json::array();
    std::ofstream out(keg_path / "INSTALL_RECEIPT.json");
    out << j.dump(2) << "\n";
}

// Add a keg to the Cellar with a minimal receipt.
static void add_keg(const fs::path& cellar, const std::string& name, const std::string& version,
                    bool on_request = true) {
    const fs::path keg = cellar / name / version;
    fs::create_directories(keg / "bin");
    std::ofstream(keg / "bin" / name) << "#!/bin/sh\n";
    write_receipt(keg, on_request);
}

// Read the packages map from ROOT.json.
static std::map<std::string, std::string> read_packages(const fs::path& den_home) {
    const fs::path p = den_home / "manifests" / "ROOT.json";
    if (!fs::is_regular_file(p)) {
        return {};
    }
    std::ifstream f(p);
    const auto j = nlohmann::json::parse(f);
    if (!j.contains("packages") || !j["packages"].is_object()) {
        return {};
    }
    std::map<std::string, std::string> result;
    for (const auto& [k, v] : j["packages"].items()) {
        result[k] = v.get<std::string>();
    }
    return result;
}

// Read the auto array from ROOT.json.
static std::vector<std::string> read_auto(const fs::path& den_home) {
    const fs::path p = den_home / "manifests" / "ROOT.json";
    if (!fs::is_regular_file(p)) {
        return {};
    }
    std::ifstream f(p);
    const auto j = nlohmann::json::parse(f);
    if (!j.contains("auto") || !j["auto"].is_array()) {
        return {};
    }
    std::vector<std::string> result;
    for (const auto& a : j["auto"]) {
        if (a.is_string()) {
            result.push_back(a.get<std::string>());
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Tests.
// ---------------------------------------------------------------------------

TEST_SUITE("migration::idempotent") {

    // -------------------------------------------------------------------------
    // 1. Second run on unchanged Cellar is a no-op (packages unchanged)
    // -------------------------------------------------------------------------
    TEST_CASE("second migration run on same cellar does not change packages") {
        IdmTmpDir root;
        const fs::path cellar = root.path / "Cellar";
        const fs::path den_home = root.path / "den_home";

        add_keg(cellar, "git", "2.44.0", true);
        add_keg(cellar, "curl", "8.7.1", false);

        Config cfg;
        cfg.homebrew_cellar = cellar;
        cfg.den_home = den_home;

        // First run.
        migrate_from_homebrew(cfg, {});
        const auto after_first = read_packages(den_home);

        // Second run — same Cellar, no additions.
        migrate_from_homebrew(cfg, {});
        const auto after_second = read_packages(den_home);

        // Package set must be identical.
        CHECK(after_first == after_second);
    }

    // -------------------------------------------------------------------------
    // 2. Second run does not alter already-tracked version
    // -------------------------------------------------------------------------
    TEST_CASE("second migration run does not downgrade already-tracked version") {
        IdmTmpDir root;
        const fs::path cellar = root.path / "Cellar";
        const fs::path den_home = root.path / "den_home";

        add_keg(cellar, "tree", "2.1.1", true);

        Config cfg;
        cfg.homebrew_cellar = cellar;
        cfg.den_home = den_home;

        migrate_from_homebrew(cfg, {});
        CHECK(read_packages(den_home)["tree"] == "2.1.1");

        // Add a new (older) version of tree to the Cellar after first run —
        // shouldn't happen in practice, but the migration must not clobber
        // what's already recorded.
        add_keg(cellar, "tree", "2.0.0", true);

        migrate_from_homebrew(cfg, {});
        // tree is already tracked; skipped on second run — version unchanged.
        CHECK(read_packages(den_home)["tree"] == "2.1.1");
    }

    // -------------------------------------------------------------------------
    // 3. New keg installed between runs is picked up on second run
    // -------------------------------------------------------------------------
    TEST_CASE("second migration run picks up new keg added since first run") {
        IdmTmpDir root;
        const fs::path cellar = root.path / "Cellar";
        const fs::path den_home = root.path / "den_home";

        add_keg(cellar, "git", "2.44.0", true);

        Config cfg;
        cfg.homebrew_cellar = cellar;
        cfg.den_home = den_home;

        // First run — only git.
        migrate_from_homebrew(cfg, {});
        CHECK(read_packages(den_home).count("git") == 1);
        CHECK(read_packages(den_home).count("wget") == 0);

        // Install wget "between runs".
        add_keg(cellar, "wget", "1.24.5", true);

        // Second run — wget should be picked up.
        migrate_from_homebrew(cfg, {});
        const auto pkgs = read_packages(den_home);
        CHECK(pkgs.count("git") == 1);  // still there
        CHECK(pkgs.count("wget") == 1); // newly added
        CHECK(pkgs.at("wget") == "1.24.5");
    }

    // -------------------------------------------------------------------------
    // 4. Auto list is not extended by re-migrating already-tracked auto dep
    // -------------------------------------------------------------------------
    TEST_CASE("second migration run does not duplicate entries in auto array") {
        IdmTmpDir root;
        const fs::path cellar = root.path / "Cellar";
        const fs::path den_home = root.path / "den_home";

        // curl is an auto dep.
        add_keg(cellar, "curl", "8.7.1", false);

        Config cfg;
        cfg.homebrew_cellar = cellar;
        cfg.den_home = den_home;

        migrate_from_homebrew(cfg, {});
        const auto auto_after_first = read_auto(den_home);

        migrate_from_homebrew(cfg, {});
        const auto auto_after_second = read_auto(den_home);

        // auto array should be identical — no duplicates.
        CHECK(auto_after_first == auto_after_second);

        // curl appears exactly once.
        int curl_count = 0;
        for (const auto& a : auto_after_second) {
            if (a == "curl")
                ++curl_count;
        }
        CHECK(curl_count == 1);
    }

    // -------------------------------------------------------------------------
    // 5. Empty Cellar — second run on empty Cellar is a no-op
    // -------------------------------------------------------------------------
    TEST_CASE("migration on empty cellar produces empty manifest then stays empty") {
        IdmTmpDir root;
        const fs::path cellar = root.path / "Cellar";
        const fs::path den_home = root.path / "den_home";
        fs::create_directories(cellar);

        Config cfg;
        cfg.homebrew_cellar = cellar;
        cfg.den_home = den_home;

        migrate_from_homebrew(cfg, {});
        // Nothing added — manifest not written (or written with empty packages).
        const auto pkgs_first = read_packages(den_home);
        CHECK(pkgs_first.empty());

        migrate_from_homebrew(cfg, {});
        const auto pkgs_second = read_packages(den_home);
        CHECK(pkgs_second.empty());
    }

} // TEST_SUITE migration::idempotent

} // namespace test
} // namespace den
