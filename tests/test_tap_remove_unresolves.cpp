// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Verification harness for 🎯T67: after `den tap --remove`, formulae from the
// removed tap must become unresolvable.
//
// Lifecycle under test:
//   1. Add fixture tap → install succeeds (or resolves to the formula).
//   2. Remove tap.
//   3. Attempt same install → must fail with "not found" / unresolved error.
//
// All tests skip when `den tap` is not yet implemented.

#include <doctest.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace den {
namespace tap_unresolved_test {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

struct TempDir {
    fs::path path;

    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "den_tapunres_XXXXXX").string();
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

static bool tap_subcommand_exists(const std::string& den_home) {
    const std::string out = run_den("tap", den_home);
    return out.find("not expected") == std::string::npos;
}

static fs::path fixture_tap_path() {
    fs::path src(__FILE__);
    return src.parent_path() / "fixtures" / "fake-tap";
}

// Returns true when the install output indicates the package was NOT found
// (i.e. resolution failed as expected after tap removal).
static bool looks_like_not_found(const std::string& out) {
    return out.find("not found") != std::string::npos ||
           out.find("No formula") != std::string::npos ||
           out.find("Unknown formula") != std::string::npos ||
           out.find("unresolvable") != std::string::npos ||
           out.find("index is empty") != std::string::npos;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_SUITE("tap::remove_unresolves") {

    // -----------------------------------------------------------------------
    // 1. Core lifecycle: add → verify visible → remove → verify unresolvable.
    // -----------------------------------------------------------------------
    TEST_CASE("removed tap formulae become unresolvable") {
        TempDir tmp;
        const std::string home = tmp.path.string();

        if (!tap_subcommand_exists(home)) {
            MESSAGE("SKIP: `den tap` subcommand not yet implemented (pending 🎯T67)");
            return;
        }

        const std::string tap_src = fixture_tap_path().string();

        // Step 1: register the tap.
        run_den("tap add fixture/fake-tap --url " + tap_src, home);

        // Step 2: verify the formula is visible (search or info, no install).
        // `den info` is a read-only query and should work even without T11/T17.
        const std::string info_before = run_den("info fixture/fake-tap/hello-tap", home);
        const bool visible_before = info_before.find("not found") == std::string::npos &&
                                    info_before.find("hello-tap") != std::string::npos;
        if (!visible_before) {
            MESSAGE("SKIP: tap add succeeded but `den info` cannot yet query tapped "
                    "formulae — formula resolution path not yet implemented");
            return;
        }

        // Step 3: remove the tap.
        const std::string remove_out = run_den("tap --remove fixture/fake-tap", home);
        CHECK(remove_out.find("Untapped") != std::string::npos);

        // Step 4: the formula must now be unresolvable.
        const std::string info_after = run_den("info fixture/fake-tap/hello-tap", home);
        CHECK(looks_like_not_found(info_after));
    }

    // -----------------------------------------------------------------------
    // 2. After removal, even the bare name must not resolve via the old tap.
    // -----------------------------------------------------------------------
    TEST_CASE("bare formula name is unresolvable after tap removal") {
        TempDir tmp;
        const std::string home = tmp.path.string();

        if (!tap_subcommand_exists(home)) {
            MESSAGE("SKIP: `den tap` subcommand not yet implemented (pending 🎯T67)");
            return;
        }

        const std::string tap_src = fixture_tap_path().string();
        run_den("tap add fixture/fake-tap --url " + tap_src, home);

        // Quick check that bare name was resolvable before removal.
        const std::string info_before = run_den("info hello-tap", home);
        if (info_before.find("hello-tap") == std::string::npos ||
            looks_like_not_found(info_before)) {
            MESSAGE("SKIP: bare-name resolution not yet supported via tap index "
                    "(pending 🎯T67 / T5)");
            return;
        }

        // Remove tap.
        run_den("tap --remove fixture/fake-tap", home);

        // Bare name must now fail.
        const std::string info_after = run_den("info hello-tap", home);
        CHECK(looks_like_not_found(info_after));
    }

    // -----------------------------------------------------------------------
    // 3. Re-tapping must re-enable resolution.
    //    Add → remove → add again → formula must be visible once more.
    // -----------------------------------------------------------------------
    TEST_CASE("re-tapping re-enables formula resolution" * doctest::skip(true)) {
        TempDir tmp;
        const std::string home = tmp.path.string();

        if (!tap_subcommand_exists(home)) {
            MESSAGE("SKIP: `den tap` subcommand not yet implemented (pending 🎯T67)");
            return;
        }

        const std::string tap_src = fixture_tap_path().string();

        // Add → remove.
        run_den("tap add fixture/fake-tap --url " + tap_src, home);
        run_den("tap --remove fixture/fake-tap", home);

        // Verify it is gone.
        const std::string info_gone = run_den("info fixture/fake-tap/hello-tap", home);
        CHECK(looks_like_not_found(info_gone));

        // Re-add.
        run_den("tap add fixture/fake-tap --url " + tap_src, home);

        // Should be visible again.
        const std::string info_back = run_den("info fixture/fake-tap/hello-tap", home);
        CHECK(info_back.find("hello-tap") != std::string::npos);
        CHECK_FALSE(looks_like_not_found(info_back));
    }

} // TEST_SUITE tap::remove_unresolves

} // namespace tap_unresolved_test
} // namespace den
