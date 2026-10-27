// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Regression: the index generator must not bloat index.json with cask
// variants den can never use (Linux variants, duplicates of a fallback URL),
// and trimming must never change which archive den picks for a cask on any
// macOS arch/version (arm64 vs x86_64 selection in particular).

#include <doctest.h>

#include "index/index.h"
#include "index/package.h"
#include "platform/platform.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace den {
namespace test {

namespace {

using json = nlohmann::json;
using Selection = std::optional<std::pair<std::string, std::string>>;

constexpr const char* kArm = "https://example.com/app-arm64.dmg";
constexpr const char* kIntel = "https://example.com/app-x86_64.dmg";
constexpr const char* kOld = "https://example.com/app-old.dmg";

json variant(const std::string& url, const std::string& sha) {
    return json{{"url", url}, {"sha256", sha}};
}

/// What den would download for (arch, macOS major), straight from the
/// production selector.
Selection select(const Package& pkg, Arch arch, int major) {
    std::vector<std::string> tags;
    for (const auto& [tag, _] : pkg.archives) {
        tags.push_back(tag);
    }
    auto best = best_archive_tag(arch, MacOsVersion{major, 0}, tags);
    if (!best) {
        return std::nullopt;
    }
    const auto& ar = pkg.archives.at(*best);
    return std::make_pair(ar.url, ar.sha256);
}

/// Index a single cask (and an optional formula) through the real generator.
PackageIndex index_of(const json& cask, const json& formulae = json::array()) {
    return transform_homebrew(formulae.dump(), json::array({cask}).dump());
}

/// A cask shaped like Homebrew's visual-studio-code entry: arm64 default URL,
/// Intel variants for every codename, older pinned builds for Big Sur, and
/// Linux variants.
json vscode_like() {
    json c = {{"token", "vscode-like"},
              {"version", "1.0"},
              {"url", kArm},
              {"sha256", "aaaa"},
              {"artifacts", json::array({json{{"app", json::array({"Code.app"})}}})}};
    json v = json::object();
    for (const char* t : {"golden_gate", "tahoe", "sequoia", "sonoma", "ventura", "monterey"}) {
        v[t] = variant(kIntel, "bbbb");
    }
    v["big_sur"] = variant(kOld, "cccc");
    v["arm64_big_sur"] = variant(kOld, "dddd");
    v["x86_64_linux"] = variant("https://example.com//linux", "eeee");
    v["arm64_linux"] = variant("https://example.com//linux-arm", "ffff");
    c["variations"] = v;
    return c;
}

/// Reference: transform without trimming is the pre-trim generator output,
/// reproduced here by building the archive map exactly as #61 did.
Package untrimmed(const json& c) {
    Package pkg;
    Package::Archive base;
    if (c.contains("url")) {
        base.url = c["url"].get<std::string>();
        base.sha256 = c.value("sha256", "");
        pkg.archives["default"] = base;
    }
    if (c.contains("variations")) {
        for (auto it = c["variations"].begin(); it != c["variations"].end(); ++it) {
            Package::Archive ar =
                pkg.archives.count("default") ? pkg.archives["default"] : Package::Archive{};
            ar.url = it.value()["url"].get<std::string>();
            ar.sha256 = it.value().value("sha256", ar.sha256);
            pkg.archives[it.key()] = ar;
        }
    }
    return pkg;
}

void check_same_selection(const Package& before, const Package& after) {
    for (Arch arch : {Arch::Arm64, Arch::X86_64}) {
        for (int major = 11; major <= 40; ++major) {
            CAPTURE(static_cast<int>(arch));
            CAPTURE(major);
            CHECK(select(before, arch, major) == select(after, arch, major));
        }
    }
}

} // namespace

TEST_SUITE("cask_index_trim") {

    TEST_CASE("vscode-like cask: Linux and duplicate variants dropped") {
        const json c = vscode_like();
        auto idx = index_of(c);
        const Package* pkg = idx.find("vscode-like");
        REQUIRE(pkg != nullptr);

        for (const auto& [tag, _] : pkg->archives) {
            CHECK_FALSE(tag.ends_with("_linux"));
        }
        // default (arm64) + one Intel variant reachable by every x86_64 macOS
        // den knows; golden_gate/big_sur are unreachable for den's selector.
        CHECK(pkg->archives.size() == 2);
        CHECK(pkg->archives.count("default") == 1);
        CHECK(pkg->archives.count("monterey") == 1);
        CHECK(pkg->cask_artifacts.size() == 1);

        check_same_selection(untrimmed(c), *pkg);
    }

    TEST_CASE("arch selection is preserved: arm64 vs x86_64") {
        auto idx = index_of(vscode_like());
        const Package& pkg = *idx.find("vscode-like");
        for (int major : {12, 13, 14, 15, 26, 27}) {
            CAPTURE(major);
            CHECK(select(pkg, Arch::Arm64, major)->first == kArm);
            CHECK(select(pkg, Arch::X86_64, major)->first == kIntel);
            CHECK(select(pkg, Arch::X86_64, major)->second == "bbbb");
        }
        // macOS 11 has no bottle-tag candidates in den: falls back to default
        // exactly as before trimming.
        CHECK(select(pkg, Arch::X86_64, 11)->first == kArm);
    }

    TEST_CASE("Intel-default cask keeps its arm64 variant") {
        json c = {
            {"token", "intel-default"}, {"version", "2"}, {"url", kIntel}, {"sha256", "1111"}};
        json v = json::object();
        for (const char* t :
             {"arm64_tahoe", "arm64_sequoia", "arm64_sonoma", "arm64_ventura", "arm64_monterey"}) {
            v[t] = variant(kArm, "2222");
        }
        v["arm64_big_sur"] = variant(kArm, "2222");
        v["x86_64_linux"] = variant(kIntel, "1111");
        c["variations"] = v;

        auto idx = index_of(c);
        const Package& pkg = *idx.find("intel-default");
        CHECK(pkg.archives.size() == 2);
        CHECK(pkg.archives.count("arm64_monterey") == 1);
        check_same_selection(untrimmed(c), pkg);
        CHECK(select(pkg, Arch::Arm64, 15)->first == kArm);
        CHECK(select(pkg, Arch::X86_64, 15)->first == kIntel);
    }

    TEST_CASE("per-version URLs that differ are kept") {
        json c = {{"token", "per-version"}, {"version", "3"}, {"url", kArm}, {"sha256", "a"}};
        c["variations"] = {{"tahoe", variant("https://example.com/x86-new.dmg", "n")},
                           {"sequoia", variant(kIntel, "i")},
                           {"sonoma", variant(kIntel, "i")},
                           {"ventura", variant(kIntel, "i")},
                           {"arm64_sonoma", variant(kOld, "o")}};
        auto idx = index_of(c);
        const Package& pkg = *idx.find("per-version");
        check_same_selection(untrimmed(c), pkg);
        CHECK(pkg.archives.count("tahoe") == 1);
        CHECK(pkg.archives.count("arm64_sonoma") == 1);
        CHECK(select(pkg, Arch::X86_64, 26)->first == "https://example.com/x86-new.dmg");
        CHECK(select(pkg, Arch::X86_64, 14)->first == kIntel);
        // arm64 bottle-style candidates fall back to older codenames, so
        // arm64_sonoma wins on every arm64 macOS >= 14 (pre-existing selector
        // behaviour, preserved); below that, default.
        CHECK(select(pkg, Arch::Arm64, 26)->first == kOld);
        CHECK(select(pkg, Arch::Arm64, 14)->first == kOld);
        CHECK(select(pkg, Arch::Arm64, 13)->first == kArm);
    }

    TEST_CASE("same URL but different sha256 is not a duplicate") {
        json c = {{"token", "sha-differs"}, {"version", "1"}, {"url", kArm}, {"sha256", "a"}};
        c["variations"] = {{"sequoia", variant(kArm, "b")}};
        auto idx = index_of(c);
        const Package& pkg = *idx.find("sha-differs");
        CHECK(pkg.archives.count("sequoia") == 1);
        check_same_selection(untrimmed(c), pkg);
    }

    TEST_CASE("cask with nothing selectable on macOS is left untouched") {
        json c = {{"token", "big-sur-only"}, {"version", "1"}};
        c["variations"] = {{"big_sur", variant(kOld, "o")}, {"x86_64_linux", variant(kIntel, "l")}};
        auto idx = index_of(c);
        const Package& pkg = *idx.find("big-sur-only");
        CHECK(pkg.archives.size() == 2); // `den info` still lists them
    }

    TEST_CASE("formula bottles (including Linux) are not trimmed") {
        json f =
            json::array({json{{"name", "hello"},
                              {"versions", {{"stable", "2.12"}}},
                              {"bottle",
                               {{"stable",
                                 {{"cellar", ":any_skip_relocation"},
                                  {"files",
                                   {{"arm64_sequoia", {{"url", "u1"}, {"sha256", "s1"}}},
                                    {"arm64_sonoma", {{"url", "u1"}, {"sha256", "s1"}}},
                                    {"x86_64_linux", {{"url", "u2"}, {"sha256", "s2"}}}}}}}}}}});
        auto idx = index_of(vscode_like(), f);
        const Package* hello = idx.find("hello");
        REQUIRE(hello != nullptr);
        CHECK(hello->archives.size() == 3);
    }

    TEST_CASE("randomised: trimming never changes macOS selection") {
        const std::vector<std::string> universe = {
            "arm64_golden_gate", "golden_gate",  "arm64_tahoe",   "tahoe",         "arm64_sequoia",
            "sequoia",           "arm64_sonoma", "sonoma",        "arm64_ventura", "ventura",
            "arm64_monterey",    "monterey",     "arm64_big_sur", "big_sur",       "x86_64_linux",
            "arm64_linux",       "all"};
        std::mt19937 rng(20261027);
        for (int iter = 0; iter < 2000; ++iter) {
            Package pkg;
            pkg.name = "fuzz";
            if (rng() % 4 != 0) {
                pkg.archives["default"] = {"u" + std::to_string(rng() % 3), "s", {}};
            }
            for (const auto& tag : universe) {
                if (rng() % 2 == 0) {
                    pkg.archives[tag] = {"u" + std::to_string(rng() % 3), "s", {}};
                }
            }
            const Package before = pkg;
            trim_cask_archives(pkg);
            CAPTURE(iter);
            check_same_selection(before, pkg);
            CHECK(pkg.archives.size() <= before.archives.size());
        }
    }

    TEST_CASE("trimmed index round-trips through the existing on-disk format") {
        auto idx = index_of(vscode_like());
        const auto path =
            std::filesystem::temp_directory_path() /
            ("den_trim_roundtrip_" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
        save_index(idx, path);
        auto loaded = load_index(path);
        std::filesystem::remove(path);
        const Package* pkg = loaded.find("vscode-like");
        REQUIRE(pkg != nullptr);
        CHECK(pkg->archives.size() == 2);
        check_same_selection(untrimmed(vscode_like()), *pkg);
    }
}

} // namespace test
} // namespace den
