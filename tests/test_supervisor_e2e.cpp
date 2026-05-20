// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// 🎯T33 / 🎯T61 — Verification harness: built-in supervisor without launchctl
//
// This file exercises den's built-in supervisor (src/supervisor/) against a
// fleet of tiny shell-script fake services and asserts:
//
//   1. A service starts under den's supervisor; PID is captured.       [T61]
//   2. `den services status` reports the live PID and uptime.          [T61]
//   3. `den services list` shows the running service with PID column.  [T61]
//   4. `den services stop` terminates the process gracefully.          [T61]
//   5. `den services logs` shows captured stdout.                      [T61]
//   6. No launchctl binary is invoked at any point (PATH spy).         [T61]
//   7. Restart policy: always / on-failure / never.                    [T33]
//   8. Graceful shutdown ordering: dependents stop before deps.        [T33]
//   9. Daemon socket API exposes supervisor state.                     [T34]
//  10. Post-upgrade restart behaviour.                                 [T52]
//
// REMAINING UPSTREAM GAPS (T34, T52) keep their tests SKIPPED.

#include <doctest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace den {
namespace supervisor_e2e {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// RAII temp directory. Best-effort cleanup of any orphaned supervisor
// processes recorded under the directory's services/ tree.
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
        // Best-effort: SIGKILL any supervisors recorded under services/.
        auto svc_root = path / "services";
        if (fs::exists(svc_root)) {
            std::error_code ec;
            for (auto& e : fs::directory_iterator(svc_root, ec)) {
                if (!e.is_directory())
                    continue;
                auto sup_pid_file = e.path() / "sup.pid";
                std::ifstream f(sup_pid_file);
                if (f.is_open()) {
                    int pid = 0;
                    f >> pid;
                    if (pid > 0)
                        ::kill(static_cast<pid_t>(pid), SIGKILL);
                }
            }
        }
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

// Write a long-running heartbeat fake to a given path. Loops on SIGTERM.
static void write_heartbeat_script(const fs::path& script_path, const fs::path& log_path) {
    std::ofstream f(script_path);
    f << "#!/bin/sh\n"
      << "trap 'exit 0' TERM\n"
      << "while true; do\n"
      << "  echo heartbeat $$ >> " << log_path.string() << "\n"
      << "  sleep 1 &\n"
      << "  wait $!\n"
      << "done\n";
    f.close();
    fs::permissions(script_path,
                    fs::perms::owner_all | fs::perms::group_read | fs::perms::others_read,
                    fs::perm_options::replace);
}

// Write a script that exits with a given code immediately.
static void write_exit_script(const fs::path& script_path, int code) {
    std::ofstream f(script_path);
    f << "#!/bin/sh\nexit " << code << "\n";
    f.close();
    fs::permissions(script_path,
                    fs::perms::owner_all | fs::perms::group_read | fs::perms::others_read,
                    fs::perm_options::replace);
}

// Write a fake that logs "<tag>_stop <epoch_ns>" to a shared log on SIGTERM,
// optionally sleeping first.
static void write_ordered_stop_script(const fs::path& script_path, const fs::path& log_path,
                                      const std::string& tag, double pre_exit_sleep_secs) {
    std::ofstream f(script_path);
    f << "#!/bin/sh\n"
      << "log() {\n"
      << "  ts=$(date +%s%N 2>/dev/null || python3 -c 'import time;print(int(time.time()*1e9))')\n"
      << "  echo \"" << tag << "_stop $ts\" >> " << log_path.string() << "\n"
      << "}\n"
      << "trap 'sleep " << pre_exit_sleep_secs << "; log; exit 0' TERM\n"
      << "while true; do sleep 1 & wait $!; done\n";
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

// Write spec.json for a service into <den_home>/services/<name>/.
static void install_service(const std::string& den_home, const std::string& name,
                            const std::vector<std::string>& argv,
                            const std::string& restart = "never",
                            const std::vector<std::string>& depends_on = {}) {
    fs::path svc_dir = fs::path(den_home) / "services" / name;
    fs::create_directories(svc_dir);
    nlohmann::json spec;
    spec["name"] = name;
    spec["argv"] = argv;
    spec["env"] = nlohmann::json::object();
    spec["working_dir"] = "";
    spec["restart"] = restart;
    spec["depends_on"] = depends_on;
    std::ofstream f(svc_dir / "spec.json");
    f << spec.dump(2) << "\n";
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

// Read a small JSON file; returns the parsed object or an empty json on
// failure.
static nlohmann::json read_json(const fs::path& p) {
    std::ifstream f(p);
    if (!f.is_open())
        return nlohmann::json::object();
    try {
        nlohmann::json j;
        f >> j;
        return j;
    } catch (...) {
        return nlohmann::json::object();
    }
}

// Sleep helper.
static void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Poll for file content. Returns true when the file exists and contains
// `needle`. Times out after `timeout_ms`.
static bool wait_for_log_contains(const fs::path& path, const std::string& needle, int timeout_ms) {
    for (int waited = 0; waited < timeout_ms; waited += 50) {
        std::ifstream f(path);
        if (f.is_open()) {
            std::string content((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
            if (content.find(needle) != std::string::npos)
                return true;
        }
        sleep_ms(50);
    }
    return false;
}

// Wait up to `timeout_ms` for the given supervisor sup.pid file to disappear
// (or for the recorded pid to no longer be alive). Returns true if the
// supervisor finished within the timeout.
static bool wait_for_supervisor_exit(const fs::path& den_home, const std::string& name,
                                     int timeout_ms) {
    fs::path sup = fs::path(den_home) / "services" / name / "sup.pid";
    for (int waited = 0; waited < timeout_ms; waited += 50) {
        if (!fs::exists(sup))
            return true;
        std::ifstream f(sup);
        int pid = 0;
        f >> pid;
        if (pid > 0 && ::kill(static_cast<pid_t>(pid), 0) != 0)
            return true; // pid file lingers but pid is gone
        sleep_ms(50);
    }
    return false;
}

// ---------------------------------------------------------------------------
// Baseline: services CLI existence (always run)
// ---------------------------------------------------------------------------

TEST_SUITE("supervisor::baseline") {

    TEST_CASE("den services list subcommand exists") {
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
        CHECK(launchctl_not_invoked(invocation_log));
    }

} // TEST_SUITE supervisor::spy_sanity

// ---------------------------------------------------------------------------
// 🎯T61 core: supervisor start/stop/status/list without launchctl
// ---------------------------------------------------------------------------

TEST_CASE("services start does not invoke launchctl") {
    TempDir tmp;
    TempDir spy_dir;
    fs::path invocation_log = tmp.path / "launchctl-invocations.log";
    write_launchctl_spy(spy_dir.path, invocation_log);

    auto script = tmp.path / "heartbeat.sh";
    auto svc_log = tmp.path / "svc.log";
    write_heartbeat_script(script, svc_log);
    install_service(tmp.path.string(), "fake-svc", {script.string()}, "never");

    auto [rc, out] = run_den("services start fake-svc", tmp.path.string(), spy_dir.path.string());

    CHECK(launchctl_not_invoked(invocation_log));
    CHECK(out.find("Started") != std::string::npos);

    // Cleanup.
    run_den("services stop fake-svc", tmp.path.string(), spy_dir.path.string());
}

TEST_CASE("services status reports live PID and uptime") {
    TempDir tmp;
    TempDir spy_dir;
    fs::path invocation_log = tmp.path / "launchctl-invocations.log";
    write_launchctl_spy(spy_dir.path, invocation_log);

    auto script = tmp.path / "heartbeat.sh";
    auto svc_log = tmp.path / "svc.log";
    write_heartbeat_script(script, svc_log);
    install_service(tmp.path.string(), "fake-svc", {script.string()}, "never");

    run_den("services start fake-svc", tmp.path.string(), spy_dir.path.string());
    sleep_ms(150);

    auto [rc, out] = run_den("services status fake-svc", tmp.path.string(), spy_dir.path.string());

    CHECK(launchctl_not_invoked(invocation_log));
    CHECK(out.find("pid:") != std::string::npos);
    CHECK(out.find("uptime:") != std::string::npos);
    CHECK(out.find("running") != std::string::npos);

    run_den("services stop fake-svc", tmp.path.string(), spy_dir.path.string());
}

TEST_CASE("services list shows PID and uptime columns") {
    TempDir tmp;
    TempDir spy_dir;
    fs::path invocation_log = tmp.path / "launchctl-invocations.log";
    write_launchctl_spy(spy_dir.path, invocation_log);

    auto script = tmp.path / "heartbeat.sh";
    auto svc_log = tmp.path / "svc.log";
    write_heartbeat_script(script, svc_log);
    install_service(tmp.path.string(), "fake-svc", {script.string()}, "never");

    run_den("services start fake-svc", tmp.path.string(), spy_dir.path.string());
    sleep_ms(100);

    auto [rc, out] = run_den("services list", tmp.path.string(), spy_dir.path.string());

    CHECK(launchctl_not_invoked(invocation_log));
    CHECK(out.find("PID") != std::string::npos);
    CHECK(out.find("UPTIME") != std::string::npos);
    CHECK(out.find("fake-svc") != std::string::npos);
    CHECK(out.find("running") != std::string::npos);

    run_den("services stop fake-svc", tmp.path.string(), spy_dir.path.string());
}

TEST_CASE("services stop terminates gracefully") {
    TempDir tmp;
    TempDir spy_dir;
    fs::path invocation_log = tmp.path / "launchctl-invocations.log";
    write_launchctl_spy(spy_dir.path, invocation_log);

    auto script = tmp.path / "heartbeat.sh";
    auto svc_log = tmp.path / "svc.log";
    write_heartbeat_script(script, svc_log);
    install_service(tmp.path.string(), "fake-svc", {script.string()}, "never");

    run_den("services start fake-svc", tmp.path.string(), spy_dir.path.string());
    sleep_ms(150);
    auto [rc, out] = run_den("services stop fake-svc", tmp.path.string(), spy_dir.path.string());

    CHECK(launchctl_not_invoked(invocation_log));
    CHECK(out.find("Stopped") != std::string::npos);
    CHECK(wait_for_supervisor_exit(tmp.path, "fake-svc", 3000));
}

TEST_CASE("services logs shows captured stdout") {
    TempDir tmp;
    TempDir spy_dir;
    fs::path invocation_log = tmp.path / "launchctl-invocations.log";
    write_launchctl_spy(spy_dir.path, invocation_log);

    // Service writes "hello supervisor" to stdout then loops on heartbeats.
    auto script = tmp.path / "talker.sh";
    {
        std::ofstream f(script);
        f << "#!/bin/sh\n"
          << "trap 'exit 0' TERM\n"
          << "echo hello supervisor\n"
          << "while true; do sleep 1 & wait $!; done\n";
        f.close();
        fs::permissions(script,
                        fs::perms::owner_all | fs::perms::group_read | fs::perms::others_read,
                        fs::perm_options::replace);
    }
    install_service(tmp.path.string(), "talker", {script.string()}, "never");

    run_den("services start talker", tmp.path.string(), spy_dir.path.string());

    // Wait for the service to have echoed at least once.
    fs::path log = tmp.path / "services" / "talker" / "log";
    REQUIRE(wait_for_log_contains(log, "hello supervisor", 3000));

    auto [rc, out] = run_den("services logs talker", tmp.path.string(), spy_dir.path.string());

    CHECK(launchctl_not_invoked(invocation_log));
    CHECK(rc == 0);
    CHECK(out.find("hello supervisor") != std::string::npos);

    run_den("services stop talker", tmp.path.string(), spy_dir.path.string());
}

// ---------------------------------------------------------------------------
// 🎯T33: Restart policies
// ---------------------------------------------------------------------------

TEST_CASE("restart policy 'always' restarts a crashed service") {
    TempDir tmp;
    TempDir spy_dir;
    write_launchctl_spy(spy_dir.path, tmp.path / "launchctl-invocations.log");

    // Service exits 0 immediately. With policy=always, supervisor restarts.
    auto script = tmp.path / "exit0.sh";
    write_exit_script(script, 0);
    install_service(tmp.path.string(), "ever", {script.string()}, "always");

    run_den("services start ever", tmp.path.string(), spy_dir.path.string());
    // Initial backoff is ~1s. Wait ~2.5s to see at least one restart.
    sleep_ms(2500);

    auto state = read_json(tmp.path / "services" / "ever" / "state.json");
    CHECK(state.contains("restart_count"));
    CHECK(state["restart_count"].get<int>() >= 1);

    run_den("services stop ever", tmp.path.string(), spy_dir.path.string());
}

TEST_CASE("restart policy 'on-failure' restarts on non-zero exit only") {
    TempDir tmp;
    TempDir spy_dir;
    write_launchctl_spy(spy_dir.path, tmp.path / "launchctl-invocations.log");

    // 1. Exit 0 + on-failure → no restart, supervisor exits cleanly.
    {
        auto script = tmp.path / "exit0.sh";
        write_exit_script(script, 0);
        install_service(tmp.path.string(), "of-zero", {script.string()}, "on-failure");
        run_den("services start of-zero", tmp.path.string(), spy_dir.path.string());
        CHECK(wait_for_supervisor_exit(tmp.path, "of-zero", 3000));
        auto state = read_json(tmp.path / "services" / "of-zero" / "state.json");
        CHECK(state["restart_count"].get<int>() == 0);
        CHECK(state["last_exit"].get<int>() == 0);
    }

    // 2. Exit 1 + on-failure → at least one restart within ~2.5s.
    {
        auto script = tmp.path / "exit1.sh";
        write_exit_script(script, 1);
        install_service(tmp.path.string(), "of-fail", {script.string()}, "on-failure");
        run_den("services start of-fail", tmp.path.string(), spy_dir.path.string());
        sleep_ms(2500);
        auto state = read_json(tmp.path / "services" / "of-fail" / "state.json");
        CHECK(state["restart_count"].get<int>() >= 1);
        CHECK(state["last_exit"].get<int>() == 1);
        run_den("services stop of-fail", tmp.path.string(), spy_dir.path.string());
    }
}

TEST_CASE("restart policy 'never' does not restart after exit") {
    TempDir tmp;
    TempDir spy_dir;
    write_launchctl_spy(spy_dir.path, tmp.path / "launchctl-invocations.log");

    auto script = tmp.path / "exit0.sh";
    write_exit_script(script, 0);
    install_service(tmp.path.string(), "once", {script.string()}, "never");

    run_den("services start once", tmp.path.string(), spy_dir.path.string());

    // The supervisor should exit on its own once the service finishes.
    CHECK(wait_for_supervisor_exit(tmp.path, "once", 3000));

    auto state = read_json(tmp.path / "services" / "once" / "state.json");
    CHECK(state["restart_count"].get<int>() == 0);
}

TEST_CASE("graceful shutdown ordering: dependents stop before dependencies") {
    TempDir tmp;
    TempDir spy_dir;
    write_launchctl_spy(spy_dir.path, tmp.path / "launchctl-invocations.log");

    fs::path order_log = tmp.path / "order.log";

    // Service B is a dependency. It logs immediately on SIGTERM.
    auto script_b = tmp.path / "b.sh";
    write_ordered_stop_script(script_b, order_log, "B", 0.0);

    // Service A depends on B. It sleeps 0.4s before logging its stop, so any
    // stop-A-after-B ordering would have B's line appear FIRST in the log.
    auto script_a = tmp.path / "a.sh";
    write_ordered_stop_script(script_a, order_log, "A", 0.4);

    install_service(tmp.path.string(), "B", {script_b.string()}, "never", {});
    install_service(tmp.path.string(), "A", {script_a.string()}, "never", {"B"});

    auto [rc_start_a, out_start_a] =
        run_den("services start A", tmp.path.string(), spy_dir.path.string());
    auto [rc_start_b, out_start_b] =
        run_den("services start B", tmp.path.string(), spy_dir.path.string());
    CAPTURE(out_start_a);
    CAPTURE(out_start_b);
    REQUIRE(out_start_a.find("Started") != std::string::npos);
    REQUIRE(out_start_b.find("Started") != std::string::npos);
    sleep_ms(300);

    // Stop both — order is determined by depends_on, not argv order.
    auto [rc_stop, out_stop] =
        run_den("services stop B A", tmp.path.string(), spy_dir.path.string());
    CAPTURE(out_stop);
    CHECK(wait_for_supervisor_exit(tmp.path, "A", 3000));
    CHECK(wait_for_supervisor_exit(tmp.path, "B", 3000));
    sleep_ms(200); // allow stragglers to flush

    std::ifstream f(order_log);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto pos_a = content.find("A_stop");
    auto pos_b = content.find("B_stop");
    CAPTURE(content);
    CAPTURE(order_log.string());
    REQUIRE(pos_a != std::string::npos);
    REQUIRE(pos_b != std::string::npos);
    CHECK(pos_a < pos_b);
}

// ---------------------------------------------------------------------------
// T34: Daemon socket API (SKIPPED — IPC socket absent)
// ---------------------------------------------------------------------------

TEST_CASE("socket API exposes supervisor state to external den processes" * doctest::skip(true) *
          doctest::description("T34: daemon socket API not yet implemented")) {
    CHECK(false);
}

TEST_CASE("socket API survives daemon restart" * doctest::skip(true) *
          doctest::description("T34: daemon socket API not yet implemented")) {
    CHECK(false);
}

// ---------------------------------------------------------------------------
// T52: Post-upgrade restart behaviour (SKIPPED — deferral policy absent)
// ---------------------------------------------------------------------------

TEST_CASE("supervisor restarts service cleanly after binary upgrade" * doctest::skip(true) *
          doctest::description("T52: post-upgrade restart policy not yet implemented")) {
    CHECK(false);
}

TEST_CASE("supervisor defers restart per T52 deferral policy" * doctest::skip(true) *
          doctest::description("T52: post-upgrade deferral policy not yet implemented")) {
    CHECK(false);
}

} // namespace supervisor_e2e
} // namespace den
