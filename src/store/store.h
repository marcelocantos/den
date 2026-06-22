// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
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

/// A keg (one installed version) enriched with Cellar-inspection metadata
/// for `den list --cellar`.
struct KegInfo {
    std::string name;
    std::string version;
    fs::path path;
    uint64_t disk_bytes = 0;           // recursive on-disk size of the keg
    std::vector<std::string> env_refs; // environment paths referencing this keg, sorted
    bool orphaned() const { return env_refs.empty(); }
};

/// Recursively sum the on-disk size of a keg directory. Follows no symlinks
/// (counts the link entry, not its target) and tolerates transient I/O errors.
uint64_t keg_disk_usage(const fs::path& keg);

/// Inspect every keg in the Cellar: disk usage, the environments that
/// reference it, and whether it is orphaned (referenced by no environment).
/// References are derived from the environment manifests under den_home.
/// The result is sorted by (name, version) for deterministic output.
std::vector<KegInfo> inspect_cellar(const fs::path& store, const fs::path& den_home);

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
