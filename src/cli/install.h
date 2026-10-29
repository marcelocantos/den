// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/config.h"
#include "../env/environment.h"
#include "../env/version_change.h"
#include "../index/package.h"
#include "../provider/package_provider.h"

#include <cstdint>
#include <string>
#include <vector>

namespace den {

/// True when every name is a den-installed cask ("cask/" prefix allowed).
/// Casks link their own binaries (each reported as "==> Linked <name>"),
/// so materialise has nothing of theirs to count.
bool cask_only(const Config& config, const std::vector<std::string>& names);

/// Whether to print the post-materialise symlink count. A 0 after a
/// cask-only operation is noise ("Materialised: 0 symlinks"), not news.
bool report_link_count(uint32_t links, bool casks_only);

/// Throw a UserError describing rolled-back changes and remaining conflicts,
/// if there are any.
void report_version_change_failures(const Config& config, const VersionChangeResult& res,
                                    const std::string& verb);

/// Install packages by name. Dispatches each install through `provider`,
/// then updates the active env's manifest with the provider-reported
/// resolved version + auto-deps, and re-materialises the env. `link_opts`
/// carries `--overwrite` through to the root env's prefix linking.
void install_packages(const Config& config, PackageProvider& provider, const PackageIndex& idx,
                      const std::vector<std::string>& names,
                      const MaterialiseOptions& link_opts = {});

/// Uninstall packages by name. Dispatches each uninstall through `provider`
/// (Homebrew keeps files in the shared Cellar; other providers may reclaim),
/// removes the entries from the active env's manifest, and re-materialises.
void uninstall_packages(const Config& config, PackageProvider& provider,
                        const std::vector<std::string>& names);

/// Upgrade packages. If `names` is empty, upgrades all packages whose
/// installed version differs from the index's available version. Each
/// install is dispatched through `provider`. Installed runtime dependents the
/// index revision-bumped (same upstream version, higher `_<revision>`) are
/// upgraded in the same batch; pinned ones are reported and left. An upgrade whose new version
/// cannot be linked is rolled back (see apply_version_changes) and the call
/// throws a UserError naming it, after the other upgrades are applied.
void upgrade_packages(const Config& config, PackageProvider& provider, const PackageIndex& idx,
                      const std::vector<std::string>& names);

} // namespace den
