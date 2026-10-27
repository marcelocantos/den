// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "http.h"

#include "../core/error.h"

#include <spdlog/spdlog.h>

#include <curl/curl.h>

#include <cstddef>
#include <cstdio>
#include <memory>

namespace den {

namespace {

size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    size_t total = size * nmemb;
    buf->append(ptr, total);
    return total;
}

size_t file_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* f = static_cast<std::FILE*>(userdata);
    return std::fwrite(ptr, size, nmemb, f);
}

std::string format_bytes(std::uint64_t n) {
    constexpr std::uint64_t kMiB = 1024ull * 1024;
    constexpr std::uint64_t kGiB = kMiB * 1024;
    if (n >= kGiB && n % kGiB == 0)
        return std::to_string(n / kGiB) + " GiB";
    if (n >= kMiB && n % kMiB == 0)
        return std::to_string(n / kMiB) + " MiB";
    return std::to_string(n) + " bytes";
}

} // namespace

std::string describe_transfer_error(const std::string& url, int curl_code,
                                    const std::string& error_buffer, std::uint64_t max_bytes) {
    const auto code = static_cast<CURLcode>(curl_code);
    const std::string detail = error_buffer.empty() ? curl_easy_strerror(code) : error_buffer;
    const bool size_cap = code == CURLE_FILESIZE_EXCEEDED ||
                          detail.find("Maximum file size exceeded") != std::string::npos;
    if (size_cap) {
        return "download of " + url + " exceeds den's " + format_bytes(max_bytes) + " size limit";
    }
    return "HTTP fetch failed for " + url + ": " + detail;
}

std::string fetch_url(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw DownloadError("failed to initialise libcurl");
    }

    std::string response;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "den/" DEN_VERSION);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE,
                     static_cast<curl_off_t>(kMaxInMemoryFetchBytes));
    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        throw DownloadError(describe_transfer_error(url, res, errbuf, kMaxInMemoryFetchBytes));
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    // file:// URLs (local taps, offline tests) have no HTTP status; a
    // missing file already failed above with CURLE_FILE_COULDNT_READ_FILE.
    const bool is_file = url.starts_with("file://");
    if (!is_file && (http_code < 200 || http_code >= 300)) {
        throw DownloadError("HTTP " + std::to_string(http_code) + " fetching " + url);
    }

    return response;
}

void download_to_file(const std::string& url, const fs::path& dest, std::uint64_t max_bytes,
                      const std::vector<std::string>& headers) {
    std::error_code ec;
    if (dest.has_parent_path())
        fs::create_directories(dest.parent_path(), ec);
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> out(std::fopen(dest.c_str(), "wb"),
                                                        &std::fclose);
    if (!out) {
        throw DownloadError("cannot create download file: " + dest.string());
    }
    auto fail = [&](const std::string& message) {
        out.reset();
        fs::remove(dest, ec);
        throw DownloadError(message);
    };

    CURL* curl = curl_easy_init();
    if (!curl)
        fail("failed to initialise libcurl");
    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, file_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out.get());
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "den/" DEN_VERSION);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    // No overall deadline: a multi-gigabyte cask legitimately takes minutes.
    // Abort only when the transfer stalls (< 1 KiB/s for 60 s).
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(max_bytes));
    struct curl_slist* header_list = nullptr;
    for (const auto& h : headers)
        header_list = curl_slist_append(header_list, h.c_str());
    if (header_list)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(header_list);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_off_t received = 0;
    curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD_T, &received);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        // Belt and braces: report a cap hit as such whatever code curl used.
        if (received > 0 && static_cast<std::uint64_t>(received) > max_bytes) {
            res = CURLE_FILESIZE_EXCEEDED;
        }
        fail(describe_transfer_error(url, res, errbuf, max_bytes));
    }
    if (!url.starts_with("file://") && (http_code < 200 || http_code >= 300)) {
        fail("HTTP " + std::to_string(http_code) + " fetching " + url);
    }
    if (std::fflush(out.get()) != 0 || std::ferror(out.get())) {
        fail("write failed for: " + dest.string());
    }
    out.reset();
}

} // namespace den
