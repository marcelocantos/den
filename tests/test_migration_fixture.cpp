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
    // 7. Caskroom scanning (was SKIPPED — now implemented for T37/T71)
    // -------------------------------------------------------------------------
    // Build a Caskroom mirroring Homebrew's real layout: each cask token dir
    // holds version subdirs that are siblings of a `.metadata` bookkeeping dir.
    static fs::path build_fixture_caskroom(const fs::path& root) {
        const fs::path caskroom = root / "Caskroom";

        // visual-studio-code 1.88.0
        const fs::path vscode = caskroom / "visual-studio-code";
        fs::create_directories(vscode / "1.88.0" / "Visual Studio Code.app");
        fs::create_directories(vscode / ".metadata" / "1.88.0");
        std::ofstream(vscode / ".metadata" / "INSTALL_RECEIPT.json") << "{}\n";

        // audacity 3.7.8
        const fs::path audacity = caskroom / "audacity";
        fs::create_directories(audacity / "3.7.8");
        fs::create_directories(audacity / ".metadata");

        return caskroom;
    }

    TEST_CASE("scan_homebrew_caskroom finds casks and ignores .metadata") {
        MigTmpDir root;
        const auto caskroom = build_fixture_caskroom(root.path);

        const auto casks = scan_homebrew_caskroom(caskroom);
        REQUIRE(casks.size() == 2);

        std::map<std::string, std::string> found;
        for (const auto& c : casks) {
            found[c.name] = c.version;
        }
        CHECK(found["visual-studio-code"] == "1.88.0");
        CHECK(found["audacity"] == "3.7.8");

        // No version named ".metadata" leaks through.
        for (const auto& c : casks) {
            CHECK(c.version != ".metadata");
        }
    }

    TEST_CASE("migrate_from_homebrew records casks in manifest") {
        MigTmpDir root;
        const auto cellar = build_fixture_cellar(root.path);
        const auto caskroom = build_fixture_caskroom(root.path);

        Config cfg;
        cfg.homebrew_cellar = cellar;
        cfg.homebrew_caskroom = caskroom;
        cfg.den_home = root.path / "den_home";

        const auto summary = migrate_from_homebrew(cfg, {});
        CHECK(summary.casks == 2);

        const auto manifest = read_manifest(cfg.den_home);
        REQUIRE(manifest.contains("casks"));
        const auto& casks = manifest["casks"];
        CHECK(casks["visual-studio-code"].get<std::string>() == "1.88.0");
        CHECK(casks["audacity"].get<std::string>() == "3.7.8");
    }

    // -------------------------------------------------------------------------
    // 8. Tap import (was SKIPPED — now implemented for T37)
    // -------------------------------------------------------------------------
    static fs::path build_fixture_taps(const fs::path& root) {
        const fs::path taps = root / "Library" / "Taps";
        // Homebrew lays taps out as <user>/homebrew-<repo>.
        fs::create_directories(taps / "homebrew" / "homebrew-cask");
        fs::create_directories(taps / "homebrew" / "homebrew-core");
        fs::create_directories(taps / "marcelocantos" / "homebrew-tap");
        // A stray non-tap directory must be ignored.
        fs::create_directories(taps / "homebrew" / "not-a-tap");
        return taps;
    }

    TEST_CASE("scan_homebrew_taps canonicalises tap names") {
        MigTmpDir root;
        const auto taps = build_fixture_taps(root.path);

        const auto found = scan_homebrew_taps(taps);
        REQUIRE(found.size() == 3);

        std::vector<std::string> names;
        for (const auto& t : found) {
            names.push_back(t.name);
        }
        // Sorted by name.
        CHECK(names[0] == "homebrew/cask");
        CHECK(names[1] == "homebrew/core");
        CHECK(names[2] == "marcelocantos/tap");
    }

    TEST_CASE("migrate_from_homebrew records taps in manifest") {
        MigTmpDir root;
        const auto cellar = build_fixture_cellar(root.path);
        const auto taps = build_fixture_taps(root.path);

        Config cfg;
        cfg.homebrew_cellar = cellar;
        cfg.homebrew_taps = taps;
        cfg.den_home = root.path / "den_home";

        const auto summary = migrate_from_homebrew(cfg, {});
        CHECK(summary.taps == 3);

        const auto manifest = read_manifest(cfg.den_home);
        REQUIRE(manifest.contains("taps"));
        std::vector<std::string> tap_names;
        for (const auto& t : manifest["taps"]) {
            tap_names.push_back(t.get<std::string>());
        }
        std::sort(tap_names.begin(), tap_names.end());
        REQUIRE(tap_names.size() == 3);
        CHECK(tap_names[0] == "homebrew/cask");
        CHECK(tap_names[1] == "homebrew/core");
        CHECK(tap_names[2] == "marcelocantos/tap");
    }

    // -------------------------------------------------------------------------
    // 9. brew services migration (was SKIPPED — now implemented for T37)
    // -------------------------------------------------------------------------
    TEST_CASE("parse_brew_services_json extracts running services") {
        const std::string json = R"([
            {"name":"postgresql@16","status":"started","user":"alice",
             "file":"/Users/alice/Library/LaunchAgents/homebrew.mxcl.postgresql@16.plist"},
            {"name":"redis","status":"started","user":"alice",
             "file":"/Users/alice/Library/LaunchAgents/homebrew.mxcl.redis.plist"},
            {"name":"emacs","status":"none","user":null,"file":"/opt/homebrew/opt/emacs/x.plist"}
        ])";

        const auto svcs = parse_brew_services_json(json);
        REQUIRE(svcs.size() == 3);

        std::map<std::string, bool> running;
        for (const auto& s : svcs) {
            running[s.name] = s.running;
        }
        CHECK(running["postgresql@16"] == true);
        CHECK(running["redis"] == true);
        CHECK(running["emacs"] == false);
    }

    TEST_CASE("parse_brew_services_json tolerates malformed input") {
        CHECK(parse_brew_services_json("{ not json").empty());
        CHECK(parse_brew_services_json("{}").empty()); // object, not array
        CHECK(parse_brew_services_json("[]").empty());
    }

    TEST_CASE("scan_launchagent_plists finds homebrew.mxcl plists") {
        MigTmpDir root;
        const fs::path agents = root.path / "LaunchAgents";
        fs::create_directories(agents);
        std::ofstream(agents / "homebrew.mxcl.ollama.plist") << "<plist/>\n";
        std::ofstream(agents / "homebrew.mxcl.sawmill.plist") << "<plist/>\n";
        // Non-homebrew plist must be ignored.
        std::ofstream(agents / "com.apple.something.plist") << "<plist/>\n";

        const auto svcs = scan_launchagent_plists(agents);
        REQUIRE(svcs.size() == 2);
        std::vector<std::string> names;
        for (const auto& s : svcs) {
            names.push_back(s.name);
        }
        CHECK(names[0] == "ollama");
        CHECK(names[1] == "sawmill");
    }

    TEST_CASE("migrate_from_homebrew records services from injected json") {
        MigTmpDir root;
        const auto cellar = build_fixture_cellar(root.path);

        Config cfg;
        cfg.homebrew_cellar = cellar;
        cfg.den_home = root.path / "den_home";

        MigrateOptions opts;
        opts.brew_services_json = R"([
            {"name":"redis","status":"started","user":"bob",
             "file":"/Users/bob/Library/LaunchAgents/homebrew.mxcl.redis.plist"}
        ])";

        const auto summary = migrate_from_homebrew(cfg, {}, opts);
        CHECK(summary.services == 1);

        const auto manifest = read_manifest(cfg.den_home);
        REQUIRE(manifest.contains("services"));
        REQUIRE(manifest["services"].size() == 1);
        const auto& svc = manifest["services"][0];
        CHECK(svc["name"].get<std::string>() == "redis");
        CHECK(svc["running"].get<bool>() == true);
    }

    // -------------------------------------------------------------------------
    // 10. Dry-run writes nothing
    // -------------------------------------------------------------------------
    TEST_CASE("migrate_from_homebrew --dry-run writes no manifest") {
        MigTmpDir root;
        const auto cellar = build_fixture_cellar(root.path);
        const auto caskroom = build_fixture_caskroom(root.path);

        Config cfg;
        cfg.homebrew_cellar = cellar;
        cfg.homebrew_caskroom = caskroom;
        cfg.den_home = root.path / "den_home";

        MigrateOptions opts;
        opts.dry_run = true;

        const auto summary = migrate_from_homebrew(cfg, {}, opts);
        // Summary still reflects what *would* be migrated.
        CHECK(summary.formulae == 3);
        CHECK(summary.casks == 2);

        // But nothing is written.
        CHECK_FALSE(fs::exists(cfg.den_home / "manifests" / "ROOT.json"));
    }

    // -------------------------------------------------------------------------
    // 11. Post-migration health check passes against a consistent fixture
    // -------------------------------------------------------------------------
    TEST_CASE("check_migration_health passes for a freshly migrated fixture") {
        MigTmpDir root;
        const auto cellar = build_fixture_cellar(root.path);
        const auto caskroom = build_fixture_caskroom(root.path);

        Config cfg;
        cfg.homebrew_cellar = cellar;
        cfg.homebrew_caskroom = caskroom;
        cfg.den_home = root.path / "den_home";

        migrate_from_homebrew(cfg, {});
        const auto report = check_migration_health(cfg, /*print=*/false);

        CHECK(report.healthy());
        CHECK(report.formulae_ok == 3);
        CHECK(report.casks_ok == 2);
    }

    TEST_CASE("check_migration_health flags a missing keg") {
        MigTmpDir root;
        const auto cellar = build_fixture_cellar(root.path);

        Config cfg;
        cfg.homebrew_cellar = cellar;
        cfg.den_home = root.path / "den_home";

        migrate_from_homebrew(cfg, {});

        // Remove a keg so the manifest now references a non-existent path.
        fs::remove_all(cellar / "git");

        const auto report = check_migration_health(cfg, /*print=*/false);
        CHECK_FALSE(report.healthy());
    }

} // TEST_SUITE migration::fixture

} // namespace test
} // namespace den
