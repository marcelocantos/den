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

/// The parts of INSTALL_RECEIPT.json that den needs.
struct Tab {
    bool installed_on_request = true;
    std::vector<std::string> runtime_deps; // full_names of runtime dependencies
};

/// Scan a Homebrew Cellar directory and return all kegs found.
/// For packages with multiple installed versions, all versions are returned.
/// Results are sorted by name then version.
std::vector<HomebrewKeg> scan_homebrew_cellar(const fs::path& cellar);

/// Read the INSTALL_RECEIPT.json from a keg directory.
/// Returns nullopt if the file is absent or malformed.
std::optional<Tab> read_tab(const fs::path& keg_path);

/// Migrate packages from the Homebrew Cellar into den's root manifest.
///
/// If names is empty, scans the entire Cellar; otherwise migrates only the
/// named packages. For each package found, the latest version is recorded in
/// den's root manifest. Files are not copied — this is metadata migration only.
///
/// Prints a summary: N packages found, N added, N skipped (already tracked).
void migrate_from_homebrew(const Config& config, const std::vector<std::string>& names);

} // namespace den
