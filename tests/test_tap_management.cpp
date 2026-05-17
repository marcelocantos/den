// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Verification harness for 🎯T67: den tap add / tap --list / tap --remove.
//
// All tests that exercise unimplemented CLI surface detect the absence of the
// `tap` subcommand by inspecting the binary's exit output, emit a skip
// notice, and return immediately.  When T67 is implemented, the guard blocks
// will be removed and the tests will run for real.
//
// Fixture tap layout:
//   tests/fixtures/fake-tap/
//     Formula/hello-tap.rb   — one trivial formula
//
// Tests use an isolated DEN_HOME (temp dir) and a local path as the tap
// "remote" so no real network activity occurs.

#include <doctest.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace den {
namespace tap_test {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers (mirrors test_cli.cpp style)
// ---------------------------------------------------------------------------

struct TempDir {
    fs::path path;

    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "den_tap_XXXXXX").string();
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

// Run the den binary with isolated DEN_HOME, HOMEBREW_PREFIX, HOMEBREW_CELLAR.
// Returns combined stdout+stderr.
static std::string run_den(const std::string& args, const std::string& den_home) {
    const std::string prefix = den_home + "/brew";
    const std::string cellar = prefix + "/Cellar";
    std::string cmd = "DEN_HOME=" + den_home + " HOMEBREW_PREFIX=" + prefix +
                      " HOMEBREW_CELLAR=" + cellar + " ./den " + args + " 2>&1";

    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("popen failed: " + cmd);
    }

    std::string out;
    std::array<char, 1024> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
        out += buf.data();
    }
    ::pclose(pipe);
    return out;
}

// Returns true when den recognises the `tap` subcommand.
// CLI11 prints "The following argument was not expected" when a subcommand
// does not exist, so we probe with a bare `den tap` and look for that phrase.
static bool tap_subcommand_exists(const std::string& den_home) {
    const std::string out = run_den("tap", den_home);
    // If the subcommand is unknown CLI11 says "was not expected".
    if (out.find("not expected") != std::string::npos) {
        return false;
    }
    return true;
}

// Absolute path to the fixture tap directory, resolved relative to this
// source file at compile time via __FILE__.  Works as long as the build
// is performed from within the repo tree (standard CMake usage).
static fs::path fixture_tap_path() {
    fs::path src(__FILE__);
    // __FILE__ is tests/test_tap_management.cpp; fixtures sit alongside.
    return src.parent_path() / "fixtures" / "fake-tap";
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_SUITE("tap::management") {

    // -----------------------------------------------------------------------
    // 1. den tap add <local-path>
    //    Expected: clones (or symlinks) the tap into
    //    $DEN_HOME/taps/<user>/<repo>/ and registers it in config.
    // -----------------------------------------------------------------------
    TEST_CASE("tap add registers a local tap") {
        TempDir tmp;
        const std::string home = tmp.path.string();

        // Guard: skip if `den tap` is not yet implemented.
        if (!tap_subcommand_exists(home)) {
            MESSAGE("SKIP: `den tap` subcommand not yet implemented (pending 🎯T67)");
            return;
        }

        const std::string tap_src = fixture_tap_path().string();

        // den tap add <user/repo> --url <local-path>
        // Homebrew convention: tap name is "user/repo", stored at
        // $DEN_HOME/taps/user/repo/.
        const std::string out = run_den("tap add fixture/fake-tap --url " + tap_src, home);

        // Should confirm successful tap registration.
        CHECK(out.find("Tapped") != std::string::npos);

        // The tap directory should exist under den_home/taps/.
        const fs::path tap_dir = tmp.path / "taps" / "fixture" / "fake-tap";
        CHECK(fs::exists(tap_dir));

        // The fixture formula should be visible inside the tap.
        CHECK(fs::exists(tap_dir / "Formula" / "hello-tap.rb"));
    }

    // -----------------------------------------------------------------------
    // 2. den tap --list
    //    Expected: prints registered taps, one per line.
    // -----------------------------------------------------------------------
    TEST_CASE("tap --list shows registered taps") {
        TempDir tmp;
        const std::string home = tmp.path.string();

        if (!tap_subcommand_exists(home)) {
            MESSAGE("SKIP: `den tap` subcommand not yet implemented (pending 🎯T67)");
            return;
        }

        // Add the fixture tap first.
        const std::string tap_src = fixture_tap_path().string();
        run_den("tap add fixture/fake-tap --url " + tap_src, home);

        const std::string out = run_den("tap --list", home);
        CHECK(out.find("fixture/fake-tap") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 3. den tap --list on a fresh home
    //    Expected: empty list or a "no taps" message — never an error.
    // -----------------------------------------------------------------------
    TEST_CASE("tap --list on fresh home reports no taps") {
        TempDir tmp;
        const std::string home = tmp.path.string();

        if (!tap_subcommand_exists(home)) {
            MESSAGE("SKIP: `den tap` subcommand not yet implemented (pending 🎯T67)");
            return;
        }

        const std::string out = run_den("tap --list", home);
        // Either "No taps" message or simply empty output — must not crash.
        const bool no_taps_msg = out.find("No taps") != std::string::npos || out.empty();
        CHECK(no_taps_msg);
    }

    // -----------------------------------------------------------------------
    // 4. den tap --remove <user/repo>
    //    Expected: removes the tap directory and deregisters it.
    // -----------------------------------------------------------------------
    TEST_CASE("tap --remove deregisters a tap") {
        TempDir tmp;
        const std::string home = tmp.path.string();

        if (!tap_subcommand_exists(home)) {
            MESSAGE("SKIP: `den tap` subcommand not yet implemented (pending 🎯T67)");
            return;
        }

        // Add then immediately remove.
        const std::string tap_src = fixture_tap_path().string();
        run_den("tap add fixture/fake-tap --url " + tap_src, home);

        const std::string out = run_den("tap --remove fixture/fake-tap", home);
        CHECK(out.find("Untapped") != std::string::npos);

        // Tap directory should be gone.
        const fs::path tap_dir = tmp.path / "taps" / "fixture" / "fake-tap";
        CHECK_FALSE(fs::exists(tap_dir));

        // List should no longer show it.
        const std::string list_out = run_den("tap --list", home);
        CHECK(list_out.find("fixture/fake-tap") == std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 5. den tap --remove a tap that was never added
    //    Expected: a clear error, not a crash.
    // -----------------------------------------------------------------------
    TEST_CASE("tap --remove on unknown tap reports error") {
        TempDir tmp;
        const std::string home = tmp.path.string();

        if (!tap_subcommand_exists(home)) {
            MESSAGE("SKIP: `den tap` subcommand not yet implemented (pending 🎯T67)");
            return;
        }

        const std::string out = run_den("tap --remove nobody/nonexistent", home);
        // Must not silently succeed.
        const bool has_error = out.find("not tapped") != std::string::npos ||
                               out.find("not found") != std::string::npos ||
                               out.find("error") != std::string::npos ||
                               out.find("Error") != std::string::npos;
        CHECK(has_error);
    }

} // TEST_SUITE tap::management

} // namespace tap_test
} // namespace den
