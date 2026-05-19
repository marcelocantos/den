// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <doctest.h>

#include "env/environment.h"
#include "env/manifest.h"
#include "store/link.h"
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
            ("den_test_env_" +
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

TEST_SUITE("environment::env_dir") {

    TEST_CASE("root env maps to envs/ROOT") {
        CHECK(env_dir("/home/den", "/") == fs::path("/home/den/envs/ROOT"));
    }

    TEST_CASE("named env uses slug") {
        auto dir = env_dir("/home/den", "/ml");
        CHECK(dir == fs::path("/home/den/envs/ml"));
    }
}

TEST_SUITE("environment::active_env_path") {

    TEST_CASE("returns / when no file exists") {
        TmpDir tmp;
        CHECK(active_env_path(tmp.path) == "/");
    }

    TEST_CASE("set and read back") {
        TmpDir tmp;
        fs::create_directories(tmp.path);
        set_active_env(tmp.path, "/ml");
        CHECK(active_env_path(tmp.path) == "/ml");
    }

    TEST_CASE("overwrite active env") {
        TmpDir tmp;
        set_active_env(tmp.path, "/ml");
        set_active_env(tmp.path, "/work");
        CHECK(active_env_path(tmp.path) == "/work");
    }
}

TEST_SUITE("environment::materialise") {

    TEST_CASE("links packages from store into env dir") {
        TmpDir tmp;
        auto den_home = tmp.path / "den_home";
        auto store = tmp.path / "store";

        // Create a package in the store with a bin file.
        auto pkg = store / "tree" / "2.1.1";
        fs::create_directories(pkg / "bin");
        std::ofstream(pkg / "bin" / "tree") << "#!/bin/sh\n";

        // Write a manifest referencing it.
        Manifest m;
        m.packages["homebrew"]["tree"] = "2.1.1";
        write_manifest(den_home, "/", m);

        // Materialise the root environment.
        auto count = materialise(den_home, store, "/");
        CHECK(count > 0);

        // Verify symlinks in the env dir.
        auto eDir = env_dir(den_home, "/");
        CHECK(fs::is_symlink(eDir / "bin" / "tree"));
        CHECK(fs::read_symlink(eDir / "bin" / "tree") == pkg / "bin" / "tree");
        CHECK(fs::is_symlink(eDir / "opt" / "tree"));
    }

    TEST_CASE("skips already-linked packages at correct version") {
        TmpDir tmp;
        auto den_home = tmp.path / "den_home";
        auto store = tmp.path / "store";

        auto pkg = store / "curl" / "8.5.0";
        fs::create_directories(pkg / "bin");
        std::ofstream(pkg / "bin" / "curl") << "#!/bin/sh\n";

        Manifest m;
        m.packages["homebrew"]["curl"] = "8.5.0";
        write_manifest(den_home, "/", m);

        auto count1 = materialise(den_home, store, "/");
        CHECK(count1 > 0);

        // Second materialise should create zero new symlinks.
        auto count2 = materialise(den_home, store, "/");
        CHECK(count2 == 0);
    }

    TEST_CASE("switches version when manifest changes") {
        TmpDir tmp;
        auto den_home = tmp.path / "den_home";
        auto store = tmp.path / "store";

        // Create two versions.
        auto pkg1 = store / "tree" / "2.1.1";
        fs::create_directories(pkg1 / "bin");
        std::ofstream(pkg1 / "bin" / "tree") << "v2.1.1";

        auto pkg2 = store / "tree" / "2.2.0";
        fs::create_directories(pkg2 / "bin");
        std::ofstream(pkg2 / "bin" / "tree") << "v2.2.0";

        // First materialise with v2.1.1.
        Manifest m;
        m.packages["homebrew"]["tree"] = "2.1.1";
        write_manifest(den_home, "/", m);
        materialise(den_home, store, "/");

        auto eDir = env_dir(den_home, "/");
        CHECK(fs::read_symlink(eDir / "bin" / "tree") == pkg1 / "bin" / "tree");

        // Update manifest to v2.2.0 and re-materialise.
        m.packages["homebrew"]["tree"] = "2.2.0";
        write_manifest(den_home, "/", m);
        materialise(den_home, store, "/");

        CHECK(fs::read_symlink(eDir / "bin" / "tree") == pkg2 / "bin" / "tree");
    }
}

} // namespace test
} // namespace den
