// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "bundle.h"

#include "../core/error.h"
#include "../download/archive.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <fstream>
#include <sys/file.h>
#include <unistd.h>

// The bundle data is compiled from src/ruby/bundle_data.c.
extern "C" {
extern const unsigned char _den_ruby_bundle_start[];
extern const size_t _den_ruby_bundle_size;
}

namespace den {

fs::path ensure_ruby_bundle(const fs::path& den_home) {
    auto ruby_dir = den_home / "ruby";
    auto marker = ruby_dir / ".unpacked";

    // Fast path: already unpacked.
    if (fs::exists(marker)) {
        return ruby_dir;
    }

    // Lock to prevent concurrent unpacking.
    auto lock_path = den_home / "ruby.lock";
    fs::create_directories(den_home);
    int fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        throw InternalError("cannot create lock file: " + lock_path.string());
    }
    ::flock(fd, LOCK_EX);

    // Double-check after acquiring lock.
    if (fs::exists(marker)) {
        ::flock(fd, LOCK_UN);
        ::close(fd);
        return ruby_dir;
    }

    SPDLOG_INFO("unpacking embedded Ruby bundle to {}", ruby_dir.string());

    // Write the compressed bundle to a temp file.
    auto tmp_archive = den_home / "cache" / "ruby-bundle.tar.zst";
    fs::create_directories(tmp_archive.parent_path());
    {
        std::ofstream f(tmp_archive, std::ios::binary);
        auto size = _den_ruby_bundle_size;
        f.write(reinterpret_cast<const char*>(_den_ruby_bundle_start),
                static_cast<std::streamsize>(size));
    }

    // Extract using libarchive (handles tar.zst natively).
    fs::create_directories(ruby_dir);
    extract_archive(tmp_archive, ruby_dir);

    // Clean up the temp archive.
    std::error_code ec;
    fs::remove(tmp_archive, ec);

    // Write the marker file.
    {
        std::ofstream f(marker);
        f << "unpacked\n";
    }

    ::flock(fd, LOCK_UN);
    ::close(fd);

    SPDLOG_INFO("Ruby bundle unpacked ({} files)",
                std::distance(fs::recursive_directory_iterator(ruby_dir),
                              fs::recursive_directory_iterator{}));
    return ruby_dir;
}

} // namespace den
