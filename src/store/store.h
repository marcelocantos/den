// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace den {

namespace fs = std::filesystem;

struct InstalledPackage {
    std::string name;
    std::string version;
    fs::path path;
};

/// Return the path for a specific package version in the store.
/// Layout: store/<name>/<version>/
fs::path package_path(const fs::path& store, const std::string& name, const std::string& version);

/// Check whether a package version is installed in the store.
bool is_installed(const fs::path& store, const std::string& name, const std::string& version);

/// List all installed packages by scanning the store directory.
std::vector<InstalledPackage> list_installed(const fs::path& store);

/// Identify which installed package owns a given file path.
/// Resolves symlinks, then matches the real path against the store layout.
/// Returns nullopt if the file doesn't belong to any installed package.
std::optional<InstalledPackage> which_package(const fs::path& store, const fs::path& file);

} // namespace den
