// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Regression tests for #161: `den tap --remove user/repo` removed
// taps/user/repo but left an empty taps/user directory behind. Removing the
// last tap of a user must also remove taps/user; removing one of two taps from
// the same user must leave the sibling (and taps/user) intact. A failed
// `tap add` must not leave an empty taps/user behind either.

#include "../src/core/config.h"
#include "../src/core/error.h"
#include "../src/tap/tap.h"

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
namespace tap_prune_test {

namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "den_tapprune_XXXXXX").string();
        if (!::mkdtemp(tmpl.data()))
            throw std::runtime_error(std::string("mkdtemp failed: ") + std::strerror(errno));
        path = tmpl;
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

// A minimal local tap source with one formula.
static fs::path make_tap_source(const fs::path& root, const std::string& name) {
    const fs::path dir = root / "src" / name;
    fs::create_directories(dir / "Formula");
    std::ofstream(dir / "Formula" / (name + ".rb"))
        << "class X < Formula\n  url \"https://example.com/x-1.0.tar.gz\"\nend\n";
    return dir;
}

static Config make_config(const fs::path& root) {
    Config cfg;
    cfg.den_home = root / "den";
    fs::create_directories(cfg.den_home);
    return cfg;
}

TEST_SUITE("tap::remove_prunes_user_dir") {

    TEST_CASE("removing a user's only tap removes taps/<user>") {
        TempDir tmp;
        const Config cfg = make_config(tmp.path);
        tap_add(cfg, "someuser/sometap", make_tap_source(tmp.path, "one").string());
        REQUIRE(fs::is_directory(cfg.den_home / "taps" / "someuser" / "sometap"));

        tap_remove(cfg, "someuser/sometap");

        CHECK_FALSE(fs::exists(cfg.den_home / "taps" / "someuser" / "sometap"));
        CHECK_FALSE(fs::exists(cfg.den_home / "taps" / "someuser"));
        CHECK(tap_list(cfg).empty());
    }

    TEST_CASE("removing one of two taps from a user keeps the sibling") {
        TempDir tmp;
        const Config cfg = make_config(tmp.path);
        tap_add(cfg, "someuser/first", make_tap_source(tmp.path, "first").string());
        tap_add(cfg, "someuser/second", make_tap_source(tmp.path, "second").string());

        tap_remove(cfg, "someuser/first");

        CHECK_FALSE(fs::exists(cfg.den_home / "taps" / "someuser" / "first"));
        CHECK(fs::is_regular_file(cfg.den_home / "taps" / "someuser" / "second" / "Formula" /
                                  "second.rb"));
        auto taps = tap_list(cfg);
        REQUIRE(taps.size() == 1);
        CHECK(taps[0].name == "someuser/second");

        // Removing the last one now clears taps/someuser too.
        tap_remove(cfg, "someuser/second");
        CHECK_FALSE(fs::exists(cfg.den_home / "taps" / "someuser"));
    }

    TEST_CASE("a failed tap add leaves no empty taps/<user>") {
        TempDir tmp;
        const Config cfg = make_config(tmp.path);
        CHECK_THROWS_AS(tap_add(cfg, "someuser/missing", (tmp.path / "does-not-exist").string()),
                        UserError);
        CHECK_FALSE(fs::exists(cfg.den_home / "taps" / "someuser"));
    }

} // TEST_SUITE tap::remove_prunes_user_dir

} // namespace tap_prune_test
} // namespace den
