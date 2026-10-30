// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// `den outdated` works from the active environment's manifest, per provider
// (#171), not the keg store. outdated_plan runs the homebrew bucket through
// compute_pending, the plan the daemon, `den upgrade` and `den doctor` use,
// lists other providers' packages as untracked, and ignores kegs no
// manifest records (a stray cowsay 3.04_1, an older jq keg).

#include "../src/daemon/daemon.h"
#include "../src/doctor/doctor.h"

#include <doctest.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace den {
namespace test_outdated_plan {

namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "den_outdated_plan_XXXXXX").string();
        char* result = ::mkdtemp(tmpl.data());
        if (!result)
            throw std::runtime_error(std::string("mkdtemp failed: ") + std::strerror(errno));
        path = result;
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

Package formula(const std::string& name, const std::string& version, int scheme = 0) {
    Package p;
    p.name = name;
    p.version = version;
    p.version_scheme = scheme;
    p.artifact_type = ArtifactType::Binary;
    return p;
}

Config make_config(const fs::path& root) {
    Config c;
    c.den_home = root / "home";
    c.prefix = root / "pfx";
    c.store = c.prefix / "Cellar";
    c.cache = c.den_home / "cache";
    return c;
}

void keg(const Config& c, const std::string& name, const std::string& version, int scheme = 0) {
    const auto k = c.store / name / version;
    fs::create_directories(k / "bin");
    std::ofstream(k / "INSTALL_RECEIPT.json") << R"({"source":{"versions":{"stable":")" << version
                                              << R"(","version_scheme":)" << scheme << "}}}";
}

void manifest(const Config& c, const std::string& packages_json) {
    fs::create_directories(c.den_home / "manifests" / "ROOT");
    std::ofstream(c.den_home / "manifests" / "ROOT" / "manifest.json")
        << R"({"packages":)" << packages_json << "}";
}

TEST_SUITE("outdated::plan") {

    TEST_CASE("stray and older kegs are ignored; pip cowsay is untracked; jq listed once") {
        TempDir t;
        auto c = make_config(t.path);
        manifest(c, R"({"homebrew":{"jq":"1.6"},"pip":{"cowsay":"6.1"}})");
        keg(c, "jq", "1.5");        // older keg kept beside the current one
        keg(c, "jq", "1.6");        // the version the manifest records
        keg(c, "cowsay", "3.04_1"); // stray keg no environment uses
        PackageIndex idx;
        idx.packages["jq"] = formula("jq", "1.7.1");
        idx.packages["cowsay"] = formula("cowsay", "3.8.4");

        const auto plan = outdated_plan(c, idx);
        CHECK(plan.homebrew == 1);
        REQUIRE(plan.pending.size() == 1);
        CHECK(plan.pending[0].name == "jq");
        CHECK(plan.pending[0].installed == "1.6");
        CHECK(plan.pending[0].available == "1.7.1");
        CHECK(plan.index_older.empty());
        CHECK(plan.held.empty());
        REQUIRE(plan.untracked.size() == 1);
        CHECK(plan.untracked[0].name == "cowsay");
        CHECK(plan.untracked[0].version == "6.1");
        CHECK(plan.untracked[0].provider == "pip");
    }

    TEST_CASE("homebrew bucket uses the daemon's compute_pending: held and index-older") {
        TempDir t;
        auto c = make_config(t.path);
        manifest(c, R"({"homebrew":{"openssl@3":"3.6.3","tree":"2.3.2","wget":"1.25.0"}})");
        keg(c, "openssl@3", "3.6.3", 0);
        keg(c, "tree", "2.3.2");
        keg(c, "wget", "1.25.0");
        PackageIndex idx;
        idx.packages["openssl@3"] = formula("openssl@3", "3.5.9", 1); // held
        idx.packages["tree"] = formula("tree", "2.2.1");              // index older
        idx.packages["wget"] = formula("wget", "1.25.0");             // current

        const auto plan = outdated_plan(c, idx);
        CHECK(plan.homebrew == 3);
        CHECK(plan.pending.empty());
        REQUIRE(plan.held.size() == 1);
        CHECK(plan.held[0].name == "openssl@3");
        CHECK(plan.held[0].reason ==
              "held: index pins 3.5.9 (version_scheme 1), below installed 3.6.3");
        REQUIRE(plan.index_older.size() == 1);
        CHECK(plan.index_older[0].name == "tree");
        CHECK(plan.untracked.empty());

        // Same result as feeding the bucket to compute_pending directly, and
        // as doctor's held-package check.
        std::vector<PendingUpgrade> older;
        std::vector<DeferredUpgrade> held;
        const auto pending = compute_pending(
            {{"openssl@3", "3.6.3"}, {"tree", "2.3.2"}, {"wget", "1.25.0"}}, idx, &older,
            [&c](const std::string& n, const std::string& v) {
                return installed_version_scheme(c.store / n / v);
            },
            &held);
        CHECK(pending.size() == plan.pending.size());
        CHECK(older.size() == plan.index_older.size());
        CHECK(held.size() == plan.held.size());
        CHECK(held_packages(c, idx).size() == plan.held.size());
    }

    TEST_CASE("kegs with no manifest at all are not installed packages") {
        TempDir t;
        auto c = make_config(t.path);
        keg(c, "cowsay", "3.04_1");
        PackageIndex idx;
        idx.packages["cowsay"] = formula("cowsay", "3.8.4");
        const auto plan = outdated_plan(c, idx);
        CHECK(plan.homebrew == 0);
        CHECK(plan.pending.empty());
        CHECK(plan.untracked.empty());
    }

    TEST_CASE("untracked packages from several providers are sorted by name") {
        TempDir t;
        auto c = make_config(t.path);
        manifest(c,
                 R"({"pip":{"cowsay":"6.1"},"npm":{"prettier":"3.3.3"},"cargo":{"bat":"0.24.0"}})");
        PackageIndex idx;
        idx.packages["cowsay"] = formula("cowsay", "3.8.4");
        const auto plan = outdated_plan(c, idx);
        CHECK(plan.homebrew == 0);
        CHECK(plan.pending.empty());
        REQUIRE(plan.untracked.size() == 3);
        CHECK(plan.untracked[0].name == "bat");
        CHECK(plan.untracked[0].provider == "cargo");
        CHECK(plan.untracked[1].name == "cowsay");
        CHECK(plan.untracked[2].name == "prettier");
        CHECK(plan.untracked[2].provider == "npm");
    }

} // TEST_SUITE

} // namespace test_outdated_plan
} // namespace den
