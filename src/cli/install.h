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

/// The refusal for a named formula homebrew-core has disabled, e.g.
/// "'rye' is disabled upstream: it is not maintained upstream (disabled on
/// 2026-08-18; replacement: uv)". The reason, date and replacement come from
/// the index's disable! fields and are left out when it has none. Homebrew's
/// reason symbols read as brew words them; a custom reason is quoted as-is.
std::string disabled_formula_message(const Package& pkg);

/// The named formulae in `names` the index marks disabled (casks and names
/// not in the index are not). Each is reported on stderr as
/// "error: <disabled_formula_message>".
std::vector<std::string> refuse_disabled_formulae(const PackageIndex& idx,
                                                  const std::vector<std::string>& names);

/// Install packages by name. Dispatches each install through `provider`,
/// then updates the active env's manifest with the provider-reported
/// resolved version + auto-deps, and re-materialises the env. `link_opts`
/// carries `--overwrite` through to the root env's prefix linking. A
/// formula the index marks disabled is refused (refuse_disabled_formulae):
/// the rest are installed, then the call throws a UserError naming how many
/// were refused.
void install_packages(const Config& config, PackageProvider& provider, const PackageIndex& idx,
                      const std::vector<std::string>& names,
                      const MaterialiseOptions& link_opts = {});

/// `den install -s`: plan the chain for `names` (G3), run it through
/// `runner`, record the targets and their runtime dependencies in the active
/// env's manifest (build-only kegs stay out), and materialise the env, which
/// links every recorded keg and runs its declarative post_install steps,
/// poured or built alike. Throws a UserError when any step was deferred, and
/// before planning anything when a named formula is disabled upstream.
void install_from_source(const Config& config, const PackageIndex& idx,
                         const std::vector<std::string>& names, const MaterialiseOptions& link_opts,
                         const SourcePlanRunner& runner);

/// Uninstall packages by name. Dispatches each uninstall through `provider`
/// (Homebrew keeps files in the shared Cellar; other providers may reclaim),
/// removes the entries from the active env's manifest, and re-materialises.
void uninstall_packages(const Config& config, PackageProvider& provider,
                        const std::vector<std::string>& names);

/// Upgrade packages. If `names` is empty, upgrades what outdated_plan lists
/// as pending for `provider`'s bucket, the selection `den outdated`, `den
/// doctor` and the daemon share: casks and disabled formulae are left out
/// (newer casks are listed as upgradable by name). A package named in
/// `names` is compared the same way (select_upgrade) and upgraded even when
/// it is a cask; a named formula the index marks disabled is refused
/// (refuse_disabled_formulae), the other names proceed, and the call then
/// throws a UserError, as for a named pinned package. Only the
/// active environment's bucket for `provider` is considered: a package
/// another provider manages is never compared against `idx` (the daemon's
/// maintenance cycle reads the homebrew bucket the same way). Each
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
