// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// An upgrade never downgrades. When the index version is older than the
// installed one (a stale index, a mirror lag, or upstream moving a formula
// back, as homebrew-core did with openssl@3 3.6.x -> 3.5.9 LTS), neither the
// daemon's auto-upgrade nor `den upgrade` applies it; both report it.
// Downgrading takes an explicit `den use <name> <version>`.

#include <doctest.h>

#include "cli/daemon_status.h"
#include "cli/install.h"
#include "core/version.h"
#include "daemon/daemon.h"
#include "daemon/upgrade_pass.h"
#include "env/environment.h"
#include "env/manifest.h"
#include "index/package.h"
#include "provider/stub_provider.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace den;

namespace {

struct NdTmp {
    fs::path path;
    NdTmp() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("den-nodowngrade-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~NdTmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    NdTmp(const NdTmp&) = delete;
    NdTmp& operator=(const NdTmp&) = delete;
};

Config scratch_config(const fs::path& root) {
    Config c;
    c.den_home = root / "den";
    c.homebrew_prefix = root / "prefix";
    c.store = c.homebrew_prefix / "Cellar";
    c.homebrew_cellar = c.store;
    c.homebrew_caskroom = c.homebrew_prefix / "Caskroom";
    c.homebrew_taps = c.homebrew_prefix / "Library" / "Taps";
    c.cache = c.den_home / "cache";
    c.appdir = root / "Applications";
    c.arch = Arch::Arm64;
    fs::create_directories(c.den_home);
    fs::create_directories(c.store);
    return c;
}

PackageIndex index_with(std::initializer_list<std::pair<const char*, const char*>> pkgs) {
    PackageIndex idx;
    for (const auto& [name, version] : pkgs) {
        Package p;
        p.name = name;
        p.version = version;
        idx.packages[p.name] = p;
    }
    return idx;
}

void seed(const Config& cfg, StubProvider& stub,
          std::initializer_list<std::pair<const char*, const char*>> pkgs) {
    Manifest m;
    for (const auto& [name, version] : pkgs) {
        stub.install(cfg, name, version);
        m.packages[std::string(stub.provider_name())][name] = version;
    }
    write_manifest(cfg.den_home, active_env_path(cfg.den_home), m);
}

std::string active_version(const Config& cfg, const std::string& name) {
    auto r = resolve(cfg.den_home, active_env_path(cfg.den_home));
    auto it = r.find(name);
    return it == r.end() ? "" : it->second;
}

// Capture std::cout for the duration of a call.
struct CoutCapture {
    std::ostringstream buf;
    std::streambuf* old;
    CoutCapture() : old(std::cout.rdbuf(buf.rdbuf())) {}
    ~CoutCapture() { std::cout.rdbuf(old); }
    std::string str() const { return buf.str(); }
};

std::string read_file(const fs::path& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

TEST_SUITE("no_downgrade") {

    TEST_CASE("compare_pkg_versions orders versions, revisions and pre-releases") {
        CHECK(compare_pkg_versions("3.5.9", "3.6.3") < 0);
        CHECK(compare_pkg_versions("3.6.3", "3.5.9") > 0);
        CHECK(compare_pkg_versions("3.6.10", "3.6.9") > 0); // numeric, not lexical
        CHECK(compare_pkg_versions("1.0", "1.0.0") == 0);
        // Homebrew revisions: "_N" is part of the order.
        CHECK(compare_pkg_versions("3.6.3", "3.6.3_1") < 0);
        CHECK(compare_pkg_versions("3.6.3_2", "3.6.3_1") > 0);
        CHECK(compare_pkg_versions("3.6.3_9", "3.6.4") < 0);
        CHECK(compare_pkg_versions("3.6.4_1", "3.6.4") > 0);
        CHECK(compare_pkg_versions("3.5.9", "3.6.3_1") < 0);
        // Letter suffixes and pre-releases.
        CHECK(compare_pkg_versions("1.1.1w", "1.1.1") > 0);
        CHECK(compare_pkg_versions("1.1.1w", "1.1.1v") > 0);
        CHECK(compare_pkg_versions("2.0rc1", "2.0") < 0);
        CHECK(compare_pkg_versions("2.0-beta2", "2.0-beta10") < 0);
        CHECK(compare_pkg_versions("2.0.1", "2.0rc1") > 0);
        CHECK(compare_pkg_versions("2024a", "2023c") > 0);
    }

    TEST_CASE("version_delta classifies index vs installed") {
        CHECK(version_delta("3.6.3", "3.5.9") == VersionDelta::Older);
        CHECK(version_delta("3.6.3", "3.6.3") == VersionDelta::Same);
        CHECK(version_delta("3.6.3", "3.6.3_1") == VersionDelta::Newer);
        CHECK(version_delta("3.6.3_1", "3.6.3") == VersionDelta::Older);
        CHECK(version_delta("1.7.1", "1.8.2") == VersionDelta::Newer);
    }

    TEST_CASE("compute_pending never queues a downgrade and reports it") {
        auto idx = index_with({{"openssl@3", "3.5.9"}, {"jq", "1.8.2"}, {"xz", "5.8.1_1"}});
        std::map<std::string, std::string> installed{
            {"openssl@3", "3.6.3"}, {"jq", "1.7.1"}, {"xz", "5.8.1"}};
        std::vector<PendingUpgrade> older;
        auto pending = compute_pending(installed, idx, &older);
        REQUIRE(pending.size() == 2);
        CHECK(pending[0].name == "jq");
        CHECK(pending[1].name == "xz"); // revision bump is an upgrade
        CHECK(pending[1].available == "5.8.1_1");
        REQUIRE(older.size() == 1);
        CHECK(older[0].name == "openssl@3");
        CHECK(older[0].installed == "3.6.3");
        CHECK(older[0].available == "3.5.9");
        CHECK(index_older_message(older[0]).find("index older than installed") !=
              std::string::npos);
        CHECK(index_older_message(older[0]).find("den use openssl@3 3.5.9") != std::string::npos);

        // Revision-only downgrade is also a downgrade.
        older.clear();
        auto p2 = compute_pending({{"xz", "5.8.1_2"}}, idx, &older);
        CHECK(p2.empty());
        REQUIRE(older.size() == 1);
        CHECK(older[0].name == "xz");
    }

    TEST_CASE("daemon cycle with auto_upgrade never applies a downgrade") {
        NdTmp t;
        fs::path cellar = t.path / "Cellar";
        fs::path home = t.path / "home";
        fs::create_directories(cellar / "openssl@3" / "3.6.3" / "bin");
        fs::create_directories(cellar / "jq" / "1.7.1" / "bin");
        fs::create_directories(home);
        DaemonState state;
        std::vector<std::string> applied, fetched;
        MaintenanceHooks h;
        h.refresh_index = [] { return index_with({{"openssl@3", "3.5.9"}, {"jq", "1.8.2"}}); };
        h.installed = [] {
            return std::map<std::string, std::string>{{"openssl@3", "3.6.3"}, {"jq", "1.7.1"}};
        };
        h.prefetch = [&](const PendingUpgrade& p) { fetched.push_back(p.name); };
        h.apply = [&](const PendingUpgrade& p) { applied.push_back(p.name + "@" + p.available); };
        h.in_window = [](const std::string&) { return true; };
        MaintenanceSettings s;
        s.auto_upgrade = true;
        auto rep = run_maintenance_cycle(cellar, home, s, state, h);

        CHECK(applied == std::vector<std::string>{"jq@1.8.2"});
        CHECK(fetched == std::vector<std::string>{"jq"}); // no bottle fetched for a downgrade
        CHECK(rep.index_older == 1);
        REQUIRE(state.index_older.size() == 1);
        CHECK(state.index_older[0].name == "openssl@3");
        CHECK(state.pending.empty());
        CHECK(read_file(home / "daemon.log").find("openssl@3 3.6.3: index older than installed") !=
              std::string::npos);

        // Persisted and surfaced by `den daemon status`.
        write_daemon_state(home, state);
        auto back = read_daemon_state(home);
        REQUIRE(back.index_older.size() == 1);
        CHECK(back.index_older[0].available == "3.5.9");
        std::ostringstream out;
        format_daemon_status(out, back);
        CHECK(out.str().find("Not downgrading") != std::string::npos);
        CHECK(out.str().find("openssl@3 3.6.3 (index has 3.5.9") != std::string::npos);
    }

    TEST_CASE("upgrade pass drops a stale downgrade entry instead of applying it") {
        NdTmp t;
        fs::path cellar = t.path / "Cellar";
        fs::create_directories(cellar / "openssl@3" / "3.6.3");
        DaemonState state;
        state.pending.push_back({"openssl@3", "3.6.3", "3.5.9"}); // e.g. an older den's state
        int applies = 0;
        auto res = run_upgrade_pass(cellar, t.path / "home", state,
                                    [&](const PendingUpgrade&) { ++applies; });
        CHECK(applies == 0);
        CHECK(res.applied.empty());
        REQUIRE(res.not_newer.size() == 1);
        CHECK(state.pending.empty());
    }

    TEST_CASE("den upgrade (all) skips and reports an older index") {
        NdTmp t;
        auto cfg = scratch_config(t.path);
        StubProvider stub;
        seed(cfg, stub, {{"openssl@3", "3.6.3"}, {"jq", "1.7.1"}});
        auto idx = index_with({{"openssl@3", "3.5.9"}, {"jq", "1.8.2"}});

        CoutCapture cap;
        upgrade_packages(cfg, stub, idx, {});
        CHECK(active_version(cfg, "openssl@3") == "3.6.3");
        CHECK(active_version(cfg, "jq") == "1.8.2");
        CHECK(cap.str().find("index older than installed") != std::string::npos);
        CHECK(cap.str().find("den use openssl@3 3.5.9") != std::string::npos);
    }

    TEST_CASE("den upgrade <name> refuses to downgrade") {
        NdTmp t;
        auto cfg = scratch_config(t.path);
        StubProvider stub;
        seed(cfg, stub, {{"openssl@3", "3.6.3_1"}});
        auto idx = index_with({{"openssl@3", "3.6.3"}});

        CoutCapture cap;
        upgrade_packages(cfg, stub, idx, {"openssl@3"});
        CHECK(active_version(cfg, "openssl@3") == "3.6.3_1");
        CHECK(cap.str().find("Not downgrading 1 package(s)") != std::string::npos);
        CHECK(cap.str().find("Nothing to upgrade.") != std::string::npos);
        CHECK(cap.str().find("All packages are up to date") == std::string::npos);
    }
}
