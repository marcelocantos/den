// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Restarting dev.den.daemon via launchd: bootout gui/<uid>/<label> then
// bootstrap gui/<uid> <plist>, launchctl always run with a scrubbed
// environment, a new pid confirmed with exactly one codesigning spawn failure
// tolerated, and a detached helper (setsid, outlives the caller) for restarts
// triggered from inside the daemon's own job. Also: relocate never re-signs
// den's own binary.

#include "../src/build/relocate.h"
#include "../src/selfupdate/launchd_restart.h"

#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace den {
namespace test_launchd_restart {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("den-launchd-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string slurp(const fs::path& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::set<std::string> lines_of(const std::string& s) {
    std::set<std::string> out;
    std::istringstream in(s);
    std::string l;
    while (std::getline(in, l))
        if (!l.empty())
            out.insert(l);
    return out;
}

// Set caller-session variables that must never reach launchctl or the
// service; restore them afterwards.
struct SessionEnv {
    std::map<std::string, std::optional<std::string>> saved;
    SessionEnv() {
        for (const char* k :
             {"SSH_AUTH_SOCK", "SSH_CONNECTION", "SSH_TTY", "TERM", "DEN_TEST_SHELL_VAR"}) {
            const char* v = std::getenv(k);
            saved[k] = v ? std::optional<std::string>(v) : std::nullopt;
        }
        ::setenv("SSH_AUTH_SOCK", "/tmp/leak-agent.sock", 1);
        ::setenv("SSH_CONNECTION", "10.0.0.1 5555 10.0.0.2 22", 1);
        ::setenv("SSH_TTY", "/dev/ttys999", 1);
        ::setenv("TERM", "xterm-leak", 1);
        ::setenv("DEN_TEST_SHELL_VAR", "leak", 1);
    }
    ~SessionEnv() {
        for (auto& [k, v] : saved) {
            if (v)
                ::setenv(k.c_str(), v->c_str(), 1);
            else
                ::unsetenv(k.c_str());
        }
    }
};

void check_no_leak(const std::string& env_dump) {
    CHECK(env_dump.find("SSH_") == std::string::npos);
    CHECK(env_dump.find("TERM=") == std::string::npos);
    CHECK(env_dump.find("DEN_TEST_SHELL_VAR") == std::string::npos);
    CHECK(env_dump.find("leak") == std::string::npos);
}

// A stub launchctl: logs its args and environment; `print` reports a pid.
fs::path write_stub_launchctl(const fs::path& dir) {
    auto stub = dir / "launchctl";
    std::ofstream f(stub);
    f << "#!/bin/sh\n"
      << "echo \"launchctl $*\" >> '" << (dir / "calls.log").string() << "'\n"
      << "if [ \"$1\" = bootstrap ]; then /usr/bin/env > '" << (dir / "env.log").string()
      << "'; fi\n"
      << "if [ \"$1\" = print ]; then printf 'gui/1/x = {\\n\\tpid = 4242\\n}\\n'; fi\n"
      << "exit 0\n";
    f.close();
    fs::permissions(stub, fs::perms::owner_all);
    return stub;
}

// Scripted launchctl for restart_launchd_job: answers from queues, records calls.
struct FakeLaunchctl {
    std::vector<std::string> calls;
    std::deque<int> bootstrap_rcs;  // default 0
    std::deque<std::string> prints; // default: last element repeats
    std::chrono::steady_clock::time_point t{};

    LaunchdOps ops(std::chrono::seconds timeout = 15s) {
        LaunchdOps o;
        o.domain = "gui/501";
        o.label = "dev.den.daemon";
        o.plist = "/Users/x/Library/LaunchAgents/dev.den.daemon.plist";
        o.confirm_timeout = timeout;
        o.launchctl = [this](const std::vector<std::string>& args) {
            std::string c;
            for (const auto& a : args)
                c += (c.empty() ? "" : " ") + a;
            calls.push_back(c);
            SpawnResult r;
            r.spawned = true;
            r.exit_code = 0;
            if (args[0] == "bootstrap" && !bootstrap_rcs.empty()) {
                r.exit_code = bootstrap_rcs.front();
                bootstrap_rcs.pop_front();
                if (r.exit_code)
                    r.output = "Bootstrap failed: 5: Input/output error\n";
            } else if (args[0] == "print") {
                std::string p = prints.empty() ? "" : prints.front();
                if (prints.size() > 1)
                    prints.pop_front();
                if (p == "NOTLOADED") {
                    r.exit_code = 113;
                } else {
                    r.output = "gui/501/dev.den.daemon = {\n" + p + "}\n";
                }
            }
            return r;
        };
        o.sleep = [this](std::chrono::milliseconds d) { t += d; };
        o.now = [this] { return t; };
        return o;
    }
    size_t count(const std::string& prefix) const {
        return static_cast<size_t>(std::count_if(
            calls.begin(), calls.end(), [&](const auto& c) { return c.rfind(prefix, 0) == 0; }));
    }
};

TEST_SUITE("launchd_restart") {
    TEST_CASE("scrubbed_argv runs the tool under env -i with only HOME, USER and PATH") {
        auto argv = scrubbed_argv({"/Users/m", "m"}, "/bin/launchctl",
                                  {"bootstrap", "gui/501", "/p.plist"});
        CHECK(argv == std::vector<std::string>{"/usr/bin/env", "-i", "HOME=/Users/m", "USER=m",
                                               "PATH=/usr/bin:/bin:/usr/sbin:/sbin",
                                               "/bin/launchctl", "bootstrap", "gui/501",
                                               "/p.plist"});
    }

    TEST_CASE("run_scrubbed: the child sees only HOME, USER and PATH, never the session env") {
        SessionEnv session;
        auto r = run_scrubbed(scrubbed_argv({"/home/u", "u"}, "/usr/bin/env", {}));
        REQUIRE(r.spawned);
        CHECK(r.exit_code == 0);
        CHECK(lines_of(r.output) == std::set<std::string>{"HOME=/home/u", "USER=u",
                                                          "PATH=/usr/bin:/bin:/usr/sbin:/sbin"});
        check_no_leak(r.output);
    }

    TEST_CASE("real_launchd_ops never passes the caller's session env to launchctl") {
        SessionEnv session;
        TempDir tmp;
        auto stub = write_stub_launchctl(tmp.path);
        auto ops = real_launchd_ops("dev.den.daemon", tmp.path / "p.plist", stub.string());
        CHECK(ops.domain == "gui/" + std::to_string(::getuid()));
        auto r = ops.launchctl({"bootstrap", ops.domain, (tmp.path / "p.plist").string()});
        CHECK(r.exit_code == 0);
        auto env = slurp(tmp.path / "env.log");
        CHECK(env.find("PATH=/usr/bin:/bin:/usr/sbin:/sbin") != std::string::npos);
        CHECK(env.find("HOME=") != std::string::npos);
        check_no_leak(env);
    }

    TEST_CASE("resolve_launchctl takes the location from PATH, else /bin/launchctl") {
        TempDir tmp;
        auto stub = write_stub_launchctl(tmp.path);
        const char* old = std::getenv("PATH");
        std::string saved = old ? old : "";
        ::setenv("PATH", (tmp.path.string() + ":/usr/bin:/bin").c_str(), 1);
        CHECK(resolve_launchctl() == stub.string());
        ::setenv("PATH", "/nonexistent-den-dir", 1);
        CHECK(resolve_launchctl() == "/bin/launchctl");
        ::setenv("PATH", saved.c_str(), 1);
    }

    TEST_CASE("parse_launchctl_print reads the job's pid, runs and exit reason") {
        const std::string text = "gui/501/dev.den.daemon = {\n"
                                 "\tactive count = 1\n"
                                 "\tstate = running\n"
                                 "\tendpoints = {\n"
                                 "\t\t\"x\" = {\n"
                                 "\t\t\tpid = 1\n"
                                 "\t\t}\n"
                                 "\t}\n"
                                 "\truns = 3\n"
                                 "\tpid = 2931\n"
                                 "\tlast exit code = 0\n"
                                 "\tlast exit reason = OS_REASON_CODESIGNING\n"
                                 "}\n";
        auto st = parse_launchctl_print(true, text);
        CHECK(st.loaded);
        REQUIRE(st.pid);
        CHECK(*st.pid == 2931);
        CHECK(st.runs == 3);
        CHECK(st.codesign_failure());
        auto none = parse_launchctl_print(false, text);
        CHECK_FALSE(none.loaded);
        CHECK_FALSE(none.pid);
        auto fresh = parse_launchctl_print(true, "x = {\n\tstate = running\n\tpid = 7\n"
                                                 "\tlast exit code = (never exited)\n}\n");
        CHECK_FALSE(fresh.codesign_failure());
    }

    TEST_CASE("bootout gui/<uid>/<label>, then bootstrap gui/<uid> <plist>, then a new pid") {
        FakeLaunchctl f;
        f.prints = {"\tpid = 200\n"};
        std::ostringstream out;
        CHECK(restart_launchd_job(f.ops(), 100, out) == LaunchdRestartResult::Restarted);
        REQUIRE(f.calls.size() >= 3);
        CHECK(f.calls[0] == "bootout gui/501/dev.den.daemon");
        CHECK(f.calls[1] == "bootstrap gui/501 /Users/x/Library/LaunchAgents/dev.den.daemon.plist");
        CHECK(f.calls[2] == "print gui/501/dev.den.daemon");
        CHECK(f.count("kickstart") == 0);
    }

    TEST_CASE("a transient bootstrap failure is retried") {
        FakeLaunchctl f;
        f.bootstrap_rcs = {5, 5, 0};
        f.prints = {"\tpid = 200\n"};
        std::ostringstream out;
        CHECK(restart_launchd_job(f.ops(), 100, out) == LaunchdRestartResult::Restarted);
        CHECK(f.count("bootstrap") == 3);
    }

    TEST_CASE("bootstrap failing every time fails with launchctl's message") {
        FakeLaunchctl f;
        f.bootstrap_rcs = {5, 5, 5};
        std::ostringstream out;
        CHECK(restart_launchd_job(f.ops(), 100, out) == LaunchdRestartResult::Failed);
        CHECK(out.str().find("Input/output error") != std::string::npos);
    }

    TEST_CASE("exactly one codesigning spawn failure is tolerated (kickstart once)") {
        FakeLaunchctl f;
        f.prints = {"\truns = 1\n\tlast exit reason = OS_REASON_CODESIGNING\n",
                    "\truns = 1\n\tlast exit reason = OS_REASON_CODESIGNING\n",
                    "\truns = 2\n\tpid = 300\n"};
        std::ostringstream out;
        CHECK(restart_launchd_job(f.ops(), 100, out) ==
              LaunchdRestartResult::RestartedAfterCodesignRetry);
        CHECK(f.count("kickstart gui/501/dev.den.daemon") == 1);
        CHECK(out.str().find("after one codesigning spawn failure") != std::string::npos);
    }

    TEST_CASE("a second codesigning spawn failure is a failure") {
        FakeLaunchctl f;
        f.prints = {"\truns = 1\n\tlast exit reason = OS_REASON_CODESIGNING\n",
                    "\truns = 2\n\tlast exit reason = OS_REASON_CODESIGNING\n"};
        std::ostringstream out;
        CHECK(restart_launchd_job(f.ops(), 100, out) == LaunchdRestartResult::Failed);
        CHECK(f.count("kickstart") == 1);
        CHECK(out.str().find("twice") != std::string::npos);
    }

    TEST_CASE("no new pid within the timeout fails; the old pid does not count") {
        FakeLaunchctl f;
        f.prints = {"\tpid = 100\n"};
        std::ostringstream out;
        CHECK(restart_launchd_job(f.ops(15s), 100, out) == LaunchdRestartResult::Failed);
        CHECK(out.str().find("within 15s") != std::string::npos);
        // ~15s of 500ms polls, not an unbounded loop.
        CHECK(f.count("print") <= 32);
    }

    TEST_CASE(
        "restart from inside the job: a detached helper outlives the caller's process group") {
        SessionEnv session;
        TempDir tmp;
        auto stub = write_stub_launchctl(tmp.path);
        const auto log = tmp.path / "helper.log";
        const ScrubbedEnv senv{"/home/u", "u"};
        auto script = detached_restart_script(stub.string(), senv, "gui/501", "dev.den.daemon",
                                              tmp.path / "dev.den.daemon.plist");

        // A stand-in for the daemon: its own process group, starts the
        // helper, then is SIGKILLed together with its whole group, as a
        // bootout of the daemon's job would.
        int fds[2];
        REQUIRE(::pipe(fds) == 0);
        pid_t fake = ::fork();
        REQUIRE(fake >= 0);
        if (fake == 0) {
            ::close(fds[0]);
            ::setpgid(0, 0);
            auto pid = spawn_detached_restart(script, log, senv);
            pid_t v = pid ? *pid : -1;
            (void)::write(fds[1], &v, sizeof(v));
            ::pause();
            ::_exit(0);
        }
        ::close(fds[1]);
        pid_t helper = -1;
        REQUIRE(::read(fds[0], &helper, sizeof(helper)) == static_cast<ssize_t>(sizeof(helper)));
        ::close(fds[0]);
        REQUIRE(helper > 0);
        // A new session (made by the intermediate child), not the test's or
        // the fake daemon's, and outside the fake daemon's process group.
        CHECK(::getsid(helper) != ::getsid(0));
        CHECK(::getsid(helper) != fake);
        CHECK(::getpgid(helper) != fake);
        ::kill(-fake, SIGKILL);
        int st = 0;
        ::waitpid(fake, &st, 0);

        std::string text;
        for (int i = 0; i < 100; ++i) {
            text = slurp(log);
            if (text.find("restarted: pid 4242") != std::string::npos)
                break;
            std::this_thread::sleep_for(100ms);
        }
        CHECK(text.find("restarted: pid 4242") != std::string::npos);
        auto calls = slurp(tmp.path / "calls.log");
        auto bo = calls.find("launchctl bootout gui/501/dev.den.daemon");
        auto bs = calls.find("launchctl bootstrap gui/501 " +
                             (tmp.path / "dev.den.daemon.plist").string());
        CHECK(bo != std::string::npos);
        CHECK(bs != std::string::npos);
        CHECK(bo < bs);
        auto env = slurp(tmp.path / "env.log");
        CHECK(env.find("HOME=/home/u") != std::string::npos);
        CHECK(env.find("PATH=/usr/bin:/bin:/usr/sbin:/sbin") != std::string::npos);
        check_no_leak(env);
        check_no_leak(text);
    }

    TEST_CASE("running_inside_launchd_job follows XPC_SERVICE_NAME") {
        const char* old = std::getenv("XPC_SERVICE_NAME");
        std::string saved = old ? old : "";
        ::setenv("XPC_SERVICE_NAME", "dev.den.daemon", 1);
        CHECK(running_inside_launchd_job("dev.den.daemon"));
        ::setenv("XPC_SERVICE_NAME", "application.com.apple.Terminal.1", 1);
        CHECK_FALSE(running_inside_launchd_job("dev.den.daemon"));
        ::unsetenv("XPC_SERVICE_NAME");
        CHECK_FALSE(running_inside_launchd_job("dev.den.daemon"));
        if (old)
            ::setenv("XPC_SERVICE_NAME", saved.c_str(), 1);
    }
}

TEST_SUITE("relocate::codesign_guard") {
    TEST_CASE("is_den_own_binary matches den's executable, through symlinks, nothing else") {
        TempDir tmp;
        auto exe = tmp.path / "den";
        std::ofstream(exe) << "x";
        auto other = tmp.path / "other";
        std::ofstream(other) << "y";
        fs::create_symlink(exe, tmp.path / "den-link");
        CHECK(is_den_own_binary(exe, exe));
        CHECK(is_den_own_binary(tmp.path / "den-link", exe));
        CHECK(is_den_own_binary(tmp.path / "." / "den", exe));
        CHECK_FALSE(is_den_own_binary(other, exe));
        CHECK_FALSE(is_den_own_binary(exe, std::nullopt));
    }
}

} // namespace test_launchd_restart
} // namespace den
