// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Package pinning: den pin / unpin, and upgrades (manual and daemon) skip
// pinned packages.

#include <doctest.h>

#include "cli/install.h"
#include "cli/pin.h"
#include "core/error.h"
#include "daemon/upgrade_pass.h"
#include "env/environment.h"
#include "env/manifest.h"
#include "index/package.h"
#include "provider/stub_provider.h"
#include "store/pins.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>
#include <sys/wait.h>

namespace fs = std::filesystem;
using namespace den;

namespace {

struct PinTmp {
    fs::path path;
    PinTmp() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("den-pin-test-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~PinTmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    PinTmp(const PinTmp&) = delete;
    PinTmp& operator=(const PinTmp&) = delete;
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

// Seed the active environment with stub-provider packages.
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

} // namespace

TEST_SUITE("pins") {

    TEST_CASE("pin store round-trips and starts empty") {
        PinTmp t;
        CHECK(read_pins(t.path).empty());
        CHECK(pin_package(t.path, "jq", "1.7.1") == PinOutcome::Pinned);
        CHECK(pin_package(t.path, "tree", "2.2.1") == PinOutcome::Pinned);
        auto pins = read_pins(t.path);
        REQUIRE(pins.size() == 2);
        CHECK(pins.at("jq") == "1.7.1");
        // Re-pinning keeps the original version (brew: "already pinned").
        CHECK(pin_package(t.path, "jq", "9.9") == PinOutcome::AlreadyPinned);
        CHECK(read_pins(t.path).at("jq") == "1.7.1");
        CHECK(unpin_package(t.path, "jq"));
        CHECK_FALSE(unpin_package(t.path, "jq"));
        CHECK(read_pins(t.path).count("jq") == 0);
        CHECK(read_pins(t.path).count("tree") == 1);
    }

    TEST_CASE("corrupt pins.json fails closed") {
        PinTmp t;
        std::ofstream(pins_path(t.path)) << "{not json";
        CHECK_THROWS_AS(read_pins(t.path), UserError);
        std::ofstream(pins_path(t.path), std::ios::trunc) << R"({"pins": {"jq": 3}})";
        CHECK_THROWS_AS(read_pins(t.path), UserError);
        std::ofstream(pins_path(t.path), std::ios::trunc) << R"({"schema_version": 1})";
        CHECK_THROWS_AS(read_pins(t.path), UserError);
    }

    TEST_CASE("partition_pinned holds pinned candidates back") {
        PinMap pins{{"jq", "1.7.1"}};
        auto split = partition_pinned({{"jq", "1.7.1", "1.8.1"}, {"tree", "2.1", "2.2"}}, pins);
        REQUIRE(split.held.size() == 1);
        CHECK(split.held[0].name == "jq");
        REQUIRE(split.upgrade.size() == 1);
        CHECK(split.upgrade[0].name == "tree");
    }

    TEST_CASE("den upgrade (all) skips pinned packages") {
        PinTmp t;
        auto cfg = scratch_config(t.path);
        StubProvider stub;
        seed(cfg, stub, {{"alpha", "1.0"}, {"beta", "1.0"}});
        auto idx = index_with({{"alpha", "2.0"}, {"beta", "2.0"}});
        REQUIRE(pin_package(cfg.den_home, "alpha", "1.0") == PinOutcome::Pinned);

        upgrade_packages(cfg, stub, idx, {});

        CHECK(active_version(cfg, "alpha") == "1.0");
        CHECK(active_version(cfg, "beta") == "2.0");
    }

    TEST_CASE("den upgrade <pinned> refuses and leaves it alone") {
        PinTmp t;
        auto cfg = scratch_config(t.path);
        StubProvider stub;
        seed(cfg, stub, {{"alpha", "1.0"}, {"beta", "1.0"}});
        auto idx = index_with({{"alpha", "2.0"}, {"beta", "2.0"}});
        pin_package(cfg.den_home, "alpha", "1.0");

        CHECK_THROWS_AS(upgrade_packages(cfg, stub, idx, {"alpha"}), UserError);
        CHECK(active_version(cfg, "alpha") == "1.0");

        // Mixed request: the unpinned name still upgrades, then it reports.
        CHECK_THROWS_AS(upgrade_packages(cfg, stub, idx, {"alpha", "beta"}), UserError);
        CHECK(active_version(cfg, "alpha") == "1.0");
        CHECK(active_version(cfg, "beta") == "2.0");

        // After unpinning the upgrade goes through.
        CHECK(unpin_package(cfg.den_home, "alpha"));
        upgrade_packages(cfg, stub, idx, {"alpha"});
        CHECK(active_version(cfg, "alpha") == "2.0");
    }

    TEST_CASE("uninstall drops the pin") {
        PinTmp t;
        auto cfg = scratch_config(t.path);
        StubProvider stub;
        seed(cfg, stub, {{"alpha", "1.0"}});
        pin_package(cfg.den_home, "alpha", "1.0");
        uninstall_packages(cfg, stub, {"alpha"});
        CHECK(read_pins(cfg.den_home).empty());
    }

    TEST_CASE("run_pin / run_unpin / list_pins") {
        PinTmp t;
        auto cfg = scratch_config(t.path);
        StubProvider stub;
        seed(cfg, stub, {{"alpha", "1.0"}});

        std::ostringstream out, err;
        CHECK(run_pin(cfg, {"alpha", "ghost"}, out, err) == 1);
        CHECK(out.str().find("Pinned alpha at 1.0") != std::string::npos);
        CHECK(err.str().find("'ghost' is not installed") != std::string::npos);
        CHECK(read_pins(cfg.den_home).count("ghost") == 0);

        std::ostringstream list;
        list_pins(cfg, list);
        CHECK(list.str() == "alpha 1.0\n");

        std::ostringstream o2, e2;
        CHECK(run_unpin(cfg, {"alpha", "alpha"}, o2, e2) == 1);
        CHECK(o2.str() == "Unpinned alpha\n");
        CHECK(e2.str().find("not pinned") != std::string::npos);
    }

    TEST_CASE("daemon upgrade pass never applies a pinned upgrade") {
        PinTmp t;
        fs::path cellar = t.path / "Cellar";
        fs::path den_home = t.path / "den";
        fs::create_directories(cellar / "alpha" / "1.0");
        fs::create_directories(cellar / "beta" / "1.0");
        fs::create_directories(den_home);
        pin_package(den_home, "alpha", "1.0");

        DaemonState state;
        state.pending.push_back({"alpha", "1.0", "2.0"});
        state.pending.push_back({"beta", "1.0", "2.0"});
        std::vector<std::string> applied;
        auto result = run_upgrade_pass(
            cellar, den_home, state, [&](const PendingUpgrade& pu) { applied.push_back(pu.name); });

        CHECK(applied == std::vector<std::string>{"beta"});
        REQUIRE(result.pinned.size() == 1);
        CHECK(result.pinned[0].name == "alpha");
        // Still pending so `den outdated` keeps showing it.
        REQUIRE(state.pending.size() == 1);
        CHECK(state.pending[0].name == "alpha");
    }

    TEST_CASE("daemon upgrade pass holds everything when pins.json is corrupt") {
        PinTmp t;
        fs::path cellar = t.path / "Cellar";
        fs::path den_home = t.path / "den";
        fs::create_directories(cellar / "beta" / "1.0");
        fs::create_directories(den_home);
        std::ofstream(pins_path(den_home)) << "garbage";

        DaemonState state;
        state.pending.push_back({"beta", "1.0", "2.0"});
        bool called = false;
        auto result = run_upgrade_pass(cellar, den_home, state,
                                       [&](const PendingUpgrade&) { called = true; });
        CHECK_FALSE(called);
        CHECK(result.applied.empty());
        CHECK(state.pending.size() == 1);
        CHECK_FALSE(state.errors.empty());
    }

} // TEST_SUITE

// End-to-end through the den binary (built alongside den_tests; ctest runs
// from the build directory).
namespace {

struct Run {
    std::string out;
    int code;
};

Run run_den(const std::string& args, const fs::path& root) {
    const auto prefix = (root / "prefix").string();
    std::string cmd = "HOME=" + root.string() + " DEN_HOME=" + (root / "den").string() +
                      " DEN_PREFIX=" + prefix + " HOMEBREW_PREFIX=" + prefix +
                      " HOMEBREW_CELLAR=" + prefix + "/Cellar ./den " + args + " 2>&1";
    FILE* pipe = ::popen(cmd.c_str(), "r");
    REQUIRE(pipe != nullptr);
    std::string out;
    std::array<char, 512> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
        out += buf.data();
    }
    int raw = ::pclose(pipe);
    return {out, WIFEXITED(raw) ? WEXITSTATUS(raw) : -1};
}

} // namespace

TEST_SUITE("pins::cli") {
    TEST_CASE("den pin / unpin / pin --list") {
        if (!fs::exists("./den")) {
            MESSAGE("den binary not in cwd — skipping");
            return;
        }
        PinTmp t;
        auto cfg = scratch_config(t.path);
        StubProvider stub;
        seed(cfg, stub, {{"alpha", "1.0"}});

        auto r = run_den("pin alpha", t.path);
        CHECK(r.code == 0);
        CHECK(r.out.find("Pinned alpha at 1.0") != std::string::npos);

        r = run_den("pin ghost", t.path);
        CHECK(r.code != 0);
        CHECK(r.out.find("not installed") != std::string::npos);

        r = run_den("pin --list", t.path);
        CHECK(r.code == 0);
        CHECK(r.out.find("alpha 1.0") != std::string::npos);

        r = run_den("unpin alpha", t.path);
        CHECK(r.code == 0);
        CHECK(r.out.find("Unpinned alpha") != std::string::npos);

        r = run_den("pin", t.path);
        CHECK(r.code == 0);
        CHECK(r.out.find("alpha") == std::string::npos);
    }
}
