// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// `den upgrade` (all, named, and --dry-run) needs the package index to know
// what is newer. When the index is missing, empty, or unreadable it used to
// log a message and exit 0, indistinguishable from "everything is up to
// date". require_index() turns that into a UserError, which the CLI prints
// as "error: ..." and exits 1 (the convention from #160).

#include <doctest.h>

#include "cli/install.h"
#include "core/error.h"
#include "index/index.h"
#include "index/package.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;
using namespace den;

namespace {

struct EiTmp {
    fs::path path;
    EiTmp() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("den-emptyindex-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~EiTmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    EiTmp(const EiTmp&) = delete;
    EiTmp& operator=(const EiTmp&) = delete;
};

void check_refused(const PackageIndex& idx) {
    bool threw = false;
    try {
        require_index(idx);
    } catch (const UserError& e) {
        threw = true;
        CHECK(std::string(e.what()) == "the package index is empty; run `den update` first");
    }
    CHECK(threw);
}

} // namespace

TEST_SUITE("upgrade::empty_index") {

    TEST_CASE("empty_index_message names the fix") {
        CHECK(empty_index_message() == "the package index is empty; run `den update` first");
    }

    TEST_CASE("require_index refuses a default-constructed (empty) index") {
        PackageIndex idx;
        REQUIRE(idx.empty());
        check_refused(idx);
    }

    TEST_CASE("require_index refuses an index loaded from a missing file") {
        EiTmp tmp;
        auto idx = load_index(tmp.path / "cache" / "index.json");
        REQUIRE(idx.empty());
        check_refused(idx);
    }

    TEST_CASE("require_index refuses an index file with no packages") {
        EiTmp tmp;
        const auto p = tmp.path / "index.json";
        std::ofstream(p) << R"({"packages": []})";
        auto idx = load_index(p);
        REQUIRE(idx.empty());
        check_refused(idx);
    }

    TEST_CASE("require_index refuses an unparseable index file") {
        EiTmp tmp;
        const auto p = tmp.path / "index.json";
        std::ofstream(p) << "{not json";
        auto idx = load_index(p);
        REQUIRE(idx.empty());
        check_refused(idx);
    }

    TEST_CASE("require_index accepts an index with a package") {
        PackageIndex idx;
        Package pkg;
        pkg.name = "realpkg";
        pkg.version = "1.0.0";
        idx.packages.emplace(pkg.name, pkg);
        REQUIRE_FALSE(idx.empty());
        CHECK_NOTHROW(require_index(idx));
    }
}
