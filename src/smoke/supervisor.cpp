// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// 🎯T61 — Quick smoke check: supervisor without launchctl
//
// This compilation unit registers a smoke check that verifies den's built-in
// supervisor is reachable and does not delegate to launchctl.  It is called
// from the smoke runner when supervisor smoke checks are requested.
//
// UPSTREAM GAPS (as of 2026-05-17):
//   T61 — The services CLI (src/cli/cli.cpp:876-980) is entirely backed by
//          launchctl.  A built-in supervisor does not exist yet.  The check
//          below succeeds vacuously (the services subcommand parses) and
//          documents the expected shape once T61 lands.
//   T33 — Restart policies not implemented.
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

} // namespace

SupervisorSmokeResult run_supervisor_smoke(const fs::path& den_binary,
                                           const fs::path& scratch_dir) {
    SupervisorSmokeResult result;

    // -----------------------------------------------------------------------
    // 1. `den services list` subcommand must parse without error.
    // -----------------------------------------------------------------------
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

    std::string list_cmd = "PATH=" + path_env + " DEN_HOME=" + den_home.string() +
                           " HOMEBREW_PREFIX=" + prefix + " HOMEBREW_CELLAR=" + cellar + " " +
                           den_binary.string() + " services list";
    auto [rc, out] = shell(list_cmd);

    bool no_parse_error = out.find("No such subcommand") == std::string::npos &&
                          out.find("No such command") == std::string::npos;

    result.checks.push_back(
        {"services list parses without error", no_parse_error, no_parse_error ? "" : out});

    // -----------------------------------------------------------------------
    // 2. launchctl spy must NOT have been invoked (T61 acceptance criterion).
    //    EXPECTED FAILURE until T61 lands: the current implementation calls
    //    launchctl, so this check will fail on macOS.
    // -----------------------------------------------------------------------
    bool no_launchctl = true;
    if (fs::exists(invoc_log)) {
        std::ifstream f(invoc_log);
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        no_launchctl = content.empty();
    }

    // We report but do not hard-fail: the upstream T61 work must fix this.
    if (!no_launchctl) {
        SPDLOG_WARN("supervisor smoke: launchctl was invoked by 'den services list' — "
                    "T61 requires a built-in supervisor with no launchctl calls");
    }
    result.checks.push_back(
        {"services list does not invoke launchctl (T61)", no_launchctl,
         no_launchctl ? "" : "launchctl was invoked; see " + invoc_log.string()});

    // -----------------------------------------------------------------------
    // 3. Supervisor-specific subcommands (status, logs) — expected absent.
    //    Documented here as the target shape for T61.
    // -----------------------------------------------------------------------
    // T61 requires `den services status <name>` and `den services logs <name>`.
    // These subcommands do not exist yet. We record this as a known gap.
    SPDLOG_INFO("supervisor smoke: NOTE — 'den services status' and 'den services logs' "
                "are not yet implemented (T61 upstream gap)");
    result.checks.push_back({"services status/logs subcommands present (T61 upstream gap)", false,
                             "Not implemented yet; blocked on T61"});

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
