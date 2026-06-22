// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Tests for `den list --cellar` (Cellar state listing).
//
// 🎯T66 verification harness — Cellar state.
//
// Acceptance coverage:
//   SKIP — den list --cellar disk usage per keg     (flag not implemented)
//   SKIP — den list --cellar env references per keg (flag not implemented)
//   SKIP — den list --cellar orphaned kegs           (flag not implemented)
//   PASS — Unit tests for underlying store::list_installed() which IS
//           implemented and is the foundation the --cellar flag will use.
//
// When `den list --cellar` is implemented, convert the SKIP integration
// tests to PASS assertions.

#include <doctest.h>

#include "store/store.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace den {
namespace test_list_cellar {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// RAII temp directory.
// ---------------------------------------------------------------------------
struct TempDir {
    fs::path path;

    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "den_cellar_XXXXXX").string();
        char* result = ::mkdtemp(tmpl.data());
        if (!result) {
            throw std::runtime_error(std::string("mkdtemp failed: ") + std::strerror(errno));
        }
        path = result;
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

// ---------------------------------------------------------------------------
// Helper: run ./den with an isolated DEN_HOME and a fake Cellar rooted at
// tmp.path/brew/Cellar.
// ---------------------------------------------------------------------------
static std::string run_den(const std::string& args, const fs::path& den_home,
                           const fs::path& cellar) {
    const std::string prefix = cellar.parent_path().string();
    std::string cmd = "DEN_HOME=" + den_home.string() + " HOMEBREW_PREFIX=" + prefix +
                      " HOMEBREW_CELLAR=" + cellar.string() + " ./den " + args + " 2>&1";
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("popen failed for: " + cmd);
    }
    std::string output;
    std::array<char, 1024> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
        output += buf.data();
    }
    ::pclose(pipe);
    return output;
}

// ---------------------------------------------------------------------------
// Build a minimal fake Cellar:
//
//   <cellar>/tree/2.1.1/bin/tree      (non-empty, referenced by envA)
//   <cellar>/tree/2.2.0/bin/tree      (non-empty, referenced by envB)
//   <cellar>/curl/8.5.0/bin/curl      (non-empty, referenced by both envs)
//   <cellar>/orphan/1.0.0/bin/orphan  (non-empty, referenced by no env)
//
// Returns the set of keg paths created.
// ---------------------------------------------------------------------------
static void build_fake_cellar(const fs::path& cellar, const fs::path& den_home) {
    // Kegs.
    for (const auto& [pkg, ver, content] :
         std::initializer_list<std::tuple<std::string, std::string, std::string>>{
             {"tree", "2.1.1", "#!/bin/sh\necho tree 2.1.1"},
             {"tree", "2.2.0", "#!/bin/sh\necho tree 2.2.0"},
             {"curl", "8.5.0", "#!/bin/sh\necho curl"},
             {"orphan", "1.0.0", "#!/bin/sh\necho orphan"},
         }) {
        auto bin = cellar / pkg / ver / "bin";
        fs::create_directories(bin);
        std::ofstream(bin / pkg) << content;
    }

    // Two environments that reference some (but not all) kegs.
    // envA references tree@2.1.1 and curl@8.5.0.
    // envB references tree@2.2.0 and curl@8.5.0.
    // orphan@1.0.0 is referenced by neither.
    for (const auto& [env_name, pkgs] : std::initializer_list<
             std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>{
             {"envA", {{"tree", "2.1.1"}, {"curl", "8.5.0"}}},
             {"envB", {{"tree", "2.2.0"}, {"curl", "8.5.0"}}},
         }) {
        // Write a minimal manifest JSON.
        auto manifest_dir = den_home / "manifests" / env_name;
        fs::create_directories(manifest_dir);
        std::ofstream mf(manifest_dir / "manifest.json");
        mf << "{\"packages\":{";
        bool first = true;
        for (const auto& [n, v] : pkgs) {
            if (!first)
                mf << ",";
            mf << "\"" << n << "\":\"" << v << "\"";
            first = false;
        }
        mf << "},\"auto_deps\":[]}";
    }
}

// ===========================================================================
// Unit tests for the store layer (foundation for --cellar).
// These PASS because list_installed() is already implemented.
// ===========================================================================

TEST_SUITE("T66::list_cellar::store_unit") {

    TEST_CASE("list_installed finds all kegs in a fake Cellar") {
        TempDir tmp;
        auto cellar = tmp.path / "Cellar";
        auto den_home = tmp.path / "den";
        fs::create_directories(den_home);
        build_fake_cellar(cellar, den_home);

        auto pkgs = list_installed(cellar);
        REQUIRE(pkgs.size() == 4);

        std::set<std::string> found;
        for (const auto& p : pkgs) {
            found.insert(p.name + "@" + p.version);
        }
        CHECK(found.count("tree@2.1.1") == 1);
        CHECK(found.count("tree@2.2.0") == 1);
        CHECK(found.count("curl@8.5.0") == 1);
        CHECK(found.count("orphan@1.0.0") == 1);
    }

    TEST_CASE("list_installed returns correct paths for each keg") {
        TempDir tmp;
        auto cellar = tmp.path / "Cellar";
        auto den_home = tmp.path / "den";
        fs::create_directories(den_home);
        build_fake_cellar(cellar, den_home);

        auto pkgs = list_installed(cellar);
        for (const auto& p : pkgs) {
            // Each keg path must exist as a directory.
            CHECK(fs::is_directory(p.path));
            // Path must be rooted under the cellar.
            auto rel = p.path.lexically_relative(cellar);
            auto it = rel.begin();
            REQUIRE(it != rel.end());
            CHECK(it->string() == p.name);
            ++it;
            REQUIRE(it != rel.end());
            CHECK(it->string() == p.version);
        }
    }

    TEST_CASE("orphan keg is present in list_installed output") {
        TempDir tmp;
        auto cellar = tmp.path / "Cellar";
        auto den_home = tmp.path / "den";
        fs::create_directories(den_home);
        build_fake_cellar(cellar, den_home);

        auto pkgs = list_installed(cellar);
        bool found_orphan = false;
        for (const auto& p : pkgs) {
            if (p.name == "orphan" && p.version == "1.0.0") {
                found_orphan = true;
                break;
            }
        }
        CHECK(found_orphan);
    }
}

// ===========================================================================
// Integration tests for Cellar inspection via `den list --cellar`.
//
// Note on semantics (🎯T66 + 🎯T60): plain `den list` enumerates the *active
// environment's* installed packages across all providers (the multi-provider
// view), not the raw Cellar. Cellar enumeration — every keg in the shared
// store, regardless of environment — is the `--cellar` view. The fake-Cellar
// fixture exercises that store-level view.
// ===========================================================================

TEST_SUITE("T66::list_cellar::integration") {

    TEST_CASE("den list --cellar shows installed kegs from fake Cellar") {
        TempDir tmp;
        auto cellar = tmp.path / "Cellar";
        auto den_home = tmp.path / "den";
        fs::create_directories(den_home);
        build_fake_cellar(cellar, den_home);

        const auto out = run_den("list --cellar", den_home, cellar);
        CHECK(out.find("tree") != std::string::npos);
        CHECK(out.find("curl") != std::string::npos);
        CHECK(out.find("orphan") != std::string::npos);
        // The empty-Cellar message must NOT appear.
        CHECK(out.find("No kegs in Cellar") == std::string::npos);
    }

    // -----------------------------------------------------------------------
    // den list --cellar shows disk usage per keg (🎯T66/T4).
    // -----------------------------------------------------------------------
    TEST_CASE("den list --cellar shows disk usage per keg") {
        TempDir tmp;
        auto cellar = tmp.path / "Cellar";
        auto den_home = tmp.path / "den";
        fs::create_directories(den_home);
        build_fake_cellar(cellar, den_home);

        const auto out = run_den("list --cellar", den_home, cellar);
        CHECK(out.find("tree") != std::string::npos);
        CHECK(out.find("2.1.1") != std::string::npos);
        CHECK(out.find("2.2.0") != std::string::npos);
        // A size indicator must appear (every keg here is < 1 KiB, so " B").
        const bool has_size = out.find(" B") != std::string::npos ||
                              out.find(" KB") != std::string::npos ||
                              out.find(" MB") != std::string::npos;
        CHECK(has_size);
    }

    // -----------------------------------------------------------------------
    // den list --cellar shows which environments reference each keg (🎯T66/T4).
    // -----------------------------------------------------------------------
    TEST_CASE("den list --cellar shows env references per keg") {
        TempDir tmp;
        auto cellar = tmp.path / "Cellar";
        auto den_home = tmp.path / "den";
        fs::create_directories(den_home);
        build_fake_cellar(cellar, den_home);

        const auto out = run_den("list --cellar", den_home, cellar);
        // Manifests live under den_home/manifests/<slug>; list_all decodes the
        // slug back to an env path ("/envA", "/envB").
        CHECK(out.find("envA") != std::string::npos);
        CHECK(out.find("envB") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // den list --cellar marks orphaned (unreferenced) kegs (🎯T66/T4).
    // -----------------------------------------------------------------------
    TEST_CASE("den list --cellar marks orphaned kegs") {
        TempDir tmp;
        auto cellar = tmp.path / "Cellar";
        auto den_home = tmp.path / "den";
        fs::create_directories(den_home);
        build_fake_cellar(cellar, den_home);

        const auto out = run_den("list --cellar", den_home, cellar);
        // orphan@1.0.0 is referenced by no env, so an orphan marker must appear.
        CHECK(out.find("orphan") != std::string::npos);
        const bool has_marker = out.find("(orphan)") != std::string::npos ||
                                out.find("unreferenced") != std::string::npos;
        CHECK(has_marker);
        // Exactly one orphan in this fixture.
        CHECK(out.find("1 orphan(s)") != std::string::npos);
    }
}

} // namespace test_list_cellar
} // namespace den
