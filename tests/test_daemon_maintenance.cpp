// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// 🎯T13 — daemon maintenance cycle: index refresh, outdated detection,
// pre-download and the in-use-aware upgrade pass, driven through injected
// hooks so no network or real Cellar is touched.

#include <doctest.h>

#include "daemon/daemon.h"
#include "daemon/upgrade_pass.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace den {
namespace test {

namespace {

namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() /
               ("den-t13-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

Package formula(const std::string& name, const std::string& version) {
    Package p;
    p.name = name;
    p.version = version;
    return p;
}

PackageIndex sample_index() {
    PackageIndex idx;
    idx.packages["jq"] = formula("jq", "1.8.2");
    idx.packages["tree"] = formula("tree", "2.3.2");
    idx.packages["pv"] = formula("pv", "1.12.0");
    Package app = formula("firefox", "131.0");
    app.artifact_type = ArtifactType::App;
    idx.packages["firefox"] = app;
    Package dead = formula("oldtool", "2.0");
    dead.disabled = true;
    idx.packages["oldtool"] = dead;
    return idx;
}

std::map<std::string, std::string> sample_installed() {
    return {{"jq", "1.7.1"},      {"tree", "2.3.2"},  {"pv", "1.11.0"},
            {"firefox", "130.0"}, {"oldtool", "1.0"}, {"gone", "0.1"}};
}

} // namespace

TEST_SUITE("daemon_maintenance") {

    TEST_CASE("compute_pending lists only outdated den-managed formulae") {
        auto pending = compute_pending(sample_installed(), sample_index());
        REQUIRE(pending.size() == 2);
        CHECK(pending[0].name == "jq");
        CHECK(pending[0].installed == "1.7.1");
        CHECK(pending[0].available == "1.8.2");
        CHECK(pending[1].name == "pv");
        // up to date (tree), cask (firefox), disabled (oldtool), not in index (gone)
        for (const auto& p : pending) {
            CHECK(p.name != "tree");
            CHECK(p.name != "firefox");
            CHECK(p.name != "oldtool");
            CHECK(p.name != "gone");
        }
    }

    TEST_CASE(
        "maintenance cycle refreshes, detects and pre-downloads without upgrading by default") {
        TempDir t;
        DaemonState state;
        state.deferred.push_back({"tree", "2.3.1", "2.3.2", "binary in use"}); // stale
        int refreshes = 0;
        std::vector<std::string> fetched, applied;
        MaintenanceHooks h;
        h.refresh_index = [&] {
            ++refreshes;
            return sample_index();
        };
        h.installed = [] { return sample_installed(); };
        h.prefetch = [&](const PendingUpgrade& p) { fetched.push_back(p.name); };
        h.apply = [&](const PendingUpgrade& p) { applied.push_back(p.name); };

        MaintenanceSettings s; // auto_download on, auto_upgrade off
        auto rep = run_maintenance_cycle(t.path / "Cellar", t.path / "home", s, state, h);

        CHECK(refreshes == 1);
        CHECK(rep.index_refreshed);
        CHECK(rep.pending == 2);
        CHECK(rep.downloaded == 2);
        CHECK(fetched == std::vector<std::string>{"jq", "pv"});
        CHECK(applied.empty());
        CHECK(state.pending.size() == 2);
        CHECK(state.last_check.has_value());
        CHECK(state.deferred.empty()); // tree is no longer pending → deferral dropped
        CHECK(state.errors.empty());
    }

    TEST_CASE("maintenance cycle honours auto_download=false") {
        TempDir t;
        DaemonState state;
        int fetches = 0;
        MaintenanceHooks h;
        h.refresh_index = [] { return sample_index(); };
        h.installed = [] { return sample_installed(); };
        h.prefetch = [&](const PendingUpgrade&) { ++fetches; };
        MaintenanceSettings s;
        s.auto_download = false;
        auto rep = run_maintenance_cycle(t.path / "Cellar", t.path / "home", s, state, h);
        CHECK(fetches == 0);
        CHECK(rep.downloaded == 0);
        CHECK(state.pending.size() == 2);
    }

    TEST_CASE("maintenance cycle applies upgrades in window and clears pending") {
        TempDir t;
        fs::create_directories(t.path / "Cellar" / "jq" / "1.7.1" / "bin");
        fs::create_directories(t.path / "Cellar" / "pv" / "1.11.0" / "bin");
        DaemonState state;
        std::vector<std::string> applied;
        MaintenanceHooks h;
        h.refresh_index = [] { return sample_index(); };
        h.installed = [] { return sample_installed(); };
        h.apply = [&](const PendingUpgrade& p) { applied.push_back(p.name + "@" + p.available); };
        h.in_window = [](const std::string&) { return true; };
        MaintenanceSettings s;
        s.auto_upgrade = true;
        s.upgrade_window = "03:00-05:00";
        auto rep = run_maintenance_cycle(t.path / "Cellar", t.path / "home", s, state, h);
        CHECK(rep.applied == 2);
        CHECK(applied == std::vector<std::string>{"jq@1.8.2", "pv@1.12.0"});
        CHECK(state.pending.empty());
        CHECK(state.last_upgrade.has_value());
    }

    TEST_CASE("maintenance cycle does not upgrade outside the window") {
        TempDir t;
        DaemonState state;
        int applies = 0;
        MaintenanceHooks h;
        h.refresh_index = [] { return sample_index(); };
        h.installed = [] { return sample_installed(); };
        h.apply = [&](const PendingUpgrade&) { ++applies; };
        h.in_window = [](const std::string&) { return false; };
        MaintenanceSettings s;
        s.auto_upgrade = true;
        s.upgrade_window = "03:00-05:00";
        auto rep = run_maintenance_cycle(t.path / "Cellar", t.path / "home", s, state, h);
        CHECK(applies == 0);
        CHECK(rep.applied == 0);
        CHECK(state.pending.size() == 2);
    }

    TEST_CASE("failed apply keeps the package pending and records the error") {
        TempDir t;
        DaemonState state;
        MaintenanceHooks h;
        h.refresh_index = [] { return sample_index(); };
        h.installed = [] { return sample_installed(); };
        h.apply = [](const PendingUpgrade& p) {
            if (p.name == "jq")
                throw std::runtime_error("pour failed");
        };
        MaintenanceSettings s;
        s.auto_upgrade = true;
        auto rep = run_maintenance_cycle(t.path / "Cellar", t.path / "home", s, state, h);
        CHECK(rep.applied == 1);
        REQUIRE(state.pending.size() == 1);
        CHECK(state.pending[0].name == "jq");
        REQUIRE(state.errors.size() == 1);
        CHECK(state.errors[0].find("upgrade jq: pour failed") != std::string::npos);
    }

    TEST_CASE("index refresh failure keeps previous pending and skips download/upgrade") {
        TempDir t;
        DaemonState state;
        state.pending.push_back({"jq", "1.7.1", "1.8.2"});
        int fetches = 0, applies = 0;
        MaintenanceHooks h;
        h.refresh_index = []() -> PackageIndex { throw std::runtime_error("offline"); };
        h.installed = [] { return sample_installed(); };
        h.prefetch = [&](const PendingUpgrade&) { ++fetches; };
        h.apply = [&](const PendingUpgrade&) { ++applies; };
        MaintenanceSettings s;
        s.auto_upgrade = true;
        auto rep = run_maintenance_cycle(t.path / "Cellar", t.path / "home", s, state, h);
        CHECK_FALSE(rep.index_refreshed);
        CHECK(fetches == 0);
        CHECK(applies == 0);
        REQUIRE(state.pending.size() == 1);
        REQUIRE(state.errors.size() == 1);
        CHECK(state.errors[0].find("index refresh: offline") != std::string::npos);
    }

    TEST_CASE("download failures are recorded per package and do not stop the cycle") {
        TempDir t;
        DaemonState state;
        MaintenanceHooks h;
        h.refresh_index = [] { return sample_index(); };
        h.installed = [] { return sample_installed(); };
        h.prefetch = [](const PendingUpgrade& p) {
            if (p.name == "jq")
                throw std::runtime_error("sha mismatch");
        };
        MaintenanceSettings s;
        auto rep = run_maintenance_cycle(t.path / "Cellar", t.path / "home", s, state, h);
        CHECK(rep.downloaded == 1);
        REQUIRE(state.errors.size() == 1);
        CHECK(state.errors[0].find("download jq") != std::string::npos);
    }

} // TEST_SUITE

} // namespace test
} // namespace den
