// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "package.h"

#include "../core/config.h"
#include "../download/http.h"

#include <filesystem>
#include <string>

namespace den {

namespace fs = std::filesystem;

/// Load a PackageIndex from a local JSON cache file.
/// Returns an empty index if the file does not exist.
///
/// When a valid binary lookup cache (index_cache.h) sits next to the file,
/// the returned index is lazily backed by it: find() decodes only the
/// packages actually asked for. Otherwise index.json is parsed in full and
/// the lookup cache is (re)built for next time. Callers that iterate
/// `packages` must call PackageIndex::materialise_all() first.
PackageIndex load_index(const fs::path& cache_path);

/// Parse index.json in full, ignoring any lookup cache.
PackageIndex load_index_json(const fs::path& cache_path);

/// Save a PackageIndex to a local JSON cache file (atomic write), and
/// refresh its binary lookup cache (best effort).
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
/// Each download is retried on transient network/TLS errors
/// (fetch_url_retrying with the default policy).
PackageIndex fetch_and_transform(const Config& config);

/// fetch_and_transform() against the given URLs and retry policy (tests).
PackageIndex fetch_and_transform_from(const std::string& formula_url, const std::string& cask_url,
                                      const FetchRetryPolicy& policy = {});

} // namespace den
