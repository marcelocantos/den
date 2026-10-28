// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/config.h"
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
};

/// Single-quote `s` for /bin/sh.
std::string sh_quote(const std::string& s);

/// After a successful self-update, make sure no daemon keeps running the old
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
