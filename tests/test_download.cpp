// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Archive downloads stream to disk. Previously every archive was buffered in
// memory through fetch_url(), with a 500 MiB cap and a 60 s total timeout, so
// large casks (flutter is 2.27 GB) could never install, and the cap hit was
// reported as libcurl's generic "Failure when receiving data from the peer".

#include "core/error.h"
#include "download/fetch.h"
#include "download/http.h"
#include "download/sha256.h"

#include <curl/curl.h>
#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace den;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() / ("den-download-" + std::to_string(::getpid()));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string file_url(const fs::path& p) {
    return "file://" + fs::absolute(p).string();
}

void write_pattern(const fs::path& p, std::size_t bytes) {
    std::ofstream out(p, std::ios::binary);
    std::string chunk(64 * 1024, '\0');
    for (std::size_t i = 0; i < chunk.size(); ++i)
        chunk[i] = static_cast<char>(i * 31 + 7);
    for (std::size_t written = 0; written < bytes;) {
        std::size_t n = std::min(chunk.size(), bytes - written);
        out.write(chunk.data(), static_cast<std::streamsize>(n));
        written += n;
    }
}

std::string error_of(auto&& fn) {
    try {
        fn();
    } catch (const DownloadError& e) {
        return e.what();
    }
    return {};
}

} // namespace

TEST_SUITE("download::streaming") {
    TEST_CASE("download_to_file streams the body to disk intact") {
        TempDir tmp;
        fs::path src = tmp.path / "src.bin";
        write_pattern(src, 3 * 1024 * 1024 + 17);
        fs::path dest = tmp.path / "out" / "dest.bin";
        download_to_file(file_url(src), dest);
        REQUIRE(fs::exists(dest));
        CHECK(fs::file_size(dest) == fs::file_size(src));
        CHECK(hash_file(dest) == hash_file(src));
    }

    TEST_CASE("a size-cap hit is reported as such and leaves no partial file") {
        TempDir tmp;
        fs::path src = tmp.path / "big.bin";
        write_pattern(src, 2 * 1024 * 1024);
        fs::path dest = tmp.path / "dest.bin";
        std::string err = error_of([&] { download_to_file(file_url(src), dest, 1024 * 1024); });
        CHECK(err.find("exceeds den's 1 MiB size limit") != std::string::npos);
        CHECK(err.find(src.string()) != std::string::npos);
        CHECK_FALSE(fs::exists(dest));
    }

    TEST_CASE("an HTTP/2 cap hit surfacing as CURLE_RECV_ERROR names the limit") {
        // What libcurl 8.7 returns for the flutter cask over HTTP/2: code 56
        // with the real cause only in the error buffer.
        std::string msg =
            describe_transfer_error("https://example.test/flutter.zip", CURLE_RECV_ERROR,
                                    "Maximum file size exceeded", kMaxInMemoryFetchBytes);
        CHECK(msg.find("exceeds den's 500 MiB size limit") != std::string::npos);
        CHECK(msg.find("Failure when receiving data") == std::string::npos);
    }

    TEST_CASE("other transfer errors keep curl's detailed message") {
        std::string msg = describe_transfer_error(
            "https://example.test/x.zip", CURLE_OPERATION_TIMEDOUT,
            "Operation too slow. Less than 1024 bytes/sec transferred the last 60 seconds",
            kMaxArchiveDownloadBytes);
        CHECK(msg.find("Operation too slow") != std::string::npos);
        CHECK(msg.find("https://example.test/x.zip") != std::string::npos);
        CHECK(describe_transfer_error("u", CURLE_COULDNT_CONNECT, "", 1)
                  .find(curl_easy_strerror(CURLE_COULDNT_CONNECT)) != std::string::npos);
    }

    TEST_CASE("archive caps leave room for multi-gigabyte casks") {
        CHECK(kMaxArchiveDownloadBytes > 2267355570ull); // flutter 3.47.8 arm64
    }
}

TEST_SUITE("download::fetch_archive") {
    TEST_CASE("fetch_archive accepts an archive larger than the old 500 MiB cap") {
        TempDir tmp;
        fs::path src = tmp.path / "large.zip";
        write_pattern(src, kMaxInMemoryFetchBytes + 1024 * 1024);
        const std::string sha = hash_file(src);
        fs::path cache = tmp.path / "cache";
        fs::path got = fetch_archive(file_url(src), sha, cache);
        CHECK(got == cache / (sha + ".zip"));
        CHECK(fs::file_size(got) == fs::file_size(src));
        CHECK(hash_file(got) == sha);
        CHECK_FALSE(fs::exists(cache / (sha + ".zip.download")));
    }

    TEST_CASE("fetch_archive refuses a mismatched download and caches nothing") {
        TempDir tmp;
        fs::path src = tmp.path / "a.tar.gz";
        write_pattern(src, 4096);
        const std::string wrong(64, 'a');
        fs::path cache = tmp.path / "cache";
        std::string err = error_of([&] { fetch_archive(file_url(src), wrong, cache); });
        CHECK(err.find("SHA256 mismatch") != std::string::npos);
        CHECK(fs::is_empty(cache));
    }
}

// GHCR bottle blobs go through fetch_verified_to (with a bearer header), so
// they stream to disk under the archive cap instead of being buffered in
// memory under the 500 MiB fetch_url cap.
TEST_SUITE("download::fetch_verified_to") {
    TEST_CASE("a blob larger than the old in-memory cap streams to the cache") {
        TempDir tmp;
        fs::path src = tmp.path / "blob";
        write_pattern(src, kMaxInMemoryFetchBytes + 1024 * 1024);
        const std::string sha = hash_file(src);
        fs::path cached = tmp.path / (sha + ".tar.gz");
        fetch_verified_to(file_url(src), sha, cached, {"Authorization: Bearer test-token"});
        CHECK(fs::file_size(cached) == fs::file_size(src));
        CHECK(hash_file(cached) == sha);
        CHECK_FALSE(fs::exists(tmp.path / (sha + ".tar.gz.download")));
    }

    TEST_CASE("a mismatched blob is refused once and leaves nothing behind") {
        TempDir tmp;
        fs::path src = tmp.path / "blob";
        write_pattern(src, 4096);
        const std::string wrong(64, 'b');
        fs::path cached = tmp.path / "cache" / (wrong + ".tar.gz");
        fs::create_directories(cached.parent_path());
        std::string err = error_of(
            [&] { fetch_verified_to(file_url(src), wrong, cached, {"Authorization: Bearer x"}); });
        CHECK(err.find("SHA256 mismatch for " + file_url(src)) == 0);
        CHECK(err.find("SHA256 mismatch", 1) == std::string::npos);
        CHECK(err.find("expected " + wrong) != std::string::npos);
        CHECK(err.find("got " + hash_file(src)) != std::string::npos);
        CHECK(fs::is_empty(cached.parent_path()));
    }

    TEST_CASE("no_check skips verification") {
        TempDir tmp;
        fs::path src = tmp.path / "blob";
        write_pattern(src, 4096);
        fs::path cached = tmp.path / "out";
        fetch_verified_to(file_url(src), "no_check", cached);
        CHECK(hash_file(cached) == hash_file(src));
    }
}
