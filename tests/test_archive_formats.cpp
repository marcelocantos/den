// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// extract_archive must read every archive format den meets in the wild:
// bottles (.tar.gz, plus .tar.xz/.tar.zst/.tar.bz2 for some taps and source
// tarballs) and casks (.zip). The fixtures under tests/fixtures/archives were
// written by Python's own compressors (see make_fixtures.py), so this also
// checks interop with an independent implementation.
//
// The codec checks guard the statically linked, vendored libarchive: if a
// codec were left out of the build, libarchive would silently fall back to
// spawning an external `xz`/`zstd`/... program (ARCHIVE_WARN), which a
// stock macOS or Linux box may not have.

#include <doctest.h>

#include "download/archive.h"

#include <archive.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace den {
namespace test {

namespace fs = std::filesystem;

namespace {

struct Tmp {
    fs::path path;
    Tmp() {
        path = fs::temp_directory_path() /
               ("den_arfmt_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(path);
    }
    ~Tmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    Tmp(const Tmp&) = delete;
    Tmp& operator=(const Tmp&) = delete;
};

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

fs::path fixture(const std::string& name) {
    return fs::path(DEN_SOURCE_DIR) / "tests" / "fixtures" / "archives" / name;
}

void check_extracts(const std::string& name) {
    CAPTURE(name);
    Tmp tmp;
    auto res = extract_archive(fixture(name), tmp.path);
    CHECK(res.root == fs::path("hello"));
    CHECK(slurp(tmp.path / "hello" / "README.txt") == "hello from den\n");
    auto tool = tmp.path / "hello" / "bin" / "tool";
    REQUIRE(fs::is_regular_file(tool));
    CHECK(slurp(tool) == "#!/bin/sh\necho hi\n");
    CHECK((fs::status(tool).permissions() & fs::perms::owner_exec) != fs::perms::none);
}

} // namespace

TEST_CASE("extract_archive: .zip (casks)") {
    check_extracts("hello.zip");
}

TEST_CASE("extract_archive: .tar.gz (bottles)") {
    check_extracts("hello.tar.gz");
}

TEST_CASE("extract_archive: .tar.xz") {
    check_extracts("hello.tar.xz");
}

TEST_CASE("extract_archive: .tar.zst") {
    check_extracts("hello.tar.zst");
}

TEST_CASE("extract_archive: .tar.bz2") {
    check_extracts("hello.tar.bz2");
}

TEST_CASE("libarchive has built-in codecs (no external-program fallback)") {
    struct archive* a = archive_read_new();
    REQUIRE(a != nullptr);
    // ARCHIVE_OK means the codec is compiled in; ARCHIVE_WARN means
    // libarchive would shell out to an external program instead.
    CHECK(archive_read_support_filter_gzip(a) == ARCHIVE_OK);
    CHECK(archive_read_support_filter_bzip2(a) == ARCHIVE_OK);
    CHECK(archive_read_support_filter_xz(a) == ARCHIVE_OK);
    CHECK(archive_read_support_filter_lzma(a) == ARCHIVE_OK);
    CHECK(archive_read_support_filter_zstd(a) == ARCHIVE_OK);
    CHECK(archive_read_support_filter_lz4(a) == ARCHIVE_OK);
    CHECK(archive_read_support_format_zip(a) == ARCHIVE_OK);
    CHECK(archive_read_support_format_tar(a) == ARCHIVE_OK);
    archive_read_free(a);
}

} // namespace test
} // namespace den
