// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "fetch.h"

#include "http.h"
#include "sha256.h"

#include "../core/error.h"

#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

namespace den {

void fetch_verified_to(const std::string& url, const std::string& sha256, const fs::path& cached,
                       const std::vector<std::string>& headers,
                       const std::optional<FetchRetryPolicy>& retry) {
    // Stream to a temporary file next to the cache entry, so large archives
    // are never held in memory, and only publish it once verified. Both
    // download paths remove `tmp` on any transfer failure.
    fs::path tmp = cached;
    tmp += ".download";
    if (retry) {
        download_to_file_retrying(url, tmp, *retry, kMaxArchiveDownloadBytes, headers);
    } else {
        download_to_file(url, tmp, kMaxArchiveDownloadBytes, headers);
    }

    if (!sha256.empty() && sha256 != "no_check") {
        std::string actual = hash_file(tmp);
        if (actual != sha256) {
            std::error_code rec;
            fs::remove(tmp, rec);
            throw DownloadError("SHA256 mismatch for " + url + ": expected " + sha256 + ", got " +
                                actual);
        }
    }
    std::error_code ec;
    fs::rename(tmp, cached, ec);
    if (ec) {
        std::error_code rec;
        fs::remove(tmp, rec);
        throw DownloadError("could not move " + tmp.string() + " into place: " + ec.message());
    }
}

namespace {

bool is_sha256_hex(const std::string& s) {
    return s.size() == 64 &&
           std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c); });
}

} // namespace

fs::path fetch_archive(const std::string& url, const std::string& sha256,
                       const fs::path& cache_dir) {
    fs::create_directories(cache_dir);

    // Content-addressed path: cache_dir/<sha256>[.ext from url].
    // Use just the sha256 as the filename with the original extension.
    // When sha256 is empty (cask "no_check"), key the cache by the URL hash.
    fs::path url_path(url);
    std::string ext;
    // Extract compound extensions like .tar.gz.
    std::string filename = url_path.filename().string();
    // Strip query strings from extension detection.
    auto qpos = filename.find('?');
    if (qpos != std::string::npos) {
        filename = filename.substr(0, qpos);
    }
    if (filename.ends_with(".tar.gz")) {
        ext = ".tar.gz";
    } else if (filename.ends_with(".tar.xz")) {
        ext = ".tar.xz";
    } else if (filename.ends_with(".tar.bz2")) {
        ext = ".tar.bz2";
    } else {
        auto dot = filename.rfind('.');
        ext = dot == std::string::npos ? std::string{} : filename.substr(dot);
    }

    const bool skip_verify = sha256.empty() || sha256 == "no_check";
    const std::string cache_key = skip_verify ? ("nocheck-" + hash_string(url)) : sha256;
    fs::path cached = cache_dir / (cache_key + ext);

    // If the file already exists and hash matches, skip download.
    if (fs::exists(cached)) {
        if (skip_verify || verify_file(cached, sha256)) {
            SPDLOG_INFO("cache hit for {}", cache_key);
            return cached;
        }
        // Hash mismatch — stale/corrupt cache entry. Remove and re-fetch.
        SPDLOG_WARN("cache file hash mismatch, re-downloading: {}", cached.string());
        fs::remove(cached);
    }

    SPDLOG_INFO("downloading {}", url);
    fetch_verified_to(url, skip_verify ? std::string{} : sha256, cached);

    SPDLOG_INFO("cached {} ({} bytes)", cache_key, fs::file_size(cached));
    return cached;
}

fs::path fetch_ghcr_blob(const std::string& repo_path, const std::string& sha256,
                         const fs::path& cache_dir, const FetchRetryPolicy& retry,
                         const std::string& registry) {
    // The blob is always verified: refuse a missing or malformed digest
    // rather than letting fetch_verified_to skip the check.
    if (!is_sha256_hex(sha256)) {
        throw DownloadError("GHCR blob for " + repo_path + " has no valid sha256 digest: '" +
                            sha256 + "'");
    }
    fs::create_directories(cache_dir);

    fs::path cached = cache_dir / (sha256 + ".tar.gz");

    // Check cache first.
    if (fs::exists(cached)) {
        if (verify_file(cached, sha256)) {
            SPDLOG_INFO("cache hit for ghcr blob {}", sha256);
            return cached;
        }
        SPDLOG_WARN("cache file hash mismatch, re-downloading ghcr blob");
        fs::remove(cached);
    }

    // Step 1: Get anonymous bearer token from ghcr.io.
    std::string token_url =
        registry + "/token?scope=repository:" + repo_path + ":pull&service=ghcr.io";

    SPDLOG_INFO("requesting GHCR token for {}", repo_path);
    std::string token_response = fetch_url_retrying(token_url, retry);

    auto token_json = nlohmann::json::parse(token_response);
    if (!token_json.contains("token")) {
        throw DownloadError("GHCR token response missing 'token' field");
    }
    std::string token = token_json["token"].get<std::string>();

    // Step 2: Fetch the blob using the OCI distribution API.
    // The blob URL uses the sha256 digest.
    std::string blob_url = registry + "/v2/" + repo_path + "/blobs/sha256:" + sha256;

    SPDLOG_INFO("fetching GHCR blob sha256:{}", sha256);
    // The registry answers with a redirect to a CDN URL; the bearer token is
    // only needed on the first hop. Streamed to disk like other archives, so
    // large bottles are not buffered in memory or capped at 500 MiB. Every
    // attempt restarts from scratch and carries the token again.
    fetch_verified_to(blob_url, sha256, cached, {"Authorization: Bearer " + token}, retry);

    SPDLOG_INFO("cached GHCR blob {} ({} bytes)", sha256, fs::file_size(cached));
    return cached;
}

} // namespace den
