// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Fixture-based migration tests for 🎯T71.
//
// Exercises migrate_from_homebrew() against a synthetic Homebrew layout built
// in a temporary directory.  The fixture mimics:
//
//   <brew_root>/Cellar/
//     git/2.44.0/       INSTALL_RECEIPT.json (installed_on_request=true)
//     curl/8.7.1/       INSTALL_RECEIPT.json (installed_on_request=false)
//     python@3.12/3.12.3/  (no INSTALL_RECEIPT — treated as on_request=true)
//
// SKIPPED areas (not yet implemented in src/migrate/):
//   - Caskroom scanning
//   - Library/Taps import
//   - brew services migration
//
// These are noted inline as SKIP with upstream gap references (T37).

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
struct MigTmpDir {
    fs::path path;

    MigTmpDir() {
        std::string tmpl = (fs::temp_directory_path() / "den_mig_XXXXXX").string();
        char* result = ::mkdtemp(tmpl.data());
        if (!result) {
            throw std::runtime_error("mkdtemp failed");
        }
        path = result;
    }

    ~MigTmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    MigTmpDir(const MigTmpDir&) = delete;
    MigTmpDir& operator=(const MigTmpDir&) = delete;
};

// ---------------------------------------------------------------------------
// Fixture helpers.
// ---------------------------------------------------------------------------

// Write an INSTALL_RECEIPT.json inside <keg_path>/ with the given fields.
static void write_receipt(const fs::path& keg_path, bool installed_on_request,
                          const std::vector<std::string>& runtime_deps = {}) {
    nlohmann::json j;
    j["installed_on_request"] = installed_on_request;
    nlohmann::json deps = nlohmann::json::array();
    for (const auto& dep : runtime_deps) {
        nlohmann::json d;
        d["full_name"] = dep;
        deps.push_back(d);
    }
    j["runtime_dependencies"] = deps;

    std::ofstream out(keg_path / "INSTALL_RECEIPT.json");
    out << j.dump(2) << "\n";
}

// Build the standard fixture Cellar under <root>/Cellar/.
// Returns the path to the Cellar directory.
static fs::path build_fixture_cellar(const fs::path& root) {
    const fs::path cellar = root / "Cellar";

    // git — explicitly requested
    const fs::path git_keg = cellar / "git" / "2.44.0";
    fs::create_directories(git_keg / "bin");
    std::ofstream(git_keg / "bin" / "git") << "#!/bin/sh\n";
    write_receipt(git_keg, /*installed_on_request=*/true);

    // curl — installed as a dependency (auto)
    const fs::path curl_keg = cellar / "curl" / "8.7.1";
    fs::create_directories(curl_keg / "lib");
    write_receipt(curl_keg, /*installed_on_request=*/false, {"zlib", "openssl@3"});

    // python@3.12 — no receipt, defaults to on_request=true
    const fs::path py_keg = cellar / "python@3.12" / "3.12.3";
    fs::create_directories(py_keg / "bin");
    std::ofstream(py_keg / "bin" / "python3") << "#!/bin/sh\n";
    // Intentionally no INSTALL_RECEIPT.json.

    return cellar;
}

// Read ROOT.json manifest from <den_home>/manifests/ROOT.json.
static nlohmann::json read_manifest(const fs::path& den_home) {
    const fs::path p = den_home / "manifests" / "ROOT.json";
    if (!fs::is_regular_file(p)) {
        return nlohmann::json{};
    }
    std::ifstream f(p);
    return nlohmann::json::parse(f);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_SUITE("migration::fixture") {

    // -------------------------------------------------------------------------
    // 1. scan_homebrew_cellar — low-level unit test against fixture
    // -------------------------------------------------------------------------
    TEST_CASE("scan_homebrew_cellar finds expected kegs") {
        MigTmpDir root;
        const auto cellar = build_fixture_cellar(root.path);

        const auto kegs = scan_homebrew_cellar(cellar);

        // Should find exactly one keg per formula (three formulae).
        CHECK(kegs.size() == 3);

        // Build a name->version map for easy lookup.
        std::map<std::string, std::string> found;
        for (const auto& k : kegs) {
            found[k.name] = k.version;
        }

        CHECK(found.count("git") == 1);
        CHECK(found["git"] == "2.44.0");

        CHECK(found.count("curl") == 1);
        CHECK(found["curl"] == "8.7.1");

        CHECK(found.count("python@3.12") == 1);
        CHECK(found["python@3.12"] == "3.12.3");
    }

    TEST_CASE("scan_homebrew_cellar returns empty for missing cellar") {
        CHECK(scan_homebrew_cellar("/nonexistent/path/to/Cellar").empty());
    }

    // -------------------------------------------------------------------------
    // 2. read_tab — verify receipt parsing
    // -------------------------------------------------------------------------
    TEST_CASE("read_tab: on-request formula") {
        MigTmpDir tmp;
        fs::create_directories(tmp.path);
        write_receipt(tmp.path, true, {});

        const auto tab = read_tab(tmp.path);
        REQUIRE(tab.has_value());
        CHECK(tab->installed_on_request == true);
        CHECK(tab->runtime_deps.empty());
    }

    TEST_CASE("read_tab: auto dependency with runtime_deps") {
        MigTmpDir tmp;
        fs::create_directories(tmp.path);
        write_receipt(tmp.path, false, {"zlib", "openssl@3"});

        const auto tab = read_tab(tmp.path);
        REQUIRE(tab.has_value());
        CHECK(tab->installed_on_request == false);
        CHECK(tab->runtime_deps.size() == 2);
        CHECK(tab->runtime_deps[0] == "zlib");
        CHECK(tab->runtime_deps[1] == "openssl@3");
    }

    TEST_CASE("read_tab: missing receipt returns nullopt") {
        MigTmpDir tmp;
        CHECK_FALSE(read_tab(tmp.path).has_value());
    }

    TEST_CASE("read_tab: malformed JSON returns nullopt") {
        MigTmpDir tmp;
        std::ofstream(tmp.path / "INSTALL_RECEIPT.json") << "{ not valid json";
        CHECK_FALSE(read_tab(tmp.path).has_value());
    }

    // -------------------------------------------------------------------------
    // 3. migrate_from_homebrew — all formulae picked up
    // -------------------------------------------------------------------------
    TEST_CASE("migrate_from_homebrew picks up all formulae from fixture Cellar") {
        MigTmpDir root;
        const auto cellar = build_fixture_cellar(root.path);

        Config cfg;
        cfg.homebrew_cellar = cellar;
        cfg.den_home = root.path / "den_home";

        migrate_from_homebrew(cfg, {});

        const auto manifest = read_manifest(cfg.den_home);
        REQUIRE(manifest.contains("packages"));
        const auto& pkgs = manifest["packages"];

        CHECK(pkgs.contains("git"));
        CHECK(pkgs["git"].get<std::string>() == "2.44.0");

        CHECK(pkgs.contains("curl"));
        CHECK(pkgs["curl"].get<std::string>() == "8.7.1");

        CHECK(pkgs.contains("python@3.12"));
        CHECK(pkgs["python@3.12"].get<std::string>() == "3.12.3");
    }

    // -------------------------------------------------------------------------
    // 4. Auto vs on-request classification in manifest
    // -------------------------------------------------------------------------
    TEST_CASE("migrate_from_homebrew marks auto-deps in manifest auto array") {
        MigTmpDir root;
        const auto cellar = build_fixture_cellar(root.path);

        Config cfg;
        cfg.homebrew_cellar = cellar;
        cfg.den_home = root.path / "den_home";

        migrate_from_homebrew(cfg, {});

        const auto manifest = read_manifest(cfg.den_home);
        REQUIRE(manifest.contains("auto"));
        const auto& auto_arr = manifest["auto"];

        // curl was installed_on_request=false, so it appears in the auto array.
        bool curl_auto = false;
        for (const auto& a : auto_arr) {
            if (a.is_string() && a.get<std::string>() == "curl") {
                curl_auto = true;
            }
        }
        CHECK(curl_auto);

        // git was installed_on_request=true, should NOT be in auto array.
        bool git_auto = false;
        for (const auto& a : auto_arr) {
            if (a.is_string() && a.get<std::string>() == "git") {
                git_auto = true;
            }
        }
        CHECK_FALSE(git_auto);
    }

    // -------------------------------------------------------------------------
    // 5. Selective migration by name
    // -------------------------------------------------------------------------
    TEST_CASE("migrate_from_homebrew respects explicit name filter") {
        MigTmpDir root;
        const auto cellar = build_fixture_cellar(root.path);

        Config cfg;
        cfg.homebrew_cellar = cellar;
        cfg.den_home = root.path / "den_home";

        // Migrate only git.
        migrate_from_homebrew(cfg, {"git"});

        const auto manifest = read_manifest(cfg.den_home);
        REQUIRE(manifest.contains("packages"));
        const auto& pkgs = manifest["packages"];

        CHECK(pkgs.contains("git"));
        CHECK_FALSE(pkgs.contains("curl"));
        CHECK_FALSE(pkgs.contains("python@3.12"));
    }

    // -------------------------------------------------------------------------
    // 6. Multi-version: only the newest version is recorded
    // -------------------------------------------------------------------------
    TEST_CASE("migrate_from_homebrew records newest version when multiple installed") {
        MigTmpDir root;
        const fs::path cellar = root.path / "Cellar";

        // Two versions of tree installed.
        fs::create_directories(cellar / "tree" / "2.1.1");
        write_receipt(cellar / "tree" / "2.1.1", true);
        fs::create_directories(cellar / "tree" / "2.2.0");
        write_receipt(cellar / "tree" / "2.2.0", true);

        Config cfg;
        cfg.homebrew_cellar = cellar;
        cfg.den_home = root.path / "den_home";

        migrate_from_homebrew(cfg, {});

        const auto manifest = read_manifest(cfg.den_home);
        REQUIRE(manifest.contains("packages"));
        // Lexicographically "2.2.0" > "2.1.1" — newest is recorded.
        CHECK(manifest["packages"]["tree"].get<std::string>() == "2.2.0");
    }

    // -------------------------------------------------------------------------
    // SKIPPED: Caskroom migration
    // -------------------------------------------------------------------------
    // SKIP — src/migrate/ does not scan Caskroom/.
    // Upstream gap: T37 / T71 acceptance requires cask support.
    // When implemented, fixture should include:
    //   <brew_root>/Caskroom/visual-studio-code/1.88.0/.metadata/
    // and the manifest should gain a "casks" key.

    // -------------------------------------------------------------------------
    // SKIPPED: Tap import
    // -------------------------------------------------------------------------
    // SKIP — src/migrate/ does not read Library/Taps/.
    // Upstream gap: T37 requires taps to be re-added as den tap sources.
    // When implemented, fixture should include:
    //   <brew_root>/Library/Taps/homebrew/homebrew-cask/
    // and a "taps" key should appear in the manifest or a separate tap index.

    // -------------------------------------------------------------------------
    // SKIPPED: brew services migration
    // -------------------------------------------------------------------------
    // SKIP — src/migrate/ does not inspect brew services.
    // Upstream gap: T37 acceptance requires migrated services to remain running.
    // When implemented, the fixture would need a fake `brew services list` output
    // and the migration should record service state in the manifest or launchd.

} // TEST_SUITE migration::fixture

} // namespace test
} // namespace den
