// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// After `den self-update` swaps the binary, a running daemon must not keep
// running the old one: a launchd-managed daemon is re-bootstrapped onto the
// new binary, any other daemon is stopped with a pointer to restart it.
// Also: the doctor's broken-link pointer and shell-profile `den init`
// detection used to keep shell integration idempotent.

#include "../src/doctor/doctor.h"
#include "../src/migrate/migrate.h"
#include "../src/selfupdate/daemon_restart.h"

#include <doctest.h>

#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace den {
namespace test_selfupdate_restart {

struct Recorder {
    std::vector<std::string> calls;
    LaunchdRestartResult launchd_result = LaunchdRestartResult::Restarted;
    std::optional<pid_t> helper_pid = 4242;
    bool inside_job = false;
    int stops = 0;
    bool stop_throws = false;

    DaemonRestartEnv env(bool running, std::optional<std::filesystem::path> plist) {
        DaemonRestartEnv e;
        e.daemon_running = running;
        e.launchd_plist = std::move(plist);
        e.launchd_domain = "gui/501";
        e.inside_launchd_job = inside_job;
        e.helper_log = "/h/.den/daemon-restart.log";
        e.restart_launchd = [this](std::ostream&) {
            calls.push_back("restart");
            return launchd_result;
        };
        e.spawn_detached = [this] {
            calls.push_back("detached");
            return helper_pid;
        };
        e.stop = [this] {
            ++stops;
            if (stop_throws)
                throw std::runtime_error("no such process");
        };
        return e;
    }
};

TEST_SUITE("selfupdate_restart") {
    TEST_CASE("sh_quote single-quotes and escapes embedded quotes") {
        CHECK(sh_quote("/a b/c") == "'/a b/c'");
        CHECK(sh_quote("it's") == "'it'\\''s'");
    }

    TEST_CASE("no running daemon: nothing is touched") {
        Recorder r;
        std::ostringstream out;
        auto o = restart_daemon_after_update(
            r.env(false,
                  std::filesystem::path("/Users/x/Library/LaunchAgents/dev.den.daemon.plist")),
            out);
        CHECK(o == DaemonRestartOutcome::NotRunning);
        CHECK(r.calls.empty());
        CHECK(r.stops == 0);
        CHECK(out.str().empty());
    }

    TEST_CASE("launchd daemon is restarted in-process when we are not part of its job") {
        Recorder r;
        std::ostringstream out;
        auto o = restart_daemon_after_update(
            r.env(true,
                  std::filesystem::path("/Users/x/Library/LaunchAgents/dev.den.daemon.plist")),
            out);
        CHECK(o == DaemonRestartOutcome::Relaunched);
        CHECK(r.calls == std::vector<std::string>{"restart"});
        CHECK(r.stops == 0);
        CHECK(out.str().find("Restarted the den daemon") != std::string::npos);
        CHECK(out.str().find("OS_REASON_CODESIGNING") != std::string::npos);
        CHECK(out.str().find("docs/self-update.md") != std::string::npos);
    }

    TEST_CASE("a restart that needed the one tolerated codesign retry still counts") {
        Recorder r;
        r.launchd_result = LaunchdRestartResult::RestartedAfterCodesignRetry;
        std::ostringstream out;
        auto o = restart_daemon_after_update(
            r.env(true, std::filesystem::path("/p/dev.den.daemon.plist")), out);
        CHECK(o == DaemonRestartOutcome::Relaunched);
    }

    TEST_CASE("failed launchd restart is reported with the recovery command") {
        Recorder r;
        r.launchd_result = LaunchdRestartResult::Failed;
        std::ostringstream out;
        auto o = restart_daemon_after_update(
            r.env(true, std::filesystem::path("/p/dev.den.daemon.plist")), out);
        CHECK(o == DaemonRestartOutcome::Failed);
        CHECK(out.str().find("den daemon install") != std::string::npos);
    }

    TEST_CASE("inside the daemon's own job the restart runs from a detached helper") {
        Recorder r;
        r.inside_job = true;
        std::ostringstream out;
        auto o = restart_daemon_after_update(
            r.env(true, std::filesystem::path("/p/dev.den.daemon.plist")), out);
        CHECK(o == DaemonRestartOutcome::Scheduled);
        // Never an in-process bootout: it would kill us mid-sequence.
        CHECK(r.calls == std::vector<std::string>{"detached"});
        CHECK(out.str().find("detached helper (pid 4242") != std::string::npos);
        CHECK(out.str().find("daemon-restart.log") != std::string::npos);
    }

    TEST_CASE("a detached helper that cannot start is reported") {
        Recorder r;
        r.inside_job = true;
        r.helper_pid = std::nullopt;
        std::ostringstream out;
        auto o = restart_daemon_after_update(
            r.env(true, std::filesystem::path("/p/dev.den.daemon.plist")), out);
        CHECK(o == DaemonRestartOutcome::Failed);
        CHECK(r.calls == std::vector<std::string>{"detached"});
        CHECK(out.str().find("den daemon install") != std::string::npos);
    }

    TEST_CASE("daemon outside launchd is stopped, with how to start it again") {
        Recorder r;
        std::ostringstream out;
        auto o = restart_daemon_after_update(r.env(true, std::nullopt), out);
        CHECK(o == DaemonRestartOutcome::Stopped);
        CHECK(r.stops == 1);
        CHECK(r.calls.empty());
        CHECK(out.str().find("den daemon run") != std::string::npos);
    }

    TEST_CASE("a stop that fails is reported, not thrown") {
        Recorder r;
        r.stop_throws = true;
        std::ostringstream out;
        auto o = restart_daemon_after_update(r.env(true, std::nullopt), out);
        CHECK(o == DaemonRestartOutcome::Failed);
        CHECK(out.str().find("still running the old binary") != std::string::npos);
    }

    TEST_CASE("doctor points broken links at den migrate-prefix without repairing") {
        auto many = broken_symlinks_hint(311, true);
        CHECK(many.find("311 broken symlinks") != std::string::npos);
        CHECK(many.find("`den migrate-prefix`") != std::string::npos);
        CHECK(many.find("(coming)") == std::string::npos);
        CHECK(many.find("does not repair") != std::string::npos);
        CHECK(broken_symlinks_hint(1, true).find("1 broken symlink in") != std::string::npos);
        auto own = broken_symlinks_hint(3, false);
        CHECK(own.find("migrate-prefix") == std::string::npos);
        CHECK(own.find("does not repair") != std::string::npos);
        CHECK(shared_prefix_hint() ==
              "den is still installing into Homebrew's folder, so Homebrew upgrades can break "
              "your den environments. Run `den migrate-prefix` to see what moving would do. It "
              "changes nothing until you add --apply.");
    }

    TEST_CASE("profile_has_den_init recognises every den init form, not comments") {
        CHECK(profile_has_den_init("eval \"$(den init)\"\n"));
        CHECK(profile_has_den_init("x\neval \"$(\"/Users/m/.den/bin/den\" init)\"\n"));
        CHECK(profile_has_den_init("eval \"$(\"$HOME/.den/bin/den\" init --shell zsh)\"\n"));
        CHECK(profile_has_den_init("/Users/m/.den/bin/den init --shell fish | source\n"));
        CHECK_FALSE(profile_has_den_init("# den init goes here\n"));
        CHECK_FALSE(profile_has_den_init("# den — universal development environment manager\n"));
        CHECK_FALSE(profile_has_den_init("export PATH=$HOME/.den/bin:$PATH\n"));
        CHECK_FALSE(profile_has_den_init(""));
    }
}

} // namespace test_selfupdate_restart
} // namespace den
