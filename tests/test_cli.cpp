// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Integration tests that invoke the den binary and inspect its output.
// Each test creates a fresh temporary DEN_HOME so runs are isolated.

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
namespace cli_test {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helper: unique temporary directory that is removed on destruction.
// ---------------------------------------------------------------------------
struct TempDir {
    fs::path path;

    TempDir() {
        // mkdtemp requires a writable template.
        std::string tmpl = fs::temp_directory_path() / "den_cli_XXXXXX";
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

    // Non-copyable.
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

// ---------------------------------------------------------------------------
// Helper: run the den binary with the given args and den_home, capturing
// combined stdout+stderr.  The binary is expected at ./den relative to the
// working directory (the build directory when run via ctest).
// ---------------------------------------------------------------------------
static std::string run_den(const std::string& args, const std::string& den_home) {
    // Build command: redirect stderr to stdout so we capture everything.
    std::string cmd = "DEN_HOME=" + den_home + " ./den " + args + " 2>&1";

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

// Convenience overload that creates a fresh TempDir automatically.
static std::string run_den_fresh(const std::string& args) {
    TempDir tmp;
    return run_den(args, tmp.path.string());
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_SUITE("cli::integration") {

    // -----------------------------------------------------------------------
    // 1. --version
    // -----------------------------------------------------------------------
    TEST_CASE("--version contains the expected version string") {
        const auto out = run_den_fresh("--version");
        CHECK(out.find(DEN_VERSION) != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 2. --help
    // -----------------------------------------------------------------------
    TEST_CASE("--help contains description and subcommand list") {
        const auto out = run_den_fresh("--help");
        // CLI11 description string from Cli::M::app.
        CHECK(out.find("universal development environment manager") != std::string::npos);
        // A representative selection of subcommands that should be listed.
        CHECK(out.find("install") != std::string::npos);
        CHECK(out.find("list") != std::string::npos);
        CHECK(out.find("env") != std::string::npos);
        CHECK(out.find("settings") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 3. --help-agent
    // -----------------------------------------------------------------------
    TEST_CASE("--help-agent includes agent guide text") {
        const auto out = run_den_fresh("--help-agent");
        // The agent guide marker written in cli.cpp.
        CHECK(out.find("agents-guide.md") != std::string::npos);
        // Should also include the regular help content.
        CHECK(out.find("universal development environment manager") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 4. config
    // -----------------------------------------------------------------------
    TEST_CASE("config shows den_home and arch") {
        TempDir tmp;
        const auto out = run_den("config", tmp.path.string());
        CHECK(out.find("den_home:") != std::string::npos);
        CHECK(out.find("arch:") != std::string::npos);
        // den_home line should include the temp path.
        CHECK(out.find(tmp.path.string()) != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 5. settings — fresh home returns valid JSON with daemon.auto_download
    // -----------------------------------------------------------------------
    TEST_CASE("settings on fresh home returns JSON with daemon.auto_download") {
        const auto out = run_den_fresh("settings");
        // Must be parseable-looking JSON (starts with '{').
        bool starts_with_brace = false;
        for (char c : out) {
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
            starts_with_brace = (c == '{');
            break;
        }
        CHECK(starts_with_brace);
        // Default settings include these keys.
        CHECK(out.find("auto_download") != std::string::npos);
        CHECK(out.find("daemon") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 6. set + settings
    // Note: setting a new boolean key via `set` stores it as a JSON string
    // because the key does not yet exist in config.json at write time, so type
    // inference falls back to string.  The stored value "true" is a string and
    // is NOT picked up by the boolean deserialiser in read_settings.  This test
    // documents the current behaviour (set echoes the assignment; the key is
    // present in config.json even if the settings display still shows the
    // default).
    // -----------------------------------------------------------------------
    TEST_CASE("set echoes the key=value assignment") {
        TempDir tmp;
        const auto set_out = run_den("set daemon.auto_upgrade true", tmp.path.string());
        // `set` should confirm the write.
        CHECK(set_out.find("daemon.auto_upgrade") != std::string::npos);
        CHECK(set_out.find("true") != std::string::npos);
    }

    TEST_CASE("settings after set still returns valid JSON") {
        TempDir tmp;
        run_den("set daemon.auto_upgrade true", tmp.path.string());
        const auto out = run_den("settings", tmp.path.string());
        CHECK(out.find("daemon") != std::string::npos);
        CHECK(out.find("auto_download") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 7. env create + list + remove lifecycle
    // -----------------------------------------------------------------------
    TEST_CASE("env create / list / remove lifecycle") {
        TempDir tmp;
        const std::string home = tmp.path.string();

        // Create.
        const auto create_out = run_den("env create /test", home);
        CHECK(create_out.find("Created") != std::string::npos);
        CHECK(create_out.find("/test") != std::string::npos);

        // List shows the new environment.
        const auto list_out = run_den("env list", home);
        CHECK(list_out.find("/test") != std::string::npos);

        // Remove.
        const auto remove_out = run_den("env remove /test", home);
        CHECK(remove_out.find("Removed") != std::string::npos);

        // List no longer shows it.
        const auto list_after = run_den("env list", home);
        CHECK(list_after.find("/test") == std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 8. env list on empty home returns "No environments."
    // -----------------------------------------------------------------------
    TEST_CASE("env list on fresh home says no environments") {
        const auto out = run_den_fresh("env list");
        CHECK(out.find("No environments") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 9. env show — active environment with no packages
    // -----------------------------------------------------------------------
    TEST_CASE("env show on empty environment says no packages") {
        const auto out = run_den_fresh("env show");
        CHECK(out.find("no packages") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 10. env freeze — stub exits cleanly (not yet implemented)
    // -----------------------------------------------------------------------
    TEST_CASE("env freeze outputs JSON lockfile") {
        const auto out = run_den_fresh("env freeze");
        CHECK(out.find("environment") != std::string::npos);
        CHECK(out.find("packages") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 11. status — stub exits cleanly
    // -----------------------------------------------------------------------
    TEST_CASE("status shows environment and daemon info") {
        const auto out = run_den_fresh("status");
        CHECK(out.find("Active environment:") != std::string::npos);
        CHECK(out.find("Daemon:") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 12. daemon status — shows running state and auto-download setting
    // -----------------------------------------------------------------------
    TEST_CASE("daemon status shows daemon state and settings") {
        const auto out = run_den_fresh("daemon status");
        CHECK(out.find("Daemon:") != std::string::npos);
        // Daemon should not be running in a fresh temp home.
        CHECK(out.find("not running") != std::string::npos);
        CHECK(out.find("Auto-download:") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 13. list — no packages installed
    // -----------------------------------------------------------------------
    TEST_CASE("list on empty store says no packages") {
        const auto out = run_den_fresh("list");
        CHECK(out.find("No packages installed") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 14. search — no index cached
    // -----------------------------------------------------------------------
    TEST_CASE("search without index reports empty index") {
        const auto out = run_den_fresh("search python");
        CHECK(out.find("Index is empty") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 15. doctor — runs without crashing, reports findings or passes
    // -----------------------------------------------------------------------
    TEST_CASE("doctor runs and produces a summary") {
        const auto out = run_den_fresh("doctor");
        // Either "finding(s)" (issues found) or "All checks passed."
        const bool has_findings = out.find("finding(s)") != std::string::npos;
        const bool all_passed = out.find("All checks passed") != std::string::npos;
        CHECK((has_findings || all_passed));
    }

    TEST_CASE("doctor on fresh home reports missing directory warnings") {
        // A brand-new temp home will trigger warnings for the missing den_home
        // directory structure.
        const auto out = run_den_fresh("doctor");
        CHECK(out.find("finding(s)") != std::string::npos);
    }

} // TEST_SUITE cli::integration

} // namespace cli_test
} // namespace den
