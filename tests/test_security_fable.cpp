// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Regression tests for the 2026-07 Fable audit findings (🎯T78).

#include <doctest.h>

#include "build/source_build.h"
#include "core/error.h"
#include "download/archive.h"
#include "env/environment.h"
#include "env/manifest.h"
#include "index/index.h"
#include "index/sat_solver.h"
#include "selfupdate/selfupdate.h"
#include "store/link.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "provider/exec.h"

namespace den {
namespace test {

namespace fs = std::filesystem;

struct TmpDir {
    fs::path path;
    TmpDir(const char* prefix = "den_fable_") {
        path =
            fs::temp_directory_path() /
            (std::string(prefix) +
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
// F1 — command injection via package name
// ---------------------------------------------------------------------------
TEST_SUITE("fable::F1_package_name") {

    TEST_CASE("is_valid_package_name rejects shell metacharacters") {
        CHECK_FALSE(is_valid_package_name("x; touch /tmp/pwned #"));
        CHECK_FALSE(is_valid_package_name("x$(id)"));
        CHECK_FALSE(is_valid_package_name("x`id`"));
        CHECK_FALSE(is_valid_package_name("a|b"));
        CHECK(is_valid_package_name("jq"));
        CHECK(is_valid_package_name("python@3.12"));
    }

    TEST_CASE("build_from_source rejects metacharacter names before any shell") {
        TmpDir tmp;
        auto sentinel = tmp.path / "pwned";
        PackageIndex idx;
        Config cfg;
        cfg.den_home = tmp.path / "den";
        cfg.store = tmp.path / "store";
        fs::create_directories(cfg.store);

        // Name that would create the sentinel if shell-injected.
        std::string evil = "x; touch " + sentinel.string() + " #";
        CHECK_THROWS_AS(build_from_source(cfg, idx, evil, "1.0"), UserError);
        CHECK_FALSE(fs::exists(sentinel));
    }
}

// ---------------------------------------------------------------------------
// F2 — archive symlink escape
// ---------------------------------------------------------------------------
TEST_SUITE("fable::F2_archive_symlink") {

    TEST_CASE("extract_archive rejects absolute symlink targets") {
        TmpDir tmp;
        auto attacker = tmp.path / "attacker";
        fs::create_directories(attacker);
        auto archive_path = tmp.path / "evil.tar";
        auto dest = tmp.path / "dest";
        fs::create_directories(dest);

        // Craft a tar: symlink pkg/evil -> absolute attacker dir, then a file
        // that would write through it if the symlink were accepted.
        // Use bsdtar/tar with a staging tree.
        auto stage = tmp.path / "stage";
        fs::create_directories(stage / "pkg");
        // Create the symlink with absolute target.
        std::error_code ec;
        fs::create_directory_symlink(attacker, stage / "pkg" / "evil", ec);
        REQUIRE_FALSE(ec);
        // Put payload next to the symlink in the archive as pkg/evil/payload —
        // tar will record the symlink entry; we also add a regular file that
        // path-walks through it by creating content under a real dir and then
        // archiving carefully. Simpler: just archive the symlink alone and
        // assert extract refuses it.
        auto tar = run_tool({"tar", "-cf", archive_path.string(), "-C", stage.string(), "pkg"});
        REQUIRE(tar.spawned);
        REQUIRE(tar.exit_code == 0);

        CHECK_THROWS_AS(extract_archive(archive_path, dest), ArchiveError);
        // Nothing should land in the attacker dir.
        CHECK(fs::is_empty(attacker));
    }

    TEST_CASE("extract_archive allows relative in-keg .. symlinks like Homebrew bottles") {
        TmpDir tmp;
        auto archive_path = tmp.path / "ok.tar";
        auto dest = tmp.path / "dest";
        fs::create_directories(dest);
        auto stage = tmp.path / "stage" / "pkg";
        fs::create_directories(stage / "bin");
        fs::create_directories(stage / "share" / "doc");
        {
            std::ofstream out(stage / "bin" / "tool");
            out << "#!/bin/sh\necho ok\n";
        }
        std::error_code ec;
        // share/doc/tool -> ../../bin/tool (same pattern as git bottles)
        fs::create_directory_symlink(fs::path("../../bin/tool"), stage / "share" / "doc" / "tool",
                                     ec);
        REQUIRE_FALSE(ec);
        auto tar = run_tool(
            {"tar", "-cf", archive_path.string(), "-C", (tmp.path / "stage").string(), "pkg"});
        REQUIRE(tar.exit_code == 0);
        CHECK_NOTHROW(extract_archive(archive_path, dest));
        CHECK(fs::is_symlink(dest / "pkg" / "share" / "doc" / "tool"));
    }
}

// ---------------------------------------------------------------------------
// F3 — env_slug path traversal
// ---------------------------------------------------------------------------
TEST_SUITE("fable::F3_env_slug") {

    TEST_CASE("env_slug encodes dots so .. cannot navigate") {
        CHECK(env_slug("..") != "..");
        CHECK(env_slug("..").find("..") == std::string::npos);
        CHECK(env_slug("/..") != "..");
        CHECK(env_slug(".").find('.') == std::string::npos);
        // Must not be a bare navigation component.
        CHECK(env_slug("..") == "%2E%2E");
        CHECK(env_slug("/..") == "%2E%2E");
    }

    TEST_CASE("env_dir for .. stays under den_home/envs") {
        auto dir = env_dir("/home/den", "..");
        CHECK(dir == fs::path("/home/den/envs/%2E%2E"));
        // Canonical resolve of parent must not escape envs when joined under den_home.
        auto under = fs::path("/home/den/envs") / env_slug("..");
        CHECK(under.filename() == "%2E%2E");
    }
}

// ---------------------------------------------------------------------------
// F4 — self-update fail-closed without checksum
// ---------------------------------------------------------------------------
TEST_SUITE("fable::F4_selfupdate") {

    TEST_CASE("apply_update refuses empty checksum_url") {
        Config cfg;
        UpdateInfo info;
        info.latest_version = "9.9.9";
        info.download_url = "https://example.invalid/den.tar.gz";
        info.checksum_url = "";
        CHECK_THROWS_AS(apply_update(info, cfg), UserError);
    }

    TEST_CASE("version_compare treats 1.0.0 as newer than 0.12.0") {
        CHECK(sat::version_compare("1.0.0", "0.12.0") > 0);
        CHECK(sat::version_compare("0.12.0", "1.0.0") < 0);
        CHECK(sat::version_compare("1.0.0", "1.0.0") == 0);
    }
}

// ---------------------------------------------------------------------------
// F5 — corrupt manifest is not treated as empty
// ---------------------------------------------------------------------------
TEST_SUITE("fable::F5_manifest") {

    TEST_CASE("read_manifest throws on corrupt present file") {
        TmpDir tmp;
        auto den_home = tmp.path;
        auto man_dir = den_home / "manifests" / "ROOT";
        fs::create_directories(man_dir);
        {
            std::ofstream out(man_dir / "manifest.json");
            out << "{\"packages\": {\"homebrew\": {\"jq\": \"1.0\"}}\n"; // truncated
        }
        CHECK_THROWS_AS(read_manifest(den_home, "/"), UserError);
    }

    TEST_CASE("read_manifest returns empty for absent file") {
        TmpDir tmp;
        auto m = read_manifest(tmp.path, "/");
        CHECK(m.packages.empty());
    }

    TEST_CASE("with_manifest does not clobber corrupt file on install-style mutation") {
        TmpDir tmp;
        auto den_home = tmp.path;
        auto man_path = den_home / "manifests" / "ROOT" / "manifest.json";
        fs::create_directories(man_path.parent_path());
        const std::string original =
            R"({"packages":{"homebrew":{"a":"1","b":"2"}},"auto_deps":{}})";
        {
            std::ofstream out(man_path);
            out << "{not valid json";
        }
        CHECK_THROWS(
            with_manifest(den_home, "/", [](Manifest& m) { m.packages["homebrew"]["c"] = "3"; }));
        // File must still be the corrupt original (not rewritten to only c).
        std::ifstream in(man_path);
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(content.find("not valid json") != std::string::npos);
        CHECK(content.find("\"c\"") == std::string::npos);
    }
}

} // namespace test
} // namespace den
