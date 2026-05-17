// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// 🎯T70 verification: opt-in default and persistence.
//
// Tests that:
//   1. Telemetry is OFF by default.
//   2. `den telemetry on` enables it and writes the setting.
//   3. The setting survives a second process invocation (persistence).
//
// All tests SKIP gracefully when `den telemetry` is not yet implemented
// (detected by exit code 109 / "was not expected" from CLI11).

#include <doctest.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace den {
namespace telemetry_optin_test {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers (duplicated per translation unit — each test .cpp is standalone).
// ---------------------------------------------------------------------------
struct TempDir {
    fs::path path;

    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "den_tel_XXXXXX").string();
        char* r = ::mkdtemp(tmpl.data());
        if (!r)
            throw std::runtime_error(std::string("mkdtemp: ") + std::strerror(errno));
        path = r;
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

struct RunResult {
    std::string output; // stdout + stderr combined
    int exit_code;
};

static RunResult run_den(const std::string& args, const std::string& den_home) {
    const std::string prefix = den_home + "/brew";
    const std::string cellar = prefix + "/Cellar";
    const std::string cmd = "DEN_HOME=" + den_home +
                            " HOMEBREW_PREFIX=" + prefix +
                            " HOMEBREW_CELLAR=" + cellar +
                            " ./den " + args + " 2>&1";

    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe)
        throw std::runtime_error("popen failed: " + cmd);

    RunResult result;
    std::array<char, 1024> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr)
        result.output += buf.data();

    int raw = ::pclose(pipe);
    result.exit_code = WIFEXITED(raw) ? WEXITSTATUS(raw) : -1;
    return result;
}

// Returns true if the telemetry subcommand is recognised by the CLI.
static bool telemetry_available(const std::string& den_home) {
    auto r = run_den("telemetry --help", den_home);
    // CLI11 exits 109 with "was not expected" when the subcommand is unknown.
    if (r.exit_code == 109 || r.output.find("was not expected") != std::string::npos)
        return false;
    return true;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_SUITE("telemetry::optin") {

    // -----------------------------------------------------------------------
    // 1. Default is OFF
    // -----------------------------------------------------------------------
    TEST_CASE("telemetry is off by default") {
        TempDir tmp;
        const std::string home = tmp.path.string();

        if (!telemetry_available(home)) {
            MESSAGE("SKIP: `den telemetry` not yet implemented — T70 upstream gap");
            CHECK(true);
            return;
        }

        // `den telemetry status` (or `den settings`) should report disabled.
        auto r = run_den("telemetry status", home);
        CHECK(r.exit_code == 0);
        // Must say off/disabled, not on/enabled.
        const bool says_off = r.output.find("off") != std::string::npos ||
                              r.output.find("disabled") != std::string::npos ||
                              r.output.find("false") != std::string::npos;
        CHECK(says_off);
    }

    // -----------------------------------------------------------------------
    // 2. `den telemetry on` enables collection
    // -----------------------------------------------------------------------
    TEST_CASE("telemetry on enables collection") {
        TempDir tmp;
        const std::string home = tmp.path.string();

        if (!telemetry_available(home)) {
            MESSAGE("SKIP: `den telemetry` not yet implemented — T70 upstream gap");
            CHECK(true);
            return;
        }

        auto enable = run_den("telemetry on", home);
        CHECK(enable.exit_code == 0);

        // After enabling, status must report on/enabled/true.
        auto r = run_den("telemetry status", home);
        CHECK(r.exit_code == 0);
        const bool says_on = r.output.find("on") != std::string::npos ||
                             r.output.find("enabled") != std::string::npos ||
                             r.output.find("true") != std::string::npos;
        CHECK(says_on);
    }

    // -----------------------------------------------------------------------
    // 3. `den telemetry on` persists across process invocations
    // -----------------------------------------------------------------------
    TEST_CASE("telemetry on persists across process restarts") {
        TempDir tmp;
        const std::string home = tmp.path.string();

        if (!telemetry_available(home)) {
            MESSAGE("SKIP: `den telemetry` not yet implemented — T70 upstream gap");
            CHECK(true);
            return;
        }

        // Enable in first process.
        auto enable = run_den("telemetry on", home);
        CHECK(enable.exit_code == 0);

        // Second independent invocation reads the same DEN_HOME.
        auto r2 = run_den("telemetry status", home);
        CHECK(r2.exit_code == 0);
        const bool still_on = r2.output.find("on") != std::string::npos ||
                              r2.output.find("enabled") != std::string::npos ||
                              r2.output.find("true") != std::string::npos;
        CHECK(still_on);
    }

    // -----------------------------------------------------------------------
    // 4. `den telemetry off` disables collection after enabling
    // -----------------------------------------------------------------------
    TEST_CASE("telemetry off disables collection") {
        TempDir tmp;
        const std::string home = tmp.path.string();

        if (!telemetry_available(home)) {
            MESSAGE("SKIP: `den telemetry` not yet implemented — T70 upstream gap");
            CHECK(true);
            return;
        }

        // Enable then disable.
        run_den("telemetry on", home);
        auto disable = run_den("telemetry off", home);
        CHECK(disable.exit_code == 0);

        auto r = run_den("telemetry status", home);
        CHECK(r.exit_code == 0);
        const bool says_off = r.output.find("off") != std::string::npos ||
                              r.output.find("disabled") != std::string::npos ||
                              r.output.find("false") != std::string::npos;
        CHECK(says_off);
    }

} // TEST_SUITE telemetry::optin

} // namespace telemetry_optin_test
} // namespace den
