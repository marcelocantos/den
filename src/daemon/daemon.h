// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/config.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace den {

namespace fs = std::filesystem;

// ---- Data types ------------------------------------------------------------

struct PendingUpgrade {
    std::string name;
    std::string installed;
    std::string available;
};

struct DaemonState {
    std::optional<uint64_t> last_check;
    std::vector<PendingUpgrade> pending;
    std::optional<uint64_t> last_upgrade;
    std::vector<std::string> errors;
};

// ---- Lifecycle -------------------------------------------------------------

/// Main daemon loop. Writes PID + flock, registers SIGTERM, then repeatedly:
///   sleep(interval), refresh index, check outdated, pre-download if
///   auto_download is set, upgrade if auto_upgrade is set and in window.
/// Blocks until SIGTERM is received.
void run_daemon(const Config& config);

/// Send SIGTERM to the running daemon (reads PID from den_home/daemon.pid).
void stop_daemon(const Config& config);

/// Return true if the daemon is running (tries exclusive flock on PID file).
bool is_daemon_running(const Config& config);

// ---- State I/O -------------------------------------------------------------

DaemonState read_daemon_state(const fs::path& den_home);
void        write_daemon_state(const fs::path& den_home, const DaemonState& state);

// ---- Utilities -------------------------------------------------------------

/// Current Unix timestamp in seconds.
uint64_t now_secs();

/// Parse "HH:MM-HH:MM" and return true if the current local time falls in
/// that range. Returns true for empty/invalid strings (no restriction).
bool in_maintenance_window(const std::string& window);

/// Generate a launchd plist XML string for the daemon service.
/// Label: dev.den.daemon. RunAtLoad and KeepAlive are both true.
/// Interpolated strings are XML-escaped.
std::string launchd_plist(const fs::path& den_binary, const fs::path& den_home);

/// Append a timestamped log line to den_home/daemon.log.
void log_daemon(const fs::path& den_home, const std::string& msg);

} // namespace den
