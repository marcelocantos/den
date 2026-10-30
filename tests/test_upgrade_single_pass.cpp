// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// `den upgrade` with no names makes exactly one upgrade pass. upgrade_packages
// already scans every installed package, so the CLI once calling it per
// manifest provider bucket planned and dispatched the same upgrade-all again
// for each further bucket (through that bucket's provider).

#include <doctest.h>

#include "cli/install.h"
#include "env/environment.h"
#include "env/manifest.h"
#include "index/package.h"
#include "provider/registry.h"
#include "provider/stub_provider.h"

#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace den;

namespace {

struct UspTmp {
    fs::path path;
    UspTmp() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("den-upgrade-single-pass-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~UspTmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    UspTmp(const UspTmp&) = delete;
    UspTmp& operator=(const UspTmp&) = delete;
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

// A stub provider under a chosen name that records every install it is asked
// to do, so a test can count how often an upgrade reached each provider.
class CountingProvider : public StubProvider {
  public:
    explicit CountingProvider(std::string name) : name_(std::move(name)) {}
    std::string_view provider_name() const override { return name_; }
    InstallResult install(const Config& config, std::string_view name,
                          std::string_view version_hint) override {
        installs.push_back(std::string(name) + "@" + std::string(version_hint));
        return StubProvider::install(config, name, version_hint);
    }
    std::vector<std::string> installs;

  private:
    std::string name_;
};

struct CoutCapture {
    std::ostringstream buf;
    std::streambuf* old;
    CoutCapture() : old(std::cout.rdbuf(buf.rdbuf())) {}
    ~CoutCapture() { std::cout.rdbuf(old); }
    std::string str() const { return buf.str(); }
};

size_t count(const std::string& haystack, const std::string& needle) {
    size_t n = 0;
    for (auto pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size()))
        ++n;
    return n;
}

struct Fixture {
    UspTmp t;
    Config cfg = scratch_config(t.path);
    std::shared_ptr<CountingProvider> primary = std::make_shared<CountingProvider>("primary");
    std::shared_ptr<CountingProvider> other = std::make_shared<CountingProvider>("other");
    ProviderRegistry registry;
    PackageIndex idx;

    // Two non-empty provider buckets: the case that used to run two passes.
    Fixture() {
        registry.add(primary); // first added: the registry's default
        registry.add(other);
        Manifest m;
        primary->install(cfg, "jq", "1.7.1");
        m.packages["primary"]["jq"] = "1.7.1";
        other->install(cfg, "tool", "1.0");
        m.packages["other"]["tool"] = "1.0";
        write_manifest(cfg.den_home, active_env_path(cfg.den_home), m);
        primary->installs.clear();
        other->installs.clear();

        Package p;
        p.name = "jq";
        p.version = "1.8.2";
        idx.packages[p.name] = p;
    }

    std::string active_version(const std::string& name) const {
        auto r = resolve(cfg.den_home, active_env_path(cfg.den_home));
        auto it = r.find(name);
        return it == r.end() ? "" : it->second;
    }
};

} // namespace

TEST_SUITE("upgrade_single_pass") {

    TEST_CASE("den upgrade (all) with two provider buckets makes exactly one pass") {
        Fixture f;
        CoutCapture cap;
        upgrade_all_packages(f.cfg, f.registry, f.idx);
        const auto out = cap.str();

        CHECK(f.active_version("jq") == "1.8.2");
        CHECK(f.active_version("tool") == "1.0");
        // One plan, one apply, one summary: a second pass would print its own
        // "All packages are up to date." after the first had upgraded jq.
        CHECK(count(out, "package(s) to upgrade:") == 1);
        CHECK(count(out, "package(s) upgraded.") == 1);
        CHECK(count(out, "All packages are up to date.") == 0);
        // Installed once, through one provider; the other bucket's provider
        // is not handed the same upgrade again.
        CHECK(f.primary->installs == std::vector<std::string>{"jq@1.8.2"});
        CHECK(f.other->installs.empty());
    }

    TEST_CASE("den upgrade --dry-run (all) prints one plan") {
        Fixture f;
        CoutCapture cap;
        upgrade_all_packages(f.cfg, f.registry, f.idx, /*dry_run=*/true);
        const auto out = cap.str();

        CHECK(count(out, "Would upgrade 1 package(s):") == 1);
        CHECK(count(out, "Dry run: nothing changed.") == 1);
        CHECK(f.active_version("jq") == "1.7.1");
        CHECK(f.primary->installs.empty());
        CHECK(f.other->installs.empty());
    }

    TEST_CASE("den upgrade (all) prefers the homebrew provider over the default") {
        Fixture f;
        auto brew = std::make_shared<CountingProvider>("homebrew");
        f.registry.add(brew);
        CoutCapture cap;
        upgrade_all_packages(f.cfg, f.registry, f.idx);

        CHECK(brew->installs == std::vector<std::string>{"jq@1.8.2"});
        CHECK(f.primary->installs.empty());
        CHECK(f.other->installs.empty());
        CHECK(count(cap.str(), "package(s) to upgrade:") == 1);
    }

    TEST_CASE("den upgrade (all) still runs its one pass when the active bucket set is empty") {
        // Old dispatch skipped empty buckets and so made zero passes for an
        // environment whose packages all come from its parent chain; the one
        // pass scans the resolved environment either way.
        UspTmp t;
        auto cfg = scratch_config(t.path);
        auto prov = std::make_shared<CountingProvider>("primary");
        ProviderRegistry registry;
        registry.add(prov);
        write_manifest(cfg.den_home, active_env_path(cfg.den_home), Manifest{});
        PackageIndex idx;
        CoutCapture cap;
        upgrade_all_packages(cfg, registry, idx);
        CHECK(count(cap.str(), "All packages are up to date.") == 1);
        CHECK(prov->installs.empty());
    }

    TEST_CASE("den upgrade (all) warns about a bucket under an unregistered provider") {
        Fixture f;
        Manifest m = read_manifest(f.cfg.den_home, active_env_path(f.cfg.den_home));
        m.packages["ghost"]["thing"] = "1.0";
        write_manifest(f.cfg.den_home, active_env_path(f.cfg.den_home), m);

        std::ostringstream err;
        auto* old_err = std::cerr.rdbuf(err.rdbuf());
        CoutCapture cap;
        upgrade_all_packages(f.cfg, f.registry, f.idx);
        std::cerr.rdbuf(old_err);

        CHECK(err.str().find("provider 'ghost'") != std::string::npos);
        CHECK(count(cap.str(), "package(s) to upgrade:") == 1);
        CHECK(f.primary->installs == std::vector<std::string>{"jq@1.8.2"});
    }
}
