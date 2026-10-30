// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/config.h"
#include "../core/local_time.h"
#include "../core/version.h"
#include "../index/package.h"
#include "launchd_plist.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
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

// Signals to tests/consumers that DaemonState carries a `version_held` array.
#define DAEMON_STATE_HAS_VERSION_HELD 1

// One disabled package an upgrade would pour, with the message that refuses
// it (disabled_package_message / disabled_dependency_message, prefixed with
// "dependent <name> <from> (<to> available): " for a revision-bumped
// dependent).
struct DisabledBlocker {
    std::string disabled; // the disabled package (may be the upgrade's own name)
    std::string reason;
    bool operator==(const DisabledBlocker&) const = default;
};

// An upgrade the daemon did not apply because it would pour a package the
// index marks disabled: the package itself, a dependency it would pour, or
// a revision-bumped dependent that is disabled or would pour one. The same
// check `den install` / `den upgrade` refuse with (disabled_pours); the
// daemon has no --force. Every disabled package in the way is recorded, in
// disabled_pours order (the package's own, then each refused dependent's).
// It is re-checked every cycle and dropped once the upgrade applies or is no
// longer pending.
//
// daemon_state.json keeps every blocker in `blockers` (and `recorded`); it
// also writes the first blocker as `disabled` and the message's reasons
// (each disabled package once), joined with "; ", as `reason`, the
// single-blocker form older den reads. A file in that older
// form reads as one blocker, recorded at the file's last_check.
struct DisabledBlocked {
    std::string name;      // the package whose upgrade is blocked
    std::string installed; // installed version
    std::string available; // index version
    std::vector<DisabledBlocker> blockers;
    std::optional<uint64_t> recorded; // the cycle or pass that last found it (epoch seconds)
};

// Signals to tests/consumers that DaemonState carries a `disabled_blocked` array.
#define DAEMON_STATE_HAS_DISABLED_BLOCKED 1

struct DaemonState {
    std::optional<uint64_t> last_check;
    std::vector<PendingUpgrade> pending;
    // Packages whose index version is older than the installed one. They are
    // never upgraded (an upgrade never downgrades); reported so the user can
    // downgrade explicitly with `den use <name> <version>` if they want to.
    std::vector<PendingUpgrade> index_older;
    // Packages whose index version is lower than the installed one but whose
    // version_scheme is higher (openssl@3 3.6.3 -> 3.5.9, scheme 0 -> 1).
    // Held, never pending; reason from version_hold_reason. Only an explicit
    // `den use <name> <version>` moves them down.
    std::vector<DeferredUpgrade> version_held;
    std::vector<DeferredUpgrade> deferred;
    std::optional<uint64_t> last_upgrade;
    std::vector<std::string> errors;
    // Pending upgrades the last upgrade pass held back under the unattended
    // trust policy (reason in DeferredUpgrade::reason). They stay pending.
    std::vector<DeferredUpgrade> trust_held;
    // When the daemon last posted a held-back notification.
    std::optional<uint64_t> last_trust_notification;
    // Upgrades not applied because they would pour a package disabled
    // upstream. One whose dependency or dependent is disabled stays pending
    // (as trust_held ones do); one that is itself disabled never is.
    std::vector<DisabledBlocked> disabled_blocked;
};

/// The blockers of `b` with each disabled package once: the first blocker
/// naming it (the package's own pour comes before a dependent's).
std::vector<DisabledBlocker> unique_disabled_blockers(const DisabledBlocked& b);

/// The one-line message for a blocked upgrade, e.g. "jq 1.7.1 -> 1.8.2:
/// 'jq' needs 'libold', which is disabled upstream: ...", the reasons of
/// unique_disabled_blockers joined with "; ". daemon.log uses it as is.
std::string disabled_blocked_message(const DisabledBlocked& b);

/// disabled_blocked_message plus when it was recorded, e.g. "...  [recorded
/// 2026-10-31 01:46]", so a stale entry is obvious; shared by `den status`,
/// `den outdated`, `den doctor` and `den daemon status`.
std::string disabled_blocked_report(const DisabledBlocked& b);

/// The disabled packages blocking `b`, unique, comma-separated ("libold, jqx").
std::string disabled_blocker_names(const DisabledBlocked& b);

/// Apply one pending upgrade for the daemon's upgrade pass: install it,
/// record it in the active env and link it, as one unit, together with any
/// installed dependents the index revision-bumped for it. If the new version
/// cannot be linked it is rolled back (see apply_version_changes) and a
/// UserError is thrown, which run_upgrade_pass records as a failed apply.
/// When the batch would pour a package disabled upstream (disabled_pours on
/// the package, then on each revision-bumped dependent), nothing is poured
/// and UpgradeDisabled is thrown, which run_upgrade_pass records in
/// DaemonState::disabled_blocked. There is no bypass.
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

/// Why the upgrade selection leaves an installed package out regardless of
/// its version delta.
enum class UpgradeSkip {
    None,       // selectable: the delta decides
    NotInIndex, // no index entry, or one with no version
    Cask,       // a cask/app: upgrading swaps a live .app bundle, so only by name
    Disabled,   // the index marks the formula or cask disabled (checked first)
};

/// How one installed package compares with its index entry. This is the one
/// selection rule behind compute_pending, and so behind `den upgrade`, `den
/// outdated`, `den doctor` and the daemon's maintenance cycle.
struct UpgradeSelection {
    UpgradeSkip skip = UpgradeSkip::None;
    VersionDelta delta = VersionDelta::Same; // meaningless when NotInIndex
    int installed_scheme = 0;                // as read for the delta (0 when not read)
};

/// Classify installed `name` `version` against `pkg` (idx.find(name), may
/// be null). An index version equal to the installed one is Same without
/// asking `installed_scheme`; otherwise the delta is version_delta with the
/// installed keg's scheme from `installed_scheme` (0 when empty).
UpgradeSelection select_upgrade(const std::string& name, const std::string& version,
                                const Package* pkg, const InstalledSchemeFn& installed_scheme = {});

/// Outdated detection: for each den-managed package (`installed`: name →
/// version, from the active env's resolved homebrew bucket) whose index
/// (version_scheme, version, revision) tuple is newer, produce a
/// PendingUpgrade. An index version older than the installed one is never
/// pending (an upgrade never downgrades); it goes to `index_older` when
/// given, or to `version_held` (with its reason) when the index's
/// version_scheme is higher but its version is lower. Casks/apps, disabled formulae and names
/// missing from the index are skipped: cask upgrades swap live .app bundles and stay a manual `den
/// upgrade`. Sorted by name. `installed_scheme` gives each installed keg's
/// version_scheme (from its receipt); when empty every keg counts as 0. The
/// index side's scheme is Package::version_scheme. The selection is
/// select_upgrade's. A skipped cask whose index is newer goes to `casks`
/// when given, so a caller can say that it was left for an explicit upgrade.
/// A skipped disabled formula whose index is newer goes to `disabled` when
/// given (the daemon records it as blocked).
std::vector<PendingUpgrade> compute_pending(const std::map<std::string, std::string>& installed,
                                            const PackageIndex& idx,
                                            std::vector<PendingUpgrade>* index_older = nullptr,
                                            const InstalledSchemeFn& installed_scheme = {},
                                            std::vector<DeferredUpgrade>* version_held = nullptr,
                                            std::vector<PendingUpgrade>* casks = nullptr,
                                            std::vector<PendingUpgrade>* disabled = nullptr);

/// The one-line report for a package whose index is older than what is
/// installed, e.g. "openssl@3 3.6.3: index older than installed (index has
/// 3.5.9); not downgrading. To downgrade explicitly: den use openssl@3 3.5.9".
std::string index_older_message(const PendingUpgrade& p);

/// The one-line report for a held package, e.g. "openssl@3 3.6.3: held:
/// index pins 3.5.9 (version_scheme 1), below installed 3.6.3; not
/// downgrading. To downgrade explicitly: den use openssl@3 3.5.9".
std::string version_held_message(const DeferredUpgrade& h);

/// A package in the active environment that another provider (pip, cargo,
/// go, npm) manages. Its versions come from that provider's own source, not
/// the Homebrew index, so nothing here compares it: `den outdated` lists it
/// as not tracked and `den upgrade <name>` warns and skips it.
struct UntrackedPackage {
    std::string name;
    std::string version;
    std::string provider;
};

/// What `den outdated` reports and what a bare `den upgrade` applies: the
/// active environment's manifest, resolved per provider
/// (resolve_per_provider), with its homebrew bucket run through
/// compute_pending, the same comparison and hold logic the daemon's
/// maintenance cycle and `den doctor`'s held-package check use. Kegs in the
/// store that no manifest records are not part of it (they are for `den
/// doctor` / `den cleanup`), nor are other kegs of a recorded package.
struct OutdatedPlan {
    std::size_t homebrew = 0;                // packages in the homebrew bucket
    std::vector<PendingUpgrade> pending;     // index newer: an upgrade is available
    std::vector<PendingUpgrade> index_older; // index older: never downgraded
    std::vector<DeferredUpgrade> held;       // VersionDelta::Held, with its reason
    std::vector<PendingUpgrade> casks;       // index newer, but a cask: by name only
    std::vector<UntrackedPackage> untracked; // other providers, sorted by name
};

/// Build the OutdatedPlan for `config`'s active environment against `idx`.
/// Installed version_schemes come from each keg's receipt, as in the daemon.
/// `provider` names the bucket compared against `idx` (`homebrew`, counted
/// in OutdatedPlan::homebrew); every other bucket is untracked.
/// upgrade_packages passes its provider's name.
OutdatedPlan outdated_plan(const Config& config, const PackageIndex& idx,
                           std::string_view provider = "homebrew");

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
    // optional: pending name → reason for upgrades to hold back under the
    // unattended trust policy (see trust_screen.h). Runs before the pass.
    std::function<std::map<std::string, std::string>(const std::vector<PendingUpgrade>&)>
        trust_screen;
    // optional: told once per pass about upgrades newly held back (not held
    // in the previous pass). Returns true when a notification went out.
    std::function<bool(const std::vector<std::string>&)> notify_held;
};

struct MaintenanceReport {
    bool index_refreshed = false;
    std::size_t pending = 0;
    std::size_t index_older = 0;  // index version older than installed: skipped
    std::size_t version_held = 0; // higher scheme, lower version: held
    std::size_t downloaded = 0;
    std::size_t skipped_pinned = 0; // pending but pinned: not pre-downloaded
    std::size_t applied = 0;
    std::size_t deferred = 0;
    std::size_t trust_held = 0;       // held back by the unattended trust policy
    std::size_t disabled_blocked = 0; // would pour a package disabled upstream
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

/// Append a timestamped log line to den_home/daemon.log.
void log_daemon(const fs::path& den_home, const std::string& msg);

} // namespace den
