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
fs::path build_from_source(const Config& config, const PackageIndex& idx,
                           const std::string& name, const std::string& version);

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

} // namespace den
