// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// 🎯T33 / 🎯T61 — Quick smoke check: built-in supervisor without launchctl
//
// Verifies that den's services CLI is reachable AND that it never delegates
// to launchctl. Also checks that the supervisor-specific subcommands
// (status, logs) are present.
//
// Remaining gaps tracked elsewhere:
//   T34 — Daemon socket API not implemented.
//   T52 — Post-upgrade restart behaviour not implemented.

#include "supervisor.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace den {

namespace fs = std::filesystem;

namespace {

// Run a shell command and return {exit_code, combined stdout+stderr}.
std::pair<int, std::string> shell(const std::string& cmd) {
    FILE* pipe = ::popen((cmd + " 2>&1").c_str(), "r");
    if (!pipe)
        return {-1, "popen failed"};
    std::string out;
    std::array<char, 1024> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe))
        out += buf.data();
    int status = ::pclose(pipe);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return {code, out};
}

// Write a spy launchctl binary that records invocations and exits non-zero.
fs::path make_launchctl_spy(const fs::path& dir, const fs::path& log) {
    auto spy = dir / "launchctl";
    std::ofstream f(spy);
    f << "#!/bin/sh\n"
      << "echo launchctl-invoked: \"$@\" >> " << log.string() << "\n"
      << "exit 1\n";
    f.close();
    fs::permissions(spy, fs::perms::owner_all | fs::perms::group_read | fs::perms::others_read,
                    fs::perm_options::replace);
    return spy;
}

bool subcommand_present(const std::string& out) {
    return out.find("No such subcommand") == std::string::npos &&
           out.find("No such command") == std::string::npos &&
           out.find("requires a subcommand") == std::string::npos;
}

} // namespace

SupervisorSmokeResult run_supervisor_smoke(const fs::path& den_binary,
                                           const fs::path& scratch_dir) {
    SupervisorSmokeResult result;

    fs::path spy_dir = scratch_dir / "spy-bin";
    fs::path invoc_log = scratch_dir / "launchctl-invocations.log";
    fs::path den_home = scratch_dir / "den-home";
    fs::create_directories(spy_dir);
    fs::create_directories(den_home);

    make_launchctl_spy(spy_dir, invoc_log);

    const char* real_path = std::getenv("PATH");
    std::string path_env = spy_dir.string() + ":" + (real_path ? real_path : "/usr/bin:/bin");
    std::string prefix = den_home.string() + "/brew";
    std::string cellar = prefix + "/Cellar";
    std::string env_prefix = "PATH=" + path_env + " DEN_HOME=" + den_home.string() +
                             " HOMEBREW_PREFIX=" + prefix + " HOMEBREW_CELLAR=" + cellar + " ";

    auto invoke = [&](const std::string& args) {
        return shell(env_prefix + den_binary.string() + " " + args);
    };

    // 1. `den services list` subcommand must parse without error.
    auto [rc_list, out_list] = invoke("services list");
    bool list_ok = subcommand_present(out_list);
    result.checks.push_back(
        {"services list parses without error", list_ok, list_ok ? "" : out_list});

    // 2. launchctl spy must NOT have been invoked (🎯T61 acceptance).
    bool no_launchctl = true;
    if (fs::exists(invoc_log)) {
        std::ifstream f(invoc_log);
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        no_launchctl = content.empty();
    }
    if (!no_launchctl) {
        SPDLOG_WARN("supervisor smoke: launchctl was invoked by 'den services list' — "
                    "T61 requires a built-in supervisor with no launchctl calls");
    }
    result.checks.push_back(
        {"services list does not invoke launchctl (🎯T61)", no_launchctl,
         no_launchctl ? "" : "launchctl was invoked; see " + invoc_log.string()});

    // 3. Supervisor-specific subcommands must exist.
    auto [rc_status, out_status] = invoke("services status nonexistent");
    bool status_present = subcommand_present(out_status);
    result.checks.push_back({"services status subcommand present (🎯T61)", status_present,
                             status_present ? "" : out_status});

    auto [rc_logs, out_logs] = invoke("services logs nonexistent");
    bool logs_present = subcommand_present(out_logs);
    result.checks.push_back(
        {"services logs subcommand present (🎯T61)", logs_present, logs_present ? "" : out_logs});

    // Aggregate.
    result.all_passed = true;
    for (const auto& c : result.checks) {
        if (!c.passed) {
            result.all_passed = false;
        }
    }

    return result;
}

} // namespace den
