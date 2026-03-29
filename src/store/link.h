// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace den {

namespace fs = std::filesystem;

/// Validate a package name. Must match [a-zA-Z0-9_][a-zA-Z0-9_.+-]*(@[a-zA-Z0-9]+)?
/// and must not contain "..".
bool is_valid_package_name(const std::string& name);

/// Recursively symlink a package's public directories (bin, sbin, lib,
/// include, share) into an environment directory. Also creates
/// opt/<name> pointing to the package root.
/// Returns the number of symlinks created.
uint32_t link_package(const fs::path& pkg_path, const fs::path& env_dir,
                      const std::string& name);

/// Remove symlinks created by link_package.
/// Returns the number of symlinks removed.
uint32_t unlink_package(const fs::path& pkg_path, const fs::path& env_dir,
                        const std::string& name);

/// Record which version of a package is linked in an environment.
/// Writes to env_dir/.den/linked/<name>.
void record_linked_version(const fs::path& env_dir, const std::string& name,
                           const std::string& version);

/// Read which version of a package is linked in an environment.
std::optional<std::string> linked_version(const fs::path& env_dir,
                                          const std::string& name);

} // namespace den
