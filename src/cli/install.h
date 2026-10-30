// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../build/source_plan.h"
#include "../core/config.h"
#include "../env/environment.h"
#include "../env/version_change.h"
#include "../index/package.h"
#include "../provider/package_provider.h"
#include "../provider/registry.h"

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

/// Why a command that needs the Homebrew package index cannot proceed before
/// the first `den update` (the index is missing, empty, or unreadable).
std::string empty_index_message();

/// Throw UserError(empty_index_message()) when `idx` holds no packages, so
/// the CLI prints "error: ..." and exits 1 instead of exiting 0.
void require_index(const PackageIndex& idx);

/// Install packages by name. Dispatches each install through `provider`,
/// then updates the active env's manifest with the provider-reported
/// resolved version + auto-deps, and re-materialises the env. `link_opts`
/// carries `--overwrite` through to the root env's prefix linking.
void install_packages(const Config& config, PackageProvider& provider, const PackageIndex& idx,
                      const std::vector<std::string>& names,
                      const MaterialiseOptions& link_opts = {});

/// `den install -s`: plan the chain for `names` (G3), run it through
/// `runner`, record the targets and their runtime dependencies in the active
/// env's manifest (build-only kegs stay out), and materialise the env, which
/// links every recorded keg and runs its declarative post_install steps,
/// poured or built alike. Throws a UserError when any step was deferred.
void install_from_source(const Config& config, const PackageIndex& idx,
                         const std::vector<std::string>& names, const MaterialiseOptions& link_opts,
                         const SourcePlanRunner& runner);

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
/// An index version below the installed one is never applied: a lower
/// version in the same scheme is reported as "index older than installed",
/// and a lower version with a higher version_scheme as held (with its
/// reason); `den use <name> <version>` is the only way down. With `dry_run`
/// the same plan is printed and nothing is installed, linked or recorded.
void upgrade_packages(const Config& config, PackageProvider& provider, const PackageIndex& idx,
                      const std::vector<std::string>& names, bool dry_run = false);

/// `den upgrade` with no names: exactly one upgrade_packages pass over
/// every installed package. That pass already scans the whole resolved
/// environment, so it runs once, not once per manifest provider bucket
/// (which planned, printed and dispatched the same upgrades again for each
/// further bucket, through that bucket's provider). Upgrade candidates come
/// from the package index, which is Homebrew's, so the pass dispatches
/// through the "homebrew" provider, or the registry's default when none is
/// registered under that name. Buckets under an unregistered provider are
/// reported, as before.
void upgrade_all_packages(const Config& config, const ProviderRegistry& registry,
                          const PackageIndex& idx, bool dry_run = false);

} // namespace den
