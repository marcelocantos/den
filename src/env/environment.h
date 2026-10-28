// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../index/package.h"

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace den {

namespace fs = std::filesystem;

/// Return the environment directory for a given env path.
/// Layout: den_home/envs/<slug>/
fs::path env_dir(const fs::path& den_home, const std::string& env_path);

/// Options for materialise.
struct MaterialiseOptions {
    /// Packages whose conflicting prefix paths may be replaced (brew's
    /// `link --overwrite`). Only consulted for the root environment.
    std::set<std::string> overwrite;
};

/// A root-env path den refused to clobber, with the package that wanted it.
struct PrefixConflict {
    std::string package;
    fs::path path;
};

/// What materialise did: symlinks created, and root-env paths it refused to
/// clobber (the packages that wanted them were left as they were).
struct MaterialiseResult {
    uint32_t links = 0;
    std::vector<PrefixConflict> conflicts;
};

/// materialise, but conflicts are returned instead of thrown, so a caller
/// can undo the manifest change that caused them.
MaterialiseResult materialise_checked(const fs::path& den_home, const fs::path& store,
                                      const std::string& env_path,
                                      const PackageIndex* idx = nullptr,
                                      const MaterialiseOptions& opts = {});

/// The message materialise throws for `conflicts` in `prefix`.
std::string describe_conflicts(const fs::path& prefix,
                               const std::vector<PrefixConflict>& conflicts);

/// Materialise an environment by resolving its manifest (walking the
/// parent chain) and linking all resolved packages from the store.
/// Keg-only packages get an opt/ symlink but are not linked into
/// bin/, lib/, etc. (they shadow system tools).
///
/// The root environment ("/") is linked into the prefix itself
/// (store.parent_path(): bin, sbin, lib, include, share, etc, var,
/// Frameworks), the way `brew link` does, so tools that compute paths from
/// their own prefix (automake's aclocal dir, findutils' locate db, ...) find
/// what they expect. envs/ROOT keeps den's state and compat links to those
/// prefix dirs. Named environments are linked into envs/<slug> as before.
///
/// A root package whose links would clobber something den does not manage is
/// left unlinked; once every package has been processed a UserError lists the
/// conflicting paths. A root package already linked at another version keeps
/// that version linked (and recorded) when the new one would conflict: the
/// swap is checked before the old links come down.
/// Returns the total number of symlinks created.
uint32_t materialise(const fs::path& den_home, const fs::path& store, const std::string& env_path,
                     const PackageIndex* idx = nullptr, const MaterialiseOptions& opts = {});

/// True when envs/ROOT still uses the old layout (real link directories or
/// .den/linked records from before the root env was linked into the prefix).
bool root_env_needs_migration(const fs::path& den_home);

struct RootMigrationReport {
    bool migrated = false;
    uint32_t removed = 0;            // old envs/ROOT links into den's store, dropped
    uint32_t moved = 0;              // other links (e.g. cask binaries) moved into the prefix
    std::vector<fs::path> preserved; // envs/ROOT dirs holding non-link files, renamed aside
    std::string conflicts;           // materialise's conflict report, if any
};

/// One-time move of the root environment from envs/ROOT/{bin,lib,...} into
/// the prefix. Idempotent: does nothing once envs/ROOT is in the new layout.
RootMigrationReport migrate_root_env_to_prefix(const fs::path& den_home, const fs::path& store,
                                               const PackageIndex* idx = nullptr);

/// Read the active environment path from den_home/active_env.
/// Returns "/" if no active environment is set.
std::string active_env_path(const fs::path& den_home);

/// Set the active environment path (atomic write).
void set_active_env(const fs::path& den_home, const std::string& env_path);

} // namespace den
