// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/config.h"

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
    /// Runs a shell command, returning its exit status.
    std::function<int(const std::string&)> run;
    /// Sends SIGTERM to the daemon (stop_daemon). May throw.
    std::function<void()> stop;
};

/// Single-quote `s` for /bin/sh.
std::string sh_quote(const std::string& s);

/// After a successful self-update, make sure no daemon keeps running the old
/// binary. A launchd-managed daemon is re-bootstrapped (bootout, then
/// bootstrap of the same plist): launchd re-reads the program and its code
/// signature, so the job comes back on the new binary straight away instead
/// of being respawned by KeepAlive against the signature it recorded for the
/// old one. A daemon outside launchd is stopped with SIGTERM and the user is
/// told how to start it again. Reports what happened on `out`.
DaemonRestartOutcome restart_daemon_after_update(const DaemonRestartEnv& env, std::ostream& out);

/// restart_daemon_after_update wired to the real host (is_daemon_running,
/// launchctl via std::system, stop_daemon).
DaemonRestartOutcome restart_daemon_after_update(const Config& cfg, std::ostream& out);

} // namespace den
