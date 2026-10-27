// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Regression: `den uninstall` keeps the keg until `den cleanup`, but it must
// not leave <prefix>/opt/<name> advertising a version no environment uses.
// The link is handed to another installed, den-owned, still-referenced
// version, or removed. Links den does not own (Homebrew's) are never touched.

#include <doctest.h>

#include "env/manifest.h"
#include "store/link.h"
#include "store/store.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

namespace den {
namespace test {

namespace fs = std::filesystem;

namespace {

struct UninstallTmp {
    fs::path path;
    UninstallTmp() {
        path = fs::temp_directory_path() /
               ("den_test_uninstall_opt_" +
                std::to_string(static_cast<size_t>(
                    std::chrono::steady_clock::now().time_since_epoch().count())));
        fs::create_directories(path);
    }
    ~UninstallTmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    UninstallTmp(const UninstallTmp&) = delete;
    UninstallTmp& operator=(const UninstallTmp&) = delete;
};

// <tmp>/prefix/{Cellar,opt}, <tmp>/home (den_home).
struct Layout {
    UninstallTmp tmp;
    fs::path prefix = tmp.path / "prefix";
    fs::path cellar = prefix / "Cellar";
    fs::path opt = prefix / "opt";
    fs::path home = tmp.path / "home";

    fs::path keg(const std::string& name, const std::string& version, bool den_owned = true) {
        const auto k = cellar / name / version;
        fs::create_directories(k / "bin");
        std::ofstream(k / "bin" / name) << "#!/bin/sh\n";
        if (den_owned) {
            mark_keg_owned(k, name, version);
        }
        return k;
    }

    void env(const std::string& env_path, const std::map<std::string, std::string>& pkgs) {
        Manifest m;
        for (const auto& [n, v] : pkgs) {
            m.packages["homebrew"][n] = v;
        }
        write_manifest(home, env_path, m);
    }
};

} // namespace

TEST_SUITE("uninstall::opt_link") {

    TEST_CASE("opt link is removed when no other version is installed") {
        Layout l;
        const auto keg = l.keg("automake", "1.19");
        REQUIRE(ensure_prefix_opt_link(l.cellar, "automake", keg) == OptLinkAction::Created);
        l.env("/", {}); // automake already dropped from the manifest

        CHECK(release_prefix_opt_link(l.cellar, l.home, "automake") == OptReleaseAction::Removed);
        CHECK_FALSE(fs::is_symlink(l.opt / "automake"));
        // The keg itself stays until `den cleanup`.
        CHECK(fs::is_directory(keg));
    }

    TEST_CASE("opt link is handed to another installed, referenced version") {
        Layout l;
        const auto v1 = l.keg("node", "24.0.0");
        const auto v2 = l.keg("node", "25.1.0");
        REQUIRE(ensure_prefix_opt_link(l.cellar, "node", v2) == OptLinkAction::Created);
        // Root dropped node 25; a child env still uses node 24.
        l.env("/", {});
        l.env("/work", {{"node", "24.0.0"}});

        std::string to;
        CHECK(release_prefix_opt_link(l.cellar, l.home, "node", &to) ==
              OptReleaseAction::HandedOff);
        CHECK(to == "24.0.0");
        CHECK(fs::read_symlink(l.opt / "node") == fs::path("../Cellar/node/24.0.0"));
        CHECK(fs::equivalent(l.opt / "node", v1));
        CHECK(fs::is_directory(v2));
    }

    TEST_CASE("an unreferenced or Homebrew-owned version is not a hand-off target") {
        Layout l;
        l.keg("node", "23.0.0");                      // den-owned, awaiting cleanup
        l.keg("node", "24.0.0", /*den_owned=*/false); // Homebrew's
        const auto v3 = l.keg("node", "25.1.0");
        REQUIRE(ensure_prefix_opt_link(l.cellar, "node", v3) == OptLinkAction::Created);
        l.env("/", {});

        CHECK(release_prefix_opt_link(l.cellar, l.home, "node") == OptReleaseAction::Removed);
        CHECK_FALSE(fs::is_symlink(l.opt / "node"));
    }

    TEST_CASE("a Homebrew-owned opt link is never touched") {
        Layout l;
        const auto brew = l.keg("libgit2", "1.8.4", /*den_owned=*/false);
        l.keg("libgit2", "1.9.7_2"); // den's, just uninstalled
        fs::create_directories(l.opt);
        fs::create_symlink("../Cellar/libgit2/1.8.4", l.opt / "libgit2");
        l.env("/", {});

        CHECK(release_prefix_opt_link(l.cellar, l.home, "libgit2") == OptReleaseAction::NotOwned);
        CHECK(fs::read_symlink(l.opt / "libgit2") == fs::path("../Cellar/libgit2/1.8.4"));
        CHECK(fs::equivalent(l.opt / "libgit2", brew));

        // Nor is a real directory, a link outside the store, or a dangling link.
        fs::create_directories(l.opt / "realdir");
        CHECK(release_prefix_opt_link(l.cellar, l.home, "realdir") == OptReleaseAction::NotOwned);
        CHECK(fs::is_directory(l.opt / "realdir"));

        const auto elsewhere = l.tmp.path / "elsewhere" / "foo" / "1.0";
        fs::create_directories(elsewhere);
        mark_keg_owned(elsewhere, "foo", "1.0");
        fs::create_symlink(elsewhere, l.opt / "foo");
        CHECK(release_prefix_opt_link(l.cellar, l.home, "foo") == OptReleaseAction::NotOwned);
        CHECK(fs::is_symlink(l.opt / "foo"));

        fs::create_symlink("../Cellar/gone/1.0", l.opt / "gone");
        CHECK(release_prefix_opt_link(l.cellar, l.home, "gone") == OptReleaseAction::NotOwned);
        CHECK(fs::is_symlink(l.opt / "gone"));
    }

    TEST_CASE("opt link stays while another env still references the version") {
        Layout l;
        const auto keg = l.keg("automake", "1.19");
        REQUIRE(ensure_prefix_opt_link(l.cellar, "automake", keg) == OptLinkAction::Created);
        l.env("/", {});                       // uninstalled here
        l.env("/ml", {{"automake", "1.19"}}); // still used here

        CHECK(release_prefix_opt_link(l.cellar, l.home, "automake") == OptReleaseAction::Kept);
        CHECK(fs::read_symlink(l.opt / "automake") == fs::path("../Cellar/automake/1.19"));
    }

    TEST_CASE("cleanup still reclaims the uninstalled keg after the opt link is released") {
        Layout l;
        const auto keg = l.keg("automake", "1.19");
        const auto dep = l.keg("m4", "1.4.21");
        REQUIRE(ensure_prefix_opt_link(l.cellar, "automake", keg) == OptLinkAction::Created);
        REQUIRE(ensure_prefix_opt_link(l.cellar, "m4", dep) == OptLinkAction::Created);
        l.env("/", {{"m4", "1.4.21"}});

        REQUIRE(release_prefix_opt_link(l.cellar, l.home, "automake") == OptReleaseAction::Removed);
        auto report = cleanup_kegs(l.cellar, l.home, l.home / "cache", /*dry_run=*/false);
        REQUIRE(report.removed.size() == 1);
        CHECK(report.removed[0].name == "automake");
        CHECK_FALSE(fs::exists(keg));
        CHECK_FALSE(fs::exists(l.cellar / "automake")); // empty formula dir too
        CHECK_FALSE(fs::is_symlink(l.opt / "automake"));
        // The still-referenced dependency and its link are untouched.
        CHECK(fs::is_directory(dep));
        CHECK(fs::read_symlink(l.opt / "m4") == fs::path("../Cellar/m4/1.4.21"));
    }
}

} // namespace test
} // namespace den
