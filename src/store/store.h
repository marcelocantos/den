// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <filesystem>
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

} // namespace den
