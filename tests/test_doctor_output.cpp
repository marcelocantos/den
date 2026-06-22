// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Tests for `den doctor` and `den config` output fields.
//
// 🎯T66 verification harness — host config and Cellar state.
//
// Acceptance coverage:
//   PASS  — den doctor runs and produces a summary (findings or "All checks passed.")
//   PASS  — den config reports den_home, store, cache, arch, homebrew_prefix, homebrew_cellar
//   SKIP  — Xcode version / CLT version / SDK path (not implemented in doctor.cpp)
//   SKIP  — Homebrew-config parity keys (HOMEBREW_PREFIX, HOMEBREW_CELLAR, etc. from `brew config`)
//   SKIP  — Linux host-fact equivalents (no Linux-specific doctor checks yet)
//
// These tests bind to the real, existing output format. When the missing
// checks land in doctor.cpp / cli.cpp, flip the SKIP markers to PASS.

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
namespace test_doctor {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helper: unique temporary directory (mkdtemp, removed on destruction).
// ---------------------------------------------------------------------------
struct TempDir {
    fs::path path;

    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "den_doctor_XXXXXX").string();
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
// Helper: run ./den with isolated DEN_HOME and fake Homebrew paths.
// Captures combined stdout+stderr.
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

static std::string run_den_fresh(const std::string& args) {
    TempDir tmp;
    return run_den(args, tmp.path.string());
}

// ===========================================================================
// Tests
// ===========================================================================

TEST_SUITE("T66::doctor_output") {

    // -----------------------------------------------------------------------
    // PASS — den doctor runs and emits a summary line.
    // -----------------------------------------------------------------------
    TEST_CASE("doctor produces a summary (findings or all-clear)") {
        const auto out = run_den_fresh("doctor");
        const bool has_findings = out.find("finding(s)") != std::string::npos;
        const bool all_passed = out.find("All checks passed") != std::string::npos;
        const bool your_system = out.find("Your system is ready") != std::string::npos;
        CHECK((has_findings || all_passed || your_system));
    }

    // -----------------------------------------------------------------------
    // PASS — on a brand-new home doctor reports warnings (missing dirs).
    // -----------------------------------------------------------------------
    TEST_CASE("doctor on fresh home reports missing-directory warnings") {
        const auto out = run_den_fresh("doctor");
        // A totally empty temp dir has no bin/ envs/ cache/ under den_home,
        // so doctor must emit at least one finding.
        CHECK(out.find("finding(s)") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // PASS — den config emits the basic path and arch fields that ARE present.
    // -----------------------------------------------------------------------
    TEST_CASE("config shows den_home, store, cache, arch, homebrew paths") {
        TempDir tmp;
        const auto out = run_den("config", tmp.path.string());

        // Keys that must be present in the current implementation.
        CHECK(out.find("den_home:") != std::string::npos);
        CHECK(out.find("store:") != std::string::npos);
        CHECK(out.find("cache:") != std::string::npos);
        CHECK(out.find("arch:") != std::string::npos);
        CHECK(out.find("homebrew_prefix:") != std::string::npos);
        CHECK(out.find("homebrew_cellar:") != std::string::npos);

        // The den_home value should match the TempDir path we injected.
        CHECK(out.find(tmp.path.string()) != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // PASS — arch value is one of the known valid strings.
    // -----------------------------------------------------------------------
    TEST_CASE("config arch is a recognised architecture string") {
        const auto out = run_den_fresh("config");
        const bool arm64 = out.find("aarch64") != std::string::npos;
        const bool x86_64 = out.find("x86_64") != std::string::npos;
        CHECK((arm64 || x86_64));
    }

    // -----------------------------------------------------------------------
    // PASS — macos_version field is present (may be "n/a" on Linux).
    // -----------------------------------------------------------------------
    TEST_CASE("config emits macos_version field") {
        const auto out = run_den_fresh("config");
        CHECK(out.find("macos_version:") != std::string::npos);
    }

#ifdef __APPLE__
    // -----------------------------------------------------------------------
    // den config reports the Xcode version field (🎯T66/T2).
    // -----------------------------------------------------------------------
    TEST_CASE("den config reports Xcode version") {
        const auto out = run_den_fresh("config");
        CHECK(out.find("xcode_version:") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // den config reports the Command Line Tools version field (🎯T66/T2).
    // -----------------------------------------------------------------------
    TEST_CASE("den config reports CLT version") {
        const auto out = run_den_fresh("config");
        CHECK(out.find("clt_version:") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // den config reports the SDK path field (🎯T66/T2).
    // -----------------------------------------------------------------------
    TEST_CASE("den config reports SDK path") {
        const auto out = run_den_fresh("config");
        CHECK(out.find("sdk_path:") != std::string::npos);
    }
#endif

    // -----------------------------------------------------------------------
    // den config mirrors the Homebrew `brew config` keys (🎯T66/T2).
    // The section header is always present; when brew is installed the
    // standard HOMEBREW_VERSION key is echoed under it.
    // -----------------------------------------------------------------------
    TEST_CASE("den config mirrors Homebrew config keys") {
        const auto out = run_den_fresh("config");
        CHECK(out.find("homebrew_config") != std::string::npos);
        // When brew is present, parity keys appear; otherwise the section
        // degrades gracefully to "n/a (brew not found)".
        const bool has_parity = out.find("HOMEBREW_VERSION:") != std::string::npos;
        const bool degraded = out.find("brew not found") != std::string::npos;
        CHECK((has_parity || degraded));
    }

#ifdef __linux__
    // -----------------------------------------------------------------------
    // den config reports Linux-equivalent host facts (🎯T66/T2).
    // -----------------------------------------------------------------------
    TEST_CASE("den config reports Linux host facts") {
        const auto out = run_den_fresh("config");
        CHECK(out.find("os_name:") != std::string::npos);
        CHECK(out.find("kernel:") != std::string::npos);
        CHECK(out.find("glibc:") != std::string::npos);
    }
#endif

} // TEST_SUITE T66::doctor_output

} // namespace test_doctor
} // namespace den
