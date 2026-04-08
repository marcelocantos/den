// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../index/package.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace den {

namespace fs = std::filesystem;

/// Return the environment directory for a given env path.
/// Layout: den_home/envs/<slug>/
fs::path env_dir(const fs::path& den_home, const std::string& env_path);

/// Materialise an environment by resolving its manifest (walking the
/// parent chain) and linking all resolved packages from the store.
/// Keg-only packages get an opt/ symlink but are not linked into
/// bin/, lib/, etc. (they shadow system tools).
/// Returns the total number of symlinks created.
uint32_t materialise(const fs::path& den_home, const fs::path& store,
                     const std::string& env_path, const PackageIndex* idx = nullptr);

/// Read the active environment path from den_home/active_env.
/// Returns "/" if no active environment is set.
std::string active_env_path(const fs::path& den_home);

/// Set the active environment path (atomic write).
void set_active_env(const fs::path& den_home, const std::string& env_path);

} // namespace den
