// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "http.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace den {

namespace fs = std::filesystem;

/// Download a URL to a content-addressed cache location, verify its
/// SHA256 hash, and return the cached file path.  If the file already
/// exists in the cache with the correct hash, the download is skipped.
/// An empty or "no_check" sha256 (casks with `sha256 :no_check`) skips
/// verification, as before. Transient transfer failures are retried with
/// `retry` (fetch_verified_to): each attempt restarts from scratch, nothing
/// partial or unverified is left behind, and a SHA256 mismatch, an HTTP
/// 4xx or a size-cap hit is not retried.
fs::path fetch_archive(const std::string& url, const std::string& sha256, const fs::path& cache_dir,
                       const FetchRetryPolicy& retry = {});

/// Stream `url` to `<cached>.download`, verify it against `sha256`, and only
/// then publish it at `cached`. The body is never held in memory and is
/// subject to kMaxArchiveDownloadBytes. On a mismatch or transfer error
/// nothing is left behind. `headers` are extra request headers. An empty or
/// "no_check" sha256 skips verification. Throws DownloadError.
/// With `retry`, transient transfer failures are retried
/// (download_to_file_retrying): each attempt restarts the download from
/// scratch with the same headers, and no partial file survives between
/// attempts. A SHA256 mismatch is never retried. Without it, one attempt.
void fetch_verified_to(const std::string& url, const std::string& sha256, const fs::path& cached,
                       const std::vector<std::string>& headers = {},
                       const std::optional<FetchRetryPolicy>& retry = std::nullopt);

/// Fetch a blob from GHCR (GitHub Container Registry) using the OCI
/// distribution API.  Performs anonymous token auth against
/// ghcr.io/token, then fetches the blob by digest.  Returns the
/// cached file path.
///
/// repo_path: e.g. "homebrew/core/ffmpeg" (without ghcr.io prefix).
/// sha256: the expected sha256 digest of the blob.
/// `sha256` must be 64 hex digits: the blob is always verified.
/// The token request (fetch_url_retrying) and the blob download
/// (fetch_verified_to) both retry transient failures with `retry`; the
/// bearer token goes on every blob attempt and never on the token request.
/// `registry` is the registry's base URL (tests point it at a local server).
fs::path fetch_ghcr_blob(const std::string& repo_path, const std::string& sha256,
                         const fs::path& cache_dir, const FetchRetryPolicy& retry = {},
                         const std::string& registry = "https://ghcr.io");

} // namespace den
