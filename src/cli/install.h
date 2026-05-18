// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/config.h"
#include "../index/package.h"
#include "../provider/package_provider.h"

#include <string>
#include <vector>

namespace den {

/// Install packages by name. Dispatches each install through `provider`,
/// then updates the active env's manifest with the provider-reported
/// resolved version + auto-deps, and re-materialises the env.
void install_packages(const Config& config, PackageProvider& provider, const PackageIndex& idx,
                      const std::vector<std::string>& names);

/// Uninstall packages by name. Dispatches each uninstall through `provider`
/// (Homebrew keeps files in the shared Cellar; other providers may reclaim),
/// removes the entries from the active env's manifest, and re-materialises.
void uninstall_packages(const Config& config, PackageProvider& provider,
                        const std::vector<std::string>& names);

/// Upgrade packages. If `names` is empty, upgrades all packages whose
/// installed version differs from the index's available version. Each
/// install is dispatched through `provider`.
void upgrade_packages(const Config& config, PackageProvider& provider, const PackageIndex& idx,
                      const std::vector<std::string>& names);

} // namespace den
