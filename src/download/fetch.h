// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <filesystem>
#include <string>

namespace den {

namespace fs = std::filesystem;

/// Download a URL to a content-addressed cache location, verify its
/// SHA256 hash, and return the cached file path.  If the file already
/// exists in the cache with the correct hash, the download is skipped.
fs::path fetch_archive(const std::string& url, const std::string& sha256,
                       const fs::path& cache_dir);

/// Fetch a blob from GHCR (GitHub Container Registry) using the OCI
/// distribution API.  Performs anonymous token auth against
/// ghcr.io/token, then fetches the blob by digest.  Returns the
/// cached file path.
///
/// repo_path: e.g. "homebrew/core/ffmpeg" (without ghcr.io prefix).
/// sha256: the expected sha256 digest of the blob.
fs::path fetch_ghcr_blob(const std::string& repo_path,
                         const std::string& sha256,
                         const fs::path& cache_dir);

} // namespace den
