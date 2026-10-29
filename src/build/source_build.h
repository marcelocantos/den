// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/config.h"
#include "../index/package.h"
#include "../store/store.h"

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

/// Where a formula's Ruby source came from.
struct FormulaOrigin {
    std::string tap;    // "homebrew/core", or "<user>/<repo>" for a tap formula
    std::string commit; // the commit fetched; empty when unpinned or local
    std::string path;   // ruby_source_path ("Formula/t/tree.rb", or a local file)
};

/// The URL fetch_formula_source fetches a homebrew-core formula from: by
/// `pkg.tap_git_head` when the index has one, else from master.
std::string formula_source_url(const Package& pkg);

/// Fetch formula Ruby source via ruby_source_path from the index.
/// A homebrew-core formula is fetched at the index's tap_git_head and, when
/// the index carries ruby_source_sha256, verified against it: a mismatch
/// throws UserError (the recipe is not the one the index describes).
/// Returns the formula text, or empty string when it cannot be fetched.
/// `origin`, when given, receives where the text came from.
std::string fetch_formula_source(const PackageIndex& idx, const std::string& name,
                                 FormulaOrigin* origin = nullptr);

/// Parse den_build.rb's result lines ("source=<url>\t<sha256>",
/// "resource=<name>\t<url>\t<sha256>", "patch=<url>\t<sha256>") into the
/// receipt's inputs. Other lines are ignored.
void parse_ruby_build_inputs(const std::string& output, SourceBuildReceipt& receipt);

BuildRecipe extract_build_recipe(const PackageIndex& idx, const std::string& name);

/// The shell command for the bundled-Ruby build (Path A): den_build.rb run
/// by the bundled Ruby with an environment derived from `config` (see
/// bundled_ruby_env), so the build uses den's prefix and Cellar and never a
/// live Homebrew's paths.
std::string bundled_ruby_command(const Config& config, const fs::path& ruby_dir,
                                 const fs::path& formula_file, const std::string& name,
                                 const std::string& version);

/// Called when a Ruby build of `name` `version` failed (`why`) and den is
/// about to build it with its native recipe parser instead. With
/// build.strict set in `den_home`'s config this throws UserError. Otherwise
/// it prints a warning and records "fallback": "native" in `receipt`.
void allow_native_fallback(const fs::path& den_home, const std::string& name,
                           const std::string& version, const std::string& why,
                           SourceBuildReceipt& receipt);

/// The `brew` an unqualified `brew` would run on `path`, or empty.
fs::path find_brew_on_path(const std::string& path);

/// True when `brew` belongs to the Homebrew installed at `prefix`, i.e.
/// `brew ruby` would build into den's own Cellar. A brew for any other prefix
/// must not be used for den's source builds.
bool brew_serves_prefix(const fs::path& brew, const fs::path& prefix);

} // namespace den
