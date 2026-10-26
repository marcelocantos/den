// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "package.h"

#include "../core/config.h"

#include <filesystem>

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

/// Fetch Homebrew's formula.json and cask.json from the API,
/// transform them into den's unified Package format, and return
/// the combined index.
PackageIndex fetch_and_transform(const Config& config);

} // namespace den
