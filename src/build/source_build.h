// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/config.h"
#include "../index/package.h"

#include <filesystem>
#include <string>

namespace den {

namespace fs = std::filesystem;

/// Build a package from source using its Homebrew formula.
///
/// 1. Fetch source tarball (URL from index or brew info)
/// 2. Extract to a temp build directory
/// 3. Invoke the build (detected from formula: make, cmake, meson, etc.)
/// 4. Install into the den store at store/<name>/<version>/
///
/// Returns the store path of the installed package.
fs::path build_from_source(const Config& config, const PackageIndex& idx, const std::string& name,
                           const std::string& version);

/// Extract build metadata from a formula's Ruby source.
struct BuildRecipe {
    std::string source_url;
    std::string source_sha256;
    std::string build_system; // "make", "cmake", "meson", "autotools", "custom"
    std::vector<std::string> build_steps;
};

/// Fetch formula Ruby source via ruby_source_path from the index.
/// Returns the formula text, or empty string on failure.
std::string fetch_formula_source(const PackageIndex& idx, const std::string& name);

BuildRecipe extract_build_recipe(const PackageIndex& idx, const std::string& name);

/// The shell command for the bundled-Ruby build (Path A): den_build.rb run
/// by the bundled Ruby with an environment derived from `config` (see
/// bundled_ruby_env), so the build uses den's prefix and Cellar and never a
/// live Homebrew's paths.
std::string bundled_ruby_command(const Config& config, const fs::path& ruby_dir,
                                 const fs::path& formula_file, const std::string& name,
                                 const std::string& version);

/// The `brew` an unqualified `brew` would run on `path`, or empty.
fs::path find_brew_on_path(const std::string& path);

/// True when `brew` belongs to the Homebrew installed at `prefix`, i.e.
/// `brew ruby` would build into den's own Cellar. A brew for any other prefix
/// must not be used for den's source builds.
bool brew_serves_prefix(const fs::path& brew, const fs::path& prefix);

} // namespace den
