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
#include "env/version_change.h"
#include "index/index.h"
#include "index/package.h"
#include "provider/stub_provider.h"
#include "store/store.h"

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

void write_text(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p);
    f << text;
}

// A Homebrew INSTALL_RECEIPT.json carrying source.versions.version_scheme.
void write_brew_receipt(const fs::path& keg, const std::string& stable, int scheme) {
    write_text(keg / "INSTALL_RECEIPT.json",
               R"({"installed_on_request":true,"source":{"spec":"stable","versions":{"stable":")" +
                   stable + R"(","head":null,"version_scheme":)" + std::to_string(scheme) + "}}}");
}

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

    // ---- version_scheme: (version_scheme, version, revision) ordering ----

    TEST_CASE("version tuple: a scheme bump to a lower version is held, not an upgrade") {
        // The tuple order itself is unchanged: (1, 3.5.9) outranks (0, 3.6.3).
        CHECK(compare_versioned(1, "3.5.9", 0, "3.6.3") > 0);
        CHECK(compare_versioned(0, "3.6.3", 1, "3.5.9") < 0);
        // But an upgrade never lowers the version: openssl@3 3.6.3 (scheme 0)
        // -> 3.5.9 LTS (scheme 1) is held.
        CHECK(version_delta("3.6.3", 0, "3.5.9", 1) == VersionDelta::Held);
        CHECK(version_delta("3.6.3_2", 0, "3.5.9", 1) == VersionDelta::Held);
        CHECK(version_delta("3.6.3_1", 0, "3.6.3", 1) == VersionDelta::Held);
        CHECK(version_hold_reason("3.6.3", "3.5.9", 1) ==
              "held: index pins 3.5.9 (version_scheme 1), below installed 3.6.3");
        // A scheme bump to a higher version is still an upgrade.
        CHECK(version_delta("3.6.3", 0, "3.7.0", 1) == VersionDelta::Newer);
        CHECK(version_delta("3.6.3", 0, "3.6.3_1", 1) == VersionDelta::Newer);
        // An index that predates the bump is older than a scheme-1 install.
        CHECK(version_delta("3.5.9", 1, "3.6.3", 0) == VersionDelta::Older);
        // Same pkg_version is the same keg whatever the schemes say.
        CHECK(version_delta("3.5.9", 0, "3.5.9", 1) == VersionDelta::Same);
    }

    TEST_CASE("version tuple: lower version in the same scheme is refused") {
        CHECK(version_delta("3.6.3", 0, "3.5.9", 0) == VersionDelta::Older);
        CHECK(version_delta("3.6.3", 1, "3.5.9", 1) == VersionDelta::Older);
        CHECK(version_delta("3.6.3_1", 1, "3.6.3", 1) == VersionDelta::Older);
        CHECK(version_delta("3.5.9", 1, "3.6.0", 1) == VersionDelta::Newer);
    }

    TEST_CASE("version tuple: revision ordering") {
        CHECK(compare_versioned(0, "1.11.1", 0, "1.11.1_7") < 0);
        CHECK(compare_versioned(0, "1.11.1_7", 0, "1.11.2") < 0);
        CHECK(compare_versioned(0, "1.11.1", 0, "1.11.2") < 0);
        CHECK(compare_versioned(2, "1.11.1_7", 2, "1.11.1_6") > 0);
        CHECK(version_delta("1.11.1", 0, "1.11.1_7", 0) == VersionDelta::Newer);
        CHECK(version_delta("1.11.1_7", 0, "1.11.2", 0) == VersionDelta::Newer);
        CHECK(version_delta("1.11.2", 0, "1.11.1_7", 0) == VersionDelta::Older);
    }

    TEST_CASE("installed_version_scheme: DEN_RECEIPT, then INSTALL_RECEIPT, else 0") {
        NdTmp t;
        fs::path keg = t.path / "Cellar" / "openssl@3" / "3.6.3";
        fs::create_directories(keg);
        CHECK(installed_version_scheme(keg) == 0); // no receipt
        CHECK(installed_version_scheme(t.path / "missing") == 0);

        write_brew_receipt(keg, "3.6.3", 2);
        CHECK(installed_version_scheme(keg) == 2);

        write_text(keg / "INSTALL_RECEIPT.json", "{not json");
        CHECK(installed_version_scheme(keg) == 0);

        // den's own receipt wins and records the scheme it poured with.
        mark_keg_owned(keg, "openssl@3", "3.6.3", 1);
        CHECK(installed_version_scheme(keg) == 1);
        CHECK(keg_owned_by_den(keg));

        // A receipt from before den recorded schemes falls back to brew's.
        write_text(keg / "DEN_RECEIPT.json",
                   R"({"poured_by":"den","name":"openssl@3","version":"3.6.3"})");
        write_brew_receipt(keg, "3.6.3", 3);
        CHECK(installed_version_scheme(keg) == 3);
    }

    TEST_CASE("index reads version_scheme from formula JSON and keeps it in the cache") {
        NdTmp t;
        const std::string formulae = R"([
            {"name":"openssl@3","full_name":"openssl@3","versions":{"stable":"3.5.9"},
             "revision":0,"version_scheme":1,"dependencies":[]},
            {"name":"jq","full_name":"jq","versions":{"stable":"1.8.2"},"revision":0,
             "dependencies":[]}
        ])";
        auto idx = transform_homebrew(formulae, "[]");
        REQUIRE(idx.find("openssl@3") != nullptr);
        CHECK(idx.find("openssl@3")->version_scheme == 1);
        REQUIRE(idx.find("jq") != nullptr);
        CHECK(idx.find("jq")->version_scheme == 0); // absent -> 0

        save_index(idx, t.path / "index.json");
        auto back = load_index(t.path / "index.json");
        REQUIRE(back.find("openssl@3") != nullptr);
        CHECK(back.find("openssl@3")->version_scheme == 1);
        CHECK(back.find("jq")->version_scheme == 0);
        auto from_json = load_index_json(t.path / "index.json"); // JSON, not the binary cache
        REQUIRE(from_json.find("openssl@3") != nullptr);
        CHECK(from_json.find("openssl@3")->version_scheme == 1);
    }

    // ---- openssl@3 3.6.3 installed, index pins 3.5.9 with version_scheme 1 ----

    TEST_CASE("daemon plan: a scheme bump to a lower version is held with its reason") {
        auto idx = index_with({{"openssl@3", "3.5.9"}, {"jq", "1.8.2"}, {"libfoo", "2.0"}});
        idx.packages["openssl@3"].version_scheme = 1;
        std::map<std::string, std::string> installed{
            {"openssl@3", "3.6.3"}, {"jq", "1.7.1"}, {"libfoo", "2.1"}};
        auto schemes = [](const std::string&, const std::string&) { return 0; };
        std::vector<PendingUpgrade> older;
        std::vector<DeferredUpgrade> held;
        auto pending = compute_pending(installed, idx, &older, schemes, &held);
        REQUIRE(pending.size() == 1);
        CHECK(pending[0].name == "jq"); // a normal newer version still upgrades
        REQUIRE(older.size() == 1);
        CHECK(older[0].name == "libfoo"); // lower version, same scheme
        REQUIRE(held.size() == 1);
        CHECK(held[0].name == "openssl@3");
        CHECK(held[0].installed == "3.6.3");
        CHECK(held[0].available == "3.5.9");
        CHECK(held[0].reason == "held: index pins 3.5.9 (version_scheme 1), below installed 3.6.3");
        CHECK(version_held_message(held[0]) ==
              "openssl@3 3.6.3: held: index pins 3.5.9 (version_scheme 1), below installed "
              "3.6.3; not downgrading. To downgrade explicitly: den use openssl@3 3.5.9");

        // Full cycle with auto_upgrade: the scheme comes from the keg receipt.
        NdTmp t;
        fs::path cellar = t.path / "Cellar";
        fs::path home = t.path / "home";
        fs::create_directories(home);
        write_brew_receipt(cellar / "openssl@3" / "3.6.3", "3.6.3", 0);
        fs::create_directories(cellar / "jq" / "1.7.1");
        fs::create_directories(cellar / "libfoo" / "2.1");
        DaemonState state;
        std::vector<std::string> applied, fetched;
        MaintenanceHooks h;
        h.refresh_index = [&] { return idx; };
        h.installed = [&] { return installed; };
        h.installed_scheme = [&](const std::string& n, const std::string& v) {
            return installed_version_scheme(cellar / n / v);
        };
        h.prefetch = [&](const PendingUpgrade& p) { fetched.push_back(p.name); };
        h.apply = [&](const PendingUpgrade& p) { applied.push_back(p.name + "@" + p.available); };
        h.in_window = [](const std::string&) { return true; };
        MaintenanceSettings s;
        s.auto_upgrade = true;
        auto rep = run_maintenance_cycle(cellar, home, s, state, h);
        CHECK(applied == std::vector<std::string>{"jq@1.8.2"});
        CHECK(fetched == std::vector<std::string>{"jq"}); // no bottle fetched for openssl@3
        CHECK(rep.version_held == 1);
        CHECK(rep.index_older == 1);
        CHECK(state.pending.empty());
        REQUIRE(state.version_held.size() == 1);
        CHECK(state.version_held[0].name == "openssl@3");
        CHECK(read_file(home / "daemon.log")
                  .find("openssl@3 3.6.3: held: index pins 3.5.9 (version_scheme 1), below "
                        "installed 3.6.3; not downgrading") != std::string::npos);

        // Without auto_upgrade the plan (pending) carries only jq.
        DaemonState plan;
        MaintenanceSettings s2;
        s2.auto_download = false;
        run_maintenance_cycle(cellar, home, s2, plan, h);
        REQUIRE(plan.pending.size() == 1);
        CHECK(plan.pending[0].name == "jq");

        // The hold and its reason survive the state file and `den daemon status`.
        write_daemon_state(home, plan);
        auto back = read_daemon_state(home);
        REQUIRE(back.pending.size() == 1);
        CHECK(back.pending[0].name == "jq");
        REQUIRE(back.version_held.size() == 1);
        CHECK(back.version_held[0].reason ==
              "held: index pins 3.5.9 (version_scheme 1), below installed 3.6.3");
        std::ostringstream out;
        format_daemon_status(out, back);
        const auto status = out.str();
        CHECK(status.find("Held (index version below installed):") != std::string::npos);
        CHECK(status.find("openssl@3 3.6.3 (held: index pins 3.5.9 (version_scheme 1), below "
                          "installed 3.6.3; to downgrade: den use openssl@3 3.5.9)") !=
              std::string::npos);
        CHECK(status.find("openssl@3 3.6.3 -> 3.5.9") == std::string::npos);

        // A persisted entry from an older den (pending openssl@3 -> 3.5.9,
        // scheme 1) is dropped by the upgrade pass, not applied.
        DaemonState stale;
        stale.pending.push_back({"openssl@3", "3.6.3", "3.5.9", 0, 1});
        int applies = 0;
        auto res = run_upgrade_pass(cellar, home, stale, [&](const PendingUpgrade&) { ++applies; });
        CHECK(applies == 0);
        CHECK(res.not_newer.size() == 1);
    }

    TEST_CASE("den upgrade --dry-run and den upgrade hold a scheme bump to a lower version") {
        NdTmp t;
        auto cfg = scratch_config(t.path);
        StubProvider stub;
        seed(cfg, stub, {{"openssl@3", "3.6.3"}, {"jq", "1.7.1"}, {"libfoo", "2.1"}});
        write_brew_receipt(cfg.store / "openssl@3" / "3.6.3", "3.6.3", 0);
        auto idx = index_with({{"openssl@3", "3.5.9"}, {"jq", "1.8.2"}, {"libfoo", "2.0"}});
        idx.packages["openssl@3"].version_scheme = 1;
        const std::string held_line = "openssl@3 3.6.3 (held: index pins 3.5.9 (version_scheme "
                                      "1), below installed 3.6.3; to downgrade: den use "
                                      "openssl@3 3.5.9)";
        const auto manifest_before =
            read_file(cfg.den_home / "manifests" / "ROOT" / "manifest.json");

        { // --dry-run: the plan holds openssl@3, lists jq, and changes nothing.
            CoutCapture cap;
            upgrade_packages(cfg, stub, idx, {}, /*dry_run=*/true);
            const auto out = cap.str();
            CHECK(out.find("Holding 1 package(s) (index version below installed):") !=
                  std::string::npos);
            CHECK(out.find(held_line) != std::string::npos);
            CHECK(out.find("Would upgrade 1 package(s):\n  jq 1.7.1 -> 1.8.2\n") !=
                  std::string::npos);
            CHECK(out.find("openssl@3 3.6.3 -> 3.5.9") == std::string::npos);
            CHECK(out.find("Dry run: nothing changed.") != std::string::npos);
            CHECK(active_version(cfg, "openssl@3") == "3.6.3");
            CHECK(active_version(cfg, "jq") == "1.7.1");
            CHECK(read_file(cfg.den_home / "manifests" / "ROOT" / "manifest.json") ==
                  manifest_before);
            CHECK_FALSE(stub.is_installed(cfg, "jq", "1.8.2"));
            CHECK_FALSE(stub.is_installed(cfg, "openssl@3", "3.5.9"));
        }
        { // Naming it explicitly is held too.
            CoutCapture cap;
            upgrade_packages(cfg, stub, idx, {"openssl@3"}, /*dry_run=*/true);
            CHECK(cap.str().find(held_line) != std::string::npos);
            CHECK(cap.str().find("Nothing to upgrade.") != std::string::npos);
        }
        { // Real run: jq upgrades, openssl@3 stays at 3.6.3 and is reported held.
            CoutCapture cap;
            upgrade_packages(cfg, stub, idx, {});
            const auto out = cap.str();
            CHECK(out.find(held_line) != std::string::npos);
            CHECK(out.find("1 package(s) to upgrade:\n  jq 1.7.1 -> 1.8.2\n") != std::string::npos);
            CHECK(out.find("openssl@3 3.6.3 -> 3.5.9") == std::string::npos);
            CHECK(active_version(cfg, "openssl@3") == "3.6.3");
            CHECK(active_version(cfg, "jq") == "1.8.2");
            CHECK(active_version(cfg, "libfoo") == "2.1"); // same-scheme downgrade refused
            CHECK_FALSE(stub.is_installed(cfg, "openssl@3", "3.5.9"));
        }
        { // `den upgrade openssl@3` does not move it either.
            CoutCapture cap;
            upgrade_packages(cfg, stub, idx, {"openssl@3"});
            CHECK(cap.str().find(held_line) != std::string::npos);
            CHECK(cap.str().find("Nothing to upgrade.") != std::string::npos);
            CHECK(active_version(cfg, "openssl@3") == "3.6.3");
        }
        { // `den use openssl@3 3.5.9` (install, then switch without install,
            // as the command does) is the explicit way down, and still works.
            stub.install(cfg, "openssl@3", "3.5.9");
            VersionChangeOptions vopts;
            vopts.install = false;
            auto res =
                apply_version_changes(cfg, stub, nullptr, {{"openssl@3", "3.6.3", "3.5.9"}}, vopts);
            CHECK(res.rolled_back.empty());
            REQUIRE(res.applied.size() == 1);
            CHECK(active_version(cfg, "openssl@3") == "3.5.9");
        }
    }
}
