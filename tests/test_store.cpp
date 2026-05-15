// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <doctest.h>

#include "store/link.h"
#include "store/store.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace den {
namespace test {

namespace fs = std::filesystem;

/// RAII temporary directory: creates a unique subdir under the system
/// temp path and removes it (recursively) on destruction.
struct TmpDir {
    fs::path path;

    TmpDir() {
        path =
            fs::temp_directory_path() /
            ("den_test_store_" +
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

// ---------------------------------------------------------------------------
// store.h tests
// ---------------------------------------------------------------------------

TEST_SUITE("store::package_path") {

    TEST_CASE("returns store/name/version layout") {
        CHECK(package_path("/opt/den/store", "tree", "2.1.1") ==
              fs::path("/opt/den/store/tree/2.1.1"));
    }

    TEST_CASE("handles versioned formula names") {
        CHECK(package_path("/s", "python@3.12", "3.12.3") == fs::path("/s/python@3.12/3.12.3"));
    }
}

TEST_SUITE("store::is_installed") {

    TEST_CASE("returns true when directory exists") {
        TmpDir tmp;
        auto pkg = tmp.path / "tree" / "2.1.1";
        fs::create_directories(pkg);
        CHECK(is_installed(tmp.path, "tree", "2.1.1"));
    }

    TEST_CASE("returns false when directory is missing") {
        TmpDir tmp;
        CHECK_FALSE(is_installed(tmp.path, "tree", "2.1.1"));
    }

    TEST_CASE("returns false when path is a file not a directory") {
        TmpDir tmp;
        auto pkg = tmp.path / "tree" / "2.1.1";
        fs::create_directories(pkg.parent_path());
        std::ofstream(pkg) << "not a dir";
        CHECK_FALSE(is_installed(tmp.path, "tree", "2.1.1"));
    }
}

TEST_SUITE("store::list_installed") {

    TEST_CASE("returns empty for nonexistent store") {
        CHECK(list_installed("/nonexistent/store/path").empty());
    }

    TEST_CASE("finds all installed packages") {
        TmpDir tmp;
        fs::create_directories(tmp.path / "tree" / "2.1.1");
        fs::create_directories(tmp.path / "tree" / "2.2.0");
        fs::create_directories(tmp.path / "curl" / "8.5.0");

        auto pkgs = list_installed(tmp.path);
        CHECK(pkgs.size() == 3);

        // Build a set of name@version for easy checking.
        std::set<std::string> found;
        for (const auto& p : pkgs) {
            found.insert(p.name + "@" + p.version);
        }
        CHECK(found.count("tree@2.1.1") == 1);
        CHECK(found.count("tree@2.2.0") == 1);
        CHECK(found.count("curl@8.5.0") == 1);
    }

    TEST_CASE("ignores files at the top level") {
        TmpDir tmp;
        fs::create_directories(tmp.path / "tree" / "2.1.1");
        std::ofstream(tmp.path / "stray_file") << "ignore me";

        auto pkgs = list_installed(tmp.path);
        CHECK(pkgs.size() == 1);
    }
}

TEST_SUITE("store::which_package") {

    TEST_CASE("identifies package from a file inside the store") {
        TmpDir tmp;
        auto pkg = tmp.path / "tree" / "2.1.1";
        fs::create_directories(pkg / "bin");
        std::ofstream(pkg / "bin" / "tree") << "#!/bin/sh\n";

        auto result = which_package(tmp.path, pkg / "bin" / "tree");
        REQUIRE(result.has_value());
        CHECK(result->name == "tree");
        CHECK(result->version == "2.1.1");
    }

    TEST_CASE("resolves symlinks to find the owning package") {
        TmpDir tmp;
        auto pkg = tmp.path / "tree" / "2.1.1";
        fs::create_directories(pkg / "bin");
        std::ofstream(pkg / "bin" / "tree") << "#!/bin/sh\n";

        // Create a symlink outside the store pointing into it.
        auto link = tmp.path / "link_to_tree";
        fs::create_symlink(pkg / "bin" / "tree", link);

        auto result = which_package(tmp.path, link);
        REQUIRE(result.has_value());
        CHECK(result->name == "tree");
        CHECK(result->version == "2.1.1");
    }

    TEST_CASE("returns nullopt for file outside the store") {
        TmpDir tmp;
        fs::create_directories(tmp.path / "tree" / "2.1.1");

        auto result = which_package(tmp.path, "/usr/bin/ls");
        CHECK_FALSE(result.has_value());
    }

    TEST_CASE("returns nullopt for nonexistent file") {
        TmpDir tmp;
        auto result = which_package(tmp.path, tmp.path / "no" / "such" / "file");
        CHECK_FALSE(result.has_value());
    }
}

// ---------------------------------------------------------------------------
// link.h tests
// ---------------------------------------------------------------------------

TEST_SUITE("link::is_valid_package_name") {

    TEST_CASE("valid names") {
        CHECK(is_valid_package_name("tree"));
        CHECK(is_valid_package_name("python@3"));
        CHECK(is_valid_package_name("python@312"));
        CHECK(is_valid_package_name("node-lts"));
        CHECK(is_valid_package_name("lib_foo"));
        CHECK(is_valid_package_name("a"));
        CHECK(is_valid_package_name("boost+icu"));
    }

    TEST_CASE("invalid names") {
        CHECK_FALSE(is_valid_package_name(""));
        CHECK_FALSE(is_valid_package_name(".."));
        CHECK_FALSE(is_valid_package_name("../evil"));
        CHECK_FALSE(is_valid_package_name(".hidden"));
        CHECK_FALSE(is_valid_package_name("-starts-with-dash"));
    }
}

TEST_SUITE("link::link_package") {

    TEST_CASE("creates symlinks for bin and lib files and opt link") {
        TmpDir tmp;
        auto pkg = tmp.path / "store" / "mypkg" / "1.0";
        auto env = tmp.path / "env";
        fs::create_directories(pkg / "bin");
        fs::create_directories(pkg / "lib");
        fs::create_directories(env);

        // Create some files in the package.
        std::ofstream(pkg / "bin" / "foo") << "#!/bin/sh\n";
        std::ofstream(pkg / "lib" / "bar.so") << "ELF";

        auto count = link_package(pkg, env, "mypkg");
        // 2 file symlinks + 1 opt symlink = 3
        CHECK(count == 3);

        // Verify bin/foo is a symlink pointing to the right place.
        CHECK(fs::is_symlink(env / "bin" / "foo"));
        CHECK(fs::read_symlink(env / "bin" / "foo") == pkg / "bin" / "foo");

        // Verify lib/bar.so.
        CHECK(fs::is_symlink(env / "lib" / "bar.so"));
        CHECK(fs::read_symlink(env / "lib" / "bar.so") == pkg / "lib" / "bar.so");

        // Verify opt/mypkg symlink.
        CHECK(fs::is_symlink(env / "opt" / "mypkg"));
        CHECK(fs::read_symlink(env / "opt" / "mypkg") == pkg);
    }

    TEST_CASE("handles nested directories in share") {
        TmpDir tmp;
        auto pkg = tmp.path / "store" / "mypkg" / "1.0";
        auto env = tmp.path / "env";
        fs::create_directories(pkg / "share" / "man" / "man1");
        fs::create_directories(env);

        std::ofstream(pkg / "share" / "man" / "man1" / "foo.1") << ".TH FOO";

        auto count = link_package(pkg, env, "mypkg");
        // 1 file + 1 opt = 2
        CHECK(count == 2);
        CHECK(fs::is_symlink(env / "share" / "man" / "man1" / "foo.1"));
    }
}

TEST_SUITE("link::unlink_package") {

    TEST_CASE("removes symlinks created by link_package") {
        TmpDir tmp;
        auto pkg = tmp.path / "store" / "mypkg" / "1.0";
        auto env = tmp.path / "env";
        fs::create_directories(pkg / "bin");
        fs::create_directories(env);

        std::ofstream(pkg / "bin" / "foo") << "#!/bin/sh\n";

        link_package(pkg, env, "mypkg");
        CHECK(fs::is_symlink(env / "bin" / "foo"));
        CHECK(fs::is_symlink(env / "opt" / "mypkg"));

        auto removed = unlink_package(pkg, env, "mypkg");
        CHECK(removed == 2); // bin/foo + opt/mypkg

        CHECK_FALSE(fs::exists(env / "bin" / "foo"));
        CHECK_FALSE(fs::exists(env / "opt" / "mypkg"));
    }
}

TEST_SUITE("link::record_linked_version / linked_version") {

    TEST_CASE("write and read back") {
        TmpDir tmp;
        auto env = tmp.path / "env";
        fs::create_directories(env);

        record_linked_version(env, "tree", "2.1.1");
        auto ver = linked_version(env, "tree");
        REQUIRE(ver.has_value());
        CHECK(*ver == "2.1.1");
    }

    TEST_CASE("returns nullopt for unrecorded package") {
        TmpDir tmp;
        auto env = tmp.path / "env";
        fs::create_directories(env);

        CHECK_FALSE(linked_version(env, "tree").has_value());
    }

    TEST_CASE("overwrites previous version") {
        TmpDir tmp;
        auto env = tmp.path / "env";
        fs::create_directories(env);

        record_linked_version(env, "tree", "2.1.1");
        record_linked_version(env, "tree", "2.2.0");
        auto ver = linked_version(env, "tree");
        REQUIRE(ver.has_value());
        CHECK(*ver == "2.2.0");
    }
}

} // namespace test
} // namespace den
