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

/// The directories a ROOT-environment package is linked into under the
/// prefix. This is Homebrew's Keg#link set, so a keg den links looks the same
/// on disk as one `brew link` would have produced.
const std::vector<std::string>& prefix_link_dirs();

/// Where ROOT-environment links live and where den keeps its bookkeeping.
struct PrefixLinkContext {
    fs::path prefix;    // store.parent_path(): the @@HOMEBREW_PREFIX@@ den relocates to
    fs::path store;     // the Cellar
    fs::path den_home;  // packages of non-Homebrew providers live under here
    fs::path state_dir; // envs/ROOT/.den: holds the link ledger and version records
};

PrefixLinkContext make_prefix_link_context(const fs::path& den_home, const fs::path& store);

/// Result of linking one package into the prefix.
struct PrefixLinkResult {
    uint32_t created = 0;
    /// Paths den would have had to clobber. When non-empty nothing was linked.
    std::vector<fs::path> conflicts;
};

/// Link the contents of `subdirs` of `pkg_path` into `<prefix>/<subdir>`,
/// following brew's conflict rules:
///  - a missing path, or a link that already points at the same file, is fine;
///  - a link into a keg den poured (or one den's ledger says it created), or
///    a dangling link, is replaced;
///  - anything else (a link into a keg den did not pour, or a real file) is a
///    conflict. With `overwrite` it is replaced instead. etc/ and var/ keep an
///    existing real file, as brew keeps config files.
/// Conflicts are checked for the whole package before anything is written,
/// so a refused package leaves the prefix untouched. Every link den writes is
/// recorded in a per-package ledger, which is all unlink_from_prefix removes.
PrefixLinkResult link_into_prefix(const PrefixLinkContext& ctx, const fs::path& pkg_path,
                                  const std::string& name, const std::vector<std::string>& subdirs,
                                  bool overwrite);

/// The conflicts link_into_prefix would report for this package, without
/// writing anything. Links den's ledgers own (an older version of the same
/// package, say) are replaceable, so this answers "would swapping to
/// `pkg_path` succeed?" while the old version is still linked.
std::vector<fs::path> prefix_link_conflicts(const PrefixLinkContext& ctx, const fs::path& pkg_path,
                                            const std::string& name,
                                            const std::vector<std::string>& subdirs,
                                            bool overwrite);

/// Point `<prefix>/opt/<name>` at `pkg_path` for a package whose root is not a
/// Homebrew Cellar keg (pip, npm, cargo, go). Created when missing or
/// dangling, or when den's ledger owns it; recorded in the ledger. Returns
/// true when the link was written.
bool link_opt_into_prefix(const PrefixLinkContext& ctx, const fs::path& pkg_path,
                          const std::string& name);

/// Remove the links den's ledger records for `name`, but only those that
/// still point where den pointed them. Empty directories left behind are
/// pruned. Clears the ledger and version record. Returns links removed.
uint32_t unlink_from_prefix(const PrefixLinkContext& ctx, const std::string& name);

/// Version record for a ROOT package linked into the prefix.
std::optional<std::string> prefix_linked_version(const PrefixLinkContext& ctx,
                                                 const std::string& name);
void record_prefix_linked_version(const PrefixLinkContext& ctx, const std::string& name,
                                  const std::string& version);
/// Names with a version record or a ledger.
std::vector<std::string> prefix_linked_names(const PrefixLinkContext& ctx);
/// True when den's ledger for `name` holds a link outside `<prefix>/opt`.
bool prefix_ledger_has_tree_links(const PrefixLinkContext& ctx, const std::string& name);

} // namespace den
