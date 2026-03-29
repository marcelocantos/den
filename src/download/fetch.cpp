// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "fetch.h"

#include "http.h"
#include "sha256.h"

#include "../core/error.h"

#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include <curl/curl.h>

#include <fstream>

namespace den {

namespace {

size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    size_t total = size * nmemb;
    buf->append(ptr, total);
    return total;
}

/// Write raw data to a file atomically (write to .tmp, then rename).
void write_file_atomic(const fs::path& path, const std::string& data) {
    fs::path tmp = path;
    tmp += ".tmp";

    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out) {
            throw DownloadError("cannot create cache file: " + tmp.string());
        }
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) {
            throw DownloadError("write failed for: " + tmp.string());
        }
    }

    fs::rename(tmp, path);
}

} // namespace

fs::path fetch_archive(const std::string& url, const std::string& sha256,
                       const fs::path& cache_dir) {
    fs::create_directories(cache_dir);

    // Content-addressed path: cache_dir/<sha256>[.ext from url].
    // Use just the sha256 as the filename with the original extension.
    fs::path url_path(url);
    std::string ext;
    // Extract compound extensions like .tar.gz.
    std::string filename = url_path.filename().string();
    if (filename.ends_with(".tar.gz")) {
        ext = ".tar.gz";
    } else if (filename.ends_with(".tar.xz")) {
        ext = ".tar.xz";
    } else if (filename.ends_with(".tar.bz2")) {
        ext = ".tar.bz2";
    } else {
        ext = url_path.extension().string();
    }

    fs::path cached = cache_dir / (sha256 + ext);

    // If the file already exists and hash matches, skip download.
    if (fs::exists(cached)) {
        if (verify_file(cached, sha256)) {
            SPDLOG_INFO("cache hit for {}", sha256);
            return cached;
        }
        // Hash mismatch — stale/corrupt cache entry. Remove and re-fetch.
        SPDLOG_WARN("cache file hash mismatch, re-downloading: {}", cached.string());
        fs::remove(cached);
    }

    SPDLOG_INFO("downloading {}", url);
    std::string data = fetch_url(url);

    // Verify hash before writing to cache.
    std::string actual = hash_string(data);
    if (actual != sha256) {
        throw DownloadError("SHA256 mismatch for " + url + ": expected " + sha256 + ", got " +
                            actual);
    }

    write_file_atomic(cached, data);

    SPDLOG_INFO("cached {} ({} bytes)", sha256, data.size());
    return cached;
}

fs::path fetch_ghcr_blob(const std::string& repo_path, const std::string& sha256,
                         const fs::path& cache_dir) {
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
        "https://ghcr.io/token?scope=repository:" + repo_path + ":pull&service=ghcr.io";

    SPDLOG_INFO("requesting GHCR token for {}", repo_path);
    std::string token_response = fetch_url(token_url);

    auto token_json = nlohmann::json::parse(token_response);
    if (!token_json.contains("token")) {
        throw DownloadError("GHCR token response missing 'token' field");
    }
    std::string token = token_json["token"].get<std::string>();

    // Step 2: Fetch the blob using the OCI distribution API.
    // The blob URL uses the sha256 digest.
    std::string blob_url = "https://ghcr.io/v2/" + repo_path + "/blobs/sha256:" + sha256;

    SPDLOG_INFO("fetching GHCR blob sha256:{}", sha256);

    // We need to pass the Authorization header. Since fetch_url
    // doesn't support custom headers, we use libcurl directly here.
    // For now, we construct a curl request inline. A future refactor
    // could add header support to fetch_url.

    // Use the existing fetch_url for the blob — GHCR redirects to a
    // CDN URL that doesn't need auth. But we need the initial
    // redirect with the token. Build a minimal curl call.
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw DownloadError("failed to initialise libcurl for GHCR");
    }

    std::string response;

    curl_easy_setopt(curl, CURLOPT_URL, blob_url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "den/" DEN_VERSION);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(500 * 1024 * 1024));

    // Set the Authorization header.
    struct curl_slist* headers = nullptr;
    std::string auth_header = "Authorization: Bearer " + token;
    headers = curl_slist_append(headers, auth_header.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        std::string err = curl_easy_strerror(res);
        curl_easy_cleanup(curl);
        throw DownloadError("GHCR blob fetch failed: " + err);
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (http_code < 200 || http_code >= 300) {
        throw DownloadError("HTTP " + std::to_string(http_code) +
                            " fetching GHCR blob sha256:" + sha256);
    }

    // Verify hash.
    std::string actual = hash_string(response);
    if (actual != sha256) {
        throw DownloadError("SHA256 mismatch for GHCR blob: expected " + sha256 + ", got " +
                            actual);
    }

    write_file_atomic(cached, response);

    SPDLOG_INFO("cached GHCR blob {} ({} bytes)", sha256, response.size());
    return cached;
}

} // namespace den
