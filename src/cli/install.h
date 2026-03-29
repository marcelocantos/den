// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/config.h"
#include "../index/package.h"

#include <string>
#include <vector>

namespace den {

/// Install packages by name. Resolves dependencies, downloads archives,
/// extracts into the store, updates the manifest, and re-materialises
/// the active environment.
void install_packages(const Config& config, const PackageIndex& idx,
                      const std::vector<std::string>& names);

/// Uninstall packages by name. Removes from the manifest and
/// re-materialises. Does not remove files from the store (use cleanup).
void uninstall_packages(const Config& config, const std::vector<std::string>& names);

/// Upgrade packages. If names is empty, upgrades all outdated packages.
void upgrade_packages(const Config& config, const PackageIndex& idx,
                      const std::vector<std::string>& names);

} // namespace den
