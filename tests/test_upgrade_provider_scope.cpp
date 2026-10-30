// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// `den upgrade` compares a package only against the index of its own
// provider. The package index describes Homebrew formulae, so a package the
// pip provider manages (cowsay 6.1) is not the index's cowsay (3.8.4). The
// CLI used to flatten every provider bucket into one name -> version map and
// compare all of it against the index: pip's cowsay 6.1 came out as "index
// older than installed", and a pip package older than a same-named formula
// would have been "upgraded" by installing the formula. The daemon's
// maintenance cycle already read only the homebrew bucket, so the two
// disagreed about index_older.

#include <doctest.h>

#include "cli/install.h"
#include "daemon/daemon.h"
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

struct PsTmp {
    fs::path path;
    PsTmp() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("den-upgrade-provider-scope-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~PsTmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    PsTmp(const PsTmp&) = delete;
    PsTmp& operator=(const PsTmp&) = delete;
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

class NamedProvider : public StubProvider {
  public:
    explicit NamedProvider(std::string name) : name_(std::move(name)) {}
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

Package pkg(const std::string& name, const std::string& version) {
    Package p;
    p.name = name;
    p.version = version;
    return p;
}

// homebrew: jq 1.7.1 (index 1.8.2); pip: cowsay at `cowsay_version` (index
// has the unrelated formula cowsay 3.8.4).
struct Fixture {
    PsTmp t;
    Config cfg = scratch_config(t.path);
    NamedProvider brew{"homebrew"};
    PackageIndex idx;

    explicit Fixture(const std::string& cowsay_version) {
        Manifest m;
        brew.install(cfg, "jq", "1.7.1");
        m.packages["homebrew"]["jq"] = "1.7.1";
        m.packages["pip"]["cowsay"] = cowsay_version;
        write_manifest(cfg.den_home, active_env_path(cfg.den_home), m);
        brew.installs.clear();
        idx.packages["jq"] = pkg("jq", "1.8.2");
        idx.packages["cowsay"] = pkg("cowsay", "3.8.4");
    }

    std::string version_in(const std::string& provider, const std::string& name) const {
        auto per = resolve_per_provider(cfg.den_home, active_env_path(cfg.den_home));
        auto b = per.find(provider);
        if (b == per.end())
            return "";
        auto it = b->second.find(name);
        return it == b->second.end() ? "" : it->second;
    }
};

} // namespace

TEST_SUITE("upgrade_provider_scope") {

    TEST_CASE("dry-run does not report a pip package as index-older") {
        Fixture f("6.1");
        CoutCapture cap;
        upgrade_packages(f.cfg, f.brew, f.idx, {}, /*dry_run=*/true);
        const auto out = cap.str();
        CAPTURE(out);
        CHECK(out.find("cowsay") == std::string::npos);
        CHECK(out.find("Not downgrading") == std::string::npos);
        CHECK(out.find("Would upgrade 1 package(s):") != std::string::npos);
        CHECK(out.find("jq 1.7.1 -> 1.8.2") != std::string::npos);
    }

    TEST_CASE("a pip package older than a same-named formula is not upgraded to the formula") {
        Fixture f("1.0");
        CoutCapture cap;
        upgrade_packages(f.cfg, f.brew, f.idx, {});
        CHECK(f.brew.installs == std::vector<std::string>{"jq@1.8.2"});
        CHECK(f.version_in("pip", "cowsay") == "1.0");
        CHECK(f.version_in("homebrew", "cowsay").empty());
        CHECK(f.version_in("homebrew", "jq") == "1.8.2");
    }

    TEST_CASE("CLI plan and daemon cycle agree on index_older for the same environment") {
        Fixture f("6.1");
        // The daemon's view: the homebrew bucket only (make_live_hooks).
        auto per = resolve_per_provider(f.cfg.den_home, active_env_path(f.cfg.den_home));
        std::vector<PendingUpgrade> older;
        auto pending = compute_pending(per["homebrew"], f.idx, &older);
        CHECK(older.empty());
        REQUIRE(pending.size() == 1);
        CHECK(pending[0].name == "jq");

        // The CLI's view of the same environment reports no index-older entry
        // either, and plans the same single upgrade.
        CoutCapture cap;
        upgrade_packages(f.cfg, f.brew, f.idx, {}, /*dry_run=*/true);
        CHECK(cap.str().find("index older than installed") == std::string::npos);
        CHECK(cap.str().find("Would upgrade 1 package(s):") != std::string::npos);
    }

    TEST_CASE("a homebrew package with an older index is still reported") {
        Fixture f("6.1");
        Manifest m = read_manifest(f.cfg.den_home, active_env_path(f.cfg.den_home));
        m.packages["homebrew"]["cowsay"] = "3.9.0";
        write_manifest(f.cfg.den_home, active_env_path(f.cfg.den_home), m);
        CoutCapture cap;
        upgrade_packages(f.cfg, f.brew, f.idx, {}, /*dry_run=*/true);
        CHECK(cap.str().find("cowsay 3.9.0 (index has 3.8.4") != std::string::npos);
    }
}
