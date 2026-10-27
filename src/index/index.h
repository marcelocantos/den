// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "package.h"

#include "../core/config.h"

#include <filesystem>
#include <string>

namespace den {

namespace fs = std::filesystem;

/// Load a PackageIndex from a local JSON cache file.
/// Returns an empty index if the file does not exist.
PackageIndex load_index(const fs::path& cache_path);

/// Save a PackageIndex to a local JSON cache file (atomic write).
void save_index(const PackageIndex& idx, const fs::path& cache_path);

/// Transform raw Homebrew formula.json and cask.json payloads (JSON arrays)
/// into den's unified index. Cask archives are trimmed with
/// trim_cask_archives. Throws DownloadError if either payload fails to parse.
PackageIndex transform_homebrew(const std::string& formula_json, const std::string& cask_json);

/// Remove cask archive variants that can never change which download den
/// picks on macOS.
///
/// Homebrew's cask.json carries per-OS/arch "variations" (including Linux
/// ones and many that repeat the default URL). Casks are macOS-only, so:
///   1. every "*_linux" variant is dropped, and
///   2. any other variant is dropped when, for every macOS arch/version den
///      can run on, best_archive_tag() would still resolve to an archive with
///      the same url and sha256 without it.
/// The archive den selects for each (arch, macOS version) is therefore
/// unchanged, and the result stays in the existing index format. Packages
/// with no selectable archive on any macOS platform are left untouched.
void trim_cask_archives(Package& pkg);

/// Fetch Homebrew's formula.json and cask.json from the API,
/// transform them into den's unified Package format, and return
/// the combined index.
PackageIndex fetch_and_transform(const Config& config);

} // namespace den
