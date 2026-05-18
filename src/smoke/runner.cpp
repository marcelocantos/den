// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "runner.h"

#include "../build/source_build.h"
#include "../cli/install.h"
#include "../env/environment.h"
#include "../env/manifest.h"
#include "../index/index.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

namespace den {

namespace {

using json = nlohmann::json;

/// Run a shell command and capture stdout+stderr. Returns {exit_code, output}.
std::pair<int, std::string> run_cmd(const std::string& cmd) {
    std::string output;
    std::array<char, 4096> buf{};
    FILE* pipe = popen((cmd + " 2>&1").c_str(), "r");
    if (!pipe)
        return {-1, "popen failed"};
    while (fgets(buf.data(), buf.size(), pipe)) {
        output += buf.data();
    }
    int status = pclose(pipe);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return {exit_code, output};
}

/// Run a check that executes a command relative to the package prefix.
CheckResult run_cmd_check(const fs::path& pkg_prefix, const std::string& cmd, int expect_exit,
                          const std::string& expect_stdout) {
    CheckResult r;
    r.description = cmd;

    // Resolve the command relative to the package prefix.
    // The first word of cmd is the binary path.
    std::string full_cmd = cmd;
    if (cmd.substr(0, 4) == "bin/" || cmd.substr(0, 4) == "lib/" || cmd.substr(0, 6) == "sbin/") {
        full_cmd = (pkg_prefix / cmd.substr(0, cmd.find(' '))).string();
        auto space = cmd.find(' ');
        if (space != std::string::npos) {
            full_cmd += cmd.substr(space);
        }
    }

    auto [exit_code, output] = run_cmd(full_cmd);

    if (expect_exit >= 0 && exit_code != expect_exit) {
        r.passed = false;
        r.output = "expected exit " + std::to_string(expect_exit) + ", got " +
                   std::to_string(exit_code) + "\n" + output;
        return r;
    }

    if (!expect_stdout.empty()) {
        // Trim trailing newline from output.
        auto trimmed = output;
        while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r'))
            trimmed.pop_back();
        if (trimmed.find(expect_stdout) == std::string::npos) {
            r.passed = false;
            r.output = "expected stdout containing '" + expect_stdout + "', got:\n" + output;
            return r;
        }
    }

    r.passed = true;
    return r;
}

/// Run a check that executes a command in a fresh temp directory.
CheckResult run_tmpdir_check(const fs::path& pkg_prefix, const std::string& cmd, int expect_exit,
                             const std::string& expect_stdout) {
    // Create a temp directory for this check.
    auto tmpdir = fs::temp_directory_path() / ("den-smoke-" + std::to_string(std::rand()));
    fs::create_directories(tmpdir);

    CheckResult r;
    r.description = "tmpdir: " + cmd;

    // Build the full command: cd to tmpdir, resolve binary paths.
    std::string full_cmd = "cd " + tmpdir.string() + " && " + cmd;
    // Replace bin/ references with the package prefix.
    // Simple approach: set PATH to include the package prefix.
    full_cmd = "PATH=" + (pkg_prefix / "bin").string() + ":$PATH " + full_cmd;

    auto [exit_code, output] = run_cmd(full_cmd);

    if (expect_exit >= 0 && exit_code != expect_exit) {
        r.passed = false;
        r.output = "expected exit " + std::to_string(expect_exit) + ", got " +
                   std::to_string(exit_code) + "\n" + output;
    } else if (!expect_stdout.empty() && output.find(expect_stdout) == std::string::npos) {
        r.passed = false;
        r.output = "expected stdout containing '" + expect_stdout + "', got:\n" + output;
    } else {
        r.passed = true;
    }

    // Clean up.
    std::error_code ec;
    fs::remove_all(tmpdir, ec);
    return r;
}

/// Check that a file exists relative to the package prefix.
CheckResult check_file_exists(const fs::path& pkg_prefix, const std::string& rel_path) {
    CheckResult r;
    r.description = "file_exists: " + rel_path;
    auto full = pkg_prefix / rel_path;
    // Also check with .tbd extension (macOS stubs).
    r.passed = fs::exists(full) || fs::exists(fs::path(full.string() + ".tbd"));
    if (!r.passed) {
        r.output = "not found: " + full.string();
    }
    return r;
}

} // namespace

std::vector<SmokeResult> run_smoke_tests(const fs::path& test_defs_json, const Config& config,
                                         int max_packages) {
    // Load test definitions.
    std::ifstream f(test_defs_json);
    if (!f) {
        SPDLOG_ERROR("cannot open test definitions: {}", test_defs_json.string());
        return {};
    }
    json defs;
    f >> defs;

    // Load the package index.
    auto idx = load_index(config.cache / "index.json");
    if (idx.packages.empty()) {
        SPDLOG_ERROR("package index is empty — run `den update` first");
        return {};
    }

    auto tests = defs["tests"];
    std::vector<SmokeResult> results;
    int count = 0;

    for (const auto& test : tests) {
        if (max_packages > 0 && count >= max_packages)
            break;
        ++count;

        std::string name = test["name"];
        SmokeResult result;
        result.package_name = name;

        std::cout << "=== " << name << " ===" << std::endl;

        // Install the package into the shared config (uses content-addressed cache).
        auto* pkg = idx.find(name);
        if (!pkg) {
            SPDLOG_WARN("{}: not found in index, skipping", name);
            result.installed = false;
            results.push_back(std::move(result));
            continue;
        }

        bool from_source = test.value("source", false);
        try {
            if (from_source) {
                std::string version = pkg ? pkg->version : "unknown";
                build_from_source(config, idx, name, version);
                // Update manifest so materialise works.
                with_manifest(config.den_home, "/",
                              [&](Manifest& m) { m.packages[name] = version; });
                materialise(config.den_home, config.store, "/");
            } else {
                install_packages(config, idx, {name});
            }
            result.installed = true;
        } catch (const std::exception& e) {
            SPDLOG_ERROR("{}: install failed: {}", name, e.what());
            result.installed = false;
            results.push_back(std::move(result));
            continue;
        }

        // Determine the package prefix in the store.
        auto pkg_prefix = config.store / name / pkg->version;
        if (!fs::is_directory(pkg_prefix)) {
            SPDLOG_ERROR("{}: installed but prefix not found at {}", name, pkg_prefix.string());
            result.installed = false;
            results.push_back(std::move(result));
            continue;
        }

        // Run checks.
        for (const auto& check : test["checks"]) {
            if (check.contains("cmd")) {
                std::string cmd = check["cmd"];
                int expect_exit = check.value("expect_exit", 0);
                std::string expect_stdout = check.value("expect_stdout", "");
                result.checks.push_back(run_cmd_check(pkg_prefix, cmd, expect_exit, expect_stdout));
            } else if (check.contains("cmd_in_tmpdir")) {
                std::string cmd = check["cmd_in_tmpdir"];
                int expect_exit = check.value("expect_exit", 0);
                std::string expect_stdout = check.value("expect_stdout", "");
                result.checks.push_back(
                    run_tmpdir_check(pkg_prefix, cmd, expect_exit, expect_stdout));
            } else if (check.contains("file_exists")) {
                std::string path = check["file_exists"];
                result.checks.push_back(check_file_exists(pkg_prefix, path));
            }
        }

        // Report.
        int passed = result.pass_count();
        int total = static_cast<int>(result.checks.size());
        if (result.all_passed()) {
            std::cout << "  PASS (" << passed << "/" << total << " checks)\n";
        } else {
            std::cout << "  FAIL (" << passed << "/" << total << " checks)\n";
            for (const auto& c : result.checks) {
                if (!c.passed) {
                    std::cout << "    FAIL: " << c.description << "\n";
                    if (!c.output.empty()) {
                        std::cout << "      " << c.output.substr(0, 200) << "\n";
                    }
                }
            }
        }

        results.push_back(std::move(result));
    }

    // Summary.
    int total_pass = 0, total_fail = 0;
    for (const auto& r : results) {
        if (r.all_passed())
            ++total_pass;
        else
            ++total_fail;
    }
    std::cout << "\n=== Summary: " << total_pass << " passed, " << total_fail << " failed out of "
              << results.size() << " packages ===\n";

    return results;
}

} // namespace den
