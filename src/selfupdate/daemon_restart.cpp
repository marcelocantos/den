// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "daemon_restart.h"

#include "../daemon/daemon.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <exception>
#include <fstream>
#include <sstream>
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

PlistPathRefresh refresh_daemon_plist_path(const Config& cfg, const fs::path& plist,
                                           std::ostream& out) {
    std::string text;
    {
        std::ifstream f(plist, std::ios::binary);
        if (!f) {
            return PlistPathRefresh::LeftAlone;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        text = ss.str();
    }
    const auto home = plist_env_value(text, "DEN_HOME");
    if (!home || fs::path(*home).lexically_normal() != cfg.den_home.lexically_normal()) {
        return PlistPathRefresh::LeftAlone; // another den home's daemon
    }
    fs::path prefix = cfg.prefix;
    if (auto dp = plist_env_value(text, "DEN_PREFIX"); dp && !dp->empty()) {
        prefix = *dp;
    } else if (cfg.prefix_source == PrefixSource::EnvDenPrefix) {
        // The daemon was installed without DEN_PREFIX; this process's
        // DEN_PREFIX says nothing about which prefix it maintains.
        return PlistPathRefresh::LeftAlone;
    }
    return refresh_launchd_plist_path(plist, launchd_path(cfg.den_home, prefix),
                                      cfg.den_home / "dev.den.daemon.plist.bak", out);
}

DaemonRestartOutcome restart_daemon_after_update(const DaemonRestartEnv& env, std::ostream& out) {
    if (env.launchd_plist && env.refresh_plist) {
        try {
            env.refresh_plist(out);
        } catch (const std::exception& e) {
            out << "Could not refresh the den daemon plist's PATH (" << e.what()
                << "); run `den daemon install` to regenerate it.\n";
        }
    }
    if (!env.daemon_running) {
        return DaemonRestartOutcome::NotRunning;
    }
    if (env.launchd_plist) {
        if (env.inside_launchd_job) {
            auto pid = env.spawn_detached ? env.spawn_detached() : std::nullopt;
            if (pid) {
                out << "Restarting the den daemon onto the new binary from a detached helper (pid "
                    << *pid << "; log: " << env.helper_log.string() << ").\n";
                return DaemonRestartOutcome::Scheduled;
            }
            out << "Could not start the detached restart helper; run `den daemon install` to "
                   "reload the daemon on the new binary.\n";
            return DaemonRestartOutcome::Failed;
        }
        std::ostringstream detail;
        auto r = env.restart_launchd ? env.restart_launchd(detail) : LaunchdRestartResult::Failed;
        out << detail.str();
        if (r != LaunchdRestartResult::Failed) {
            out << "Restarted the den daemon so it runs the new binary.\n"
                << "  (If `launchctl print` shows `last exit reason = OS_REASON_CODESIGNING` for "
                   "dev.den.daemon, that is the old process's exit around the binary swap; it "
                   "is transient and harmless. See docs/self-update.md.)\n";
            return DaemonRestartOutcome::Relaunched;
        }
        out << "Could not restart the den daemon via launchd; run `den daemon install` to "
               "reload it on the new binary.\n";
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
    static const std::string kLabel = "dev.den.daemon";
    DaemonRestartEnv env;
    env.daemon_running = is_daemon_running(cfg);
#ifdef __APPLE__
    const auto senv = scrubbed_env_for_current_user();
    if (!senv.home.empty()) {
        auto plist = fs::path(senv.home) / "Library" / "LaunchAgents" / (kLabel + ".plist");
        std::error_code ec;
        if (fs::is_regular_file(plist, ec)) {
            env.launchd_plist = plist;
        }
    }
    env.launchd_domain = "gui/" + std::to_string(static_cast<unsigned>(::getuid()));
    env.inside_launchd_job = running_inside_launchd_job(kLabel);
    env.helper_log = cfg.den_home / "daemon-restart.log";
    if (env.launchd_plist) {
        const auto plist = *env.launchd_plist;
        env.refresh_plist = [&cfg, plist](std::ostream& o) {
            refresh_daemon_plist_path(cfg, plist, o);
        };
        env.restart_launchd = [plist](std::ostream& o) {
            auto ops = real_launchd_ops(kLabel, plist);
            auto before = ops.launchctl({"print", ops.domain + "/" + ops.label});
            auto st = parse_launchctl_print(before.spawned && before.exit_code == 0, before.output);
            return restart_launchd_job(ops, st.pid, o);
        };
        env.spawn_detached = [plist, senv, domain = env.launchd_domain, log = env.helper_log] {
            return spawn_detached_restart(
                detached_restart_script(resolve_launchctl(), senv, domain, kLabel, plist), log,
                senv);
        };
    }
#endif
    env.stop = [&cfg] { stop_daemon(cfg); };
    return restart_daemon_after_update(env, out);
}

} // namespace den
