// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// `den doctor` warns about held packages: the index's only version for an
// installed formula is lower but carries a higher version_scheme (openssl@3
// 3.6.3 installed, index pins 3.5.9 with version_scheme 1), so den holds it
// and never downgrades on its own. The hold comes from the same plan the
// daemon and `den upgrade` compute (compute_pending / version_delta).

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
namespace test_doctor_held {

namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "den_doctor_held_XXXXXX").string();
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

// A den home whose ROOT manifest lists `pkgs` (name -> version) in the
// homebrew bucket, each with a keg whose receipt records `scheme`.
Config make_home(const fs::path& root,
                 const std::map<std::string, std::pair<std::string, int>>& pkgs) {
    Config c;
    c.den_home = root / "home";
    c.prefix = root / "pfx";
    c.store = c.prefix / "Cellar";
    c.cache = c.den_home / "cache";
    std::string bucket;
    for (const auto& [name, vs] : pkgs) {
        const auto keg = c.store / name / vs.first;
        fs::create_directories(keg / "bin");
        std::ofstream(keg / "INSTALL_RECEIPT.json")
            << R"({"source":{"versions":{"stable":")" << vs.first << R"(","version_scheme":)"
            << vs.second << "}}}";
        if (!bucket.empty())
            bucket += ",";
        bucket += "\"" + name + "\":\"" + vs.first + "\"";
    }
    fs::create_directories(c.den_home / "manifests" / "ROOT");
    std::ofstream(c.den_home / "manifests" / "ROOT" / "manifest.json")
        << R"({"packages":{"homebrew":{)" << bucket << "}}}";
    return c;
}

TEST_SUITE("doctor::held_packages") {

    TEST_CASE("openssl@3 3.6.3 with the index pinning 3.5.9 (version_scheme 1) is held") {
        TempDir t;
        auto c = make_home(t.path, {{"openssl@3", {"3.6.3", 0}}, {"jq", {"1.7.1", 0}}});
        PackageIndex idx;
        idx.packages["openssl@3"] = formula("openssl@3", "3.5.9", 1);
        idx.packages["jq"] = formula("jq", "1.8.0"); // a plain upgrade, not held
        auto held = held_packages(c, idx);
        REQUIRE(held.size() == 1);
        CHECK(held[0].name == "openssl@3");
        CHECK(held[0].installed == "3.6.3");
        CHECK(held[0].available == "3.5.9");
        CHECK(held[0].reason == "held: index pins 3.5.9 (version_scheme 1), below installed 3.6.3");
    }

    TEST_CASE("nothing held when the index matches, is newer, or is merely older") {
        TempDir t;
        auto c = make_home(
            t.path, {{"openssl@3", {"3.6.3", 0}}, {"jq", {"1.7.1", 0}}, {"tree", {"2.3.2", 0}}});
        PackageIndex idx;
        idx.packages["openssl@3"] = formula("openssl@3", "3.6.3");
        idx.packages["jq"] = formula("jq", "1.8.0");
        idx.packages["tree"] = formula("tree", "2.2.1"); // older, same scheme: not a hold
        CHECK(held_packages(c, idx).empty());
    }

    TEST_CASE("a keg already on the higher scheme is not held") {
        TempDir t;
        // Installed 3.5.9 with scheme 1, index 3.5.9 scheme 1: same.
        auto c = make_home(t.path, {{"openssl@3", {"3.5.9", 1}}});
        PackageIndex idx;
        idx.packages["openssl@3"] = formula("openssl@3", "3.5.9", 1);
        CHECK(held_packages(c, idx).empty());
    }

    TEST_CASE("format lists installed vs available, the reason and the den use remedy") {
        std::vector<DeferredUpgrade> held{
            {"openssl@3", "3.6.3", "3.5.9",
             "held: index pins 3.5.9 (version_scheme 1), below installed 3.6.3"}};
        const auto text = format_held_packages(held);
        CHECK(text.find("1 package(s) held") != std::string::npos);
        CHECK(text.find("openssl@3: installed 3.6.3, index has 3.5.9 (held: index pins 3.5.9 "
                        "(version_scheme 1), below installed 3.6.3)") != std::string::npos);
        CHECK(text.find("den use openssl@3 3.5.9") != std::string::npos);
        CHECK(format_held_packages({}).empty());
    }

} // TEST_SUITE

} // namespace test_doctor_held
} // namespace den
