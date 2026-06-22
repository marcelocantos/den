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

/// A single installed keg found in the Homebrew Cellar.
struct HomebrewKeg {
    std::string name;    // Formula name, e.g. "git"
    std::string version; // Installed version, e.g. "2.44.0"
    fs::path path;       // Full path: /opt/homebrew/Cellar/git/2.44.0
};

/// A single installed cask found in the Homebrew Caskroom.
struct HomebrewCask {
    std::string name;    // Cask token, e.g. "visual-studio-code"
    std::string version; // Installed version, e.g. "1.88.0"
    fs::path path;       // Full path: <Caskroom>/visual-studio-code/1.88.0
};

/// A single Homebrew tap found under Library/Taps.
struct HomebrewTap {
    std::string name; // Fully-qualified tap, e.g. "homebrew/cask"
    fs::path path;    // Full path to the tap checkout directory
};

/// A `brew services`-managed service.
struct HomebrewService {
    std::string name;    // Service name, e.g. "postgresql@16"
    std::string status;  // "started", "stopped", "none", ...
    std::string user;    // Owning user (may be empty)
    fs::path plist_path; // Path to the launchd plist (may be empty)
    bool running = false;
};

/// The parts of INSTALL_RECEIPT.json that den needs.
struct Tab {
    bool installed_on_request = true;
    std::vector<std::string> runtime_deps; // full_names of runtime dependencies
};

/// Aggregate summary of a migration (used by both real and dry-run paths).
struct MigrationSummary {
    int formulae = 0; // distinct formulae that would be / were recorded
    int casks = 0;    // distinct casks
    int taps = 0;     // taps
    int services = 0; // services
};

// ---------------------------------------------------------------------------
// Scanners — all strictly read-only against Homebrew state.
// ---------------------------------------------------------------------------

/// Scan a Homebrew Cellar directory and return all kegs found.
/// For packages with multiple installed versions, all versions are returned.
/// Results are sorted by name then version.
std::vector<HomebrewKeg> scan_homebrew_cellar(const fs::path& cellar);

/// Read the INSTALL_RECEIPT.json from a keg directory.
/// Returns nullopt if the file is absent or malformed.
std::optional<Tab> read_tab(const fs::path& keg_path);

/// Scan a Homebrew Caskroom directory and return all installed casks.
/// Each cask token directory contains version subdirectories (siblings of a
/// `.metadata` directory). All non-metadata version dirs are returned, sorted
/// by name then version (newest first within a name).
std::vector<HomebrewCask> scan_homebrew_caskroom(const fs::path& caskroom);

/// Scan a Homebrew `Library/Taps` directory and return all taps.
/// The on-disk layout is `<user>/homebrew-<repo>`; the returned name is the
/// canonical `<user>/<repo>` form. Results are sorted by name.
std::vector<HomebrewTap> scan_homebrew_taps(const fs::path& taps_root);

/// Parse the JSON produced by `brew services list --json` into a service list.
/// Returns an empty vector on malformed input. Strictly a pure parser — it
/// performs no I/O and never touches Homebrew or launchd state.
std::vector<HomebrewService> parse_brew_services_json(const std::string& json_text);

/// Enumerate `brew services`-managed services by parsing the launchd plists
/// in a directory (e.g. ~/Library/LaunchAgents). Used as a fallback when the
/// `brew services` CLI is unavailable. Only `homebrew.mxcl.<name>.plist`
/// files are considered. Read-only.
std::vector<HomebrewService> scan_launchagent_plists(const fs::path& launch_agents_dir);

// ---------------------------------------------------------------------------
// Migration
// ---------------------------------------------------------------------------

/// Options controlling a migration run.
struct MigrateOptions {
    bool dry_run = false;         // compute + report only, write nothing
    bool integrate_shell = false; // append `eval "$(den init)"` to the profile
    /// Optional pre-captured `brew services list --json` output. When set, it
    /// is parsed instead of shelling out to `brew` (used by tests).
    std::optional<std::string> brew_services_json;
    /// When true (the real CLI path), enumerate services by shelling out to
    /// `brew services list --json`, falling back to the LaunchAgents plists.
    /// Defaults to false so unit tests stay hermetic — they inject services
    /// via `brew_services_json` instead of touching the host environment.
    bool query_services = false;
};

/// Migrate packages from the Homebrew Cellar into den's root manifest.
///
/// If names is empty, scans the entire Cellar; otherwise migrates only the
/// named formulae. For each formula found, the latest version is recorded in
/// den's root manifest. Files are not copied — this is metadata migration only.
///
/// In addition to formulae, this records casks, taps, and `brew services`
/// state into the same ROOT.json manifest. The migration is non-destructive
/// (Homebrew's files are never modified) and idempotent (re-running picks up
/// new installs without disturbing existing den state).
///
/// Prints a summary: N formulae, N casks, N taps, N services.
MigrationSummary migrate_from_homebrew(const Config& config, const std::vector<std::string>& names,
                                       const MigrateOptions& opts = {});

// ---------------------------------------------------------------------------
// Post-migration health check
// ---------------------------------------------------------------------------

struct HealthIssue {
    std::string message;
    bool error = false; // true = failure, false = warning
};

struct HealthReport {
    int formulae_ok = 0;
    int casks_ok = 0;
    int taps_ok = 0;
    int services_ok = 0;
    std::vector<HealthIssue> issues;

    bool healthy() const {
        for (const auto& i : issues) {
            if (i.error) {
                return false;
            }
        }
        return true;
    }
};

/// Verify a completed migration: every manifest formula resolves to a keg in
/// the Cellar, every cask resolves to a Caskroom entry, and recorded services
/// are accounted for. Read-only; touches only den_home and (read-only) the
/// Homebrew Cellar/Caskroom. Prints a summary line when `print` is true.
HealthReport check_migration_health(const Config& config, bool print = true);

} // namespace den
