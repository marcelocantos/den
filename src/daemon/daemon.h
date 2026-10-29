// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/config.h"
#include "../index/package.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace den {

namespace fs = std::filesystem;

class PackageProvider;

// ---- Data types ------------------------------------------------------------

struct PendingUpgrade {
    std::string name;
    std::string installed;
    std::string available;
    // Homebrew version_scheme of each side (0 when absent); ordering
    // compares (version_scheme, version, revision). See core/version.h.
    int installed_scheme = 0;
    int available_scheme = 0;
};

/// Looks up the version_scheme of an installed keg by (name, version).
using InstalledSchemeFn = std::function<int(const std::string&, const std::string&)>;

// An upgrade the daemon staged but did not apply because one or more of the
// installed keg's files were held open by a live process (🎯T51 / 🎯T72).
// It is re-attempted on the next upgrade pass once the process exits.
struct DeferredUpgrade {
    std::string name;
    std::string installed;
    std::string available;
    std::string reason; // human-readable, e.g. "binary in use"
};

// Signals to tests/consumers that DaemonState carries a `deferred` array.
#define DAEMON_STATE_HAS_DEFERRED 1

// Signals to tests/consumers that DaemonState carries an `index_older` array.
#define DAEMON_STATE_HAS_INDEX_OLDER 1

struct DaemonState {
    std::optional<uint64_t> last_check;
    std::vector<PendingUpgrade> pending;
    // Packages whose index version is older than the installed one. They are
    // never upgraded (an upgrade never downgrades); reported so the user can
    // downgrade explicitly with `den use <name> <version>` if they want to.
    std::vector<PendingUpgrade> index_older;
    std::vector<DeferredUpgrade> deferred;
    std::optional<uint64_t> last_upgrade;
    std::vector<std::string> errors;
};

/// Apply one pending upgrade for the daemon's upgrade pass: install it,
/// record it in the active env and link it, as one unit, together with any
/// installed dependents the index revision-bumped for it. If the new version
/// cannot be linked it is rolled back (see apply_version_changes) and a
/// UserError is thrown, which run_upgrade_pass records as a failed apply.
void apply_daemon_upgrade(const Config& config, PackageProvider& provider, const PackageIndex& idx,
                          const PendingUpgrade& pu);

// ---- Lifecycle -------------------------------------------------------------

/// Main daemon loop. Writes PID + flock, registers SIGTERM, then repeatedly:
///   sleep(interval), refresh index, check outdated, pre-download if
///   auto_download is set, upgrade if auto_upgrade is set and in window.
/// Blocks until SIGTERM is received.
/// With `once`, runs a single maintenance cycle immediately (no initial
/// sleep) and returns; useful for testing and for cron-style invocation.
void run_daemon(const Config& config, bool once = false);

/// Send SIGTERM to the running daemon (reads PID from den_home/daemon.pid).
void stop_daemon(const Config& config);

/// Return true if the daemon is running (tries exclusive flock on PID file).
bool is_daemon_running(const Config& config);

// ---- Maintenance cycle (🎯T13) ---------------------------------------------

/// Outdated detection: for each den-managed package (`installed`: name →
/// version, from the active env's resolved homebrew bucket) whose index
/// (version_scheme, version, revision) tuple is newer, produce a
/// PendingUpgrade. An index version older than the installed one is never
/// pending (an upgrade never downgrades); it goes to `index_older` when
/// given. Casks/apps, disabled formulae and names missing from the index are
/// skipped: cask upgrades swap live .app bundles and stay a manual `den
/// upgrade`. Sorted by name. `installed_scheme` gives each installed keg's
/// version_scheme (from its receipt); when empty every keg counts as 0. The
/// index side's scheme is Package::version_scheme.
std::vector<PendingUpgrade> compute_pending(const std::map<std::string, std::string>& installed,
                                            const PackageIndex& idx,
                                            std::vector<PendingUpgrade>* index_older = nullptr,
                                            const InstalledSchemeFn& installed_scheme = {});

/// The one-line report for a package whose index is older than what is
/// installed, e.g. "openssl@3 3.6.3: index older than installed (index has
/// 3.5.9); not downgrading. To downgrade explicitly: den use openssl@3 3.5.9".
std::string index_older_message(const PendingUpgrade& p);

struct MaintenanceSettings {
    bool auto_download = true;
    bool auto_upgrade = false;
    std::string upgrade_window; // "HH:MM-HH:MM" or empty (no restriction)
};

/// Side-effecting steps of a cycle, injected so the cycle logic is testable
/// without network or Cellar writes. Any hook may throw; failures are
/// recorded in DaemonState::errors and never abort the daemon.
struct MaintenanceHooks {
    std::function<PackageIndex()> refresh_index;                   // required
    std::function<std::map<std::string, std::string>()> installed; // required
    std::function<void(const PendingUpgrade&)> prefetch;           // optional
    std::function<void(const PendingUpgrade&)> apply;              // optional
    std::function<bool(const std::string&)> in_window;             // default: in_maintenance_window
    InstalledSchemeFn installed_scheme; // optional: installed keg's version_scheme (default 0)
};

struct MaintenanceReport {
    bool index_refreshed = false;
    std::size_t pending = 0;
    std::size_t index_older = 0; // index version older than installed: skipped
    std::size_t downloaded = 0;
    std::size_t skipped_pinned = 0; // pending but pinned: not pre-downloaded
    std::size_t applied = 0;
    std::size_t deferred = 0;
};

/// One maintenance cycle: refresh the index, recompute `state.pending`
/// (dropping deferrals for packages that are no longer pending), prefetch
/// archives when auto_download is on (never for packages pinned in
/// den_home/pins.json; an unreadable pins.json skips every prefetch), and run the in-use-aware
/// upgrade pass when auto_upgrade is on and the window allows. Mutates `state` (including
/// last_check and errors); the caller persists it.
MaintenanceReport run_maintenance_cycle(const fs::path& cellar, const fs::path& den_home,
                                        const MaintenanceSettings& settings, DaemonState& state,
                                        const MaintenanceHooks& hooks);

// ---- State I/O -------------------------------------------------------------

DaemonState read_daemon_state(const fs::path& den_home);
void write_daemon_state(const fs::path& den_home, const DaemonState& state);

// ---- Utilities -------------------------------------------------------------

/// Current Unix timestamp in seconds.
uint64_t now_secs();

/// Parse "HH:MM-HH:MM" and return true if the current local time falls in
/// that range. Returns true for empty/invalid strings (no restriction).
bool in_maintenance_window(const std::string& window);

/// Generate a launchd plist XML string for the daemon service.
/// Label: dev.den.daemon. RunAtLoad and KeepAlive are both true.
/// Interpolated strings are XML-escaped. When `prefix` is den's own prefix
/// (not Homebrew's), `<prefix>/bin` leads the agent's PATH.
std::string launchd_plist(const fs::path& den_binary, const fs::path& den_home,
                          const fs::path& prefix = {});

/// Append a timestamped log line to den_home/daemon.log.
void log_daemon(const fs::path& den_home, const std::string& msg);

} // namespace den
