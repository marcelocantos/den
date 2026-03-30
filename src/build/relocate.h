// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace den {

namespace fs = std::filesystem;

/// Relocation: replace path prefixes in text and binary files.
/// Used for:
/// 1. `:any` bottle relocation (@@HOMEBREW_PREFIX@@ → den's paths)
/// 2. Ruby binary relocation (hardcoded prefix → den's paths)

/// Replace @@HOMEBREW_PREFIX@@, @@HOMEBREW_CELLAR@@, and similar
/// placeholders in all text files under a directory tree.
/// Returns the number of files modified.
uint32_t relocate_text_placeholders(const fs::path& dir, const std::string& prefix,
                                    const std::string& cellar);

/// Replace a hardcoded path string in binary files. The replacement
/// must be <= the original length; the remainder is null-padded.
/// Returns the number of files modified.
uint32_t relocate_binary_paths(const fs::path& dir, const std::string& old_path,
                               const std::string& new_path);

/// Fix Mach-O dylib install names and rpaths using install_name_tool.
/// Rewrites @rpath references to point at the package's own lib/.
uint32_t fix_dylib_paths(const fs::path& dir, const fs::path& prefix);

/// Full relocation pass for a poured bottle.
/// Applies text placeholder replacement + dylib path fixes.
void relocate_bottle(const fs::path& package_dir, const std::string& name,
                     const std::string& version, const fs::path& store);

/// Relocate the bundled Portable Ruby binary to work from a new prefix.
void relocate_ruby(const fs::path& ruby_dir, const std::string& original_prefix);

} // namespace den
