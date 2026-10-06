// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Regression: `den cleanup` shares the Homebrew Cellar, so it must not delete
// a keg den did not install. Ownership is a receipt written at pour time.
// Kegs with no receipt (Homebrew, or den installs from before receipts) stay.

#include <doctest.h>

#include "env/manifest.h"
#include "store/store.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace den {
namespace test {

namespace fs = std::filesystem;

struct TmpDir {
    fs::path path;

    TmpDir() {
        path =
            fs::temp_directory_path() /
            ("den_test_cleanup_" +
             std::to_string(
                 std::hash<std::string>{}(std::to_string(reinterpret_cast<uintptr_t>(this))) ^
                 static_cast<size_t>(std::chrono::steady_clock::now().time_since_epoch().count())));
        fs::create_directories(path);
    }

    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    TmpDir(const TmpDir&) = delete;
    TmpDir& operator=(const TmpDir&) = delete;
};

static void write_file(const fs::path& path, const std::string& body) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    out << body;
}

static void plant_keg(const fs::path& cellar, const std::string& name, const std::string& version,
                      const std::string& marker) {
    write_file(cellar / name / version / marker, marker);
}

TEST_SUITE("cleanup::ownership") {

    TEST_CASE("brew-owned keg survives and a den-owned stale version is removed") {
        TmpDir tmp;
        const auto cellar = tmp.path / "cellar";
        const auto home = tmp.path / "home";
        const auto cache = home / "cache";

        // The keg from the report: Homebrew poured it, den has no record of it.
        plant_keg(cellar, "brew-only-pkg", "1.0", "KEEP");
        write_file(cellar / "brew-only-pkg" / "1.0" / "INSTALL_RECEIPT.json", "{}\n");

        // A den-poured version nothing references anymore.
        plant_keg(cellar, "den-pkg", "1.0", "STALE");
        mark_keg_owned(cellar / "den-pkg" / "1.0", "den-pkg", "1.0");

        // The version an environment still uses.
        plant_keg(cellar, "den-pkg", "2.0", "KEEP");
        mark_keg_owned(cellar / "den-pkg" / "2.0", "den-pkg", "2.0");

        // Poured by den before receipts existed: no marker, must be kept.
        plant_keg(cellar, "den-legacy", "0.9", "KEEP");

        Manifest manifest;
        manifest.packages["homebrew"]["den-pkg"] = "2.0";
        write_manifest(home, "/", manifest);

        write_file(cache / "archives" / "blob.tar.gz", "cached");
        write_file(cache / "index.json", "keep-me");

        auto report = cleanup_kegs(cellar, home, cache, /*dry_run=*/false);

        CHECK(fs::exists(cellar / "brew-only-pkg" / "1.0" / "KEEP"));
        CHECK(fs::exists(cellar / "den-pkg" / "2.0" / "KEEP"));
        CHECK(fs::exists(cellar / "den-legacy" / "0.9" / "KEEP"));
        CHECK_FALSE(fs::exists(cellar / "den-pkg" / "1.0"));
        // The formula dir stays because 2.0 is still installed.
        CHECK(fs::is_directory(cellar / "den-pkg"));
        CHECK(fs::exists(cellar));

        REQUIRE(report.removed.size() == 1);
        CHECK(report.removed[0].name == "den-pkg");
        CHECK(report.removed[0].version == "1.0");
        // brew-only-pkg and den-legacy are the unowned leftovers.
        CHECK(report.kept_unowned == 2);
        CHECK(report.cache_cleared);
        CHECK_FALSE(fs::exists(cache / "archives"));
        CHECK(fs::exists(cache / "index.json"));
    }

    TEST_CASE("dry-run previews the den-owned stale keg and deletes nothing") {
        TmpDir tmp;
        const auto cellar = tmp.path / "cellar";
        const auto home = tmp.path / "home";
        const auto cache = home / "cache";

        plant_keg(cellar, "brew-only-pkg", "1.0", "KEEP");
        plant_keg(cellar, "den-pkg", "1.0", "STALE");
        mark_keg_owned(cellar / "den-pkg" / "1.0", "den-pkg", "1.0");
        write_file(cache / "archives" / "blob.tar.gz", "cached");

        auto report = cleanup_kegs(cellar, home, cache, /*dry_run=*/true);

        CHECK(fs::exists(cellar / "brew-only-pkg" / "1.0" / "KEEP"));
        CHECK(fs::exists(cellar / "den-pkg" / "1.0" / "STALE"));
        CHECK(fs::exists(cellar / "den-pkg" / "1.0" / kDenReceiptFilename));
        CHECK(fs::exists(cache / "archives" / "blob.tar.gz"));
        REQUIRE(report.removed.size() == 1);
        CHECK(report.removed[0].name == "den-pkg");
        CHECK(report.kept_unowned == 1);
        CHECK(report.cache_cleared);
    }

    TEST_CASE("a referenced den keg is kept even when another version is stale") {
        TmpDir tmp;
        const auto cellar = tmp.path / "cellar";
        const auto home = tmp.path / "home";

        plant_keg(cellar, "wget", "1.21.3", "old");
        plant_keg(cellar, "wget", "1.21.4", "new");
        mark_keg_owned(cellar / "wget" / "1.21.3", "wget", "1.21.3");
        mark_keg_owned(cellar / "wget" / "1.21.4", "wget", "1.21.4");

        // Referenced from a child environment, not the root.
        Manifest child;
        child.packages["homebrew"]["wget"] = "1.21.4";
        write_manifest(home, "/work", child);

        auto report = cleanup_kegs(cellar, home, home / "cache", /*dry_run=*/false);

        CHECK_FALSE(fs::exists(cellar / "wget" / "1.21.3"));
        CHECK(fs::exists(cellar / "wget" / "1.21.4" / "new"));
        REQUIRE(report.removed.size() == 1);
        CHECK(report.removed[0].version == "1.21.3");
        CHECK(report.kept_unowned == 0);
    }

    TEST_CASE("removing the last den-owned version drops the empty formula directory") {
        TmpDir tmp;
        const auto cellar = tmp.path / "cellar";
        plant_keg(cellar, "solo", "1.0", "STALE");
        mark_keg_owned(cellar / "solo" / "1.0", "solo", "1.0");

        auto report =
            cleanup_kegs(cellar, tmp.path / "home", tmp.path / "cache", /*dry_run=*/false);

        CHECK_FALSE(fs::exists(cellar / "solo"));
        CHECK(fs::is_directory(cellar));
        CHECK(report.removed.size() == 1);
    }

    TEST_CASE("a receipt that does not name this keg is not ownership") {
        TmpDir tmp;
        const auto cellar = tmp.path / "cellar";
        plant_keg(cellar, "brew-only-pkg", "1.0", "KEEP");
        // Copied from some other keg, and a Homebrew receipt alongside it.
        write_file(
            cellar / "brew-only-pkg" / "1.0" / kDenReceiptFilename,
            "{\n  \"poured_by\": \"den\",\n  \"name\": \"other\",\n  \"version\": \"9.9\"\n}\n");
        write_file(cellar / "brew-only-pkg" / "1.0" / "INSTALL_RECEIPT.json",
                   "{\"installed_on_request\": true}\n");

        CHECK_FALSE(keg_owned_by_den(cellar / "brew-only-pkg" / "1.0"));

        auto report =
            cleanup_kegs(cellar, tmp.path / "home", tmp.path / "cache", /*dry_run=*/false);
        CHECK(fs::exists(cellar / "brew-only-pkg" / "1.0" / "KEEP"));
        CHECK(report.removed.empty());
        CHECK(report.kept_unowned == 1);
    }

    TEST_CASE("malformed, foreign, and symlinked receipts are not ownership") {
        TmpDir tmp;
        const auto cellar = tmp.path / "cellar";

        plant_keg(cellar, "broken", "1.0", "KEEP");
        write_file(cellar / "broken" / "1.0" / kDenReceiptFilename, "{ not json");

        plant_keg(cellar, "foreign", "1.0", "KEEP");
        write_file(cellar / "foreign" / "1.0" / kDenReceiptFilename,
                   "{\n  \"poured_by\": \"homebrew\",\n  \"name\": \"foreign\",\n  \"version\": "
                   "\"1.0\"\n}\n");

        plant_keg(cellar, "linked", "1.0", "KEEP");
        const auto real_receipt = tmp.path / "elsewhere.json";
        write_file(
            real_receipt,
            "{\n  \"poured_by\": \"den\",\n  \"name\": \"linked\",\n  \"version\": \"1.0\"\n}\n");
        fs::create_symlink(real_receipt, cellar / "linked" / "1.0" / kDenReceiptFilename);

        CHECK_FALSE(keg_owned_by_den(cellar / "broken" / "1.0"));
        CHECK_FALSE(keg_owned_by_den(cellar / "foreign" / "1.0"));
        CHECK_FALSE(keg_owned_by_den(cellar / "linked" / "1.0"));

        auto report =
            cleanup_kegs(cellar, tmp.path / "home", tmp.path / "cache", /*dry_run=*/false);
        CHECK(fs::exists(cellar / "broken" / "1.0" / "KEEP"));
        CHECK(fs::exists(cellar / "foreign" / "1.0" / "KEEP"));
        CHECK(fs::exists(cellar / "linked" / "1.0" / "KEEP"));
        CHECK(report.removed.empty());
        CHECK(report.kept_unowned == 3);
    }

    TEST_CASE("mark_keg_owned writes a receipt cleanup will honour") {
        TmpDir tmp;
        const auto keg = tmp.path / "cellar" / "tree" / "2.1.1";
        plant_keg(tmp.path / "cellar", "tree", "2.1.1", "bin");

        CHECK_FALSE(keg_owned_by_den(keg));
        mark_keg_owned(keg, "tree", "2.1.1");
        CHECK(keg_owned_by_den(keg));
        CHECK(fs::is_regular_file(keg / kDenReceiptFilename));

        // Missing directory is a no-op, not an error.
        mark_keg_owned(tmp.path / "no-such-keg", "tree", "2.1.1");
        CHECK_FALSE(keg_owned_by_den(tmp.path / "no-such-keg"));
    }

    TEST_CASE("an unowned keg that an environment references is not counted as removable") {
        TmpDir tmp;
        const auto cellar = tmp.path / "cellar";
        const auto home = tmp.path / "home";
        plant_keg(cellar, "brew-only-pkg", "1.0", "KEEP");

        Manifest manifest;
        manifest.packages["homebrew"]["brew-only-pkg"] = "1.0";
        write_manifest(home, "/", manifest);

        auto report = cleanup_kegs(cellar, home, home / "cache", /*dry_run=*/false);
        CHECK(fs::exists(cellar / "brew-only-pkg" / "1.0" / "KEEP"));
        CHECK(report.removed.empty());
        CHECK(report.kept_unowned == 0);
        CHECK_FALSE(report.cache_cleared);
    }
}

} // namespace test
} // namespace den
