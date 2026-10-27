// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/config.h"

#include <filesystem>
#include <optional>
#include <string>

namespace den {

struct UpdateInfo {
    std::string latest_version; // e.g. "0.5.0" (no v prefix)
    std::string download_url;   // tarball URL for this platform
    std::string checksum_url;   // .sha256 URL
};

/// The command that upgrades a Homebrew-installed den.
inline constexpr const char* kBrewUpgradeCommand = "brew upgrade marcelocantos/tap/den";

/// If `exe` (the path den was run from) belongs to a Homebrew keg, returns the
/// keg path that shows it; otherwise nullopt. A keg is detected when `exe` or
/// its realpath lies under a `.../Cellar/den/<version>/` directory, or under a
/// formula opt path `<P>/opt/den/` where `<P>/Cellar/den` exists (so a plain
/// /opt/den install is not mistaken for one). Paths are injected so tests can
/// exercise this without a real Homebrew install.
std::optional<std::filesystem::path> brew_keg_for_exe(const std::filesystem::path& exe);

/// Throws UserError telling the user to run kBrewUpgradeCommand when `exe` is
/// in a Homebrew keg (see brew_keg_for_exe); returns normally otherwise.
/// `den self-update` calls this before any network access or file change, so
/// brew stays the only thing that replaces a brew-installed den.
void refuse_self_update_if_brew_managed(const std::filesystem::path& exe);

/// The path the running den was started from (unresolved where the OS
/// reports it that way), or nullopt if it cannot be determined.
std::optional<std::filesystem::path> running_exe_path();

/// Fetch the latest available release version from GitHub Releases.
/// Returns empty string if unable to determine (network error, API oddity, etc.).
/// Unlike check_for_update, this always returns the version — it does NOT compare
/// against DEN_VERSION, making it suitable for --check / --dry-run queries.
std::string resolve_latest_version();

/// Check GitHub Releases for a newer version of den.
/// Returns nullopt if already up to date or unable to determine.
std::optional<UpdateInfo> check_for_update(const Config& cfg);

/// Download and install a new den binary, replacing the current one.
/// The running binary is atomically replaced via rename.
void apply_update(const UpdateInfo& info, const Config& cfg);

/// After extracting an update tarball into `extracted_root`, refresh an
/// existing <exe_dir>/../share/den/known_hashes.json from the tarball's
/// share/den/known_hashes.json (🎯T42). No-op if either is absent.
void refresh_packaged_replica(const std::filesystem::path& extracted_root,
                              const std::filesystem::path& exe);

} // namespace den
