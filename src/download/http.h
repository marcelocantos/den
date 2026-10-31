// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
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

/// How fetch_url_retrying() retries a failed fetch.
struct FetchRetryPolicy {
    /// Tries in all, including the first (values below 1 count as 1).
    int max_attempts = 3;
    /// Before try n+1, wait base_delay * n (so 2 s, then 4 s, by default).
    std::chrono::milliseconds base_delay{2000};
    /// Ceiling on any single wait (default 4 s, the longest default backoff).
    /// It caps both the backoff and a server's Retry-After on an HTTP 429 or
    /// 503, so a "Retry-After: 3600" cannot stall `den update`, self-update
    /// or the daemon.
    std::chrono::milliseconds max_delay{4000};
    /// Called with each retry's log message after it is logged (tests).
    std::function<void(const std::string&)> on_retry;
    /// Performs each wait; empty means std::this_thread::sleep_for. Tests
    /// inject one to record the computed delays without sleeping.
    std::function<void(std::chrono::milliseconds)> sleep;
};

/// True when a failed fetch says nothing about the bytes and is worth
/// retrying: DNS, connect and send/receive failures (connection reset or
/// refused), timeouts, TLS handshake and record errors (e.g. LibreSSL's
/// "bad decrypt"), an empty reply or truncated body, HTTP/2 stream errors,
/// HTTP 408 (request timeout), 429 (too many requests) and 5xx. Any other
/// HTTP 4xx, and a size-cap hit, is never transient. (Whether to retry is
/// decided here; how long to wait, including Retry-After on a 429 or 503,
/// is fetch_url_retrying()'s business.) The same set as
/// install.sh's is_transient_download_error(); keep the two in sync.
/// `curl_code` is a CURLcode; `http_code` is the response status (0 if none).
bool is_transient_fetch_failure(int curl_code, long http_code);

/// fetch_url(), retrying transient failures (is_transient_fetch_failure)
/// with a linear backoff, logging a warning for each retry. On an HTTP 429
/// or 503 a Retry-After header (seconds or HTTP-date) replaces the backoff;
/// other statuses ignore it. Every wait is capped at policy.max_delay.
/// Anything else fails at once. After the last try it throws a
/// DownloadError naming the last error and the number of attempts.
std::string fetch_url_retrying(const std::string& url, const FetchRetryPolicy& policy = {});

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
