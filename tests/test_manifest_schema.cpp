// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Tests for the multi-provider manifest schema introduced by 🎯T23.4.
//
// Two on-disk shapes coexist:
//
//   - **Legacy flat** (pre-T23.4, treated as Homebrew on read; never written):
//       { "packages": { "ffmpeg": "6.1.1" }, "auto_deps": ["pcre2"] }
//
//   - **Nested per-provider** (the only shape written today):
//       { "packages": { "homebrew": { "ffmpeg": "6.1.1" },
//                       "pip":      { "requests": "2.31.0" } },
//         "auto_deps": { "homebrew": ["pcre2"] } }

#include <doctest.h>

#include "env/manifest.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace den {
namespace test {

namespace fs = std::filesystem;
using json = nlohmann::json;

struct TmpDir {
    fs::path path;

    TmpDir() {
        path =
            fs::temp_directory_path() /
            ("den_test_manifest_schema_" +
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

static fs::path manifest_path(const fs::path& den_home, const std::string& env_path) {
    auto slug = env_slug(env_path);
    return den_home / "manifests" / slug / "manifest.json";
}

static void write_raw_manifest(const fs::path& den_home, const std::string& env_path,
                               const std::string& body) {
    auto path = manifest_path(den_home, env_path);
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    out << body;
}

TEST_SUITE("T23.4::manifest_schema") {

    TEST_CASE("legacy flat packages reads as homebrew block") {
        TmpDir tmp;
        write_raw_manifest(tmp.path, "/", R"({
            "packages": {"ffmpeg": "6.1.1", "curl": "8.5.0"},
            "auto_deps": ["pcre2", "openssl"]
        })");

        auto m = read_manifest(tmp.path, "/");

        CHECK(m.packages["homebrew"]["ffmpeg"] == "6.1.1");
        CHECK(m.packages["homebrew"]["curl"] == "8.5.0");
        CHECK(m.auto_deps["homebrew"].count("pcre2") == 1);
        CHECK(m.auto_deps["homebrew"].count("openssl") == 1);
    }

    TEST_CASE("nested per-provider packages round-trips") {
        TmpDir tmp;

        Manifest m;
        m.packages["homebrew"]["ffmpeg"] = "6.1.1";
        m.packages["pip"]["requests"] = "2.31.0";
        m.packages["pip"]["urllib3"] = "2.0.7";
        m.auto_deps["homebrew"].insert("pcre2");
        m.auto_deps["pip"].insert("urllib3");
        write_manifest(tmp.path, "/", m);

        auto loaded = read_manifest(tmp.path, "/");

        CHECK(loaded.packages["homebrew"]["ffmpeg"] == "6.1.1");
        CHECK(loaded.packages["pip"]["requests"] == "2.31.0");
        CHECK(loaded.packages["pip"]["urllib3"] == "2.0.7");
        CHECK(loaded.auto_deps["homebrew"].count("pcre2") == 1);
        CHECK(loaded.auto_deps["pip"].count("urllib3") == 1);
    }

    TEST_CASE("writer emits nested form even for homebrew-only manifests") {
        TmpDir tmp;

        Manifest m;
        m.packages["homebrew"]["ffmpeg"] = "6.1.1";
        m.auto_deps["homebrew"].insert("pcre2");
        write_manifest(tmp.path, "/", m);

        std::ifstream in(manifest_path(tmp.path, "/"));
        json j = json::parse(in);

        REQUIRE(j["packages"].is_object());
        REQUIRE(j["packages"]["homebrew"].is_object());
        CHECK(j["packages"]["homebrew"]["ffmpeg"] == "6.1.1");
        REQUIRE(j["auto_deps"].is_object());
        REQUIRE(j["auto_deps"]["homebrew"].is_array());
    }

    TEST_CASE("non-homebrew provider block survives read -> modify -> write") {
        TmpDir tmp;

        // Seed with two providers.
        Manifest m;
        m.packages["homebrew"]["ffmpeg"] = "6.1.1";
        m.packages["pip"]["requests"] = "2.31.0";
        m.auto_deps["pip"].insert("urllib3");
        write_manifest(tmp.path, "/", m);

        // Read, modify only the homebrew block.
        auto loaded = read_manifest(tmp.path, "/");
        loaded.packages["homebrew"]["curl"] = "8.5.0";
        write_manifest(tmp.path, "/", loaded);

        // The pip block must still be intact.
        auto re_loaded = read_manifest(tmp.path, "/");
        CHECK(re_loaded.packages["homebrew"]["ffmpeg"] == "6.1.1");
        CHECK(re_loaded.packages["homebrew"]["curl"] == "8.5.0");
        CHECK(re_loaded.packages["pip"]["requests"] == "2.31.0");
        CHECK(re_loaded.auto_deps["pip"].count("urllib3") == 1);
    }

    TEST_CASE("missing manifest file returns empty manifest") {
        TmpDir tmp;
        auto m = read_manifest(tmp.path, "/");
        CHECK(m.packages.empty());
        CHECK(m.auto_deps.empty());
        CHECK_FALSE(m.origin.has_value());
    }

    TEST_CASE("legacy flat with empty auto_deps array reads cleanly") {
        TmpDir tmp;
        write_raw_manifest(tmp.path, "/", R"({
            "packages": {"tree": "2.1.1"},
            "auto_deps": []
        })");

        auto m = read_manifest(tmp.path, "/");
        CHECK(m.packages["homebrew"]["tree"] == "2.1.1");
        CHECK(m.auto_deps["homebrew"].empty());
    }
}

} // namespace test
} // namespace den
