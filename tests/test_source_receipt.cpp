// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Source-build receipt (docs/source-builds.md, "Receipt"): the index carries
// the pinned formula source, the receipt records a build's verified inputs,
// and doctor lists source-built kegs and flags pending rebuilds.

#include <doctest.h>

#include "build/source_build.h"
#include "doctor/doctor.h"
#include "index/index.h"
#include "store/store.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace den {
namespace test {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

struct ReceiptTmpDir {
    fs::path path;
    ReceiptTmpDir() {
        path = fs::temp_directory_path() /
               ("den_test_receipt_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~ReceiptTmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

const std::string kCommit = "0123456789abcdef0123456789abcdef01234567";
const std::string kRubySha(64, 'd');
const std::string kSrcSha(64, 'c');

json formula_json(const std::string& name, json extra = json::object()) {
    json f = {
        {"name", name},
        {"versions", {{"stable", "1.0"}}},
        {"urls",
         {{"stable", {{"url", "https://example.com/" + name + ".tar.gz"}, {"checksum", kSrcSha}}}}},
        {"ruby_source_path", "Formula/" + name.substr(0, 1) + "/" + name + ".rb"},
    };
    f.update(extra);
    return f;
}

SourceBuildReceipt sample_receipt() {
    SourceBuildReceipt r;
    r.source = {"", "https://example.com/tree-1.0.tar.gz", kSrcSha};
    r.resources = {{"extra", "https://example.com/extra.tar.gz", std::string(64, 'e')}};
    r.patches = {{"", "https://example.com/fix.patch", std::string(64, 'f')}};
    r.formula_tap = "homebrew/core";
    r.formula_commit = kCommit;
    r.formula_path = "Formula/t/tree.rb";
    r.toolchain_sdk = "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk";
    r.toolchain_clang = "Apple clang version 17.0.0";
    r.opt_prefix_baked = "/tmp/prefix/opt/tree";
    return r;
}

} // namespace

TEST_SUITE("source_receipt::index") {

    TEST_CASE("index carries tap_git_head and the formula's ruby source sha256") {
        auto f = formula_json(
            "tree", {{"tap_git_head", kCommit}, {"ruby_source_checksum", {{"sha256", kRubySha}}}});
        auto idx = transform_homebrew(json::array({f}).dump(), "[]");
        const auto* pkg = idx.find("tree");
        REQUIRE(pkg);
        CHECK(pkg->tap_git_head == kCommit);
        CHECK(pkg->ruby_source_sha256 == kRubySha);
        CHECK(formula_source_url(*pkg) ==
              "https://raw.githubusercontent.com/Homebrew/homebrew-core/" + kCommit +
                  "/Formula/t/tree.rb");
    }

    TEST_CASE("malformed pins are dropped, and the fetch falls back to master") {
        auto f = formula_json("tree", {{"tap_git_head", "main; rm -rf /"},
                                       {"ruby_source_checksum", {{"sha256", "abc"}}}});
        auto idx = transform_homebrew(json::array({f}).dump(), "[]");
        const auto* pkg = idx.find("tree");
        REQUIRE(pkg);
        CHECK_FALSE(pkg->tap_git_head);
        CHECK_FALSE(pkg->ruby_source_sha256);
        CHECK(formula_source_url(*pkg) ==
              "https://raw.githubusercontent.com/Homebrew/homebrew-core/master/Formula/t/tree.rb");
    }

    TEST_CASE("pins survive the index JSON cache and the binary cache") {
        ReceiptTmpDir tmp;
        auto f = formula_json(
            "tree", {{"tap_git_head", kCommit}, {"ruby_source_checksum", {{"sha256", kRubySha}}}});
        auto idx = transform_homebrew(json::array({f}).dump(), "[]");
        auto path = tmp.path / "index.json";
        save_index(idx, path);
        for (auto loaded : {load_index_json(path), load_index(path)}) {
            const auto* pkg = loaded.find("tree");
            REQUIRE(pkg);
            CHECK(pkg->tap_git_head == kCommit);
            CHECK(pkg->ruby_source_sha256 == kRubySha);
        }
    }
}

TEST_SUITE("source_receipt::store") {

    TEST_CASE("a source build's receipt round-trips") {
        ReceiptTmpDir tmp;
        auto keg = tmp.path / "tree/1.0";
        fs::create_directories(keg);
        const auto r = sample_receipt();
        mark_keg_owned(keg, "tree", "1.0", 0, &r);

        std::ifstream in(keg / "DEN_RECEIPT.json");
        auto j = json::parse(in);
        CHECK(j["poured_by"] == "den");
        CHECK(j["built_from_source"] == true);
        CHECK(j["source"]["sha256"] == kSrcSha);
        CHECK(j["source"]["resources"][0]["name"] == "extra");
        CHECK(j["source"]["patches"][0]["url"] == "https://example.com/fix.patch");
        CHECK(j["formula"]["commit"] == kCommit);
        CHECK(j["toolchain"]["clang"] == "Apple clang version 17.0.0");
        CHECK(j["opt_prefix_baked"] == "/tmp/prefix/opt/tree");

        auto back = read_source_build_receipt(keg);
        REQUIRE(back);
        CHECK(back->source.url == r.source.url);
        CHECK(back->source.sha256 == r.source.sha256);
        REQUIRE(back->resources.size() == 1);
        CHECK(back->resources[0].name == "extra");
        CHECK(back->resources[0].sha256 == std::string(64, 'e'));
        REQUIRE(back->patches.size() == 1);
        CHECK(back->patches[0].sha256 == std::string(64, 'f'));
        CHECK(back->formula_tap == "homebrew/core");
        CHECK(back->formula_commit == kCommit);
        CHECK(back->formula_path == "Formula/t/tree.rb");
        CHECK(back->toolchain_sdk == r.toolchain_sdk);
        CHECK(back->opt_prefix_baked == r.opt_prefix_baked);
    }

    TEST_CASE("a poured keg has no source-build receipt") {
        ReceiptTmpDir tmp;
        auto keg = tmp.path / "tree/1.0";
        fs::create_directories(keg);
        mark_keg_owned(keg, "tree", "1.0");
        CHECK(keg_owned_by_den(keg));
        CHECK_FALSE(read_source_build_receipt(keg));
        CHECK_FALSE(read_source_build_receipt(tmp.path / "missing/1.0"));
    }

    TEST_CASE("den_build.rb result lines become receipt inputs") {
        SourceBuildReceipt r;
        parse_ruby_build_inputs("name=tree\nversion=1.0\nprefix=/x\n"
                                "source=https://e.com/t.tgz\t" +
                                    kSrcSha +
                                    "\n"
                                    "resource=extra\thttps://e.com/x.tgz\t" +
                                    std::string(64, 'e') +
                                    "\r\n"
                                    "patch=https://e.com/p.diff\t" +
                                    std::string(64, 'f') + "\nstatus=ok\n",
                                r);
        CHECK(r.source.url == "https://e.com/t.tgz");
        CHECK(r.source.sha256 == kSrcSha);
        REQUIRE(r.resources.size() == 1);
        CHECK(r.resources[0].name == "extra");
        CHECK(r.resources[0].sha256 == std::string(64, 'e'));
        REQUIRE(r.patches.size() == 1);
        CHECK(r.patches[0].url == "https://e.com/p.diff");
    }
}

TEST_SUITE("source_receipt::doctor") {

    TEST_CASE("doctor lists source-built kegs and flags a changed source") {
        ReceiptTmpDir tmp;
        auto store = tmp.path / "Cellar";
        const auto r = sample_receipt();
        for (const auto& [name, version] :
             std::vector<std::pair<std::string, std::string>>{{"tree", "1.0"}, {"jq", "1.7"}}) {
            fs::create_directories(store / name / version);
            mark_keg_owned(store / name / version, name, version, 0, &r);
        }
        fs::create_directories(store / "poured/2.0");
        mark_keg_owned(store / "poured/2.0", "poured", "2.0");

        // tree: same version, different tarball sha256 -> pending rebuild.
        // jq: the index moved on to 1.8 -> an upgrade, not a rebuild.
        auto tree = formula_json("tree");
        tree["urls"]["stable"]["checksum"] = std::string(64, '9');
        auto jq = formula_json("jq");
        jq["versions"]["stable"] = "1.8";
        tree["tap_git_head"] = kCommit;
        auto idx = transform_homebrew(json::array({tree, jq}).dump(), "[]");

        auto kegs = source_built_kegs(store, &idx);
        REQUIRE(kegs.size() == 2);
        CHECK(kegs[0].name == "jq");
        CHECK(kegs[0].pending_rebuild.empty());
        CHECK(kegs[1].name == "tree");
        CHECK(kegs[1].pending_rebuild.find(std::string(64, '9')) != std::string::npos);

        auto note = format_source_built_kegs(kegs);
        CHECK(note.find("source-built kegs: 2") != std::string::npos);
        CHECK(note.find("tree 1.0 (homebrew/core@0123456789ab) [rebuild pending]") !=
              std::string::npos);
        // jq is listed but not flagged.
        CHECK(note.find("jq 1.7 (homebrew/core@0123456789ab)\n") != std::string::npos);
        CHECK(note.find("poured") == std::string::npos);

        // Without an index, kegs are still listed and none is flagged.
        for (const auto& k : source_built_kegs(store, nullptr))
            CHECK(k.pending_rebuild.empty());
        CHECK(format_source_built_kegs({}).empty());
    }
}

} // namespace test
} // namespace den
