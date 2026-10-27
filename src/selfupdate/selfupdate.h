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
