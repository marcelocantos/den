// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace den {

namespace fs = std::filesystem;

/// Size cap for in-memory fetches (index JSON, formula sources, metadata).
inline constexpr std::uint64_t kMaxInMemoryFetchBytes = 500ull * 1024 * 1024;

/// Size cap for archives streamed to disk (bottles, cask downloads). Large
/// casks such as flutter exceed 2 GiB; this only guards against runaways.
inline constexpr std::uint64_t kMaxArchiveDownloadBytes = 64ull * 1024 * 1024 * 1024;

/// Fetch a URL and return the response body as a string.
/// Throws DownloadError on failure.
std::string fetch_url(const std::string& url);

/// Stream a URL to `dest` without buffering it in memory. Fails if the body
/// exceeds `max_bytes`, or if the transfer stalls (no total time limit, so
/// multi-gigabyte downloads are not cut off). `headers` are extra request
/// headers ("Name: value"), e.g. a registry bearer token. On failure `dest`
/// is removed. Throws DownloadError.
void download_to_file(const std::string& url, const fs::path& dest,
                      std::uint64_t max_bytes = kMaxArchiveDownloadBytes,
                      const std::vector<std::string>& headers = {});

/// The message den reports for a failed transfer. libcurl's generic
/// curl_easy_strerror() text hides the real cause in some cases (a size cap
/// hit mid-stream over HTTP/2 comes back as CURLE_RECV_ERROR, "Failure when
/// receiving data from the peer"); prefer the detailed error buffer and name
/// the size limit when it was the cause.
std::string describe_transfer_error(const std::string& url, int curl_code,
                                    const std::string& error_buffer, std::uint64_t max_bytes);

} // namespace den
