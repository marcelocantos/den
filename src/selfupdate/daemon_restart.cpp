// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "daemon_restart.h"

#include "../daemon/daemon.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <exception>
#include <system_error>

#include <unistd.h>

namespace den {

namespace fs = std::filesystem;

std::string sh_quote(const std::string& s) {
    std::string q = "'";
    for (char c : s) {
        if (c == '\'') {
            q += "'\\''";
        } else {
            q += c;
        }
    }
    q += "'";
    return q;
}

DaemonRestartOutcome restart_daemon_after_update(const DaemonRestartEnv& env, std::ostream& out) {
    if (!env.daemon_running) {
        return DaemonRestartOutcome::NotRunning;
    }
    if (env.launchd_plist) {
        const auto plist = sh_quote(env.launchd_plist->string());
        const auto domain = sh_quote(env.launchd_domain);
        // bootout stops the old process (SIGTERM; the daemon shuts down
        // cleanly) and unregisters the job; bootstrap registers it again from
        // the plist, re-reading the new binary.
        (void)env.run("launchctl bootout " + domain + " " + plist + " 2>/dev/null");
        int rc = env.run("launchctl bootstrap " + domain + " " + plist);
        if (rc == 0) {
            out << "Restarted the den daemon so it runs the new binary.\n"
                << "  (If `launchctl print` shows `last exit reason = OS_REASON_CODESIGNING` for "
                   "dev.den.daemon, that is the old process's exit around the binary swap; it "
                   "is transient and harmless. See docs/self-update.md.)\n";
            return DaemonRestartOutcome::Relaunched;
        }
        out << "Could not restart the den daemon via launchd (launchctl bootstrap exited " << rc
            << "); run `den daemon install` to reload it on the new binary.\n";
        return DaemonRestartOutcome::Failed;
    }
    try {
        env.stop();
    } catch (const std::exception& e) {
        out << "Could not stop the running den daemon (" << e.what()
            << "); it is still running the old binary. Stop it with `den daemon stop` and "
               "start it again.\n";
        return DaemonRestartOutcome::Failed;
    }
    out << "Stopped the running den daemon (it was running the old binary). Start it again "
           "with `den daemon run`, or `den daemon install` to have launchd manage it.\n";
    return DaemonRestartOutcome::Stopped;
}

DaemonRestartOutcome restart_daemon_after_update(const Config& cfg, std::ostream& out) {
    DaemonRestartEnv env;
    env.daemon_running = is_daemon_running(cfg);
#ifdef __APPLE__
    const char* home = std::getenv("HOME");
    if (home && *home) {
        auto plist = fs::path(home) / "Library" / "LaunchAgents" / "dev.den.daemon.plist";
        std::error_code ec;
        if (fs::is_regular_file(plist, ec)) {
            env.launchd_plist = plist;
        }
    }
    env.launchd_domain = "gui/" + std::to_string(static_cast<unsigned>(::getuid()));
#endif
    env.run = [](const std::string& cmd) {
        SPDLOG_DEBUG("self-update: running {}", cmd);
        return std::system(cmd.c_str());
    };
    env.stop = [&cfg] { stop_daemon(cfg); };
    return restart_daemon_after_update(env, out);
}

} // namespace den
