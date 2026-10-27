// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Regression tests: `den tap add user/repo` must clone
// github.com/user/homebrew-repo (Homebrew's convention), not github.com/user/repo.
// Before the fix, `den tap add mutagen-io/mutagen` silently cloned the mutagen
// source repo, and `den tap add marcelocantos/tap` failed.

#include "../src/core/config.h"
#include "../src/tap/tap.h"

#include <doctest.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace den {
namespace tap_url_test {

namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "den_tapurl_XXXXXX").string();
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

static std::pair<int, std::string> sh(const std::string& cmd) {
    FILE* pipe = ::popen((cmd + " 2>&1").c_str(), "r");
    if (!pipe)
        throw std::runtime_error("popen failed: " + cmd);
    std::string out;
    std::array<char, 1024> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe))
        out += buf.data();
    int st = ::pclose(pipe);
    return {WIFEXITED(st) ? WEXITSTATUS(st) : -1, out};
}

TEST_SUITE("tap::default_url") {

    TEST_CASE("default_tap_url adds the homebrew- prefix") {
        CHECK(default_tap_url("marcelocantos/tap") ==
              "https://github.com/marcelocantos/homebrew-tap.git");
        CHECK(default_tap_url("mutagen-io/mutagen") ==
              "https://github.com/mutagen-io/homebrew-mutagen.git");
    }

    TEST_CASE("default_tap_url does not double an explicit homebrew- prefix") {
        CHECK(default_tap_url("mutagen-io/homebrew-mutagen") ==
              "https://github.com/mutagen-io/homebrew-mutagen.git");
    }

    TEST_CASE("default_tap_url rejects invalid names") {
        CHECK(default_tap_url("notatap").empty());
        CHECK(default_tap_url("a/b/c").empty());
        CHECK(default_tap_url("../x").empty());
    }

    TEST_CASE("canonical_tap_name strips homebrew- like brew does") {
        CHECK(canonical_tap_name("user/homebrew-repo") == "user/repo");
        CHECK(canonical_tap_name("user/repo") == "user/repo");
        // A repo named exactly "homebrew-" has nothing after the prefix.
        CHECK(canonical_tap_name("user/homebrew-") == "user/homebrew-");
        CHECK(canonical_tap_name("bogus") == "bogus");
    }

    TEST_CASE("tap_dir treats both spellings as one tap") {
        Config cfg;
        cfg.den_home = "/tmp/x";
        CHECK(tap_dir(cfg, "user/homebrew-repo") == tap_dir(cfg, "user/repo"));
    }

    // End to end: git's insteadOf rewrites https://github.com/ to a local
    // directory, so the test proves which URL `den tap add` asks git for
    // without touching the network.
    TEST_CASE("den tap add user/repo clones user/homebrew-repo") {
        if (!fs::exists("./den")) {
            MESSAGE("skipping: ./den not found (run from the build directory)");
            return;
        }
        if (sh("git --version").first != 0) {
            MESSAGE("skipping: git not available");
            return;
        }
        TempDir tmp;
        const fs::path remotes = tmp.path / "remotes";
        // Two repos: the tap (homebrew-mutagen) and a decoy source repo
        // (mutagen) that the old code cloned.
        for (const auto& [repo, formula] :
             {std::pair{"homebrew-mutagen", "right"}, std::pair{"mutagen", "decoy"}}) {
            const fs::path dir = remotes / "mutagen-io" / (std::string(repo) + ".git");
            fs::create_directories(dir / "Formula");
            std::ofstream(dir / "Formula" / (std::string(formula) + ".rb"))
                << "class X < Formula\n  url \"https://example.com/x-1.0.tar.gz\"\nend\n";
            auto [rc, out] = sh("cd '" + dir.string() +
                                "' && git init -q && git add -A && git -c user.name=t -c "
                                "user.email=t@t commit -qm init");
            REQUIRE_MESSAGE(rc == 0, out);
        }
        const fs::path gitcfg = tmp.path / "gitconfig";
        std::ofstream(gitcfg) << "[url \"file://" << remotes.string()
                              << "/\"]\n\tinsteadOf = https://github.com/\n"
                              << "[protocol \"file\"]\n\tallow = always\n";

        const std::string den_home = (tmp.path / "den").string();
        const std::string env =
            "GIT_CONFIG_GLOBAL=" + gitcfg.string() + " GIT_CONFIG_NOSYSTEM=1 DEN_HOME=" + den_home +
            " HOMEBREW_PREFIX=" + den_home + "/brew HOMEBREW_CELLAR=" + den_home + "/brew/Cellar ";
        auto [rc, out] = sh(env + "./den tap add mutagen-io/mutagen");
        REQUIRE_MESSAGE(rc == 0, out);
        const fs::path tapped = fs::path(den_home) / "taps" / "mutagen-io" / "mutagen";
        CHECK(fs::exists(tapped / "Formula" / "right.rb"));
        CHECK_FALSE(fs::exists(tapped / "Formula" / "decoy.rb"));

        // The long spelling names the same tap, so a second add is refused.
        auto [rc2, out2] = sh(env + "./den tap add mutagen-io/homebrew-mutagen");
        CHECK(rc2 != 0);
        CHECK(out2.find("already tapped") != std::string::npos);

        // And removal accepts either spelling.
        auto [rc3, out3] = sh(env + "./den tap --remove mutagen-io/homebrew-mutagen");
        CHECK_MESSAGE(rc3 == 0, out3);
        CHECK_FALSE(fs::exists(tapped));
    }
}

} // namespace tap_url_test
} // namespace den
