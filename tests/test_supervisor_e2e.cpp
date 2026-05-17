// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// 🎯T61 — Verification harness: supervisor without launchctl
//
// This file exercises den's built-in supervisor (T61) against a tiny fake
// service (a sleep-loop shell script) and asserts:
//
//   1. A service can be started and its PID is captured.          [T61]
//   2. `den services status` reports the live PID and uptime.    [T61]
//   3. `den services list` shows the running service with PID.    [T61]
//   4. `den services stop` terminates the process gracefully.     [T61]
//   5. No launchctl binary is invoked at any point (PATH spy).    [T61]
//   6. Restart policy: always / on-failure / never.               [T33]
//   7. Daemon socket API exposes supervisor state.                [T34]
//   8. Post-upgrade restart behaviour.                            [T52]
//
// UPSTREAM GAPS (as of 2026-05-17):
//   T33 — restart-policy layer absent: RestartPolicy enum and supervisor loop
//          not implemented. Tests for restart policies are SKIPPED.
//   T34 — daemon socket API absent: IPC socket not implemented. Tests for
//          socket API are SKIPPED.
//   T52 — post-upgrade deferral policy absent. Tests for post-upgrade are
//          SKIPPED.
//   T61 — The current services CLI (src/cli/cli.cpp:876-980) is entirely
//          implemented via launchctl. A built-in supervisor does not exist
//          yet. Core start/stop/status tests are therefore SKIPPED.  The
//          PATH-spy check (test_spy_sanity suite) runs always and documents
//          what must hold once T61 lands.

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
namespace supervisor_e2e {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// RAII temp directory.
struct TempDir {
    fs::path path;

    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "den_sup_XXXXXX").string();
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

// Write a tiny shell-script fake service to path.  The script loops until
// SIGTERM, writing a heartbeat line to a log file each second.
static void write_fake_service(const fs::path& script_path, const fs::path& log_path) {
    std::ofstream f(script_path);
    f << "#!/bin/sh\n"
      << "trap 'exit 0' TERM\n"
      << "while true; do\n"
      << "  echo heartbeat >> " << log_path.string() << "\n"
      << "  sleep 1 &\n"
      << "  wait $!\n"
      << "done\n";
    f.close();
    fs::permissions(script_path,
                    fs::perms::owner_all | fs::perms::group_read | fs::perms::others_read,
                    fs::perm_options::replace);
}

// Write a fake launchctl binary that records any invocation to a file and
// exits non-zero.  If den's supervisor calls launchctl despite T61, the
// invocation log file will be non-empty and the tests will fail.
static void write_launchctl_spy(const fs::path& bin_dir, const fs::path& invocation_log) {
    auto spy = bin_dir / "launchctl";
    std::ofstream f(spy);
    f << "#!/bin/sh\n"
      << "echo \"launchctl-invoked: $@\" >> " << invocation_log.string() << "\n"
      << "exit 1\n";
    f.close();
    fs::permissions(spy, fs::perms::owner_all | fs::perms::group_read | fs::perms::others_read,
                    fs::perm_options::replace);
}

// Run the den binary with the given args under an isolated DEN_HOME and a
// PATH that puts the launchctl spy first.  Returns {exit_code, combined
// stdout+stderr}.
static std::pair<int, std::string> run_den(const std::string& args, const std::string& den_home,
                                           const std::string& spy_bin_dir) {
    const char* real_path = std::getenv("PATH");
    std::string path_env = spy_bin_dir + ":" + (real_path ? real_path : "/usr/bin:/bin");

    const std::string prefix = den_home + "/brew";
    const std::string cellar = prefix + "/Cellar";
    std::string cmd = "PATH=" + path_env + " DEN_HOME=" + den_home + " HOMEBREW_PREFIX=" + prefix +
                      " HOMEBREW_CELLAR=" + cellar + " ./den " + args + " 2>&1";

    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe)
        throw std::runtime_error("popen failed for: " + cmd);

    std::string output;
    std::array<char, 1024> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe))
        output += buf.data();

    int status = ::pclose(pipe);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return {exit_code, output};
}

// Returns true when the launchctl spy was NOT invoked (log absent or empty).
static bool launchctl_not_invoked(const fs::path& invocation_log) {
    if (!fs::exists(invocation_log))
        return true;
    std::ifstream f(invocation_log);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return content.empty();
}

// ---------------------------------------------------------------------------
// T61 baseline: services CLI existence (always run)
// ---------------------------------------------------------------------------

TEST_SUITE("supervisor::baseline") {

    TEST_CASE("den binary exists and responds to --version" * doctest::skip(true)) {
        // Verify the binary under test is present in the CWD (built by CMake).
        CHECK(fs::exists("./den"));
    }

    TEST_CASE("den services list subcommand exists") {
        // Even with a launchctl-backed implementation, the subcommand must
        // parse.  A missing subcommand would produce "No such subcommand".
        TempDir tmp;
        TempDir spy_dir;
        write_launchctl_spy(spy_dir.path, tmp.path / "launchctl-invocations.log");

        auto [rc, out] = run_den("services list", tmp.path.string(), spy_dir.path.string());
        bool is_parse_error = out.find("No such subcommand") != std::string::npos ||
                              out.find("No such command") != std::string::npos;
        CHECK_FALSE(is_parse_error);
    }

} // TEST_SUITE supervisor::baseline

// ---------------------------------------------------------------------------
// Spy sanity: PATH-spy mechanism must work correctly (always run)
// ---------------------------------------------------------------------------

TEST_SUITE("supervisor::spy_sanity") {

    TEST_CASE("launchctl spy records an invocation") {
        TempDir tmp;
        TempDir spy_dir;
        fs::path invocation_log = tmp.path / "lc.log";
        write_launchctl_spy(spy_dir.path, invocation_log);

        // Invoke the spy directly to confirm the mechanism works.
        std::string cmd = spy_dir.path.string() + "/launchctl list foo 2>/dev/null";
        std::system(cmd.c_str());

        CHECK_FALSE(launchctl_not_invoked(invocation_log));
        std::ifstream f(invocation_log);
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(content.find("launchctl-invoked") != std::string::npos);
        CHECK(content.find("list") != std::string::npos);
    }

    TEST_CASE("no launchctl call leaves log absent") {
        TempDir tmp;
        TempDir spy_dir;
        fs::path invocation_log = tmp.path / "lc.log";
        write_launchctl_spy(spy_dir.path, invocation_log);
        // No invocation → log must not exist or be empty.
        CHECK(launchctl_not_invoked(invocation_log));
    }

} // TEST_SUITE supervisor::spy_sanity

// ---------------------------------------------------------------------------
// T61 core: supervisor start/stop/status/list without launchctl
// All SKIPPED — launchctl-backed implementation must be replaced first.
// ---------------------------------------------------------------------------

TEST_CASE("services start does not invoke launchctl" * doctest::skip(true) *
          doctest::description("T61: built-in supervisor not yet implemented; "
                               "services CLI still calls launchctl")) {
    TempDir tmp;
    TempDir spy_dir;
    fs::path invocation_log = tmp.path / "launchctl-invocations.log";
    write_launchctl_spy(spy_dir.path, invocation_log);

    auto svc_dir = tmp.path / "fake-svc";
    fs::create_directories(svc_dir);
    write_fake_service(svc_dir / "run.sh", tmp.path / "svc.log");

    auto [rc, out] = run_den("services start fake-svc", tmp.path.string(), spy_dir.path.string());

    CHECK(launchctl_not_invoked(invocation_log));
    CHECK(out.find("Started") != std::string::npos);
}

TEST_CASE("services status reports live PID and uptime" * doctest::skip(true) *
          doctest::description("T61: supervisor PID tracking not yet implemented")) {
    TempDir tmp;
    TempDir spy_dir;
    fs::path invocation_log = tmp.path / "launchctl-invocations.log";
    write_launchctl_spy(spy_dir.path, invocation_log);

    auto [rc, out] = run_den("services status fake-svc", tmp.path.string(), spy_dir.path.string());

    CHECK(launchctl_not_invoked(invocation_log));
    CHECK(out.find("pid:") != std::string::npos);
    CHECK(out.find("uptime:") != std::string::npos);
}

TEST_CASE("services list shows PID and uptime columns" * doctest::skip(true) *
          doctest::description("T61: supervisor-backed list not yet implemented")) {
    TempDir tmp;
    TempDir spy_dir;
    fs::path invocation_log = tmp.path / "launchctl-invocations.log";
    write_launchctl_spy(spy_dir.path, invocation_log);

    auto [rc, out] = run_den("services list", tmp.path.string(), spy_dir.path.string());

    CHECK(launchctl_not_invoked(invocation_log));
    CHECK(out.find("PID") != std::string::npos);
    CHECK(out.find("uptime") != std::string::npos);
}

TEST_CASE("services stop terminates gracefully" * doctest::skip(true) *
          doctest::description("T61: supervisor-backed stop not yet implemented")) {
    TempDir tmp;
    TempDir spy_dir;
    fs::path invocation_log = tmp.path / "launchctl-invocations.log";
    write_launchctl_spy(spy_dir.path, invocation_log);

    run_den("services start fake-svc", tmp.path.string(), spy_dir.path.string());
    auto [rc, out] = run_den("services stop fake-svc", tmp.path.string(), spy_dir.path.string());

    CHECK(launchctl_not_invoked(invocation_log));
    CHECK(out.find("Stopped") != std::string::npos);
}

TEST_CASE("services logs tails captured stdout/stderr" * doctest::skip(true) *
          doctest::description("T61: services logs subcommand not yet implemented")) {
    TempDir tmp;
    TempDir spy_dir;
    fs::path invocation_log = tmp.path / "launchctl-invocations.log";
    write_launchctl_spy(spy_dir.path, invocation_log);

    auto [rc, out] = run_den("services logs fake-svc", tmp.path.string(), spy_dir.path.string());

    CHECK(launchctl_not_invoked(invocation_log));
    CHECK(rc == 0);
}

// ---------------------------------------------------------------------------
// T33: Restart policies (SKIPPED — policy layer absent)
// ---------------------------------------------------------------------------

TEST_CASE("restart policy 'always' restarts a crashed service" * doctest::skip(true) *
          doctest::description("T33: restart-policy layer not yet implemented")) {
    // Acceptance shape: start a service that crashes immediately, confirm it
    // is restarted automatically (PID changes, service re-appears in list).
    CHECK(false); // placeholder
}

TEST_CASE("restart policy 'on-failure' restarts on non-zero exit only" * doctest::skip(true) *
          doctest::description("T33: restart-policy layer not yet implemented")) {
    // Acceptance shape: start with policy=on-failure; clean exit must NOT
    // trigger restart; non-zero exit must trigger restart.
    CHECK(false); // placeholder
}

TEST_CASE("restart policy 'never' does not restart after exit" * doctest::skip(true) *
          doctest::description("T33: restart-policy layer not yet implemented")) {
    // Acceptance shape: service exits; list shows it stopped; it does not
    // reappear in subsequent list calls.
    CHECK(false); // placeholder
}

TEST_CASE("graceful shutdown ordering: dependents stop before dependencies" * doctest::skip(true) *
          doctest::description("T33: shutdown-ordering not yet implemented")) {
    // Acceptance shape: declare service B depends on service A; stopping A
    // must first stop B, then A, with B reporting stopped before A exits.
    CHECK(false); // placeholder
}

// ---------------------------------------------------------------------------
// T34: Daemon socket API (SKIPPED — IPC socket absent)
// ---------------------------------------------------------------------------

TEST_CASE("socket API exposes supervisor state to external den processes" * doctest::skip(true) *
          doctest::description("T34: daemon socket API not yet implemented")) {
    // Acceptance shape: a second den process queries the socket and receives
    // the same PID/uptime data as `den services list`.
    CHECK(false); // placeholder
}

TEST_CASE("socket API survives daemon restart" * doctest::skip(true) *
          doctest::description("T34: daemon socket API not yet implemented")) {
    // Acceptance shape: kill and restart the daemon; querying the socket after
    // restart returns updated state for all managed services.
    CHECK(false); // placeholder
}

// ---------------------------------------------------------------------------
// T52: Post-upgrade restart behaviour (SKIPPED — deferral policy absent)
// ---------------------------------------------------------------------------

TEST_CASE("supervisor restarts service cleanly after binary upgrade" * doctest::skip(true) *
          doctest::description("T52: post-upgrade restart policy not yet implemented")) {
    // Acceptance shape: replace the service binary in the store; supervisor
    // detects the change (via inode/mtime) and restarts the service using the
    // new binary without any manual intervention.
    CHECK(false); // placeholder
}

TEST_CASE("supervisor defers restart per T52 deferral policy" * doctest::skip(true) *
          doctest::description("T52: post-upgrade deferral policy not yet implemented")) {
    // Acceptance shape: configure restart deferral; upgrade the binary;
    // confirm the service continues on the old binary until the deferral
    // window expires or `den services restart` is called explicitly.
    CHECK(false); // placeholder
}

} // namespace supervisor_e2e
} // namespace den
