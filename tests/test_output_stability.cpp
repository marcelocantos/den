// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Tests for output stability of `den doctor` and `den config`.
//
// 🎯T66 verification harness — output stability for regression detection.
//
// Methodology:
//   - Invoke the command twice against the same isolated DEN_HOME.
//   - Assert that the output is byte-for-byte identical (deterministic output).
//   - Fields known to be volatile (timestamps, uptime) are excluded via the
//     helper filter_volatile_lines().
//
// Acceptance coverage:
//   PASS — den doctor output is stable across two runs.
//   PASS — den config output is stable across two runs.
//   NOTE — `den daemon status` last-check time is volatile; its stability
//           test strips the "Last check:" line before comparing.

#include <doctest.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace den {
namespace test_output_stability {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// RAII temp directory.
// ---------------------------------------------------------------------------
struct TempDir {
    fs::path path;

    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "den_stability_XXXXXX").string();
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
// Run ./den with an isolated DEN_HOME and fake Homebrew paths.
// ---------------------------------------------------------------------------
static std::string run_den(const std::string& args, const std::string& den_home) {
    const std::string prefix = den_home + "/brew";
    const std::string cellar = prefix + "/Cellar";
    std::string cmd = "DEN_HOME=" + den_home + " HOMEBREW_PREFIX=" + prefix +
                      " HOMEBREW_CELLAR=" + cellar + " ./den " + args + " 2>&1";
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
// Filter lines that are known to contain volatile values (timestamps, uptime,
// process IDs) so that two consecutive runs can be compared for stability.
//
// A line is dropped when any of the prefixes in `volatile_prefixes` matches
// the start of the trimmed line.
// ---------------------------------------------------------------------------
static std::string filter_volatile_lines(const std::string& output,
                                         const std::vector<std::string>& volatile_prefixes) {
    std::istringstream ss(output);
    std::string result;
    std::string line;
    while (std::getline(ss, line)) {
        bool drop = false;
        for (const auto& pfx : volatile_prefixes) {
            // Strip leading whitespace for comparison.
            size_t start = line.find_first_not_of(' ');
            std::string trimmed = (start == std::string::npos) ? "" : line.substr(start);
            if (trimmed.find(pfx) == 0) {
                drop = true;
                break;
            }
        }
        if (!drop) {
            result += line + "\n";
        }
    }
    return result;
}

// ===========================================================================
// Tests
// ===========================================================================

TEST_SUITE("T66::output_stability") {

    // -----------------------------------------------------------------------
    // den doctor — two runs on the same DEN_HOME must produce identical output.
    // -----------------------------------------------------------------------
    TEST_CASE("den doctor output is stable across two consecutive runs") {
        TempDir tmp;
        const std::string home = tmp.path.string();

        // No volatile lines in doctor output — it is purely structural.
        const auto out1 = run_den("doctor", home);
        const auto out2 = run_den("doctor", home);

        CHECK(out1 == out2);
    }

    // -----------------------------------------------------------------------
    // den config — deterministic (no timestamps).
    // -----------------------------------------------------------------------
    TEST_CASE("den config output is stable across two consecutive runs") {
        TempDir tmp;
        const std::string home = tmp.path.string();

        const auto out1 = run_den("config", home);
        const auto out2 = run_den("config", home);

        CHECK(out1 == out2);
    }

    // -----------------------------------------------------------------------
    // den daemon status — the "Last check:" line can change; strip it before
    // comparing.  Everything else must be stable.
    // -----------------------------------------------------------------------
    TEST_CASE("den daemon status output is stable except for Last-check timestamp") {
        TempDir tmp;
        const std::string home = tmp.path.string();

        // Known-volatile prefixes in daemon status output.
        const std::vector<std::string> volatile_prefixes = {
            "Last check:",  // e.g. "Last check: 3s ago"
        };

        const auto raw1 = run_den("daemon status", home);
        const auto raw2 = run_den("daemon status", home);

        const auto stable1 = filter_volatile_lines(raw1, volatile_prefixes);
        const auto stable2 = filter_volatile_lines(raw2, volatile_prefixes);

        CHECK(stable1 == stable2);
    }

    // -----------------------------------------------------------------------
    // den list — deterministic for a fixed Cellar.
    // -----------------------------------------------------------------------
    TEST_CASE("den list output is stable across two consecutive runs") {
        TempDir tmp;
        const std::string home = tmp.path.string();

        const auto out1 = run_den("list", home);
        const auto out2 = run_den("list", home);

        CHECK(out1 == out2);
    }

    // -----------------------------------------------------------------------
    // Stability under repeated doctor calls with an existing DEN_HOME that has
    // been partially initialised (to exercise more code paths).
    // -----------------------------------------------------------------------
    TEST_CASE("den doctor output is stable after den init-like directory creation") {
        TempDir tmp;
        const std::string home = tmp.path.string();

        // Create the expected subdirectories that doctor checks for.
        fs::create_directories(tmp.path / "bin");
        fs::create_directories(tmp.path / "envs");
        fs::create_directories(tmp.path / "cache");

        const auto out1 = run_den("doctor", home);
        const auto out2 = run_den("doctor", home);

        CHECK(out1 == out2);
    }

} // TEST_SUITE T66::output_stability

} // namespace test_output_stability
} // namespace den
