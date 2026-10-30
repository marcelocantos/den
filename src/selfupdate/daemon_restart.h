// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/config.h"
#include "../daemon/launchd_plist.h"
#include "launchd_restart.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <ostream>
#include <string>

namespace den {

/// What `den self-update` did about a running daemon after replacing the
/// binary.
enum class DaemonRestartOutcome {
    NotRunning, ///< No daemon was running; nothing to do.
    Relaunched, ///< The launchd job was re-bootstrapped onto the new binary.
    Scheduled,  ///< We run inside the launchd job: a detached helper restarts it.
    Stopped,    ///< A daemon not managed by launchd was sent SIGTERM.
    Failed,     ///< A restart was needed but did not succeed.
};

/// Everything restart_daemon_after_update needs from the host, injectable
/// for tests.
struct DaemonRestartEnv {
    bool daemon_running = false;
    /// The installed launchd plist (~/Library/LaunchAgents/dev.den.daemon.plist),
    /// if present (macOS only).
    std::optional<std::filesystem::path> launchd_plist;
    /// launchd domain target, e.g. "gui/501".
    std::string launchd_domain;
    /// True when this process belongs to the daemon's own launchd job, so a
    /// bootout would kill it mid-restart.
    bool inside_launchd_job = false;
    /// In-process bootout + bootstrap + confirm (restart_launchd_job).
    std::function<LaunchdRestartResult(std::ostream&)> restart_launchd;
    /// Starts the detached restart helper; returns its pid.
    std::function<std::optional<pid_t>()> spawn_detached;
    /// Where the detached helper logs.
    std::filesystem::path helper_log;
    /// Sends SIGTERM to the daemon (stop_daemon). May throw.
    std::function<void()> stop;
    /// Brings the installed plist's PATH up to date (refresh_daemon_plist_path).
    /// Called whenever launchd_plist is set, before any restart, so the
    /// reloaded job (or the next login) runs with the current PATH.
    std::function<void(std::ostream&)> refresh_plist;
};

/// Bring the installed daemon plist `plist`'s PATH up to date with den's own
/// layout (launchd_path), for a plist that belongs to `cfg`'s den home. The
/// prefix is the plist's DEN_PREFIX when it sets one, else cfg.prefix. A plist
/// for another den home, or one installed without DEN_PREFIX while this
/// process runs with DEN_PREFIX set, is left untouched. Hand-edited plists are
/// reported, not rewritten (refresh_launchd_plist_path). The previous plist is
/// saved to <den_home>/dev.den.daemon.plist.bak.
PlistPathRefresh refresh_daemon_plist_path(const Config& cfg, const std::filesystem::path& plist,
                                           std::ostream& out);

/// Single-quote `s` for /bin/sh.
std::string sh_quote(const std::string& s);

/// After a successful self-update, refresh the installed launchd plist's PATH
/// (env.refresh_plist, whether or not the daemon is running), then make sure
/// no daemon keeps running the old
/// binary. A launchd-managed daemon is re-bootstrapped
/// (`launchctl bootout gui/<uid>/<label>`, then
/// `launchctl bootstrap gui/<uid> <plist>`, scrubbed environment; see
/// launchd_restart.h for why, and why it is a stopgap). When this process is
/// itself part of the daemon's launchd job, the bootout would kill it
/// mid-sequence, so the restart runs from a detached helper that outlives it.
/// A daemon outside launchd is stopped with SIGTERM and the user is told how
/// to start it again. Reports what happened on `out`.
DaemonRestartOutcome restart_daemon_after_update(const DaemonRestartEnv& env, std::ostream& out);

/// restart_daemon_after_update wired to the real host (is_daemon_running,
/// restart_launchd_job / spawn_detached_restart, stop_daemon).
DaemonRestartOutcome restart_daemon_after_update(const Config& cfg, std::ostream& out);

} // namespace den
