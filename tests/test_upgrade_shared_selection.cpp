// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// `den upgrade` with no names selects its upgrades through outdated_plan, the
// same selection `den outdated`, `den doctor` and the daemon's maintenance
// cycle use (compute_pending). upgrade_packages used to run its own loop,
// which differed in one way: it did not skip casks or disabled formulae, so
// a bare `den upgrade` swapped live .app bundles that `den outdated` and the
// daemon never listed. Casks are now upgraded only when named.

#include <doctest.h>

#include "cli/install.h"
#include "daemon/daemon.h"
#include "env/environment.h"
#include "env/manifest.h"
#include "index/package.h"
#include "provider/stub_provider.h"

#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace den;

namespace {

struct SsTmp {
    fs::path path;
    SsTmp() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("den-upgrade-shared-selection-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~SsTmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    SsTmp(const SsTmp&) = delete;
    SsTmp& operator=(const SsTmp&) = delete;
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

class RecordingProvider : public StubProvider {
  public:
    std::string_view provider_name() const override { return "homebrew"; }
    InstallResult install(const Config& config, std::string_view name,
                          std::string_view version_hint) override {
        installs.push_back(std::string(name) + "@" + std::string(version_hint));
        return StubProvider::install(config, name, version_hint);
    }
    std::vector<std::string> installs;
};

// Runs fn with std::cout captured and returns what it printed. doctest
// reports to std::cout too, so assertions go after this returns.
template <typename Fn> std::string captured(Fn&& fn) {
    std::ostringstream buf;
    auto* old = std::cout.rdbuf(buf.rdbuf());
    try {
        fn();
    } catch (...) {
        std::cout.rdbuf(old);
        throw;
    }
    std::cout.rdbuf(old);
    return buf.str();
}

Package pkg(const std::string& name, const std::string& version) {
    Package p;
    p.name = name;
    p.version = version;
    return p;
}

// homebrew bucket: jq 1.7.1 (index 1.8.2), the cask firefox 120.0 (index
// 121.0) and the disabled formula oldtool 1.0 (index 1.1).
struct Fixture {
    SsTmp t;
    Config cfg = scratch_config(t.path);
    RecordingProvider brew;
    PackageIndex idx;

    Fixture() {
        Manifest m;
        for (const auto& [name, version] : std::vector<std::pair<std::string, std::string>>{
                 {"jq", "1.7.1"}, {"firefox", "120.0"}, {"oldtool", "1.0"}}) {
            brew.install(cfg, name, version);
            m.packages["homebrew"][name] = version;
        }
        write_manifest(cfg.den_home, active_env_path(cfg.den_home), m);
        brew.installs.clear();
        idx.packages["jq"] = pkg("jq", "1.8.2");
        auto ff = pkg("firefox", "121.0");
        ff.artifact_type = ArtifactType::App;
        idx.packages["firefox"] = ff;
        auto old = pkg("oldtool", "1.1");
        old.disabled = true;
        idx.packages["oldtool"] = old;
    }
};

} // namespace

TEST_SUITE("upgrade_shared_selection") {

    TEST_CASE("bare den upgrade does not pick up a cask or a disabled formula") {
        Fixture f;
        const auto out =
            captured([&] { upgrade_packages(f.cfg, f.brew, f.idx, {}, /*dry_run=*/true); });
        CAPTURE(out);
        CHECK(out.find("Would upgrade 1 package(s):") != std::string::npos);
        CHECK(out.find("jq 1.7.1 -> 1.8.2") != std::string::npos);
        CHECK(out.find("firefox 120.0 -> 121.0") == std::string::npos);
        CHECK(out.find("oldtool 1.0 -> 1.1") == std::string::npos);
        CHECK(out.find("Not upgrading 1 cask(s)") != std::string::npos);
        CHECK(out.find("firefox 120.0 (121.0 available)") != std::string::npos);

        captured([&] { upgrade_packages(f.cfg, f.brew, f.idx, {}); });
        CHECK(f.brew.installs == std::vector<std::string>{"jq@1.8.2"});
    }

    TEST_CASE("bare den upgrade plans exactly what outdated_plan lists") {
        Fixture f;
        const auto plan = outdated_plan(f.cfg, f.idx);
        REQUIRE(plan.pending.size() == 1);
        CHECK(plan.pending[0].name == "jq");
        captured([&] { upgrade_packages(f.cfg, f.brew, f.idx, {}); });
        REQUIRE(f.brew.installs.size() == plan.pending.size());
        CHECK(f.brew.installs[0] == plan.pending[0].name + "@" + plan.pending[0].available);
    }

    TEST_CASE("a named cask is still upgraded explicitly") {
        Fixture f;
        const auto out = captured([&] { upgrade_packages(f.cfg, f.brew, f.idx, {"firefox"}); });
        CAPTURE(out);
        CHECK(f.brew.installs == std::vector<std::string>{"firefox@121.0"});
    }
}
