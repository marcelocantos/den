// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "http.h"

#include "../core/error.h"

#include <spdlog/spdlog.h>

#include <curl/curl.h>

#include <cstddef>
#include <cstdio>
#include <memory>
#include <thread>

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

/// libcurl reports a CURLOPT_MAXFILESIZE hit as CURLE_FILESIZE_EXCEEDED, or
/// (over HTTP/2, mid-stream) as a receive error whose detail names it.
bool is_size_cap_hit(CURLcode code, const std::string& detail) {
    return code == CURLE_FILESIZE_EXCEEDED ||
           detail.find("Maximum file size exceeded") != std::string::npos;
}

} // namespace

std::string describe_transfer_error(const std::string& url, int curl_code,
                                    const std::string& error_buffer, std::uint64_t max_bytes) {
    const auto code = static_cast<CURLcode>(curl_code);
    const std::string detail = error_buffer.empty() ? curl_easy_strerror(code) : error_buffer;
    if (is_size_cap_hit(code, detail)) {
        return "download of " + url + " exceeds den's " + format_bytes(max_bytes) + " size limit";
    }
    return "HTTP fetch failed for " + url + ": " + detail;
}

namespace {

/// One in-memory fetch. `error` is empty on success.
struct FetchAttempt {
    CURLcode res = CURLE_OK;
    long http_code = 0;
    /// Retry-After in seconds (either form, parsed by libcurl); 0 if absent.
    long long retry_after = 0;
    bool size_cap = false;
    std::string body;
    std::string error;
};

FetchAttempt fetch_once(const std::string& url) {
    FetchAttempt a;
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw DownloadError("failed to initialise libcurl");
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &a.body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "den/" DEN_VERSION);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE,
                     static_cast<curl_off_t>(kMaxInMemoryFetchBytes));
    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

    a.res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &a.http_code);
    curl_off_t retry_after = 0;
    if (curl_easy_getinfo(curl, CURLINFO_RETRY_AFTER, &retry_after) == CURLE_OK)
        a.retry_after = static_cast<long long>(retry_after);
    curl_easy_cleanup(curl);

    if (a.res != CURLE_OK) {
        a.error = describe_transfer_error(url, a.res, errbuf, kMaxInMemoryFetchBytes);
        a.size_cap = is_size_cap_hit(a.res, errbuf);
        return a;
    }

    // file:// URLs (local taps, offline tests) have no HTTP status; a
    // missing file already failed above with CURLE_FILE_COULDNT_READ_FILE.
    const bool is_file = url.starts_with("file://");
    if (!is_file && (a.http_code < 200 || a.http_code >= 300)) {
        a.error = "HTTP " + std::to_string(a.http_code) + " fetching " + url;
    }
    return a;
}

} // namespace

std::string fetch_url(const std::string& url) {
    FetchAttempt a = fetch_once(url);
    if (!a.error.empty()) {
        throw DownloadError(a.error);
    }
    return std::move(a.body);
}

bool is_transient_fetch_failure(int curl_code, long http_code) {
    // Copied from install.sh's is_transient_download_error(), which retries
    // curl exit codes 5|6|7|16|18|28|35|52|55|56|92, and HTTP 408|429|5xx
    // (curl exit 22). The two lists must stay in sync: change both or neither.
    switch (static_cast<CURLcode>(curl_code)) {
    case CURLE_OK:
        // Transfer completed: retry a request timeout, a rate limit, or a
        // server error. Every other status (404, 403, ...) is final.
        return http_code == 408 || http_code == 429 || (http_code >= 500 && http_code <= 599);
    case CURLE_COULDNT_RESOLVE_PROXY: // 5
    case CURLE_COULDNT_RESOLVE_HOST:  // 6
    case CURLE_COULDNT_CONNECT:       // 7
    case CURLE_HTTP2:                 // 16
    case CURLE_PARTIAL_FILE:          // 18
    case CURLE_OPERATION_TIMEDOUT:    // 28
    case CURLE_SSL_CONNECT_ERROR:     // 35
    case CURLE_GOT_NOTHING:           // 52
    case CURLE_SEND_ERROR:            // 55
    case CURLE_RECV_ERROR:            // 56
    case CURLE_HTTP2_STREAM:          // 92
        return true;
    default:
        return false;
    }
}

std::string fetch_url_retrying(const std::string& url, const FetchRetryPolicy& policy) {
    const int max_attempts = policy.max_attempts < 1 ? 1 : policy.max_attempts;
    for (int attempt = 1;; ++attempt) {
        FetchAttempt a = fetch_once(url);
        if (a.error.empty()) {
            return std::move(a.body);
        }
        const bool transient = !a.size_cap && is_transient_fetch_failure(a.res, a.http_code);
        if (!transient) {
            throw DownloadError(a.error);
        }
        if (attempt >= max_attempts) {
            throw DownloadError(a.error + " (gave up after " + std::to_string(attempt) +
                                (attempt == 1 ? " attempt)" : " attempts)"));
        }
        std::chrono::milliseconds delay = policy.base_delay * attempt;
        std::string note;
        if (a.http_code == 429 && a.retry_after > 0) {
            // Honour the server's Retry-After, within the ceiling. Clamp in
            // seconds first so a huge value cannot overflow milliseconds.
            constexpr long long kDay = 24 * 60 * 60;
            delay = std::chrono::seconds(a.retry_after < kDay ? a.retry_after : kDay);
            note = " (Retry-After: " + std::to_string(a.retry_after) + " s";
            note += delay > policy.max_delay ? ", capped)" : ")";
        }
        if (delay > policy.max_delay)
            delay = policy.max_delay;
        if (delay.count() < 0)
            delay = std::chrono::milliseconds(0);
        const std::string msg = a.error + "; retrying (" + std::to_string(attempt) + " of " +
                                std::to_string(max_attempts - 1) + ") in " +
                                std::to_string(delay.count()) + " ms" + note;
        SPDLOG_WARN("{}", msg);
        if (policy.on_retry)
            policy.on_retry(msg);
        if (policy.sleep)
            policy.sleep(delay);
        else if (delay.count() > 0)
            std::this_thread::sleep_for(delay);
    }
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
