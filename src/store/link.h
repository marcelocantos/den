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

/// Validate a package name. Must match [a-zA-Z0-9_][a-zA-Z0-9_.+-]*(@[a-zA-Z0-9]+)?
/// and must not contain "..".
bool is_valid_package_name(const std::string& name);

/// Recursively symlink the contents of each `subdir` under `pkg_path` into
/// `env_dir/<subdir>`. The caller (typically the env materialiser) picks
/// the subdir set from the owning provider's `binary_paths()` — Homebrew
/// surfaces `bin/sbin/lib/include/share`; other providers may surface
/// fewer. Also creates `opt/<name>` pointing to the package root.
/// Returns the number of symlinks created.
uint32_t link_package(const fs::path& pkg_path, const fs::path& env_dir, const std::string& name,
                      const std::vector<std::string>& subdirs);

/// Remove symlinks created by `link_package`. Pass the same `subdirs`
/// the original `link_package` call used.
/// Returns the number of symlinks removed.
uint32_t unlink_package(const fs::path& pkg_path, const fs::path& env_dir, const std::string& name,
                        const std::vector<std::string>& subdirs);

/// Outcome of ensure_prefix_opt_link.
enum class OptLinkAction {
    Created,   // link was missing or dangling and now points at the keg
    Repointed, // link pointed at an older den-poured keg and now points at this one
    Unchanged, // link already resolves to this keg
    Kept,      // link resolves to another keg den does not own (Homebrew's) or a newer one
    Failed,    // filesystem error (logged)
};

/// Maintain Homebrew's shared `<prefix>/opt/<name>` link for a keg den has
/// poured or linked, where `<prefix>` is `store.parent_path()` (the same
/// prefix relocate_bottle substitutes for @@HOMEBREW_PREFIX@@). Text files
/// such as `#!<prefix>/opt/node/bin/node` shebangs and wrapper scripts are
/// relocated to that path and need the link to exist.
///
/// The link is created (relative, `../Cellar/<name>/<version>`, as Homebrew
/// writes it) when missing or dangling. An existing link is repointed only
/// when it resolves to an *older* keg of the same formula that den itself
/// poured (keg_owned_by_den). A link resolving to a keg den does not own
/// (Homebrew's) is never touched.
OptLinkAction ensure_prefix_opt_link(const fs::path& store, const std::string& name,
                                     const fs::path& keg);

/// If `<store.parent>/opt/<name>` points at `keg` (resolved lexically, so a
/// link left dangling by deleting the keg still matches), remove it. Returns
/// true when a link was removed.
bool remove_prefix_opt_link_to(const fs::path& store, const std::string& name, const fs::path& keg);

/// Record which version of a package is linked in an environment.
/// Writes to env_dir/.den/linked/<name>.
void record_linked_version(const fs::path& env_dir, const std::string& name,
                           const std::string& version);

/// Read which version of a package is linked in an environment.
std::optional<std::string> linked_version(const fs::path& env_dir, const std::string& name);

} // namespace den
